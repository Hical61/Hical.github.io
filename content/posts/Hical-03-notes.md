+++
title = '第3课：Asio 事件循环与定时器'
date = '2026-04-15'
draft = false
tags = ["C++", "Boost.Asio", "事件循环", "线程池", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "深入理解 AsioEventLoop 如何封装 io_context，掌握 1线程:1事件循环的线程模型和 EventLoopPool 的 round-robin 分发机制。"
+++

# 第3课：Asio 事件循环与定时器 - 学习笔记

> 深入理解 AsioEventLoop 如何封装 io_context，掌握 1线程:1事件循环 的线程模型，理解 EventLoopPool 的 round-robin 分发机制。

---

## 一、AsioEventLoop — io_context 封装

### 1.1 整体结构

**源码位置**：`src/asio/AsioEventLoop.h` / `src/asio/AsioEventLoop.cpp`

```cpp
class AsioEventLoop : public EventLoop {
private:
    boost::asio::io_context ioContext_;                        // 核心：Asio 事件循环
    std::unique_ptr<work_guard<io_context::executor_type>> workGuard_;  // 防空退出
    std::thread::id threadId_;                                 // 所属线程 ID
    std::atomic<bool> running_{false};                         // 运行状态
    std::atomic<bool> quit_{false};                            // 退出标记

    size_t index_{0};                                          // 在线程池中的编号

    std::atomic<TimerId> nextTimerId_{1};                       // 定时器 ID 自增器
    std::map<TimerId, std::shared_ptr<AsioTimer>> timers_;     // 定时器注册表
    std::mutex timersMutex_;                                    // 注册表锁

    std::vector<Func> quitCallbacks_;                           // 退出钩子列表
    std::mutex quitMutex_;                                      // 钩子列表锁
};
```

### 1.2 构造函数 — work_guard 的创建

```cpp
AsioEventLoop::AsioEventLoop()
    : workGuard_(std::make_unique<work_guard<executor_type>>(
          boost::asio::make_work_guard(ioContext_)))
{}
```

**work_guard 是什么？**

`io_context::run()` 的行为是：处理所有待执行的任务，任务队列空了就返回。但事件循环需要**持续等待新事件**（新连接、数据到达、定时器触发），不能因为当前没任务就退出。

`work_guard` 相当于一个永不完成的"假任务"，它告诉 `io_context`："还有工作要做，别退出。"

```
没有 work_guard:
  io_context.run() → 队列空 → 返回 → 事件循环结束（❌ 不是我们想要的）

有 work_guard:
  io_context.run() → 队列空 → 发现 work_guard → 继续等待 → ... → stop() → 退出 ✓
```

### 1.3 run() — 启动事件循环

```cpp
void AsioEventLoop::run() {
    threadId_ = std::this_thread::get_id();   // 记录运行在哪个线程
    running_.store(true);
    quit_.store(false);

    ioContext_.run();    // 阻塞！直到 stop() 被调用

    // run() 返回后，执行退出钩子
    {
        std::lock_guard<std::mutex> lock(quitMutex_);
        for (auto& cb : quitCallbacks_) { cb(); }
        quitCallbacks_.clear();
    }

    running_.store(false);
}
```

**执行流程**：

```
run() 被调用（在某个线程上）
    │
    ├── 记录线程 ID → 用于后续 isInLoopThread() 判断
    ├── 设置 running_ = true
    │
    ├── ioContext_.run() ← 阻塞在这里！
    │     │
    │     ├── 处理 post() 投递的任务
    │     ├── 处理定时器回调
    │     ├── 处理 I/O 事件
    │     └── ... 循环处理直到 stop() 被调用
    │
    ├── 执行所有退出钩子 runOnQuit()
    └── 设置 running_ = false
```

### 1.4 stop() — 停止事件循环

```cpp
void AsioEventLoop::stop() {
    quit_.store(true);
    workGuard_.reset();     // 释放 work_guard → io_context 可以退出了
    ioContext_.stop();       // 立即中断 run()
}
```

**两步停止策略**：
1. `workGuard_.reset()` → 告诉 io_context 不再需要等待
2. `ioContext_.stop()` → 强制中断正在等待的 `run()`

为什么需要两步？`ioContext_.stop()` 是强制中断，但如果只做这一步，下次 `run()` 之前需要 `reset()` io_context。先释放 work_guard 是更优雅的做法。

### 1.5 dispatch vs post 的实现

```cpp
void AsioEventLoop::dispatch(Func cb) {
    if (isInLoopThread()) {
        cb();                      // 同线程：直接执行，零开销
    } else {
        post(std::move(cb));       // 跨线程：投递到队列
    }
}

void AsioEventLoop::post(Func cb) {
    boost::asio::post(ioContext_, std::move(cb));  // 总是投递到队列
}
```

**isInLoopThread() 的实现**：

```cpp
bool AsioEventLoop::isInLoopThread() const {
    return threadId_ == std::this_thread::get_id();
}
```

就是比较当前线程 ID 和 `run()` 时记录的线程 ID。

**游戏服务器类比**：
- `dispatch` ≈ "如果当前在主线程就直接处理消息，否则通过消息队列发到主线程"
- `post` ≈ "无论在哪，都发到消息队列下一帧处理"

### 1.6 定时器管理

```cpp
TimerId AsioEventLoop::runAfter(double delay, Func cb) {
    TimerId id = nextTimerId_.fetch_add(1);   // 原子自增，生成唯一 ID

    auto timer = std::make_shared<AsioTimer>(this, delay, std::move(cb));

    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        timers_[id] = timer;                   // 注册到表中
    }

    timer->start();                            // 启动定时器
    return id;
}

void AsioEventLoop::cancelTimer(TimerId id) {
    std::lock_guard<std::mutex> lock(timersMutex_);
    auto it = timers_.find(id);
    if (it != timers_.end()) {
        it->second->cancel();                  // 取消定时器
        timers_.erase(it);                     // 从注册表移除
    }
}
```

**定时器管理的数据结构选择**：
- `std::map<TimerId, shared_ptr<AsioTimer>>` — 有序映射
- 用 `map` 而不是 `unordered_map`：定时器数量通常不多（百级别），map 的有序性在调试时更友好
- `shared_ptr` 管理定时器生命周期：注册表和异步回调各持有一份引用

### 1.7 PMR 分配器

```cpp
std::pmr::polymorphic_allocator<std::byte> AsioEventLoop::allocator() const {
    return MemoryPool::instance().threadLocalAllocator();
}
```

直接返回线程本地池的分配器。因为 1 Thread : 1 EventLoop，所以 EventLoop 关联的分配器就是当前线程的线程本地池。

---

## 二、AsioTimer — steady_timer 封装

### 2.1 整体结构

**源码位置**：`src/asio/AsioTimer.h` / `src/asio/AsioTimer.cpp`

```cpp
class AsioTimer : public Timer, public std::enable_shared_from_this<AsioTimer> {
private:
    AsioEventLoop* loop_;                     // 所属事件循环
    boost::asio::steady_timer timer_;          // Asio 定时器（基于 steady_clock）
    Callback callback_;                        // 回调函数
    double interval_;                          // 间隔（秒）
    bool repeating_;                            // 是否周期执行
    std::atomic<bool> cancelled_{false};        // 是否已取消
};
```

**为什么用 steady_timer 而不是 system_timer？**

| 定时器         | 时钟源         | 特点                            |
| -------------- | -------------- | ------------------------------- |
| `steady_timer` | `steady_clock` | 单调递增，不受系统时钟调整影响  |
| `system_timer` | `system_clock` | 受 NTP 同步、用户手动改时间影响 |

游戏服务器中，"5秒后执行"一定是 5 秒，不能因为系统时钟被调整就提前或延后。所以定时器必须用 `steady_clock`。

### 2.2 单次定时器的实现

```cpp
void AsioTimer::scheduleOnce() {
    if (cancelled_.load()) return;

    // 设置超时时间
    timer_.expires_after(std::chrono::milliseconds(static_cast<int>(interval_ * 1000)));

    // 异步等待
    timer_.async_wait(
        [this, self = shared_from_this()](const boost::system::error_code& ec) {
            handleTimeout(ec);
        });
}

void AsioTimer::handleTimeout(const boost::system::error_code& ec) {
    if (ec || cancelled_.load()) return;   // 错误或已取消 → 忽略
    callback_();                            // 执行回调
}
```

**`self = shared_from_this()` 的作用**：

异步等待期间，AsioTimer 对象必须保持存活。Lambda 捕获 `self`（一个 `shared_ptr`）确保引用计数至少为 1，对象不会被提前析构。

**`ec` 错误码检查**：

`timer_.cancel()` 被调用时，等待中的 `async_wait` 回调会以 `boost::asio::error::operation_aborted` 错误码被触发。`if (ec) return;` 就是在这里过滤掉取消事件。

### 2.3 周期定时器的实现 — 递归调度

```cpp
void AsioTimer::scheduleRepeating() {
    if (cancelled_.load()) return;

    timer_.expires_after(std::chrono::milliseconds(static_cast<int>(interval_ * 1000)));

    timer_.async_wait(
        [this, self = shared_from_this()](const boost::system::error_code& ec) {
            if (ec || cancelled_.load()) return;

            callback_();                  // 执行回调

            if (!cancelled_.load()) {
                scheduleRepeating();       // 递归：重新调度下一次
            }
        });
}
```

**执行流程**：

```
scheduleRepeating()
    │
    ├── expires_after(100ms)
    ├── async_wait(callback)
    │       │
    │       │  ... 100ms 后 ...
    │       │
    │       ├── callback_()         ← 执行用户回调
    │       └── scheduleRepeating() ← 递归调度下一次
    │               │
    │               ├── expires_after(100ms)
    │               └── async_wait(callback)
    │                       │
    │                       │  ... 100ms 后 ...
    │                       │
    │                       ├── callback_()
    │                       └── scheduleRepeating()
    │                               └── ...
    │
    └── cancel() 被调用 → cancelled_ = true → 递归停止
```

这是经典的 **Asio 异步递归模式**：每次回调完成后重新注册下一次异步等待，实现周期执行。

**注意**：这不是真正的递归（不会爆栈），因为 `scheduleRepeating()` 只是注册异步操作然后立即返回，实际执行在下一次事件循环迭代中。

### 2.4 cancel — 原子标记 + 定时器取消

```cpp
void AsioTimer::cancel() {
    if (!cancelled_.exchange(true)) {   // CAS：只执行一次
        timer_.cancel();                 // 触发 async_wait 回调，ec = operation_aborted
    }
}
```

`exchange(true)` 是原子操作，返回旧值。如果旧值是 `false`（首次取消），则执行取消逻辑；如果已经是 `true`（重复取消），则跳过。这保证了 `timer_.cancel()` 只调用一次。

---

## 三、EventLoopPool — 多线程事件循环池

### 3.1 整体结构

**源码位置**：`src/asio/EventLoopPool.h` / `src/asio/EventLoopPool.cpp`

```cpp
class EventLoopPool {
private:
    std::vector<std::unique_ptr<AsioEventLoop>> loops_;  // N 个事件循环
    std::vector<std::thread> threads_;                     // N 个线程
    std::atomic<size_t> nextIndex_{0};                     // round-robin 计数器
    std::atomic<bool> running_{false};                     // 运行状态
};
```

### 3.2 线程模型：1 Thread : 1 io_context

```
EventLoopPool (4 threads)
│
├── Thread 0 ←→ AsioEventLoop[0] ←→ io_context[0]
│                    处理连接 0, 4, 8, 12, ...
│
├── Thread 1 ←→ AsioEventLoop[1] ←→ io_context[1]
│                    处理连接 1, 5, 9, 13, ...
│
├── Thread 2 ←→ AsioEventLoop[2] ←→ io_context[2]
│                    处理连接 2, 6, 10, 14, ...
│
└── Thread 3 ←→ AsioEventLoop[3] ←→ io_context[3]
                     处理连接 3, 7, 11, 15, ...
```

**对比另一种模型（1 io_context : N threads）**：

| 模型                  | 线程安全          | 特点                                                          |
| --------------------- | ----------------- | ------------------------------------------------------------- |
| **1:1（Hical 选择）** | 天然无竞争        | 每个连接固定在一个线程上，回调不需要加锁                      |
| N:1                   | 需要加锁或 strand | 多个线程共享一个 io_context，同一连接的回调可能在不同线程执行 |

Hical 选择 1:1 模型的原因：
1. **简单**：连接绑定到固定线程，所有回调天然串行，无需加锁
2. **Cache 友好**：同一连接的数据始终在同一 CPU 核心处理
3. **可预测**：没有锁竞争，延迟更稳定

### 3.3 构造 — 预创建所有 EventLoop

```cpp
EventLoopPool::EventLoopPool(size_t numThreads) {
    for (size_t i = 0; i < numThreads; ++i) {
        auto loop = std::make_unique<AsioEventLoop>();
        loop->setIndex(i);                  // 设置索引
        loops_.push_back(std::move(loop));
    }
}
```

构造时只创建 EventLoop 对象，不启动线程。

### 3.4 start — 为每个 EventLoop 分配一个线程

```cpp
void EventLoopPool::start() {
    if (running_.exchange(true)) return;  // 防止重复启动

    for (auto& loop : loops_) {
        auto* ptr = loop.get();
        threads_.emplace_back([ptr]() {
            ptr->run();                    // 每个线程运行一个 EventLoop
        });
    }
}
```

`running_.exchange(true)` 是原子操作：如果旧值是 `false` 则设为 `true` 并启动；如果已经是 `true` 则直接返回。

### 3.5 stop — 优雅关闭

```cpp
void EventLoopPool::stop() {
    if (!running_.exchange(false)) return;  // 防止重复停止

    // 1. 通知所有 EventLoop 停止
    for (auto& loop : loops_) {
        loop->stop();
    }

    // 2. 等待所有线程结束
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    threads_.clear();
}
```

**两阶段关闭**：
1. 先 `stop()` 所有事件循环 → 中断 `run()` 的阻塞
2. 再 `join()` 等待线程真正退出 → 确保所有退出钩子执行完毕

### 3.6 getNextLoop — Round-Robin 分发

```cpp
AsioEventLoop* EventLoopPool::getNextLoop() {
    if (loops_.empty()) return nullptr;

    size_t index = nextIndex_.fetch_add(1) % loops_.size();
    return loops_[index].get();
}
```

**Round-Robin（轮询）的工作方式**：

```
第 1 个连接 → nextIndex_ = 0 → loops_[0]
第 2 个连接 → nextIndex_ = 1 → loops_[1]
第 3 个连接 → nextIndex_ = 2 → loops_[2]
第 4 个连接 → nextIndex_ = 3 → loops_[3]
第 5 个连接 → nextIndex_ = 4 → loops_[0]  ← 回到第 0 个
...
```

**为什么用 Round-Robin 而不是"最少连接数"？**

| 策略            | 优点                    | 缺点                                 |
| --------------- | ----------------------- | ------------------------------------ |
| **Round-Robin** | O(1) 无锁原子操作，极快 | 不考虑连接负载差异                   |
| 最少连接数      | 更均匀的负载分配        | 需要统计每个 loop 的连接数，有锁竞争 |

在高并发场景下，Round-Robin 的 O(1) 无锁分发比精确的负载均衡更重要。连接数足够多时，统计上各 loop 的负载自然趋于均匀。

**`fetch_add` 的溢出安全性**：

`nextIndex_` 是 `size_t`（64位），假设每秒 100 万个新连接，溢出需要 ~58 万年。`% loops_.size()` 保证索引始终在范围内。

---

## 四、Echo Server 示例分析

**源码位置**：`examples/echo_server.cpp`

这是一个纯 Boost.Asio 的回声服务器（不使用 Hical 框架），展示协程式网络编程的基本模式：

### 4.1 会话协程

```cpp
awaitable<void> handleSession(tcp::socket socket) {
    try {
        char data[1024];
        for (;;) {
            auto bytesRead = co_await socket.async_read_some(
                boost::asio::buffer(data), use_awaitable);        // 异步读
            co_await boost::asio::async_write(
                socket, boost::asio::buffer(data, bytesRead), use_awaitable);  // 异步写（回显）
        }
    } catch (const std::exception&) {
        // 连接关闭
    }
}
```

**与回调式写法的对比**：

回调式（回调地狱）：
```cpp
void doRead(tcp::socket& socket) {
    socket.async_read_some(buffer, [&](error_code ec, size_t n) {
        if (!ec) {
            async_write(socket, buffer(data, n), [&](error_code ec, size_t) {
                if (!ec) {
                    doRead(socket);  // 递归回调
                }
            });
        }
    });
}
```

协程式（线性逻辑）：
```cpp
for (;;) {
    auto n = co_await socket.async_read_some(buffer, use_awaitable);
    co_await async_write(socket, buffer(data, n), use_awaitable);
}
```

协程版本像同步代码一样直观，错误处理用 try-catch，不需要在每层回调中检查 ec。

### 4.2 监听协程

```cpp
awaitable<void> listener(tcp::acceptor acceptor) {
    for (;;) {
        auto socket = co_await acceptor.async_accept(use_awaitable);  // 异步接受
        co_spawn(acceptor.get_executor(), handleSession(std::move(socket)), detached);
    }
}
```

- `co_await acceptor.async_accept()` → 等待新连接
- `co_spawn(..., handleSession(...), detached)` → 为每个连接启动独立协程
- `detached` → 协程完成后不关心结果（fire and forget）

### 4.3 main 函数

```cpp
int main() {
    boost::asio::io_context ioContext;
    tcp::acceptor acceptor(ioContext, tcp::endpoint(tcp::v4(), port));
    co_spawn(ioContext, listener(std::move(acceptor)), detached);
    ioContext.run();  // 阻塞，驱动所有协程
}
```

这是 Asio 协程应用的标准模式：创建 io_context → 启动初始协程 → `run()` 驱动事件循环。

---

## 五、从测试看完整用法

### 5.1 EventLoop 测试覆盖

**源码位置**：`tests/test_asio_event_loop.cpp`

| 测试                 | 验证点                                             |
| -------------------- | -------------------------------------------------- |
| `BasicLoopStartStop` | 启动后 isRunning=true，stop 后 isRunning=false     |
| `Post`               | 10 个 post 任务全部执行，计数器=10                 |
| `Dispatch`           | 跨线程 dispatch 最终在事件循环线程中执行           |
| `RunAfter`           | 延迟 200ms 的定时器确实在 ≥200ms 后触发            |
| `RunEvery`           | 100ms 间隔定时器在 350ms 内执行 3~4 次             |
| `CancelTimer`        | 创建后立即取消，回调不触发                         |
| `RunOnQuit`          | stop 后退出钩子全部执行                            |
| **`ThreadSafety`**   | **10 个线程同时 post，共 1000 个任务全部正确执行** |
| `PmrAllocator`       | 分配器可用，能创建 pmr::vector                     |

**线程安全测试特别重要**：

```cpp
TEST(AsioEventLoopTest, ThreadSafety) {
    // 10 个线程，每个投递 100 个任务
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 100; ++j) {
                loop.post([&]() { counter++; });
            }
        });
    }
    // 最终 counter 必须等于 1000
    EXPECT_EQ(counter.load(), 1000);
}
```

这验证了 `post()` 的线程安全性：`boost::asio::post()` 内部有锁保护，多线程同时投递不会丢失任务。

### 5.2 Timer 测试覆盖

**源码位置**：`tests/test_asio_timer.cpp`

| 测试                 | 验证点                                                     |
| -------------------- | ---------------------------------------------------------- |
| `RunOnce`            | 单次定时器触发一次，isActive/isRepeating/interval 属性正确 |
| `RunRepeatedly`      | 周期定时器在 350ms 内执行 3~4 次                           |
| `CancelOnce`         | 取消后单次定时器不触发                                     |
| `CancelRepeating`    | 取消后周期定时器停止，计数器不再增加                       |
| `TimingPrecision`    | 100ms 定时器的实际延迟在 80~150ms 范围内                   |
| `ViaEventLoop`       | 通过 EventLoop 接口使用定时器                              |
| `CancelViaEventLoop` | 通过 EventLoop 接口取消定时器                              |
| `GetLoop`            | getLoop() 返回正确的 EventLoop 指针                        |

**精度测试注意**：

```cpp
TEST(AsioTimerTest, TimingPrecision) {
    // 设置 100ms 定时器
    // 验证实际延迟在 80~150ms
    EXPECT_GE(delay, 80);    // 不能提前太多
    EXPECT_LE(delay, 150);   // 不能延迟太多
}
```

定时器精度受 OS 调度影响，通常有 ±50ms 的抖动。对游戏服务器来说，定时器精度在毫秒级别就足够了（心跳检测、定时存档等都不需要微秒级精度）。

---

## 六、关键设计模式总结

| 模式             | 应用                               | 说明                                     |
| ---------------- | ---------------------------------- | ---------------------------------------- |
| **RAII**         | work_guard、线程管理               | 构造时获取资源，析构时释放               |
| **原子操作**     | nextTimerId_、cancelled_、running_ | 无锁线程安全                             |
| **CAS**          | `cancelled_.exchange(true)`        | 原子性的"比较并交换"，保证只执行一次     |
| **异步递归**     | `scheduleRepeating()`              | 回调中重新注册异步操作，实现周期执行     |
| **生命周期捕获** | `self = shared_from_this()`        | Lambda 持有 shared_ptr，防止对象提前析构 |
| **Round-Robin**  | `getNextLoop()`                    | O(1) 无锁负载分发                        |
| **两阶段关闭**   | EventLoopPool::stop()              | 先通知停止，再等待线程退出               |

---

## 七、关键问题思考与回答

**Q1: work_guard 的作用是什么？如果不用会怎样？**

> `io_context::run()` 在没有待处理任务时会立即返回。work_guard 相当于一个"永不完成的假任务"，让 run() 持续阻塞等待新事件。不用的话，服务器启动后如果暂时没有连接进来，事件循环就退出了。
>
> **类比**：游戏服务器的主循环不会因为"当前没有玩家在线"就关闭。work_guard 确保事件循环始终待命。

**Q2: post() 和 dispatch() 的区别在哪里？什么时候用哪个？**

> - `dispatch()`：如果当前在事件循环线程 → 直接执行；否则 → 投递到队列
> - `post()`：无论如何都投递到队列，延迟到下一次循环迭代执行
>
> **用 dispatch 的场景**：保证线程安全的前提下尽量快。例如 `GenericConnection::close()` 中用 dispatch，如果已经在事件循环线程就直接关闭。
>
> **用 post 的场景**：需要避免递归/重入。例如回调中触发的操作不想在当前调用栈中执行。

**Q3: EventLoopPool 为什么采用 round-robin 而不是最少连接数？**

> 1. **性能**：round-robin 是 `fetch_add + 取模`，O(1) 无锁操作；最少连接数需要遍历或维护优先队列
> 2. **简单**：实现简单，不容易出 bug
> 3. **实际效果好**：大量连接时，各 loop 的负载统计上自然均匀
> 4. **连接数 ≠ 负载**：一个"空闲"连接和一个"高频通信"连接的负载完全不同，单纯按连接数分配并不一定更好

**Q4: 定时器回调中如何处理 operation_aborted 错误码？**

> `timer_.cancel()` 会导致正在等待的 `async_wait` 回调以 `operation_aborted` 错误码被触发。处理方式：
> ```cpp
> void handleTimeout(const boost::system::error_code& ec) {
>     if (ec || cancelled_.load()) return;  // 错误或已取消 → 忽略
>     callback_();
> }
> ```
> 同时检查 `ec`（来自 Asio）和 `cancelled_`（自己的标记），双保险。

---

## 八、与游戏服务器架构的对比

| Hical 概念             | 游戏服务器等价物                              | 说明                 |
| ---------------------- | --------------------------------------------- | -------------------- |
| `AsioEventLoop::run()` | 主循环 `while(running) { ProcessMessage(); }` | 阻塞处理事件         |
| `work_guard`           | 主循环的 `running` 标记                       | 防止循环提前退出     |
| `post()`               | `PostMessage()` / 消息队列                    | 跨线程投递消息       |
| `dispatch()`           | "能直接处理就处理，否则丢消息队列"            | 减少不必要的队列开销 |
| `EventLoopPool`        | 工作线程池（每个线程处理一组连接）            | 多核利用             |
| `Round-Robin` 分发     | 新连接按编号分配到工作线程                    | 简单高效             |
| `AsioTimer`            | 定时器系统（心跳、定时存档、活动倒计时）      | 周期/单次任务调度    |
| `runOnQuit()`          | 服务器关闭前的数据持久化                      | 优雅退出             |

---

*下一课：第4课 - PMR 内存管理，将深入三级内存池架构、TrackedResource 统计和 PmrBuffer 的实际应用。*
