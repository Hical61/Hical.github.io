+++
title = '用 if constexpr + 模板在一份代码中同时处理 TCP 和 SSL 连接'
date = '2026-05-04'
draft = false
tags = ["C++17", "if constexpr", "模板", "SSL", "Hical"]
categories = ["Hical框架"]
description = "以 Hical 框架的 GenericConnection 为例，用 C++17 if constexpr + 类型萃取在一个模板类中统一 TCP 和 SSL 连接。"
+++

# 用 if constexpr + 模板在一份代码中同时处理 TCP 和 SSL 连接

> 本文以 Hical 框架的 GenericConnection 为例，展示如何用 C++17 的 `if constexpr` + 类型萃取在一个模板类中统一 TCP 和 SSL 两种连接，实现编译期零开销分支。

---

## 问题：TCP 和 SSL 的代码高度相似

TCP 连接和 SSL 连接的区别只有三处：
1. **socket 类型不同**：`tcp::socket` vs `ssl::stream<tcp::socket>`
2. **连接建立多一步 TLS 握手**
3. **关闭多一步 `ssl::stream::async_shutdown`**

其余 99% 的代码——读循环、写循环、缓冲区管理、回调触发、状态机——完全相同。

### 传统方案：继承 + 虚函数

```cpp
class TcpConnection : public Connection {
    tcp::socket socket_;
    void doRead() override { ... }
};

class SslConnection : public Connection {
    ssl::stream<tcp::socket> socket_;
    void doRead() override { ... }  // 几乎一样的代码
};
```

问题：
- 两个类 90% 的代码重复
- 每次调用 `doRead/doWrite` 都经过虚函数表间接调用
- 修改共同逻辑需要同步两处

### Hical 方案：一个模板统一

```cpp
template <typename SocketType>
class GenericConnection : public TcpConnection {
    SocketType socket_;
    // 一份读循环、一份写循环、一份状态机
    // 差异部分用 if constexpr 处理
};

using PlainConnection = GenericConnection<tcp::socket>;
using SslConnection = GenericConnection<ssl::stream<tcp::socket>>;
```

## 核心技术：类型萃取 + if constexpr

### 类型萃取：编译期判断 socket 类型

```cpp
template <typename T>
struct IsSslStream : std::false_type {};

template <typename T>
struct IsSslStream<boost::asio::ssl::stream<T>> : std::true_type {};

template <typename T>
inline constexpr bool hIsSslStream = IsSslStream<T>::value;
```

这是一个经典的模板特化技巧：
- `IsSslStream<tcp::socket>` → `false_type`
- `IsSslStream<ssl::stream<tcp::socket>>` → `true_type`

### if constexpr：编译期分支消除

`if constexpr` 的关键特性：**被丢弃的分支不需要编译通过**。

```cpp
template <typename SocketType>
auto& GenericConnection<SocketType>::lowestLayerSocket()
{
    if constexpr (hIsSslStream<SocketType>)
    {
        return socket_.lowest_layer();  // SSL: 穿透获取底层 tcp::socket
    }
    else
    {
        return socket_;  // TCP: 本身就是 tcp::socket
    }
}
```

如果用普通 `if`，当 `SocketType = tcp::socket` 时，`socket_.lowest_layer()` 会编译失败，因为 `tcp::socket` 没有 `lowest_layer()` 方法。`if constexpr` 在编译期丢弃了这个分支，完全不参与编译。

## 三处差异的处理

### 差异一：获取底层 socket

TCP 的 `socket_` 本身就是 `tcp::socket`，SSL 的 `socket_` 是 `ssl::stream<tcp::socket>`，需要 `lowest_layer()` 获取底层 socket（用于 bind、connect、close 等操作）。

```cpp
auto& lowestLayerSocket()
{
    if constexpr (hIsSslStream<SocketType>)
        return socket_.lowest_layer();
    else
        return socket_;
}
```

### 差异二：连接建立

TCP 连接建立后直接可以读写。SSL 连接需要先完成 TLS 握手：

```cpp
void connectEstablished()
{
    state_.store(State::hConnected);

    if constexpr (hIsSslStream<SocketType>)
    {
        // SSL: 先执行 TLS 握手，成功后再触发回调
        co_spawn(executor(), [self = sharedThis()]() -> awaitable<void> {
            co_await self->socket_.async_handshake(
                ssl::stream_base::server, use_awaitable);
            if (self->connectionCallback_) self->connectionCallback_(self);
            self->startRead();
        }, detached);
    }
    else
    {
        // TCP: 直接触发回调并开始读取
        if (connectionCallback_) connectionCallback_(sharedThis());
        startRead();
    }
}
```

### 差异三：连接关闭

SSL 关闭需要先发送 TLS `close_notify` 通知对端：

```cpp
void shutdownInLoop()
{
    if constexpr (hIsSslStream<SocketType>)
    {
        // SSL: 先发 close_notify
        boost::system::error_code ec;
        socket_.shutdown(ec);  // TLS shutdown
        // 再关闭底层 TCP
        auto& sock = socket_.lowest_layer();
        sock.shutdown(tcp::socket::shutdown_send, ec);
    }
    else
    {
        // TCP: 直接关闭
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_send, ec);
    }
}
```

## 共享的 99%：读写循环

读循环对 TCP 和 SSL 完全相同——Boost.Asio 的 `async_read_some` 对两种 socket 类型都可用（SSL stream 也实现了 `AsyncReadStream` concept）：

```cpp
template <typename SocketType>
awaitable<void> GenericConnection<SocketType>::readLoop()
{
    while (reading_ && state_.load() == State::hConnected)
    {
        inputBuffer_.ensureWritableBytes(4096);
        auto bytesRead = co_await socket_.async_read_some(
            boost::asio::buffer(inputBuffer_.beginWrite(), inputBuffer_.writableBytes()),
            use_awaitable);

        bytesReceived_ += bytesRead;
        inputBuffer_.hasWritten(bytesRead);

        if (messageCallback_) messageCallback_(sharedThis(), &inputBuffer_);
    }
}
```

写循环同理。Scatter-Gather I/O 也是共享的——`async_write` 对两种 socket 都可用。

## 编译产物：零开销

当编译器实例化 `GenericConnection<tcp::socket>` 时：
- `hIsSslStream<tcp::socket>` = `false`
- 所有 `if constexpr (true for SSL)` 分支被**彻底删除**
- 不会生成任何 SSL 相关的代码和符号

当实例化 `GenericConnection<ssl::stream<tcp::socket>>` 时：
- `hIsSslStream<...>` = `true`
- SSL 分支的代码被保留

两个实例化各自只包含必要的代码。没有虚函数表、没有运行时判断、没有分支预测开销。

## 与虚函数方案的对比

| 维度           | if constexpr 模板              | 虚函数继承           |
| -------------- | ------------------------------ | -------------------- |
| 代码重复       | 一份代码                       | 两个子类，大量重复   |
| 运行时开销     | 零（编译期消除）               | vtable 查找          |
| 编译期类型安全 | 丢弃分支不编译                 | 基类指针丢失具体类型 |
| 可读性         | 差异清晰标注在 if constexpr 中 | 分散在两个类中       |
| 灵活性         | 可以用 SSL 特有的 API          | 受限于基类接口       |

## 局限性

`if constexpr` 不是万能的：
- **运行时切换**不可能——TCP 和 SSL 的选择在编译期确定。如果需要运行时根据配置决定用哪种连接，仍需要通过工厂模式 + 类型擦除
- **模板膨胀**——每种 SocketType 都生成一份完整的代码。但实际上只有 TCP 和 SSL 两种实例化，影响极小

Hical 的 `TcpServer` 通过 `enableSsl()` 配置在启动时确定连接类型，不存在运行时切换的需求，`if constexpr` 方案完美适配。

## 总结

`if constexpr` + 类型萃取是 C++17 模板编程的利器：**一份代码、两种行为、零运行时开销**。它不只适用于网络连接，任何"两种类型有 90% 共同逻辑 + 少量差异"的场景都可以用这个模式。

---

> 源码参考：[Hical/src/asio/GenericConnection.h](https://github.com/Hical61/Hical/blob/main/src/asio/GenericConnection.h)
> 项目地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
