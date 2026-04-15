+++
title = '第7课：中间件与 WebSocket'
date = '2026-04-15'
draft = false
tags = ["C++", "中间件", "WebSocket", "洋葱模型", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "理解洋葱模型中间件管道的执行顺序，掌握 MiddlewareNext 链式调用机制，以及 WebSocket 会话的协程化接口。"
+++

# 第7课：中间件与 WebSocket - 学习笔记

> 理解洋葱模型（Onion Model）中间件管道的执行顺序，掌握 MiddlewareNext 链式调用机制，以及 WebSocket 会话的协程化接口。

---

## 一、中间件管道 — 洋葱模型

### 1.1 核心类型定义

**源码位置**：`src/core/Middleware.h`

```cpp
// next 回调：调用下一个中间件或最终处理器
using MiddlewareNext = std::function<Awaitable<HttpResponse>(const HttpRequest&)>;

// 中间件：接收请求和 next，返回响应
using MiddlewareHandler = std::function<Awaitable<HttpResponse>(const HttpRequest&, MiddlewareNext)>;
```

中间件的签名核心在于 **MiddlewareNext** 参数——调用它就进入下一层，不调用就拦截请求。

### 1.2 洋葱模型执行顺序

注册顺序：`A → B → C`

```
请求到达
    │
    ▼
┌─── Middleware A ───────────────────────┐
│   A.pre  (order.push_back(1))         │
│   ┌─── Middleware B ──────────────┐    │
│   │   B.pre  (order.push_back(2))│    │
│   │   ┌─── Handler ─────────┐   │    │
│   │   │   处理请求            │   │    │
│   │   │   返回响应            │   │    │
│   │   └──────────────────────┘   │    │
│   │   B.post (order.push_back(3))│    │
│   └──────────────────────────────┘    │
│   A.post (order.push_back(4))         │
└───────────────────────────────────────┘
    │
    ▼
响应返回

执行顺序：1 → 2 → handler → 3 → 4
```

**像剥洋葱**：请求从外到内穿过所有中间件到达处理器，响应再从内到外原路返回。每个中间件在 `co_await next(req)` **之前**执行前置逻辑，**之后**执行后置逻辑。

### 1.3 中间件代码示例

```cpp
// 日志中间件
auto logger = [](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    std::cout << "[请求] " << req.path() << std::endl;      // 前置：记录请求
    auto res = co_await next(req);                            // 进入下一层
    std::cout << "[响应] " << res.statusCode() << std::endl;  // 后置：记录响应
    co_return res;
};

// 鉴权中间件（可能拦截）
auto auth = [](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    if (req.header("Authorization").empty()) {
        co_return HttpResponse::badRequest("Unauthorized");   // 拦截！不调用 next
    }
    co_return co_await next(req);                              // 通过，继续
};
```

### 1.4 execute — 调用链构建

**源码位置**：`src/core/Middleware.cpp`

```cpp
Awaitable<HttpResponse> MiddlewarePipeline::execute(
    const HttpRequest& req, MiddlewareNext finalHandler)
{
    if (middlewares_.empty()) {
        co_return co_await finalHandler(req);   // 无中间件，直接执行处理器
    }

    // 从最内层向外层构建调用链
    MiddlewareNext current = std::move(finalHandler);

    for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i) {
        auto mw = middlewares_[i];   // 值拷贝，避免 vector 扩容后引用悬空
        current = [mw, next = std::move(current)](const HttpRequest& r) -> Awaitable<HttpResponse> {
            co_return co_await mw(r, next);
        };
    }

    co_return co_await current(req);
}
```

**逆序构建调用链的原理**：

假设注册了 A、B 两个中间件，finalHandler 为 H：

```
初始：current = H

i=1（B）：current = λ(req) { co_await B(req, H) }
i=0（A）：current = λ(req) { co_await A(req, λ(req) { co_await B(req, H) }) }

调用 current(req)：
→ A(req, nextA)     其中 nextA = λ(req) { B(req, H) }
  → A.pre
  → co_await nextA(req)
    → B(req, nextB)   其中 nextB = H
      → B.pre
      → co_await nextB(req)
        → H(req) → 返回 response
      → B.post
    → 返回
  → A.post
→ 返回最终 response
```

**为什么逆序？** 因为最先注册的中间件应该在最外层（最先执行前置、最后执行后置）。逆序构建让最先注册的中间件包裹在最外面。

**值拷贝 `auto mw = middlewares_[i]`**：Lambda 捕获 `mw` 的拷贝而不是引用。如果捕获引用，当 `middlewares_` vector 扩容或被修改时，引用会悬空。

### 1.5 拦截机制

中间件可以选择**不调用 next**，直接返回响应：

```cpp
pipeline.use([](const HttpRequest&, MiddlewareNext) -> Awaitable<HttpResponse> {
    co_return HttpResponse(HttpStatusCode::hForbidden, "Forbidden");
    // 不调用 next → 后续中间件和处理器都不会执行
});
```

这在鉴权、限流、IP 黑名单等场景中非常有用。

---

## 二、WebSocket 会话

### 2.1 整体结构

**源码位置**：`src/core/WebSocket.h` / `src/core/WebSocket.cpp`

```cpp
class WebSocketSession {
    using WsStream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

    WsStream stream_;    // Beast WebSocket 流
    bool open_{true};    // 连接状态
};
```

WebSocketSession 对 Beast 的 `websocket::stream` 做了简洁的协程化封装。

### 2.2 send — 发送消息

```cpp
Awaitable<void> WebSocketSession::send(const std::string& msg) {
    co_await stream_.async_write(boost::asio::buffer(msg), boost::asio::use_awaitable);
}
```

一行代码完成异步发送。`co_await` 挂起直到数据写入完成。

### 2.3 receive — 接收消息

```cpp
Awaitable<std::optional<std::string>> WebSocketSession::receive() {
    beast::flat_buffer buffer;
    try {
        co_await stream_.async_read(buffer, boost::asio::use_awaitable);
        co_return beast::buffers_to_string(buffer.data());
    } catch (const beast::system_error& e) {
        open_ = false;
        if (e.code() == beast::websocket::error::closed ||
            e.code() == boost::asio::error::eof) {
            co_return std::nullopt;     // 正常关闭 → 返回空
        }
        throw;                           // 异常错误 → 继续抛出
    }
}
```

**返回 `std::optional<std::string>` 的设计意图**：

| 返回值         | 含义             |
| -------------- | ---------------- |
| `"Hello"`      | 收到一条消息     |
| `std::nullopt` | 对端正常关闭连接 |
| 抛异常         | 网络错误         |

用 `optional` 区分"正常关闭"和"收到消息"，让调用者可以用简洁的循环处理：

```cpp
while (auto msg = co_await session.receive()) {
    // 处理 *msg
}
// 循环退出 = 连接关闭
```

### 2.4 close — 关闭连接

```cpp
void WebSocketSession::close() {
    if (open_) {
        open_ = false;
        boost::system::error_code ec;
        stream_.close(beast::websocket::close_code::normal, ec);  // 发送 Close 帧
    }
}
```

WebSocket 的关闭需要发送 Close 帧（Close Frame），这是协议规定的优雅关闭流程。`close_code::normal` (1000) 表示正常关闭。

### 2.5 isOpen — 状态查询

```cpp
bool WebSocketSession::isOpen() const {
    return open_ && stream_.is_open();  // 自己的标记 AND 底层 socket 状态
}
```

双重检查：`open_` 是自己维护的标记（receive 异常时设为 false），`stream_.is_open()` 是 Beast 的底层状态。

---

## 三、WebSocket 路由与生命周期

### 3.1 路由注册

```cpp
// 在 Router 中注册 WebSocket 路由
server.router().ws("/ws/echo",
    // onMessage：收到消息时
    [](const std::string& msg, WebSocketSession& session) -> Awaitable<void> {
        co_await session.send("Echo: " + msg);
    },
    // onConnect：连接建立时（可选）
    [](WebSocketSession& session) -> Awaitable<void> {
        co_await session.send("Welcome!");
    }
);
```

### 3.2 WebSocket 升级流程

```
客户端发送 HTTP 请求（带 Upgrade: websocket 头）
    │
    ▼
HttpServer 收到请求
    │
    ├── 检查 ws::is_upgrade(req) → 是 WebSocket 升级请求？
    │       │
    │       ├── 是 → router.findWsRoute(path)
    │       │         │
    │       │         ├── 找到 → 执行 WebSocket 升级
    │       │         │         stream.async_accept(req)
    │       │         │         调用 onConnect (如果有)
    │       │         │         进入收发循环
    │       │         │
    │       │         └── 未找到 → 当普通 HTTP 处理 → 404
    │       │
    │       └── 否 → 走正常 HTTP 路由分发
```

### 3.3 完整的 WebSocket 服务端会话

```cpp
// HttpServer 内部的 WebSocket 会话处理（伪代码）
if (ws::is_upgrade(req)) {
    auto* wsRoute = router.findWsRoute(path);
    if (wsRoute) {
        // 升级为 WebSocket
        WsStream wsStream(std::move(socket));
        co_await wsStream.async_accept(req, use_awaitable);

        WebSocketSession session(std::move(wsStream));

        // 触发连接回调
        if (wsRoute->onConnect) {
            co_await wsRoute->onConnect(session);
        }

        // 收发循环
        while (auto msg = co_await session.receive()) {
            co_await wsRoute->onMessage(*msg, session);
        }
        // 循环退出 = 连接关闭
    }
}
```

---

## 四、从测试看完整用法

### 4.1 中间件测试

**源码位置**：`tests/test_middleware.cpp`

| 测试                 | 验证点                                    |
| -------------------- | ----------------------------------------- |
| `EmptyPipeline`      | 无中间件时直接执行 finalHandler           |
| `SingleMiddleware`   | 单个中间件可在后置阶段添加 Header         |
| **`ExecutionOrder`** | **洋葱模型顺序：1 → 2 → handler → 3 → 4** |
| `Intercept`          | 拦截中间件不调用 next，handler 不执行     |
| `ModifyRequest`      | 中间件在后置阶段修改响应                  |

**ExecutionOrder 测试最为核心**：

```cpp
pipeline.use([&order](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    order.push_back(1);                    // A.pre
    auto res = co_await next(req);
    order.push_back(4);                    // A.post
    co_return res;
});

pipeline.use([&order](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    order.push_back(2);                    // B.pre
    auto res = co_await next(req);
    order.push_back(3);                    // B.post
    co_return res;
});

// 执行后 order = {1, 2, 3, 4}
// 证明：A.pre → B.pre → handler → B.post → A.post
```

**Intercept 测试**：

```cpp
pipeline.use([](const HttpRequest&, MiddlewareNext) -> Awaitable<HttpResponse> {
    co_return HttpResponse(403, "Forbidden");   // 不调用 next！
});

// 结果：handler 的 handlerCalled 标记仍为 false
// 证明中间件可以完全拦截请求
```

### 4.2 WebSocket 测试

**源码位置**：`tests/test_websocket.cpp`

| 测试                          | 验证点                                              |
| ----------------------------- | --------------------------------------------------- |
| `EchoMessage`                 | 发送 "Hello WS" → 收到 "Echo: Hello WS"             |
| `ConnectCallback`             | 连接后立即收到 "Welcome!"，再发消息收到 "Got: test" |
| `UnregisteredPathFallsToHttp` | 非 WS 路径的请求返回 404                            |

**EchoMessage 测试的完整流程**：

```
1. 启动 HttpServer，注册 ws("/ws/echo", handler)
2. 客户端 TCP 连接
3. 客户端 WebSocket 握手 → wsClient.handshake("127.0.0.1", "/ws/echo")
4. 客户端发送 "Hello WS" → wsClient.write(buffer("Hello WS"))
5. 服务端 handler 收到 msg="Hello WS"
6. 服务端 session.send("Echo: Hello WS")
7. 客户端接收 → "Echo: Hello WS" ✓
8. 客户端关闭 → wsClient.close(normal)
```

**ConnectCallback 测试验证了两阶段回调**：

```
连接建立 → onConnect 回调 → session.send("Welcome!")
    │
    ▼ 客户端收到 "Welcome!"
    │
客户端发送 "test" → onMessage 回调 → session.send("Got: test")
    │
    ▼ 客户端收到 "Got: test"
```

---

## 五、中间件的典型应用场景

### 5.1 日志中间件

```cpp
pipeline.use([](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    auto start = std::chrono::steady_clock::now();

    auto res = co_await next(req);

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cout << req.method() << " " << req.path()
              << " → " << res.statusCode() << " (" << ms << "ms)" << std::endl;

    co_return res;
});
```

### 5.2 鉴权中间件

```cpp
pipeline.use([](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    auto token = req.header("Authorization");
    if (token.empty() || !validateToken(token)) {
        co_return HttpResponse::badRequest("Unauthorized");  // 拦截
    }
    co_return co_await next(req);                             // 放行
});
```

### 5.3 CORS 中间件

```cpp
pipeline.use([](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    auto res = co_await next(req);
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE");
    res.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    co_return res;
});
```

### 5.4 异常捕获中间件

```cpp
pipeline.use([](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    try {
        co_return co_await next(req);
    } catch (const std::exception& e) {
        co_return HttpResponse::serverError();  // 500，不暴露内部错误
    }
});
```

---

## 六、设计模式总结

| 模式                  | 应用                                | 说明                                     |
| --------------------- | ----------------------------------- | ---------------------------------------- |
| **责任链 / 洋葱模型** | MiddlewarePipeline                  | 请求依次穿过多层中间件，每层可拦截或放行 |
| **函数式组合**        | execute 的 Lambda 嵌套              | 逆序构建 `f(g(h(x)))` 调用链             |
| **optional 语义**     | `receive()` 返回 `optional<string>` | 区分"数据到达"与"连接关闭"               |
| **协议升级**          | HTTP → WebSocket                    | 同一端口同时支持 HTTP 和 WebSocket       |
| **回调分离**          | onConnect + onMessage               | 连接建立与消息处理分离                   |

---

## 七、关键问题思考与回答

**Q1: 洋葱模型中，中间件 A → B → C 的 pre/post 执行顺序是什么？**

> 前置（pre）按注册顺序：A → B → C
> 后置（post）按逆序：C → B → A
>
> 完整顺序：A.pre → B.pre → C.pre → handler → C.post → B.post → A.post
>
> 这就像剥洋葱：从外到内穿过各层到达核心（handler），再从内到外原路返回。

**Q2: 中间件如何实现「提前返回」（不调用 next）？**

> 直接 `co_return` 一个 HttpResponse 即可。不调用 `next()` 意味着后续中间件和最终处理器都不会执行。
>
> ```cpp
> [](const HttpRequest&, MiddlewareNext) -> Awaitable<HttpResponse> {
>     co_return HttpResponse::badRequest("Blocked");  // 不调用 next = 拦截
> };
> ```
>
> 这在鉴权失败、限流触发、IP 黑名单等场景中使用。

**Q3: WebSocket 的 receive() 返回 std::optional 的设计意图是什么？**

> 区分三种情况：
> - `optional("message")` → 正常收到消息
> - `nullopt` → 对端正常关闭连接（Close 帧或 EOF）
> - 抛异常 → 网络错误
>
> 这允许用简洁的循环处理消息：
> ```cpp
> while (auto msg = co_await session.receive()) {
>     // 处理 *msg
> }
> // 退出循环 = 连接关闭
> ```

**Q4: HTTP 到 WebSocket 的升级过程发生在哪一层？**

> 发生在 **HttpServer 层**（第8课会详细讲）。流程：
> 1. HttpServer 的 session 协程收到 HTTP 请求
> 2. 检查 `ws::is_upgrade(req)` → 是否为 WebSocket 升级请求
> 3. 是 → 调用 `router.findWsRoute(path)` 查找 WS 路由
> 4. 找到 → `stream.async_accept(req)` 完成协议升级
> 5. 进入 WebSocket 收发循环
>
> 升级发生在路由分发之前，因为 WebSocket 请求的处理方式与 HTTP 完全不同。

---

## 八、与游戏服务器的对比

| Hical 概念          | 游戏服务器等价物                                     |
| ------------------- | ---------------------------------------------------- |
| 中间件管道          | 消息预处理链（解密 → 解压 → 验签 → 业务处理 → 日志） |
| MiddlewareNext      | 处理链的"传递给下一步"机制                           |
| 拦截（不调用 next） | 消息过滤器拦截非法包（签名错误 → 丢弃）              |
| 前置逻辑            | 请求计时开始、解密                                   |
| 后置逻辑            | 响应加密、请求计时结束并记录                         |
| WebSocket           | 长连接通信（游戏本身就是长连接，WebSocket 类似）     |
| `receive()` 循环    | 消息收取主循环                                       |
| `send()`            | 下发消息给客户端                                     |
| onConnect 回调      | 连接建立后的初始化（发送登录确认、同步初始数据）     |

---

*下一课：第8课 - HttpServer 整合，将学习 HttpServer 如何整合 TcpServer + Router + MiddlewarePipeline，完成完整的 HTTP 请求生命周期。*
