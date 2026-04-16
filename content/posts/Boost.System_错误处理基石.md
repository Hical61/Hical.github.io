+++
title = 'Boost.System 学习课程：错误处理基石'
date = 2026-04-14T01:00:00+08:00
draft = false
tags = ["Boost", "Boost.System", "error_code", "C++", "Hical"]
categories = ["Boost学习课程"]
description = "理解 Boost.System 的 error_code + error_category 体系，掌握 I/O 操作的两种错误处理模式，以及 Hical 的跨平台错误码映射层。"
+++

> **课程导航**：[学习路径]({{< relref "posts/Boost库学习课程_学习路径导航.md" >}}) | **Boost.System** | [Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}}) | [Boost.Beast]({{< relref "posts/Boost.Beast_HTTP与WebSocket.md" >}}) | [Boost.JSON]({{< relref "posts/Boost.JSON_序列化与反序列化.md" >}})

## 前置知识

- C++ 基础（类、模板、异常处理）
- 了解操作系统错误码概念（errno、GetLastError）

## 学习目标

完成本课程后，你将能够：
1. 理解 `error_code` + `error_category` 的设计原理
2. 掌握 I/O 操作中 **错误码** 和 **异常** 两种错误处理模式
3. 编写自定义 `error_category`
4. 读懂 Hical 的跨平台错误码映射层

---

## 目录

- [前置知识](#前置知识)
- [学习目标](#学习目标)
- [目录](#目录)
- [1. 核心概念](#1-核心概念)
  - [1.1 为什么需要统一的错误码体系](#11-为什么需要统一的错误码体系)
  - [1.2 error\_code 三要素](#12-error_code-三要素)
  - [1.3 error\_category 体系](#13-error_category-体系)
  - [1.4 error\_condition vs error\_code](#14-error_condition-vs-error_code)
- [2. 基础用法](#2-基础用法)
  - [2.1 创建和检查 error\_code](#21-创建和检查-error_code)
  - [2.2 两种错误处理模式](#22-两种错误处理模式)
  - [2.3 常见错误码速查表](#23-常见错误码速查表)
- [3. 进阶主题](#3-进阶主题)
  - [3.1 自定义 error\_category](#31-自定义-error_category)
  - [3.2 跨平台错误码映射](#32-跨平台错误码映射)
- [4. Hical 实战解读](#4-hical-实战解读)
  - [4.1 Error.h：框架级错误码枚举](#41-errorh框架级错误码枚举)
  - [4.2 Error.cpp：fromBoostError 跨平台映射](#42-errorcppfromboosterror-跨平台映射)
  - [4.3 错误码在连接管理中的使用](#43-错误码在连接管理中的使用)
  - [4.4 设计模式总结](#44-设计模式总结)
- [5. 练习题](#5-练习题)
  - [练习 1：error\_code 基础](#练习-1error_code-基础)
  - [练习 2：自定义 error\_category](#练习-2自定义-error_category)
  - [练习 3：阅读源码](#练习-3阅读源码)
- [6. 总结与拓展阅读](#6-总结与拓展阅读)
  - [核心要点](#核心要点)
  - [拓展阅读](#拓展阅读)
  - [下一步](#下一步)

---

## 1. 核心概念

### 1.1 为什么需要统一的错误码体系

**C 语言 errno 的问题**：
- 全局变量（即使 thread-local 化，仍然语义模糊）
- 不同库对同一数值的含义不同
- 跨平台差异：Windows 用 `GetLastError()` + `WSAGetLastError()`，POSIX 用 `errno`

**C++ 异常 vs 错误码**：

| 特性     | 异常 (exception)         | 错误码 (error_code)    |
| -------- | ------------------------ | ---------------------- |
| 语法     | try/catch，代码清晰      | if 判断，稍显啰嗦      |
| 性能     | 抛出时开销大             | 几乎零开销             |
| 适用场景 | 致命错误、配置错误       | 可恢复的 I/O 错误      |
| 强制处理 | 不强制（可能忘记 catch） | 不强制（可能忘记检查） |

在网络编程中，`connection_reset`、`eof` 等是**常规事件**，不是异常情况。如果每次断连都抛异常，性能开销不可接受。Boost.System 提供了统一的错误码体系，让这类场景可以用零开销的方式处理。

### 1.2 error_code 三要素

`boost::system::error_code` 由三个部分组成：

```
┌───────────────────────────────────┐
│         error_code                │
├───────────────────────────────────┤
│  value()    → int (错误码数值)     │
│  category() → error_category&     │
│  message()  → string (人类可读)    │
└───────────────────────────────────┘
```

- **value()**：整型错误码，不同 category 下相同数值含义不同
- **category()**：错误类别引用，决定数值的解释方式
- **message()**：调用 `category().message(value())` 生成人类可读描述

**关键特性**：
- `error_code` 可以隐式转 `bool`——值为 0 表示无错误（`false`），非 0 表示有错误（`true`）
- 空 `error_code`（默认构造）等同于 "无错误"

**与 `std::error_code` 的关系**：

C++11 标准库吸收了 Boost.System 的设计，`std::error_code` 和 `boost::system::error_code` 接口几乎一致。Boost.Asio 使用 `boost::system::error_code`，但两者可以互转。

### 1.3 error_category 体系

Boost.System 预定义了两个核心 category：

| Category             | 说明               | 来源                                       |
| -------------------- | ------------------ | ------------------------------------------ |
| `system_category()`  | 操作系统原生错误码 | Windows: `GetLastError()`，POSIX: `errno`  |
| `generic_category()` | POSIX 通用错误码   | 跨平台统一（`ECONNRESET`、`ETIMEDOUT` 等） |

**为什么需要两个 category？**

同一个 "连接被重置" 的错误：
- 在 Linux 上：`errno = ECONNRESET (104)`
- 在 Windows 上：`WSAGetLastError() = WSAECONNRESET (10054)`
- 在 generic_category 中：统一为 `errc::connection_reset`

```
                   system_category()
                  ┌──────────────────┐
    Linux:        │ value = 104      │  → "Connection reset by peer"
    Windows:      │ value = 10054    │  → "Connection reset by peer"
                  └──────────────────┘

                   generic_category()
                  ┌──────────────────┐
    跨平台:       │ errc::connection │  → "Connection reset by peer"
                  │     _reset       │
                  └──────────────────┘
```

### 1.4 error_condition vs error_code

- **error_code**：精确的、平台相关的错误（"Windows 错误码 10054"）
- **error_condition**：可移植的错误条件（"连接被重置"）

`error_code` 可以与 `error_condition` 比较——这是跨平台判断错误类型的推荐方式：

```cpp
boost::system::error_code ec = ...;  // 可能来自 Windows 或 Linux

// 跨平台判断：不管底层是 ECONNRESET 还是 WSAECONNRESET
if (ec == boost::asio::error::connection_reset)
{
    // 连接被重置
}
```

> **核心思想**：`error_code` 描述 "到底发生了什么"，`error_condition` 描述 "这类错误意味着什么"。

---

## 2. 基础用法

### 2.1 创建和检查 error_code

```cpp
// example_error_code_basics.cpp
// 编译：g++ -std=c++20 example_error_code_basics.cpp -lboost_system -o example

#include <boost/system/error_code.hpp>
#include <boost/asio/error.hpp>
#include <iostream>

int main()
{
    // 1. 默认构造：无错误
    boost::system::error_code ok;
    std::cout << "ok: value=" << ok.value()
              << " bool=" << static_cast<bool>(ok)
              << " message=\"" << ok.message() << "\"\n";
    // 输出：ok: value=0 bool=0 message="Success"

    // 2. 从 Asio 错误常量构造
    boost::system::error_code eof = boost::asio::error::eof;
    std::cout << "eof: value=" << eof.value()
              << " bool=" << static_cast<bool>(eof)
              << " category=" << eof.category().name()
              << " message=\"" << eof.message() << "\"\n";
    // 输出：eof: value=2 bool=1 category=asio.misc message="End of file"

    // 3. 布尔判断（最常用）
    if (!ok)
    {
        std::cout << "ok 表示无错误\n";
    }
    if (eof)
    {
        std::cout << "eof 表示有错误: " << eof.message() << "\n";
    }

    // 4. 相等比较
    if (eof == boost::asio::error::eof)
    {
        std::cout << "确认是 EOF 错误\n";
    }

    return 0;
}
```

### 2.2 两种错误处理模式

**模式 A：错误码参数（推荐用于可恢复错误）**

```cpp
// 接受连接（错误码版本）
boost::system::error_code ec;
auto socket = acceptor.accept(ec);

if (ec == boost::asio::error::operation_aborted)
{
    // 服务器正在关闭，正常退出
    return;
}
if (ec)
{
    // 其他错误：记录日志并继续
    std::cerr << "Accept failed: " << ec.message() << "\n";
    continue;
}
// 成功：处理连接
```

**模式 B：异常（适用于致命错误）**

```cpp
// 接受连接（异常版本）——不传 error_code 参数
try
{
    auto socket = acceptor.accept();  // 失败时抛 system_error
    // 成功：处理连接
}
catch (const boost::system::system_error& e)
{
    // e.code() 返回 error_code
    // e.what() 返回错误描述
    std::cerr << "Fatal: " << e.what() << "\n";
    throw;  // 致命错误，向上传播
}
```

**什么时候用哪种？**

```
                      这个错误可恢复吗？
                      ┌─────┴─────┐
                     是            否
                      │            │
               用 error_code    用异常 / system_error
               （继续运行）      （中止或向上传播）

    示例：                     示例：
    • 客户端断连               • SSL 证书加载失败
    • 读取超时                 • 端口已被占用
    • 临时资源不足             • 配置文件无效
```

### 2.3 常见错误码速查表

以下是 Boost.Asio 中最常遇到的错误码：

| 错误码常量                               | 含义             | 典型触发场景                    |
| ---------------------------------------- | ---------------- | ------------------------------- |
| `boost::asio::error::eof`                | 对端正常关闭连接 | 读取时对方 close()              |
| `boost::asio::error::operation_aborted`  | 操作被取消       | 定时器 cancel()、socket close() |
| `boost::asio::error::connection_reset`   | 连接被对端重置   | 对方进程崩溃                    |
| `boost::asio::error::connection_refused` | 连接被拒绝       | 目标端口无服务监听              |
| `boost::asio::error::timed_out`          | 连接超时         | TCP 握手超时                    |
| `boost::asio::error::address_in_use`     | 地址已被占用     | bind() 端口冲突                 |
| `boost::asio::error::broken_pipe`        | 管道破裂         | 向已关闭连接写入                |
| `boost::asio::error::would_block`        | 资源暂时不可用   | 非阻塞 I/O 无数据               |
| `boost::asio::error::no_descriptors`     | 文件描述符不足   | 连接数超系统限制                |
| `boost::asio::error::access_denied`      | 权限不足         | bind() 特权端口                 |

---

## 3. 进阶主题

### 3.1 自定义 error_category

当你的应用有自己的错误码体系时，可以扩展 Boost.System：

```cpp
// example_custom_category.cpp
// 编译：g++ -std=c++20 example_custom_category.cpp -lboost_system -o example

#include <boost/system/error_code.hpp>
#include <iostream>
#include <string>

// 第一步：定义错误码枚举
enum class HttpError
{
    hOk = 0,
    hBadRequest = 400,
    hUnauthorized = 401,
    hForbidden = 403,
    hNotFound = 404,
    hInternalServerError = 500,
};

// 第二步：实现 error_category 子类
class HttpErrorCategory : public boost::system::error_category
{
public:
    const char* name() const noexcept override
    {
        return "http";
    }

    std::string message(int ev) const override
    {
        switch (static_cast<HttpError>(ev))
        {
            case HttpError::hOk:
                return "OK";
            case HttpError::hBadRequest:
                return "Bad Request";
            case HttpError::hUnauthorized:
                return "Unauthorized";
            case HttpError::hForbidden:
                return "Forbidden";
            case HttpError::hNotFound:
                return "Not Found";
            case HttpError::hInternalServerError:
                return "Internal Server Error";
            default:
                return "Unknown HTTP error";
        }
    }
};

// 第三步：全局单例（保证地址唯一）
const HttpErrorCategory& httpCategory()
{
    static HttpErrorCategory instance;
    return instance;
}

// 第四步：make_error_code 重载（启用隐式转换）
boost::system::error_code make_error_code(HttpError e)
{
    return {static_cast<int>(e), httpCategory()};
}

// 第五步：特化 is_error_code_enum（启用枚举 → error_code 自动转换）
namespace boost::system
{
    template <>
    struct is_error_code_enum<HttpError> : std::true_type
    {
    };
} // namespace boost::system

int main()
{
    // 枚举自动转换为 error_code
    boost::system::error_code ec = HttpError::hNotFound;

    std::cout << "category: " << ec.category().name() << "\n";   // "http"
    std::cout << "value:    " << ec.value() << "\n";              // 404
    std::cout << "message:  " << ec.message() << "\n";            // "Not Found"
    std::cout << "is error: " << static_cast<bool>(ec) << "\n";   // 1

    // 可以和其他 error_code 统一处理
    if (ec)
    {
        std::cout << "错误 [" << ec.category().name() << ":"
                  << ec.value() << "] " << ec.message() << "\n";
    }

    return 0;
}
```

### 3.2 跨平台错误码映射

同一个网络错误在不同平台上有不同的数值：

| 错误含义   | POSIX errno          | Windows WSA               |
| ---------- | -------------------- | ------------------------- |
| 连接重置   | `ECONNRESET` (104)   | `WSAECONNRESET` (10054)   |
| 连接拒绝   | `ECONNREFUSED` (111) | `WSAECONNREFUSED` (10061) |
| 连接超时   | `ETIMEDOUT` (110)    | `WSAETIMEDOUT` (10060)    |
| 地址占用   | `EADDRINUSE` (98)    | `WSAEADDRINUSE` (10048)   |
| 网络不可达 | `ENETUNREACH` (101)  | `WSAENETUNREACH` (10051)  |

Boost.Asio 的错误常量（如 `boost::asio::error::connection_reset`）已经做了平台抽象，**直接用 Asio 常量比较是最安全的跨平台方式**。

但如果你需要处理 `system_category()` 或 `generic_category()` 下的原始系统错误，就必须自己做映射——这正是 Hical 的 `Error.cpp` 所做的事情。

---

## 4. Hical 实战解读

### 4.1 Error.h：框架级错误码枚举

> 源码：`src/core/Error.h`

Hical 不直接向上层暴露 Boost 错误码，而是定义了**框架自己的错误码枚举**：

```cpp
enum class ErrorCode : uint32_t
{
    hNoError = 0,

    // ============ 连接错误 ============
    hEof,                  // 对端正常关闭
    hConnectionReset,      // 连接被重置
    hConnectionRefused,    // 连接被拒绝
    hTimedOut,             // 连接超时
    // ...

    // ============ SSL/TLS 错误 ============
    hSslHandshakeError,    // SSL 握手失败
    hSslInvalidCertificate,
    hSslProtocolError,

    hUnknown = 0xFFFF
};
```

**设计思路**：
- 上层业务代码**不直接依赖** `boost::system::error_code`
- 所有 Boost 错误先经过 `fromBoostError()` 转换为框架错误码
- 好处：如果未来替换底层网络库（如换掉 Asio），上层代码无需修改

同时还封装了 `NetworkError` 结构体，组合错误码和描述信息，并提供便捷查询方法（`isEof()`、`isCancelled()`）：

```cpp
struct NetworkError
{
    ErrorCode code {ErrorCode::hNoError};
    std::string message;

    explicit operator bool() const { return code != ErrorCode::hNoError; }
    bool ok() const { return code == ErrorCode::hNoError; }
    bool isEof() const { return code == ErrorCode::hEof; }
    bool isCancelled() const { return code == ErrorCode::hOperationAborted; }
};
```

### 4.2 Error.cpp：fromBoostError 跨平台映射

> 源码：`src/core/Error.cpp:11-153`

这是本课程的核心示范——如何构建一个**生产级的跨平台错误码映射层**。

**第一层映射：Asio 错误常量**（`Error.cpp:18-78`）

```cpp
ErrorCode fromBoostError(const boost::system::error_code& ec)
{
    if (!ec)
        return ErrorCode::hNoError;

    // Asio 提供的跨平台错误常量，直接匹配
    if (ec == boost::asio::error::eof)
        return ErrorCode::hEof;
    if (ec == boost::asio::error::connection_reset)
        return ErrorCode::hConnectionReset;
    if (ec == boost::asio::error::operation_aborted)
        return ErrorCode::hOperationAborted;
    // ...
}
```

> **要点**：`boost::asio::error::xxx` 是跨平台的，优先用它们做比较。

**第二层映射：系统原始错误码**（`Error.cpp:80-150`）

当 Asio 常量无法覆盖时，回退到平台相关的原始值：

```cpp
auto value = ec.value();
auto& category = ec.category();

if (category == boost::system::system_category()
    || category == boost::system::generic_category())
{
#ifdef _WIN32
    switch (value)
    {
        case WSAECONNRESET:    return ErrorCode::hConnectionReset;
        case WSAECONNREFUSED:  return ErrorCode::hConnectionRefused;
        case WSAETIMEDOUT:     return ErrorCode::hTimedOut;
        // ...
    }
#else
    switch (value)
    {
        case ECONNRESET:       return ErrorCode::hConnectionReset;
        case ECONNREFUSED:     return ErrorCode::hConnectionRefused;
        case ETIMEDOUT:        return ErrorCode::hTimedOut;
        // ...
    }
#endif
}

return ErrorCode::hUnknown;  // 未知错误
```

> **要点**：这里用了 `#ifdef _WIN32` 条件编译，因为 `WSAECONNRESET` 和 `ECONNRESET` 是不同平台的宏。

**映射流程图**：

```
boost::system::error_code
    │
    ├─ 匹配 Asio 常量？ ──→ 是 ──→ 直接返回对应 ErrorCode
    │
    └─ 否 ──→ 检查 category 是 system 或 generic？
              │
              ├─ Windows ──→ switch(WSA 错误码)
              │
              └─ POSIX   ──→ switch(errno 值)
              │
              └─ 都不匹配 ──→ ErrorCode::hUnknown
```

### 4.3 错误码在连接管理中的使用

**场景 1：定时器回调中的 error_code**

> 源码：`src/asio/AsioTimer.cpp`

定时器的 `async_wait` 回调会收到一个 `error_code` 参数：
- `!ec`（无错误）= 定时器正常到期，执行回调
- `ec == operation_aborted` = 定时器被 `cancel()` 了，不执行回调

**场景 2：Accept 循环中的 system_error 异常**

> 源码：`src/core/HttpServer.cpp:170-202`

```cpp
Awaitable<void> HttpServer::acceptLoop()
{
    while (running_.load())
    {
        try
        {
            // 协程式 async_accept 在失败时抛 system_error
            auto socket = co_await acceptor_->async_accept(
                boost::asio::use_awaitable);
            // ...
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == boost::asio::error::operation_aborted
                || e.code() == boost::asio::error::bad_descriptor)
            {
                break;  // 服务器正在关闭，正常退出循环
            }
            // 其他错误（如 EMFILE）：忽略并继续接受
        }
    }
}
```

> **要点**：协程模式下，Asio 默认**抛异常**（因为没有传 `error_code` 参数的地方）。所以必须用 try/catch 包裹。

**场景 3：HTTP 请求处理中的错误分支**

> 源码：`src/core/HttpServer.cpp:331-348`

```cpp
catch (const beast::system_error& e)
{
    if (e.code() == http::error::body_limit)
    {
        // 请求体超过限制 → 返回 413
    }
    else if (e.code() != beast::errc::not_connected
             && e.code() != boost::asio::error::eof)
    {
        // 忽略正常关闭，只处理异常错误
    }
}
```

### 4.4 设计模式总结

Hical 中错误处理的经验法则：

```
                      错误类型？
               ┌────────┴────────┐
            I/O 操作            配置 / 初始化
               │                    │
         协程中？                用异常
        ┌───┴───┐              （SslContext 加载证书
       是       否              acceptor bind 端口）
        │        │
   try/catch   error_code 参数
   (system_     (定时器回调)
    error)
```

| 场景               | 错误处理方式             | 示例                               |
| ------------------ | ------------------------ | ---------------------------------- |
| 协程中的异步 I/O   | try/catch `system_error` | `co_await async_accept()`          |
| 回调中的异步 I/O   | `error_code` 参数检查    | `timer.async_wait([](ec) { ... })` |
| 同步操作（可恢复） | `error_code` 输出参数    | `socket.close(ec)`                 |
| 配置 / 初始化错误  | 抛异常向上传播           | 证书加载失败                       |

---

## 5. 练习题

### 练习 1：error_code 基础

编写一个程序，使用 `boost::asio::ip::tcp::resolver` 解析域名。分别演示：
- 解析 `"www.google.com"` 成功时 `error_code` 的状态
- 解析一个不存在的域名（如 `"this.domain.does.not.exist.invalid"`）失败时的 `error_code`

**提示**：使用 `resolver.resolve(host, service, ec)` 的 error_code 重载版本。

### 练习 2：自定义 error_category

定义一个 `GameErrorCategory`，包含以下错误码：
- `PlayerNotFound = 1`
- `InventoryFull = 2`
- `InsufficientGold = 3`
- `LevelTooLow = 4`

实现完整的 `error_category`、`make_error_code`、`is_error_code_enum` 特化，并在 main 中演示使用。

### 练习 3：阅读源码

阅读 Hical 的 `src/core/Error.cpp`，回答以下问题：
1. `fromBoostError()` 为什么要做两层映射（先 Asio 常量，再平台原始值）？
2. 如果 `error_code` 的 category 既不是 `system_category` 也不是 `generic_category`（比如 SSL 错误），会返回什么？
3. `toNetworkError()` 函数的 `message` 字段来自哪里？

---

## 6. 总结与拓展阅读

### 核心要点

| 概念              | 要点                             |
| ----------------- | -------------------------------- |
| `error_code`      | 值 + 类别 + 描述，可隐式转 bool  |
| `error_category`  | 解释错误码含义的上下文           |
| `system_error`    | 包裹 error_code 的异常类         |
| `error_condition` | 可移植的错误条件，用于跨平台比较 |

### 拓展阅读

- [Boost.System 官方文档](https://www.boost.org/doc/libs/release/libs/system/doc/html/system.html)
- [C++ `<system_error>` 标准提案 (N2241)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2241.html)
- [Boost.Asio Error Handling](https://www.boost.org/doc/libs/release/doc/html/boost_asio/overview/core/error_handling.html)

### 下一步

error_code 是 Asio 一切异步操作的基础返回值。在 [课程 2: Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}}) 中，你将看到 error_code 如何在 `async_read`、`async_write`、`async_accept` 等操作中被实际使用。
