+++
title = '拆开 Hical：Vyukov MPSC 无锁队列在 HTTP 服务器上的实战——GenericConnection 写路径'
date = 2026-08-10T00:00:00+08:00
draft = false
tags = ["Hical", "MPSC", "无锁队列", "GenericConnection", "源码分析", "Vyukov"]
categories = ["Hical源码精读"]
description = "拆开 Hical 第 4 篇：Vyukov MPSC 无锁队列在 HTTP 服务器写路径上的实战，以及为什么不用 Disruptor。"
+++

# [Hical] Vyukov MPSC 无锁队列在 HTTP 服务器上的实战：GenericConnection 的写路径

> 本专栏文章：拆开 Hical · 第 4 篇

前面三篇都在 HTTP 层面打转——请求怎么解析、路由怎么匹配、中间件怎么执行。这一篇沉到网络层，看一个具体的问题：**多个协程想往同一个 socket 写数据时，怎么不加锁？**

答案藏在 GenericConnection 的 Vyukov MPSC 无锁队列里。

---

## 1. 问题：多个协程同时往一个连接上写

先搞清楚为什么会有这个问题。HTTP/2 和 WebSocket 都允许在一个 TCP 连接上并发地处理多个"流"：

```
线程 A（协程处理 WebSocket frame）──→ 想往 socket 写数据
线程 B（协程处理心跳 ping）      ──→ 也想往 socket 写数据
线程 C（IO 线程正在写上一批数据） ──→ socket 只能同时一个写操作
```

传统的做法是 `std::mutex + std::queue`。但这里有三个痛点：
1. **生产者多、消费者一个**：多个协程往队列里塞数据，只有一个写协程取出来发给 socket
2. **mutex 竞争**：每秒几十万次 send → 几十万次 mutex lock/unlock → 内核态的 futex 开销
3. **队列长度短**：大多数时候队列深度 < 5，争锁的开销比实际写数据还大

---

## 2. Vyukov MPSC 队列：核心原理

Dmitry Vyukov 的 MPSC 队列专门为"多生产者、单消费者"场景设计。先直观理解——想象排队买票：

```
生产者 A 拿到号牌 1 → 写着"下一个是 nullptr"
生产者 B 拿到号牌 2 → 写着"下一个是 A"
生产者 C 拿到号牌 3 → 写着"下一个是 B"

消费者：从后往前看—
  看到 3 → 我拿 3，看看下一个是 B
  看到 B → 不对，B 不是数字...
  
好吧，实际上链表不是数字，是指针。
生产者在链表头 push，消费者从链表尾 pop。
```

实际实现：

```cpp
struct MpscNode 
{
    MpscNode* next;    // 单向链表
};

struct MpscQueue 
{
    alignas(64) std::atomic<MpscNode*> head_;  // 消费者从这里 pop
    alignas(64) std::atomic<MpscNode*> tail_;  // 生产者从这里 push

    // 消费者端：单消费者，无锁
    MpscNode* pop() 
    {
        // 1. 先看 head（消费者自己的头）
        auto* head = head_.load(std::memory_order_relaxed);
        if (head) 
        {
            head_ = head->next;  // 取走当前，头移到下一个
            return head;
        }

        // 2. head 为空 → 把整个 tail 链表端走 + 反转
        auto* tail = tail_.exchange(nullptr, std::memory_order_acq_rel);
        if (!tail) return nullptr;  // 真的空了

        // 3. 反转链表（LIFO 入队 → FIFO 出队的关键一步）
        MpscNode* reversed = nullptr;
        while (tail) 
        {
            auto* next = tail->next;
            tail->next = reversed;
            reversed = tail;
            tail = next;
        }

        // 4. 弹出第一个
        head_ = reversed->next;
        return reversed;
    }

    // 生产者端：CAS 循环，多线程安全
    void push(MpscNode* node) 
    {
        node->next = nullptr;
        auto* oldTail = tail_.exchange(node, std::memory_order_acq_rel);
        if (oldTail) 
        {
            oldTail->next = node;  // 挂到原队尾的后面
        } 
        else 
        {
            head_ = node;  // 队列之前是空的 → 直接放到 head
        }
    }
};
```

### 2.1 为什么 alignas(64) 在 tail_ 上？

`tail_` 被多生产者并发 CAS（每 send 一次就写一次），`head_` 只有消费者读/写。如果它们在同一条 64 字节 cache line 上，生产者的每次 CAS 都会刷掉消费者的 cache line。

`alignas(64)` 把 tail_ 推到独立的 cache line 上——生产者折腾 tail_ 时不干扰消费者读 head_。

### 2.2 链表反转：LIFO 入队转 FIFO 出队

生产端用 CAS 无锁 LIFO 入队（新节点挂到 tail_ 上），消费端反转一次链表变成 FIFO 顺序消费。这和大多数无锁 MPSC 队列的思路一致——在消费端付出一次 O(N) 反转的开销，换取生产端真正的 wait-free push。

---

## 3. MpscNodePool：消除写路径上的 malloc

每次 send 需要分配一个 MpscNode。如果每次都 `new`，每秒 10 万次 send = 每秒 10 万次 malloc。MpscNodePool 的解法：

```cpp
class MpscNodePool 
{
    // thread_local：每线程一个空闲链表，无锁
    static thread_local MpscNode* freeList_;
    static thread_local size_t freeCount_;
    static constexpr size_t kMaxFreeNodes = 128;

    static MpscNode* allocateNode(WriteEntry entry) 
    {
        MpscNode* node;
        if (freeCount_ > 0) 
        {
            node = freeList_;           // 从空闲链表取
            freeList_ = freeList_->next;
            --freeCount_;
        } 
        else 
        {
            node = new MpscNode();      // 空闲链表空了 → 真的 new
        }
        // placement new 写入数据（复用已分配的内存）
        new (&node->entry) WriteEntry(std::move(entry));
        return node;
    }

    static void deallocateNode(MpscNode* node) 
    {
        node->entry.~WriteEntry();       // 析构数据
        if (freeCount_ < kMaxFreeNodes) 
        {
            node->next = freeList_;      // 归还到空闲链表
            freeList_ = node;
            ++freeCount_;
        } 
        else 
        {
            delete node;                 // 太多了 → 真的 delete
        }
    }
};
```

**thread_local 是关键**：生产者和消费者在同一个 io_context 线程上时，allocate 和 deallocate 都在同一条线程上——没有任何跨线程的竞争。

> 💡 kMaxFreeNodes = 128：上限控制是为了防止连接在突发流量后长期空闲。如果一个连接 1 秒内发了 10 万个包，128 个节点很快用完，之后就走真正的 new/delete——但剩下的 99,872 个节点也已经被正确销毁了，空闲连接只持有最多 128 个闲置节点（< 10KB 内存）。

---

## 4. 写循环：从队列取出 → scatter-gather → async_write

```cpp
template <typename SocketType>
Awaitable<void> GenericConnection<SocketType>::writeLoop() 
{
    constexpr size_t kMaxDrainBatch = 256;

    while (state_ == State::hConnected) 
    {
        // 从 MPSC 队列取一个节点
        auto* node = writeQueue_.pop();
        if (!node) 
        {
            // 队列空了 → 退出写循环
            writing_.store(false);
            node = writeQueue_.pop();  // double-check！
            if (!node) break;
            // double-check 期间来了新数据 → 继续写
            writing_.store(true);
        }

        // 批量收取最多 256 个节点
        std::array<boost::asio::const_buffer, kMaxDrainBatch> buffers;
        size_t bufCount = 0;

        do {
            if (node->entry.hasMemory()) 
            {
                auto& str = node->entry.asMemory();
                buffers[bufCount++] = boost::asio::buffer(str->data(), str->size());
            } 
            else if (node->entry.hasFile()) 
            {
                // FileWriteNode：先刷已收集的 buffer，再单独发文件
                if (bufCount > 0) 
                {
                    co_await boost::asio::async_write(socket_,
                        std::span(buffers.data(), bufCount), use_awaitable);
                    bufCount = 0;
                }
                co_await writeFile(node->entry.asFile());
            }

            deallocateNode(node);  // 归还到 thread_local 池
            node = writeQueue_.pop();
        } while (node != nullptr && bufCount < kMaxDrainBatch);

        // scatter-gather：一次 async_write 发出已收集的全部 buffer
        if (bufCount > 0) 
        {
            co_await boost::asio::async_write(socket_,
                std::span(buffers.data(), bufCount), use_awaitable);
        }
    }
}
```

### 4.1 kMaxDrainBatch = 256 的背压含义

如果生产者疯狂往队列里塞数据（比如 17 个协程同时大量 send），消费者一次只收 256 个节点——还给生产者留了机会在下次循环中继续塞。如果不设限制（无限收取），消费者可能被一次超大 coalesce 阻塞太久，其他连接得不到公平的 IO 时间。

256 的选择是工程折中：足够大以收集大部分 burst（HTTP 响应通常一个节点就能搞定），但不太大以至于造成调度饥饿。

### 4.2 double-check 模式

```cpp
writing_.store(false);
node = writeQueue_.pop();  // double-check！
if (!node) break;
writing_.store(true);
```

**为什么需要 double-check？** `writing_.store(false)` 和 `writeQueue_.pop()` 之间存在窗口。如果正好有生产者在 `store(false)` 之后、`pop()` 之前 push 了新数据——生产者看到 `writing_ == false`，会启动新的写协程；但如果消费者已经在 `pop()` 的路上，它看到新数据就会继续写。两者同时启动写协程 → 两个协程同时写同一个 socket → 数据乱序。

> 💡 Double-check 是防止 missing wakeup 的标准模式——在标记"我已经做完了"之后、真正退出之前，再检查一遍。

---

## 5. GenericConnection 的 if constexpr 分支

TCP 和 SSL 的 socket 操作有差异（SSL 需要 handshake、shutdown 有两步），但 GenericConnection 通过 `if constexpr` 把两种路径合并到同一个模板里：

```cpp
template <typename SocketType>
auto& GenericConnection<SocketType>::lowestLayerSocket() 
{
    if constexpr (hIsSslStream<SocketType>) 
    {
        return socket_.lowest_layer();   // SSL: 拿底层 TCP socket
    } 
    else 
    {
        return socket_;                  // TCP: 就是它自己
    }
}
```

这种编译期分支的好处：两个版本共享同一套逻辑，但生成的机器码各自独立——TCP 路径不包含任何 SSL 相关指令，SSL 路径不引入多余的 `if (isSsl)` 运行时判断。

---

## 6. 为什么选 Vyukov 队列而不是 Disruptor？

你可能听说过 LMAX Disruptor——高性能环形缓冲 + 序列号机制的无锁队列。为什么 Hical 不用？

|          | Vyukov MPSC（Hical）       | Disruptor 风格                     |
| -------- | -------------------------- | ---------------------------------- |
| 复杂度   | ~80 行                     | ~800 行（多生产者+barrier+traits） |
| 数据结构 | 单向链表                   | 环形缓冲 + 发布数组                |
| 适用场景 | 队列深度 < 5               | 队列深度 > 1000                    |
| 背压     | 无背压（链表理论上无限长） | claim 阻塞级别背压                 |
| 批量操作 | pop 后循环收集（最多 256） | 序列号批量分配                     |

HTTP 响应发送的场景：单条消息通常 0.5-10KB，队列深度通常 1-3，突发后迅速消耗。Vyukov 链表足够应付。Disruptor 的环形缓冲 + 序列号 + 背压机制更适合金融交易系统那种"每秒百万条消息、每条几十字节"的高吞吐管道。

**选方案不是选最牛的，是选最合适的。**

---

## 回顾一下你学会了什么

1. Vyukov MPSC 队列：生产者 push 是 wait-free O(1)的 CAS，消费者 pop 加链表反转
2. alignas(64) 分离 tail_ 和 head_ 所在的 cache line，防止 false sharing
3. MpscNodePool 的 thread_local 空闲链表消除热路径 malloc
4. writeLoop 批量收取最多 256 个节点 → scatter-gather 一次 async_write
5. double-check 模式防止 missing wakeup——在标记"已完成"后、真正退出前再检查一遍
6. if constexpr 让 TCP 和 SSL 共享代码但生成独立机器码
7. Vyukov vs Disruptor：不是越复杂越好，HTTP 响应用链表就够了

---

> 下一篇：[为什么不用每个连接一个 timer 协程？IdleScanner 的集中式空闲扫描设计]({{< relref "posts/hical-source-study/05-IdleScanner.md" >}})
