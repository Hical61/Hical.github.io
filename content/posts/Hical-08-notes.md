+++
title = '第8课：HttpServer 整合'
date = '2026-04-15'
draft = false
tags = ["C++", "HTTP服务器", "Keep-Alive", "安全", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "理解 HttpServer 如何整合 TcpServer + Router + MiddlewarePipeline，掌握完整的 HTTP 请求生命周期。"
+++

# 第8课：HttpServer 整合 - 学习笔记

> 理解 HttpServer 如何整合 TcpServer + Router + MiddlewarePipeline，掌握完整的 HTTP 请求生命周期。

---

## 一、HttpServer — 高层门面

### 1.1 职责

**源码位置**：`src/core/HttpServer.h` / `src/core/HttpServer.cpp`

HttpServer 是整个框架的**顶层门面（Facade）**，整合所有底层组件：

```
HttpServer（用户直接使用的入口）
    │
    ├── io_context + acceptor      网络监听
    ├── Router                      路由分发
    ├── MiddlewarePipeline          中间件管道
    ├── SslContext（可选）           SSL/TLS 支持
    └── MemoryPool                  内存管理
```

**用户只需几行代码即可启动完整的 HTTP 服务器**：

```cpp
HttpServer server(8080);
server.router().get("/", handler);
server.use(logMiddleware);
server.start();  // 阻塞
```

### 1.2 内部成员

```cpp
class HttpServer {
    std::atomic<uint16_t> port_;                  // 监听端口
    size_t ioThreads_;                             // IO 线程数
    boost::asio::io_context ioContext_;             // 事件循环
    std::unique_ptr<tcp::acceptor> acceptor_;       // 监听器
    std::atomic<bool> running_{false};

    Router router_;                                 // 路由器
    MiddlewarePipeline middlewarePipeline_;          // 中间件管道

    std::shared_ptr<SslContext> sslCtx_;            // SSL 上下文（可选）

    size_t maxBodySize_{1024 * 1024};              // 最大请求体 1MB
    size_t maxHeaderSize_{8192};                   // 最大请求头 8KB
};
```

与第5课的 TcpServer 不同，HttpServer 直接持有 `io_context`，不依赖 AsioEventLoop 层。这是因为 HttpServer 是面向最终用户的简化 API，不需要暴露底层的 EventLoop 抽象。

---

## 二、start() — 启动流程

```cpp
void HttpServer::start() {
    running_.store(true);

    // 1. 创建并绑定 acceptor
    acceptor_ = std::make_unique<tcp::acceptor>(ioContext_);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(reuse_address(true));  // SO_REUSEADDR
    acceptor_->bind(endpoint);
    acceptor_->listen();

    // 2. 更新实际端口（端口 0 时系统分配）
    port_.store(acceptor_->local_endpoint().port());

    // 3. 启动 accept 协程
    coSpawn(ioContext_, acceptLoop());

    // 4. 多线程运行 io_context
    std::vector<std::thread> threads;
    struct ThreadJoiner {                      // RAII 守卫
        std::vector<std::thread>& threads;
        ~ThreadJoiner() {
            for (auto& t : threads)
                if (t.joinable()) t.join();
        }
    } joiner{threads};

    for (size_t i = 1; i < ioThreads_; ++i) {
        threads.emplace_back([this]() { ioContext_.run(); });
    }

    ioContext_.run();  // 主线程也参与（阻塞在这里）

    running_.store(false);
}
```

**关键设计点**：

**1. ThreadJoiner RAII 守卫**

如果 `ioContext_.run()` 抛异常，`joiner` 的析构函数会自动 `join` 所有工作线程，避免 `std::terminate`。

**2. 多线程模型**

```
ioThreads_ = 4 的情况：

Thread 0 (主线程)  → ioContext_.run()   ← start() 阻塞在这里
Thread 1 (工作)    → ioContext_.run()
Thread 2 (工作)    → ioContext_.run()
Thread 3 (工作)    → ioContext_.run()

所有线程共享同一个 io_context，Asio 内部保证并发安全。
```

注意：这里用的是 **N threads : 1 io_context** 模型（多线程共享一个 io_context），与 TcpServer 的 **1:1** 模型不同。HttpServer 选择更简单的方式，适合中小规模服务。

**3. 端口 0 → 系统分配**

测试中常用 `HttpServer(0)`，系统会分配空闲端口。`start()` 后通过 `port()` 获取实际端口。

---

## 三、stop() — 优雅关闭

```cpp
void HttpServer::stop() {
    if (!running_.exchange(false)) return;

    // 在 io_context 线程内关闭 acceptor（避免与 acceptLoop 竞态）
    boost::asio::post(ioContext_, [this]() {
        if (acceptor_) {
            boost::system::error_code ec;
            acceptor_->close(ec);
        }
    });

    ioContext_.stop();  // 中断所有线程的 run()
}
```

**为什么用 post 关闭 acceptor？**

`acceptor_->close()` 必须在 `acceptLoop` 所在的线程中执行，否则会出现竞态：acceptLoop 正在 `async_accept`，另一个线程同时 `close()` acceptor。`post` 保证关闭操作被调度到 io_context 线程内串行执行。

---

## 四、handleSession — HTTP 请求生命周期

这是整个框架最核心的协程，一次 HTTP 连接的**完整生命周期**都在这里：

```cpp
Awaitable<void> HttpServer::handleSession(tcp::socket socket) {
    // RAII 守卫：确保 socket 在任何路径都被关闭
    struct SocketGuard {
        tcp::socket& sock;
        ~SocketGuard() {
            if (sock.is_open()) {
                boost::system::error_code ec;
                sock.shutdown(tcp::socket::shutdown_send, ec);
                sock.close(ec);
            }
        }
    } guard{socket};

    try {
        // 1. 创建请求级 PMR 单调池
        auto requestPool = MemoryPool::instance().createRequestPool();
        std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());
        beast::basic_flat_buffer<pmr_alloc> buffer(alloc);

        for (;;) {
            // 2. 解析请求（带大小限制）
            http::request_parser<http::string_body> parser;
            parser.body_limit(maxBodySize_);      // 默认 1MB
            parser.header_limit(maxHeaderSize_);  // 默认 8KB

            co_await http::async_read(socket, buffer, parser, use_awaitable);
            auto beastReq = parser.release();

            // 3. 检查 WebSocket 升级
            if (ws::is_upgrade(beastReq)) {
                auto* wsRoute = router_.findWsRoute(reqPath);
                if (wsRoute) {
                    co_await handleWebSocket(std::move(socket), std::move(beastReq), *wsRoute);
                    co_return;  // WebSocket 接管，HTTP 会话结束
                }
            }

            // 4. 封装为 HttpRequest
            HttpRequest req(std::move(beastReq));

            // 5. 中间件 + 路由分发
            HttpResponse res;
            if (middlewarePipeline_.size() > 0) {
                res = co_await middlewarePipeline_.execute(req, [this, &req](...) {
                    co_return co_await router_.dispatch(req);
                });
            } else {
                res = co_await router_.dispatch(req);
            }

            // 6. 设置通用响应头
            res.native().version(11);
            res.native().set(http::field::server, "hical/0.2.0");
            res.native().keep_alive(req.native().keep_alive());
            res.native().prepare_payload();

            // 7. 发送响应
            co_await http::async_write(socket, res.native(), use_awaitable);

            // 8. Keep-Alive 检查
            if (!res.native().keep_alive()) break;
        }
    } catch (const beast::system_error& e) {
        if (e.code() == http::error::body_limit) {
            // 请求体过大 → 413 Payload Too Large
            http::response<http::string_body> res{http::status::payload_too_large, 11};
            res.body() = "Request body too large";
            res.prepare_payload();
            http::write(socket, res, writeEc);
        }
    }
    // SocketGuard 析构 → socket 自动关闭
}
```

### 4.1 完整请求处理流程图

```
客户端连接
    │
    ▼
acceptLoop: co_await acceptor.async_accept()
    │
    ▼ co_spawn 启动 handleSession 协程
    │
┌───▼──── handleSession ──────────────────────────────────────┐
│                                                              │
│  创建 SocketGuard（RAII 保证关闭）                           │
│  创建请求级 PMR 单调池                                       │
│                                                              │
│  for (;;) {   ← Keep-Alive 循环                             │
│      │                                                       │
│      ├── async_read（解析 HTTP 请求，带 body/header 限制）    │
│      │                                                       │
│      ├── WebSocket 升级检查？                                │
│      │   ├── 是 → handleWebSocket() → co_return              │
│      │   └── 否 → 继续                                      │
│      │                                                       │
│      ├── 封装 HttpRequest                                    │
│      │                                                       │
│      ├── middlewarePipeline.execute()                         │
│      │   └── router.dispatch()                               │
│      │       ├── 静态路由哈希查找 O(1)                       │
│      │       ├── 参数路由线性匹配 O(N)                       │
│      │       └── 404 Not Found                               │
│      │                                                       │
│      ├── 设置响应头（Server, Keep-Alive）                    │
│      │                                                       │
│      ├── async_write（发送响应）                              │
│      │                                                       │
│      └── keep_alive? → 是: 继续循环 / 否: break             │
│  }                                                           │
│                                                              │
│  SocketGuard 析构 → socket 关闭                              │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 安全防护

| 防护             | 实现                                    | 防御目标                         |
| ---------------- | --------------------------------------- | -------------------------------- |
| **body_limit**   | `parser.body_limit(1MB)`                | 超大请求体 OOM 攻击              |
| **header_limit** | `parser.header_limit(8KB)`              | 超大请求头 OOM 攻击              |
| **路径深度限制** | Router 中 `hMaxPathSegments = 32`       | 超深路径 CPU 消耗                |
| **参数长度限制** | Router 中 `hMaxParamValueLength = 1024` | 超长参数内存消耗                 |
| **413 响应**     | body_limit 异常时返回                   | 告知客户端请求太大               |
| **SocketGuard**  | RAII 析构关闭 socket                    | 防止任何异常路径导致 socket 泄漏 |

### 4.3 Keep-Alive 支持

```cpp
// 请求端设置
res.native().keep_alive(req.native().keep_alive());

// 循环结束条件
if (!res.native().keep_alive()) break;
```

Keep-Alive 允许在一个 TCP 连接上发送多个 HTTP 请求，避免每次请求都重新建连。框架通过 `for(;;)` 循环处理同一连接上的多个请求，直到客户端请求 `Connection: close`。

### 4.4 PMR 在 handleSession 中的应用

```cpp
auto requestPool = MemoryPool::instance().createRequestPool();
std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());
beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);
```

Beast 的 `flat_buffer` 支持自定义分配器。这里使用请求级 PMR 单调池，使得 HTTP 解析过程中的所有中间缓冲区都从单调池分配。连接关闭时 `requestPool` 析构，整块内存一次性归还。

---

## 五、handleWebSocket — WebSocket 会话

```cpp
Awaitable<void> HttpServer::handleWebSocket(
    tcp::socket socket,
    http::request<http::string_body> req,
    const Router::WsRoute& wsRoute)
{
    try {
        // 1. 创建 WebSocket 流并接受升级
        ws::stream<tcp::socket> wsStream(std::move(socket));
        co_await wsStream.async_accept(req, use_awaitable);

        // 2. 创建 WebSocketSession
        WebSocketSession session(std::move(wsStream));

        // 3. 触发连接回调
        if (wsRoute.onConnect) {
            co_await wsRoute.onConnect(session);
        }

        // 4. 消息循环
        while (session.isOpen()) {
            auto msg = co_await session.receive();
            if (!msg.has_value()) break;            // 连接关闭

            if (wsRoute.onMessage) {
                co_await wsRoute.onMessage(*msg, session);
            }
        }
    } catch (const beast::system_error& e) {
        // 忽略正常关闭
    }
}
```

注意 `handleWebSocket` 会 **接管 socket**：一旦升级为 WebSocket，HTTP 会话协程 `co_return`，控制权转移到 WebSocket 循环。

---

## 六、从测试看完整用法

### 6.1 HttpServer 端到端测试

**源码位置**：`tests/test_http_server.cpp`

| 测试           | 验证点                                          |
| -------------- | ----------------------------------------------- |
| `StartAndStop` | 启动/停止生命周期正确                           |
| `GetRequest`   | GET 返回 200 + 正确 body                        |
| `PostRequest`  | POST echo body 正确                             |
| `NotFound`     | 未注册路由返回 404                              |
| `PathParam`    | `/users/42` → param("id")="42"                  |
| `Middleware`   | 中间件添加的 X-Powered-By 头出现在响应中        |
| `JsonResponse` | Content-Type=application/json，JSON body 可解析 |

### 6.2 集成测试

**源码位置**：`tests/test_integration.cpp`

**A. 网络边界场景**：

| 测试                        | 验证点                                   |
| --------------------------- | ---------------------------------------- |
| `LargeBody`                 | 1MB 请求体完整传输和回显                 |
| `HalfClose`                 | 客户端 shutdown(send) 后仍能收到响应     |
| `KeepAlive`                 | 同一连接 3 个请求，全部成功              |
| `EmptyBodyPost`             | 空 body POST 不崩溃                      |
| `NotFoundRoute`             | 未注册路由返回 404                       |
| **`ConcurrentConnections`** | **20 客户端 × 10 请求 = 200 次全部成功** |

**B. 并发安全**：

| 测试                       | 验证点                                 |
| -------------------------- | -------------------------------------- |
| `ConcurrentRouterDispatch` | 8 线程 × 1000 次路由分发，全部结果正确 |
| `ConcurrentMiddleware`     | 4 线程 × 500 次中间件执行，计数器准确  |

**C. 内存安全**：

| 测试                    | 验证点                                     |
| ----------------------- | ------------------------------------------ |
| `RequestPoolNoLeak`     | 10000 次请求池创建/销毁，当前分配 < 1MB    |
| `PmrBufferNoLeak`       | 5000 次 PmrBuffer 扩容/回收，无崩溃        |
| `MultiThreadPoolNoLeak` | 8 线程 × 5000 次分配/释放，当前分配 < 10MB |

**ConcurrentConnections 测试特别重要**：

```cpp
TEST_F(IntegrationTest, ConcurrentConnections) {
    // 20 个客户端线程，每个发 10 个 Keep-Alive 请求
    for (int c = 0; c < 20; ++c) {
        threads.emplace_back([&]() {
            tcp::socket sock(io);
            sock.connect(...);
            for (int r = 0; r < 10; ++r) {
                http::write(sock, req);
                http::read(sock, buffer, res);
                if (res.result_int() == 200 && res.body() == "hello")
                    successCount++;
            }
        });
    }
    EXPECT_EQ(successCount.load(), 200);  // 全部成功
}
```

---

## 七、examples/reflection_server.cpp — 反射路由示例

**源码位置**：`examples/reflection_server.cpp`

这是展示 C++26 反射层（C++20 回退模式）的完整示例，核心区别是**路由自动注册**和**JSON 自动序列化**：

```cpp
// DTO 结构体 —— 用 HICAL_JSON 标注，自动序列化/反序列化
struct UserDTO {
    std::string name;
    int age;
    std::string email;
    HICAL_JSON(UserDTO, name, age, email)
};

// Handler —— 用 HICAL_HANDLER/HICAL_ROUTES 标注，自动注册路由
struct ApiHandler {
    HttpResponse getStatus(const HttpRequest&) { ... }
    HICAL_HANDLER(Get, "/api/status", getStatus)

    HttpResponse createUser(const HttpRequest& req) {
        auto user = req.readJson<UserDTO>();     // 一行反序列化
        return HttpResponse::json(meta::toJson(user));  // 一行序列化
    }
    HICAL_HANDLER(Post, "/api/users", createUser)

    HICAL_ROUTES(ApiHandler, getStatus, ...)
};

// 注册：一行搞定所有路由
ApiHandler handler;
meta::registerRoutes(server.router(), handler);
```

与 `http_server.cpp` 的区别：
- 路由注册从**逐条手动**变为**一行自动**
- JSON 处理从**手动构建**变为**DTO 自动映射**
- Handler 生命周期通过 `shared_ptr` 安全管理

---

## 八、examples/http_server.cpp 示例分析

**源码位置**：`examples/http_server.cpp`

这是面向用户的完整示例，展示了框架的所有核心功能：

```cpp
HttpServer server(port);

// 1. 中间件
server.use(logMiddleware);

// 2. HTTP 路由
server.router().get("/", ...);                    // 首页
server.router().get("/api/status", ...);          // JSON 状态
server.router().post("/api/echo", ...);           // Echo
server.router().get("/api/hello", ...);           // 查询参数
server.router().get("/users/{id}", ...);          // 路径参数

// 3. WebSocket 路由
server.router().ws("/ws/echo", onMessage, onConnect);

// 4. 启动（阻塞）
server.start();
```

**对应的 curl 测试命令**：

```bash
curl http://localhost:8080/                       # → "Welcome to hical!"
curl http://localhost:8080/api/status              # → {"status":"running",...}
curl -X POST -d '{"msg":"hi"}' http://localhost:8080/api/echo  # → {"msg":"hi"}
curl http://localhost:8080/users/42                # → {"userId":"42","name":"User 42"}
```

---

## 九、从第0课到第8课的全链路回顾

```
C++20 特性（第0课）
    │
    ├── Concepts → 后端约束（第1课）
    ├── Coroutines → 所有异步操作（贯穿始终）
    ├── PMR → 三级内存池（第4课）
    └── if constexpr → TCP/SSL 统一（第5课）
         │
抽象接口（第1课）                    错误/地址（第2课）
    EventLoop / Timer / TcpConnection   ErrorCode / InetAddress / HttpTypes
         │                                  │
Asio 实现（第3课）                         │
    AsioEventLoop / AsioTimer / EventLoopPool
         │                                  │
内存管理（第4课）                           │
    MemoryPool / TrackedResource / PmrBuffer
         │                                  │
TCP 连接与服务器（第5课）                   │
    GenericConnection / TcpServer / SslContext
         │                                  │
HTTP 协议与路由（第6课）  ←─────────────────┘
    HttpRequest / HttpResponse / Router
         │
中间件与 WebSocket（第7课）
    MiddlewarePipeline / WebSocketSession
         │
HttpServer 整合（第8课） ← 你在这里
    acceptLoop → handleSession → middleware → router → response
         │
C++26 反射层（第9课）
    Reflection.h / MetaJson.h / MetaRoutes.h
    自动 JSON 序列化 + 自动路由注册
```

---

## 十、关键问题思考与回答

**Q1: HttpServer 的 start() 是阻塞的，框架内部如何处理并发请求？**

> `start()` 内部调用 `ioContext_.run()`，它在处理事件时是阻塞的，但**不是串行的**：
> 1. **多线程**：`ioThreads_` 个线程同时调用 `ioContext_.run()`，共享一个 io_context
> 2. **协程并发**：每个连接的 `handleSession` 是独立协程，`co_await` 挂起时让出 CPU，其他协程可以执行
> 3. 效果：即使单线程，多个协程也能交替执行（协作式并发）

**Q2: HTTP 请求和 WebSocket 升级请求是在哪里分流的？**

> 在 `handleSession` 协程中，HTTP 请求解析完成后：
> ```cpp
> if (ws::is_upgrade(beastReq)) {
>     auto* wsRoute = router_.findWsRoute(reqPath);
>     if (wsRoute) {
>         co_await handleWebSocket(std::move(socket), ...);
>         co_return;  // HTTP 会话结束
>     }
> }
> // 否则走正常 HTTP 路由
> ```
> 分流发生在路由匹配之前。WebSocket 请求的 HTTP 头部包含 `Upgrade: websocket`，Beast 的 `is_upgrade()` 检查这个标记。

**Q3: setMaxBodySize 和 setMaxHeaderSize 解决什么安全问题？**

> 防止 **OOM 攻击**：
> - 恶意客户端发送 10GB 的请求体 → 不限制会把服务器内存撑爆
> - `body_limit(1MB)` 让 Beast 在读取超过 1MB 时抛 `body_limit` 异常
> - 服务器捕获异常后返回 413 Payload Too Large，而不是崩溃
>
> 类似于游戏服务器中的"消息包最大长度限制"。

**Q4: 如果想支持 HTTP/2，架构上需要改动哪些部分？**

> 1. **handleSession 协程**：HTTP/2 支持多路复用（一个连接上多个并行请求流），需要用 HTTP/2 协议栈替换当前的 for 循环
> 2. **Beast 依赖**：Boost.Beast 主要支持 HTTP/1.1，HTTP/2 需要引入 nghttp2 等第三方库
> 3. **Router/Middleware**：不需要改动，它们只关心 HttpRequest/HttpResponse 抽象
> 4. **TLS**：HTTP/2 通常要求 TLS（h2），ALPN 协商需要在 SslContext 中配置
> 5. 核心思路：只改协议解析层（handleSession），上层接口不变

---

## 十一、与游戏服务器架构的对比

| Hical 概念                      | 游戏服务器等价物                               |
| ------------------------------- | ---------------------------------------------- |
| `HttpServer::start()`           | 服务器启动入口（阻塞主线程）                   |
| `handleSession`                 | 客户端连接处理协程                             |
| Keep-Alive 循环                 | 游戏客户端的长连接消息循环                     |
| `middlewarePipeline.execute()`  | 消息预处理链（解密→验签→解压→分发）            |
| `router_.dispatch()`            | 消息 ID → 处理函数分发                         |
| `maxBodySize` / `maxHeaderSize` | 消息包最大长度限制                             |
| SocketGuard RAII                | 连接异常时自动清理会话                         |
| 413 Payload Too Large           | 超大消息包直接丢弃并断开                       |
| WebSocket 升级                  | 协议切换（如从 HTTP 管理口切到游戏二进制协议） |
| `examples/http_server.cpp`      | 游戏服务器的 main 函数                         |

---

## 十二、课程总结

至此，Hical 框架的 8 课核心内容全部完成。回顾知识脉络：

| 课程  | 核心收获                                                                   |
| ----- | -------------------------------------------------------------------------- |
| 第0课 | C++20 四大特性（Concepts/Coroutines/PMR/if constexpr）在框架中的应用       |
| 第1课 | 两层架构（core 抽象 + asio 实现）、三大接口、NetworkBackend Concept        |
| 第2课 | 统一错误码、跨平台地址封装、HTTP 类型枚举                                  |
| 第3课 | io_context 封装、work_guard、dispatch vs post、EventLoopPool round-robin   |
| 第4课 | 三级 PMR 内存池、TrackedResource CAS 统计、PmrBuffer 读写模型              |
| 第5课 | GenericConnection 模板、连接状态机、TcpServer accept 循环、SSL 集成        |
| 第6课 | HttpRequest/Response 封装、Router 双策略（哈希 O(1) + 参数线性）、安全限制 |
| 第7课 | 洋葱模型中间件、MiddlewareNext 链式调用、WebSocket 协程化接口              |
| 第8课 | HttpServer 整合、完整请求生命周期、Keep-Alive、安全防护、并发测试          |

**接下来进入第9课 Cookie、Session 与文件服务**，学习 Web 应用"最后一公里"的四个模块。

**之后进入综合项目阶段**：
- **项目 A**：性能压测与分析（http_benchmark + pmr_benchmark）
- **项目 B**：动手扩展新功能（Rate Limiter / 带鉴权的文件管理系统 / JSON RPC）

---

*下一课：[第9课 - Cookie、Session 与文件服务]({{< relref "Hical-09-notes" >}})，将学习 Cookie 解析/设置、Session 会话管理、静态文件安全托管、Multipart 文件上传。*
