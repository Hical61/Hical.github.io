+++
title = 'Boost.Asio 学习课程：异步 I/O 与协程'
date = 2026-04-14T02:00:00+08:00
draft = false
tags = ["Boost", "Boost.Asio", "协程", "io_context", "C++20", "Hical"]
categories = ["Boost学习课程"]
description = "从 io_context 出发，掌握 C++20 协程式异步 I/O，学会 TCP 服务器、定时器和多线程模型，结合 Hical 框架实战解读。"
+++

> **课程导航**：[学习路径]({{< relref "posts/Boost库学习课程_学习路径导航.md" >}}) | [Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}}) | **Boost.Asio** | [Boost.Beast]({{< relref "posts/Boost.Beast_HTTP与WebSocket.md" >}}) | [Boost.JSON]({{< relref "posts/Boost.JSON_序列化与反序列化.md" >}})

## 前置知识

- [课程 1: Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}})（`error_code`、`system_error`）
- C++ 基础：模板、lambda、智能指针
- C++20 协程语法（`co_await`、`co_return`）——本课程会从零讲解

## 学习目标

完成本课程后，你将能够：
1. 理解 `io_context` 的工作原理和生命周期管理
2. 掌握 C++20 协程式异步编程（`co_await` + `use_awaitable`）
3. 编写协程式 TCP 服务器和客户端
4. 使用 `steady_timer` 实现定时任务
5. 理解多线程模型的选型和 `strand` 序列化
6. 读懂 Hical 的 EventLoop、连接管理和 SSL 集成

---

## 目录

- [前置知识](#前置知识)
- [学习目标](#学习目标)
- [目录](#目录)
- [1. 核心概念](#1-核心概念)
  - [1.1 Asio 的设计哲学](#11-asio-的设计哲学)
  - [1.2 io\_context：事件循环的心脏](#12-io_context事件循环的心脏)
  - [1.3 Executor 模型：post vs dispatch](#13-executor-模型post-vs-dispatch)
  - [1.4 三种异步完成方式](#14-三种异步完成方式)
- [2. 基础用法](#2-基础用法)
  - [2.1 最小 io\_context 示例](#21-最小-io_context-示例)
  - [2.2 TCP 基础：同步与异步](#22-tcp-基础同步与异步)
  - [2.3 协程式异步 I/O](#23-协程式异步-io)
  - [2.4 steady\_timer 定时器](#24-steady_timer-定时器)
  - [2.5 buffer 操作](#25-buffer-操作)
- [3. 进阶主题](#3-进阶主题)
  - [3.1 多线程模型](#31-多线程模型)
  - [3.2 strand 序列化执行](#32-strand-序列化执行)
  - [3.3 SSL/TLS 支持](#33-ssltls-支持)
  - [3.4 signal\_set 信号处理](#34-signal_set-信号处理)
- [4. Hical 实战解读](#4-hical-实战解读)
  - [4.1 AsioEventLoop：io\_context 的框架封装](#41-asioeventloopio_context-的框架封装)
  - [4.2 dispatch vs post 实战](#42-dispatch-vs-post-实战)
  - [4.3 EventLoopPool：多线程池模型](#43-eventlooppool多线程池模型)
  - [4.4 AsioTimer：定时器的生产级封装](#44-asiotimer定时器的生产级封装)
  - [4.5 TcpServer：协程式 accept 循环](#45-tcpserver协程式-accept-循环)
  - [4.6 Coroutine.h：协程工具函数](#46-coroutineh协程工具函数)
  - [4.7 SSL 集成](#47-ssl-集成)
- [5. 练习题](#5-练习题)
  - [练习 1：协程式 Echo Server](#练习-1协程式-echo-server)
  - [练习 2：周期性日志](#练习-2周期性日志)
  - [练习 3：多 io\_context 模型](#练习-3多-io_context-模型)
  - [练习 4：SSL Echo Server](#练习-4ssl-echo-server)
  - [练习 5：协程式 HTTP 客户端](#练习-5协程式-http-客户端)
- [6. 总结与拓展阅读](#6-总结与拓展阅读)
  - [核心 API 速查表](#核心-api-速查表)
  - [三种异步模式对比](#三种异步模式对比)
  - [拓展阅读](#拓展阅读)
  - [下一步](#下一步)

---

## 1. 核心概念

### 1.1 Asio 的设计哲学

Boost.Asio 采用 **Proactor 模式**——应用程序发起异步操作，操作系统完成后通知应用。

```
Proactor 模式事件流：

  应用层                 Asio                操作系统
    │                     │                     │
    │ async_read(buf)     │                     │
    ├────────────────────→│                     │
    │                     │  提交到 OS          │
    │                     ├────────────────────→│
    │                     │                     │
    │  （应用继续执行      │                     │ 数据到达
    │   其他任务）         │                     │
    │                     │  完成通知           │
    │                     │←────────────────────┤
    │  回调/协程恢复       │                     │
    │←────────────────────┤                     │
```

**底层实现因平台而异**：

| 平台    | 机制     | 模式                    |
| ------- | -------- | ----------------------- |
| Linux   | `epoll`  | Reactor → Proactor 模拟 |
| macOS   | `kqueue` | Reactor → Proactor 模拟 |
| Windows | `IOCP`   | 原生 Proactor           |

对于应用开发者来说，这些差异被 Asio 完全封装——你写的代码在所有平台上行为一致。

### 1.2 io_context：事件循环的心脏

`io_context` 是 Asio 的核心——它既是**任务队列**，又是**事件分发器**。

```
┌──────────────────────────────────────────┐
│              io_context                   │
│                                          │
│  ┌──────────────────────────────────┐    │
│  │  任务队列                         │    │
│  │  [回调A] [回调B] [协程恢复C] ... │    │
│  └──────────────────────────────────┘    │
│                                          │
│  ┌──────────────────────────────────┐    │
│  │  OS 事件监听                      │    │
│  │  • socket 可读事件               │    │
│  │  • socket 可写事件               │    │
│  │  • 定时器到期事件                 │    │
│  └──────────────────────────────────┘    │
│                                          │
│  run() → 循环：取任务 → 执行 → 取任务... │
└──────────────────────────────────────────┘
```

**关键 API**：

| 方法        | 说明                               |
| ----------- | ---------------------------------- |
| `run()`     | 阻塞执行事件循环，直到没有任务为止 |
| `stop()`    | 中断 `run()`，使其尽快返回         |
| `restart()` | 重置停止状态，允许再次 `run()`     |

**work_guard 防止提前退出**：

`run()` 在**没有待执行任务**时会返回。但服务器通常需要持续等待新连接，即使当前没有任何活跃操作。`work_guard` 告诉 `io_context` "还有工作要做"，阻止 `run()` 提前退出：

```cpp
boost::asio::io_context ioCtx;

// 创建 work_guard：run() 不会因为没有任务而退出
auto workGuard = boost::asio::make_work_guard(ioCtx);

// 在另一个线程或定时器中...
workGuard.reset();  // 允许 run() 退出
ioCtx.stop();       // 立即停止
```

### 1.3 Executor 模型：post vs dispatch

Asio 提供两种方式向 `io_context` 投递任务：

| 方法                  | 行为                                                                            |
| --------------------- | ------------------------------------------------------------------------------- |
| `post(ioCtx, fn)`     | **总是**将 fn 排入队列，异步执行                                                |
| `dispatch(ioCtx, fn)` | 如果当前线程正在运行此 ioCtx 的 `run()`，则**立即执行** fn；否则等同于 `post()` |

```
dispatch() 的决策逻辑：

  调用 dispatch(fn)
        │
  当前线程是 ioCtx 线程？
   ┌────┴────┐
  是         否
   │          │
  立即执行   排入队列
  fn()       （等 run() 取出执行）
```

### 1.4 三种异步完成方式

Asio 的异步操作支持三种完成通知方式：

**方式 1：回调（Callback）**
```cpp
socket.async_read_some(buffer,
    [](boost::system::error_code ec, size_t bytesRead)
    {
        // 读取完成
    });
```

**方式 2：Future**
```cpp
auto future = socket.async_read_some(buffer, boost::asio::use_future);
auto bytesRead = future.get();  // 阻塞等待
```

**方式 3：协程（Coroutine）—— 推荐**
```cpp
auto bytesRead = co_await socket.async_read_some(buffer,
    boost::asio::use_awaitable);
```

| 方式     | 代码风格     | 性能             | 错误处理              | 推荐场景     |
| -------- | ------------ | ---------------- | --------------------- | ------------ |
| 回调     | 嵌套层深     | 最优             | `error_code` 参数     | 简单场景     |
| Future   | 同步风格     | 较差（阻塞线程） | 异常                  | 测试代码     |
| **协程** | **同步风格** | **接近回调**     | **异常（try/catch）** | **生产代码** |

> Hical 全面采用协程方式。

---

## 2. 基础用法

### 2.1 最小 io_context 示例

```cpp
// example_io_context.cpp
// 编译：g++ -std=c++20 example_io_context.cpp -lboost_system -lpthread -o example

#include <boost/asio.hpp>
#include <iostream>

int main()
{
    boost::asio::io_context ioCtx;

    // 向 io_context 投递两个任务
    boost::asio::post(ioCtx, []()
    {
        std::cout << "任务 1 执行\n";
    });

    boost::asio::post(ioCtx, []()
    {
        std::cout << "任务 2 执行\n";
    });

    std::cout << "run() 之前\n";

    // run() 执行所有任务，然后返回（因为没有更多任务了）
    ioCtx.run();

    std::cout << "run() 之后\n";
    return 0;
}
// 输出：
// run() 之前
// 任务 1 执行
// 任务 2 执行
// run() 之后
```

**work_guard 示例**：

```cpp
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <chrono>

int main()
{
    boost::asio::io_context ioCtx;

    // 没有 work_guard，run() 会立即返回（因为没有任务）
    // 有了 work_guard，run() 会持续等待
    auto guard = boost::asio::make_work_guard(ioCtx);

    // 在另一个线程中 3 秒后释放 guard
    std::thread t([&guard]()
    {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout << "释放 work_guard\n";
        guard.reset();
    });

    std::cout << "run() 开始（将阻塞 3 秒）\n";
    ioCtx.run();
    std::cout << "run() 结束\n";

    t.join();
    return 0;
}
```

### 2.2 TCP 基础：同步与异步

**同步 TCP 客户端**：

```cpp
// example_tcp_sync.cpp
// 编译：g++ -std=c++20 example_tcp_sync.cpp -lboost_system -lpthread -o example

#include <boost/asio.hpp>
#include <iostream>
#include <string>

using boost::asio::ip::tcp;

int main()
{
    boost::asio::io_context ioCtx;

    // 解析地址
    tcp::resolver resolver(ioCtx);
    auto endpoints = resolver.resolve("httpbin.org", "80");

    // 同步连接
    tcp::socket socket(ioCtx);
    boost::asio::connect(socket, endpoints);

    // 发送 HTTP 请求
    std::string request =
        "GET /get HTTP/1.1\r\n"
        "Host: httpbin.org\r\n"
        "Connection: close\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request));

    // 读取响应
    boost::system::error_code ec;
    std::string response;
    char buf[1024];
    while (size_t n = socket.read_some(boost::asio::buffer(buf), ec))
    {
        response.append(buf, n);
    }

    if (ec == boost::asio::error::eof)
    {
        std::cout << "响应长度: " << response.size() << " bytes\n";
    }

    return 0;
}
```

**回调式异步 Echo Server**：

```cpp
// example_echo_callback.cpp
#include <boost/asio.hpp>
#include <iostream>
#include <memory>

using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(tcp::socket socket) : socket_(std::move(socket)) {}

    void start()
    {
        doRead();
    }

private:
    void doRead()
    {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(data_),
            [this, self](boost::system::error_code ec, size_t length)
            {
                if (!ec)
                {
                    doWrite(length);
                }
            });
    }

    void doWrite(size_t length)
    {
        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(data_, length),
            [this, self](boost::system::error_code ec, size_t)
            {
                if (!ec)
                {
                    doRead();
                }
            });
    }

    tcp::socket socket_;
    char data_[1024];
};
```

> 注意回调风格的嵌套——`doRead` → `doWrite` → `doRead` 形成回调链，代码难以线性阅读。

### 2.3 协程式异步 I/O

协程让异步代码看起来像同步代码——这是 Hical 采用的核心模式。

**核心三件套**：
- `boost::asio::awaitable<T>`：协程返回类型
- `boost::asio::use_awaitable`：告诉异步操作 "我在协程中，完成后恢复我"
- `boost::asio::co_spawn`：启动一个协程

```cpp
// example_echo_coroutine.cpp
// 编译：g++ -std=c++20 -fcoroutines example_echo_coroutine.cpp \
//        -lboost_system -lpthread -o example

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;

// 处理单个连接的协程
awaitable<void> handleSession(tcp::socket socket)
{
    try
    {
        char data[1024];
        for (;;)
        {
            // co_await 会挂起当前协程，数据到达后自动恢复
            auto bytesRead = co_await socket.async_read_some(
                boost::asio::buffer(data), use_awaitable);

            // 将收到的数据原样发回
            co_await boost::asio::async_write(
                socket, boost::asio::buffer(data, bytesRead), use_awaitable);
        }
    }
    catch (const boost::system::system_error& e)
    {
        if (e.code() != boost::asio::error::eof)
        {
            std::cerr << "Session error: " << e.what() << "\n";
        }
        // EOF = 客户端正常断开，不是错误
    }
}

// 接受连接的协程
awaitable<void> listener(tcp::acceptor& acceptor)
{
    for (;;)
    {
        auto socket = co_await acceptor.async_accept(use_awaitable);

        std::cout << "新连接: "
                  << socket.remote_endpoint().address().to_string() << "\n";

        // 为每个连接启动独立的处理协程
        boost::asio::co_spawn(
            acceptor.get_executor(),
            handleSession(std::move(socket)),
            boost::asio::detached);
    }
}

int main()
{
    boost::asio::io_context ioCtx;

    tcp::acceptor acceptor(ioCtx, {tcp::v4(), 9999});
    std::cout << "Echo server listening on port 9999\n";

    // 启动 listener 协程
    boost::asio::co_spawn(ioCtx, listener(acceptor), boost::asio::detached);

    // 运行事件循环
    ioCtx.run();
    return 0;
}
```

> 对比回调版本：协程代码是**线性的**（read → write → read），没有嵌套回调。

**协程中的错误处理**：

协程模式下，`use_awaitable` 遇到错误时会**抛出 `boost::system::system_error` 异常**。用 `try/catch` 捕获：

```cpp
awaitable<void> safeRead(tcp::socket& socket)
{
    try
    {
        char buf[1024];
        auto n = co_await socket.async_read_some(
            boost::asio::buffer(buf), use_awaitable);
    }
    catch (const boost::system::system_error& e)
    {
        if (e.code() == boost::asio::error::eof)
        {
            // 正常关闭
        }
        else if (e.code() == boost::asio::error::operation_aborted)
        {
            // 操作被取消
        }
        else
        {
            // 其他错误
        }
    }
}
```

### 2.4 steady_timer 定时器

`steady_timer` 是基于 `std::chrono::steady_clock` 的定时器。

**同步等待**：

```cpp
boost::asio::steady_timer timer(ioCtx, std::chrono::seconds(3));
timer.wait();  // 阻塞 3 秒
```

**异步回调等待**：

```cpp
timer.async_wait([](boost::system::error_code ec)
{
    if (!ec)
    {
        std::cout << "定时器到期\n";
    }
    else if (ec == boost::asio::error::operation_aborted)
    {
        std::cout << "定时器被取消\n";
    }
});
```

**协程式等待（推荐）**：

```cpp
awaitable<void> delayedTask(boost::asio::io_context& ioCtx)
{
    boost::asio::steady_timer timer(ioCtx, std::chrono::seconds(2));
    co_await timer.async_wait(use_awaitable);
    std::cout << "2 秒后执行\n";
}
```

**周期定时器（手动重置）**：

```cpp
awaitable<void> periodicTask(boost::asio::io_context& ioCtx)
{
    boost::asio::steady_timer timer(ioCtx);
    for (int i = 0; i < 5; ++i)
    {
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait(use_awaitable);
        std::cout << "第 " << (i + 1) << " 次触发\n";
    }
}
```

### 2.5 buffer 操作

Asio 的 buffer 是对现有内存的**非拥有视图**（不分配内存）：

```cpp
// 从 string 创建只读 buffer
std::string data = "Hello";
auto cbuf = boost::asio::buffer(data);  // const_buffer

// 从 array 创建可写 buffer
char arr[1024];
auto mbuf = boost::asio::buffer(arr);   // mutable_buffer

// 从 vector 创建
std::vector<char> vec(2048);
auto vbuf = boost::asio::buffer(vec);
```

**async_read_some vs async_read**：

| 方法              | 行为                                  | 返回           |
| ----------------- | ------------------------------------- | -------------- |
| `async_read_some` | 读取**可用的**数据（可能不满 buffer） | 实际读取字节数 |
| `async_read`      | 读取**恰好**指定字节数（除非 EOF）    | 指定字节数     |

```cpp
// async_read_some：有多少读多少
char buf[1024];
auto n = co_await socket.async_read_some(
    boost::asio::buffer(buf), use_awaitable);
// n 可能是 1 ~ 1024 之间的任意值

// async_read：精确读取 100 字节
char exact[100];
co_await boost::asio::async_read(
    socket, boost::asio::buffer(exact), use_awaitable);
// 要么读满 100 字节，要么抛异常（如 EOF）
```

---

## 3. 进阶主题

### 3.1 多线程模型

**模型 A：单 io_context + 多线程 run()**

```
┌──────────────────────────────────┐
│          io_context              │
│  ┌────────────────────────────┐  │
│  │       任务队列              │  │
│  └────────────────────────────┘  │
│                                  │
│  Thread 1: run()                 │
│  Thread 2: run()     ← 竞争取任务│
│  Thread 3: run()                 │
└──────────────────────────────────┘
```

- **优点**：实现简单
- **缺点**：共享资源需加锁/strand 保护
- **Hical 使用**：`HttpServer::start()` 采用此模型

```cpp
// 单 io_context 多线程
std::vector<std::thread> threads;
for (int i = 1; i < numThreads; ++i)
{
    threads.emplace_back([&ioCtx]() { ioCtx.run(); });
}
ioCtx.run();  // 主线程也参与

for (auto& t : threads)
{
    t.join();
}
```

**模型 B：多 io_context，每线程一个**

```
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  io_context 1   │  │  io_context 2   │  │  io_context 3   │
│  Thread 1: run()│  │  Thread 2: run()│  │  Thread 3: run()│
│  [连接A, 连接D] │  │  [连接B, 连接E] │  │  [连接C, 连接F] │
└─────────────────┘  └─────────────────┘  └─────────────────┘
         ↑                    ↑                    ↑
         └────────────────────┴────────────────────┘
                     round-robin 分发新连接
```

- **优点**：每个连接绑定到一个线程，无需 strand/锁
- **缺点**：负载可能不均匀
- **Hical 使用**：`EventLoopPool` + `TcpServer` 采用此模型

### 3.2 strand 序列化执行

当多个线程运行同一个 `io_context` 时，多个回调可能并发执行。`strand` 保证提交给它的任务**按顺序逐一执行**，即使来自不同线程：

```cpp
boost::asio::strand<boost::asio::io_context::executor_type> strand(
    boost::asio::make_strand(ioCtx));

// 这两个任务保证不会并发执行
boost::asio::post(strand, []() { std::cout << "A\n"; });
boost::asio::post(strand, []() { std::cout << "B\n"; });
```

**协程与 strand 的关系**：

在同一个协程内部，代码天然是串行的——`co_await` 挂起后恢复时，保证在同一个执行上下文中。所以协程内部通常**不需要显式 strand**。

但如果一个对象被多个协程共享（如连接的发送队列被读写协程同时访问），仍然需要同步——Hical 选择用 `dispatch/post` + `isInLoopThread` 来实现（见第 4.2 节）。

### 3.3 SSL/TLS 支持

Asio 通过 `boost::asio::ssl` 命名空间提供 SSL/TLS 支持：

```cpp
// example_ssl_client.cpp
// 编译：g++ -std=c++20 example_ssl_client.cpp \
//        -lboost_system -lssl -lcrypto -lpthread -o example

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;
namespace ssl = boost::asio::ssl;

awaitable<void> httpsGet(boost::asio::io_context& ioCtx)
{
    // 1. 创建 SSL 上下文
    ssl::context sslCtx(ssl::context::tls_client);
    sslCtx.set_default_verify_paths();

    // 2. 解析地址
    tcp::resolver resolver(ioCtx);
    auto endpoints = co_await resolver.async_resolve(
        "httpbin.org", "443", use_awaitable);

    // 3. 创建 SSL 流（在 TCP socket 之上叠加 SSL 层）
    ssl::stream<tcp::socket> stream(ioCtx, sslCtx);

    // 4. 连接
    co_await boost::asio::async_connect(
        stream.lowest_layer(), endpoints, use_awaitable);

    // 5. SSL 握手
    co_await stream.async_handshake(
        ssl::stream_base::client, use_awaitable);

    // 6. 发送 HTTPS 请求
    std::string request =
        "GET /get HTTP/1.1\r\n"
        "Host: httpbin.org\r\n"
        "Connection: close\r\n\r\n";
    co_await boost::asio::async_write(
        stream, boost::asio::buffer(request), use_awaitable);

    // 7. 读取响应
    char buf[4096];
    try
    {
        for (;;)
        {
            auto n = co_await stream.async_read_some(
                boost::asio::buffer(buf), use_awaitable);
            std::cout.write(buf, n);
        }
    }
    catch (const boost::system::system_error& e)
    {
        if (e.code() != boost::asio::error::eof)
        {
            throw;
        }
    }

    // 8. SSL 关闭
    co_await stream.async_shutdown(use_awaitable);
}

int main()
{
    boost::asio::io_context ioCtx;
    boost::asio::co_spawn(ioCtx, httpsGet(ioCtx), boost::asio::detached);
    ioCtx.run();
    return 0;
}
```

**SSL 协议栈**：

```
┌─────────────────────┐
│  应用数据 (HTTP)     │
├─────────────────────┤
│  ssl::stream         │  ← 加密/解密层
├─────────────────────┤
│  tcp::socket         │  ← TCP 传输层
├─────────────────────┤
│  操作系统内核         │
└─────────────────────┘
```

### 3.4 signal_set 信号处理

用于优雅关闭服务器：

```cpp
boost::asio::signal_set signals(ioCtx, SIGINT, SIGTERM);
signals.async_wait([&ioCtx](boost::system::error_code, int signo)
{
    std::cout << "收到信号 " << signo << "，关闭服务器\n";
    ioCtx.stop();
});
```

---

## 4. Hical 实战解读

### 4.1 AsioEventLoop：io_context 的框架封装

> 源码：`src/asio/AsioEventLoop.h`、`src/asio/AsioEventLoop.cpp`

Hical 将 `io_context` 封装为 `AsioEventLoop`，实现了框架的 `EventLoop` 抽象接口。

**构造函数（AsioEventLoop.cpp:7-11）**：

```cpp
AsioEventLoop::AsioEventLoop()
    : workGuard_(std::make_unique<...>(
          boost::asio::make_work_guard(ioContext_)))
{
}
```

> 构造时立即创建 `work_guard`，保证 `run()` 不会因为没有初始任务而立即返回。

**run() 实现（AsioEventLoop.cpp:23-43）**：

```cpp
void AsioEventLoop::run()
{
    threadId_ = std::this_thread::get_id();  // 记录线程 ID
    running_.store(true);

    ioContext_.run();  // 阻塞，直到 stop() 或 work_guard 释放

    // 执行退出回调
    for (auto& cb : quitCallbacks_)
    {
        cb();
    }
    running_.store(false);
}
```

**stop() 实现（AsioEventLoop.cpp:45-50）**：

```cpp
void AsioEventLoop::stop()
{
    quit_.store(true);
    workGuard_.reset();  // 先释放 work_guard
    ioContext_.stop();    // 再停止 io_context
}
```

> **顺序很重要**：先 `reset()` work_guard（允许 run() 自然退出），再 `stop()`（强制中断）。如果只 `stop()` 不 reset work_guard，下次 `restart()` + `run()` 会因为 work_guard 仍在而无法正常停止。

### 4.2 dispatch vs post 实战

> 源码：`src/asio/AsioEventLoop.cpp:59-74`

```cpp
void AsioEventLoop::dispatch(Func cb)
{
    if (isInLoopThread())
    {
        cb();  // 当前就是 loop 线程，直接执行
    }
    else
    {
        post(std::move(cb));  // 不是 loop 线程，投递到队列
    }
}

void AsioEventLoop::post(Func cb)
{
    boost::asio::post(ioContext_, std::move(cb));
}
```

**实际应用场景**——GenericConnection 的发送函数：

当用户从任意线程调用 `send()` 时，需要确保数据写入操作在连接所属的 loop 线程中执行：

```cpp
void send(const char* data, size_t len)
{
    if (loop_->isInLoopThread())
    {
        sendInLoop(data, len);  // 当前线程安全，直接写
    }
    else
    {
        auto msg = std::make_shared<std::string>(data, len);
        loop_->post([this, msg]()
        {
            sendInLoop(msg->data(), msg->size());  // 投递到 loop 线程
        });
    }
}
```

> **设计原则**：同线程直接执行（零开销），跨线程才投递（一次队列操作）。

### 4.3 EventLoopPool：多线程池模型

> 源码：`src/asio/EventLoopPool.h`

```cpp
class EventLoopPool
{
    std::vector<std::unique_ptr<AsioEventLoop>> loops_;  // N 个事件循环
    std::vector<std::thread> threads_;                    // N 个线程
    std::atomic<size_t> nextIndex_ {0};                   // round-robin 索引
};
```

**round-robin 分发**：

```cpp
AsioEventLoop* getNextLoop()
{
    size_t index = nextIndex_.fetch_add(1) % loops_.size();
    return loops_[index].get();
}
```

**架构图**：

```
                        TcpServer
                     (主 EventLoop)
                          │
                    acceptLoop()
                    co_await async_accept()
                          │
         ┌────────────────┼────────────────┐
         ▼                ▼                ▼
   EventLoop 0      EventLoop 1      EventLoop 2
   (Thread 0)       (Thread 1)       (Thread 2)
   [连接A,D,G]     [连接B,E,H]     [连接C,F,I]
```

> TcpServer 在主 EventLoop 上接受连接，通过 `getNextLoop()` 将新连接分发到 worker EventLoop。每个连接的所有 I/O 操作都在其分配到的 EventLoop 线程中执行，不需要锁。

### 4.4 AsioTimer：定时器的生产级封装

> 源码：`src/asio/AsioTimer.h`、`src/asio/AsioTimer.cpp`（通过 `src/asio/AsioEventLoop.cpp:78-125` 间接使用）

**两种定时器**：

| 方法                     | 含义       | 清理策略                  |
| ------------------------ | ---------- | ------------------------- |
| `runAfter(delay, cb)`    | 单次定时器 | 触发后自动从 map 移除     |
| `runEvery(interval, cb)` | 周期定时器 | 手动 `cancelTimer()` 移除 |

**单次定时器的自动清理**（AsioEventLoop.cpp:78-99）：

```cpp
TimerId AsioEventLoop::runAfter(double delay, Func cb)
{
    TimerId id = nextTimerId_.fetch_add(1);
    auto timer = std::make_shared<AsioTimer>(this, delay, std::move(cb));

    // 设置清理回调：触发后自动从 map 中移除，防内存泄漏
    timer->setCleanup(id, [this](uint64_t timerId)
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        timers_.erase(timerId);
    });

    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        timers_[id] = timer;
    }
    timer->start();
    return id;
}
```

> **关键设计**：
> - `shared_ptr` 持有 timer，避免回调触发时 timer 已析构
> - `enable_shared_from_this` 让 timer 在异步回调中安全引用自身
> - 清理回调确保单次定时器不会永久占用 map 内存

### 4.5 TcpServer：协程式 accept 循环

> 源码：`src/asio/TcpServer.cpp:30-65`（启动）、`src/asio/TcpServer.cpp:157-233`（accept 循环）

**启动流程**：

```cpp
void TcpServer::start()
{
    // 1. 创建 IO 线程池
    if (ioLoopNum_ > 0)
    {
        ioPool_ = std::make_unique<EventLoopPool>(ioLoopNum_);
        ioPool_->start();
    }

    // 2. 配置 acceptor
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    // 3. 启动 accept 协程
    boost::asio::co_spawn(
        baseLoop_->getIoContext(),
        [this]() -> Awaitable<void> { co_await acceptLoop(); },
        boost::asio::detached);
}
```

**acceptLoop 协程**（TcpServer.cpp:157-233）：

```cpp
Awaitable<void> TcpServer::acceptLoop()
{
    while (running_.load())
    {
        try
        {
            // 协程式接受连接
            tcp::socket socket = co_await acceptor_.async_accept(
                boost::asio::use_awaitable);

            // round-robin 选择目标 IO 循环
            auto* ioLoop = getNextIoLoop();

            // 根据是否启用 SSL 创建不同连接类型
            TcpConnection::Ptr conn;
            if (sslCtx_)
            {
                boost::asio::ssl::stream<tcp::socket> sslStream(
                    std::move(socket), sslCtx_->native());
                conn = std::make_shared<SslConnection>(
                    ioLoop, std::move(sslStream), localAddr, peerAddr);
            }
            else
            {
                conn = std::make_shared<PlainConnection>(
                    ioLoop, std::move(socket), localAddr, peerAddr);
            }

            conn->connectEstablished();  // SSL 连接会触发握手
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == boost::asio::error::operation_aborted)
            {
                break;  // acceptor 被关闭，正常退出
            }
            // EMFILE 等瞬态错误，继续接受
        }
    }
}
```

> **关键 Asio 用法**：
> - `co_await acceptor_.async_accept(use_awaitable)` —— 协程式接受连接
> - `boost::asio::co_spawn(..., detached)` —— 启动独立协程，不关心返回值
> - try/catch 处理 `system_error` —— 协程中错误必须用异常捕获

### 4.6 Coroutine.h：协程工具函数

> 源码：`src/core/Coroutine.h`

Hical 对 Asio 的协程 API 做了简洁封装：

```cpp
// 协程返回类型别名
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;

// 协程内 sleep（从当前执行器获取 timer）
inline Awaitable<void> sleep(double seconds)
{
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(
        executor,
        std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)));
    co_await timer.async_wait(boost::asio::use_awaitable);
}

// co_spawn 便捷封装
template <typename F>
void coSpawn(boost::asio::io_context& ioCtx, F&& coroutine)
{
    boost::asio::co_spawn(ioCtx, std::forward<F>(coroutine),
                          boost::asio::detached);
}
```

> **`this_coro::executor`**：在协程内获取当前执行器，无需传入 `io_context` 引用。这样 `sleep()` 可以在任何协程中直接调用。

### 4.7 SSL 集成

> 源码：`src/core/SslContext.h/cpp`

Hical 封装了 `boost::asio::ssl::context`，提供简洁的配置接口：

```cpp
// 使用方式
sslCtx = std::make_shared<SslContext>(ssl::context::tls_server);
sslCtx->loadCertificate("server.crt");   // use_certificate_chain_file
sslCtx->loadPrivateKey("server.key");     // use_private_key_file
```

在 `GenericConnection.h` 中，通过 `if constexpr` 在编译期区分 SSL 和普通连接：

```cpp
template <typename SocketType>
Awaitable<void> GenericConnection<SocketType>::sslHandshake()
{
    if constexpr (hIsSslStream<SocketType>)
    {
        // 只有 SSL 连接才执行握手
        co_await socket_.async_handshake(
            ssl::stream_base::server, use_awaitable);
    }
    co_return;
}
```

> **零开销抽象**：`if constexpr` 在编译期展开，普通 TCP 连接的代码中不会包含任何 SSL 逻辑。

---

## 5. 练习题

### 练习 1：协程式 Echo Server

编写一个协程式 TCP echo server（参考 2.3 节），支持多个并发连接。要求：
- 监听端口 8888
- 每个连接独立协程处理
- 正确处理 EOF（客户端断开）

### 练习 2：周期性日志

使用 `steady_timer` 实现一个 `PeriodicLogger` 协程：每隔 N 秒向标准输出写一行带时间戳的日志。要求：
- 使用 `co_await timer.async_wait(use_awaitable)` 模式
- 运行 10 次后自动退出

### 练习 3：多 io_context 模型

将练习 1 的 echo server 改为多 io_context 模型：
- 主线程运行 accept 循环
- 创建 N 个 worker 线程，每个持有独立的 `io_context`
- 新连接通过 round-robin 分配到 worker

### 练习 4：SSL Echo Server

为练习 1 添加 SSL 支持：
- 使用自签名证书（`openssl req -x509 -nodes -newkey rsa:2048 -keyout key.pem -out cert.pem`）
- 用 `openssl s_client -connect localhost:8888` 测试

### 练习 5：协程式 HTTP 客户端

编写一个协程式 HTTP 客户端：
- 支持 GET 请求
- 解析 HTTP 响应的 status code 和 body
- 支持 keep-alive（可选）

---

## 6. 总结与拓展阅读

### 核心 API 速查表

| API                                     | 用途                    |
| --------------------------------------- | ----------------------- |
| `io_context`                            | 事件循环核心            |
| `io_context::run()`                     | 阻塞执行事件循环        |
| `make_work_guard()`                     | 防止 run() 提前退出     |
| `post(ioCtx, fn)`                       | 异步投递任务            |
| `dispatch(ioCtx, fn)`                   | 同线程直接执行/异步投递 |
| `co_spawn(ioCtx, coro, detached)`       | 启动协程                |
| `co_await xxx.async_yyy(use_awaitable)` | 协程式异步调用          |
| `this_coro::executor`                   | 协程内获取执行器        |
| `steady_timer`                          | 定时器                  |
| `tcp::acceptor`                         | TCP 连接接受器          |
| `tcp::socket`                           | TCP 套接字              |
| `ssl::context`                          | SSL 配置上下文          |
| `ssl::stream<tcp::socket>`              | SSL 加密套接字          |
| `signal_set`                            | 信号处理                |

### 三种异步模式对比

|            | 回调              | Future         | 协程         |
| ---------- | ----------------- | -------------- | ------------ |
| 代码风格   | 嵌套回调链        | 阻塞式         | 线性同步风格 |
| 错误处理   | `error_code` 参数 | 异常           | `try/catch`  |
| 性能       | 最优              | 差（阻塞线程） | 接近回调     |
| 可读性     | 差（回调地狱）    | 好             | **最好**     |
| Hical 使用 | 定时器回调        | 未使用         | **主要方式** |

### 拓展阅读

- [Boost.Asio 官方文档](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html)
- [Asio C++20 Coroutines](https://www.boost.org/doc/libs/release/doc/html/boost_asio/overview/composition/cpp20_coroutines.html)
- [C++20 协程标准提案 (P0912)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0912r5.html)

### 下一步

Asio 提供了 TCP 层的异步 I/O。在 [课程 3: Boost.Beast]({{< relref "posts/Boost.Beast_HTTP与WebSocket.md" >}}) 中，你将学习如何在 Asio 之上构建 HTTP/WebSocket 协议——Beast 用 Asio 的 socket 和 buffer，加上 HTTP 解析器，实现完整的 Web 协议栈。
