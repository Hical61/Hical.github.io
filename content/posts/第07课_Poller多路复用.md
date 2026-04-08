+++
date = '2026-03-15'
draft = false
title = 'Poller — I/O 多路复用'
categories = ["网络编程"]
tags = ["C++", "trantor", "Poller", "IO多路复用", "学习笔记"]
description = "trantor Poller IO 多路复用封装解析，epoll/kqueue/select 的统一抽象层。"
+++


# 第 7 课：Poller — I/O 多路复用

> 对应源文件：
> - `trantor/net/inner/Poller.h` / `Poller.cc` — 抽象基类 + 工厂函数
> - `trantor/net/inner/poller/EpollPoller.h/.cc` — Linux/Windows 实现
> - `trantor/net/inner/poller/KQueue.h/.cc` — macOS/BSD 实现
> - `trantor/net/inner/poller/PollPoller.h/.cc` — 其他 Unix 兜底实现

---

## 一、Poller 在架构中的位置

```
EventLoop
    │ poll(timeoutMs, &activeChannels)
    ▼
  Poller（抽象基类）
    ├── EpollPoller  ← Linux / Windows(wepoll)
    ├── KQueue       ← macOS / FreeBSD / OpenBSD
    └── PollPoller   ← 其他 Unix（兜底）
```

Poller 是**桥接模式**的经典应用：上层 EventLoop 只依赖抽象基类 `Poller`，底层平台差异完全被屏蔽。EventLoop 的代码里看不到任何 `epoll_wait` 或 `kevent`。

---

## 二、抽象基类：三个纯虚方法

```cpp
// Poller.h 第 39-41 行
virtual void poll(int timeoutMs, ChannelList *activeChannels) = 0;
virtual void updateChannel(Channel *channel) = 0;
virtual void removeChannel(Channel *channel) = 0;
```

| 方法              | 职责                                                       |
| ----------------- | ---------------------------------------------------------- |
| `poll()`          | 阻塞等待 I/O 事件，把就绪的 Channel 放入 activeChannels    |
| `updateChannel()` | 新增/修改某个 fd 的监听事件（Channel.update() 的最终调用） |
| `removeChannel()` | 从 Poller 注销某个 fd                                      |

### 工厂函数（Poller.cc）

```cpp
Poller *Poller::newPoller(EventLoop *loop)
{
#if defined __linux__ || defined _WIN32
    return new EpollPoller(loop);      // Linux 和 Windows 都用 epoll API
#elif defined __FreeBSD__ || defined __OpenBSD__ || defined __APPLE__
    return new KQueue(loop);           // BSD 系和 macOS 用 kqueue
#else
    return new PollPoller(loop);       // 其他 Unix 用 poll（兜底）
#endif
}
```

---

## 三、EpollPoller：Linux 核心实现

### 3.1 epoll 三个系统调用

```
epoll_create1(EPOLL_CLOEXEC)  → 创建 epoll 实例，返回 epollfd
epoll_ctl(epollfd, op, fd, &event)  → 增/删/改 fd 的监听
epoll_wait(epollfd, events, maxevents, timeout)  → 阻塞等待就绪事件
```

`EPOLL_CLOEXEC`：`fork` 后子进程自动关闭 epollfd，防止资源泄漏。

### 3.2 数据结构

```cpp
int epollfd_;                    // epoll 实例 fd
EventList events_;               // vector<epoll_event>，存放 epoll_wait 返回的事件
// 初始大小 16，满了自动 2 倍扩容

// Debug 模式下额外维护
ChannelMap channels_;            // map<fd, Channel*>，用于 assert 校验
// Release 模式编译掉，不占运行时开销
```

### 3.3 `poll()` — 等待事件

```cpp
// EpollPoller.cc 第 78-108 行
void EpollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    int numEvents = ::epoll_wait(
        epollfd_,
        &*events_.begin(),          // 输出缓冲区
        static_cast<int>(events_.size()),  // 最多返回多少个事件
        timeoutMs                   // 超时（毫秒），-1=永久，0=立即返回
    );

    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);

        // 自动扩容：如果返回的事件数恰好等于 events_ 大小，
        // 说明可能还有事件没返回，翻倍扩容
        if (static_cast<size_t>(numEvents) == events_.size())
            events_.resize(events_.size() * 2);
    }
    else if (numEvents < 0 && savedErrno != EINTR) {
        LOG_SYSERR << "EPollEpollPoller::poll()";  // EINTR 是信号打断，正常忽略
    }
}
```

**扩容时机**很精妙：如果返回的事件数 `== events_.size()`，说明 epoll 可能因为缓冲区满而截断了结果，下次调用提前把空间翻倍，减少"截断再调用"的次数。

### 3.4 `fillActiveChannels()` — 从 epoll_event 提取 Channel

```cpp
// EpollPoller.cc 第 110-133 行
void EpollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    for (int i = 0; i < numEvents; ++i) {
        // 关键：epoll_event.data.ptr 存的是 Channel 指针！
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        channel->setRevents(events_[i].events);  // 填写实际发生的事件
        activeChannels->push_back(channel);
    }
}
```

**核心技巧**：`epoll_event.data` 是一个 union：
```c
union epoll_data {
    void        *ptr;   // ← trantor 用这个，存 Channel 指针
    int          fd;
    uint32_t     u32;
    uint64_t     u64;
};
```

`data.ptr = channel` 意味着 epoll 返回事件时，**直接携带 Channel 指针**，不需要额外的 `fd → Channel` 查表操作，O(1) 直取，非常高效。

### 3.5 `updateChannel()` — 状态机驱动 epoll_ctl

```cpp
// EpollPoller.cc 第 135-181 行
void EpollPoller::updateChannel(Channel *channel)
{
    const int index = channel->index();  // kNew=-1 / kAdded=1 / kDeleted=2

    if (index == kNew || index == kDeleted) {
        // kNew：首次添加 → EPOLL_CTL_ADD
        // kDeleted：曾经删除，重新启用 → 也用 EPOLL_CTL_ADD
        channel->setIndex(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else {  // index == kAdded
        if (channel->isNoneEvent()) {
            // 当前无事件监听 → 从 epoll 删除（但保留在 channels_ map 中）
            update(EPOLL_CTL_DEL, channel);
            channel->setIndex(kDeleted);
        }
        else {
            // 修改已有的监听事件
            update(EPOLL_CTL_MOD, channel);
        }
    }
}
```

**状态机图**：

```
                    enableReading()
    [kNew=-1] ─────────────────────► [kAdded=1]
                    EPOLL_CTL_ADD          │
                                           │ disableAll()
                                           │ EPOLL_CTL_DEL
                                           ▼
                    enableReading()   [kDeleted=2]
    [kAdded=1] ◄─────────────────────
                    EPOLL_CTL_ADD

    [kAdded=1]  ── enableWriting() ──► EPOLL_CTL_MOD（还是 kAdded）

    remove() ─────────────────────────► 从 channels_ map 删除，setIndex(kNew)
```

### 3.6 `update()` — 最终调用 epoll_ctl

```cpp
// EpollPoller.cc 第 203-222 行
void EpollPoller::update(int operation, Channel *channel)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events   = channel->events();   // 感兴趣的事件掩码
    event.data.ptr = channel;             // 把 Channel 指针存进去

    int fd = channel->fd();
    ::epoll_ctl(epollfd_, operation, fd, &event);
    // operation: EPOLL_CTL_ADD / EPOLL_CTL_MOD / EPOLL_CTL_DEL
}
```

**epoll LT vs ET**：

trantor 使用的是**水平触发（LT，Level Triggered）**——`event.events` 中没有设置 `EPOLLET` 标志。

| 模式           | 行为                                      | 适用场景                                  |
| -------------- | ----------------------------------------- | ----------------------------------------- |
| LT（水平触发） | 只要 fd 可读/可写，每次 epoll_wait 都返回 | 简单安全，不怕漏事件                      |
| ET（边缘触发） | 只在状态**变化**时返回一次                | 性能更高，但必须一次读完/写完，否则漏事件 |

LT 模式更安全（不会因为漏读导致数据丢失），代价是如果不及时消费事件会反复返回。trantor 通过"用完即 `disableWriting`"来避免写事件的 busy loop。

---

## 四、KQueue：macOS/BSD 实现

### 4.1 kqueue 与 epoll 的核心差异

| 特性     | epoll                           | kqueue                         |
| -------- | ------------------------------- | ------------------------------ |
| 系统调用 | `epoll_create/ctl/wait` (3个)   | `kqueue()` + `kevent()` (2个)  |
| 读写事件 | 一个 `epoll_event` 可同时含读写 | **读写是分开的两个 filter**    |
| 修改接口 | `EPOLL_CTL_ADD/MOD/DEL`         | 同一个 `kevent()` 即查询又修改 |

### 4.2 读写分离的 kevent

```cpp
// KQueue.cc 第 171-228 行
void KQueue::update(Channel *channel)
{
    struct kevent ev[2];  // 最多 2 个：读 + 写
    int n = 0;

    // 只处理"有变化"的部分（与 oldEvents 比较）
    if (新增读 && 之前没读) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, ...);
    }
    else if (取消读 && 之前有读) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, ...);
    }
    if (新增写 && 之前没写) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, ...);
    }
    else if (取消写 && 之前有写) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, ...);
    }

    kevent(kqfd_, ev, n, NULL, 0, NULL);  // 批量提交变更
}
```

**n 为 0 时不调用 kevent**——只有真正发生变化才提交系统调用，减少无效开销。

### 4.3 fillActiveChannels：事件转换为统一标志

kqueue 返回的 `filter` 是 `EVFILT_READ` / `EVFILT_WRITE`，需要转换为 trantor 统一使用的 `POLLIN` / `POLLOUT`：

```cpp
int events = events_[i].filter;
if (events == EVFILT_READ)       channel->setRevents(POLLIN);
else if (events == EVFILT_WRITE) channel->setRevents(POLLOUT);
```

这样 Channel 的 `handleEventSafely()` 代码完全不需要知道底层是 epoll 还是 kqueue，统一用 `POLLIN/POLLOUT` 判断。

### 4.4 `resetAfterFork()`

```cpp
void KQueue::resetAfterFork()
{
    close(kqfd_);
    kqfd_ = kqueue();  // fork 后子进程的 kqueue fd 失效，重新创建
    for (auto &ch : channels_) {
        // 重新注册所有 Channel
        if (ch.second.second->isReading() || ch.second.second->isWriting())
            update(ch.second.second);
    }
}
```

`fork()` 后子进程虽然继承了父进程的 fd，但 kqueue 内核对象是进程私有的，子进程的 kqueue 不会继承父进程注册的事件，必须重建。EpollPoller 有类似的 `resetAfterFork`（epoll 同理）。

---

## 五、平台选择总结

```
Poller::newPoller()
    │
    ├─ Linux ──────────────────────► EpollPoller（epoll，最优）
    ├─ Windows ────────────────────► EpollPoller（wepoll 模拟 epoll API）
    ├─ macOS / FreeBSD / OpenBSD ──► KQueue（kqueue，与 epoll 性能相当）
    └─ 其他 Unix ──────────────────► PollPoller（poll，兜底，O(n) 复杂度）
```

**wepoll**（Windows）：是一个第三方库，把 Windows IOCP 封装成 epoll API，让 EpollPoller 代码在 Windows 上也能直接使用，最大化代码复用。

---

## 六、三个实现横向对比

| 特性             | EpollPoller   | KQueue           | PollPoller      |
| ---------------- | ------------- | ---------------- | --------------- |
| 平台             | Linux/Windows | macOS/BSD        | 其他 Unix       |
| 内核调用         | `epoll_wait`  | `kevent`         | `poll`          |
| 复杂度           | O(就绪事件数) | O(就绪事件数)    | O(所有监听fd数) |
| Channel 指针传递 | `data.ptr`    | `udata`          | 遍历查找        |
| 读写事件         | 同一个结构体  | 分开的 filter    | 同一个结构体    |
| `fork` 后需重置  | 是            | 是（且需重注册） | 否              |
| 最大 fd 数       | 无硬性限制    | 无硬性限制       | `RLIMIT_NOFILE` |

---

## 七、完整调用链（以 Linux 为例）

```
[用户代码]
  conn = server.accept()
  channel->enableReading()
        │
        ▼
[Channel::update()]
  → loop_->updateChannel(this)
        │
        ▼
[EventLoop::updateChannel(channel)]
  → poller_->updateChannel(channel)
        │
        ▼
[EpollPoller::updateChannel(channel)]
  → update(EPOLL_CTL_ADD, channel)
        │
        ▼
[EpollPoller::update()]
  event.data.ptr = channel   ← 存指针！
  epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &event)
        │
   （内核记录：fd 有事件时，返回携带 channel 指针的 epoll_event）

[数据到达]
        │
        ▼
[EventLoop::loop()]
  poller_->poll(10000, &activeChannels)
        │
        ▼
[EpollPoller::poll()]
  epoll_wait(epollfd_, events_, size, 10000)  ← 阻塞，直到事件到来
        │
        ▼
[EpollPoller::fillActiveChannels()]
  channel = events_[i].data.ptr    ← 直接取指针，O(1)
  channel->setRevents(events)
  activeChannels->push_back(channel)
        │
        ▼
[EventLoop::loop() 继续]
  for channel in activeChannels:
      channel->handleEvent()       ← 分发到 readCallback_ 等
```

## 核心收获

- Poller 是策略模式：抽象接口屏蔽 epoll/kqueue/IOCP，EventLoop 不感知底层实现
- Linux `epoll_create1(EPOLL_CLOEXEC)`：原子设置 close-on-exec，防止 fork 后 fd 泄漏到子进程
- `EpollPoller` 用 `unordered_map<int, Channel*>` 保存 fd→Channel 映射，O(1) 查找
- Windows 通过 `wepoll`（封装 IOCP）提供兼容 epoll 的接口，上层代码零改动
- PollPoller 的 O(n) 遍历在万级连接下性能崩塌，这是 epoll 取代 poll 的根本原因

---

## 八、思考题

1. `epoll_event.data.ptr` 存的是 `Channel *`，而不是 `fd`。如果一个 fd 被关闭后重新打开（fd 复用），新的 fd 可能获得相同的数字，这会导致什么问题？trantor 如何避免？
2. EpollPoller 的 `channels_` map 仅在 `#ifndef NDEBUG` 时存在，说明它只用于断言检查。Release 模式下没有这个 map，Poller 如何知道一个 fd 是否已经注册？（提示：Channel::index()）
3. KQueue 的 `update()` 只提交"有变化"的事件（对比 oldEvents），而 EpollPoller 每次都重新 `epoll_ctl`。哪种方式更好？
4. PollPoller 的时间复杂度是 O(n)（n = 监听的 fd 数），在 10000 个连接的服务器上，这意味着什么？这也是为什么 epoll 在高并发场景替代 poll 的根本原因。

---

## 九、思考题参考答案

### 1. fd 被关闭后重新打开（fd 复用），`epoll_event.data.ptr` 会导致什么问题？

**问题：新 fd 的 epoll 事件可能携带旧 Channel 的指针，导致悬空指针访问。**

**fd 复用的背景**：Linux 内核分配 fd 采用"最小可用编号"策略。当 fd=5 被 `close()` 后，下一次 `accept()/socket()` 可能再次返回 fd=5。

**具体危险场景**：

```
时刻 T1: 连接 A 使用 fd=5，Channel_A 注册到 epoll，data.ptr = Channel_A
时刻 T2: 连接 A 关闭 → close(5)
         但如果没有先 epoll_ctl(DEL, 5)，内核 epoll 表中还残留 fd=5 的注册
时刻 T3: 新连接 B accept() 得到 fd=5（内核复用了同一个编号）
时刻 T4: epoll_wait 返回 fd=5 的事件，data.ptr 仍然是 Channel_A（已析构！）
         → 访问悬空指针 → 崩溃
```

**trantor 的防护措施**：

trantor 通过严格的**关闭顺序**确保 fd 复用不会出问题：

1. **先从 epoll 注销，再关闭 fd**：TcpConnection 关闭时的流程是：
   - `channel_->disableAll()` → `epoll_ctl(EPOLL_CTL_DEL, fd)` — 从内核 epoll 中删除
   - `channel_->remove()` → 从 Poller 的 map 中删除
   - 然后才 `::close(fd)`

   这样内核 epoll 表中不再有 fd=5 的注册，即使 fd 被复用，旧的 `data.ptr` 不可能再被返回。

2. **Channel 的 `index_` 状态机**：Channel 有 `kNew → kAdded → kDeleted → kNew` 的状态转移（EpollPoller.cc 第 48-51 行）。`removeChannel` 后 `index_` 被设为 `kNew`（第 202 行），如果任何代码意外地对已移除的 Channel 调用 `update()`，状态机会走 `kNew` 分支执行 `EPOLL_CTL_ADD`，而不会错误地 `MOD` 一个不存在的注册。

3. **`close()` 自动移除 epoll 注册**：实际上 Linux 内核有一条规则——当一个 fd 被 `close()` 后，如果该 fd 对应的**文件描述（file description）** 的引用计数降为 0，内核会**自动从 epoll 中删除**该注册。但这只在"没有 `dup`/`fork` 等导致引用计数 > 1"时才可靠。trantor 不依赖这个隐式行为，而是**显式调用 `epoll_ctl DEL`**，更安全。

4. **单线程模型保证时序**：所有 Channel 操作（注册、注销、关闭 fd）都在同一个 EventLoop 线程中串行执行。不存在"T2 和 T3 并发"的情况——close 和 accept 要么在同一线程顺序发生，要么通过 `runInLoop` 序列化。

---

### 2. Release 模式下没有 `channels_` map，Poller 如何知道一个 fd 是否已注册？

**答案：不需要知道！通过 `Channel::index_` 完全替代了 map 查询。**

看 EpollPoller 中 `#ifndef NDEBUG` 的使用（EpollPoller.cc 第 124-129 行、第 146-158 行、第 186-193 行）：

```cpp
#ifndef NDEBUG
    int fd = channel->fd();
    ChannelMap::const_iterator it = channels_.find(fd);
    assert(it != channels_.end());
    assert(it->second == channel);
#endif
```

`channels_` map 在 Release 模式下完全被编译掉（`NDEBUG` 宏定义时 `#ifndef NDEBUG` 区块不参与编译）。所有对 `channels_` 的操作都在 `#ifndef NDEBUG` 内部，**仅用于断言校验**。

那么核心逻辑如何判断 fd 的注册状态？答案在 `updateChannel`（EpollPoller.cc 第 135-182 行）：

```cpp
void EpollPoller::updateChannel(Channel *channel)
{
    const int index = channel->index();  // ← 关键！用 Channel 自己记录的状态

    if (index == kNew || index == kDeleted) {
        // 需要 EPOLL_CTL_ADD
        channel->setIndex(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else {  // index == kAdded
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->setIndex(kDeleted);
        }
        else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}
```

**`Channel::index_` 的三个状态完全编码了 fd 在 Poller 中的状态**：

| `index_` 值    | 含义             | epoll_ctl 操作      |
| -------------- | ---------------- | ------------------- |
| `kNew = -1`    | 从未注册到 epoll | 需要 `ADD`          |
| `kAdded = 1`   | 已在 epoll 中    | 可以 `MOD` 或 `DEL` |
| `kDeleted = 2` | 曾注册但已 DEL   | 重新 `ADD`          |

这是一种**状态存储在对象自身**的设计。每个 Channel 自己"记住"它在 Poller 中的状态，而不是由 Poller 维护一个外部 map 来查询。这样做：

- Release 模式零额外内存开销（不需要 `unordered_map`）
- 判断状态是 O(1)（直接读 `channel->index()`），而不是 O(1) 平均 / O(n) 最差的哈希查找
- `epoll_ctl` 只关心操作类型（ADD/MOD/DEL），不关心"当前注册了多少 fd"

Debug 模式的 `channels_` map 存在的意义纯粹是**双重校验**：验证 `index_` 状态和 map 状态的一致性，帮助开发者在调试时尽早发现 bug。

---

### 3. KQueue 只提交有变化的事件 vs EpollPoller 每次都 `epoll_ctl`，哪种更好？

**两种方案各有优劣，KQueue 的差异化提交在理论上更优，但 EpollPoller 的方案在实际中足够好。**

**KQueue 的做法**（KQueue.cc 第 171-228 行）：

```cpp
void KQueue::update(Channel *channel)
{
    auto events = channel->events();
    int oldEvents = channels_[fd].first;  // 记录旧事件

    // 只提交变化的部分
    if ((events & kReadEvent) && !(oldEvents & kReadEvent))
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, ...);
    else if (!(events & kReadEvent) && (oldEvents & kReadEvent))
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, ...);
    // 写事件同理...

    if (n > 0) kevent(kqfd_, ev, n, NULL, 0, NULL);  // 有变化才调系统调用
}
```

优点：
- **避免无效系统调用**：如果 `enableWriting()` 后又 `enableWriting()`（事件没变），`n == 0`，不调用 `kevent`。系统调用是昂贵的（用户态 → 内核态切换）。
- **批量提交**：一次 `kevent` 调用可以同时添加读和写两个 filter，而不是分两次调用。

缺点：
- **额外维护 `oldEvents`**：需要在 `channels_` map 中同时存储 `{oldEvents, Channel*}`（KQueue.cc 第 183 行 `channels_[fd] = {events, channel}`），增加了内存和维护成本。
- **逻辑更复杂**：4 个 if-else 分支判断读写的增/减变化，容易出错。

**EpollPoller 的做法**（EpollPoller.cc 第 203-222 行）：

```cpp
void EpollPoller::update(int operation, Channel *channel)
{
    struct epoll_event event;
    event.events = channel->events();   // 直接用最新事件
    event.data.ptr = channel;
    ::epoll_ctl(epollfd_, operation, fd, &event);  // 无条件调用
}
```

优点：
- **实现极简**：不需要记录旧事件，不需要做差异计算。
- **绝对正确**：每次都把最新状态同步到内核，不可能出现旧状态残留的 bug。

缺点：
- **可能做无效系统调用**：如果事件实际没变（例如连续两次 `enableWriting()`），仍然会调用 `epoll_ctl(MOD)`。但实际上 trantor 的代码路径不太会出现这种情况——`enableWriting()` 是幂等的位操作 `events_ |= kWriteEvent`，如果已经有 `kWriteEvent`，`events_` 值不变，但 `update()` 仍会被调用。

**结论**：

在实际中 EpollPoller 的方案更好，原因如下：

1. **事件变更频率低**：一个连接的事件变更主要发生在建立（enableReading）、发送（enableWriting/disableWriting）和关闭（disableAll）时。正常运行中 99% 的时间都在 `epoll_wait`，`epoll_ctl` 的调用次数远小于 `epoll_wait`，少一两次系统调用的优化意义不大。

2. **kqueue 必须分离读写 filter**：kqueue 的读和写是两个独立的 `kevent`，不能像 epoll 那样用一个 `epoll_event.events = POLLIN | POLLOUT` 一次搞定。所以 kqueue 本身就需要差异化计算来决定添加/删除哪个 filter，这不是"优化"而是"必须"。

3. **简单性 > 微优化**：EpollPoller 的简单实现意味着更少的 bug 可能性，对于网络框架这种基础设施来说，正确性永远优先于微小的性能提升。

---

### 4. PollPoller 的 O(n) 复杂度在 10000 个连接下意味着什么？

**在 10000 个连接的服务器上，PollPoller 会导致严重的性能退化，这是 epoll 取代 poll 的根本原因。**

**`poll()` 系统调用的工作原理**：

```c
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
```

内核需要做以下操作：
1. **拷贝**：把用户态的 `pollfd` 数组（`nfds` 个元素）拷贝到内核态
2. **遍历**：内核遍历所有 `nfds` 个 fd，检查每个 fd 的状态
3. **拷贝回**：把结果（`revents`）拷贝回用户态

每次 `poll()` 调用的时间复杂度都是 **O(n)**，其中 n = 监听的 fd 总数。

**10000 连接的量化分析**：

假设 10000 个连接中，任一时刻只有 10 个是活跃的（有数据到达），这在实际场景中很常见（大量空闲连接 + 少量活跃连接）。

| 指标                      | `poll()`                                            | `epoll_wait`                                    |
| ------------------------- | --------------------------------------------------- | ----------------------------------------------- |
| 每次系统调用遍历          | 10000 个 fd                                         | 仅返回 10 个活跃 fd                             |
| 用户态→内核态数据拷贝     | `10000 * sizeof(pollfd)` = 80KB                     | 0（内核直接维护 interest list）                 |
| `fillActiveChannels` 遍历 | PollPoller.cc 第 65-86 行：遍历整个 `pollfds_` 数组 | EpollPoller.cc 第 114 行：只遍历 `numEvents` 个 |
| 每秒可处理的事件轮数      | 受限于遍历开销                                      | 几乎只取决于活跃事件数                          |

PollPoller 源码中的 `fillActiveChannels`（PollPoller.cc 第 65-86 行）：

```cpp
void PollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    int processedEvents = 0;
    for (auto pfd : pollfds_)  // ← 遍历全部 10000 个！
    {
        if (pfd.revents > 0) {
            auto ch = channels_.find(pfd.fd);  // 还要查 map
            // ...
            processedEvents++;
            if (processedEvents == numEvents)
                break;  // 找到所有活跃的才停
        }
    }
}
```

即使只有 10 个事件就绪，也必须遍历前面 9990 个不活跃的 fd。

**实际影响**：

1. **延迟增加**：假设遍历 10000 个 fd 需要 50us（保守估计），每秒循环 1000 次，光遍历就消耗 50ms 的 CPU 时间。而 epoll 只遍历 10 个活跃 fd，耗时可以忽略。

2. **吞吐量下降**：EventLoop 主循环的每一轮都变慢了，意味着单位时间内能处理的事件数减少。在高并发场景下，新事件不能及时处理，导致队列堆积、延迟飙升。

3. **CPU 利用率恶化**：大量 CPU 时间浪费在检查空闲 fd 上。这些 fd 大部分时间没有事件，但 `poll` 每次都要检查它们——纯粹的 CPU 浪费。

4. **不线性扩展**：连接数从 10000 涨到 100000 时，`poll` 的开销线性增长 10 倍，而 epoll 几乎不变（只要活跃连接数不变）。

这也是 PollPoller 源码中有警告日志的原因（PollPoller.cc 第 31-34 行）：

```cpp
std::call_once(warning_flag, []() {
    LOG_WARN << "Creating a PollPoller. This poller is slow and should "
                "only be used when no other pollers are available";
});
```

**对比总结**：

```
poll:   每次调用 O(总连接数)，不管活跃与否都要遍历
epoll:  每次调用 O(活跃连接数)，空闲连接零开销

10000 连接、10 个活跃：
  poll:  遍历 10000 次 → 1000x 浪费
  epoll: 遍历 10 次    → 精确命中
```

这就是为什么 epoll（和 kqueue）是高并发服务器的标配，而 `poll`/`select` 只适合小规模（几百个连接以内）或作为兜底方案。

---

*学习日期：2026-03-15 | 上一课：[第06课_Channel事件通道]({{< relref "第06课_Channel事件通道.md" >}}) | 下一课：[第08课_定时器系统]({{< relref "第08课_定时器系统.md" >}})*
