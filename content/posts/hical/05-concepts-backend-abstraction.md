+++
title = '用 C++20 Concepts 设计可替换的网络后端：从 Boost.Asio 到未来的 io_uring'
date = '2026-04-12'
draft = false
tags = ["C++20", "Concepts", "网络后端", "io_uring", "Hical"]
categories = ["Hical框架"]
description = "以 Hical 框架为例，展示如何用 C++20 Concepts 约束网络后端接口，实现编译期类型安全的后端抽象。"
+++

# 用 C++20 Concepts 设计可替换的网络后端：从 Boost.Asio 到未来的 io_uring

> 本文以 Hical 框架为例，展示如何用 C++20 Concepts 约束网络后端接口，实现编译期类型安全的后端抽象。

---

## 问题：网络后端绑定的困境

大多数 C++ 网络框架和底层网络库深度绑定。Drogon 绑定 Trantor，muduo 绑定自研的 EventLoop。一旦想换后端（比如从 epoll 切到 io_uring），基本等于重写。

原因是传统的抽象手段——虚函数继承——有两个问题：
1. **运行时开销**：每次调用都经过 vtable
2. **接口松散**：基类定义了接口，但"你的实现是否真的完整？"只能在链接期或运行时才知道

## Concepts：编译期的接口约束

C++20 Concepts 提供了一种**具名约束**机制——在编译期验证类型是否满足一组要求：

```cpp
template <typename T>
concept EventLoopLike = requires(T loop, std::function<void()> func, double delay) {
    { loop.run() } -> std::same_as<void>;
    { loop.stop() } -> std::same_as<void>;
    { loop.isRunning() } -> std::convertible_to<bool>;
    { loop.post(func) } -> std::same_as<void>;
    { loop.dispatch(func) } -> std::same_as<void>;
    { loop.runAfter(delay, func) } -> std::convertible_to<uint64_t>;
    { loop.runEvery(delay, func) } -> std::convertible_to<uint64_t>;
    { loop.cancelTimer(uint64_t{}) } -> std::same_as<void>;
    { loop.isInLoopThread() } -> std::convertible_to<bool>;
    { loop.index() } -> std::convertible_to<size_t>;
    { loop.allocator() } -> std::same_as<std::pmr::polymorphic_allocator<std::byte>>;
};
```

如果某个类型缺少 `run()` 方法或返回类型不对，编译器**立即报错**，而不是在链接时给出晦涩的"未定义引用"。

## Hical 的四层 Concept 设计

Hical 定义了 4 个核心 Concept，形成层级约束：

```
NetworkBackend (组合约束)
├── EventLoopType   必须满足 EventLoopLike
├── ConnectionType  必须满足 TcpConnectionLike
└── TimerType       必须满足 TimerLike
```

### EventLoopLike：事件循环约束

要求：生命周期管理（run/stop）、任务调度（post/dispatch）、定时器、线程属性、PMR 分配器。

### TcpConnectionLike：连接约束

```cpp
template <typename T>
concept TcpConnectionLike = requires(T conn, const char* data, size_t len, const std::string& msg) {
    { conn.send(data, len) } -> std::same_as<void>;
    { conn.send(msg) } -> std::same_as<void>;
    { conn.shutdown() } -> std::same_as<void>;
    { conn.close() } -> std::same_as<void>;
    { conn.connected() } -> std::convertible_to<bool>;
    { conn.disconnected() } -> std::convertible_to<bool>;
    { conn.bytesSent() } -> std::convertible_to<size_t>;
    { conn.bytesReceived() } -> std::convertible_to<size_t>;
};
```

### TimerLike：定时器约束

```cpp
template <typename T>
concept TimerLike = requires(T timer) {
    { timer.cancel() } -> std::same_as<void>;
    { timer.isActive() } -> std::convertible_to<bool>;
    { timer.isRepeating() } -> std::convertible_to<bool>;
    { timer.interval() } -> std::convertible_to<double>;
};
```

### NetworkBackend：组合约束

```cpp
template <typename T>
concept NetworkBackend =
    requires {
        typename T::EventLoopType;
        typename T::ConnectionType;
        typename T::TimerType;
    }
    && EventLoopLike<typename T::EventLoopType>
    && TcpConnectionLike<typename T::ConnectionType>
    && TimerLike<typename T::TimerType>;
```

这是一个**组合 Concept**——不仅要求三个关联类型存在，还要求每个关联类型满足对应的子 Concept。

## Asio 后端：当前的默认实现

```cpp
struct AsioBackend
{
    using EventLoopType = AsioEventLoop;
    using ConnectionType = TcpConnection;  // GenericConnection<tcp::socket>
    using TimerType = AsioTimer;
};
```

三行代码就定义了一个完整的后端。编译器会自动验证：
- `AsioEventLoop` 确实有 `run()`、`post()` 等方法
- `TcpConnection` 确实有 `send()`、`shutdown()` 等方法
- `AsioTimer` 确实有 `cancel()`、`isActive()` 等方法

## 编译期验证：static_assert

```cpp
// tests/test_concepts.cpp

// 正向验证：满足约束的类型
static_assert(EventLoopLike<AsioEventLoop>);
static_assert(TimerLike<AsioTimer>);
static_assert(TcpConnectionLike<PlainConnection>);
static_assert(TcpConnectionLike<SslConnection>);
static_assert(NetworkBackend<AsioBackend>);

// 反向验证：不满足约束的类型
struct IncompleteBackend {
    using EventLoopType = int;
    using ConnectionType = int;
    using TimerType = int;
};
static_assert(!NetworkBackend<IncompleteBackend>);
static_assert(!EventLoopLike<int>);
```

这些 `static_assert` 在编译期执行，不产生任何运行时代码。如果未来的代码修改意外破坏了接口约束，编译器会**立即指出**哪个方法缺失或签名不对。

## 使用 Concept 约束模板

```cpp
template <NetworkBackend Backend>
class GenericServer
{
    using Loop = typename Backend::EventLoopType;
    using Conn = typename Backend::ConnectionType;

    Loop mainLoop_;
    // ...
};

// 使用
GenericServer<AsioBackend> server;
```

如果尝试用一个不满足 `NetworkBackend` 的类型实例化：

```cpp
GenericServer<IncompleteBackend> server;
// 编译错误：IncompleteBackend does not satisfy NetworkBackend
//   因为 int 不满足 EventLoopLike（缺少 run() 方法）
```

错误信息清晰指出"哪个约束不满足、哪个方法缺失"，远优于传统模板的 SFINAE 错误。

## Concepts vs 虚函数：何时用哪个？

Hical 两者并用：

| 场景           | 选择          | 原因                                                             |
| -------------- | ------------- | ---------------------------------------------------------------- |
| 后端切换       | Concepts      | 编译期决定，零开销                                               |
| EventLoop 抽象 | 虚函数        | 需要运行时多态（EventLoopPool 管理多个循环）                     |
| 连接抽象       | 虚函数 + 模板 | TcpConnection 基类用虚函数，GenericConnection 用模板处理 TCP/SSL |

关键原则：**如果在编译期就能确定类型，用 Concepts；如果需要运行时选择，用虚函数。**

## 未来：添加 io_uring 后端

假设未来要添加一个基于 Linux io_uring 的后端：

```cpp
struct IoUringBackend
{
    using EventLoopType = IoUringEventLoop;   // 满足 EventLoopLike
    using ConnectionType = IoUringConnection; // 满足 TcpConnectionLike
    using TimerType = IoUringTimer;           // 满足 TimerLike
};

// 只要三个类型各自满足对应 Concept，就能直接使用
GenericServer<IoUringBackend> server;
```

不需要修改 `GenericServer` 一行代码。Concept 约束保证新后端的接口完整性。

## 设计启示

Concepts 的价值不只是类型检查——它们是**可执行的文档**。

```cpp
// 这行代码同时是：
// 1. 接口文档：EventLoop 需要哪些方法
// 2. 约束检查：编译器自动验证
// 3. 错误提示：清晰指出缺少什么
template <typename T>
concept EventLoopLike = requires(T loop) { ... };
```

传统虚基类也能定义接口，但 Concepts 额外提供了：
- **无需继承**：鸭子类型——只要有对应方法就满足约束
- **编译期检查**：而非链接期
- **零运行时开销**：模板实例化后和手写代码一样
- **组合能力**：`&&` 组合多个 Concept

## 总结

C++20 Concepts 让"可替换后端"从理论上的设计理念变成编译器保证的契约。在 Hical 中，`NetworkBackend` Concept 确保了：任何满足约束的后端类型都能无缝接入框架，编译期零开销验证，运行期零额外开销。

这是现代 C++ 的后端抽象最佳实践。

---

> 源码参考：[Hical/src/core/Concepts.h](https://github.com/Hical61/Hical/blob/main/src/core/Concepts.h)
> 项目地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
