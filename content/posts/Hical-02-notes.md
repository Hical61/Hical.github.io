+++
title = '第2课：错误处理与网络地址'
date = '2026-04-15'
draft = false
tags = ["C++", "错误处理", "网络编程", "IPv6", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "理解框架如何将平台相关的错误码统一为内部枚举，掌握 InetAddress 对 IPv4/IPv6 的封装，以及 HTTP 类型枚举的设计。"
+++

# 第2课：错误处理与网络地址 - 学习笔记

> 理解框架如何将平台相关的错误码统一为内部枚举，掌握 InetAddress 对 IPv4/IPv6 的封装，以及 HTTP 类型枚举的设计。

---

## 一、ErrorCode — 统一错误码体系

### 1.1 问题：为什么不直接暴露 boost::system::error_code

Boost.Asio 的错误码体系有几个问题：

1. **平台差异**：同一个"连接重置"，Linux 是 `ECONNRESET`（104），Windows 是 `WSAECONNRESET`（10054）
2. **头文件污染**：使用 Boost 错误码需要 include Boost 头文件，上层业务代码不应该依赖 Boost
3. **语义不明确**：`ec.value()` 返回的整数对业务层没有直接意义

**Hical 的解决方案**：定义一套框架内部的 `ErrorCode` 枚举，在 `core/` 层统一语义。

### 1.2 ErrorCode 枚举设计

**源码位置**：`src/core/Error.h`

```cpp
enum class ErrorCode : uint32_t
{
    hNoError = 0,

    // 连接错误（6 种）
    hEof,                    // 对端正常关闭（EOF）
    hConnectionReset,        // 连接被对端重置（RST）
    hConnectionRefused,      // 连接被拒绝
    hTimedOut,               // 连接超时
    hConnectionInProgress,   // 连接进行中
    hConnectionAborted,      // 连接被中止

    // 地址错误（4 种）
    hAddressInUse,           // 地址已占用（端口冲突）
    hAddressNotAvailable,    // 地址不可用
    hNetworkUnreachable,     // 网络不可达
    hHostUnreachable,        // 主机不可达

    // 操作错误（6 种）
    hOperationAborted,       // 操作被取消
    hOperationInProgress,    // 操作进行中
    hBrokenPipe,             // 管道破裂（向已关闭连接写入）
    hPermissionDenied,       // 权限不足
    hTooManyOpenFiles,       // 文件描述符不足
    hWouldBlock,             // 资源暂不可用（非阻塞 IO）

    // SSL/TLS 错误（3 种）
    hSslHandshakeError,      // SSL 握手失败
    hSslInvalidCertificate,  // 证书无效
    hSslProtocolError,       // SSL 协议错误

    hUnknown = 0xFFFF        // 兜底
};
```

**设计要点**：
- 使用 `enum class` 强类型枚举，防止隐式转换
- 底层类型 `uint32_t`，空间充足便于未来扩展
- 按**错误类别**分组：连接 / 地址 / 操作 / SSL
- `hUnknown = 0xFFFF` 作为兜底值，所有无法映射的错误都归入此类

### 1.3 NetworkError 结构体

```cpp
struct NetworkError {
    ErrorCode code{ErrorCode::hNoError};
    std::string message;                    // 保留原始错误描述

    explicit operator bool() const;         // if (err) 判断是否有错误
    bool ok() const;                        // err.ok() 判断无错误
    bool isEof() const;                     // 对端正常断开？
    bool isCancelled() const;               // 操作被取消？
};
```

**关键方法的使用场景**：

| 方法              | 场景         | 示例                                             |
| ----------------- | ------------ | ------------------------------------------------ |
| `ok()`            | 操作成功检查 | `if (err.ok()) { /* 正常处理 */ }`               |
| `isEof()`         | 对端正常关闭 | 客户端主动断开，不是错误，无需告警               |
| `isCancelled()`   | 主动取消操作 | 调用 `timer.cancel()` 后回调收到此错误，正常忽略 |
| `operator bool()` | 快速错误判断 | `if (err) { LOG_ERROR(...); }`                   |

**游戏服务器类比**：
- `isEof()` → 玩家正常退出游戏（点了"退出"按钮）
- `isCancelled()` → 服务器主动断开连接（如踢人操作，cancel 掉所有待处理异步操作）
- `hConnectionReset` → 玩家网络断开（拔网线）
- `hTimedOut` → 心跳超时

### 1.4 fromBoostError — 三层映射策略

**源码位置**：`src/core/Error.cpp`

映射函数采用三层优先级策略：

```
第1层：Asio 错误码直接匹配
         boost::asio::error::eof → hEof
         boost::asio::error::connection_reset → hConnectionReset
         ...（约 15 种直接映射）
              │
              │ 匹配不到
              ↓
第2层：系统/POSIX 原始错误码（跨平台分支）
    ┌─ Windows: WSAECONNRESET → hConnectionReset
    │           WSAETIMEDOUT → hTimedOut
    │           ...
    └─ POSIX:   ECONNRESET → hConnectionReset
                ETIMEDOUT → hTimedOut
                ...
              │
              │ 仍然匹配不到
              ↓
第3层：兜底
         返回 hUnknown
```

**为什么需要两层？**

因为 Boost.Asio 的 `error_code` 来自两种不同的 category：
- `boost::asio::error` 命名空间 → Asio 自定义的跨平台错误码
- `boost::system::system_category()` / `generic_category()` → 操作系统原始错误码

某些错误只通过 Asio 的跨平台枚举报出，另一些可能直接用系统原始值。第2层的 `#ifdef _WIN32` 分支确保两种来源都能正确映射。

**Windows 特有的注意点**：

```cpp
#ifdef _WIN32
    case WSAECONNRESET:     return ErrorCode::hConnectionReset;    // 10054
    case WSAECONNREFUSED:   return ErrorCode::hConnectionRefused;  // 10061
    case WSAETIMEDOUT:      return ErrorCode::hTimedOut;           // 10060
    // ...
#else
    case ECONNRESET:        return ErrorCode::hConnectionReset;    // 104
    case ECONNREFUSED:      return ErrorCode::hConnectionRefused;  // 111
    case ETIMEDOUT:         return ErrorCode::hTimedOut;           // 110
    // ...
#endif
```

同一个语义错误在 Windows（WSAE* 系列，值 10000+）和 Linux（E* 系列，值 < 200）有完全不同的数值。框架在此层做了统一。

### 1.5 errorCodeToString — 人类可读描述

```cpp
const char* errorCodeToString(ErrorCode code);
```

返回静态字符串（`const char*`），无动态分配。switch-case 覆盖所有枚举值。

与 `NetworkError::message` 的区别：
- `errorCodeToString()` → 框架定义的通用描述（如 "Connection reset by peer"）
- `NetworkError::message` → 操作系统原始描述（如 Linux 的 "Connection reset by peer" 或 Windows 的 "An existing connection was forcibly closed"）

---

## 二、InetAddress — 网络地址封装

### 2.1 设计目标

将平台相关的 `sockaddr_in`（IPv4）和 `sockaddr_in6`（IPv6）统一封装为一个跨平台的地址类型。

**源码位置**：`src/core/InetAddress.h` / `src/core/InetAddress.cpp`

### 2.2 内存布局 — union 技巧

```cpp
class InetAddress {
private:
    union {
        struct sockaddr_in addr_;     // IPv4: 16 字节
        struct sockaddr_in6 addr6_;   // IPv6: 28 字节
    };
    bool isIpV6_{false};
};
```

使用 **union** 让 IPv4 和 IPv6 共享同一块内存：
- `sizeof(InetAddress)` = `sizeof(sockaddr_in6)` + padding + `bool` ≈ 32 字节
- 不需要为"可能是 v4 也可能是 v6"分配两份空间
- `isIpV6_` 标记当前使用哪个成员

### 2.3 构造方式

**方式1：从 IP 字符串 + 端口构造**

```cpp
InetAddress::InetAddress(const std::string& ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));

    // 先尝试 IPv4
    if (inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) == 1) {
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);    // 主机字节序 → 网络字节序
        isIpV6_ = false;
    } else {
        // 再尝试 IPv6
        std::memset(&addr6_, 0, sizeof(addr6_));
        if (inet_pton(AF_INET6, ip.c_str(), &addr6_.sin6_addr) == 1) {
            addr6_.sin6_family = AF_INET6;
            addr6_.sin6_port = htons(port);
            isIpV6_ = true;
        }
    }
}
```

关键函数：
- `inet_pton()`：将文本 IP 地址转为二进制格式（Presentation to Network）
- `htons()`：Host TO Network Short，将 16 位端口号从主机字节序转为网络字节序（大端）

**为什么要字节序转换？**
网络协议规定使用大端序（Big-Endian），而 x86/x64 CPU 使用小端序（Little-Endian）。例如端口 8080：
- 主机字节序（小端）：`0x90 0x1F`
- 网络字节序（大端）：`0x1F 0x90`

**方式2：从 sockaddr 构造（Asio 回调中使用）**

```cpp
explicit InetAddress(const struct sockaddr_in& addr);    // 接受连接后获得的地址
explicit InetAddress(const struct sockaddr_in6& addr);
```

### 2.4 地址转换方法

```cpp
std::string toIp() const;       // → "127.0.0.1" 或 "::1"
std::string toIpPort() const;   // → "127.0.0.1:8080" 或 "::1:8080"
uint16_t port() const;          // → 8080（已转回主机字节序）
```

`toIp()` 使用 `inet_ntop()`（Network TO Presentation）做反向转换。

`port()` 的实现值得注意：

```cpp
uint16_t InetAddress::port() const {
    return ntohs(isIpV6_ ? addr6_.sin6_port : addr_.sin_port);
}
```

`ntohs()` 将网络字节序转回主机字节序，这样用户拿到的 port 就是直觉上的 8080 而不是 0x901F。

### 2.5 为什么要同时支持 IPv4 和 IPv6

1. **IPv6 是趋势**：越来越多的部署环境（云平台、移动网络）优先使用 IPv6
2. **双栈兼容**：服务器通常需要同时监听 IPv4 和 IPv6
3. **框架透明**：上层代码不需要关心连接是 v4 还是 v6，统一用 `InetAddress`

### 2.6 跨平台头文件差异

```cpp
#ifdef _WIN32
    #include <winsock2.h>     // sockaddr_in, htons 等
    #include <ws2tcpip.h>     // inet_pton, inet_ntop
#else
    #include <netinet/in.h>   // sockaddr_in, sockaddr_in6
    #include <sys/socket.h>   // sockaddr
    #include <arpa/inet.h>    // inet_pton, inet_ntop (实现文件)
#endif
```

Windows 使用 Winsock2 API，Linux/macOS 使用 POSIX socket API。函数名和参数一致，但头文件不同。

---

## 三、HttpTypes — HTTP 枚举类型

### 3.1 HttpMethod 枚举

**源码位置**：`src/core/HttpTypes.h`

```cpp
enum class HttpMethod : uint8_t {
    hGet,       // GET
    hPost,      // POST
    hPut,       // PUT
    hDelete,    // DELETE
    hPatch,     // PATCH
    hHead,      // HEAD
    hOptions,   // OPTIONS
    hUnknown    // 未知方法
};
```

**设计要点**：
- `uint8_t` 底层类型：HTTP 方法数量极少，1 字节足够
- `hUnknown` 兜底：解析到非标准方法时不崩溃

**双向转换函数**：

```cpp
const char* httpMethodToString(HttpMethod method);       // hGet → "GET"
HttpMethod stringToHttpMethod(const std::string& str);   // "GET" → hGet
```

### 3.2 HttpStatusCode 枚举

```cpp
enum class HttpStatusCode : uint16_t {
    // 2xx 成功
    hOk = 200,                    hCreated = 201,
    hAccepted = 202,              hNoContent = 204,

    // 3xx 重定向
    hMovedPermanently = 301,      hFound = 302,
    hNotModified = 304,

    // 4xx 客户端错误
    hBadRequest = 400,            hUnauthorized = 401,
    hForbidden = 403,             hNotFound = 404,
    hMethodNotAllowed = 405,      hConflict = 409,
    hTooManyRequests = 429,

    // 5xx 服务端错误
    hInternalServerError = 500,   hNotImplemented = 501,
    hBadGateway = 502,            hServiceUnavailable = 503
};
```

**设计要点**：
- `uint16_t` 底层类型：状态码范围 100-599，2 字节足够
- 枚举值直接等于 HTTP 状态码数值（`hOk = 200`），方便与协议直接对应
- 按 RFC 标准分组：2xx/3xx/4xx/5xx
- 覆盖最常用的 17 个状态码

**转换函数**：

```cpp
const char* httpStatusCodeToString(HttpStatusCode code);  // hOk → "OK"
```

### 3.3 HTTP 枚举在框架中的使用位置

| 枚举             | 使用者                | 场景                                |
| ---------------- | --------------------- | ----------------------------------- |
| `HttpMethod`     | Router                | 路由匹配时比较请求方法              |
| `HttpMethod`     | HttpRequest           | 封装请求的 method 字段              |
| `HttpStatusCode` | HttpResponse          | 设置响应状态码                      |
| `HttpStatusCode` | HttpResponse 工厂方法 | `ok()`、`notFound()` 等快速构建响应 |

---

## 四、从测试看用法

### 4.1 错误码测试要点

**源码位置**：`tests/test_error.cpp`

测试覆盖了 3 个层面：

**1. Asio 错误码 → 框架错误码的映射**

```cpp
TEST(ErrorCodeTest, ConnectionReset) {
    auto ec = boost::asio::error::connection_reset;
    EXPECT_EQ(fromBoostError(ec), ErrorCode::hConnectionReset);
}
```

每种 Asio 错误码都有专门的测试用例。

**2. NetworkError 结构体的便捷方法**

```cpp
TEST(ErrorCodeTest, NetworkErrorStruct) {
    NetworkError eofErr{ErrorCode::hEof, "End of file"};
    EXPECT_FALSE(eofErr.ok());
    EXPECT_TRUE(eofErr.isEof());       // EOF 是"正常的错误"
    EXPECT_FALSE(eofErr.isCancelled());
}
```

**3. 未知错误的兜底处理**

```cpp
TEST(ErrorCodeTest, UnknownError) {
    boost::system::error_code ec(999, boost::system::generic_category());
    EXPECT_EQ(fromBoostError(ec), ErrorCode::hUnknown);

    auto err = toNetworkError(ec);
    EXPECT_EQ(err.code, ErrorCode::hUnknown);
    EXPECT_FALSE(err.message.empty());  // 即使是未知错误，也保留原始消息
}
```

### 4.2 HTTP 类型测试要点

**源码位置**：`tests/test_http_types.cpp`

这个测试文件除了 HttpTypes 外，还顺带测试了 HttpRequest 和 HttpResponse：

**双向转换正确性**：

```cpp
TEST(HttpTypesTest, StringToMethod) {
    EXPECT_EQ(stringToHttpMethod("GET"), HttpMethod::hGet);
    EXPECT_EQ(stringToHttpMethod("INVALID"), HttpMethod::hUnknown);  // 非法输入兜底
}
```

**HttpRequest 构建器模式**：

```cpp
TEST(HttpRequestTest, SetAndGetTarget) {
    HttpRequest req;
    req.setTarget("/api/users?page=1");

    EXPECT_EQ(req.path(), "/api/users");   // 自动分离路径
    EXPECT_EQ(req.query(), "page=1");      // 自动分离查询参数
}
```

**HttpResponse 工厂方法**：

```cpp
TEST(HttpResponseTest, FactoryJson) {
    auto res = HttpResponse::json({{"key", "value"}});
    EXPECT_EQ(res.statusCode(), HttpStatusCode::hOk);
    EXPECT_EQ(res.header("Content-Type"), "application/json");
}

TEST(HttpResponseTest, FactoryNotFound) {
    auto res = HttpResponse::notFound();
    EXPECT_EQ(res.statusCode(), HttpStatusCode::hNotFound);
    EXPECT_EQ(res.body(), "Not Found");
}
```

工厂方法（`ok()`、`json()`、`notFound()`、`badRequest()`、`serverError()`）的设计避免了重复设置状态码 + Content-Type 的样板代码。

---

## 五、设计模式总结

| 模式                | 应用                                        | 说明                              |
| ------------------- | ------------------------------------------- | --------------------------------- |
| **适配器 / 映射层** | `fromBoostError()`                          | 将 Boost 错误码适配为框架内部枚举 |
| **值对象**          | `NetworkError`、`InetAddress`               | 不可变语义，按值传递              |
| **union 节省空间**  | `InetAddress` 的 addr_ / addr6_             | IPv4 和 IPv6 共享内存             |
| **工厂方法**        | `HttpResponse::ok()` / `json()`             | 简化响应对象的创建                |
| **强类型枚举**      | `ErrorCode`、`HttpMethod`、`HttpStatusCode` | 防止隐式转换错误                  |
| **条件编译**        | `#ifdef _WIN32`                             | 跨平台错误码和头文件差异处理      |

---

## 六、关键问题思考与回答

**Q1: 为什么不直接暴露 boost::system::error_code 给上层？**

> 三个原因：
> 1. **解耦**：上层代码（路由、中间件）不应该依赖 Boost 头文件
> 2. **跨平台统一**：同一个语义错误在 Windows/Linux 有不同的数值，框架层统一为一个枚举值
> 3. **语义明确**：`ErrorCode::hConnectionReset` 比 `ec.value() == 104` 可读性高得多

**Q2: NetworkError 的 ok()、isEof()、isCancelled() 在哪些场景下使用？**

> - `ok()`：检查操作是否成功，正常的业务流程判断
> - `isEof()`：区分"对端正常关闭"和"异常断开"。游戏服务器中，玩家正常退出（EOF）不需要告警，而连接被重置需要记录日志
> - `isCancelled()`：主动关闭连接/取消定时器时，异步回调会收到 `operation_aborted`。此时应正常忽略，而不是当成错误处理
>
> 在 GenericConnection 的 readLoop 中可以看到实际使用：
> ```cpp
> catch (const boost::system::system_error& e) {
>     if (e.code() != boost::asio::error::operation_aborted) {
>         handleClose();  // 非主动取消的错误才触发关闭
>     }
> }
> ```

**Q3: InetAddress 为什么要同时支持 sockaddr_in 和 sockaddr_in6？**

> 1. **协议无关性**：框架代码不应该硬编码 IPv4 或 IPv6，统一为 InetAddress
> 2. **双栈部署**：生产环境的服务器通常同时接受 IPv4 和 IPv6 连接
> 3. **零成本抽象**：union 让两种地址共享内存，`bool isIpV6_` 只增加 1 字节开销
> 4. **Asio 回调需要**：`async_accept` 回调中拿到的可能是 v4 也可能是 v6 的地址

---

## 七、与游戏服务器的对比

| Hical 概念                         | 游戏服务器等价物                       |
| ---------------------------------- | -------------------------------------- |
| `ErrorCode::hEof`                  | 玩家正常退出                           |
| `ErrorCode::hConnectionReset`      | 玩家掉线（网络异常）                   |
| `ErrorCode::hTimedOut`             | 心跳超时踢人                           |
| `ErrorCode::hAddressInUse`         | 服务器启动失败"端口被占用"             |
| `ErrorCode::hTooManyOpenFiles`     | 连接数达到系统 fd 上限                 |
| `ErrorCode::hBrokenPipe`           | 向已断开的客户端发消息                 |
| `InetAddress`                      | 客户端 IP + 端口记录（用于日志、封禁） |
| `HttpStatusCode::hTooManyRequests` | 频率限制 / 防刷                        |

---

*下一课：第3课 - Asio 事件循环与定时器，将深入 AsioEventLoop、AsioTimer 和 EventLoopPool 的具体实现。*
