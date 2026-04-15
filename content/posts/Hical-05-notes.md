+++
title = '第5课：TCP 连接与服务器'
date = '2026-04-15'
draft = false
tags = ["C++", "TCP", "SSL", "if constexpr", "协程", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "理解 GenericConnection 模板如何用 if constexpr 统一 TCP/SSL，掌握连接状态机和 TcpServer 的协程 accept 循环。"
+++

# 第5课：TCP 连接与服务器 - 学习笔记

> 理解 GenericConnection 模板如何用 if constexpr 统一 TCP/SSL，掌握连接状态机和 TcpServer 的协程 accept 循环。

---

## 一、GenericConnection — 模板化连接

### 1.1 核心设计思想

**源码位置**：`src/asio/GenericConnection.h`

`GenericConnection<SocketType>` 是一个类模板，通过 **一套代码** 同时支持普通 TCP 和 SSL/TLS 连接：

```cpp
template <typename SocketType>
class GenericConnection : public TcpConnection { ... };

// 两种实例化：
using PlainConnection = GenericConnection<boost::asio::ip::tcp::socket>;
using SslConnection   = GenericConnection<boost::asio::ssl::stream<tcp::socket>>;
```

**为什么用模板而不是运行时多态？**

| 方式                  | 代码量           | 性能                 | SSL 分支         |
| --------------------- | ---------------- | -------------------- | ---------------- |
| 模板 + `if constexpr` | 1 份             | 零开销（编译期消除） | 被丢弃分支不编译 |
| 虚函数 + 两个子类     | 2 份（大量重复） | vtable 查找          | 运行时分支       |

模板方案的关键收益：readLoop、writeLoop、send 等逻辑 TCP 和 SSL 完全相同，只有握手、关闭、获取底层 socket 等少数地方有差异。`if constexpr` 精准处理差异，避免代码重复。

### 1.2 类型萃取 — IsSslStream

```cpp
// 默认：不是 SSL
template <typename T>
struct IsSslStream : std::false_type {};

// 特化：ssl::stream<T> 是 SSL
template <typename T>
struct IsSslStream<boost::asio::ssl::stream<T>> : std::true_type {};

// 便捷变量模板
template <typename T>
inline constexpr bool hIsSslStream = IsSslStream<T>::value;
```

这是经典的 **模板特化类型萃取** 模式：
- `hIsSslStream<tcp::socket>` → `false`
- `hIsSslStream<ssl::stream<tcp::socket>>` → `true`

编译器在编译期就确定了类型，用于 `if constexpr` 分支选择。

### 1.3 if constexpr 应用场景一览

GenericConnection 中共有 **4 处** `if constexpr` 分支：

| 方法                   | TCP 分支                 | SSL 分支                                |
| ---------------------- | ------------------------ | --------------------------------------- |
| `lowestLayerSocket()`  | 直接返回 `socket_`       | 返回 `socket_.lowest_layer()`           |
| `socketExecutor()`     | `socket_.get_executor()` | `socket_.lowest_layer().get_executor()` |
| `connectEstablished()` | 直接触发回调 + startRead | 先异步握手，成功后再触发回调            |
| `shutdownInLoop()`     | TCP shutdown_send        | SSL close_notify + TCP shutdown         |

**最典型的例子 — connectEstablished**：

```cpp
void connectEstablished() {
    state_.store(State::hConnected);

    if constexpr (hIsSslStream<SocketType>) {
        // SSL：先握手
        co_spawn(socketExecutor(), [conn]() -> awaitable<void> {
            co_await conn->sslHandshake();  // 异步 TLS 握手
        }, detached);
        // 握手成功后在 sslHandshake() 内部触发回调和 startRead
    } else {
        // TCP：直接就绪
        if (connectionCallback_) connectionCallback_(self);
        startRead();
    }
}
```

### 1.4 连接状态机

```cpp
enum class State {
    hConnecting,      // 初始状态 / SSL 正在握手
    hConnected,       // 连接已建立，可以收发数据
    hDisconnecting,   // 正在关闭（shutdown 发起后）
    hDisconnected     // 已完全断开
};
```

**状态转换图**：

```
                 构造
                  │
                  ▼
            ┌──────────┐
            │Connecting │
            └─────┬────┘
                  │ connectEstablished()
                  │ (SSL: 握手成功后)
                  ▼
            ┌──────────┐
            │Connected  │ ←── 正常工作状态
            └──┬────┬──┘
               │    │
    shutdown() │    │ close()
               │    │
               ▼    ▼
        ┌───────────────┐
        │ Disconnecting  │
        └───────┬───────┘
                │ handleClose()
                ▼
        ┌───────────────┐
        │ Disconnected   │ ←── 终态
        └───────────────┘
```

**原子状态变量**：`std::atomic<State> state_` 保证跨线程读写安全。

```cpp
// close() 中的 CAS 确保只转换一次
auto expected = State::hConnected;
if (state_.compare_exchange_strong(expected, State::hDisconnecting)) {
    // 成功转换 → 执行关闭逻辑
}
// 如果已经是 hDisconnecting → 直接返回（防止重复关闭）
```

### 1.5 读循环协程 — readLoop

```cpp
boost::asio::awaitable<void> GenericConnection<SocketType>::readLoop() {
    try {
        static constexpr size_t hMinReadSize = 4096;
        while (reading_ && state_.load() == State::hConnected) {
            // 确保缓冲区有足够空间
            inputBuffer_.ensureWritableBytes(hMinReadSize);

            // 零拷贝：直接读入 PmrBuffer 的可写区域
            auto bytesRead = co_await socket_.async_read_some(
                boost::asio::buffer(inputBuffer_.beginWrite(), inputBuffer_.writableBytes()),
                boost::asio::use_awaitable);

            bytesReceived_ += bytesRead;
            inputBuffer_.hasWritten(bytesRead);      // 标记已写入

            if (messageCallback_) {
                messageCallback_(sharedThis(), &inputBuffer_);  // 通知上层
            }
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != boost::asio::error::operation_aborted) {
            handleClose();   // 非主动取消的错误 → 关闭连接
        }
    }
}
```

**关键点**：

1. **零拷贝读取**：直接把数据读入 PmrBuffer 的可写区域，避免中间临时 buffer 的拷贝
2. **每次读取后回调**：不等凑齐完整消息，收到多少就通知多少——**粘包/拆包由上层处理**
3. **operation_aborted 过滤**：主动关闭连接时，Asio 会以此错误码取消异步操作，这是正常行为，不应视为错误

### 1.6 写循环协程 — writeLoop

```cpp
boost::asio::awaitable<void> GenericConnection<SocketType>::writeLoop() {
    try {
        while (true) {
            std::deque<std::shared_ptr<std::string>> batch;

            {
                std::lock_guard<std::mutex> lock(writeMutex_);
                if (writeQueue_.empty()) {
                    writing_.store(false);
                    // 双重检查：防止 store false 与 enqueue 之间的竞态
                    if (!writeQueue_.empty()) {
                        writing_.store(true);
                        batch.swap(writeQueue_);
                        // 更新 queuedBytes_...
                    } else {
                        // 通知写完成
                        if (writeCompleteCallback_) { ... }
                        co_return;
                    }
                } else {
                    batch.swap(writeQueue_);
                    // 更新 queuedBytes_...
                }
            }

            if (batch.size() == 1) {
                // 单条消息：直接发送
                auto bytesWritten = co_await async_write(socket_, buffer(*batch.front()), use_awaitable);
            } else {
                // 多条消息：Scatter-Gather I/O 一次发送
                std::vector<const_buffer> buffers;
                for (const auto& msg : batch) buffers.emplace_back(buffer(*msg));
                auto bytesWritten = co_await async_write(socket_, buffers, use_awaitable);
            }
        }
    } catch (...) {
        writing_.store(false);
        handleClose();
    }
}
```

**设计亮点**：

| 技术                   | 说明                                              |
| ---------------------- | ------------------------------------------------- |
| **批量发送**           | 一次取出队列中所有消息，减少系统调用              |
| **Scatter-Gather I/O** | 多个缓冲区合并为一次 `writev` 系统调用            |
| **写协程按需启动**     | `tryStartWrite()` 用 CAS 确保只有一个写协程在运行 |
| **双重检查锁**         | 在 `writing_ = false` 后重检队列，防止竞态丢消息  |

**tryStartWrite 的 CAS 机制**：

```cpp
void tryStartWrite() {
    bool expected = false;
    if (writing_.compare_exchange_strong(expected, true)) {
        // CAS 成功 → 没有写协程在运行 → 启动新的
        co_spawn(socketExecutor(), [self]() -> awaitable<void> {
            co_await self->writeLoop();
        }, detached);
    }
    // CAS 失败 → 已有写协程在运行，它会处理新入队的数据
}
```

### 1.7 高水位线机制

```cpp
// 在 sendInLoop 中检查
if (highWaterMarkCallback_ && queuedBytes_ >= highWaterMark_) {
    highWaterMarkCallback_(self, currentQueuedBytes);
}
```

**queuedBytes_** 维护当前写队列的累计字节数（O(1)），避免每次遍历队列计算。

**高水位线解决什么问题？** 如果客户端很慢（比如手机弱网），服务器不断 send 但对端来不及收，写队列会无限增长，最终 OOM。高水位回调让上层可以暂停发送或断开连接。

**游戏服务器类比**：给某个玩家发消息时，如果他的发送队列超过 64MB（默认 `highWaterMark_`），说明这个客户端网络极差或恶意不接收数据，应该踢掉。

### 1.8 send 的线程安全设计

```cpp
void send(const char* data, size_t len) {
    if (state_.load() != State::hConnected) return;

    if (loop_->isInLoopThread()) {
        sendInLoop(data, len);              // 同线程：直接入队
    } else {
        auto msg = std::make_shared<std::string>(data, len);
        auto self = sharedThis();
        loop_->post([this, msg, self]() {   // 跨线程：投递到事件循环
            sendInLoop(msg->data(), msg->size());
        });
    }
}
```

跨线程 send 时，数据被拷贝到 `shared_ptr<string>` 中，通过 `post` 投递到连接所属的事件循环线程执行。这保证了 writeQueue_ 的操作总在同一个线程。

---

## 二、TcpServer — 连接管理器

### 2.1 整体结构

**源码位置**：`src/asio/TcpServer.h` / `src/asio/TcpServer.cpp`

```cpp
class TcpServer {
    AsioEventLoop* baseLoop_;                  // 主循环（负责 accept）
    InetAddress listenAddr_;
    std::string name_;
    boost::asio::ip::tcp::acceptor acceptor_;  // 监听器
    std::atomic<bool> running_{false};

    size_t ioLoopNum_{0};                      // IO 线程数
    std::unique_ptr<EventLoopPool> ioPool_;    // IO 线程池

    std::set<TcpConnection::Ptr> connections_; // 连接集合
    mutable std::mutex connectionsMutex_;

    // 三种回调
    NewConnectionCallback newConnectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    TcpConnection::CloseCallback closeCallback_;

    std::shared_ptr<SslContext> sslCtx_;       // SSL（可选）
    std::shared_ptr<std::atomic<bool>> alive_; // 生命周期标志
};
```

### 2.2 线程模型

```
                    ┌─────────────────┐
                    │  baseLoop (主)   │  运行 accept 协程
                    │  Thread 0        │
                    └────────┬────────┘
                             │ 新连接到来
                             │ getNextIoLoop() round-robin
                ┌────────────┼────────────┐
                ▼            ▼            ▼
          ┌──────────┐ ┌──────────┐ ┌──────────┐
          │ ioLoop 0 │ │ ioLoop 1 │ │ ioLoop 2 │  处理连接的读写
          │ Thread 1 │ │ Thread 2 │ │ Thread 3 │
          └──────────┘ └──────────┘ └──────────┘
```

- **baseLoop**：运行 accept 协程，接收新连接
- **ioLoop**：处理连接的数据读写（由 EventLoopPool 管理）
- 如果 `ioLoopNum_ == 0`：所有连接都在 baseLoop 上处理（单线程模式）

### 2.3 start() — 启动流程

```cpp
void TcpServer::start() {
    if (running_.exchange(true)) return;     // 防重入

    // 1. 启动 IO 线程池
    if (ioLoopNum_ > 0) {
        ioPool_ = std::make_unique<EventLoopPool>(ioLoopNum_);
        ioPool_->start();
    }

    // 2. 打开监听
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(reuse_address(true));  // SO_REUSEADDR
    acceptor_.bind(endpoint);
    acceptor_.listen();

    // 3. 更新实际监听地址（端口 0 → 系统分配）
    auto actualEndpoint = acceptor_.local_endpoint();
    listenAddr_ = InetAddress(actualEndpoint.address().to_string(), actualEndpoint.port());

    // 4. 启动 accept 协程
    co_spawn(baseLoop_->getIoContext(), [this]() -> Awaitable<void> {
        co_await acceptLoop();
    }, detached);
}
```

**端口 0 的用途**：测试中常用 `InetAddress("127.0.0.1", 0)` 让系统自动分配空闲端口，避免端口冲突。`start()` 后通过 `listenAddr().port()` 获取实际端口。

### 2.4 acceptLoop — 协程式连接接受

```cpp
Awaitable<void> TcpServer::acceptLoop() {
    while (running_.load()) {
        try {
            // 1. 异步等待新连接
            tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);

            // 2. 获取地址信息
            InetAddress peerAddr(remoteEp.address().to_string(), remoteEp.port());
            InetAddress localAddr(localEp.address().to_string(), localEp.port());

            // 3. 选择目标 IO 循环
            auto* ioLoop = getNextIoLoop();   // round-robin

            // 4. 创建连接（根据是否启用 SSL）
            TcpConnection::Ptr conn;
            if (sslCtx_) {
                ssl::stream<tcp::socket> sslStream(std::move(socket), sslCtx_->native());
                conn = std::make_shared<SslConnection>(ioLoop, std::move(sslStream), ...);
            } else {
                conn = std::make_shared<PlainConnection>(ioLoop, std::move(socket), ...);
            }

            // 5. 设置回调
            conn->onMessage(messageCallback_);
            conn->onClose([aliveFlag, self, userCb](const TcpConnection::Ptr& c) {
                if (userCb) userCb(c);           // 用户回调
                if (aliveFlag->load()) {
                    self->removeConnection(c);    // 从连接集合移除
                }
            });

            // 6. 注册并建立
            addConnection(conn);
            if (newConnectionCallback_) newConnectionCallback_(conn);
            conn->connectEstablished();  // SSL 会在此触发握手

        } catch (const system_error& e) {
            if (e.code() == operation_aborted) break;  // acceptor 被关闭
        }
    }
}
```

### 2.5 alive_ 标志 — 防止 use-after-free

```cpp
std::shared_ptr<std::atomic<bool>> alive_;

// 析构时
TcpServer::~TcpServer() {
    alive_->store(false);  // 标记已销毁
    // ...
}

// 关闭回调中
conn->onClose([aliveFlag, self, userCb](const TcpConnection::Ptr& c) {
    if (userCb) userCb(c);
    if (aliveFlag->load()) {          // 检查 TcpServer 是否还活着
        self->removeConnection(c);
    }
});
```

**问题场景**：TcpServer 析构后，某个连接的 onClose 回调被触发，回调中访问已销毁的 TcpServer（`self->removeConnection`）。

**解决方案**：`alive_` 是 `shared_ptr<atomic<bool>>`，回调捕获它的拷贝（引用计数 +1）。即使 TcpServer 析构了，`alive_` 指向的 bool 还在（因为回调还持有 shared_ptr），可以安全地读取 `false`。

### 2.6 stop() — 优雅关闭

```cpp
void TcpServer::stop() {
    if (!running_.exchange(false)) return;

    // 1. 关闭 acceptor → accept 协程收到 operation_aborted → 退出
    acceptor_.close(ec);

    // 2. 关闭所有连接
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& conn : connections_) conn->close();
        connections_.clear();
    }

    // 3. 停止 IO 线程池
    if (ioPool_) ioPool_->stop();
}
```

三阶段关闭：先停止接受新连接 → 再关闭已有连接 → 最后停线程池。

---

## 三、SslContext — SSL 配置封装

**源码位置**：`src/core/SslContext.h` / `src/core/SslContext.cpp`

### 3.1 职责

封装 `boost::asio::ssl::context`，提供友好的证书/密钥加载接口：

```cpp
class SslContext {
    boost::asio::ssl::context ctx_;     // 底层 Asio SSL 上下文

public:
    SslContext();                                      // 默认 TLS 自适应
    SslContext(ssl::context::method method);            // 指定 TLS 版本

    void loadCertificate(const std::string& certFile); // 加载 PEM 证书
    void loadPrivateKey(const std::string& keyFile);   // 加载 PEM 私钥
    void loadCaCertificate(const std::string& caFile); // 加载 CA 证书
    void setVerifyPeer(bool verifyPeer);               // 是否验证对端

    ssl::context& native();                            // 获取底层对象
};
```

### 3.2 典型使用方式

```cpp
// 服务端 SSL 配置
auto sslCtx = std::make_shared<SslContext>(ssl::context::tls_server);
sslCtx->loadCertificate("server.crt");
sslCtx->loadPrivateKey("server.key");

TcpServer server(&loop, addr, "https-server");
server.enableSsl(sslCtx);   // 之后所有新连接都走 SSL
server.start();
```

### 3.3 验证模式

```cpp
void SslContext::setVerifyPeer(bool verifyPeer) {
    if (verifyPeer) {
        ctx_.set_verify_mode(verify_peer | verify_fail_if_no_peer_cert);
    } else {
        ctx_.set_verify_mode(verify_none);
    }
}
```

| 模式                          | 说明             | 使用场景       |
| ----------------------------- | ---------------- | -------------- |
| `verify_none`                 | 不验证对端证书   | 开发/测试环境  |
| `verify_peer`                 | 验证对端证书     | 生产环境 HTTPS |
| `verify_fail_if_no_peer_cert` | 对端必须提供证书 | 双向 TLS 认证  |

---

## 四、SSL 连接的完整生命周期

```
accept() 返回 raw tcp::socket
    │
    ▼
创建 ssl::stream<tcp::socket>(std::move(socket), sslCtx.native())
    │
    ▼
connectEstablished()
    │
    ├── if constexpr SSL → 启动握手协程
    │       │
    │       ▼
    │   sslHandshake()
    │       co_await socket_.async_handshake(server, use_awaitable)
    │       │
    │       ├── 握手成功 → connectionCallback_ → startRead()
    │       └── 握手失败 → handleClose()
    │
    └── if constexpr TCP → connectionCallback_ → startRead()

    ... 正常读写 ...

shutdown() / close()
    │
    ├── if constexpr SSL →
    │       co_await socket_.async_shutdown()   // TLS close_notify
    │       sock.shutdown(shutdown_send)         // TCP FIN
    │
    └── if constexpr TCP →
            sock.shutdown(shutdown_send)         // TCP FIN
```

---

## 五、从测试看完整用法

### 5.1 TcpServer 测试

**源码位置**：`tests/test_tcp_server.cpp`

| 测试               | 验证点                                                         |
| ------------------ | -------------------------------------------------------------- |
| `StartAndStop`     | 启动后 isRunning=true，停止后 false                            |
| `AcceptConnection` | 客户端连接后 newConnectionCallback 被触发，connectionCount ≥ 1 |
| `ReceiveMessage`   | 客户端发 "Hello TcpServer!"，服务端 onMessage 收到相同数据     |
| `WithIoPool`       | 设置 2 个 IO 线程，3 个客户端连接全部成功                      |

**AcceptConnection 测试的典型模式**：

```cpp
TEST(TcpServerTest, AcceptConnection) {
    AsioEventLoop loop;
    InetAddress addr("127.0.0.1", 0);   // 端口 0 = 系统分配
    TcpServer server(&loop, addr, "test-server");

    std::atomic<bool> connected{false};
    server.onNewConnection([&connected](const TcpConnection::Ptr&) {
        connected = true;
    });

    // 在后台线程运行事件循环
    std::thread loopThread([&loop]() { loop.run(); });

    server.start();

    // 客户端连接（用实际分配的端口）
    tcp::socket client(loop.getIoContext());
    client.connect(tcp::endpoint(make_address("127.0.0.1"), server.listenAddr().port()));

    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(connected.load());
}
```

### 5.2 SSL 测试

**源码位置**：`tests/test_ssl_connection.cpp`

| 测试                    | 验证点                                                   |
| ----------------------- | -------------------------------------------------------- |
| `DefaultConstruction`   | SslContext 默认构造不抛异常                              |
| `SetVerifyMode`         | 设置验证模式不抛异常                                     |
| `LoadInvalidCertThrows` | 加载不存在的证书文件抛 runtime_error                     |
| `NativeSslHandshake`    | 完整的 SSL 握手 + 数据收发（使用原生 Asio）              |
| `IsSslCompileTime`      | `static_assert` 验证 `PlainConnection::isSsl() == false` |

**NativeSslHandshake 测试是最完整的**：

```
服务端协程                              客户端协程
─────────                               ─────────
acceptor.async_accept()                 socket.async_connect()
    │                                       │
    ▼                                       ▼
serverSsl.async_handshake(server)       clientSsl.async_handshake(client)
    │                                       │
    ▼                                       ▼
serverSsl.async_read_some()             async_write("Hello SSL!")
    │
    ▼
receivedData == "Hello SSL!" ✓
```

---

## 六、关键设计模式总结

| 模式                    | 应用                       | 说明                                      |
| ----------------------- | -------------------------- | ----------------------------------------- |
| **模板 + if constexpr** | GenericConnection          | 一套代码支持 TCP/SSL，编译期分支消除      |
| **类型萃取**            | IsSslStream                | 编译期判断 socket 类型                    |
| **协程式 accept**       | TcpServer::acceptLoop      | 协程循环替代回调式 accept                 |
| **CAS 原子操作**        | writing_、state_、running_ | 无锁状态管理                              |
| **shared_ptr 生命周期** | alive_ 标志                | 防止异步回调中的 use-after-free           |
| **双重检查**            | writeLoop 的队列检查       | 防止 writing_=false 与 enqueue 之间的竞态 |
| **Scatter-Gather I/O**  | writeLoop 的多缓冲区发送   | 减少系统调用                              |
| **连接注册表**          | connections_ set           | 跟踪所有活跃连接，支持优雅关闭            |

---

## 七、关键问题思考与回答

**Q1: GenericConnection 为什么用模板而不是运行时多态？**

> 1. **代码复用**：TCP 和 SSL 的 readLoop/writeLoop/send 逻辑完全相同，模板只需一份代码
> 2. **零开销**：`if constexpr` 在编译期消除不需要的分支，没有虚函数开销
> 3. **类型安全**：被丢弃的 SSL 分支不需要编译通过，TCP 实例化不会引入 SSL 相关代码
> 4. **如果用虚函数**：需要 PlainConnection 和 SslConnection 两个独立子类，90% 的代码重复

**Q2: 连接的读写循环为什么用协程而不是回调？**

> 协程的优势在 readLoop 中最为明显：
> - **线性逻辑**：`while(connected) { read → process }` 直观清晰
> - **错误处理**：一个 try-catch 覆盖所有异步操作，回调方式每层都要检查 ec
> - **生命周期**：`shared_from_this()` 一次性捕获，协程运行期间对象不会析构
> - **状态管理**：不需要在回调间传递状态，局部变量自然保持

**Q3: TcpServer 如何通过 EventLoopPool 实现多线程处理？**

> 1. baseLoop 运行 accept 协程，接收新连接
> 2. `getNextIoLoop()` 通过 round-robin 选择一个 IO EventLoop
> 3. 新连接绑定到选中的 IO EventLoop 上（`GenericConnection` 构造时传入 ioLoop）
> 4. 该连接后续的所有读写操作都在 ioLoop 的线程上执行
> 5. 效果：accept 在主线程，IO 分散到多个工作线程

**Q4: 高水位线回调（onHighWaterMark）解决什么问题？**

> 防止发送队列无限增长导致 OOM。当 `queuedBytes_` 超过阈值（默认 64MB）时，通知上层。上层可以：
> - 暂停向该连接发送数据
> - 直接断开连接（客户端可能已经不在了）
> - 记录日志告警
>
> 游戏服务器中，这对应"某个客户端网络极差，发送积压超限，强制踢出"的场景。

**Q5: if constexpr (hIsSslStream) 相比虚函数有什么优势？**

> 1. **被丢弃的分支不需要编译通过**：TCP 实例化时，`socket_.async_handshake()` 这样的 SSL 专有方法不需要存在
> 2. **零运行时开销**：编译期消除，最终机器码中只有一个分支
> 3. **一份代码**：维护成本低，修改一处两种连接同步更新

---

## 八、与游戏服务器架构的对比

| Hical 概念                  | 游戏服务器等价物                   |
| --------------------------- | ---------------------------------- |
| `TcpServer`                 | 网关服务器（Gateway）              |
| `acceptLoop`                | 连接接受器（AcceptThread）         |
| `GenericConnection`         | 客户端 Session 对象                |
| `readLoop`                  | 收包循环                           |
| `writeLoop + writeQueue_`   | 发包队列 + 发送线程                |
| `highWaterMark`             | 发送积压告警 → 踢人                |
| `connectionEstablished`     | Session 初始化（绑定玩家数据）     |
| `handleClose`               | 断线处理（保存数据、通知其他玩家） |
| `EventLoopPool` round-robin | 工作线程组按编号分配连接           |
| `alive_` 标志               | 服务关闭时防止回调访问已释放资源   |
| `SslContext`                | HTTPS 网关或加密通信通道           |

---

*下一课：第6课 - HTTP 协议与路由，将深入 HttpRequest/HttpResponse 封装和 Router 的双策略设计。*
