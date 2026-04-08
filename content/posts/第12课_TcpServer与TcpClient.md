+++
date = '2026-03-25'
draft = false
title = 'TcpServer & TcpClient — 网络通信的两端'
categories = ["网络编程"]
tags = ["C++", "trantor", "TcpServer", "TcpClient", "学习笔记"]
description = "trantor TcpServer 与 TcpClient 解析，网络通信两端的高层封装。"
+++


# 第 12 课：TcpServer & TcpClient — 网络通信的两端

> 对应源文件：
> - `trantor/net/TcpServer.h` / `TcpServer.cc` — TCP 服务器
> - `trantor/net/TcpClient.h` / `TcpClient.cc` — TCP 客户端

---

## 一、两个类的定位

```
                     ┌──────────────────────────────────────────┐
                     │              TcpServer                    │
                     │  loop_（Accept 线程）                     │
                     │  Acceptor（监听 socket）                  │
                     │  connSet_（所有连接的生命周期管理）         │
                     │  ioLoops_（I/O 线程池）                   │
                     │  timingWheelMap_（每个 I/O 线程一个时间轮）│
                     └──────────────────────────────────────────┘
                                          │ newConnection()
                                          ▼
                              TcpConnectionImpl（每个连接一个）
                              运行在 ioLoops_ 中的某个 EventLoop

                     ┌──────────────────────────────────────────┐
                     │              TcpClient                    │
                     │  loop_（单一 EventLoop）                  │
                     │  connector_（发起连接）                   │
                     │  connection_（当前连接，mutex_ 保护）     │
                     └──────────────────────────────────────────┘
```

`TcpServer` 是**一对多**：管理一个监听端口和大量并发连接。
`TcpClient` 是**一对一**：管理一条到服务器的连接（可断线重连）。

---

## 二、TcpServer

### 2.1 构造：一次性完成核心初始化

```cpp
TcpServer::TcpServer(EventLoop *loop, const InetAddress &address,
                     std::string name, bool reUseAddr, bool reUsePort)
    : loop_(loop),
      acceptorPtr_(new Acceptor(loop, address, reUseAddr, reUsePort)),
      recvMessageCallback_([](const TcpConnectionPtr &, MsgBuffer *buffer) {
          // 默认回调：清空数据并打印警告（防止用户忘记设置回调）
          LOG_ERROR << "unhandled recv message";
          buffer->retrieveAll();
      }),
      ioLoops_({loop}),    // 默认：I/O 和 Accept 在同一个 loop
      numIoLoops_(1)
{
    acceptorPtr_->setNewConnectionCallback(
        [this](int fd, const InetAddress &peer) { newConnection(fd, peer); });
}
```

**默认 `recvMessageCallback_` 的用意**：如果用户忘记设置消息回调，数据会被清空并打印 ERROR 日志，而不是死锁或 busy loop（缓冲区满了会持续触发可读事件）。

**`ioLoops_({loop})` 的含义**：默认情况下只有一个 I/O 线程，就是传入的 `loop_`，即 Accept 线程和 I/O 线程复用同一个 EventLoop（单线程模式）。

### 2.2 三种 I/O 线程配置方式

```cpp
// ① 内部线程池（常用）：TcpServer 自己创建并管理线程
server.setIoLoopNum(4);   // 创建 4 个 I/O 线程

// ② 外部线程池（共享）：多个 TcpServer 共用同一个线程池
auto pool = std::make_shared<EventLoopThreadPool>(4);
server.setIoLoopThreadPool(pool);

// ③ 外部 loop 列表（最大灵活性）：调用者自己管理线程生命周期
std::vector<EventLoop *> loops = { loop1, loop2, loop3 };
server.setIoLoops(loops);
```

三种方式都用 `assert(!started_)` 保证只能在 `start()` 之前调用。

### 2.3 `start()` — 启动服务器

```cpp
void TcpServer::start()
{
    loop_->runInLoop([this]() {
        assert(!started_);
        started_ = true;

        // 为每个 I/O 线程创建一个时间轮
        if (idleTimeout_ > 0) {
            for (EventLoop *loop : ioLoops_) {
                timingWheelMap_[loop] = std::make_shared<TimingWheel>(
                    loop,
                    idleTimeout_,
                    1.0F,                               // tick 间隔 1 秒
                    idleTimeout_ < 500 ? idleTimeout_ + 1 : 100  // 桶数量
                );
            }
        }

        acceptorPtr_->listen();   // 开始监听，epoll 注册 listenFd
    });
}
```

**每个 I/O 线程独立的 TimingWheel**：
- 每个连接只会在分配给它的 I/O EventLoop 上运行
- 时间轮必须在对应的 EventLoop 上操作（见第 8 课：TimingWheel 内部用 `runEvery`）
- 所以用 `map<EventLoop*, TimingWheel>` 为每个 I/O 线程分配独立时间轮，完全避免跨线程竞争

### 2.4 `newConnection()` — 分配连接到 I/O 线程

```cpp
void TcpServer::newConnection(int sockfd, const InetAddress &peer)
{
    loop_->assertInLoopThread();    // 此函数在 Accept 线程运行

    // 轮询选择 I/O 线程（Round-Robin）
    EventLoop *ioLoop = ioLoops_[nextLoopIdx_];
    if (++nextLoopIdx_ >= numIoLoops_) nextLoopIdx_ = 0;

    // 创建连接（指定运行在哪个 I/O loop 上）
    auto newPtr = std::make_shared<TcpConnectionImpl>(
        ioLoop, sockfd,
        InetAddress(Socket::getLocalAddr(sockfd)),
        peer,
        policyPtr_,    // TLS（可选）
        sslContextPtr_
    );

    // 开启空闲踢出
    if (idleTimeout_ > 0) {
        newPtr->enableKickingOff(idleTimeout_, timingWheelMap_[ioLoop]);
    }

    // 注册用户回调
    newPtr->setRecvMsgCallback(recvMessageCallback_);
    newPtr->setConnectionCallback([this](const TcpConnectionPtr &conn) {
        if (connectionCallback_) connectionCallback_(conn);
    });
    newPtr->setWriteCompleteCallback([this](const TcpConnectionPtr &conn) {
        if (writeCompleteCallback_) writeCompleteCallback_(conn);
    });

    // 注册内部关闭回调
    newPtr->setCloseCallback([this](const TcpConnectionPtr &conn) {
        connectionClosed(conn);
    });

    // 把连接加入 connSet_（维持 shared_ptr 引用，连接存活）
    connSet_.insert(newPtr);

    // 激活连接（在 ioLoop 线程上执行）
    newPtr->connectEstablished();
}
```

**Round-Robin 负载均衡**的简洁之美：仅用一个递增计数器加取模，无锁，O(1)，把新连接均匀分配给所有 I/O 线程。适合游戏服务器场景（连接数量有限，每个连接处理量相近）。

### 2.5 `connSet_` — 连接的生命周期管理

```cpp
std::set<TcpConnectionPtr> connSet_;
```

`connSet_` 存放所有活跃连接的 `shared_ptr`，是 TcpServer 持有连接生命周期的关键：

```
TcpServer::connSet_        → shared_ptr（引用计数=1+N）
用户回调持有的 shared_ptr  → shared_ptr（回调执行时引用计数+1）
```

连接关闭时，`connectionClosed()` 从 `connSet_` 移除 `shared_ptr`，引用计数减 1。当没有其他地方持有时，`TcpConnectionImpl` 析构，Socket 关闭。

### 2.6 连接关闭的跨线程处理

```cpp
// 连接关闭事件来自 I/O 线程（handleClose() → closeCallback_ → connectionClosed）
void TcpServer::connectionClosed(const TcpConnectionPtr &connectionPtr)
{
    if (loop_->isInLoopThread()) {
        handleCloseInLoop(connectionPtr);
    } else {
        // I/O 线程 → 投递到 Accept 线程（loop_）处理
        loop_->queueInLoop([this, connectionPtr]() {
            handleCloseInLoop(connectionPtr);
        });
    }
}

void TcpServer::handleCloseInLoop(const TcpConnectionPtr &connectionPtr)
{
    connSet_.erase(connectionPtr);  // 从 connSet_ 移除（在 Accept 线程）

    // connectDestroyed() 必须在 I/O 线程执行（因为要 ioChannelPtr_->remove()）
    // 即使当前在 Accept 线程，也要 queueInLoop 投递到 I/O 线程
    auto connLoop = connectionPtr->getLoop();
    connLoop->queueInLoop([connectionPtr]() {
        connectionPtr->connectDestroyed();
    });
}
```

**为什么 `connectDestroyed()` 用 `queueInLoop` 而不是直接调用？**

注释里说得清楚：此时连接可能还在 `loop_`（Accept 线程）的 `activeChannels_` 中等待处理（因为连接关闭事件是从 I/O 线程发过来的）。如果直接调用 `connectDestroyed()`（里面会调用 `ioChannelPtr_->remove()`），会在 Channel 还未处理完的情况下把它从 Poller 里移除，产生悬垂指针。用 `queueInLoop` 延迟到下一次循环，保证当前 active channels 处理完。

### 2.7 `stop()` — 同步停止服务器

```cpp
void TcpServer::stop()
{
    // 在 Accept 线程执行：停止接受新连接 + 关闭所有现有连接
    if (!loop_->isInLoopThread()) {
        std::promise<void> pro;
        auto f = pro.get_future();
        loop_->queueInLoop([this, &pro]() {
            acceptorPtr_.reset();           // 停止监听
            for (auto &conn : connPtrs) {
                conn->forceClose();          // 强制关闭所有连接
            }
            pro.set_value();
        });
        f.get();   // 阻塞等待上面完成（同步语义）
    }

    loopPoolPtr_.reset();   // 停止并销毁内部线程池

    // 停止每个 I/O 线程的时间轮（也是同步等待）
    for (auto &iter : timingWheelMap_) {
        std::promise<void> pro;
        auto f = pro.get_future();
        iter.second->getLoop()->runInLoop([&iter, &pro]() {
            iter.second.reset();
            pro.set_value();
        });
        f.get();
    }
}
```

**`stop()` 使用 `promise/future` 同步**的原因：调用 `stop()` 的线程（通常是主线程）需要等到服务器真正停止后再继续（例如打印"服务器已停止"或开始清理资源）。`promise/future` 是 C++ 标准库提供的轻量同步原语，适合这种"等一次操作完成"的场景。

---

## 三、TcpClient

### 3.1 关键设计：`connection_` 受 mutex 保护

```cpp
mutable std::mutex mutex_;
TcpConnectionPtr connection_;  // @GuardedBy mutex_
```

`connection_` 是 `TcpClient` 最重要的成员，它在多线程下被访问：
- I/O 线程：`newConnection()` 设置它，`removeConnection()` 清空它
- 用户线程：`connection()` 读取它，`disconnect()` 用它 `shutdown()`

所以必须加锁。读取时也加锁（`const`，但 `mutable mutex_`）。

### 3.2 `connect()` — 发起连接

```cpp
void TcpClient::connect()
{
    auto weakPtr = std::weak_ptr<TcpClient>(shared_from_this());
    connector_->setNewConnectionCallback([weakPtr](int sockfd) {
        auto ptr = weakPtr.lock();
        if (ptr) ptr->newConnection(sockfd);
        // weak_ptr 检查：如果 TcpClient 已析构，忽略这次连接
    });
    connector_->setErrorCallback([weakPtr]() {
        auto ptr = weakPtr.lock();
        if (ptr && ptr->connectionErrorCallback_)
            ptr->connectionErrorCallback_();
    });
    connect_ = true;
    connector_->start();
}
```

**为什么 `Connector` 的回调用 `weak_ptr<TcpClient>`？**

`Connector` 的生命周期由 `TcpClient` 管理（`TcpClient` 持有 `ConnectorPtr`）。但 `Connector` 的定时器（指数退避）会延迟执行 `startInLoop`，持有 `shared_from_this()`。如果 `TcpClient` 析构了，而 `Connector` 的重连定时器还在，用 `weak_ptr` 检测 `TcpClient` 是否存活，避免野指针调用。

### 3.3 `newConnection()` — 连接建立后的初始化

```cpp
void TcpClient::newConnection(int sockfd)
{
    loop_->assertInLoopThread();
    InetAddress peerAddr(Socket::getPeerAddr(sockfd));
    InetAddress localAddr(Socket::getLocalAddr(sockfd));

    auto conn = std::make_shared<TcpConnectionImpl>(loop_, sockfd, localAddr, peerAddr, ...);
    conn->setConnectionCallback(connectionCallback_);
    conn->setRecvMsgCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // close 回调：通知 TcpClient 连接断开（可选重连）
    std::weak_ptr<TcpClient> weakSelf(shared_from_this());
    conn->setCloseCallback([weakSelf](const TcpConnectionPtr &c) {
        if (auto self = weakSelf.lock())
            self->removeConnection(c);
        else
            c->getLoop()->queueInLoop([c] { c->connectDestroyed(); });
        // TcpClient 已析构：不能调用 removeConnection，
        // 但仍需 connectDestroyed() 清理 Channel
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;   // 保存连接（加锁）
    }
    conn->connectEstablished();
}
```

### 3.4 `removeConnection()` — 连接断开时的重连决策

```cpp
void TcpClient::removeConnection(const TcpConnectionPtr &conn)
{
    loop_->assertInLoopThread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();   // 清空连接引用
    }

    loop_->queueInLoop([conn]() { conn->connectDestroyed(); });

    // 重连条件：用户启用了重试 && 用户没有主动 stop()
    if (retry_ && connect_) {
        LOG_TRACE << "Reconnecting to " << connector_->serverAddress().toIpPort();
        connector_->restart();   // 触发 Connector 重连（指数退避）
    }
}
```

**`retry_` vs `connect_` 的区别**：
- `retry_`：用户是否启用断线重连（`enableRetry()` 设置）
- `connect_`：当前是否应该保持连接状态（`disconnect()` / `stop()` 会置 false）

主动断线（`disconnect()`）后不重连，非主动断线后重连。

### 3.5 析构时的细节

```cpp
TcpClient::~TcpClient()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_ == nullptr) {
        connector_->stop();   // 正在连接中，取消
        return;
    }
    // 连接已建立：替换 close 回调，不再通知 TcpClient（已析构）
    auto conn = std::atomic_load_explicit(&connection_, std::memory_order_relaxed);
    loop_->runInLoop([conn = std::move(conn)]() {
        conn->setCloseCallback([](const TcpConnectionPtr &connPtr) {
            connPtr->getLoop()->queueInLoop([connPtr] {
                connPtr->connectDestroyed();   // 仍需清理 Channel
            });
        });
    });
    connection_->forceClose();
}
```

析构时的关键处理：
1. 替换 close 回调（原来的 Lambda 捕获了 `shared_ptr<TcpClient>`，析构后访问是 UB）
2. 新的回调只做 `connectDestroyed()`（Channel 清理）
3. `forceClose()` 触发关闭

---

## 四、线程模型全景图

```
                        ┌─────────────────────────────────┐
                        │         主线程（用户代码）         │
                        │  TcpServer server(loop, ...)    │
                        │  server.setIoLoopNum(4)         │
                        │  server.start()                 │
                        └────────────┬────────────────────┘
                                     │
                    ┌────────────────▼──────────────────────┐
                    │         Accept 线程（loop_）            │
                    │  Acceptor::readCallback()             │
                    │  TcpServer::newConnection()           │
                    │  → 轮询分配 ioLoop                    │
                    │  → connSet_.insert(conn)              │
                    │  → conn->connectEstablished()         │
                    └────────────────────────────────────────┘
                          │         │         │         │
                    ┌─────▼──┐ ┌───▼────┐ ┌──▼─────┐ ┌──▼─────┐
                    │ioLoop 1│ │ioLoop 2│ │ioLoop 3│ │ioLoop 4│
                    │conn A  │ │conn B  │ │conn C  │ │conn D  │
                    │conn E  │ │conn F  │ │...     │ │...     │
                    │TimingWh│ │TimingWh│ │TimingWh│ │TimingWh│
                    └────────┘ └────────┘ └────────┘ └────────┘
```

**关键不变量**：
- 每个 `TcpConnectionImpl` 只在**一个** I/O 线程的 EventLoop 上运行
- `connSet_` 只在 Accept 线程（`loop_`）上读写
- `TcpConnectionImpl` 内部的 `readBuffer_`、`writeBufferList_` 没有锁——因为只有对应的 I/O 线程会访问

### IgnoreSigPipe 的必要性

```cpp
class IgnoreSigPipe {
  public:
    IgnoreSigPipe() { ::signal(SIGPIPE, SIG_IGN); }
} initObj;  // 静态成员，程序启动时自动构造
```

在 Linux/macOS 上，向已关闭的 socket 写数据会触发 `SIGPIPE` 信号，默认行为是**终止进程**。服务器程序必须忽略它，否则客户端突然断线会导致服务器崩溃。trantor 把这个防护嵌入 `TcpServer`/`TcpClient` 的静态成员构造中，用户无需手动处理。

---

## 五、`kickoffIdleConnections()` — 空闲连接超时

```cpp
void TcpServer::kickoffIdleConnections(size_t timeout)
{
    loop_->runInLoop([this, timeout]() {
        assert(!started_);    // 必须在 start() 之前调用！
        idleTimeout_ = timeout;
    });
}
```

在 `start()` 时，为每个 I/O 线程创建一个 `TimingWheel`。每个新连接都调用 `enableKickingOff(idleTimeout_, timingWheelMap_[ioLoop])`，把 `KickoffEntry` 插入对应的时间轮。

**为什么每个 I/O 线程独立一个时间轮**：

时间轮内部用 `runEvery()` 定时 tick，tick 触发时操作时间轮内的数据结构（`vector<deque<set<EntryPtr>>>`）。如果多个连接共用一个时间轮，时间轮必须在同一个 EventLoop 上运行。由于不同连接可能在不同 I/O 线程，让每个 I/O 线程有自己的时间轮，完全避免跨线程访问。

---

## 六、完整连接建立与关闭时序

### 建立连接

```
[Accept 线程]
  epoll 通知 listenFd 可读
  → Acceptor::readCallback()
  → accept4(listenFd) → newsockfd + peerAddr
  → TcpServer::newConnection(newsockfd, peerAddr)
      → 选 ioLoop = ioLoops_[nextLoopIdx_++]
      → 创建 TcpConnectionImpl(ioLoop, newsockfd, ...)
      → connSet_.insert(conn)
      → conn->connectEstablished()

[I/O 线程（ioLoop）]
  connectEstablished() 在 ioLoop 上执行：
  → ioChannelPtr_->tie(conn)
  → ioChannelPtr_->enableReading()   ← 注册到 ioLoop 的 epoll
  → status_ = Connected
  → connectionCallback_(conn)        ← 用户回调"连接建立"
```

### 关闭连接

```
[I/O 线程]
  readCallback() 返回 0（对端关闭）
  → handleClose()
      → status_ = Disconnected
      → ioChannelPtr_->disableAll()
      → connectionCallback_(conn)    ← 用户回调"连接断开"
      → closeCallback_(conn)         ← TcpServer::connectionClosed()

[Accept 线程]（由 queueInLoop 投递过来）
  → connSet_.erase(conn)             ← 移除 shared_ptr

[I/O 线程]（由 queueInLoop 投递过来）
  → connectDestroyed()
  → ioChannelPtr_->remove()          ← 从 epoll 彻底注销
  （此后 conn 的引用计数降为 0，析构，socket 关闭）
```

---

## 七、游戏服务器实践

### 7.1 游戏网关服配置

```cpp
// 网关服：多线程 + 空闲踢出
EventLoop mainLoop;
TcpServer gateway(&mainLoop, InetAddress(9000), "Gateway");
gateway.setIoLoopNum(4);              // 4 个 I/O 线程处理收发
gateway.kickoffIdleConnections(300);  // 5 分钟无数据踢出（防幽灵连接）

gateway.setConnectionCallback([](const TcpConnectionPtr &conn) {
    if (conn->connected()) {
        conn->setContext(std::make_shared<PlayerSession>());
        LOG_INFO << "玩家接入: " << conn->peerAddr().toIpPort();
    } else {
        auto session = conn->getContext<PlayerSession>();
        if (session) onPlayerLogout(session);
    }
});

gateway.setRecvMessageCallback([](const TcpConnectionPtr &conn, MsgBuffer *buf) {
    auto session = conn->getContext<PlayerSession>();
    protocolHandler.dispatch(conn, session, buf);
});

gateway.start();
mainLoop.loop();
```

### 7.2 服务器间内部连接（TcpClient）

```cpp
// 游戏服连接逻辑服（内部，断线自动重连）
EventLoop ioLoop;
auto client = std::make_shared<TcpClient>(&ioLoop,
    InetAddress("10.0.0.2", 7001), "LogicSrvConn");
client->enableRetry();    // 断线后指数退避重连

client->setConnectionCallback([client](const TcpConnectionPtr &conn) {
    if (conn->connected()) {
        LOG_INFO << "已连接逻辑服";
        // 注册此服务器到逻辑服
        conn->send(buildRegisterPacket(SERVER_ID));
    } else {
        LOG_WARN << "逻辑服断线，等待重连...";
    }
});

client->setMessageCallback([](const TcpConnectionPtr &conn, MsgBuffer *buf) {
    handleLogicServerMsg(conn, buf);
});

client->connect();
ioLoop.loop();
```

### 7.3 热重载 SSL 证书

```cpp
// 证书续期后，不停服重载
server.reloadSSL();
// 内部：新建 SSLContext，已有连接不影响，新连接使用新证书
```

## 核心收获

- TcpServer 三层：Accept 线程（监听）+ I/O 线程池（处理连接）+ 业务线程（可选）
- `nextLoopIdx_++ % loopNum` Round-Robin 分配新连接到各 I/O EventLoop，跨线程调用安全
- `connSet_` 是连接的所有权持有者，`shared_ptr` 引用计数归零才真正析构 TcpConnection
- `connectionClosed` 双 `queueInLoop`：I/O 线程投递到 Accept 线程，`connSet_.erase` 在正确线程执行
- `TcpClient::connection_` 由 `mutex_` 保护——这是 trantor 少数需要 mutex 的地方（供跨线程安全读取）
- `retry_`（用户意图）vs `connect_`（当前状态）：两个标志区分"用户主动断开"与"网络异常断开"

---

## 八、思考题

1. `TcpServer::newConnection()` 使用 Round-Robin 分配连接，但游戏服务器里不同玩家的数据量差异很大（大R玩家 vs 新手）。Round-Robin 可能导致某个 I/O 线程负载过高。有什么改进方案？

2. `connSet_` 是 `std::set<TcpConnectionPtr>`（红黑树，`erase` 是 O(log n)）。如果服务器有 10 万连接，每次连接断开都要 O(log n) 查找和删除，是否有性能问题？改用 `unordered_set` 有什么代价？

3. `TcpClient::removeConnection()` 在 `retry_ && connect_` 时调用 `connector_->restart()`，但 `Connector::restart()` 的实现是空函数体（`{}`）。这是一个 bug 吗？（提示：看 `Connector` 的状态变迁，`retry()` 已经调用了 `runAfter(startInLoop)`）

4. `stop()` 用 `promise/future` 阻塞等待服务器停止，但游戏服务器一般要"优雅关闭"（等玩家数据保存完再退出），`forceClose()` 会丢弃发送缓冲区的数据。如何在 trantor 框架上实现真正的优雅关闭？

---

## 九、思考题参考答案

### 1. Round-Robin 分配连接可能导致负载不均，有什么改进方案？

Round-Robin 的核心假设是**每个连接的负载大致相同**。在游戏服务器中，大 R 玩家（高活跃度、大量装备/交易操作）和新手玩家的数据量可能相差数倍甚至数十倍，这个假设不一定成立。

**改进方案有以下几种，按复杂度递增排列：**

**方案一：最少连接数（Least Connections）**

为每个 IO 线程维护一个当前活跃连接数计数器，新连接分配给连接数最少的线程。实现简单，比 Round-Robin 更能平衡负载，但连接数不等于实际负载（一个空闲连接和一个疯狂收发的连接负载差异巨大）。

```cpp
EventLoop *getLeastConnectionsLoop() {
    return *std::min_element(ioLoops_.begin(), ioLoops_.end(),
        [](EventLoop *a, EventLoop *b) {
            return a->connectionCount() < b->connectionCount();
        });
}
```

**方案二：加权轮询（Weighted Round-Robin）**

根据每个 IO 线程的实时负载（如 CPU 使用率、事件处理延迟、缓冲区积压量等）动态调整权重。负载轻的线程获得更多新连接。这需要一个监控机制，定期采集各线程的性能指标。

**方案三：动态迁移（Connection Migration）**

连接建立时仍用 Round-Robin 分配，但运行期间监测各线程的负载。当某个线程过载时，把部分连接"迁移"到负载轻的线程。这在 trantor 当前架构下很难实现，因为 Channel 和 fd 绑定到了特定的 epoll 实例，迁移需要：先从旧 epoll 移除 → 在新线程的 epoll 注册 → 切换所有回调的执行上下文。代价较大，一般不推荐。

**方案四：业务层哈希分配**

根据业务特征（如玩家所在的地图区域、公会 ID）做哈希，让同一组玩家落到同一个 IO 线程。这既能利用局部性（同地图的玩家经常交互），又能通过合理的分组避免极端不均。需要在 `newConnection()` 之前就知道玩家身份，通常需要在认证协议中携带。

**实际推荐**：对于游戏网关服，Round-Robin 在大多数场景下已经足够好（因为连接数通常有限，且负载差异不像 HTTP 服务器那么极端）。如果确实遇到热点问题，优先考虑方案一或方案四。

### 2. `connSet_` 使用 `std::set` 的 O(log n) 是否有性能问题？改用 `unordered_set` 有什么代价？

**性能分析：**

`std::set` 基于红黑树，`erase` 时间复杂度 O(log n)。10 万连接时，log₂(100000) ≈ 17 次比较。每次比较是 `shared_ptr` 的指针比较（比较裸指针地址，一条 `cmp` 指令），非常快。所以 17 次指针比较的绝对耗时大约在几十纳秒量级，**对于网络 IO 场景来说完全不是瓶颈**。

况且 `connSet_` 只在 Accept 线程（`loop_`）的 `handleCloseInLoop` 中操作 `erase`，连接建立/断开的频率远低于数据收发。即使每秒有 1000 次连接断开（已经是极端场景），总共也只消耗 ~50 微秒的 CPU 时间，可以忽略。

**改用 `unordered_set` 的代价：**

1. **哈希函数**：`std::shared_ptr<T>` 在 C++11 之后的标准库中有 `std::hash<shared_ptr<T>>` 特化，它对内部裸指针做哈希，性能没问题。

2. **内存开销**：`unordered_set` 底层是哈希桶数组，需要预分配桶空间。当连接数波动大时，rehash 会造成一次性 O(n) 的开销，且期间所有迭代器失效。相比之下 `std::set` 的内存是逐节点分配的，更加平稳。

3. **迭代顺序**：`std::set` 有确定的排序，`stop()` 中遍历所有连接 `forceClose()` 时行为可预测。`unordered_set` 无序，不影响正确性但调试时不太方便。

4. **缓存友好性**：这两者都不好——`set` 是树节点散布在堆上，`unordered_set` 是链表桶也散布在堆上。真正追求缓存友好应该用 `flat_hash_set`（如 absl::flat_hash_set）。

**结论**：对于 10 万连接级别，`std::set` 的 O(log n) 不会构成性能瓶颈。改用 `unordered_set` 能得到 O(1) 均摊复杂度，但收益微乎其微（从 ~50ns 降到 ~10ns），而且引入了 rehash 的不确定性。trantor 选择 `std::set` 是简洁稳定的选择。

### 3. `Connector::restart()` 是空函数体，这是 bug 吗？

**不是 bug，是有意为之的设计。** 但可以说是一个"不完美但无害"的实现。

分析 `TcpClient::removeConnection()` 的调用链：

```cpp
void TcpClient::removeConnection(const TcpConnectionPtr &conn)
{
    // ... 清空 connection_，调用 connectDestroyed ...
    if (retry_ && connect_) {
        connector_->restart();
    }
}
```

`removeConnection()` 在连接断开时被调用。我们需要理解：**连接是怎么断开的？**

连接断开意味着之前有一条成功建立的连接。回看 `Connector` 的状态变迁：

1. `Connector::start()` → `startInLoop()` → `connect()` → 连接成功 → `handleWrite()` → `status_ = Connected` → `newConnectionCallback_(sockfd)` → 此后 sockfd 交给了 `TcpConnectionImpl`，`Connector` 的 `channelPtr_` 已被 `removeAndResetChannel()` 清理。

2. 连接成功后，`Connector` 处于 `Connected` 状态，但已经"交出"了 socket，它自身不再持有任何 Channel 或 fd。

3. 当连接断开（对端关闭或错误），是 `TcpConnectionImpl::handleClose()` 触发的，最终调到 `TcpClient::removeConnection()`。

此时 `Connector` 的状态是 `Connected`，要重连需要：重置状态为 `Disconnected`，然后调用 `startInLoop()`。

看 `restart()` 的空实现：它什么也不做。但这不会导致问题，因为 `Connector` 原本的 `retry()` 函数（在初次连接失败时使用）已经实现了指数退避重连：

```cpp
void Connector::retry(int sockfd) {
    ::close(sockfd);
    status_ = Status::Disconnected;
    if (connect_) {
        loop_->runAfter(retryInterval_ / 1000.0,
                        std::bind(&Connector::startInLoop, shared_from_this()));
        retryInterval_ = retryInterval_ * 2;
        // ...
    }
}
```

但 `retry()` 是在**连接建立失败**时调用的，`removeConnection()` 对应的是**连接建立成功后断开**的场景。`restart()` 本应实现类似逻辑：重置 `status_` 并重新调用 `startInLoop()`。

**实际上 `restart()` 为空意味着：在 trantor 当前版本中，`TcpClient` 的"断线重连"功能事实上是不完整的。** 调用 `restart()` 后什么也不会发生，连接不会重新建立。

但这可能是**故意简化**：trantor 的 `Connector` 构造时有一个 `retry` 参数，`TcpClient` 构造 `Connector` 时传入 `false`：

```cpp
connector_(new Connector(loop, serverAddr, false))
```

这意味着 `TcpClient` 的 `Connector` 在**初次连接失败时不重试**。而断线重连（`restart()`）的逻辑也被留空，可能是等待后续完善，或者设计者认为重连策略应该由上层（如 drogon 框架）来控制，而不是在 trantor 底层实现。

**如果要修复**，`restart()` 应该实现为：

```cpp
void Connector::restart()
{
    loop_->assertInLoopThread();
    status_ = Status::Disconnected;
    retryInterval_ = kInitRetryDelayMs;  // 重置退避间隔
    connect_ = true;
    startInLoop();
}
```

### 4. 如何在 trantor 框架上实现真正的优雅关闭？

`stop()` 中直接 `forceClose()` 会丢弃发送缓冲区数据，这对游戏服务器是不可接受的——玩家最后的数据保存确认包、下线通知包都可能丢失。以下是一个分阶段优雅关闭方案：

**阶段一：停止接受新连接**

```cpp
void gracefulStop(TcpServer &server, EventLoop *loop) {
    // 1. 停止 Acceptor，不再接受新连接
    loop->runInLoop([&server]() {
        server.getAcceptor()->stopListening();  // 需要暴露接口或改造
    });
}
```

**阶段二：通知所有在线玩家即将关服**

```cpp
    // 2. 向所有连接发送关服通知
    for (auto &conn : connSet_) {
        conn->send(buildShutdownNoticePacket());
    }
```

**阶段三：等待业务层完成数据保存**

```cpp
    // 3. 通知业务层保存所有玩家数据
    saveAllPlayerData([&server, loop]() {
        // 数据保存完成回调
        // 4. 对所有连接调用 shutdown()（而非 forceClose()）
        loop->runInLoop([&server]() {
            for (auto &conn : server.getConnections()) {
                conn->shutdown();  // 优雅关闭：等发送缓冲区清空后半关闭
            }
        });
    });
```

**阶段四：设置超时兜底**

```cpp
    // 5. 设置超时：如果 N 秒后还有连接没关完，强制关闭
    loop->runAfter(30.0, [&server]() {
        for (auto &conn : server.getConnections()) {
            if (conn->connected()) {
                LOG_WARN << "Force closing connection: " << conn->peerAddr().toIpPort();
                conn->forceClose();
            }
        }
    });
```

**阶段五：等所有连接关闭后退出事件循环**

```cpp
    // 6. 定时检查连接数，归零后退出
    std::shared_ptr<trantor::Timer> checker;
    loop->runEvery(0.5, [&server, loop]() {
        if (server.getConnections().empty()) {
            loop->quit();  // 所有连接已关闭，退出事件循环
        }
    });
```

**完整流程总结**：

```
停止监听 → 发关服通知 → 保存玩家数据 → shutdown() 所有连接
    → 等待缓冲区排空 → 连接自然关闭 → connSet_ 清空 → loop quit
                                    ↑
                              超时兜底 forceClose()
```

**关键点**：
- 用 `shutdown()` 替代 `forceClose()`，让发送缓冲区中的数据能发完
- 业务层先完成数据落库，再关闭连接
- 必须有超时兜底，防止某些连接（如客户端已崩溃）永远不关闭
- 整个过程是异步的，不会阻塞事件循环

---

*学习日期：2026-03-25 | 上一课：[第11课_TcpConnection连接生命周期](第11课_TcpConnection连接生命周期.md) | 下一课：[第13课_多线程EventLoop](第13课_多线程EventLoop.md)*
