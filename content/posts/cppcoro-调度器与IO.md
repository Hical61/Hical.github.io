+++
title = '协程调度器到底在调度什么？从 inline_scheduler 到工作窃取线程池'
date = '2024-11-25'
draft = false
tags = ["C++", "cppcoro", "协程", "C++20", "源码分析", "调度器", "IOCP"]
categories = ["C++"]
description = "拆开 cppcoro 给你看 · 第 5 篇。三层递进：从 inline_scheduler 到 static_thread_pool，最后看 IOCP 如何协程化。"
+++

# 协程调度器到底在调度什么？从 inline_scheduler 到工作窃取线程池

> 本专栏文章：拆开 cppcoro 给你看 · 第 5 篇

前几篇我们一直在协程"内部"兜圈子——task 怎么设计、同步原语怎么写、并发怎么编排。但有一个问题一直没认真回答：**协程在哪个线程上执行？**

`co_await` 之后你可能在任何线程上恢复——这取决于谁调了 `handle.resume()`。大部分时候这不是问题，但有时你确实想控制恢复的线程。这就是调度器的概念。

本文分三层递进：

```
Layer 1: 调度器概念（30 分钟能读完）
  ├─ inline_scheduler     — 22 行，不调度也是调度
  ├─ round_robin_scheduler — 125 行，对称转移调度的杰作
  └─ schedule_on vs resume_on

Layer 2: 工作窃取线程池（核心）
  ├─ 本地 LIFO 队列
  ├─ 全局 MPSC 队列
  ├─ 偷取 (work-stealing)
  └─ 睡眠/唤醒协议

Layer 3: IOCP + 取消 + 文件/网络 I/O
  ├─ OVERLAPPED 嵌入 awaiter (CRTP)
  ├─ 可取消操作的四态状态机
  ├─ cancellation_token/source/registration
  ├─ io_service + io_work_scope
  └─ async_scope
```

---

## Layer 1：调度器概念

### 问题：协程在哪个线程上执行？

```cpp
task<> my_coro() {
    std::cout << "I'm on thread " << std::this_thread::get_id() << std::endl;
    co_await some_io();
    std::cout << "Now I'm on thread " << std::this_thread::get_id() << std::endl;
    // ↑ 恢复后可能在完全不同的线程上！
}
```

### 1.1 `inline_scheduler` —— "不调度"也是调度

```cpp
class inline_scheduler {
public:
    std::suspend_never schedule() const noexcept {
        return {};  // 不挂起，原地继续
    }
};
```

返回 `suspend_never` 意味着 `co_await scheduler.schedule()` 等价于什么都不做。但它在泛型代码中有实际意义——你的函数接受一个调度器参数，`inline_scheduler` 就是"不需要调度的调度器"。

### 1.2 `round_robin_scheduler<N>` —— 用户态协程调度

这是整个 cppcoro 中我最欣赏的设计之一——不到 125 行代码实现了完整的用户态协程调度。灵感来自 Gor Nishanov 的 CppCon 2018 演讲。

#### 核心思想：N 个协程轮流执行，每次切换都是对称转移

```cpp
template<size_t N>
class round_robin_scheduler {
    size_t m_index = 0;                              // 当前轮到谁
    std::coroutine_handle<> m_noop;                  // 空操作句柄
    std::array<std::coroutine_handle<>, N - 1> m_coroutines;  // N-1 个等待的协程
};
```

为什么只存 N-1 个？当前正在执行的协程不需要存在数组里——它的句柄由调用者持有。

#### `schedule()`：时间片到了，别人上

```cpp
class schedule_operation {
    bool await_ready() noexcept { return false; }  // 永远挂起

    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> awaiting) noexcept {
        // 1. 当前协程存入槽位 m_index
        // 2. 从槽位 m_index 取出前一个挂起的协程
        // 3. m_index 轮转
        // 4. 返回取出的协程 → 对称转移！
        return m_scheduler.exchange_next(awaiting);
    }
};
```

#### 具体执行过程（N=3）

```
初始: m_coroutines = [A, B]  (C 正在执行)

C: co_await scheduler.schedule():
  exchange_next(C的句柄):
    prev = m_coroutines[0] = A
    m_coroutines[0] = C
    m_index = 1
    return A  ──对称转移──→  A 开始执行

A: co_await scheduler.schedule():
  exchange_next(A的句柄):
    prev = m_coroutines[1] = B
    m_coroutines[1] = A
    m_index = 2
    return B  ──对称转移──→  B 开始执行

B: co_await scheduler.schedule():
  exchange_next(B的句柄):
    prev = m_coroutines[2] = m_noop (空)
    m_coroutines[2] = B
    m_index = 0
    return m_noop → 没有协程可切换 → B 继续
```

> 💡 每次切换都是对称转移（tail-call jump），零额外栈帧。核心逻辑不到 125 行。

### 1.3 `schedule_on` vs `resume_on`

|            | `schedule_on`              | `resume_on`                      |
| ---------- | -------------------------- | -------------------------------- |
| 何时切线程 | awaitable 启动**之前**     | awaitable 完成**之后**           |
| 典型场景   | "在 IO 线程上执行这个任务" | "这个 IO 操作完成后回到 UI 线程" |
| 管道语法   | `taskA \| schedule_on(tp)` | `taskA \| resume_on(ui)`         |

---

## Layer 2：工作窃取线程池

### 2.1 问题：怎么把协程分到多个线程上执行？

最简单的方案：一个全局队列，所有线程从里面取任务。但全局队列有锁竞争，扩展性差。

cppcoro 的方案：**三层队列 + 工作窃取**。

```
     ┌─────────────────────────────────────────┐
     │          static_thread_pool              │
     │                                          │
     │  线程 0:  本地 LIFO 队列 ──┐              │
     │                           │              │
     │  线程 1:  本地 LIFO 队列 ──┤              │
     │                           ├─ 全局 MPSC 队列 │
     │  线程 2:  本地 LIFO 队列 ──┤              │
     │                           │              │
     │  窃取: 从其他线程本地队列尾部偷  ←──────────┘
     └─────────────────────────────────────────┘
```

### 2.2 本地队列（环形缓冲区 + 无锁 push/pop）

```cpp
class thread_state {
    unique_ptr<atomic<schedule_operation*>[]> m_localQueue;  // 环形缓冲
    size_t m_mask;               // bufferSize - 1 (2的幂)
    atomic<size_t> m_head;       // 生产者（本线程写）
    atomic<size_t> m_tail;       // 消费者（本线程 pop 头部，其他线程 steal 尾部）
    spin_mutex m_remoteMutex;    // 保护偷取操作
};
```

为什么用 2 的幂大小？`index & m_mask` 代替 `index % bufferSize`，位运算比除法快。

- **本地入队 (LIFO)**：只有本线程写 `m_head`，无锁
- **本地出队 (LIFO)**：本线程 pop `m_head`
- **偷取 (FIFO)**：从其他线程的 `m_tail` 偷最旧的任务

偷取从尾部取（最旧的任务）而不是头部（最新的）：
- 最旧的最有可能是其他线程入队的
- 最旧的 cache 可能已经不热了，偷走不伤偷取者的 cache
- 被偷的线程不会注意到——它从头部取

### 2.3 全局队列（MPSC）

当协程不在线程池线程上时（比如从 `main` 线程），任务被放入全局队列。生产端用 CAS 无锁入队（LIFO），消费端加锁反转链表变 FIFO——**又见链表反转模式**。

### 2.4 每个工作线程的主循环

```cpp
void run_worker_thread(uint32_t threadIndex) noexcept {
    while (true) {
        // 阶段 1：正常处理
        while (true) {
            op = try_local_pop();      // ① 先看自己的本地队列
            if (!op) {
                op = try_global_dequeue(); // ② 再看全局队列
                if (!op) {
                    op = try_steal();      // ③ 偷别人的
                    if (!op) break;        // 全空 → 休息
                }
            }
            op->m_awaitingCoroutine.resume();
        }

        // 阶段 2：自旋等待（~30 次 PAUSE）
        spin_wait spinWait;
        for (int i = 0; i < 30; ++i) {
            spinWait.spin_one();
            if (has_work()) goto normal_processing;
        }

        // 阶段 3：准备睡眠
        notify_intent_to_sleep(threadIndex);

        // 阶段 3.5：Double-check！（关键）
        if (has_any_queued_work_for(threadIndex)) {
            // 在标记"要睡了"和真正 sleep 之间，有人入队了！
            cancel_sleep();
            goto normal_processing;
        }

        // 阶段 4：真正睡眠
        sleep_until_woken();
    }
}
```

> ⚠️ 睡眠协议的 double-check 模式几乎所有高性能线程池都要用——标记意图睡眠后必须重新检查，否则会永久丢失唤醒信号。`m_sleepingThreadCount` 用 `seq_cst` 保证生产者看到"线程已睡"和睡眠线程看到"队列有新任务"之间的 happens-before 关系。

---

## Layer 3：IOCP + 取消 + 文件/网络 I/O

### 3.1 OVERLAPPED 嵌入 awaiter（CRTP）

这是 cppcoro IOCP 集成的精髓。Windows IOCP 用 `OVERLAPPED` 结构追踪异步 I/O。cppcoro 的做法：**把 `OVERLAPPED` 作为 awaiter 对象的基类**。

```
内存布局（read_file_operation 对象）:

  低地址  ┌─────────────────────────┐
         │  OVERLAPPED (基类)       │ ← IOCP 完成时通过这个找到对象
         │  ├─ Internal, InternalHigh│
         │  ├─ Offset, OffsetHigh    │
         │  └─ hEvent               │
         ├─────────────────────────┤
         │  m_errorCode             │
         │  m_numberOfBytesTransferred│
         ├─────────────────────────┤
         │  m_awaitingCoroutine     │ ← 完成回调直接 resume()
         ├─────────────────────────┤
         │  派生类特定数据           │ ← 文件句柄, buffer 指针
         └─────────────────────────┘

  IOCP 完成 → 通过 OVERLAPPED 地址找到对象 → 读 m_awaitingCoroutine → resume()
```

```cpp
template<typename OPERATION>
class win32_overlapped_operation : protected win32_overlapped_operation_base {
public:
    bool await_ready() const noexcept { return false; }  // I/O 总是异步发起

    bool await_suspend(std::coroutine_handle<> awaitingCoroutine) {
        m_awaitingCoroutine = awaitingCoroutine;
        return static_cast<OPERATION*>(this)->try_start();  // CRTP: 调用派生类
    }

private:
    static void on_operation_completed(io_state* ioState,
        dword_t errorCode, dword_t numberOfBytesTransferred, ...) noexcept {
        auto* op = static_cast<win32_overlapped_operation*>(ioState);
        op->m_errorCode = errorCode;
        op->m_numberOfBytesTransferred = numberOfBytesTransferred;
        op->m_awaitingCoroutine.resume();  // ← IOCP 回调直接 resume 协程！
    }
};
```

每个具体 I/O 操作（read/write/accept/connect）只需要实现 `try_start()` 和 `get_result()`。

### 3.2 可取消 I/O 的四态状态机

```cpp
enum class state {
    not_started,              // 还没发起 I/O
    started,                  // I/O 已发起
    cancellation_requested,   // 已取消（但 I/O 可能还没停）
    completed                 // I/O 完成
};
std::atomic<state> m_state;
```

最复杂的竞态：取消回调可能在 I/O 启动**之前**被触发。必须"先注册回调，再启动 I/O"，然后用 CAS 处理回调在 `try_start()` 之前就被触发的情况。

### 3.3 cancellation_token/source/registration

```
┌─────────────────────┐     ┌───────────────────┐     ┌────────────────────────┐
│ cancellation_source │────→│ cancellation_token │←────│ cancellation_          │
│ request_cancellation│     │ is_cancelled()    │     │ registration           │
│ token()             │     │ can_be_cancelled()│     │ (RAII, 析构时自动解注册)  │
└─────────────────────┘     └───────────────────┘     └────────────────────────┘
```

source 负责触发取消，token 负责查询状态，registration 负责注册回调。三者分离使得跨模块取消信号传递成为可能。

### 3.4 `io_service` 和 `async_scope`

- **io_service**：协程化的事件循环，`schedule()` → `PostQueuedCompletionStatus()` → IOCP worker 收到后 resume
- **io_work_scope**：RAII 工作引用计数，类似 asio 的 `work_guard`
- **async_scope**：轻量级结构化并发，`spawn()` 启动 fire-and-forget 协程，`join()` 等所有 spawned 协程完成

---

## 回顾一下你学会了什么

1. **`inline_scheduler`**：22 行，返回 `suspend_never`——不调度也是调度
2. **`round_robin_scheduler`**：对称转移驱动的用户态调度，N→N-1，125 行
3. **工作窃取三层队列**：本地 LIFO → 全局 MPSC FIFO → 偷取尾部
4. **睡眠协议 double-check**：标记意图睡眠 + 重新检查 + `seq_cst`
5. **`OVERLAPPED` 嵌入 awaiter (CRTP)**：IOCP 回调直接 `resume()`，零额外分配
6. **四态取消状态机**：not_started/started/cancellation_requested/completed
7. **取消回调先注册后启动**：防止 I/O 瞬间完成后回调永远不注册
8. **token/source/registration 分离**：source 触发，token 查询，registration 回调
9. **`schedule_on` vs `resume_on`**：启动前切线程 vs 完成后切线程
10. **`async_scope`**：原子计数 + 协程句柄回调，轻量级结构化并发

---

## 常见坑

### 坑 1：在 io_service 停止后再调用 schedule()

```cpp
io_service svc;
svc.stop();
co_await svc.schedule();  // 可能永远不被执行！IOCP 已经停了
```

### 坑 2：cancellation_registration 的回调不在协程线程上执行

```cpp
cancellation_registration reg(ct, [&shared_data] {
    shared_data.modify();  // ← 在 request_cancellation() 的调用者线程上执行！
});
```

回调执行的线程 = 调用 `cancellation_source::request_cancellation()` 的线程，不是你协程所在的线程。

### 坑 3：round_robin_scheduler 不能跨线程

`round_robin_scheduler` 的对称转移假设了单线程环境，不能跨线程使用。

---

> 下一篇：[IO 与无锁序列：cppcoro 的网络文件 I/O，以及 LMAX Disruptor 的协程化](06-文件IO与网络+07-无锁序列.md)
