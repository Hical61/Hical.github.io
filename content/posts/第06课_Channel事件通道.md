+++
date = '2026-03-12'
draft = false
title = 'Channel — 事件通道'
categories = ["网络编程"]
tags = ["C++", "trantor", "Channel", "事件通道", "学习笔记"]
description = "trantor Channel 事件通道解析，fd 与回调函数的绑定桥梁。"
+++


# 第 6 课：Channel — 事件通道

> 对应源文件：
> - `trantor/net/Channel.h` — 公共接口
> - `trantor/net/Channel.cc` — 实现（仅 112 行）

---

## 一、Channel 是什么？

Channel 是 trantor Reactor 模式中的**中间层**，它把一个文件描述符（fd）的事件管理和回调分发封装在一起。

```
fd ──── Channel ──── Poller（注册/更新感兴趣的事件）
              └────── EventLoop（事件就绪后，回调分发）
```

**三个关键概念**：
- `events_`：当前**感兴趣**的事件（告诉 Poller 我想监听什么）
- `revents_`：Poller 返回的**实际发生**的事件（poll/epoll 填写）
- 回调函数：事件发生后调用哪个函数

**Channel 不拥有 fd**——fd 的生命周期由 `Socket` 对象管理，Channel 只是"贴在" fd 上的事件管理标签。

---

## 二、事件标志位

```cpp
// Channel.cc 第 31-34 行
const int Channel::kNoneEvent  = 0;
const int Channel::kReadEvent  = POLLIN | POLLPRI;   // 可读 + 紧急数据
const int Channel::kWriteEvent = POLLOUT;             // 可写
```

**为什么用 `POLLIN/POLLOUT` 而不是 `EPOLLIN/EPOLLOUT`？**

`poll.h` 定义的 `POLLIN/POLLOUT` 是 POSIX 标准，数值上与 Linux 的 `EPOLLIN/EPOLLOUT` 完全相同。Windows 上通过宏重定义做了映射（Channel.cc 第 19-27 行）：

```cpp
#ifdef _WIN32
#define POLLIN  EPOLLIN    // 映射到 wepoll 的定义
#define POLLOUT EPOLLOUT
// ...
#endif
```

这样 Channel 的代码对上层保持统一，平台差异由宏处理。

**`POLLPRI`**：带外数据（Out-of-Band），TCP 紧急指针，极少用到，但 trantor 把它和 `POLLIN` 一起归入"可读"事件。

**Linux 独有：`POLLRDHUP`**（Channel.cc 第 91 行）：
```cpp
#ifdef __linux__
if (revents_ & (POLLIN | POLLPRI | POLLRDHUP))  // Linux 多了 POLLRDHUP
```
`POLLRDHUP` 表示对端关闭了写方向（半关闭），Linux 2.6.17+ 支持。收到这个事件说明对端不会再发数据了，可以提前知道连接即将关闭。

---

## 三、`events_` 的修改与 Poller 同步

每次修改 `events_` 后都要调用 `update()` 通知 Poller：

```cpp
void Channel::enableReading()  { events_ |= kReadEvent;   update(); }
void Channel::disableReading() { events_ &= ~kReadEvent;  update(); }
void Channel::enableWriting()  { events_ |= kWriteEvent;  update(); }
void Channel::disableWriting() { events_ &= ~kWriteEvent; update(); }
void Channel::disableAll()     { events_  = kNoneEvent;   update(); }
```

`update()` 的实现极简（Channel.cc 第 48-51 行）：
```cpp
void Channel::update() {
    loop_->updateChannel(this);
    // → Poller::updateChannel(this)
    // → epoll_ctl(epfd, EPOLL_CTL_MOD/ADD, fd, &ev)
}
```

**为什么每次都通知 Poller？**

`epoll` 内核维护一张表，记录每个 fd 感兴趣的事件。`events_` 是用户空间的"意图"，只改 `events_` 不调用 `epoll_ctl` 内核不会知道。`update()` 就是把用户的意图同步到内核。

---

## 四、`handleEvent` — 事件分发核心

当 Poller 返回活跃 Channel 后，EventLoop 调用 `channel->handleEvent()`：

```cpp
// Channel.cc 第 53-70 行
void Channel::handleEvent()
{
    if (events_ == kNoneEvent) return;  // 已注销，忽略

    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();  // 尝试提升 weak_ptr
        if (guard)
            handleEventSafely();  // 持有者存活，安全执行
        // guard 为空：持有者已析构，丢弃这次事件
    }
    else {
        handleEventSafely();
    }
}
```

### 4.1 `handleEventSafely` — 按 revents_ 分发回调

```cpp
// Channel.cc 第 71-110 行（精简）
void Channel::handleEventSafely()
{
    // 优先级 0：统一事件回调（设置后其他回调都不调用）
    if (eventCallback_) {
        eventCallback_();
        return;
    }

    // 优先级 1：连接关闭（HUP 且没有可读数据）
    if ((revents_ & POLLHUP) && !(revents_ & POLLIN)) {
        if (closeCallback_) closeCallback_();
    }

    // 优先级 2：错误
    if (revents_ & (POLLNVAL | POLLERR)) {
        if (errorCallback_) errorCallback_();
    }

    // 优先级 3：可读（含 POLLRDHUP on Linux）
    if (revents_ & (POLLIN | POLLPRI | POLLRDHUP)) {
        if (readCallback_) readCallback_();
    }

    // 优先级 4：可写（Windows 上排除 POLLHUP）
    if (revents_ & POLLOUT) {
        if (writeCallback_) writeCallback_();
    }
}
```

**事件分发优先级图**：

```
revents_ 到来
      │
      ├─ eventCallback_ 设置了？ → 调用，直接返回（不再分发其他）
      │
      ├─ POLLHUP && !POLLIN → closeCallback_()
      │
      ├─ POLLNVAL | POLLERR → errorCallback_()
      │
      ├─ POLLIN | POLLPRI | POLLRDHUP → readCallback_()
      │
      └─ POLLOUT → writeCallback_()
```

**为什么 POLLHUP 要排除 POLLIN？**

`POLLHUP` 表示连接挂断，但 TCP 半关闭时可能同时有 `POLLHUP | POLLIN`（对端关闭写，但本端还有数据没读完）。这种情况应该**先把数据读完**（`readCallback_`），不能直接关闭。所以关闭回调只在"纯 HUP 无数据"时才触发。

---

## 五、`tie()` — 防止回调时对象已析构

这是 Channel 中最精妙的设计之一。

**问题**：考虑以下场景：
1. `TcpConnection` 拥有一个 `Channel`
2. EventLoop 拿到活跃 Channel，准备调用 `handleEvent()`
3. 此时另一个线程（或本轮其他回调）销毁了 `TcpConnection`
4. `handleEvent()` 执行时，回调里引用的数据已经无效！→ **野指针崩溃**

**解决方案**：`tie()` 机制

```cpp
// Channel.h 第 269-273 行
void tie(const std::shared_ptr<void> &obj) {
    tie_ = obj;    // 存一个 weak_ptr
    tied_ = true;
}
```

使用方式（在 `TcpConnectionImpl` 中）：
```cpp
// 连接建立时，把自身 shared_ptr 绑定到 Channel
channel_->tie(shared_from_this());
```

`handleEvent` 中：
```cpp
std::shared_ptr<void> guard = tie_.lock();  // 尝试提升 weak_ptr
if (guard) {
    handleEventSafely();   // guard 存活 → 引用计数 +1，对象不会析构
}
// guard 析构时引用计数 -1
// guard 为 nullptr → 对象已销毁，跳过回调
```

**时序保障**：

```
EventLoop::loop() 中：
  ① activeChannels_ 遍历开始
  ② channel->handleEvent()
       → tie_.lock() → guard（引用计数从N变N+1）
       → handleEventSafely()（安全执行）
       → guard 析构（引用计数回N）
  ③ 下一个 Channel

即使在 ②~③ 之间 TcpConnection 被其他代码 reset()（引用计数降到N-1），
guard 还持有引用，N+1-1=N，对象依然存活直到 guard 析构
```

---

## 六、`eventCallback_` — 统一事件回调

大多数情况下 Channel 用分类回调（read/write/close/error）。

但有时需要一个"接管所有事件"的回调，比如 Windows 的 IOCP poller，事件类型语义不同，直接用统一回调更合适：

```cpp
// EventLoop.cc（Windows 路径）
poller_->setEventCallback([](uint64_t event) { assert(event == 1); });
```

设置了 `eventCallback_` 后，其他四个回调全部失效——`handleEventSafely` 的第一行直接 `return`。

---

## 七、Channel 在 Poller 中的三态

Channel 在 Poller（EpollPoller）内部有三种状态，用 `index_` 字段标记：

```cpp
// EpollPoller.cc 中定义
const int kNew     = -1;  // 从未添加到 epoll
const int kAdded   =  1;  // 已通过 epoll_ctl ADD 添加
const int kDeleted =  2;  // 曾经添加，现在已通过 epoll_ctl DEL 删除
```

**状态转移**：

```
[kNew]  ──enableReading()──► [kAdded]   (epoll_ctl ADD)
[kAdded] ──disableAll()────► [kDeleted] (epoll_ctl DEL，但 fd 还在 Poller 的 map 里)
[kDeleted] ──enableReading()► [kAdded]  (epoll_ctl ADD，重新激活)
[kAdded] ──remove()────────► 从 Poller map 删除（fd 彻底注销）
```

`kDeleted` 和彻底 `remove()` 的区别：
- `kDeleted`：fd 还在 Poller 的 `channelMap_` 中（可以快速重新激活）
- `remove()`：fd 从 `channelMap_` 删除，下次 `enableReading()` 时需重新 `ADD`

这个优化减少了"暂时禁用再重新启用"时的 `epoll_ctl` 调用次数。

---

## 八、Channel 完整生命周期

以一个 TCP 连接为例：

```
① accept() 得到新 fd
        │
        ▼
② TcpConnectionImpl 创建，同时创建 Channel(loop, fd)
        │
        ▼
③ channel->tie(shared_from_this())  ← 绑定生命周期
   channel->setReadCallback(...)
   channel->setWriteCallback(...)
   channel->setCloseCallback(...)
   channel->setErrorCallback(...)
        │
        ▼
④ channel->enableReading()          ← 注册到 Poller，开始监听读事件
        │
   ┌────┘
   │  数据到达
   ▼
⑤ Poller 返回活跃 Channel
   EventLoop 调用 channel->handleEvent()
   → readCallback_()（读数据，处理包）
        │
   发送数据时：
⑥ channel->enableWriting()          ← 临时开启写事件
   写缓冲区清空后：
   channel->disableWriting()         ← 关闭写事件（不要持续监听可写！）
        │
   连接关闭：
⑦ channel->disableAll()             ← 取消所有事件
   channel->remove()                 ← 从 Poller 注销
        │
        ▼
⑧ TcpConnectionImpl 析构，Channel 析构，fd 关闭
```

**为什么要"用完就关闭写事件"（步骤 ⑥）？**

`EPOLLOUT` 在 socket 发送缓冲区**有空间时就会持续触发**（水平触发 LT 模式）。如果一直开着写事件，没有数据要发的时候 `epoll_wait` 会立刻返回，进入空转（busy loop）。所以发送缓冲区清空后要立刻 `disableWriting()`。

---

## 九、Channel 与三个类的关系

```
Channel
  ├── 关联 EventLoop（loop_）
  │     通过 update() / remove() 通知 EventLoop → Poller
  │
  ├── 关联 fd（fd_）
  │     不拥有，只记录
  │
  └── 被 TcpConnectionImpl 持有（unique_ptr<Channel>）
        tie() 绑定生命周期（weak_ptr）
```

```
EventLoop ←──────── Channel ──────── Poller
   │       updateChannel()    updateChannel()
   │                               │
   │        handleEvent()          │ setRevents()
   └──────────────────────────────►│
```

---

## 十、五种回调对比

| 回调             | 触发条件                   | 典型用途                      |
| ---------------- | -------------------------- | ----------------------------- |
| `readCallback_`  | `POLLIN/POLLPRI/POLLRDHUP` | 读数据（`MsgBuffer::readFd`） |
| `writeCallback_` | `POLLOUT`                  | 发送缓冲区数据写入内核        |
| `closeCallback_` | `POLLHUP && !POLLIN`       | 通知 TcpServer 关闭连接       |
| `errorCallback_` | `POLLNVAL/POLLERR`         | 记录错误日志，关闭连接        |
| `eventCallback_` | 任何事件（优先）           | Windows IOCP 或特殊用途       |

---

## 核心收获

- Channel 不拥有 fd，只是 fd 的事件注册与回调分发代理，生命周期由持有者（如 TcpConnection）管理
- `tie(shared_ptr)` 防止 Channel 在 `handleEvent()` 中途被析构：持有所有者的 weak_ptr，执行回调前 lock()
- 三种状态 kNew/kAdded/kDeleted 驱动 `epoll_ctl` 的 ADD/MOD/DEL 操作
- `enableReading/Writing()` 修改 events_ 后调用 `update()` → `loop_->updateChannel()` → Poller 同步
- `disableAll()` + `remove()` 是安全移除 Channel 的标准两步流程（先停止监听，再从 Poller 注销）

---

## 十一、思考题

1. Channel 的 `fd_` 是 `const int`，为什么不允许修改？（提示：Poller 用 fd 做 key）
2. `enableWriting()` 之后为什么一定要在发送完成后 `disableWriting()`？不关掉会发生什么？（提示：epoll LT 模式）
3. `tie()` 使用 `weak_ptr<void>` 而不是 `weak_ptr<TcpConnection>`，有什么好处？
4. 如果 `handleEventSafely()` 里的 `readCallback_` 在执行过程中调用了 `channel->disableAll()` 和 `channel->remove()`，此时 Channel 对象会立刻析构吗？为什么？

---

## 十二、思考题参考答案

### 1. Channel 的 `fd_` 是 `const int`，为什么不允许修改？

**根本原因：fd 是 Channel 在 Poller 中的唯一标识，修改会导致映射关系断裂。**

看 Channel.h 第 302 行的声明：

```cpp
const int fd_;
```

以及 EpollPoller 中 `fd` 的使用方式：

1. **Debug 模式的 `channels_` map 以 fd 为 key**（EpollPoller.cc 第 147-151 行）：
```cpp
channels_[fd] = channel;  // fd → Channel* 的映射
```

2. **`epoll_ctl` 以 fd 为参数**（EpollPoller.cc 第 210 行）：
```cpp
::epoll_ctl(epollfd_, operation, fd, &event);
```

3. **KQueue 的 `channels_` 也以 fd 为 key**（KQueue.cc 第 183 行）：
```cpp
channels_[fd] = {events, channel};
```

4. **PollPoller 的 `pollfds_` 数组中存的是 fd**（PollPoller.cc 第 101-102 行）：
```cpp
pfd.fd = channel->fd();
```

如果允许修改 `fd_`，会导致以下灾难性问题：

- **映射不一致**：Channel 已经用 `fd=5` 注册到了 Poller 的 `channels_[5] = this`。如果修改 `fd_` 为 7，此后 `channel->fd()` 返回 7，但 Poller 还认为这个 Channel 对应 fd=5。执行 `update()` 时传给 `epoll_ctl` 的是 fd=7，但内核中注册的是 fd=5——两边不匹配。
- **`remove()` 找不到自己**：`remove` 通过 `channels_.erase(fd)` 删除映射，如果 fd 已变，就删不掉原来的映射，导致**内存泄漏**（map 中残留无效指针）。
- **fd 复用风险**：如果旧 fd 被 close 后被系统重新分配给新连接，Poller 还保存着旧 Channel 的映射 `channels_[旧fd]`，新连接的 Channel 注册时会冲突。

**正确做法**：如果需要关联到新的 fd，应该销毁旧 Channel 并创建新的。fd 和 Channel 是一对一绑定关系，整个生命周期不可变。这也是为什么 Channel 的构造函数直接将 fd 设为 `const`（Channel.cc 第 36-38 行）：

```cpp
Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0), index_(-1), tied_(false)
```

---

### 2. `enableWriting()` 之后为什么必须在发送完成后 `disableWriting()`？

**不关掉写事件会导致 epoll busy loop（空转），CPU 100%。**

trantor 使用的是 **LT（水平触发）** 模式——`epoll_event.events` 中没有设置 `EPOLLET` 标志（参见 EpollPoller.cc 第 207 行 `event.events = channel->events()`，只有 `POLLIN | POLLOUT`，无 `EPOLLET`）。

LT 模式的行为：**只要 fd 的条件满足，每次 `epoll_wait` 都会返回该事件**。

对于 `POLLOUT`（可写事件），"条件满足"意味着**内核发送缓冲区有空闲空间**。而 TCP 的发送缓冲区在大部分时间都是有空间的（默认 128KB~数 MB），只有在对端接收很慢、缓冲区堆满时才会"不可写"。

如果发送完数据后不 `disableWriting()`：

```
循环1: epoll_wait → POLLOUT 就绪 → writeCallback_() → 发现没有数据要发 → 返回
循环2: epoll_wait → 缓冲区仍有空间 → POLLOUT 就绪 → writeCallback_() → 没数据 → 返回
循环3: epoll_wait → 又返回 → ...
（无限循环，CPU 100%）
```

`epoll_wait` 的超时形同虚设——因为每次调用都会立即返回（有可写事件），主循环变成了不停地调用 `epoll_wait` → `handleEvent` → `doRunInLoopFuncs` → `epoll_wait` ...

**正确的写事件使用模式**：

```
① 有数据要发：先尝试直接 write
② write 返回 EAGAIN（缓冲区满）：把剩余数据放入用户态发送缓冲区，enableWriting()
③ POLLOUT 触发：从用户态缓冲区取数据继续 write
④ 所有数据发完：disableWriting() ← 关键！
```

这也是为什么 TcpConnection 的 `handleWrite()` 在缓冲区写空后一定会调用 `channel_->disableWriting()`。

---

### 3. `tie()` 使用 `weak_ptr<void>` 而不是 `weak_ptr<TcpConnection>` 的好处

**核心好处：类型擦除，Channel 不依赖 TcpConnection 的定义。**

看 Channel.h 第 269-273 行和第 312 行：

```cpp
void tie(const std::shared_ptr<void> &obj) {
    tie_ = obj;    // weak_ptr<void>
    tied_ = true;
}
// ...
std::weak_ptr<void> tie_;
```

如果改成 `weak_ptr<TcpConnection>`，会导致以下问题：

1. **循环依赖**：Channel.h 需要 `#include "TcpConnection.h"`（或至少前向声明 + 完整定义在 .cc 中）。而 TcpConnection 已经包含了 Channel（`unique_ptr<Channel>`），形成 `Channel.h ↔ TcpConnection.h` 的循环头文件依赖。虽然可以用前向声明缓解，但 `weak_ptr<T>` 的 `lock()` 需要完整类型定义。

2. **Channel 的通用性丧失**：Channel 不仅被 TcpConnection 使用，还被以下对象使用：
   - `EventLoop` 自己的 `wakeupChannelPtr_`（绑定到 EventLoop，不是 TcpConnection）
   - `TimerQueue` 内部的 timerfd Channel（Linux）
   - `Acceptor` 的监听 Channel

   这些持有者都不是 TcpConnection。如果 `tie` 只接受 `TcpConnection`，这些场景就无法使用 `tie` 机制。

3. **`weak_ptr<void>` 的转换是安全的**：C++ 标准保证 `shared_ptr<Derived>` 可以隐式转换为 `shared_ptr<void>`，且转换后 `weak_ptr<void>::lock()` 返回的 `shared_ptr<void>` 仍然持有正确的引用计数和正确的删除器。也就是说，类型信息只在析构时需要——而析构器（deleter）在 `shared_ptr` 创建时就被捕获了，与 `void` 转换无关。

4. **符合最小知识原则**：Channel 只关心"持有者是否还活着"，不关心持有者的具体类型。`handleEvent` 里只做 `tie_.lock()` 检查是否为空：

```cpp
std::shared_ptr<void> guard = tie_.lock();  // 只需知道 "活着" 还是 "死了"
if (guard) {
    handleEventSafely();  // guard 续命，保证回调期间不析构
}
```

这是一种经典的**类型擦除**设计模式，让底层组件（Channel）不依赖上层组件（TcpConnection）的具体类型，实现了依赖反转。

---

### 4. `readCallback_` 中调用 `disableAll()` + `remove()`，Channel 会立刻析构吗？

**不会立刻析构。原因有两层保护。**

**第一层保护：`tie()` 的 `guard`**

回顾 `handleEvent()` 的调用链（Channel.cc 第 53-69 行）：

```cpp
void Channel::handleEvent()
{
    if (events_ == kNoneEvent) return;
    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();  // 引用计数 +1
        if (guard) {
            handleEventSafely();  // ← readCallback_ 在这里被调用
        }
    }
    else {
        handleEventSafely();
    }
    // guard 在这里析构，引用计数 -1
}
```

`guard` 是一个局部 `shared_ptr<void>`，在 `handleEvent()` 整个函数作用域内持有 TcpConnection 的引用。即使 `readCallback_` 内部导致 TcpConnection 的其他所有 `shared_ptr` 被释放，`guard` 仍然保持引用计数 >= 1，TcpConnection 不会析构。

TcpConnection 不析构 → 它持有的 `unique_ptr<Channel>` 不析构 → Channel 不析构。

**第二层保护：`disableAll()` + `remove()` 只修改状态，不析构**

看这两个操作做了什么：

```cpp
// disableAll()（Channel.h 第 165-169 行）
void disableAll() {
    events_ = kNoneEvent;
    update();  // → epoll_ctl(EPOLL_CTL_DEL) 从内核移除
}

// remove()（Channel.cc 第 41-46 行）
void Channel::remove() {
    assert(events_ == kNoneEvent);
    addedToLoop_ = false;
    loop_->removeChannel(this);  // → 从 Poller 的 channels_ map 移除，setIndex(kNew)
}
```

这两步操作都只是修改了 Channel 的状态和 Poller 的内部映射，**不涉及 Channel 对象本身的 delete/析构**。Channel 的生命周期完全由持有它的 `unique_ptr`（在 TcpConnection 中）管理。

**完整时序分析**：

```
① EventLoop::loop() 遍历 activeChannels_
② channel->handleEvent()
③   guard = tie_.lock()       → 引用计数 N+1
④   handleEventSafely()
⑤     readCallback_() 执行
⑥       channel->disableAll()  → events_ = 0, epoll_ctl DEL（只改状态）
⑦       channel->remove()      → 从 Poller map 删除（只改映射）
⑧     readCallback_() 返回
⑨   guard 析构                → 引用计数回 N
⑩ 遍历下一个 Channel

Channel 在整个过程中始终存活。
直到后续 TcpConnection 真正析构时（所有 shared_ptr 归零），
Channel 才会随着 unique_ptr 析构而被 delete。
```

**如果没有 `tie()` 机制呢？** 如果 Channel 没有调用 `tie()`（`tied_ == false`），`handleEventSafely()` 直接执行，没有 `guard` 保护。此时如果 `readCallback_` 内部触发了 TcpConnection 的析构（比如这是最后一个 `shared_ptr`），Channel 确实会被析构——在自己的成员函数还没返回时！这就是经典的"回调中自杀"问题，会导致未定义行为。所以 `tie()` 机制不是可选的——对于 TcpConnection 拥有的 Channel，它是必须的安全保障。

---

*学习日期：2026-03-12 | 上一课：[第05课_EventLoop事件循环](第05课_EventLoop事件循环.md) | 下一课：[第07课_Poller多路复用](第07课_Poller多路复用.md)*
