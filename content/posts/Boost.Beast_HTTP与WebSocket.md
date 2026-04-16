+++
title = 'Boost.Beast 学习课程：HTTP 与 WebSocket'
date = 2026-04-14T03:00:00+08:00
draft = false
tags = ["Boost", "Boost.Beast", "HTTP", "WebSocket", "C++", "Hical"]
categories = ["Boost学习课程"]
description = "在 Asio 之上构建 HTTP/WebSocket 协议层，学会 Beast 的请求解析、响应构建、Parser 安全限制和 WebSocket 消息循环。"
+++

> **课程导航**：[学习路径]({{< relref "posts/Boost库学习课程_学习路径导航.md" >}}) | [Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}}) | [Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}}) | **Boost.Beast** | [Boost.JSON]({{< relref "posts/Boost.JSON_序列化与反序列化.md" >}})

## 前置知识

- [课程 1: Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}})（`error_code`、`system_error`）
- [课程 2: Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}})（`io_context`、协程、TCP socket）
- HTTP 协议基础（请求/响应格式、状态码、头部）

## 学习目标

完成本课程后，你将能够：
1. 理解 Beast 的 HTTP message 模型和 Body 类型系统
2. 使用 Parser 安全解析 HTTP 请求（含 body_limit 保护）
3. 编写协程式 HTTP 服务端和客户端
4. 实现 WebSocket 升级和消息循环
5. 读懂 Hical 的 HttpServer、HttpRequest/Response 封装和 WebSocket 集成

---

## 目录

- [前置知识](#前置知识)
- [学习目标](#学习目标)
- [目录](#目录)
- [1. 核心概念](#1-核心概念)
  - [1.1 Beast 的定位](#11-beast-的定位)
  - [1.2 HTTP message 模型](#12-http-message-模型)
  - [1.3 Buffer 体系](#13-buffer-体系)
  - [1.4 Parser 与安全限制](#14-parser-与安全限制)
- [2. 基础用法](#2-基础用法)
  - [2.1 构建 HTTP 请求和响应](#21-构建-http-请求和响应)
  - [2.2 协程式 HTTP 服务端](#22-协程式-http-服务端)
  - [2.3 Parser 高级用法](#23-parser-高级用法)
- [3. 进阶主题](#3-进阶主题)
  - [3.1 WebSocket](#31-websocket)
  - [3.2 自定义 Body 类型](#32-自定义-body-类型)
  - [3.3 超时机制](#33-超时机制)
- [4. Hical 实战解读](#4-hical-实战解读)
  - [4.1 handleSession：完整 HTTP 处理循环](#41-handlesession完整-http-处理循环)
  - [4.2 HttpRequest/Response 封装](#42-httprequestresponse-封装)
  - [4.3 WebSocketSession 封装](#43-websocketsession-封装)
  - [4.4 handleWebSocket：升级与消息循环](#44-handlewebsocket升级与消息循环)
  - [4.5 错误处理模式](#45-错误处理模式)
- [5. 练习题](#5-练习题)
  - [练习 1：基础 HTTP 服务端](#练习-1基础-http-服务端)
  - [练习 2：body\_limit 保护](#练习-2body_limit-保护)
  - [练习 3：WebSocket Echo Server](#练习-3websocket-echo-server)
  - [练习 4：Keep-Alive](#练习-4keep-alive)
  - [练习 5（挑战）：静态文件服务器](#练习-5挑战静态文件服务器)
- [6. 总结与拓展阅读](#6-总结与拓展阅读)
  - [Beast 核心 API 速查表](#beast-核心-api-速查表)
  - [HTTP 请求处理数据流](#http-请求处理数据流)
  - [拓展阅读](#拓展阅读)
  - [下一步](#下一步)

---

## 1. 核心概念

### 1.1 Beast 的定位

Beast 是**协议实现库**，不是 Web 框架。它在 Asio 之上添加 HTTP/WebSocket 协议的解析和序列化，但不提供路由、中间件等应用层功能（这些由 Hical 提供）。

```
协议栈层次：

┌─────────────────────────────────┐
│  应用层 (Hical)                  │  路由、中间件、Session、JSON
├─────────────────────────────────┤
│  Boost.Beast                     │  HTTP 解析/序列化、WebSocket 帧
├─────────────────────────────────┤
│  Boost.Asio                      │  TCP socket、SSL、io_context
├─────────────────────────────────┤
│  操作系统                        │  epoll / IOCP / kqueue
└─────────────────────────────────┘
```

### 1.2 HTTP message 模型

Beast 的 HTTP 消息是模板类——Body 类型决定消息体的存储方式：

```cpp
// 请求
http::request<http::string_body> req;    // 消息体存为 std::string
http::request<http::empty_body> req;     // 无消息体（如 GET）

// 响应
http::response<http::string_body> res;   // 消息体存为 std::string
http::response<http::file_body> res;     // 消息体从文件流式读取
```

**常用 Body 类型**：

| Body 类型      | 存储方式       | 适用场景           |
| -------------- | -------------- | ------------------ |
| `string_body`  | `std::string`  | 小型文本请求/响应  |
| `empty_body`   | 无             | GET 请求、204 响应 |
| `file_body`    | 文件句柄       | 文件下载、静态资源 |
| `dynamic_body` | `multi_buffer` | 大小未知的流式数据 |

**访问消息的各部分**：

```cpp
http::request<http::string_body> req;
req.method();           // http::verb::get
req.target();           // "/api/users?page=1"
req.version();          // 11 (HTTP/1.1)
req.body();             // std::string&
req[http::field::host]; // "example.com"
req.keep_alive();       // true/false
```

**构建响应**：

```cpp
http::response<http::string_body> res;
res.result(http::status::ok);                    // 200
res.set(http::field::content_type, "text/html"); // 设置头部
res.body() = "<h1>Hello</h1>";                   // 设置消息体
res.prepare_payload();                           // 自动计算 Content-Length
```

> `prepare_payload()` 必须在设置完 body 后调用——它会根据 body 大小设置 `Content-Length` 头。

### 1.3 Buffer 体系

Beast 提供两种缓冲区：

| 类型           | 内存布局 | 特点               |
| -------------- | -------- | ------------------ |
| `flat_buffer`  | 连续内存 | 简单高效，默认选择 |
| `multi_buffer` | 分段链表 | 减少大数据的拷贝   |

```cpp
// 基本使用
beast::flat_buffer buffer;

// 带自定义分配器（Hical 用 PMR）
beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);
```

缓冲区在多次读取间复用——Beast 从 socket 读取数据到 buffer，解析出完整消息后，buffer 中剩余数据留给下次解析（keep-alive 场景）。

### 1.4 Parser 与安全限制

`http::request_parser` 是增量解析器，比直接 `http::read()` 更安全：

```cpp
http::request_parser<http::string_body> parser;
parser.body_limit(1024 * 1024);           // 限制 body 最大 1MB
parser.header_limit(8 * 1024);            // 限制 header 最大 8KB

// 读取完成
co_await http::async_read(socket, buffer, parser, use_awaitable);

// 获取解析后的消息（所有权转移）
auto req = parser.release();
```

**为什么用 Parser 而不是直接读取到 message？**

```cpp
// 不安全：没有大小限制，恶意客户端可发送 10GB body 导致 OOM
http::request<http::string_body> req;
co_await http::async_read(socket, buffer, req, use_awaitable);

// 安全：parser 会在超过限制时返回错误
http::request_parser<http::string_body> parser;
parser.body_limit(maxBodySize);
co_await http::async_read(socket, buffer, parser, use_awaitable);
```

---

## 2. 基础用法

### 2.1 构建 HTTP 请求和响应

```cpp
// example_http_message.cpp
#include <boost/beast/http.hpp>
#include <iostream>

namespace http = boost::beast::http;

int main()
{
    // 构建 GET 请求
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/api/users");
    req.version(11);  // HTTP/1.1
    req.set(http::field::host, "example.com");
    req.set(http::field::user_agent, "MyClient/1.0");

    std::cout << req << "\n";
    // 输出：
    // GET /api/users HTTP/1.1
    // Host: example.com
    // User-Agent: MyClient/1.0

    // 构建 JSON 响应
    http::response<http::string_body> res;
    res.result(http::status::ok);
    res.version(11);
    res.set(http::field::content_type, "application/json");
    res.set(http::field::server, "MyServer/1.0");
    res.body() = R"({"name": "Alice", "age": 30})";
    res.prepare_payload();  // 自动设置 Content-Length

    std::cout << res << "\n";

    return 0;
}
```

### 2.2 协程式 HTTP 服务端

```cpp
// example_http_server.cpp
// 编译：g++ -std=c++20 -fcoroutines example_http_server.cpp \
//        -lboost_system -lpthread -o example

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast.hpp>
#include <iostream>

namespace beast = boost::beast;
namespace http = beast::http;
using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;

// 根据请求生成响应
http::response<http::string_body> handleRequest(
    const http::request<http::string_body>& req)
{
    http::response<http::string_body> res;
    res.version(req.version());
    res.keep_alive(req.keep_alive());

    if (req.method() == http::verb::get && req.target() == "/")
    {
        res.result(http::status::ok);
        res.set(http::field::content_type, "text/plain");
        res.body() = "Hello, World!";
    }
    else if (req.method() == http::verb::get && req.target() == "/json")
    {
        res.result(http::status::ok);
        res.set(http::field::content_type, "application/json");
        res.body() = R"({"message": "Hello from Beast"})";
    }
    else
    {
        res.result(http::status::not_found);
        res.body() = "Not Found";
    }

    res.prepare_payload();
    return res;
}

// 处理单个 HTTP 连接
awaitable<void> handleSession(tcp::socket socket)
{
    beast::flat_buffer buffer;

    try
    {
        for (;;)  // keep-alive 循环
        {
            // 读取请求
            http::request<http::string_body> req;
            co_await http::async_read(socket, buffer, req, use_awaitable);

            // 生成响应
            auto res = handleRequest(req);

            // 发送响应
            co_await http::async_write(socket, res, use_awaitable);

            if (!res.keep_alive())
            {
                break;
            }
        }
    }
    catch (const beast::system_error& e)
    {
        if (e.code() != boost::asio::error::eof)
        {
            std::cerr << "Session error: " << e.what() << "\n";
        }
    }

    // 优雅关闭
    boost::system::error_code ec;
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

// Accept 循环
awaitable<void> listener(tcp::acceptor& acceptor)
{
    for (;;)
    {
        auto socket = co_await acceptor.async_accept(use_awaitable);
        boost::asio::co_spawn(
            acceptor.get_executor(),
            handleSession(std::move(socket)),
            boost::asio::detached);
    }
}

int main()
{
    boost::asio::io_context ioCtx;
    tcp::acceptor acceptor(ioCtx, {tcp::v4(), 8080});

    std::cout << "HTTP server on port 8080\n";
    boost::asio::co_spawn(ioCtx, listener(acceptor), boost::asio::detached);
    ioCtx.run();
    return 0;
}
```

### 2.3 Parser 高级用法

```cpp
// 使用 parser 进行安全解析
awaitable<void> safeSession(tcp::socket socket)
{
    beast::flat_buffer buffer;

    for (;;)
    {
        http::request_parser<http::string_body> parser;
        parser.body_limit(1024 * 1024);   // 最大 1MB body
        parser.header_limit(8 * 1024);     // 最大 8KB header

        try
        {
            co_await http::async_read(socket, buffer, parser, use_awaitable);
        }
        catch (const beast::system_error& e)
        {
            if (e.code() == http::error::body_limit)
            {
                // 请求体过大 → 返回 413
                http::response<http::string_body> res {
                    http::status::payload_too_large, 11};
                res.body() = "Request body too large";
                res.prepare_payload();
                co_await http::async_write(socket, res, use_awaitable);
                co_return;
            }
            throw;  // 其他错误向上传播
        }

        auto req = parser.release();  // 获取完整消息
        // ... 处理请求 ...
    }
}
```

---

## 3. 进阶主题

### 3.1 WebSocket

WebSocket 通过 HTTP Upgrade 机制从 HTTP 连接升级而来。

**协议升级流程**：

```
客户端                          服务端
  │                               │
  │  GET / HTTP/1.1              │
  │  Upgrade: websocket          │
  │  Connection: Upgrade         │
  ├──────────────────────────────→│
  │                               │
  │  HTTP/1.1 101 Switching      │
  │  Upgrade: websocket          │
  │←──────────────────────────────┤
  │                               │
  │  ═══ WebSocket 帧 ═══        │
  │←─────────────────────────────→│
```

**检测升级请求**：

```cpp
if (boost::beast::websocket::is_upgrade(req))
{
    // 这是一个 WebSocket 升级请求
}
```

**协程式 WebSocket Echo Server**：

```cpp
// example_websocket_echo.cpp
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>

namespace beast = boost::beast;
namespace ws = beast::websocket;
using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;

awaitable<void> handleWsSession(tcp::socket socket)
{
    try
    {
        // 1. 创建 WebSocket 流
        ws::stream<tcp::socket> wsStream(std::move(socket));

        // 2. 接受升级
        co_await wsStream.async_accept(use_awaitable);

        // 3. 设置最大消息大小
        wsStream.read_message_max(1024 * 1024);  // 1MB

        // 4. 消息循环
        for (;;)
        {
            beast::flat_buffer buffer;
            co_await wsStream.async_read(buffer, use_awaitable);

            // 设置回复消息类型（文本 or 二进制）
            wsStream.text(wsStream.got_text());

            // 回显
            co_await wsStream.async_write(buffer.data(), use_awaitable);
        }
    }
    catch (const beast::system_error& e)
    {
        if (e.code() != ws::error::closed
            && e.code() != boost::asio::error::eof)
        {
            std::cerr << "WebSocket error: " << e.what() << "\n";
        }
    }
}
```

**关闭 WebSocket**：

```cpp
// 优雅关闭（发送 close 帧）
co_await wsStream.async_close(ws::close_code::normal, use_awaitable);
```

### 3.2 自定义 Body 类型

`file_body` 用于文件下载——Beast 直接从文件读取数据到 socket，避免整个文件加载到内存：

```cpp
http::response<http::file_body> res;
res.result(http::status::ok);

http::file_body::value_type body;
body.open("large_file.bin", beast::file_mode::scan, ec);
res.body() = std::move(body);

res.prepare_payload();
co_await http::async_write(socket, res, use_awaitable);
```

### 3.3 超时机制

**方式 1：steady_timer + async_read 竞争**（Hical 的方式）

```cpp
// 同时启动 timer 和 read，谁先完成就取消另一个
boost::asio::steady_timer deadline(executor, std::chrono::seconds(30));
deadline.async_wait([&socket](boost::system::error_code ec)
{
    if (!ec)
    {
        socket.close();  // 超时，关闭 socket → read 会收到错误
    }
});

co_await http::async_read(socket, buffer, parser, use_awaitable);
deadline.cancel();  // 读取成功，取消 timer
```

**方式 2：beast::tcp_stream 内置超时**

```cpp
beast::tcp_stream stream(std::move(socket));
stream.expires_after(std::chrono::seconds(30));

// async_read 会在 30 秒后超时
co_await http::async_read(stream, buffer, req, use_awaitable);
```

---

## 4. Hical 实战解读

### 4.1 handleSession：完整 HTTP 处理循环

> 源码：`src/core/HttpServer.cpp:204-350`

这是 Hical HTTP 处理的核心函数，展示了 Beast 在生产环境中的完整用法。

**连接计数 RAII（第 207-217 行）**：

```cpp
activeConnections_.fetch_add(1);
struct ConnectionCounter
{
    std::atomic<size_t>& count;
    ~ConnectionCounter() { count.fetch_sub(1); }
} connCounter {activeConnections_};
```

> 无论函数如何退出（正常/异常），连接计数都会正确递减。

**Socket RAII 守卫（第 219-235 行）**：

```cpp
struct SocketGuard
{
    tcp::socket& sock;
    bool transferred {false};
    ~SocketGuard()
    {
        if (!transferred && sock.is_open())
        {
            boost::system::error_code ec;
            sock.shutdown(tcp::socket::shutdown_send, ec);
            sock.close(ec);
        }
    }
} guard {socket};
```

> `transferred` 标志用于 WebSocket 升级场景——socket 被 move 给 WebSocket 后，guard 不应再关闭它。

**PMR 内存池（第 240-242 行）**：

```cpp
auto requestPool = MemoryPool::instance().createRequestPool();
std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());
beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);
```

> `basic_flat_buffer` 使用 PMR 分配器，所有 HTTP 解析的内存分配都走内存池，请求结束后整块释放。

**HTTP 请求读取（第 246-274 行）**：

```cpp
http::request_parser<http::string_body> parser;
parser.body_limit(maxBodySize_);
parser.header_limit(static_cast<std::uint32_t>(maxHeaderSize_));

// 空闲超时保护
if (idleTimeout_ > 0)
{
    steady_timer deadline(executor, timeout);
    deadline.async_wait([&socket](ec) { if (!ec) socket.close(); });
    co_await http::async_read(socket, buffer, parser, use_awaitable);
    deadline.cancel();
}
```

> **安全三重保护**：body_limit 防 OOM、header_limit 防大头攻击、idle timeout 防 Slowloris。

**WebSocket 升级检测（第 279-296 行）**：

```cpp
if (ws::is_upgrade(beastReq))
{
    auto* wsRoute = router_.findWsRoute(reqPath);
    if (wsRoute)
    {
        guard.transferred = true;  // 标记 socket 已转移
        co_await handleWebSocket(std::move(socket), ...);
        co_return;
    }
}
```

**响应发送（第 316-328 行）**：

```cpp
auto& nativeRes = res.native();
nativeRes.version(11);
nativeRes.set(http::field::server, HICAL_VERSION_STRING);
nativeRes.keep_alive(req.native().keep_alive());
nativeRes.prepare_payload();

co_await http::async_write(socket, nativeRes, use_awaitable);

if (!nativeRes.keep_alive())
{
    break;  // 客户端不要 keep-alive，关闭连接
}
```

### 4.2 HttpRequest/Response 封装

> 源码：`src/core/HttpRequest.h`、`src/core/HttpResponse.h/cpp`

Hical 不直接向用户暴露 Beast 类型，而是封装为框架类型，保留 `native()` 逃逸口：

```cpp
class HttpRequest
{
public:
    using BeastRequest = http::request<http::string_body>;

    HttpMethod method() const;        // 转换为框架枚举
    std::string_view path() const;    // 零拷贝路径
    boost::json::value jsonBody() const; // 解析 JSON body

    BeastRequest& native();           // 逃逸口：访问 Beast 原始类型

    template <typename T>
    T readJson() const;               // 反射驱动的类型安全反序列化
};
```

**HttpResponse 工厂方法**（`HttpResponse.cpp`）：

```cpp
// 便捷构造
auto res = HttpResponse::ok("Hello");           // 200 + 文本
auto res = HttpResponse::json({{"key", "val"}}); // 200 + JSON
auto res = HttpResponse::notFound();             // 404
auto res = HttpResponse::redirect("/login");     // 302
```

> **设计模式**：封装第三方库类型 + 提供 `native()` 逃逸口——在大多数情况下用框架 API，特殊场景可以直接操作 Beast 对象。

### 4.3 WebSocketSession 封装

> 源码：`src/core/WebSocket.h`

```cpp
class WebSocketSession
{
    using WsStream = ws::stream<tcp::socket>;
    static constexpr size_t hDefaultMaxMessageSize = 1024 * 1024;  // 1MB

    Awaitable<void> send(const std::string& msg);
    Awaitable<std::optional<std::string>> receive();
    Awaitable<void> closeAsync();
    bool isOpen() const;
};
```

**安全设计**：
- `read_message_max(maxMessageSize)` 限制最大消息大小，防 OOM
- `std::atomic<bool> open_` 用原子变量跟踪连接状态
- `receive()` 返回 `std::optional`——连接关闭时返回 `nullopt`

### 4.4 handleWebSocket：升级与消息循环

> 源码：`src/core/HttpServer.cpp:352-408`

```cpp
Awaitable<void> HttpServer::handleWebSocket(
    tcp::socket socket,
    http::request<http::string_body> req,
    const Router::WsRoute& wsRoute)
{
    ws::stream<tcp::socket> wsStream(std::move(socket));

    // 接受 WebSocket 升级
    co_await wsStream.async_accept(req, use_awaitable);

    auto session = std::make_unique<WebSocketSession>(std::move(wsStream));

    // 连接回调
    if (wsRoute.onConnect)
        co_await wsRoute.onConnect(*session);

    // 消息循环
    while (session->isOpen())
    {
        auto msg = co_await session->receive();
        if (!msg.has_value())
            break;
        if (wsRoute.onMessage)
            co_await wsRoute.onMessage(*msg, *session);
    }

    // 断开回调（正常和异常退出都触发）
    if (session && wsRoute.onDisconnect)
        co_await wsRoute.onDisconnect(*session);
}
```

> **关键流程**：`async_accept` 完成 HTTP → WebSocket 升级 → 进入消息循环 → 收到消息调用用户回调 → 断开时调用断开回调。

### 4.5 错误处理模式

> 源码：`src/core/HttpServer.cpp:331-348`

```cpp
catch (const beast::system_error& e)
{
    if (e.code() == http::error::body_limit)
    {
        // 413 Payload Too Large
        http::response<http::string_body> res {
            http::status::payload_too_large, 11};
        res.body() = "Request body too large";
        res.prepare_payload();
        http::write(socket, res, writeEc);  // 同步写（异常路径）
    }
    else if (e.code() != beast::errc::not_connected
             && e.code() != boost::asio::error::eof)
    {
        // 忽略正常关闭
    }
}
```

**错误分类表**：

| 错误码                       | 含义               | 处理方式       |
| ---------------------------- | ------------------ | -------------- |
| `http::error::body_limit`    | 请求体超过限制     | 返回 413 响应  |
| `boost::asio::error::eof`    | 客户端正常关闭     | 静默退出       |
| `beast::errc::not_connected` | 连接已断开         | 静默退出       |
| `ws::error::closed`          | WebSocket 正常关闭 | 静默退出       |
| 其他错误                     | 异常情况           | 记日志（可选） |

---

## 5. 练习题

### 练习 1：基础 HTTP 服务端

编写一个协程式 HTTP 服务端，支持：
- `GET /` 返回 HTML 页面
- `POST /echo` 将请求体原样返回（Content-Type: text/plain）
- 其他路径返回 404

### 练习 2：body_limit 保护

在练习 1 基础上，使用 `request_parser` + `body_limit(1024)` 限制请求体最大 1KB。当超过限制时返回 413。

测试：`curl -X POST -d "@large_file.txt" http://localhost:8080/echo`

### 练习 3：WebSocket Echo Server

编写一个同时支持 HTTP 和 WebSocket 的服务端：
- `GET /` 返回一个包含 JavaScript WebSocket 客户端的 HTML 页面
- `ws://localhost:8080/ws` 路径支持 WebSocket 升级
- WebSocket 消息原样回显

### 练习 4：Keep-Alive

在练习 1 基础上实现 keep-alive 支持：
- 在 for 循环中持续读写
- 通过 `keep_alive()` 判断是否继续
- 使用 `curl -v --http1.1` 测试（观察 `Connection: keep-alive` 头）

### 练习 5（挑战）：静态文件服务器

实现一个简单的 HTTP 文件服务器：
- 使用 `file_body` 提供指定目录下的文件下载
- 自动检测 MIME 类型（.html → text/html, .json → application/json）
- 防止路径遍历攻击（拒绝包含 `..` 的路径）

---

## 6. 总结与拓展阅读

### Beast 核心 API 速查表

| API                          | 用途                    |
| ---------------------------- | ----------------------- |
| `http::request<Body>`        | HTTP 请求消息           |
| `http::response<Body>`       | HTTP 响应消息           |
| `http::request_parser<Body>` | 增量请求解析器          |
| `http::async_read()`         | 协程式读取 HTTP 消息    |
| `http::async_write()`        | 协程式发送 HTTP 消息    |
| `beast::flat_buffer`         | 连续内存缓冲区          |
| `ws::stream<socket>`         | WebSocket 流            |
| `ws::is_upgrade(req)`        | 检测 WebSocket 升级请求 |
| `prepare_payload()`          | 自动设置 Content-Length |
| `parser.body_limit(n)`       | 限制请求体大小          |

### HTTP 请求处理数据流

```
TCP socket
    │
    ▼
beast::flat_buffer  ←── async_read (从 socket 读入 buffer)
    │
    ▼
request_parser  ←── 解析 HTTP 头 + body
    │
    ▼
parser.release() → http::request<string_body>
    │
    ▼
路由 + 中间件 → http::response<string_body>
    │
    ▼
async_write ──→ TCP socket (发送给客户端)
```

### 拓展阅读

- [Boost.Beast 官方文档](https://www.boost.org/doc/libs/release/libs/beast/doc/html/index.html)
- [Beast HTTP 示例](https://www.boost.org/doc/libs/release/libs/beast/doc/html/beast/examples.html)
- [WebSocket RFC 6455](https://www.rfc-editor.org/rfc/rfc6455)

### 下一步

Beast 负责传输 HTTP 消息，消息体中最常见的格式是 JSON。在 [课程 4: Boost.JSON]({{< relref "posts/Boost.JSON_序列化与反序列化.md" >}}) 中，你将学习如何解析和生成 JSON 数据，以及 Hical 如何通过反射层实现自动序列化。
