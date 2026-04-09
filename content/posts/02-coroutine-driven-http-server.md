+++
title = '告别回调地狱：在 C++ Web 框架中全面拥抱协程'
date = '2026-05-02'
draft = false
tags = ["C++20", "协程", "Boost.Asio", "异步编程", "Hical"]
categories = ["Hical框架"]
description = "以 Hical 框架为例，展示如何用 C++20 协程 + Boost.Asio 构建一个全协程化的 HTTP 服务器，以及工程权衡。"
+++

# 告别回调地狱：在 C++ Web 框架中全面拥抱协程

> 本文以 Hical 框架为例，展示如何用 C++20 协程 + Boost.Asio 构建一个全协程化的 HTTP 服务器，以及这样做的工程权衡。

---

## 回调有什么问题？

几乎所有 C++ 网络框架的 1.0 版本都是回调驱动的。一个简单的"读取请求 → 处理 → 发送响应"流程，回调版本长这样：

```cpp
void onAccept(tcp::socket socket)
{
    auto buf = std::make_shared<flat_buffer>();
    auto req = std::make_shared<http::request<string_body>>();

    http::async_read(socket, *buf, *req,
        [&socket, buf, req](error_code ec, size_t) {
            if (ec) return;
            auto res = std::make_shared<http::response<string_body>>();
            // ... 处理请求，构建响应 ...
            http::async_write(socket, *res,
                [&socket, res](error_code ec, size_t) {
                    if (ec) return;
                    socket.shutdown(tcp::socket::shutdown_send);
                });
        });
}
```

问题很明显：
1. **嵌套层级**：每一步异步操作都多一层 Lambda
2. **生命周期管理**：`shared_ptr` 满天飞，只是为了确保 buffer/request/response 在回调执行时还活着
3. **错误处理分散**：每个回调都需要检查 `error_code`，遗漏一个就是 bug

## 协程版本：像写同步代码一样

相同的逻辑，Hical 的协程版本：

```cpp
Awaitable<void> handleSession(tcp::socket socket)
{
    flat_buffer buffer;
    for (;;)
    {
        auto req = co_await http::async_read(socket, buffer, use_awaitable);
        auto res = processRequest(req);
        co_await http::async_write(socket, res, use_awaitable);
        if (!res.keep_alive()) break;
    }
}
```

从上到下、线性执行、自然的 for 循环处理 Keep-Alive——读起来和同步代码一样，但实际上每个 `co_await` 都是非阻塞的。

## Hical 的全协程化架构

Hical 的所有异步路径都使用协程，没有单一回调：

```
acceptLoop()          ← 协程：接受连接
    │
    └── handleSession()   ← 协程：HTTP 请求生命周期
            │
            ├── middleware.execute()  ← 协程：中间件链
            ├── router.dispatch()    ← 协程：路由分发
            └── handleWebSocket()    ← 协程：WebSocket 会话
```

### 为什么不做回调和协程的混合？

Drogon 等框架提供了回调和协程两种 API。Hical 选择全协程化，理由是：

| 维度       | 全协程                                   | 混合模式                       |
| ---------- | ---------------------------------------- | ------------------------------ |
| API 一致性 | 只有一种异步模式，无心智负担             | 两套 API，用户需要选择         |
| 中间件     | `co_await next(req)` 自然支持前/后置逻辑 | 回调模式的后置逻辑需要额外机制 |
| 错误处理   | `try/catch` 统一                         | 回调和协程各一套错误处理       |
| 代价       | 要求 C++20                               | 兼容 C++17                     |

### 关键设计：Awaitable<T> 只是类型别名

```cpp
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;
```

Hical 没有自研协程框架，直接复用 Boost.Asio 的 `awaitable<T>`。原因：
- Asio 的协程与 `io_context` 调度器深度集成，自研反而会失去兼容性
- Asio 的 Promise Type 已经处理了所有边界情况（异常传播、取消等）
- 一个类型别名就够了，不需要增加复杂度

### 洋葱模型中间件：协程的杀手级应用

回调模式下实现"请求前置 + 响应后置"逻辑非常困难。协程让洋葱模型变得自然：

```cpp
server.use([](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    // ──── 前置逻辑（请求进入） ────
    auto start = std::chrono::steady_clock::now();

    auto res = co_await next(req);  // 调用下一层（可能是另一个中间件或路由处理器）

    // ──── 后置逻辑（响应返回） ────
    auto elapsed = std::chrono::steady_clock::now() - start;
    res.setHeader("X-Response-Time", std::to_string(elapsed.count()));

    co_return res;
});
```

`co_await next(req)` 这一行完成了三件事：
1. 暂停当前中间件的执行
2. 将控制权传递给下一层
3. 下一层（以及更深层）执行完毕后，从暂停点恢复

这在回调模式下需要复杂的链式构建。

### 路由处理器：同步和协程统一

用户可以写同步或协程两种处理器，框架统一为协程：

```cpp
// 同步（简单场景）
router.get("/api/ping", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::ok("pong");
});

// 协程（需要异步操作）
router.get("/api/data", [](const HttpRequest&) -> Awaitable<HttpResponse> {
    co_await hical::sleep(0.01);  // 模拟异步 DB 查询
    co_return HttpResponse::json({{"data", "value"}});
});
```

内部实现：同步处理器被包装为协程：

```cpp
void Router::route(HttpMethod method, const std::string& path, SyncRouteHandler handler)
{
    auto asyncHandler = [h = std::move(handler)](const HttpRequest& req) -> Awaitable<HttpResponse> {
        co_return h(req);
    };
    route(method, path, std::move(asyncHandler));
}
```

## 协程的生命周期管理

协程最容易踩的坑是**对象生命周期**。协程可能在 `co_await` 处挂起很久，期间栈上的局部变量随时可能被销毁。

### 连接级：shared_from_this

```cpp
void GenericConnection::startRead()
{
    auto self = sharedThis();  // 持有 shared_ptr
    boost::asio::co_spawn(executor,
        [self]() -> awaitable<void> {
            co_await self->readLoop();  // 协程可能运行数小时
        }, detached);
}
```

`shared_from_this()` 确保协程执行期间连接对象不被销毁。

### 请求级：RAII 守卫

```cpp
Awaitable<void> handleSession(tcp::socket socket)
{
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

    // ... 协程逻辑 ...
    // 无论正常退出还是异常，SocketGuard 析构都会关闭 socket
}
```

### Handler 级：shared_ptr 管理

反射路由中的 handler 通过 `shared_ptr` 捕获，避免悬挂引用：

```cpp
template <typename Handler>
void registerOneRoute(Router& router, std::shared_ptr<Handler> pHandler,
                      const RouteInfo& info, HttpResponse (Handler::*fn)(const HttpRequest&))
{
    router.route(info.method, std::string(info.path),
                 [pHandler, fn](const HttpRequest& req) -> HttpResponse {
                     return (pHandler.get()->*fn)(req);
                 });
}
```

## 协程的性能代价

协程不是零成本的：每个协程需要一个协程帧（coroutine frame），大小通常在 200-500 字节。在极高并发下：

```
100K 并发连接 × 500 bytes/协程帧 ≈ 50MB
```

对于现代服务器来说可以忽略。真正的性能瓶颈永远在 I/O 和业务逻辑上，不在协程调度上。

## 总结

全协程化的核心价值不是性能（协程和回调性能接近），而是**代码质量**：
- 线性逻辑替代嵌套回调 → 更少 bug
- `try/catch` 统一错误处理 → 不会遗漏
- 洋葱模型中间件自然表达 → 更好的架构

代价是要求 C++20，但在 2026 年，这已经不是障碍。

---

> 源码参考：[Hical/src/core/HttpServer.cpp](https://github.com/Hical61/Hical/blob/main/src/core/HttpServer.cpp)
> 项目地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
