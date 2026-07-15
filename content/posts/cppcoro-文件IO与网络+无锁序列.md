+++
title = 'IO 与无锁序列：cppcoro 的网络文件 I/O，以及 LMAX Disruptor 的协程化'
date = '2024-11-30'
draft = false
tags = ["C++", "cppcoro", "协程", "C++20", "源码分析", "IO", "无锁", "Disruptor", "IOCP"]
categories = ["C++"]
description = "拆开 cppcoro 给你看 · 第 6 篇（完结篇）。Windows IOCP 协程化封装 + LMAX Disruptor 无锁序列的协程化改造，cppcoro 源码精读系列收官。"
+++

# IO 与无锁序列：cppcoro 的网络文件 I/O，以及 LMAX Disruptor 的协程化

> 本专栏文章：拆开 cppcoro 给你看 · 第 6 篇（完结篇）

这是系列的最后一篇，也是最"硬"的一篇。要搞懂两件事：① cppcoro 怎么把 Windows IOCP 封装成协程友好的文件/网络 I/O 接口；② cppcoro 借鉴 LMAX Disruptor 的无锁序列怎么协程化，让消费者不是忙等而是挂起等待。

> 注：IOCP 基础、`win32_overlapped_operation` CRTP 模式、四态取消状态机、`cancellation_token` 三层模型、`io_service` 和 `async_scope` 在上一篇 Layer 3 中已经讲过了。本篇聚焦文件 I/O 类型层次、socket 的协程封装，以及无锁序列原语的设计推导。

---

## 1. 文件 I/O 类型体系

### 1.1 类层次结构

```
file (基类: size(), 持有 Windows HANDLE)
  ├── readable_file (抽象: read(offset, buffer, size))
  │      └── read_only_file (具体类: open() 工厂方法)
  ├── writable_file (抽象: write(offset, buffer, size), set_size())
  │      └── write_only_file (具体类: open() 工厂方法)
  └── read_write_file (多继承: readable_file + writable_file)
```

### 1.2 为什么用静态工厂？

```cpp
static read_only_file open(
    io_service& ioService,
    const path& path,
    file_share_mode shareMode = file_share_mode::read,
    file_buffering_mode bufferingMode = file_buffering_mode::default_);
```

Windows 上打开文件涉及多个系统调用（`CreateFile` + `CreateIoCompletionPort`），且可能失败。工厂方法把全部设置逻辑封装在一处，返回**值语义**对象，调用者负责生命周期。

### 1.3 异步读写的执行流程

```cpp
task<void> count_lines(io_service& io, path p) {
    auto file = read_only_file::open(io, p);   // ① 打开（同步）
    char buf[4096];
    // ② 异步读取
    size_t n = co_await file.read(offset, buf, sizeof(buf));
    //    ↑ 内部: 构造 file_read_operation (win32_overlapped_operation 子类)
    //           try_start() → ReadFile(hFile, buf, size, &overlapped, nullptr)
    //           IOCP 完成 → on_operation_completed → resume()
}
```

`file_read_operation` 继承自 `win32_overlapped_operation`（CRTP，详见上一篇），在 `try_start()` 中调用 `ReadFile()` 发起实际的 Windows 异步读取。

### 1.4 Range 请求

```cpp
size_t n = co_await file.read(offset=1024, buffer, size=512);
```

Windows `OVERLAPPED.Offset` / `OffsetHigh` 自带偏移读取——不需要 `lseek`。

---

## 2. socket —— 协程化的网络通信

### 2.1 静态创建

```cpp
namespace cppcoro::net {
    class socket {
        static socket create_tcpv4(io_service& ioSvc);
        static socket create_tcpv6(io_service& ioSvc);
        static socket create_udpv4(io_service& ioSvc);
        static socket create_udpv6(io_service& ioSvc);
    };
}
```

所有创建函数都自动绑定到 `io_service` 的 IOCP。

### 2.2 协程化的 API

每种 I/O 操作都是**独立的 awaiter 类型**，命名清晰，不是一套回调：

```cpp
Awaitable<void> connect(const ip_endpoint& remote) noexcept;
Awaitable<void> accept(socket& newSocket) noexcept;
Awaitable<size_t> send(const void* buf, size_t size) noexcept;
Awaitable<size_t> recv(void* buf, size_t size) noexcept;
// UDP: send_to / recv_from
```

### 2.3 回声服务器示例

```cpp
task<void> handle_connection(socket s) {
    try {
        const size_t bufferSize = 16384;
        auto buffer = std::make_unique<unsigned char[]>(bufferSize);
        size_t bytesRead;
        do {
            bytesRead = co_await s.recv(buffer.get(), bufferSize);    // ① 读
            size_t bytesWritten = 0;
            while (bytesWritten < bytesRead) {
                bytesWritten += co_await s.send(                       // ② 写（循环写全）
                    buffer.get() + bytesWritten, bytesRead - bytesWritten);
            }
        } while (bytesRead != 0);
        s.close_send();                                // ③ 半关闭
        co_await s.disconnect();                       // ④ 断开
    } catch (...) {
        // 连接异常 → 静默关闭
    }
}

task<void> echo_server(ipv4_endpoint endpoint, io_service& ioSvc, cancellation_token ct) {
    async_scope scope;  // 管理所有连接协程
    auto listeningSocket = socket::create_tcpv4(ioSvc);
    listeningSocket.bind(endpoint);
    listeningSocket.listen();

    while (true) {
        auto connection = socket::create_tcpv4(ioSvc);
        co_await listeningSocket.accept(connection, ct);
        scope.spawn(handle_connection(std::move(connection)));  // 每连接一协程
    }
    co_await scope.join();
}
```

"每连接一协程"模型——每个 TCP 连接是一个独立协程，在 `async_scope` 下协同运行。如果你用过 Go 或写过 asio，这个模型应该非常熟悉。

---

## 3. 无锁序列原语 —— LMAX Disruptor 的协程化

### 3.1 问题：高并发下怎么协调生产者-消费者？

传统方案：`std::mutex` + `std::queue` → 高竞争时性能崩。cppcoro 借鉴 LMAX Disruptor，用**序列号**在环形缓冲区上无锁协调。

### 3.2 从有锁到无锁：三步推导

#### Step 1：有锁环形缓冲（简单但慢）

```cpp
template<typename T>
class LockedRingBuffer {
    std::mutex mtx;
    std::vector<T> buf;
    size_t readPos = 0, writePos = 0;
    // 每次 push/pop 都加锁 → 高竞争下性能崩
};
```

#### Step 2：原子序列号（单生产者）

只有一个生产者，不需要锁：

```cpp
template<typename SEQUENCE>
class single_producer_sequencer {
    std::atomic<SEQUENCE> m_nextToClaim;  // 生产者：下一个写入位置

    template<typename SCHEDULER>
    auto claim_one(SCHEDULER& scheduler) {
        // 挂起等待消费者读完、释放 slot
        co_await consumerBarrier.wait_until_published(
            m_nextToClaim - m_bufferSize, scheduler);
        co_return m_nextToClaim++;  // 分配到了
    }
};
```

**背压机制**：`m_nextToClaim - m_bufferSize`——生产者最多领先消费者 `bufferSize` 个位置。消费者跟不上？生产者在 `claim_one()` 中挂起等待。不丢数据，不爆内存。

#### Step 3：多生产者（fetch_add 分配槽位）

多个生产者并发 → 每个用 `fetch_add` 原子分配序列号范围：

```cpp
const SEQUENCE first = m_nextToClaim.fetch_add(m_count, std::memory_order_relaxed);
// ↑ 原子分配 m_count 个槽位：[first, first+m_count)
```

**乱序发布问题**：线程 A 拿了序列号 [5,6,7]，线程 B 拿了 [8,9]。但 B 可能比 A 先写完。消费者不能假设序列号严格递增，需要**发布数组**：

```cpp
// 每个槽位标记是否已发布
void publish(SEQUENCE sequence) noexcept {
    m_published[sequence & m_sequenceMask].store(sequence, std::memory_order_seq_cst);
}

// 消费者：找最大连续已发布的序列号
SEQUENCE last_published_after(SEQUENCE lastKnownPublished) const noexcept {
    SEQUENCE seq = lastKnownPublished + 1;
    while (m_published[seq & m_sequenceMask].load(std::memory_order_acquire) == seq) {
        lastKnownPublished = seq++;
    }
    return lastKnownPublished;  // 停在空洞处
}
```

如果 `m_published[5] == 5` 但 `m_published[4] ≠ 4` → 序列号 5 已发布但有空洞 → 消费者停在空洞处等待。

### 3.3 `sequence_barrier` —— 协程等待序列号

这是"协程化"的关键——不是忙等，而是挂起：

```cpp
template<typename SEQUENCE, typename TRAITS>
class sequence_barrier {
    alignas(CPPCORO_CPU_CACHE_LINE)  // 第一条 cache line
    std::atomic<SEQUENCE> m_lastPublished;

    alignas(CPPCORO_CPU_CACHE_LINE)  // 第二条 cache line（避免伪共享！）
    mutable std::atomic<awaiter_t*> m_awaiters;
};
```

> 💡 为什么两条 cache line？生产者频繁写 `m_lastPublished`，等待者频繁读/写 `m_awaiters`。如果在同一条 cache line 上，生产者每次写都会把等待者的 cache line 刷掉（false sharing）。分开后互不干扰。

#### `publish()` —— 唤醒满足条件的等待者

```cpp
void publish(SEQUENCE sequence) noexcept {
    m_lastPublished.store(sequence, std::memory_order_seq_cst);

    // 原子取走整条等待者链表
    auto* awaiters = m_awaiters.exchange(nullptr, std::memory_order_acquire);
    if (awaiters == nullptr) return;

    // 分两组：已满足（resume）和未满足（requeue）
    awaiter_t* awaitersToResume = nullptr;
    awaiter_t* awaitersToRequeue = nullptr;

    do {
        if (TRAITS::precedes(sequence, awaiters->m_targetSequence)) {
            // 目标序列号还没到 → requeue
            *awaitersToRequeueTail = awaiters;
        } else {
            // 目标序列号已到 → resume
            *awaitersToResumeTail = awaiters;
        }
        awaiters = awaiters->m_next;
    } while (awaiters != nullptr);

    // Requeue 未满足的（CAS 放回链表）
    // Resume 已满足的（先 scheduler.schedule() 转移线程再 resume）
}
```

**三步模式**（在 cppcoro 中反复出现）：
1. `exchange(nullptr)` 原子取走整条链表
2. 遍历分组（resume / requeue）
3. 批量 resume + requeue

### 3.4 `sequence_traits` —— 处理序列号回绕

```cpp
template<typename SEQUENCE>
struct sequence_traits {
    using difference_type = std::make_signed_t<SEQUENCE>;

    static constexpr difference_type difference(value_type a, value_type b) {
        return static_cast<difference_type>(a - b);
        // 转换为有符号 → 正确处理回绕
        // 例: a=1, b=4294967295 (uint32_t, -1) → diff=2 → a 在 b 之后
    }
};
```

当序列号从 `4,294,967,295` 回绕到 `0` 时，有符号减法正确判断先后顺序。

---

## 4. 为什么 Hical 不用 Disruptor？

读到这里你可能在想：Hical 项目的写队列为什么用的是 Vyukov MPSC 而不是 Disruptor？

这是一个"不是越复杂越好"的实战案例：

| 概念     | cppcoro (Disruptor)                      | Hical (Vyukov MPSC)                      |
| -------- | ---------------------------------------- | ---------------------------------------- |
| 无锁队列 | 序列号 + 环形缓冲                        | 指针直接存入                             |
| 发布顺序 | 多生产者允许乱序（需要发布数组）         | 严格 FIFO                                |
| 背压     | `claim_one` 阻塞（bufferSize 限制）      | 队列无限（出队限批 256，不背压生产者）   |
| 协程等待 | `sequence_barrier::wait_until_published` | 不需要（asio 完成回调处理等待）          |
| 适用场景 | 高吞吐消息管道（金融订单匹配引擎）       | HTTP 响应发送（每响应几 KB，队列深度<5） |

Disruptor 的序列号+环形缓冲更适合高吞吐消息管道。Hical 的写队列场景是 HTTP 响应——字节量不大、队列深度通常小于 5。用 Vyukov 队列就够了，Disruptor 是过度设计。**选方案不是选最牛的，是选最合适的。**

---

## 5. 系列完结：回顾一下你走完了什么

从第 0 篇的 30 行 Generator，到这里，我们拆完了 cppcoro 的全部核心模块。如果用一句话总结：**cppcoro 的价值不在于它提供了什么 API，而在于它用 12,000 行代码展示了 C++ 协程库设计的全部关键模式。**

这些模式跨模块复现：

| 模式                       | 出现位置                                     |
| -------------------------- | -------------------------------------------- |
| `atomic<void*>` 三态编码   | 4 个同步原语 + shared_task                   |
| 侵入式链表 (CAS 无锁入队)  | 4 个事件 + async_mutex + 全局队列            |
| `exchange → 分组 → resume` | sequence_barrier + async_auto_reset_event    |
| 链表反转 → FIFO            | async_mutex + recursive_generator + 全局队列 |
| 对称转移条件编译           | task + async_generator + shared_task         |
| 双 cache line 对齐         | sequence_barrier                             |

这些不是 cppcoro 特有的——任何要写协程框架的人都会遇到这些问题。而 cppcoro 提供了一套经过验证的解法。

---

## 回顾一下这篇你学会了什么

1. **文件 I/O 类型层次**：`file` → `readable_file`/`writable_file` → 具体类，静态工厂
2. **socket 每种 I/O 操作是独立的 awaiter 类型**，不是一套回调
3. **"每连接一协程"**——和你写过的任何协程网络服务同构
4. **Disruptor 三步推导**：有锁 → 原子序列号 → 多生产者 `fetch_add`
5. **序列号回绕**：有符号减法自动处理
6. **双 cache line 对齐**避免 false sharing
7. **`exchange → 分组 → resume/requeue`** 三步模式
8. **多生产者乱序发布**：发布数组 + 扫描连续已发布序列号

---

## 常见坑

### 坑 1：bufferSize 必须是 2 的幂

```cpp
assert(bufferSize > 0 && (bufferSize & (bufferSize - 1)) == 0);
// 否则 idx & mask ≠ idx % bufferSize
```

### 坑 2：sequence_barrier 发布非单调递增序列号

```cpp
barrier.publish(5);
barrier.publish(3);  // 错误！序列号必须单调递增！
```

### 坑 3：在等待者被唤醒前销毁 barrier

```cpp
{
    sequence_barrier barrier;
    co_await barrier.wait_until_published(10, scheduler);
}  // barrier 析构 → 等待者还在链表上 → assert 触发！
```

必须确保所有等待者都处理完再销毁 barrier。

---

> 专栏到此完结。如果你从头跟到尾，现在应该能从第一性原理出发，理解一个协程库的全部核心组件。
> 有问题直接在评论区留言，每一条我都会看。
