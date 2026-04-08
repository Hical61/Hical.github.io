+++
date = '2026-03-28'
draft = false
title = '多线程 EventLoop — EventLoopThread & EventLoopThreadPool'
categories = ["网络编程"]
tags = ["C++", "trantor", "多线程", "EventLoopThread", "学习笔记"]
description = "trantor 多线程 EventLoop 解析，EventLoopThread 与 EventLoopThreadPool 的线程模型。"
+++


# 第 13 课：多线程 EventLoop — EventLoopThread & EventLoopThreadPool

> 对应源文件：
> - `trantor/net/EventLoopThread.h` / `EventLoopThread.cc` — 在独立线程运行一个 EventLoop
> - `trantor/net/EventLoopThreadPool.h` / `EventLoopThreadPool.cc` — EventLoop 线程池

---

## 一、为什么需要这两个类？

在第 12 课里，我们看到 `TcpServer::setIoLoopNum(4)` 内部创建了一个 `EventLoopThreadPool`。它们解决的核心问题是：

**如何安全地在新线程里创建 EventLoop，并确保 EventLoop 真正开始运行后再返回给调用者？**

这看起来简单，实际上有一个微妙的同步问题：
- `EventLoop` 对象必须在它将要运行的线程里创建（`t_loopInThisThread` 线程局部变量）
- 调用者拿到 `EventLoop *` 之前，要保证该指针有效（对象已创建）
- `run()` 返回之前，要保证 EventLoop 确实进入了 `loop()` 主循环（否则第一个 `runInLoop` 可能无法立刻执行）

trantor 用三个 `std::promise` 精确解决了这个三阶段同步问题。

---

## 二、EventLoopThread 的三阶段启动协议

### 2.1 成员变量一览

```cpp
std::shared_ptr<EventLoop> loop_;          // EventLoop 对象（新线程里创建）
std::mutex loopMutex_;                     // 保护 loop_ 的读写（析构时用）
std::string loopThreadName_;               // 线程名（prctl 设置）
std::promise<std::shared_ptr<EventLoop>> promiseForLoopPointer_;  // ① EventLoop 指针就绪
std::promise<int> promiseForRun_;          // ② "请开始循环"信号
std::promise<int> promiseForLoop_;         // ③ "循环已在运行"确认
std::once_flag once_;                      // 保证 run() 只执行一次
std::thread thread_;                       // 实际的 OS 线程
```

### 2.2 三阶段时序图

```
主线程                                      新线程（loopFuncs）
  │                                              │
  │ EventLoopThread(name)                        │
  │  → thread_ = std::thread(loopFuncs)         │ 线程启动
  │  → f = promiseForLoopPointer_.get_future()  │
  │  ↓ 阻塞等待 ①                                │ prctl(PR_SET_NAME)
  │                                              │ loop = make_shared<EventLoop>()
  │                                        【①】│ promiseForLoopPointer_.set_value(loop)
  │  ← f.get() 返回 loop 指针                    │
  │  → this->loop_ = loop                       │ promiseForLoop_: queueInLoop 注册回调
  │                                              │ f2 = promiseForRun_.get_future()
  │ （构造完成，loop_ 有效，但循环未开始）          │ ↓ 阻塞等待 ②
  │                                              │
  │ run()                                        │
  │  → std::call_once {                         │
  │      f3 = promiseForLoop_.get_future()       │
  │  【②】promiseForRun_.set_value(1)            │
  │  ↓ 阻塞等待 ③                                │ ← f2.get() 返回
  │                                              │ loop->loop() 开始
  │                                              │ → 第一次 poll
  │                                              │ → doRunInLoopFuncs()
  │                                        【③】│   → promiseForLoop_.set_value(1)
  │  ← f3.get() 返回                             │ （循环持续运行...）
  │  }
  │ （run() 返回，EventLoop 确保在运行中）
```

**三个 promise 的职责**：

| Promise                  | 从哪里 set                        | 在哪里 get           | 含义                           |
| ------------------------ | --------------------------------- | -------------------- | ------------------------------ |
| `promiseForLoopPointer_` | 新线程 `loopFuncs()`              | 主线程构造函数       | EventLoop 对象已创建，指针有效 |
| `promiseForRun_`         | 主线程 `run()`                    | 新线程 `loopFuncs()` | "请开始执行 loop()"            |
| `promiseForLoop_`        | 新线程 loop 内的 queueInLoop 回调 | 主线程 `run()`       | EventLoop 真正进入了循环       |

### 2.3 为什么构造和运行分离？

```cpp
// 构造时线程启动，但 loop 不运行
EventLoopThread elt("IO-Thread-1");
// 此时 elt.getLoop() 有效，可以往里投递任务

// 调用 run() 才真正开始循环
elt.run();
// run() 返回后，EventLoop 确保已在 loop() 中
```

**使用场景**：`EventLoopThreadPool` 利用这个分离，先把所有 `EventLoopThread` 全部构造好（线程都启动了，EventLoop 都创建了），再统一调用 `start()` 让所有线程同时开始循环。这样所有线程几乎同时进入工作状态，避免先启动的线程抢先处理任务而后启动的线程还没准备好的问题。

### 2.4 thread_local static EventLoop

```cpp
void EventLoopThread::loopFuncs()
{
    ::prctl(PR_SET_NAME, loopThreadName_.c_str());
    thread_local static std::shared_ptr<EventLoop> loop =
        std::make_shared<EventLoop>();
    // ...
}
```

`thread_local static` 的含义：
- `thread_local`：每个线程各有一份独立的变量
- `static`：在函数内部，生命周期延续到线程结束（不是函数调用结束）

为什么用 `shared_ptr` 而不是直接 `EventLoop`？

因为 `promiseForLoopPointer_` 需要把指针传回主线程（`loop_` 是 `shared_ptr<EventLoop>`），用 `shared_ptr` 可以安全共享所有权。线程结束后，`thread_local` 变量析构，`shared_ptr` 引用计数减 1；如果主线程的 `loop_` 也 reset 了，EventLoop 就被销毁。

### 2.5 `run()` 的 `call_once` 保护

```cpp
void EventLoopThread::run()
{
    std::call_once(once_, [this]() {
        auto f = promiseForLoop_.get_future();
        promiseForRun_.set_value(1);   // 解锁新线程
        f.get();                       // 等循环真正开始
    });
}
```

`std::call_once` 保证多次调用 `run()` 只执行一次。`EventLoopThreadPool::start()` 和 `~EventLoopThread()` 都会调用 `run()`，不用担心重复触发。

### 2.6 析构：优雅退出

```cpp
EventLoopThread::~EventLoopThread()
{
    run();    // 确保循环已启动（如果构造后没有调用 run，析构时启动再关闭）

    std::shared_ptr<EventLoop> loop;
    {
        std::unique_lock<std::mutex> lk(loopMutex_);
        loop = loop_;   // 取出 shared_ptr，防止析构过程中 loop_ 被置 nullptr
    }
    if (loop) {
        loop->quit();   // 通知 EventLoop 退出 loop() 主循环
    }
    if (thread_.joinable()) {
        thread_.join(); // 等线程结束
    }
}
```

**为什么先 `run()` 再 `quit()`？**

如果 `EventLoopThread` 构造后从未调用过 `run()`（异常路径或忘记调用），新线程会一直阻塞在 `promiseForRun_.get_future().get()`，永远不会结束。析构时先调 `run()`，让线程进入 loop，再 `quit()` 让它退出，保证 `join()` 不会死锁。

---

## 三、EventLoopThreadPool

### 3.1 结构

```cpp
class EventLoopThreadPool : NonCopyable {
  private:
    std::vector<std::shared_ptr<EventLoopThread>> loopThreadVector_;
    std::atomic<size_t> loopIndex_{0};  // 无锁轮询计数器
};
```

极简设计：一个线程向量 + 一个原子计数器，仅此而已。

### 3.2 构造 vs start()

```cpp
// 构造：创建所有线程（但 EventLoop 未运行）
EventLoopThreadPool(size_t threadNum, const std::string &name)
{
    for (size_t i = 0; i < threadNum; ++i) {
        loopThreadVector_.emplace_back(
            std::make_shared<EventLoopThread>(name));
        // 此时每个 EventLoopThread 的新线程已启动
        // EventLoop 已创建，getLoop() 指针有效
        // 但 loop() 还未开始运行
    }
}

// start()：让所有 EventLoop 同时开始循环
void EventLoopThreadPool::start()
{
    for (auto &loopThread : loopThreadVector_) {
        loopThread->run();   // 解锁各线程，使其进入 loop()
    }
}
```

这个两阶段设计的价值：调用 `start()` 之前，可以往每个 loop 里投递初始化任务（通过 `getLoop(id)->queueInLoop()`）。这些任务会在 loop 开始时第一批被执行，实现"启动时的初始化工作"。

### 3.3 `getNextLoop()` — 无锁 Round-Robin

```cpp
EventLoop *EventLoopThreadPool::getNextLoop()
{
    if (loopThreadVector_.size() > 0) {
        // fetch_add：原子地取出当前值并加 1
        // memory_order_relaxed：不需要内存屏障，只需原子性
        size_t index = loopIndex_.fetch_add(1, std::memory_order_relaxed);
        return loopThreadVector_[index % loopThreadVector_.size()]->getLoop();
    }
    return nullptr;
}
```

**`memory_order_relaxed` 为什么安全？**

`loopIndex_` 只是一个计数器，我们只需要保证"每次加 1"是原子的（不会丢失更新），不需要同步其他内存操作。`relaxed` 提供最弱的原子保证（仅原子性），是最高效的选择。

计数器溢出（`size_t` 回绕到 0）也是安全的，因为取模操作依然正确。

### 3.4 完整接口

```cpp
EventLoop *getNextLoop();       // 轮询，每次调用返回下一个 loop
EventLoop *getLoop(size_t id);  // 按下标取，id >= size() 返回 nullptr
std::vector<EventLoop *> getLoops() const;  // 取所有 loop（TcpServer 用这个）
size_t size();                  // 线程数量
void wait();                    // 阻塞等待所有线程退出
```

---

## 四、EventLoopThread 启动时序的精密性

用一张图展示整个启动过程为什么是"精密"的：

```
时间线（主线程视角）：

EventLoopThread elt("worker");
  │
  ├─ std::thread 启动，新线程开始执行 loopFuncs()
  │
  ├─ 主线程阻塞在 promiseForLoopPointer_.get_future().get()
  │
  │  [新线程] prctl(PR_SET_NAME, "worker")
  │  [新线程] loop = make_shared<EventLoop>()   ← EventLoop 在新线程的栈上创建
  │                                              ← t_loopInThisThread 指向此 loop
  │  [新线程] loop->queueInLoop(设置 promiseForLoop_)
  │  [新线程] promiseForLoopPointer_.set_value(loop)  ← 解锁主线程
  │
  ├─ 主线程拿到 loop_，构造函数返回
  │  （此时可以调用 getLoop()，指针有效）
  │
  │  [新线程] 阻塞在 promiseForRun_.get_future().get()
  │
elt.run();
  │
  ├─ std::call_once 执行（只执行一次）
  ├─ f3 = promiseForLoop_.get_future()
  ├─ promiseForRun_.set_value(1)  ← 解锁新线程
  │
  │  [新线程] 收到 promiseForRun_ 信号
  │  [新线程] loop->loop() 开始！
  │  [新线程] 第一次 poll → doRunInLoopFuncs()
  │  [新线程]   → 执行 promiseForLoop_.set_value(1)  ← 解锁主线程的 f3.get()
  │
  ├─ 主线程 f3.get() 返回
  └─ run() 返回
     （保证：loop 已经在 loop() 里了，不只是"快要进入"）
```

**"快要进入" vs "已经进入"的区别**：

如果 `run()` 只等 `promiseForRun_.set_value`（即只等"新线程收到信号"），主线程可能比新线程快，导致 `runInLoop` 投递的第一个任务在 `loop->loop()` 调用之前就被队列，要等到第一次 `poll` 返回后才执行。用 `promiseForLoop_`（在 loop 内部 queueInLoop 设置）保证：run() 返回时，EventLoop 至少已经完成了一次完整的循环迭代，任何紧接着 run() 之后的 `runInLoop` 都能被及时处理。

---

## 五、与 TcpServer 的协作

```
TcpServer::setIoLoopNum(4)
  → loopPoolPtr_ = make_shared<EventLoopThreadPool>(4)
  → loopPoolPtr_->start()
     → 创建 4 个 EventLoopThread（4 个线程启动，EventLoop 创建）
     → 各线程 run()，EventLoop 开始循环
  → ioLoops_ = loopPoolPtr_->getLoops()
     → [loop0, loop1, loop2, loop3]

TcpServer::start()
  → acceptorPtr_->listen()（Accept 线程开始监听）

新连接到来
  → TcpServer::newConnection()
  → ioLoop = ioLoops_[nextLoopIdx_++ % 4]（Round-Robin）
  → TcpConnectionImpl(ioLoop, fd, ...)
  → conn->connectEstablished()
     → ioLoop->runInLoop(...)（在对应 I/O 线程注册 epoll 事件）
```

---

## 六、任务队列（TaskQueue）简介

课程表中的第 14/15 课（`SerialTaskQueue`、`ConcurrentTaskQueue`）是另一类线程模型，与 EventLoopThreadPool 的区别：

|              | EventLoopThreadPool            | SerialTaskQueue      | ConcurrentTaskQueue |
| ------------ | ------------------------------ | -------------------- | ------------------- |
| 执行模型     | 每线程一个 EventLoop，I/O 驱动 | 单线程顺序执行任务   | 线程池并行执行任务  |
| 任务类型     | I/O 事件 + 定时器 + 普通任务   | 纯计算/阻塞任务      | 纯计算/阻塞任务     |
| 是否阻塞 I/O | 不能（会影响 I/O 响应延迟）    | 独立线程，不影响 I/O | 独立线程池          |
| 典型用途     | 网络收发、定时器               | 串行数据库操作       | 并行文件处理        |

游戏服务器中，**数据库操作**（查询玩家数据）绝不能放在 EventLoop 线程里（会阻塞 I/O），应该用 `SerialTaskQueue` 保证操作顺序，或 `ConcurrentTaskQueue` 并行执行。

---

## 七、游戏服务器实践

### 7.1 标准的多线程服务器配置

```cpp
// 实践：Accept 线程 + 4 个 I/O 线程
EventLoop mainLoop;       // Accept 线程
TcpServer server(&mainLoop, InetAddress(9000), "GameGateway");
server.setIoLoopNum(4);   // 内部创建 EventLoopThreadPool(4)

server.start();
mainLoop.loop();   // 主线程进入 Accept 循环
```

### 7.2 访问特定 I/O 线程

```cpp
auto pool = std::make_shared<EventLoopThreadPool>(4);
pool->start();

// 向第 0 个线程投递初始化任务
pool->getLoop(0)->runInLoop([]() {
    // 初始化该线程的本地资源（如线程局部的数据库连接）
    initThreadLocalResources();
});

// 轮询分配任务
pool->getNextLoop()->runInLoop([]() {
    doSomeWork();
});
```

### 7.3 在 `start()` 之前做初始化

```cpp
EventLoopThreadPool pool(4, "IO");
// 此时线程已启动，EventLoop 已创建，但 loop() 未运行
// 可以安全地往各个 loop 投递初始化任务

for (size_t i = 0; i < pool.size(); ++i) {
    pool.getLoop(i)->queueInLoop([i]() {
        // 这段代码会在 start() 后 loop 开始时第一批执行
        LOG_INFO << "IO线程 " << i << " 初始化完成";
    });
}

pool.start();   // 此后投递的任务和上面的任务按顺序执行
```

## 核心收获

- 三阶段 `promise/future` 启动：① Loop 指针就绪 ② 主线程放行 loop() ③ queueInLoop 确认已在运行，解决"返回指针但 loop 还没跑"的竞态
- 分离"构造 EventLoop"与"开始 loop()"：允许在 loop 启动前安全注册定时器、Channel 等
- `thread_local static shared_ptr<EventLoop>`：在 Loop 线程上可用 `getEventLoopOfCurrentThread()` 获取自身
- `EventLoopThreadPool` 用 `atomic<size_t>` + `fetch_add(relaxed)` 实现无锁 Round-Robin
- `std::call_once` 保证 `run()` 幂等，析构时的 `run()` 调用不会造成问题

---

## 八、思考题

1. `EventLoopThread` 构造函数里立刻启动了线程并阻塞等待 `promiseForLoopPointer_`。如果 `EventLoop` 构造函数内部抛出异常（例如 eventfd 创建失败），会发生什么？`promise` 的析构行为是怎样的？

2. `EventLoopThreadPool::getNextLoop()` 使用 `memory_order_relaxed`。如果有两个线程同时调用 `getNextLoop()`，会不会拿到同一个 loop？这是 bug 吗？（提示：分析 Round-Robin 分配的目的）

3. `EventLoopThread` 的 `loopFuncs()` 里用 `thread_local static shared_ptr<EventLoop>`，而不是普通的 `EventLoop loop`。除了传回指针的需要，还有什么生命周期上的考虑？

4. `run()` 用 `std::call_once` 保证只执行一次。`~EventLoopThread()` 里也调用了 `run()`。如果用户在 `~EventLoopThread()` 析构之前已经手动调用过 `run()`，析构时的 `run()` 会做什么？这是正确行为吗？

---

## 九、思考题参考答案

### 1. `EventLoop` 构造函数抛异常时会发生什么？`promise` 的析构行为是怎样的？

**分析异常传播路径：**

`EventLoopThread` 构造函数中：

```cpp
EventLoopThread::EventLoopThread(const std::string &threadName)
    : loop_(nullptr),
      loopThreadName_(threadName),
      thread_([this]() { loopFuncs(); })   // 线程启动
{
    auto f = promiseForLoopPointer_.get_future();
    loop_ = f.get();   // ← 主线程阻塞在这里
}
```

新线程的 `loopFuncs()` 中：

```cpp
void EventLoopThread::loopFuncs()
{
    thread_local static std::shared_ptr<EventLoop> loop =
        std::make_shared<EventLoop>();   // ← 如果这里抛异常
    // ...
    promiseForLoopPointer_.set_value(loop);  // ← 这行不会执行
}
```

如果 `EventLoop` 构造函数抛异常（例如 Linux 上 `eventfd()` 失败，或 Windows 上 IOCP 创建失败），`make_shared<EventLoop>()` 会抛出异常。此时：

1. **新线程的行为**：异常在 `loopFuncs()` 中未被捕获，传播到 `std::thread` 的入口函数。根据 C++ 标准，如果线程函数抛出未捕获的异常，`std::terminate()` 会被调用，**整个进程终止**。

2. **如果假设不会 `terminate`**（比如有全局异常处理器）：`promiseForLoopPointer_` 从未调用 `set_value()` 或 `set_exception()`，新线程结束后 `promise` 对象在新线程栈上析构。根据 C++ 标准：
   - 如果 `promise` 被析构时既没有 `set_value` 也没有 `set_exception`，析构函数会存储一个 `std::future_error`（错误码 `broken_promise`）到共享状态中
   - 主线程阻塞在 `f.get()` 上，此时 `get()` 会抛出 `std::future_error` 异常

3. **主线程的行为**：`f.get()` 抛出 `std::future_error`，`EventLoopThread` 构造函数异常退出。由于 `thread_` 成员已经构造（线程已启动），但构造函数异常意味着对象未成功创建，析构函数不会被调用。`thread_` 作为成员变量会被自动析构——如果线程还在运行且 `joinable()`，`std::thread` 析构函数会调用 `std::terminate()`。

**总结**：无论哪条路径，`EventLoop` 构造失败都会导致进程终止。这在实际中是合理的——如果连事件循环都创建不了（系统资源耗尽），服务器已经无法正常工作，快速失败（fail-fast）是正确策略。

**`promise` 析构的关键规则**：
- 正常析构（已设置值）：释放共享状态
- 未设置值就析构：自动设置 `broken_promise` 异常到共享状态
- `future::get()` 会收到这个异常（类型为 `std::future_error`）

### 2. 两个线程同时调用 `getNextLoop()` 会不会拿到同一个 loop？这是 bug 吗？

**不会拿到同一个 loop**，原因在于 `fetch_add` 的原子性保证。

```cpp
EventLoop *EventLoopThreadPool::getNextLoop()
{
    if (loopThreadVector_.size() > 0)
    {
        size_t index = loopIndex_.fetch_add(1, std::memory_order_relaxed);
        return loopThreadVector_[index % loopThreadVector_.size()]->getLoop();
    }
    return nullptr;
}
```

`std::atomic::fetch_add(1, memory_order_relaxed)` 保证**原子性**：即使两个线程同时调用，每个线程都会拿到不同的 `index` 值。假设 `loopIndex_` 当前为 5：
- 线程 A 的 `fetch_add` 返回 5，`loopIndex_` 变为 6
- 线程 B 的 `fetch_add` 返回 6，`loopIndex_` 变为 7
- （或者 B 先得到 5，A 得到 6，取决于谁先执行，但不会重复）

`memory_order_relaxed` 只放宽了**内存可见性顺序**（不保证其他变量的写入对另一个线程可见的顺序），但不影响 `loopIndex_` 自身的原子性。"原子性"意味着 `fetch_add` 是一个不可分割的 RMW（read-modify-write）操作，不会有两个线程读到相同的值。

**但是，假设有一种极端情况需要讨论：**

如果 `loopIndex_` 溢出 `size_t` 的最大值回绕到 0 呢？答案是依然安全。`size_t` 的无符号溢出在 C++ 中是定义明确的行为（模 2^64 算术），回绕后取模仍然正确，只是从 `loopThreadVector_.size()-1` 跳回到 0，继续轮询。

**这不是 bug。** Round-Robin 的目的是**近似均匀分配**，不需要严格保证"连续两次调用一定分配到不同的 loop"。即使假设两个线程偶然拿到映射到同一个 loop 的不同 index（例如 index=0 和 index=4 在 4 线程池中都映射到 loop[0]），这也只是正常的轮询行为，不影响正确性。

### 3. `thread_local static shared_ptr<EventLoop>` 除了传回指针，还有什么生命周期考虑？

这个设计有两个关键的生命周期考量：

**考虑一：EventLoop 的生命周期超越 `loopFuncs()` 函数作用域**

如果用普通局部变量：

```cpp
void EventLoopThread::loopFuncs() {
    EventLoop loop;   // 栈上对象
    // ...
    loop.loop();      // 主循环
}   // ← loopFuncs() 返回时，loop 析构
```

这看起来没问题——`loop()` 返回后 `EventLoop` 析构。但有一个微妙问题：`EventLoop` 析构时，可能还有 pending 的 `queueInLoop` 任务引用了 loop 内部的数据结构。`thread_local static` 保证变量在**线程退出时才析构**（而不是函数返回时），这给了一个更长的生命周期窗口。

更重要的是，`loop_` 是 `shared_ptr<EventLoop>` 类型，`promiseForLoopPointer_` 把同一个 `shared_ptr` 传给了主线程的 `loop_` 成员。当 `loopFuncs()` 返回后：

```cpp
void EventLoopThread::loopFuncs()
{
    // ...
    loop->loop();   // 循环结束
    {
        std::unique_lock<std::mutex> lk(loopMutex_);
        loop_ = nullptr;   // 主线程的 loop_ 置空
    }
}   // ← 函数返回，但 thread_local static 的 shared_ptr 还活着
    //   引用计数 = 1（仅 thread_local 变量持有）
    //   线程退出时才析构，引用计数归零，EventLoop 销毁
```

如果用普通的 `shared_ptr<EventLoop> loop = ...`（不带 `thread_local static`），函数返回时 `loop` 析构，`loop_` 已被置为 nullptr，引用计数归零，`EventLoop` 立刻析构——但此时线程可能还没完全退出，某些全局的 `thread_local` 变量（如 `t_loopInThisThread`）可能还在引用这个 `EventLoop`。

**考虑二：`t_loopInThisThread` 的线程局部指针**

`EventLoop` 构造时会设置 `t_loopInThisThread = this`（线程局部变量，用于 `EventLoop::getEventLoopOfCurrentThread()`）。如果 `EventLoop` 对象比线程先析构，`t_loopInThisThread` 就变成悬垂指针。`thread_local static shared_ptr` 保证 `EventLoop` 的生命周期至少和线程一样长，避免了这个问题。

**考虑三：确保 `shared_from_this()` 安全**

`EventLoop` 虽然本身不继承 `enable_shared_from_this`，但它内部管理的对象（如 Timer、Channel）可能通过 `queueInLoop` 回调间接持有 `shared_ptr`。用 `thread_local static shared_ptr` 确保在所有 pending 回调执行完毕后，`EventLoop` 才被销毁。

**总结**：`thread_local static shared_ptr<EventLoop>` 的设计确保了三件事：(1) EventLoop 的生命周期与线程绑定；(2) 通过 `shared_ptr` 可以安全地跨线程共享所有权；(3) 析构顺序正确——线程退出时才释放 EventLoop。

### 4. `run()` 用 `call_once` 保证只执行一次，析构时的 `run()` 会做什么？

**如果用户已经手动调用过 `run()`，析构时的 `run()` 什么也不做。这是正确的行为。**

来看完整代码：

```cpp
void EventLoopThread::run()
{
    std::call_once(once_, [this]() {
        auto f = promiseForLoop_.get_future();
        promiseForRun_.set_value(1);   // 解锁新线程
        f.get();                       // 等循环真正开始
    });
}

EventLoopThread::~EventLoopThread()
{
    run();    // ← 这里
    // ...
    loop->quit();
    thread_.join();
}
```

`std::call_once(once_, lambda)` 的语义：
- `once_` 是 `std::once_flag` 类型
- 第一次调用时：执行 lambda，标记 `once_` 为"已执行"
- 后续所有调用：检测到 `once_` 已标记，**直接返回**，不执行 lambda

**场景一：用户手动调用过 `run()`**

```
elt.run();      // 第一次调用：call_once 执行 lambda，解锁新线程，等待循环开始
                // once_ 标记为"已执行"

// ... 运行中 ...

~EventLoopThread()
  run();        // 第二次调用：call_once 检测到 once_ 已标记，直接返回（空操作）
  loop->quit(); // 通知 EventLoop 退出
  thread_.join(); // 等线程结束
```

这是最正常的路径，析构时的 `run()` 是一个安全的 no-op。

**场景二：用户忘记调用 `run()`**

```
// 只构造，没调用 run()
~EventLoopThread()
  run();        // 第一次调用：call_once 执行 lambda
                // promiseForRun_.set_value(1) 解锁新线程
                // f.get() 等待循环开始
                // （新线程进入 loop()，然后 promiseForLoop_ 被设置）
                // run() 返回
  loop->quit(); // 立刻通知退出
  thread_.join(); // 等线程结束
```

这个路径保证了即使忘记 `run()`，析构也不会死锁。原因在之前课程中已分析过：如果不先 `run()`，新线程一直阻塞在 `promiseForRun_.get_future().get()`，永远不会结束，`thread_.join()` 会死锁。

**场景三：多线程同时调用 `run()`**

`std::call_once` 本身是线程安全的。如果多个线程同时调用 `run()`，只有一个线程会执行 lambda，其他线程会阻塞等待该 lambda 执行完成后再返回（不会跳过）。所以不存在竞态问题。

**这个设计的精妙之处**：析构函数中的 `run()` 调用不是"执行一次循环"的意思，而是"确保新线程被放行"的安全网。`call_once` 让 `run()` 成为**幂等操作**（多次调用和一次调用效果相同），从而在析构路径和正常路径之间提供了优雅的统一处理。

---

*学习日期：2026-03-28 | 上一课：[第12课_TcpServer与TcpClient]({{< relref "第12课_TcpServer与TcpClient.md" >}}) | 下一课：[第14课_任务队列]({{< relref "第14课_任务队列.md" >}})*
