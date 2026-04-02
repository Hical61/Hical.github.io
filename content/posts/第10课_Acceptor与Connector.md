+++
date = '2026-04-18'
draft = false
title = 'Acceptor & Connector — 连接的两端'
categories = ["网络编程"]
tags = ["C++", "trantor", "Acceptor", "Connector", "学习笔记"]
description = "trantor Acceptor 与 Connector 解析，服务端监听接入与客户端主动连接的两端实现。"
+++


# 第 10 课：Acceptor & Connector — 连接的两端

> 对应源文件：
> - `trantor/net/inner/Acceptor.h` / `Acceptor.cc` — 服务端：监听并接受连接
> - `trantor/net/inner/Connector.h` / `Connector.cc` — 客户端：主动发起连接（含重连）

---

## 一、两个类在架构中的角色

```
                  ┌──────────────────────────┐
                  │        TcpServer         │
                  │  ┌─────────────────────┐ │
                  │  │      Acceptor       │ │
                  │  │  Socket(listenFd)   │ │
                  │  │  Channel            │ │
                  │  └─────────────────────┘ │
                  └──────────────────────────┘
                           ↑ listen
                    客户端发起 connect
                           ↓ accept → 回调 newConnectionCallback_
                  ┌──────────────────────────┐
                  │        TcpClient         │
                  │  ┌─────────────────────┐ │
                  │  │      Connector      │ │
                  │  │  (非阻塞 connect)   │ │
                  │  │  指数退避重连        │ │
                  │  └─────────────────────┘ │
                  └──────────────────────────┘
```

`Acceptor` 和 `Connector` 都**不拥有连接**——它们的职责是把一个**已就绪的 fd** 交给上层（`TcpServer`/`TcpClient`），由上层创建 `TcpConnection` 对象来管理该 fd 的后续生命周期。

---

## 二、Acceptor — 服务端连接接收器

### 2.1 构造：一次性完成 socket 全套配置

```cpp
Acceptor::Acceptor(EventLoop *loop,
                   const InetAddress &addr,
                   bool reUseAddr = true,
                   bool reUsePort = true)
```

构造函数做了五件事：

```
① idleFd_ = open("/dev/null", O_RDONLY|O_CLOEXEC)   ← 备用 fd（见 EMFILE 处理）
② sock_ = Socket(createNonblockingSocketOrDie(...))   ← 创建非阻塞 listen socket
③ sock_.setReuseAddr(reUseAddr)                       ← 允许端口快速复用
   sock_.setReusePort(reUsePort)
④ sock_.bindAddress(addr_)                            ← 绑定地址
⑤ acceptChannel_(loop, sock_.fd())                   ← 把 listenFd 包装成 Channel
   acceptChannel_.setReadCallback(readCallback)        ← 注册读回调
```

**注意**：构造时**不** `listen()`，必须单独调用 `listen()` 方法，原因见下文。

#### 端口 0 的特殊处理

```cpp
if (addr_.toPort() == 0) {
    addr_ = InetAddress{Socket::getLocalAddr(sock_.fd())};
}
```

传入端口 0 时操作系统会自动分配可用端口，构造后立刻查询真实分配到的端口，存回 `addr_`。这样上层可以调用 `acceptor.addr().toPort()` 得到真实端口号（常用于测试）。

### 2.2 `listen()` — 启动监听

```cpp
void Acceptor::listen()
{
    loop_->assertInLoopThread();            // 必须在 EventLoop 线程调用
    if (beforeListenSetSockOptCallback_)
        beforeListenSetSockOptCallback_(sock_.fd());  // ① 监听前的 socket 选项钩子
    sock_.listen();                          // ② 系统调用 listen(fd, SOMAXCONN)
    acceptChannel_.enableReading();          // ③ 注册到 epoll，开始接受连接事件
}
```

**为什么 listen 和构造分开？**

构造时可能需要先配置回调（`setNewConnectionCallback`），或在多线程场景下先完成对象初始化再激活监听。分离构造与激活是一种常见的"惰性初始化"模式。

`beforeListenSetSockOptCallback_` 是一个钩子，允许用户在 `listen()` 之前对 socket 做自定义配置（例如设置 `TCP_DEFER_ACCEPT`），不需要修改框架代码。

### 2.3 `readCallback()` — 核心 accept 逻辑

当 `listenFd` 上有事件（新连接到达），`Channel` 调用 `readCallback()`：

```cpp
void Acceptor::readCallback()
{
    InetAddress peer;
    int newsock = sock_.accept(&peer);      // accept4() on Linux

    if (newsock >= 0) {
        if (afterAcceptSetSockOptCallback_)
            afterAcceptSetSockOptCallback_(newsock);  // ① accept 后 socket 选项钩子
        if (newConnectionCallback_)
            newConnectionCallback_(newsock, peer);    // ② 把 fd 和对端地址交给上层
        else {
            close(newsock);  // 没有注册回调 → 直接关掉（防止 fd 泄漏）
        }
    }
    else {
        // accept 失败的特殊处理（见下文 EMFILE 技巧）
        if (errno == EMFILE) { ... }
    }
}
```

### 2.4 EMFILE 问题与 idleFd_ 技巧

**EMFILE**：`Too many open files`——进程的 fd 数量已到达上限，`accept()` 无法为新连接分配 fd。

**问题**：即使 accept 失败，`listenFd` 上仍然有事件（新连接在 listen 队列中），epoll 会持续触发，形成**空转 busy loop**，CPU 飙升到 100%。

**trantor 的解决方案**：

```cpp
// 构造时预先占一个 fd
idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

// EMFILE 时：
::close(idleFd_);                   // ① 释放备用 fd，让系统有一个空闲 fd
idleFd_ = sock_.accept(&peer);      // ② 用这个空闲 fd accept，把连接从队列里取出
::close(idleFd_);                   // ③ 立刻关闭（优雅拒绝）
idleFd_ = ::open("/dev/null", ...); // ④ 重新占住备用 fd
```

**精妙之处**：
- 不是忽略错误（那会导致 busy loop）
- 而是**礼貌地拒绝**：accept 后立刻 close，让客户端收到 RST，而不是被挂起
- `idleFd_` 始终是一个打开的 `/dev/null` fd，作为"保险箱"，在 fd 枯竭时提供一次应急额度

**为什么 Windows 不需要 idleFd_？**

Windows 使用句柄而非 Unix fd，EMFILE 语义不同，wepoll 也有不同的处理机制，所以用 `#ifndef _WIN32` 把 `idleFd_` 编译掉。

### 2.5 完整时序图

```
[TcpServer::start()]
        │
        ▼
acceptor_.listen()
  → sock_.listen()
  → acceptChannel_.enableReading()
  → epoll_ctl(ADD, listenFd, EPOLLIN)
        │
   [客户端 connect()]
        │
        ▼
epoll_wait() 返回 listenFd 可读
        │
        ▼
acceptChannel_.handleEvent()
  → Acceptor::readCallback()
  → sock_.accept(&peer)           ← accept4(NONBLOCK|CLOEXEC) on Linux
  → afterAcceptSetSockOptCallback_(newsock)  ← 可设 TCP_NODELAY 等
  → newConnectionCallback_(newsock, peer)
        │
        ▼
TcpServer::newConnection(newsock, peer)
  → 创建 TcpConnectionImpl
  → 分配到某个 I/O EventLoop
  → 连接开始运行
```

---

## 三、Connector — 客户端主动连接器

### 3.1 核心难题：非阻塞 connect

普通阻塞 `connect()` 会一直等到连接建立或超时。非阻塞模式下，`connect()` 会立即返回，需要通过 epoll 的**写事件**来感知连接结果：

```
非阻塞 connect() 返回 EINPROGRESS
          │
          ▼
注册 EPOLLOUT（写事件）到 epoll
          │
   等待 epoll_wait 返回
          │
          ▼
写事件就绪（无论成功还是失败都触发）
          │
          ▼
getsockopt(SO_ERROR) 判断实际结果
     ├─ err == 0 → 连接成功
     └─ err != 0 → 连接失败
```

**为什么用写事件而不是读事件？**

`connect()` 完成（无论成功失败）后，socket 进入"可写"状态。读事件在连接成功且对端发数据时才触发，不能用来检测 connect 结果。

### 3.2 状态机

```cpp
enum class Status {
    Disconnected,   // 初始状态 / 重试中 / 已停止
    Connecting,     // connect() 已调用，等待写事件
    Connected       // 连接成功，fd 已交给上层
};
```

```
[Disconnected]
     │ start()
     ▼
  connect() → EINPROGRESS
     │ connecting(fd)
     ▼
[Connecting]
     │ epoll 写事件触发
     ▼
  handleWrite()
     ├─ SO_ERROR == 0 && !isSelfConnect → [Connected]
     │                                    newConnectionCallback_(fd)
     ├─ SO_ERROR != 0 && retry_         → retry() → [Disconnected]
     │                                    runAfter(delay, startInLoop)
     └─ 已 stop()                        → [Disconnected]
                                           close(fd)
```

### 3.3 `connect()` — errno 分类处理

```cpp
void Connector::connect()
{
    fd_ = Socket::createNonblockingSocketOrDie(serverAddr_.family());
    if (sockOptCallback_) sockOptCallback_(fd_);   // 连接前 socket 选项钩子
    errno = 0;
    int ret = Socket::connect(fd_, serverAddr_);
    int savedErrno = (ret == 0) ? 0 : errno;

    switch (savedErrno)
    {
        // ── 正常的"连接进行中"状态 ──
        case 0:           // 极少见：本机连本机瞬间成功
        case EINPROGRESS: // 标准情况：连接正在进行，等写事件
        case EINTR:       // 被信号打断，重新等
        case EISCONN:     // 已经连上了（重入安全）
            connecting(fd_);     // → 注册写事件，等结果
            break;

        // ── 可重试的暂时性错误 ──
        case EAGAIN:        // 本地端口不足
        case EADDRINUSE:    // 目标端口占用
        case EADDRNOTAVAIL: // 地址不可用
        case ECONNREFUSED:  // 对端拒绝（RST）
        case ENETUNREACH:   // 网络不可达
            if (retry_) retry(fd_);
            break;

        // ── 不可恢复的致命错误 ──
        case EACCES:      // 权限不足（如连接特权端口）
        case EPERM:
        case EAFNOSUPPORT:// 地址族不支持
        case EALREADY:    // 已有连接请求未完成
        case EBADF:       // fd 无效
        case EFAULT:      // 地址指针无效
        case ENOTSOCK:    // fd 不是 socket
            close(fd_);
            errorCallback_();  // 通知上层，不重试
            break;
    }
}
```

**错误分类的意义**：
- 可重试错误（ECONNREFUSED 等）：对端可能暂时不在线，等一会儿重试有意义
- 致命错误（EBADF 等）：程序逻辑问题，重试没有意义，直接报错

### 3.4 `handleWrite()` — 连接结果检测

```cpp
void Connector::handleWrite()
{
    if (status_ == Status::Connecting) {
        int sockfd = removeAndResetChannel();     // 取消写事件监听
        int err = Socket::getSocketError(sockfd); // getsockopt(SO_ERROR)

        if (err) {
            // 连接失败：可重试就重试，否则关闭并报错
            if (retry_) retry(sockfd);
            else { close(sockfd); }
            errorCallback_();
        }
        else if (Socket::isSelfConnect(sockfd)) {
            // 自连接检测（见第 9 课）
            if (retry_) retry(sockfd);
            errorCallback_();
        }
        else {
            // 连接成功！
            status_ = Status::Connected;
            if (connect_) {
                newConnectionCallback_(sockfd);  // 把 fd 交给上层
            }
            // 如果已经 stop()，close(sockfd)
        }
    }
}
```

**`removeAndResetChannel()` 的延迟析构**：

```cpp
int Connector::removeAndResetChannel()
{
    channelPtr_->disableAll();
    channelPtr_->remove();
    int sockfd = channelPtr_->fd();
    // 关键：不能在这里直接 reset()，因为此时正在 Channel::handleEvent() 调用栈中！
    // 用 queueInLoop 延迟到下一次循环再析构
    loop_->queueInLoop([channelPtr = channelPtr_]() {});
    channelPtr_.reset();
    return sockfd;
}
```

这是一个精妙的"延迟析构"：如果在 Channel 的 `handleEvent()` 调用链中直接 `reset()` Channel，会析构正在执行的对象，触发栈上悬垂引用。`queueInLoop` 捕获一份 `shared_ptr`，让 Channel 在当前调用链结束后（下一次 loop 迭代）再析构。

### 3.5 `retry()` — 指数退避重连

```cpp
void Connector::retry(int sockfd)
{
    ::close(sockfd);               // 先关掉失败的 socket
    status_ = Status::Disconnected;
    if (connect_) {
        LOG_INFO << "Retry in " << retryInterval_ << "ms";
        loop_->runAfter(retryInterval_ / 1000.0,
                        std::bind(&Connector::startInLoop, shared_from_this()));
        retryInterval_ = retryInterval_ * 2;           // 每次翻倍
        if (retryInterval_ > maxRetryInterval_)
            retryInterval_ = maxRetryInterval_;         // 上限 30 秒
    }
}
```

**指数退避策略**：

```
第 1 次重试：500ms 后
第 2 次重试：1000ms 后
第 3 次重试：2000ms 后
第 4 次重试：4000ms 后
...
第 n 次重试：30000ms 后（上限，不再增长）
```

**为什么指数退避？**

如果服务器宕机，大量客户端同时以固定间隔重连，会形成**重连风暴**，服务器刚恢复就被打垮。指数退避让重连请求自然分散，对服务器更友好。

**`shared_from_this()` 的必要性**：

`retry()` 用 `runAfter` 把 `startInLoop` 延迟执行。在延迟执行之前，`TcpClient` 可能已经析构了 `Connector`。用 `shared_from_this()` 让定时器持有一份引用计数，保证 `Connector` 存活到定时器触发后。这就是为什么 `Connector` 继承了 `enable_shared_from_this`。

### 3.6 `stop()` 的线程安全

```cpp
void Connector::stop()
{
    status_ = Status::Disconnected;
    if (loop_->isInLoopThread()) {
        removeAndResetChannel();
    } else {
        // 跨线程调用：投递到 EventLoop 线程执行
        loop_->queueInLoop([thisPtr = shared_from_this()]() {
            thisPtr->removeAndResetChannel();
        });
    }
}
```

`stop()` 可以从任意线程调用（如用户线程触发断线），而 Channel 操作必须在 EventLoop 线程执行，因此用 `queueInLoop` 跨线程投递。

---

## 四、两个类的对比

| 特性         | Acceptor                            | Connector                           |
| ------------ | ----------------------------------- | ----------------------------------- |
| 角色         | 服务端，被动接受                    | 客户端，主动发起                    |
| 关键系统调用 | `listen()` + `accept()`             | `connect()`                         |
| epoll 事件   | 读事件（listenFd 可读=有新连接）    | 写事件（连接完成触发）              |
| 结果判断     | `accept4()` 返回 >= 0               | `getsockopt(SO_ERROR)`              |
| 错误处理     | EMFILE 用 idleFd_ 优雅拒绝          | errno 分类，可重试错误指数退避      |
| 生命周期     | 在 TcpServer 整个运行期存在         | 每次连接新建，成功后把 fd 交出      |
| 线程安全     | listen/accept 在同一 EventLoop 线程 | stop() 跨线程安全，内部 queueInLoop |
| 自连接检测   | 不需要                              | `Socket::isSelfConnect()`           |

---

## 五、socket 选项钩子机制

两个类都提供了 socket 选项钩子，给用户不改框架源码就能定制 socket 行为的能力：

```
Acceptor:
  beforeListenSetSockOptCallback_(fd)  ← listen() 之前调用
  afterAcceptSetSockOptCallback_(fd)   ← 每次 accept() 成功后调用

Connector:
  sockOptCallback_(fd)                 ← connect() 之前调用
```

**典型使用场景**：

```cpp
// 服务器：对每个新连接设置 TCP_NODELAY
acceptor.setAfterAcceptSockOptCallback([](int fd) {
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
});

// 客户端：绑定特定本地端口（如需要端口白名单穿透防火墙）
connector.setSockOptCallback([](int fd) {
    struct sockaddr_in local;
    local.sin_port = htons(12345);  // 固定本地端口
    bind(fd, (sockaddr*)&local, sizeof(local));
});
```

---

## 六、游戏服务器实践

### 6.1 服务端场景（Acceptor）

```cpp
// 典型的游戏网关服配置
TcpServer gatewayServer(&loop, InetAddress(9000), "Gateway");
// 内部：TcpServer 持有 Acceptor，在 start() 时调用 acceptor.listen()

// 对每个新连接：关 Nagle（低延迟），设置接收缓冲区
gatewayServer.setBeforeListenSockOptCallback([](int fd) {
    int size = 4 * 1024 * 1024;  // 4MB 接收缓冲区
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
});
```

### 6.2 客户端场景（Connector）

```cpp
// 游戏服连接数据库服（内网连接，需要断线重连）
TcpClient dbClient(&loop, InetAddress("10.0.0.100", 3306), "DBClient");
dbClient.enableRetry();  // 启用指数退避重连
// 内部：TcpClient 持有 Connector，断线后 Connector::retry() 自动重连

// 重连时的日志输出：
// INFO  Retry connecting to 10.0.0.100:3306 in 500 milliseconds.
// INFO  Retry connecting to 10.0.0.100:3306 in 1000 milliseconds.
// INFO  Retry connecting to 10.0.0.100:3306 in 2000 milliseconds.
```

---

## 七、思考题

1. `Acceptor` 的 `idleFd_` 在 EMFILE 时先 close 再 accept 再 close。这个操作窗口内（第一次 close 到最后一次 open 之间），如果又有新连接到来，`readCallback` 还会被再次调用吗？会不会出现问题？

2. `Connector::handleWrite()` 用 `getsockopt(SO_ERROR)` 判断 connect 结果，而不是直接用 `connect()` 的返回值。为什么 `connect()` 返回 EINPROGRESS 就判断成功是错误的？有什么场景下连接会在写事件触发时才真正失败？

3. `Connector` 继承了 `enable_shared_from_this`，而 `Acceptor` 没有。为什么 `Connector` 需要、`Acceptor` 不需要？（提示：分析两者的 `retry` 行为和 `TcpServer`/`TcpClient` 对它们的持有方式）

4. 指数退避的上限是 30 秒，但 `retryInterval_` 在重连成功后**不会**重置为初始值（500ms）。如果连接建立 → 短暂断线 → 重连时使用的是 30 秒间隔，这合理吗？如何改进？

---

*学习日期：2026-04-02 | 上一课：[第09课_网络地址与Socket封装](第09课_网络地址与Socket封装.md) | 下一课：[第11课_TcpConnection连接生命周期](第11课_TcpConnection连接生命周期.md)*

---

## 核心收获

- `idleFd_` EMFILE 技巧：预占 `/dev/null` fd，fd 耗尽时临时借用→接受→立即关闭→重新打开，让客户端收到干净 RST 而非无限等待
- 非阻塞 `connect()` 返回 `EINPROGRESS` 是正常的：注册写事件，触发后用 `getsockopt(SO_ERROR)` 检查真实结果
- errno 要分类：`EINTR/EAGAIN/EADDRINUSE` 等可重试；`ECONNREFUSED/ENETUNREACH` 等是致命错误
- 指数退避：500ms→1s→2s→...→30s 封顶，`shared_from_this()` 在 timer 期间保活 Connector
- `removeAndResetChannel()` 用 `queueInLoop` 延迟销毁 Channel，防止在 Channel 自身的回调中析构
