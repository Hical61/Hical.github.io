+++
title: "从零理解 MPSC 无锁写队列：高性能网络框架的发送引擎"
date = '2026-05-28'
draft = false
tags: ["C++20", "无锁编程", "网络框架", "协程", "性能优化", "Hical"]
categories = ["Hical框架"]
description = "拆解 Hical 框架中 Vyukov MPSC 无锁队列的完整设计——从为什么需要它，到每一行代码怎么工作，再到协程写循环如何驱动高效 I/O。"
+++

## 写在前面

你有没有想过，一个高并发网络服务器在同时给几千个客户端发消息时，底层到底在忙什么？

最直觉的做法是加把锁——谁要发消息就抢锁、写 socket、放锁。但问题来了：如果 1000 个协程同时要给同一个连接发数据，它们就得排队等这把锁。锁竞争带来的上下文切换、cache 失效，会把性能拖进泥潭。

Hical 框架的解法很酷：用一个 **MPSC（多生产者单消费者）无锁队列** 做缓冲，配合一个**协程写循环**做消费。发消息的线程只管往队列里扔节点（wait-free，永远不阻塞），写循环协程在 IO 线程上批量取出、合并发送。

这篇文章会带你从零搞懂这个设计。不需要你有无锁编程的基础，但最好知道 C++ 的 `atomic`、`shared_ptr` 和"什么是协程"大概是怎么回事。

---

## 一、先搞清楚问题：为什么普通的锁不行？

### 1.1 一个典型场景

假设你写了个聊天服务器。用户 A 发了条群消息，服务器要转发给群里 200 个在线用户。这意味着：

```
用户A的消息 → 广播逻辑 → 同时调用 200 个连接的 send()
```

如果 `send()` 内部用 mutex 保护写操作：

```cpp
// 朴素实现（有严重性能问题）
void Connection::send(std::string data)
{
    std::lock_guard lock(writeMutex_);  // 200个协程在这里排队
    writeBuffer_.append(data);
    if (!writing_)
    {
        writing_ = true;
        doWrite();  // 触发实际的 socket 写入
    }
}
```

问题出在哪？

1. **锁竞争**：200 个协程抢同一把锁，同一时刻只有 1 个能进去。其余 199 个在干等。
2. **cache 颠簸**：mutex 内部的原子变量在多核之间反复 "弹来弹去"（cache line bouncing），每次切换约 50-100ns。
3. **线程阻塞风险**：如果是 `1线程:1个io_context` 模型（Hical 就是），锁住线程意味着这个线程上所有其他协程都停了。

### 1.2 我们真正需要的是什么？

仔细想想发送数据的本质需求：

- **多个地方**可以同时调 `send()`（多生产者）
- 但 **socket 写入**必须串行——TCP 是字节流，你不能两段数据交错写（单消费者）
- 发送方不应该被阻塞——你扔完数据就该去干别的了

这恰好是 **MPSC 队列**的适用场景：多个生产者并发 push，一个消费者串行 pop。而且我们要的是 **无锁** 的——生产者 push 时不应该等待任何人。

---

## 二、Vyukov MPSC 无锁队列：核心数据结构

Hical 采用的是 Dmitry Vyukov 发明的侵入式 MPSC 队列。"侵入式"意思是节点本身内嵌了链表指针，不需要额外分配一个包装对象。

### 2.1 节点定义

```cpp
// 队列中每个节点长这样
struct MpscNode
{
    std::atomic<MpscNode*> next{nullptr};  // 指向下一个节点
    WriteEntry entry;                       // 实际要发送的数据
};
```

就两个成员：一个原子指针 `next`（用于串成链表），一个 `entry`（存放发送内容）。

`next` 为什么要用 `atomic`？因为多个线程可能同时在修改不同节点的 `next` 指针，原子操作保证不会出现数据撕裂（一个线程写了一半，另一个线程看到半个指针值）。

### 2.2 队列本体

```cpp
struct MpscQueue
{
    MpscNode  stub;  // 哨兵节点（永远存在，不存数据）
    MpscNode* head_; // 消费者看这个（单线程访问，无竞争）

    alignas(64)
    std::atomic<MpscNode*> tail_; // 生产者抢这个（多线程竞争热点）
};
```

画个图你就懂了：

```
初始状态：

head_ ──→ [stub] ←── tail_
           next=null

stub 既是 head 又是 tail，队列为空。
```

三个关键点：

1. **`stub` 哨兵节点**：这个节点不存任何数据，它的作用是让队列永远不为空（简化边界条件判断）。没有它的话，"队列为空"和"只有一个元素"的处理逻辑会非常麻烦。

2. **`head_` 不是 atomic**：只有消费者（writeLoop 协程）会读写 `head_`，它跑在固定的 IO 线程上，所以不需要原子操作——这是 MPSC 的核心优势。

3. **`tail_` 用 `alignas(64)`**：这是为了让 `tail_` 独占一整个 CPU 缓存行（通常 64 字节）。多个生产者线程会频繁 exchange `tail_`，如果它和别的变量共享缓存行，会产生 "false sharing"——本来不相关的数据因为在同一缓存行里被反复失效。

### 2.3 push 操作：生产者怎么放数据

这是整个设计中最精妙的部分。我们逐行看：

```cpp
void MpscQueue::push(MpscNode* node)
{
    // 第 1 步：先把新节点的 next 置为 null
    node->next.store(nullptr, std::memory_order_relaxed);

    // 第 2 步：原子交换 tail_，把自己挂到末尾
    MpscNode* prev = tail_.exchange(node, std::memory_order_acq_rel);

    // 第 3 步：让前一个节点的 next 指向自己
    prev->next.store(node, std::memory_order_release);
}
```

三行代码，我们一步步分解。

**第 1 步**：`node->next = nullptr`。新来的节点肯定是最后一个，所以它后面没人。用 `relaxed` 是因为这一步只改自己内部的数据，不需要和别人同步。

**第 2 步**：`tail_.exchange(node)` 做了两件事——读出当前的 tail（存到 `prev`），同时把 tail 设为新节点。这是一个原子操作，多个线程同时执行时，每个线程都能成功拿到自己的 `prev`，不会丢失。这就是为什么它是 **wait-free** 的——不管别人在干嘛，你这一步永远在有限时间内完成。

**第 3 步**：`prev->next = node`。让前一个节点"链上"新节点，这样消费者从 head 遍历时能一路找到你。

用图来看两个线程同时 push 的过程：

```
=== 初始状态 ===

head_ ──→ [stub] ←── tail_
           next=null

=== 线程 A 执行 exchange，拿到 prev=stub，tail_=nodeA ===

head_ ──→ [stub]       [nodeA] ←── tail_
           next=?       next=null

=== 线程 B 也执行 exchange，拿到 prev=nodeA，tail_=nodeB ===

head_ ──→ [stub]       [nodeA]      [nodeB] ←── tail_
           next=?       next=?       next=null

=== 线程 A 执行 prev->next = nodeA ===

head_ ──→ [stub] ──→ [nodeA]      [nodeB] ←── tail_
                      next=?       next=null

=== 线程 B 执行 prev->next = nodeB ===

head_ ──→ [stub] ──→ [nodeA] ──→ [nodeB] ←── tail_
                                  next=null

链表完整了！消费者可以从 head_ 一路遍历到 nodeB。
```

注意：**第 2 步和第 3 步之间有一个短暂的"断链"窗口**。在线程 A 执行完 exchange 但还没执行 `prev->next = node` 时，消费者如果正好遍历到这里，会看到 `next == nullptr`——"好像后面没人了"。但实际上 nodeA 已经被挂到 tail 了，只是链还没接上。

这就是 pop 操作要处理的特殊情况。

### 2.4 pop 操作：消费者怎么取数据

```cpp
MpscNode* MpscQueue::pop()
{
    MpscNode* head = head_;         // 当前的 head（可能是 stub）
    MpscNode* next = head->next.load(std::memory_order_acquire);

    // 情况 1：head 是 stub（刚初始化或刚 pop 完上一个）
    if (head == &stub)
    {
        if (next == nullptr)
            return nullptr;         // 队列确实为空
        head_ = next;               // 跳过 stub，从真正的第一个节点开始
        head = next;
        next = head->next.load(std::memory_order_acquire);
    }

    // 情况 2：next 不为空，说明后面有节点，可以安全弹出 head
    if (next != nullptr)
    {
        head_ = next;               // head 前进一步
        return head;                // 返回被弹出的节点
    }

    // 情况 3：next 为空，但 head 不是 tail（有人正在 push，链还没接上）
    if (head != tail_.load(std::memory_order_acquire))
        return nullptr;             // 暂时取不到，等下次再来

    // 情况 4：head 就是 tail，队列真的空了
    // 重新把 stub 挂到末尾，恢复初始状态
    stub.next.store(nullptr, std::memory_order_relaxed);
    MpscNode* prev = tail_.exchange(&stub, std::memory_order_acq_rel);
    prev->next.store(&stub, std::memory_order_release);

    // 再试一次
    next = head->next.load(std::memory_order_acquire);
    if (next != nullptr)
    {
        head_ = next;
        return head;
    }
    return nullptr;
}
```

这段看起来复杂，但核心逻辑很清晰：

- **能 pop 就 pop**（next 不为 null）
- **发现断链就放弃**（有人在 push 但还没接好，返回 null 下次再来）
- **队列真空了就重置**（把 stub 重新挂到末尾）

消费者是单线程的，所以整个 pop 过程没有任何 CAS 循环或自旋——它要么成功拿到节点，要么干脆返回 null。这就是"单消费者"带来的巨大简化。

---

## 三、节点池：消灭热路径上的 malloc

光有无锁队列还不够。每次发消息都 `new` 一个 `MpscNode`，发完了再 `delete`，这些 malloc/free 调用本身就是性能杀手——它们内部有锁、有系统调用。

Hical 的方案是给每个线程配一个**节点池**：

```cpp
// 每个线程私有的节点回收站
struct MpscNodePool
{
    struct FreeNode
    {
        FreeNode* next;  // 空闲链表指针
    };

    FreeNode* head_ = nullptr; // 空闲链表头
    size_t count_ = 0;          // 当前池中节点数量

    static constexpr size_t kMaxNodes = 128; // 最多缓存 128 个
};

// thread_local：每个线程一份，零竞争
static thread_local MpscNodePool nodePool;
```

### 3.1 分配节点

```cpp
static MpscNode* allocateNode(WriteEntry entry)
{
    auto& pool = nodePool;

    if (pool.head_ != nullptr)
    {
        // 池中有空闲节点，直接取出（O(1)，零 malloc）
        FreeNode* free = pool.head_;
        pool.head_ = free->next;
        pool.count_--;

        // 在空闲节点的内存上 placement new 一个 MpscNode
        MpscNode* node = reinterpret_cast<MpscNode*>(free);
        new (node) MpscNode{};       // 构造
        node->entry = std::move(entry);
        return node;
    }

    // 池空了，老实 new 一个
    auto* node = new MpscNode{};
    node->entry = std::move(entry);
    return node;
}
```

关键理解：`FreeNode` 和 `MpscNode` 共用同一块内存。节点"在用时"是 `MpscNode`（存数据），"回收后"被当作 `FreeNode`（只用一个 next 指针串成空闲链表）。这个技巧叫做**内存复用**——避免反复申请释放相同大小的内存。

### 3.2 归还节点

```cpp
static void deallocateNode(MpscNode* node)
{
    auto& pool = nodePool;

    node->~MpscNode();  // 显式析构（释放 entry 中的 shared_ptr）

    if (pool.count_ < kMaxNodes)
    {
        // 池没满，回收到池中（O(1)，零 free）
        auto* free = reinterpret_cast<FreeNode*>(node);
        free->next = pool.head_;
        pool.head_ = free;
        pool.count_++;
    }
    else
    {
        // 池满了，真的释放
        operator delete(node);
    }
}
```

### 3.3 为什么这么有效？

在典型的网络服务器中，同一个 IO 线程上的 `send()` 和 `writeLoop()` 在同一线程执行（因为 send 经常就是在 handler 里调用的，handler 跑在 IO 线程上）。这意味着：

```
同一线程内：
  send() → allocateNode() → 从池取（O(1)）
  ...
  writeLoop() → deallocateNode() → 还到池（O(1)）
  ...
  下次 send() → allocateNode() → 池中有节点，又是 O(1)
```

节点在同一个线程里"借出→归还→借出→归还"循环，**永远不需要 malloc/free**。128 个节点的容量在绝大多数场景下绰绰有余。

---

## 四、协程写循环：把碎片合并成大块

有了无锁队列和节点池，最后一块拼图是**写循环**——它负责从队列里取数据，然后发给操作系统。

### 4.1 写循环的启动

```cpp
void GenericConnection::enqueueEntry(WriteEntry entry)
{
    // 1. 分配节点，放入队列
    MpscNode* node = allocateNode(std::move(entry));
    writeQueue_.push(node);

    // 2. 尝试启动写循环
    bool expected = false;
    if (writing_.compare_exchange_strong(expected, true))
    {
        // CAS 成功：之前没有写循环在跑，启动一个
        coSpawn(writeLoop());
    }
    // CAS 失败：已有写循环在跑，它会自己处理新数据
}
```

这里的 `writing_` 是一个 `atomic<bool>`。`compare_exchange_strong` 的意思是："如果 `writing_` 当前是 false，就把它设为 true 并返回 true；否则啥都不干返回 false"。

这保证了**同一时刻最多只有一个 writeLoop 协程在运行**。后续的 `send()` 调用只管往队列里丢数据，writeLoop 会自己把它们全部捞出来。

### 4.2 writeLoop 主体

```cpp
Awaitable<void> GenericConnection::writeLoop()
{
    for (;;)
    {
        // ═══════ 批量取出（drain）═══════
        std::vector<WriteEntry> batch;
        batch.reserve(32);  // 预分配，减少 realloc

        int drainCount = 0;
        while (auto* node = writeQueue_.pop())
        {
            batch.push_back(std::move(node->entry));
            deallocateNode(node);   // 归还节点到池

            if (++drainCount >= kMaxDrainBatch)
                break;  // 一轮最多取 256 个，防止饿死其他协程
        }

        // 队列空了 → 尝试退出
        if (batch.empty())
        {
            writing_.store(false, std::memory_order_release);

            // Double-check：退出前再看一眼，防止刚好有人 push 了
            if (writeQueue_.pop() == nullptr)
                co_return;  // 确实空了，退出协程

            // 有人赶上了，重新标记为 writing 并继续
            writing_.store(true, std::memory_order_acquire);
            continue;
        }

        // ═══════ 合并发送 ═══════
        if (batch.size() == 1)
        {
            // 只有一条 → 单次 async_write
            auto& data = *batch[0].memory;
            co_await boost::asio::async_write(
                socket_,
                boost::asio::buffer(data),
                boost::asio::use_awaitable
            );
        }
        else
        {
            // 多条 → scatter-gather I/O（一次系统调用发多块数据）
            std::vector<boost::asio::const_buffer> buffers;
            buffers.reserve(batch.size());
            for (auto& entry : batch)
                buffers.emplace_back(
                    boost::asio::buffer(*entry.memory)
                );

            co_await boost::asio::async_write(
                socket_,
                buffers,  // writev: 内核把多块数据合并成一个 TCP 段
                boost::asio::use_awaitable
            );
        }

        // 更新统计
        bytesSent_ += totalBytes;
        updateLastActiveTime();
    }
}
```

### 4.3 逐段讲解

**批量取出（drain）**：

writeLoop 不是每 push 一个就发一个，而是一口气把队列里所有待发的数据都捞出来。这就像你去取快递——不是每到一个包裹就跑一趟菜鸟驿站，而是攒一批一起拿。

`kMaxDrainBatch = 256` 是个安全阀：如果某个连接突然被灌入海量数据（比如广播风暴），限制单轮 drain 数量防止这个连接的写循环霸占整个 io_context 线程。

**退出时的 double-check**：

这是一个经典的 "check → act → re-check" 模式。如果 writeLoop 发现队列空了就直接退出，有个竞态条件：

```
时刻 1：writeLoop 调用 pop() → 返回 null（队列看起来空了）
时刻 2：另一个线程调用 push(node) + tryStartWrite()
时刻 3：tryStartWrite() 看到 writing_ == true，不启动新 writeLoop
时刻 4：writeLoop 设置 writing_ = false 并退出
```

结果：新 push 的数据永远不会被发送！

所以退出流程是：先 `writing_ = false`，再 pop 一次确认真的空了。如果不空，说明有人在时刻 2 和时刻 4 之间 push 了数据，赶紧重新 `writing_ = true` 继续干活。

**scatter-gather I/O**：

多条消息合并为一次 `writev` 系统调用。普通写法是每条消息一次 `write()`，10 条消息 = 10 次系统调用 = 10 次用户态→内核态切换。scatter-gather 只需要一次系统调用，内核帮你把多块内存拼成一个 TCP 段发出去。

---

## 五、WriteEntry：灵活处理不同类型的数据

发送的不一定都是字符串。有时候要发文件，有时候发内存数据。Hical 用 tagged union 处理：

```cpp
struct WriteEntry
{
    enum class Tag { hMemory, hNode };
    Tag tag;

    union
    {
        std::shared_ptr<std::string> memory;  // 内存数据
        std::shared_ptr<WriteNode> node;       // 多态节点（如文件）
    };

    // 工厂方法
    static WriteEntry fromMemory(std::shared_ptr<std::string> data)
    {
        WriteEntry e;
        e.tag = Tag::hMemory;
        new (&e.memory) std::shared_ptr<std::string>(std::move(data));
        return e;
    }

    static WriteEntry fromNode(std::shared_ptr<WriteNode> n)
    {
        WriteEntry e;
        e.tag = Tag::hNode;
        new (&e.node) std::shared_ptr<WriteNode>(std::move(n));
        return e;
    }
};
```

为什么用 tagged union 而不是继承？

- **继承需要虚函数**：每次访问数据都要走虚表间接调用，对热路径是不必要的开销
- **tagged union 零间接**：根据 tag 直接访问对应字段，编译器可以内联所有逻辑
- **内存紧凑**：union 大小 = 最大成员大小，不会浪费空间

在 writeLoop 中的使用：

```cpp
for (auto& entry : batch)
{
    if (entry.tag == WriteEntry::Tag::hMemory)
    {
        // 快速路径：内存数据，直接拿 buffer
        buffers.emplace_back(boost::asio::buffer(*entry.memory));
    }
    else
    {
        // 慢路径：文件节点，需要异步读发
        co_await sendFileNode(entry.node);
    }
}
```

---

## 六、StringPool：发送缓冲区的对象池

发内存数据时需要一个 `shared_ptr<string>` 装数据。每次都 new 一个 string 再 delete 太浪费。Hical 在连接层面还有一个 **StringPool**：

```cpp
// thread_local 的字符串对象池
// 5 个大小档位：256 / 512 / 1024 / 2048 / 4096 字节
// 每档最多缓存 32 个
class StringPool
{
    static shared_ptr<string> acquire(size_t size)
    {
        // 找到合适的档位
        size_t tier = selectTier(size);  // 比如 300 字节 → 512 档

        if (pool_[tier].count > 0)
        {
            // 池中有现成的，取出并 reserve 到合适大小
            auto s = pool_[tier].pop();
            s->clear();
            s->reserve(tierSize[tier]);
            return s;  // 自带 custom deleter，析构时自动归还到池
        }

        // 池空了，创建新的（带归还 deleter）
        return make_shared_with_pool_deleter(tierSize[tier]);
    }
};
```

从调用者角度看，`send("hello world")` 的完整路径是：

```
send(data)
  → StringPool::acquire(data.size())     // 从池取 string（O(1)）
  → string->assign(data)                 // 拷贝数据进去
  → WriteEntry::fromMemory(string)       // 包装为 entry
  → allocateNode(entry)                  // 从节点池取 MpscNode（O(1)）
  → writeQueue_.push(node)               // 无锁入队（wait-free）
  → tryStartWrite()                      // 如需要则启动 writeLoop
```

整条链路上：零 malloc（字符串从池取），零 malloc（节点从池取），零锁（MPSC push 是 wait-free）。

---

## 七、完整时序图

下面这张时序图展示了从业务代码调用 `send()` 到数据真正发出去的完整交互过程：

```
  业务协程          连接对象            MPSC队列          IO线程 writeLoop
  (任意线程)    (enqueueEntry)       (lock-free)         (单消费者)
     │                │                   │                   │
     │  send("hi")   │                   │                   │
     │──────────────→│                   │                   │
     │               │ 1.StringPool取string                  │
     │               │ 2.allocateNode   │                   │
     │               │─────────────────→│                   │
     │               │    push(node)     │                   │
     │               │                   │                   │
     │               │ 3.tryStartWrite   │                   │
     │               │   CAS成功         │                   │
     │               │──────────────────────────────────────→│
     │               │   coSpawn(writeLoop)                  │
     │               │                   │                   │
     │  (继续干别的)  │                   │              4.pop()
     │               │                   │←──────────────────│
     │               │                   │  返回 node        │
     │               │                   │──────────────────→│
     │               │                   │                   │
     │               │                   │              5.deallocateNode
     │               │                   │                (归还池)
     │               │                   │                   │
     │               │                   │           6.async_write(socket)
     │               │                   │                   │──→ 内核
     │               │                   │                   │
     │               │                   │              7.co_await...
     │               │                   │                   │ (挂起,让出线程)
     │               │                   │                   │
     │               │                   │              8.写完成,恢复
     │               │                   │                   │
     │               │                   │              9.再次pop()
     │               │                   │←──────────────────│
     │               │                   │  返回 null        │
     │               │                   │──────────────────→│
     │               │                   │                   │
     │               │                   │              10.writing_=false
     │               │                   │                  co_return
```

关键观察：

- **业务协程不等待**：`send()` 返回时数据只是进了队列，不是已经发出去了。业务协程可以立刻继续做下一件事。
- **writeLoop 批量处理**：如果在 writeLoop 执行 async_write 期间又有新数据 push 进来，下一轮 pop 会一次全取出来合并发送。
- **线程模型清晰**：push 可以从任意线程发生（wait-free），pop 只在 IO 线程（单消费者，无锁）。

---

## 八、对比传统方案

| 维度 | mutex + buffer | MPSC 无锁队列 |
|------|:---:|:---:|
| 生产者（send）阻塞？ | 会（等锁） | 不会（wait-free） |
| 锁竞争 | O(N) 个生产者争一把锁 | 只有 tail_ 的 exchange（极轻量） |
| 内存分配 | 每次 send 可能触发 buffer 扩容 | 节点池 O(1) 取还 |
| 系统调用 | 每条消息一次 write | 批量 scatter-gather，一次 writev |
| 代码复杂度 | 低 | 中（需理解无锁语义） |
| 适用场景 | 低并发、简单场景 | 高并发、每连接高吞吐 |

---

## 九、流程总览图

把整个发送子系统画成一张完整的流程图：

```
                    ┌──────────────────────────────────────────────┐
                    │            发送子系统全景                       │
                    └──────────────────────────────────────────────┘

    ┌─────────┐     ┌─────────────┐     ┌───────────────┐     ┌────────────┐
    │ 业务代码 │────→│ StringPool  │────→│  MpscNodePool │────→│ MpscQueue  │
    │ send()  │     │ 取string    │     │  取node       │     │  push()    │
    └─────────┘     └─────────────┘     └───────────────┘     └─────┬──────┘
                                                                     │
                    ┌────────────────────────────────────────────────┘
                    ↓
              ┌──────────────┐
              │ tryStartWrite│ CAS(writing_: false→true)
              └──────┬───────┘
                     │ 首次 → coSpawn
                     ↓
         ┌──────────────────────┐
         │    writeLoop 协程     │ (IO 线程，单消费者)
         │                      │
         │  ┌────────────────┐  │
         │  │  drain 循环     │  │  pop() → batch[]
         │  │  (最多256个)    │  │  deallocateNode → 归还池
         │  └───────┬────────┘  │
         │          ↓           │
         │  ┌────────────────┐  │
         │  │ 合并 + 发送     │  │  scatter-gather / async_write
         │  │ co_await 挂起   │  │  (让出线程给其他协程)
         │  └───────┬────────┘  │
         │          ↓           │
         │  队列空? ─Yes→ writing_=false, co_return
         │     │No              │
         │     └──→ 继续drain   │
         └──────────────────────┘
                     │
                     ↓
              ┌──────────────┐
              │   TCP Socket  │ ──→ 客户端
              └──────────────┘
```

---

## 十、几个常见疑问

### Q1：为什么不直接用 `std::queue` + `mutex`？

性能差距主要体现在高并发场景。`std::queue` + `mutex` 意味着：
- 每次 push 要获取锁（如果锁被 writeLoop 持有就要等）
- 每次 pop 也要获取锁
- push 和 pop 互相阻塞

MPSC 队列中 push 是 wait-free 的——**你永远不需要等任何人**。pop 是单线程的——**没有人和你抢**。两边都消除了等待。

### Q2：memory_order 那些参数是什么意思？

简单理解：
- `relaxed`：最快，但不保证其他线程能立刻看到你的修改
- `release`：写操作的"发布"——保证你之前的所有写入对读到这个值的线程可见
- `acquire`：读操作的"获取"——保证你看到的是对方 release 之前的所有写入
- `acq_rel`：同时具有 acquire 和 release 语义（用于 read-modify-write 操作如 exchange）

在 push 中：`tail_.exchange(node, acq_rel)` 保证前一个 push 的 `prev->next = ...` 对当前线程可见；`prev->next.store(node, release)` 保证消费者 acquire 时能看到 node 的完整内容。

### Q3：如果 writeLoop 正在写的时候连接断了怎么办？

`async_write` 会返回错误（connection reset）。协程中检测到错误后，设置连接状态为断开，清空剩余队列节点（全部归还节点池），然后 co_return 退出。后续的 `send()` 调用会检查连接状态，发现已断开就直接丢弃数据不入队。

### Q4：跨线程 send 和同线程 send 有区别吗？

从正确性角度没区别——MPSC 队列天然支持多线程 push。但性能上：

- **同线程 send**：节点从本线程池取，writeLoop 在本线程还到本线程池。节点一直在同一个线程的缓存中，cache 命中率极高。
- **跨线程 send**：节点从线程 A 的池取出，在线程 B（IO 线程）的 writeLoop 中被 deallocate。由于是另一个线程的内存，首次访问会 cache miss。不过 `deallocateNode` 只是把节点还到 IO 线程的本地池——下次这个节点被分配时就是 IO 线程本地的了。

---

## 十一、总结

Hical 的发送引擎是一个精心设计的多层系统：

| 层次 | 职责 | 核心优化 |
|------|------|---------|
| StringPool | 提供发送缓冲区 | thread_local 对象池，零 malloc |
| MpscNodePool | 提供队列节点 | thread_local free list，零 malloc |
| MpscQueue | 多生产者→单消费者 | Vyukov 算法，wait-free push |
| writeLoop | 批量取出 + 发送 | scatter-gather I/O，一次系统调用 |

四层协作的结果是：在高并发场景下，`send()` 的热路径上没有锁、没有 malloc、没有系统调用。数据被异步批量发送，最大化网络吞吐。

这个设计的思维方式也很值得借鉴：

1. **分析真实的并发模型**：不是所有场景都需要通用的多生产者多消费者队列。MPSC 的"单消费者"限制让 pop 端极其简单高效。
2. **用池消灭分配**：热路径上的任何 new/delete 都是可优化的对象。thread_local 池让分配降为 O(1) 指针操作。
3. **批量优于逐个**：攒一批数据再发，比每来一个发一个高效得多。scatter-gather I/O 把这个理念推到了系统调用层面。
4. **协程让一切优雅**：writeLoop 是一个普通的 for 循环，但 `co_await` 让它在等 I/O 时自动让出线程。没有回调地狱，代码读起来像同步的。

希望这篇文章能帮你建立对高性能网络 I/O 的直觉。下次你看到"无锁"、"对象池"、"批量写"这些关键词时，脑子里应该能浮现出它们各自解决的问题和协作方式。

---

> 源码参考：[Hical/src/asio/GenericConnection.h](https://github.com/Hical61/Hical/blob/main/src/asio/GenericConnection.h)
> 项目地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
