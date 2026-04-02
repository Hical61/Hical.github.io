+++
date = '2026-04-25'
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

---

## 八、思考题

1. `EventLoopThread` 构造函数里立刻启动了线程并阻塞等待 `promiseForLoopPointer_`。如果 `EventLoop` 构造函数内部抛出异常（例如 eventfd 创建失败），会发生什么？`promise` 的析构行为是怎样的？

2. `EventLoopThreadPool::getNextLoop()` 使用 `memory_order_relaxed`。如果有两个线程同时调用 `getNextLoop()`，会不会拿到同一个 loop？这是 bug 吗？（提示：分析 Round-Robin 分配的目的）

3. `EventLoopThread` 的 `loopFuncs()` 里用 `thread_local static shared_ptr<EventLoop>`，而不是普通的 `EventLoop loop`。除了传回指针的需要，还有什么生命周期上的考虑？

4. `run()` 用 `std::call_once` 保证只执行一次。`~EventLoopThread()` 里也调用了 `run()`。如果用户在 `~EventLoopThread()` 析构之前已经手动调用过 `run()`，析构时的 `run()` 会做什么？这是正确行为吗？

---

*学习日期：2026-04-02 | 上一课：[第12课_TcpServer与TcpClient](第12课_TcpServer与TcpClient.md) | 下一课：[第14课_任务队列](第14课_任务队列.md)*

---

## 核心收获

- 三阶段 `promise/future` 启动：① Loop 指针就绪 ② 主线程放行 loop() ③ queueInLoop 确认已在运行，解决"返回指针但 loop 还没跑"的竞态
- 分离"构造 EventLoop"与"开始 loop()"：允许在 loop 启动前安全注册定时器、Channel 等
- `thread_local static shared_ptr<EventLoop>`：在 Loop 线程上可用 `getEventLoopOfCurrentThread()` 获取自身
- `EventLoopThreadPool` 用 `atomic<size_t>` + `fetch_add(relaxed)` 实现无锁 Round-Robin
- `std::call_once` 保证 `run()` 幂等，析构时的 `run()` 调用不会造成问题
