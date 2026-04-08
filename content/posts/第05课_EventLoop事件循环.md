+++
date = '2026-03-10'
draft = false
title = 'EventLoop — 事件循环'
categories = ["网络编程"]
tags = ["C++", "trantor", "EventLoop", "事件循环", "学习笔记"]
description = "trantor 核心 EventLoop 事件循环机制解析，单线程事件驱动模型的设计与实现。"
+++


# 第 5 课：EventLoop — 事件循环

> 对应源文件：
> - `trantor/net/EventLoop.h` — 公共接口
> - `trantor/net/EventLoop.cc` — 实现
> - `trantor/utils/LockFreeQueue.h` — 无锁任务队列（`MpscQueue`）

---

## 一、EventLoop 是什么？

`EventLoop` 是 trantor **整个框架的心脏**，也是 Reactor 模式的核心。

一句话定义：**一个线程，一个循环，监听所有 I/O 事件和定时器，串行处理所有回调**。

```
┌─────────────────────────────────────────┐
│              EventLoop::loop()          │
│                                         │
│  while (!quit_) {                       │
│    ① poller_->poll(timeout)  ←阻塞等事件│
│    ② 处理所有就绪的 Channel 回调         │
│    ③ doRunInLoopFuncs()  ← 执行投递任务 │
│  }                                      │
└─────────────────────────────────────────┘
```

**三个核心原则**：
1. **One loop per thread**：一个 EventLoop 只属于一个线程，且一个线程最多一个 EventLoop
2. **所有 I/O 操作在 Loop 线程执行**：不跨线程操作 socket
3. **跨线程操作必须通过 `runInLoop/queueInLoop`**：把任务投递进去，由 Loop 线程执行

---

## 二、核心主循环 `loop()`

```cpp
// EventLoop.cc 第 204-266 行（精简版）
void EventLoop::loop()
{
    assert(!looping_);
    assertInLoopThread();                          // 必须在 Loop 线程调用
    looping_.store(true, std::memory_order_release);

    auto loopFlagCleaner = makeScopeExit(          // RAII：确保退出时清 looping_
        [this]() { looping_.store(false, ...); });

    while (!quit_.load(std::memory_order_acquire))
    {
        activeChannels_.clear();

        // ① 阻塞等待 I/O 事件（最长 10 秒）
#ifdef __linux__
        poller_->poll(kPollTimeMs, &activeChannels_);   // Linux: epoll
#else
        poller_->poll(timerQueue_->getTimeout(), &activeChannels_);
        timerQueue_->processTimers();  // 非 Linux: 手动处理定时器
#endif

        // ② 处理所有就绪 Channel 的回调
        eventHandling_ = true;
        for (auto *channel : activeChannels_) {
            currentActiveChannel_ = channel;
            channel->handleEvent();   // 分发读/写/错误回调
        }
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;

        // ③ 执行跨线程投递进来的任务
        doRunInLoopFuncs();
    }
    // 退出后执行 runOnQuit 注册的清理函数
    Func f;
    while (funcsOnQuit_.dequeue(f)) f();
}
```

### 主循环的三个阶段

```
Phase 1: poller_->poll()
   └─ 调用 epoll_wait（最多等 10 秒）
   └─ 返回：活跃的 Channel 列表（有读/写事件的 fd）

Phase 2: handleEvent()
   └─ 遍历活跃 Channel，各自回调
   └─ 例如：socket 可读 → RecvMessageCallback
            socket 可写 → WriteCompleteCallback

Phase 3: doRunInLoopFuncs()
   └─ 消费 funcs_ 队列（MpscQueue），执行所有投递进来的任务
   └─ 例如：其他线程调用了 queueInLoop(f)
```

---

## 三、wakeupFd：打破阻塞的关键

**问题**：`epoll_wait` 在等待时是阻塞的，如果其他线程此时投递了一个任务（`queueInLoop`），Loop 线程要等最多 10 秒才能执行。

**解决方案**：专门创建一个 fd 用于"唤醒"——往这个 fd 写入 1 字节，`epoll_wait` 立刻返回。

### 不同平台的实现

```
Linux:  eventfd（单个 fd，专为事件通知设计）
macOS/BSD: pipe（两个 fd：写端 wakeupFd_[1]，读端 wakeupFd_[0]）
Windows: IOCP 的 postEvent（直接向 IOCP 投递事件）
```

```cpp
// Linux：创建 eventfd（EventLoop.cc 第 53-63 行）
int createEventfd() {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    // EFD_NONBLOCK：非阻塞，避免意外阻塞
    // EFD_CLOEXEC：fork 后子进程自动关闭
    return evtfd;
}

// 写入唤醒（EventLoop.cc 第 359-373 行）
void EventLoop::wakeup() {
    uint64_t tmp = 1;
#ifdef __linux__
    write(wakeupFd_, &tmp, sizeof(tmp));   // 写 8 字节触发可读
#elif defined _WIN32
    poller_->postEvent(1);
#else
    write(wakeupFd_[1], &tmp, sizeof(tmp)); // pipe 写端
#endif
}

// 读取清零（防止 epoll 反复触发）
void EventLoop::wakeupRead() {
    uint64_t tmp;
    read(wakeupFd_, &tmp, sizeof(tmp));    // 读走，清零计数器
}
```

**构造函数中注册 wakeupFd**（EventLoop.cc 第 88-103 行）：
```cpp
wakeupChannelPtr_->setReadCallback(std::bind(&EventLoop::wakeupRead, this));
wakeupChannelPtr_->enableReading();  // 注册到 Poller，开始监听
```

`wakeupFd` 被包装成一个普通的 `Channel`，和所有业务 Channel 一起被 Poller 监听，没有特殊处理。

---

## 四、`runInLoop` vs `queueInLoop`

这是 EventLoop 最常用的两个接口，区分清楚非常重要。

### 4.1 `runInLoop`

```cpp
// EventLoop.h 第 126-136 行
template <typename Functor>
inline void runInLoop(Functor &&f)
{
    if (isInLoopThread())
        f();              // 当前就在 Loop 线程：直接执行
    else
        queueInLoop(std::forward<Functor>(f));  // 其他线程：投递
}
```

**逻辑**：
- 如果**当前线程就是 Loop 线程** → 立即同步执行 `f()`
- 如果**在其他线程** → 投入队列，Loop 线程下一轮执行

**适用场景**：不确定当前是否在 Loop 线程，都想保证在 Loop 线程执行。

### 4.2 `queueInLoop`

```cpp
// EventLoop.cc 第 273-288 行
void EventLoop::queueInLoop(Func &&cb)
{
    funcs_.enqueue(std::move(cb));  // 投入无锁队列

    // 以下情况需要唤醒 Loop 线程：
    // 1. 当前不是 Loop 线程（epoll_wait 可能在阻塞）
    // 2. 当前是 Loop 线程但正在执行 doRunInLoopFuncs
    //    （这说明是在某个 Func 里又投递了新任务，Loop 已跑完 poll 阶段，
    //     此轮不再有机会执行——需要唤醒让下轮执行）
    if (!isInLoopThread() || !looping_.load(std::memory_order_acquire))
    {
        wakeup();
    }
}
```

**唤醒条件的微妙之处**：

```
情况 A：其他线程调用 queueInLoop
  → 需要唤醒（epoll_wait 可能正在阻塞）

情况 B：Loop 线程在执行 handleEvent 时调用 queueInLoop
  → 不需要唤醒（当前循环执行完 handleEvent 后会执行 doRunInLoopFuncs）

情况 C：Loop 线程在执行 doRunInLoopFuncs 里的某个 Func 时，
         该 Func 又调用了 queueInLoop
  → 此时 isInLoopThread() == true，looping_ == true
  → 不唤醒——没问题！因为 doRunInLoopFuncs 是 while 循环，会继续消费
```

实际上条件 `!isInLoopThread() || !looping_` 中的 `!looping_` 是用来处理 **loop 还没开始跑** 时就有任务投入的情况。

### 4.3 两者的使用原则

```
其他线程想让 Loop 执行某操作：
    loop->runInLoop([conn]() { conn->send("hello"); });

    // 永远不要在非 Loop 线程直接操作 conn！
    // 错误示例（数据竞争）：
    conn->send("hello");  // ← 危险！
```

---

## 五、`MpscQueue` — 无锁任务队列

`funcs_` 和 `funcsOnQuit_` 都是 `MpscQueue<Func>`（多生产者单消费者无锁队列）。

### 5.1 数据结构

```
链表形式，head_ 是最新节点，tail_ 是最老节点（消费端）

入队（多线程安全）：
  new BufferNode(data)
  prevhead = head_.exchange(node)  ← 原子交换 head_
  prevhead->next_ = node           ← 链接前节点到新节点

出队（单线程）：
  tail = tail_
  next = tail->next_               ← 读下一节点
  if next == nullptr: 返回 false（空队列）
  output = move(*next->dataPtr_)
  tail_ = next                     ← 推进 tail_
  delete tail（旧哨兵节点）
```

### 5.2 内存布局示意

```
入队顺序: A → B → C

tail_        head_
  ↓            ↓
[哨兵] → [A] → [B] → [C] → nullptr

出队（先进先出）：
  dequeue → 得到 A
  tail_ 向右推进

[哨兵] → [B] → [C] → nullptr（A 的节点被删除，原 A 的节点成为新哨兵）
```

### 5.3 为什么能无锁？

**生产者**（多线程）只操作 `head_`，用 `exchange` 原子替换：
```cpp
BufferNode *prevhead = head_.exchange(node, std::memory_order_acq_rel);
prevhead->next_.store(node, std::memory_order_release);
```

**消费者**（单线程，即 Loop 线程）只操作 `tail_`，不与生产者竞争。

两端分离，天然无锁。对比互斥锁：高并发时无锁队列性能优于有锁队列，且不存在死锁风险。

### 5.4 内存序解释

| 操作                                  | 内存序  | 原因                                   |
| ------------------------------------- | ------- | -------------------------------------- |
| `head_.exchange(..., acq_rel)`        | acq_rel | 既要看到之前写入，也要让后续写入可见   |
| `prevhead->next_.store(..., release)` | release | 让消费者 acquire 时能看到完整数据      |
| `tail->next_.load(..., acquire)`      | acquire | 配对生产者的 release，确保看到完整数据 |

---

## 六、定时器接口

所有定时器接口最终都委托给 `TimerQueue`：

```cpp
// runAt：指定绝对时间
TimerId runAt(const Date &time, Func &&cb);

// runAfter：相对延迟（秒）
TimerId runAfter(double delay, Func &&cb);
// 也支持 chrono literals：
loop->runAfter(5s, task);      // C++14 字面量
loop->runAfter(10min, task);

// runEvery：周期执行
TimerId runEvery(double interval, Func &&cb);
loop->runEvery(1s, heartbeat);

// 取消定时器
loop->invalidateTimer(timerId);
```

**`runAt` 的实现细节**（EventLoop.cc 第 290-308 行）：

```cpp
TimerId EventLoop::runAt(const Date &time, Func &&cb)
{
    // 计算距现在的微秒差
    auto microSeconds = time.microSecondsSinceEpoch()
                      - Date::now().microSecondsSinceEpoch();

    // 转换为 steady_clock 时间点（避免系统时间跳变影响定时器）
    std::chrono::steady_clock::time_point tp =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(microSeconds);

    return timerQueue_->addTimer(std::move(cb), tp,
                                 std::chrono::microseconds(0));  // 0 = 不重复
}
```

**关键**：外部用 `Date`（wall clock，可能跳变），内部转成 `steady_clock`（单调时钟，不受系统时间调整影响）——这是一个重要的防护措施。

---

## 七、线程安全保障机制

### 7.1 `thread_local` 线程局部指针

```cpp
// EventLoop.cc 第 66 行
thread_local EventLoop *t_loopInThisThread = nullptr;
```

每个线程有独立的 `t_loopInThisThread`：
- 构造 EventLoop 时：`t_loopInThisThread = this`
- 析构时：`t_loopInThisThread = nullptr`
- 同一线程再建第二个 EventLoop：直接 `LOG_FATAL + exit(-1)`

```cpp
// 任何地方都可以拿到当前线程的 EventLoop
EventLoop *loop = EventLoop::getEventLoopOfCurrentThread();
// 返回 nullptr 说明当前线程没有 EventLoop
```

### 7.2 `isInLoopThread()` 保障

```cpp
bool isInLoopThread() const {
    return threadId_ == std::this_thread::get_id();
}
```

`threadId_` 在构造时固定（`std::this_thread::get_id()`），之后只读，无需加锁。

### 7.3 原子标志

```cpp
std::atomic<bool> looping_;   // 是否在 loop() 中
std::atomic<bool> quit_;      // 是否已请求退出
```

`quit()` 方法：
```cpp
void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);  // 设置退出标志
    if (!isInLoopThread()) {
        wakeup();  // 唤醒可能阻塞在 epoll_wait 的 Loop 线程
    }
    // Loop 线程自己调用：下次 while 检查时自然退出
}
```

---

## 八、`ScopeExit`：异常安全的 RAII 工具

EventLoop.cc 里内嵌了一个轻量级 RAII 辅助类（第 183-201 行）：

```cpp
template <typename F>
struct ScopeExit {
    ScopeExit(F &&f) : f_(std::forward<F>(f)) {}
    ~ScopeExit() { f_(); }  // 无论怎么退出，都执行 f_
    F f_;
};
```

用法：
```cpp
// 确保 loop() 退出时（无论正常退出还是抛异常）都清除 looping_ 标志
auto loopFlagCleaner = makeScopeExit(
    [this]() { looping_.store(false, std::memory_order_release); });
```

如果没有这个，一旦 `handleEvent` 里抛异常，`looping_` 永远不会被清除，析构函数里的 `while (looping_)` 就会死循环。

---

## 九、完整数据流示意图

```
【其他线程】
  loop->runInLoop(f)
       │
       ├─ isInLoopThread() == true → f() 立即执行
       │
       └─ isInLoopThread() == false
              │
              ▼
         funcs_.enqueue(f)     ← 无锁入队
              │
              ▼
         wakeup()              ← 写 wakeupFd
              │
              ▼
【Loop 线程 epoll_wait 被唤醒】
              │
              ▼
    wakeupChannelPtr_->handleEvent()
      → wakeupRead()           ← 读走 wakeupFd 的数据，清零
              │
              ▼
    doRunInLoopFuncs()
      → while funcs_.dequeue(func): func()   ← 执行所有投递的任务
```

---

## 十、EventLoop 成员变量一览

```cpp
// 线程相关
std::thread::id threadId_;           // 绑定线程 ID
std::atomic<bool> looping_;          // 是否在 loop() 中
std::atomic<bool> quit_;             // 退出标志
EventLoop **threadLocalLoopPtr_;     // 指向 thread_local 变量的指针

// I/O 多路复用
std::unique_ptr<Poller> poller_;     // epoll/kqueue/IOCP 封装
ChannelList activeChannels_;         // 本次 poll 返回的活跃 Channel
Channel *currentActiveChannel_;      // 当前正在处理的 Channel

// 唤醒机制
int wakeupFd_;                       // Linux: eventfd; macOS: pipe[0]
std::unique_ptr<Channel> wakeupChannelPtr_; // wakeupFd 对应的 Channel

// 任务队列
MpscQueue<Func> funcs_;              // 待执行任务（无锁）
bool callingFuncs_{false};           // 正在执行任务标志

// 定时器
std::unique_ptr<TimerQueue> timerQueue_;

// 退出回调
MpscQueue<Func> funcsOnQuit_;        // loop() 退出时执行
```

---

## 十一、游戏服务器中的应用模式

### 模式1：在任意线程安全修改玩家数据

```cpp
// 数据库线程查询完毕，需要修改玩家数据（玩家在 IO 线程管理）
void onDbQueryComplete(int playerId, const PlayerData &data) {
    // 不能直接修改！PlayerManager 在 IO Loop 线程
    ioLoop->runInLoop([playerId, data]() {
        playerManager.updatePlayer(playerId, data);
    });
}
```

### 模式2：定时广播

```cpp
// 每 10 秒广播一次服务器时间
loop->runEvery(10.0, [&]() {
    MsgBuffer timeMsg;
    buildTimeMsg(timeMsg);
    for (auto &[id, conn] : onlinePlayers) {
        conn->send(timeMsg);
    }
});
```

### 模式3：延迟关闭连接

```cpp
// 发送"你已被踢出"消息后，1 秒后强制关闭连接
conn->send(kickMsg);
loop->runAfter(1.0, [conn]() {
    conn->forceClose();
});
```

## 核心收获

- **核心不变量**：EventLoop 是单线程的，所有网络 I/O 操作必须在其所属线程执行
- `runInLoop(f)`：当前线程直接执行；其他线程 → `queueInLoop` → 写入 `MpscQueue` → 唤醒 → 下轮执行
- `wakeupFd_`（eventfd/pipe）：跨线程投递任务后写入触发 epoll，唤醒阻塞的 `poll()` 调用
- `MpscQueue<Func> funcs_`：任务队列采用无锁 MPSC 队列，多线程并发投递不需要 mutex
- 游戏服务器铁律：EventLoop 线程只做非阻塞 I/O 和轻量计算，数据库/文件操作必须卸载到 TaskQueue

---

## 十二、思考题

1. `runInLoop` 在 Loop 线程调用时直接执行 `f()`，而不是入队。这在什么情况下可能导致问题？（提示：考虑在 `handleEvent` 回调中调用 `runInLoop` 再操作当前 Channel）
2. `MpscQueue` 使用链表而非环形数组，有什么优缺点？
3. `quit()` 在非 Loop 线程调用时会 `wakeup()`，如果 Loop 线程恰好不在 `epoll_wait` 而在执行 `doRunInLoopFuncs`，这次 `wakeup()` 写入的值会不会"丢失"？下次循环会怎样？
4. `looping_` 和 `quit_` 都是 `atomic<bool>`，为什么不用普通 `bool` + `mutex`？两种方案各有什么优劣？

---

## 十三、思考题参考答案

### 1. `runInLoop` 在 Loop 线程直接执行 `f()` 可能导致什么问题？

**核心风险：在 `handleEvent` 回调中直接操作当前正在遍历的 Channel，破坏遍历不变量。**

看 `runInLoop` 的实现（EventLoop.h 第 126-136 行）：

```cpp
template <typename Functor>
inline void runInLoop(Functor &&f)
{
    if (isInLoopThread())
        f();              // 直接执行！
    else
        queueInLoop(std::forward<Functor>(f));
}
```

当 Loop 线程处于 `handleEvent` 阶段（EventLoop.cc 第 229-236 行），正在遍历 `activeChannels_` 列表时：

```cpp
eventHandling_ = true;
for (auto it = activeChannels_.begin(); it != activeChannels_.end(); ++it)
{
    currentActiveChannel_ = *it;
    currentActiveChannel_->handleEvent();  // ← 如果这里面调用了 runInLoop...
}
```

假设 Channel A 的 `readCallback_` 内调用了 `runInLoop(f)`，而 `f` 内部执行了：
- `channelB->disableAll()` + `channelB->remove()` — 删除了 Channel B
- Channel B 恰好也在本轮 `activeChannels_` 中，尚未被遍历到

此时 `f()` 被**立即同步执行**，Channel B 被移除甚至析构，后续遍历到 Channel B 时就是**悬空指针**。

**如果走 `queueInLoop` 路径**，`f` 会被推迟到 `doRunInLoopFuncs()` 阶段执行，此时 `activeChannels_` 的遍历早已结束，不存在迭代器失效问题。

**trantor 的防御措施**：
1. `tie()` 机制通过 `weak_ptr::lock()` 防止对象在 `handleEvent` 执行期间被析构
2. `Channel::handleEvent()` 检查 `events_ == kNoneEvent` 直接返回（Channel.cc 第 56 行），即使 Channel 已被 `disableAll()`，只要内存还在就不会崩溃
3. `currentActiveChannel_` 记录当前正在处理的 Channel，方便调试

**实际中这通常不是问题**的原因是：trantor 的设计让同一个 fd 的所有操作都通过同一个 Channel，不太会出现"A 的回调删除 B"的情况。但理论上这是直接执行的固有风险。

---

### 2. `MpscQueue` 使用链表而非环形数组的优缺点

**链表方案的优点**：

1. **无需预分配固定大小**：环形数组（如 `boost::lockfree::spsc_queue`）需要在创建时指定容量。如果容量不够，入队操作要么阻塞要么失败；如果容量过大，浪费内存。链表天然无界——只要堆内存够，就能一直入队。对于 EventLoop 这种"突发大量任务投递"的场景（例如服务器同时收到大量新连接），无界队列更安全。

2. **实现更简单**：无锁环形数组的 MPSC 实现需要处理复杂的 wrap-around（环绕）逻辑，且要用 CAS 循环来抢占槽位。链表只需一次 `exchange` 原子操作即可入队：

```cpp
// LockFreeQueue.h 第 56-57 行
BufferNode *prevhead{head_.exchange(node, std::memory_order_acq_rel)};
prevhead->next_.store(node, std::memory_order_release);
```

3. **无假满问题**：环形数组中如果消费者不够快，生产者会看到"队列满"。而链表不存在这个问题。

**链表方案的缺点**：

1. **每次入队/出队都有 `new`/`delete`**：每个任务入队时 `new BufferNode` + `new T`（LockFreeQueue.h 第 55 行），出队时 `delete dataPtr_` + `delete tail`（第 83-85 行）。频繁的堆分配/释放会产生：
   - 内存碎片
   - malloc/free 本身的锁争用（glibc malloc 内部有锁，虽然 tcmalloc/jemalloc 缓解了这个问题）
   - Cache miss：链表节点在堆上随机分布，不像数组那样连续

2. **缓存不友好**：环形数组的元素在内存中连续存放，CPU 缓存预取效果好（spatial locality）。链表节点散布在堆上，每次 `dequeue` 都可能产生 cache miss。

3. **内存开销更大**：每个节点额外存储一个 `atomic<BufferNode*> next_` 和一个 `T* dataPtr_` 指针，比数组直接存 `T` 多了两个指针（16 字节/节点在 64 位系统上）。

**trantor 的选择理由**：EventLoop 的任务队列并非超高频热路径（不是每个数据包都走队列，大部分操作是 Loop 线程内直接执行的）。无界 + 实现简单的优势大于缓存友好性的劣势。如果换成环形数组，还需要额外处理"队列满了怎么办"的策略，反而增加复杂度。

---

### 3. `wakeup()` 写入的值会不会"丢失"？

**不会丢失，下次循环一定能正确处理。**

分析具体时序：

```
时刻 T1: Loop 线程在执行 doRunInLoopFuncs()（不在 epoll_wait）
时刻 T2: 其他线程调用 quit()
         → quit_.store(true)
         → wakeup() → write(wakeupFd_, 1)
时刻 T3: Loop 线程 doRunInLoopFuncs() 执行完毕
时刻 T4: 回到 while 循环顶部检查 quit_
```

**情况分析**：

**Linux `eventfd` 的语义**：`eventfd` 内部维护一个 `uint64_t` 计数器。`write` 操作会**累加**到计数器上，`read` 操作会**读取并清零**。即使没有人立刻 `read`，写入的值不会丢失——它一直累积在内核的计数器中。

在上面的时序中：
- T2 写入 `wakeupFd_`，计数器变为 1
- T3 `doRunInLoopFuncs()` 结束
- T4 `while (!quit_.load(...))` — 这里 `quit_` 已经是 `true`，循环**直接退出**，根本不会再进入 `epoll_wait`

所以 wakeup 写入的值虽然没人读，但也无所谓——`quit_` 标志已经可见，循环已退出。

**另一种情况**：如果不是 `quit()` 而是 `queueInLoop(f)`：
- T2 其他线程 `funcs_.enqueue(f)` + `wakeup()`
- T3 `doRunInLoopFuncs()` 可能已经消费完本轮任务了
- T4 回到 `while` 顶部 → `poller_->poll()` → `epoll_wait`
- 此时 `wakeupFd_` 已经有数据（T2 写入的），`epoll_wait` **立即返回**
- `wakeupRead()` 消费掉计数器
- `doRunInLoopFuncs()` 消费 `f`

**关键点**：`eventfd` 写入的值永远不会丢失。最差情况就是 `epoll_wait` 立即返回一次，代价只是多一轮空循环，完全正确且安全。

**macOS/BSD `pipe` 的情况类似**：`pipe` 有内核缓冲区（通常 64KB），写入的字节会一直保存直到被读取。只要不溢出（而每次只写 8 字节），就不会丢失。

---

### 4. 为什么用 `atomic<bool>` 而不是 `bool` + `mutex`？

**`atomic<bool>` 的优势**：

1. **性能**：`atomic<bool>` 在 x86 上编译为普通的 `mov` 指令加上内存屏障（fence），没有锁的开销。而 `mutex` 的 `lock()/unlock()` 即使没有竞争，也要执行 `futex` 系统调用（Linux）或至少一次原子 CAS + 分支。在 EventLoop 的主循环中，`quit_` 每一轮都要检查，`looping_` 在析构函数中可能被自旋检查——这些是热路径，`atomic` 比 `mutex` 快一个数量级。

2. **不会死锁**：`quit_` 的使用场景是跨线程的简单标志读写。如果用 `mutex`，需要在 `quit()` 设置时加锁、在 `loop()` 检查时加锁。假设某个回调函数内部调用了 `quit()`，而这个回调恰好也持有了该 `mutex`——就会死锁。`atomic` 完全不存在这个问题。

3. **语义匹配**：`looping_` 和 `quit_` 只是简单的 `bool` 标志，操作只有 `load` 和 `store`，不需要"检查-修改-写回"这样的复合操作。这正是 `atomic<bool>` 的最佳适用场景。`mutex` 适合保护**多步骤复合操作**（例如"读取余额→扣减→写回"），用它来保护单个 `bool` 是大材小用。

4. **等待唤醒的成本**：如果用 `mutex` + `condition_variable`，析构函数里等待 `looping_` 变 `false` 需要用条件变量通知。而源码中析构函数用的是简单的自旋等待（EventLoop.cc 第 133-140 行）：

```cpp
while (looping_.load(std::memory_order_acquire))
{
    nanosleep(&delay, nullptr);  // 1ms 间隔自旋
}
```

这种低频自旋（只在析构时发生一次）用 `atomic` 最简单。

**`bool` + `mutex` 的优势（备选方案）**：

1. **更强的顺序保证**：`mutex` 提供 sequentially consistent 语义（`lock` = acquire, `unlock` = release），不需要手动指定 `memory_order`，出错概率更低。
2. **可配合条件变量**：如果需要"等到 `looping_` 变 false 再继续"这种等待，`condition_variable` 比自旋更节能。但析构只发生一次，自旋也可以接受。
3. **调试友好**：`mutex` 可以配合工具检测死锁、竞争等问题（如 ThreadSanitizer）。`atomic` 的内存序错误更难排查。

**结论**：对于简单的跨线程 `bool` 标志，`atomic<bool>` 是最优选择——性能好、无死锁风险、语义清晰。`mutex` 在这里是过度设计。

---

*学习日期：2026-03-10 | 上一课：[第04课_回调类型定义]({{< relref "第04课_回调类型定义.md" >}}) | 下一课：[第06课_Channel事件通道]({{< relref "第06课_Channel事件通道.md" >}})*
