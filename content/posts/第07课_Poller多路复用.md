+++
date = '2026-04-10'
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

---

## 八、思考题

1. `epoll_event.data.ptr` 存的是 `Channel *`，而不是 `fd`。如果一个 fd 被关闭后重新打开（fd 复用），新的 fd 可能获得相同的数字，这会导致什么问题？trantor 如何避免？
2. EpollPoller 的 `channels_` map 仅在 `#ifndef NDEBUG` 时存在，说明它只用于断言检查。Release 模式下没有这个 map，Poller 如何知道一个 fd 是否已经注册？（提示：Channel::index()）
3. KQueue 的 `update()` 只提交"有变化"的事件（对比 oldEvents），而 EpollPoller 每次都重新 `epoll_ctl`。哪种方式更好？
4. PollPoller 的时间复杂度是 O(n)（n = 监听的 fd 数），在 10000 个连接的服务器上，这意味着什么？这也是为什么 epoll 在高并发场景替代 poll 的根本原因。

---

*学习日期：2026-04-01 | 上一课：[第06课_Channel事件通道](第06课_Channel事件通道.md) | 下一课：[第08课_定时器系统](第08课_定时器系统.md)*

---

## 核心收获

- Poller 是策略模式：抽象接口屏蔽 epoll/kqueue/IOCP，EventLoop 不感知底层实现
- Linux `epoll_create1(EPOLL_CLOEXEC)`：原子设置 close-on-exec，防止 fork 后 fd 泄漏到子进程
- `EpollPoller` 用 `unordered_map<int, Channel*>` 保存 fd→Channel 映射，O(1) 查找
- Windows 通过 `wepoll`（封装 IOCP）提供兼容 epoll 的接口，上层代码零改动
- PollPoller 的 O(n) 遍历在万级连接下性能崩塌，这是 epoll 取代 poll 的根本原因
