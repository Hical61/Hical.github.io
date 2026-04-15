+++
title = '第0课：C++20 核心特性速览'
date = '2026-04-15'
draft = false
tags = ["C++20", "Concepts", "协程", "PMR", "if constexpr", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "结合 Hical 项目源码，系统学习 Concepts、Coroutines、PMR、if constexpr 四大 C++20 核心特性在真实框架中的应用。"
+++

# 第0课：C++20 核心特性速览 - 学习笔记

> 结合 Hical 项目源码，系统学习 C++20 四大核心特性在真实框架中的应用。

---

## 一、Concepts（概念约束）

### 1.1 是什么

C++20 Concepts 是一种**编译期类型约束机制**，可以对模板参数施加具名约束，替代传统的 SFINAE（Substitution Failure Is Not An Error）技巧。

核心语法：

```cpp
// 定义一个 concept
template <typename T>
concept EventLoopLike = requires(T loop, std::function<void()> func, double delay) {
    { loop.run() } -> std::same_as<void>;
    { loop.stop() } -> std::same_as<void>;
    { loop.isRunning() } -> std::convertible_to<bool>;
    // ... 更多约束
};

// 使用 concept 约束模板参数
template <NetworkBackend Backend>
class GenericServer {
    using Loop = typename Backend::EventLoopType;
};
```

### 1.2 Hical 中的实际应用

**源码位置**：`src/core/Concepts.h`

Hical 定义了 **4 个核心 Concept**：

| Concept             | 约束目标 | 核心要求                                                              |
| ------------------- | -------- | --------------------------------------------------------------------- |
| `EventLoopLike`     | 事件循环 | run/stop、post/dispatch、runAfter/runEvery、isInLoopThread、allocator |
| `TcpConnectionLike` | TCP 连接 | send(多重载)、shutdown/close、connected/disconnected、流量统计        |
| `TimerLike`         | 定时器   | cancel、isActive、isRepeating、interval                               |
| `NetworkBackend`    | 后端整合 | 包含上面三个关联类型，且各自满足对应 Concept                          |

**关键设计**：`NetworkBackend` 是一个**组合 Concept**，它要求后端类型提供三个关联类型（`EventLoopType`、`ConnectionType`、`TimerType`），并且每个关联类型都必须满足对应的子 Concept：

```cpp
template <typename T>
concept NetworkBackend =
    requires {
        typename T::EventLoopType;
        typename T::ConnectionType;
        typename T::TimerType;
    } && EventLoopLike<typename T::EventLoopType>
      && TcpConnectionLike<typename T::ConnectionType>
      && TimerLike<typename T::TimerType>;
```

**Asio 后端定义**（`AsioBackend`）：

```cpp
struct AsioBackend {
    using EventLoopType = AsioEventLoop;
    using ConnectionType = TcpConnection;  // 即 PlainConnection
    using TimerType = AsioTimer;
};
```

### 1.3 Concepts vs 虚函数继承

| 维度     | Concepts               | 虚函数继承          |
| -------- | ---------------------- | ------------------- |
| 约束时机 | 编译期                 | 运行时              |
| 性能开销 | 零（编译期消除）       | vtable 查找开销     |
| 错误信息 | 清晰指出哪个约束不满足 | SFINAE 报错晦涩难懂 |
| 灵活性   | 不需要继承关系         | 必须继承基类        |
| 适用场景 | 后端切换、策略模式     | 运行时多态          |

**Hical 的选择**：两者并用。核心抽象层（`EventLoop`、`TcpConnection`、`Timer`）用**虚函数**实现运行时多态，同时用 **Concepts** 做编译期后端约束。这样既保留了运行时灵活性，又能在编译期捕获类型错误。

### 1.4 从测试看用法

**源码位置**：`tests/test_concepts.cpp`

```cpp
// 正向验证：满足约束的类型
static_assert(EventLoopLike<AsioEventLoop>);
static_assert(TimerLike<AsioTimer>);
static_assert(TcpConnectionLike<PlainConnection>);
static_assert(TcpConnectionLike<SslConnection>);

// 反向验证：不满足约束的类型被拒绝
struct IncompleteBackend {
    using EventLoopType = int;  // int 不满足 EventLoopLike
    using ConnectionType = int;
    using TimerType = int;
};
static_assert(!NetworkBackend<IncompleteBackend>);
static_assert(!EventLoopLike<int>);
```

`static_assert` 在编译期就能验证类型约束，不需要运行程序。

---

## 二、Coroutines（协程）

### 2.1 是什么

C++20 协程是一种**可暂停和恢复的函数**。当函数中出现 `co_await`、`co_return` 或 `co_yield` 时，编译器自动将其转换为协程。

核心概念：
- **`co_await`**：暂停当前协程，等待异步操作完成后恢复
- **`co_return`**：从协程返回值
- **Promise Type**：编译器要求的协程"驱动器"，控制协程的创建、挂起、恢复、销毁

### 2.2 Hical 中的实际应用

**源码位置**：`src/core/Coroutine.h`

Hical 没有自己实现 Promise Type，而是直接复用 Boost.Asio 的协程框架：

```cpp
// 类型别名，将 Boost.Asio 的 awaitable 包装为更简洁的名字
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;
```

**为什么只用类型别名而不自定义类？** 因为 `boost::asio::awaitable<T>` 已经完整实现了 Promise Type 和 Awaiter 接口，与 Asio 的 `io_context` 完美集成。自定义反而会增加复杂度且无法与 Asio 调度器配合。

**便捷工具函数**：

```cpp
// sleep - 在协程中等待（自动获取当前 executor）
inline Awaitable<void> sleep(double seconds) {
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(...));
    co_await timer.async_wait(boost::asio::use_awaitable);
}

// coSpawn - 启动一个新协程
template <typename F>
void coSpawn(boost::asio::io_context& ioCtx, F&& coroutine) {
    boost::asio::co_spawn(ioCtx, std::forward<F>(coroutine), boost::asio::detached);
}
```

### 2.3 协程在网络 I/O 中的应用

**源码位置**：`src/asio/GenericConnection.h`

协程最大的价值体现在网络读写循环中——把传统的回调地狱变成线性的顺序代码：

```cpp
// 读循环协程（简化版）
boost::asio::awaitable<void> GenericConnection<SocketType>::readLoop() {
    while (reading_ && state_.load() == State::hConnected) {
        inputBuffer_.ensureWritableBytes(4096);
        // co_await 暂停，等待数据到来后自动恢复
        auto bytesRead = co_await socket_.async_read_some(
            boost::asio::buffer(inputBuffer_.beginWrite(), inputBuffer_.writableBytes()),
            boost::asio::use_awaitable);

        bytesReceived_ += bytesRead;
        inputBuffer_.hasWritten(bytesRead);

        if (messageCallback_) {
            messageCallback_(sharedThis(), &inputBuffer_);
        }
    }
}
```

对比传统回调写法，协程版本：
- **可读性**：代码像同步逻辑一样从上到下执行
- **错误处理**：用 try-catch 替代分散的错误回调
- **生命周期**：通过 `shared_from_this()` 确保协程执行期间对象不被销毁

### 2.4 协程执行流程

```
coSpawn(ioCtx, readLoop())
    |
    v
readLoop() 开始执行
    |
    v
co_await async_read_some(...)  <-- 暂停，交出控制权给 io_context
    |                                     |
    |   io_context 继续处理其他事件...     |
    |                                     |
    v  <-- 数据到达，io_context 恢复协程
处理收到的数据
    |
    v
co_await async_read_some(...)  <-- 再次暂停...
    ...
```

---

## 三、PMR（Polymorphic Memory Resource）

### 3.1 是什么

PMR 是 C++17 引入的**多态内存资源**框架（`<memory_resource>` 头文件）。核心思想：把内存分配策略从容器类型中解耦出来，容器使用 `std::pmr::polymorphic_allocator`，实际分配行为由运行时绑定的 `memory_resource` 决定。

关键类型：
- `std::pmr::memory_resource` — 抽象基类，定义 `allocate/deallocate` 接口
- `std::pmr::polymorphic_allocator<T>` — 使用 memory_resource 的分配器
- `std::pmr::synchronized_pool_resource` — 线程安全的池式分配器
- `std::pmr::unsynchronized_pool_resource` — 非线程安全（单线程更快）
- `std::pmr::monotonic_buffer_resource` — 只分配不释放，最后整体释放

### 3.2 Hical 的三级内存池架构

**源码位置**：`src/core/MemoryPool.h`

```
第1级：全局同步池 (synchronized_pool_resource)
├── 线程安全，跨线程共享
├── 上游：TrackedResource（统计层）→ new_delete_resource
└── 用途：全局共享数据

第2级：线程本地池 (unsynchronized_pool_resource)
├── thread_local 缓存，无锁访问
├── 上游：全局同步池
└── 用途：线程内的频繁分配（如连接缓冲区）

第3级：请求级单调池 (monotonic_buffer_resource)
├── 只分配不释放，请求结束后整体释放
├── 上游：线程本地池
└── 用途：HTTP 请求生命周期内的临时分配
```

**为什么需要三级？**

| 级别         | 解决的问题     | 性能特点                   |
| ------------ | -------------- | -------------------------- |
| 全局同步池   | 跨线程共享数据 | 有锁，中等性能             |
| 线程本地池   | 单线程频繁分配 | 无锁，高性能               |
| 请求级单调池 | 请求内临时对象 | 零碎片，极致性能，整体释放 |

### 3.3 TrackedResource — 零开销统计

```cpp
class TrackedResource : public std::pmr::memory_resource {
protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* p = upstream_->allocate(bytes, alignment);
        totalAllocations_.fetch_add(1, std::memory_order_relaxed);
        auto current = currentBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        // 无锁 CAS 更新峰值
        auto peak = peakBytes_.load(std::memory_order_relaxed);
        while (current > peak && !peakBytes_.compare_exchange_weak(peak, current, ...)) {}
        return p;
    }
};
```

关键技巧：
- 用 `std::atomic` + `memory_order_relaxed` 实现近零开销的统计
- 峰值更新用 **CAS（Compare-And-Swap）** 循环，避免加锁

### 3.4 PmrBuffer — 网络缓冲区

**源码位置**：`src/core/PmrBuffer.h`

```
+-------------------+------------------+------------------+
| prepend (8字节)   | readable data    | writable space   |
+-------------------+------------------+------------------+
|                   ^                  ^                  ^
0              readIndex_         writeIndex_        buffer_.size()
```

**prepend 区域的用途**：网络协议中，经常需要先写入消息体，再在前面补上长度头。8 字节的 prepend 区域可以直接写入 4/8 字节的长度字段，避免数据搬移。

**自动扩容策略**（`makeSpace` 方法）：
1. 先尝试数据前移（把已读完的空间腾出来）
2. 空间仍然不够才 resize 扩容

```cpp
void makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + hPrependSize) {
        buffer_.resize(writeIndex_ + len);  // 真正扩容
    } else {
        // 移动数据到前面，回收已读空间
        std::copy(begin() + readIndex_, begin() + writeIndex_, begin() + hPrependSize);
        readIndex_ = hPrependSize;
        writeIndex_ = readIndex_ + readable;
    }
}
```

---

## 四、if constexpr（编译期分支）

### 4.1 是什么

`if constexpr` 是 C++17 引入的**编译期条件分支**。与普通 `if` 不同，编译器会在编译期评估条件，**丢弃不满足条件的分支**（不参与编译）。

### 4.2 Hical 中的实际应用

**源码位置**：`src/asio/GenericConnection.h`

Hical 用 `if constexpr` 在一个模板类中同时处理 TCP 和 SSL 两种连接：

**类型萃取**：
```cpp
// 判断是否为 SSL 流
template <typename T>
struct IsSslStream : std::false_type {};

template <typename T>
struct IsSslStream<boost::asio::ssl::stream<T>> : std::true_type {};

template <typename T>
inline constexpr bool hIsSslStream = IsSslStream<T>::value;
```

**用法示例 1：获取底层 socket**

```cpp
template <typename SocketType>
auto& GenericConnection<SocketType>::lowestLayerSocket() {
    if constexpr (hIsSslStream<SocketType>) {
        return socket_.lowest_layer();  // SSL: 穿透获取底层 tcp::socket
    } else {
        return socket_;                 // TCP: 直接就是 tcp::socket
    }
}
```

**用法示例 2：连接建立流程**

```cpp
void connectEstablished() {
    state_.store(State::hConnected);

    if constexpr (hIsSslStream<SocketType>) {
        // SSL 连接：先执行 TLS 握手，再触发回调
        co_spawn(socketExecutor(), [conn]() -> awaitable<void> {
            co_await conn->sslHandshake();
        }, detached);
    } else {
        // 普通 TCP：直接触发连接回调并开始读取
        if (connectionCallback_) connectionCallback_(self);
        startRead();
    }
}
```

**用法示例 3：关闭连接**

```cpp
void shutdownInLoop() {
    if constexpr (hIsSslStream<SocketType>) {
        // SSL: 先发 TLS close_notify，再 TCP shutdown
        co_await socket_.async_shutdown(use_awaitable);
        sock.shutdown(tcp::socket::shutdown_send, ec);
    } else {
        // TCP: 直接 shutdown
        sock.shutdown(tcp::socket::shutdown_send, ec);
    }
}
```

### 4.3 if constexpr vs 虚函数 vs 普通 if

| 方式           | 分支消除             | 运行开销     | 代码复用                 |
| -------------- | -------------------- | ------------ | ------------------------ |
| `if constexpr` | 编译期，丢弃无用分支 | 零           | 一份模板代码             |
| 虚函数         | 无                   | vtable 查找  | 需要两个子类             |
| 普通 `if`      | 运行时判断           | 分支预测开销 | 编译时两个分支都必须合法 |

`if constexpr` 的核心优势：**被丢弃的分支不需要编译通过**。这对于 SSL/TCP 差异化处理至关重要——SSL 流有 `lowest_layer()`、`async_handshake()` 等方法，普通 socket 没有，如果用普通 `if` 会编译失败。

---

## 五、前置知识回顾

### 5.1 enable_shared_from_this

**Hical 中的使用**：`TcpConnection` 继承了 `std::enable_shared_from_this<TcpConnection>`。

**为什么需要？** 协程的生命周期可能超过对象的作用域。在协程中需要持有对象的 `shared_ptr`，防止对象在协程执行期间被销毁：

```cpp
void GenericConnection<SocketType>::startRead() {
    auto self = sharedThis();  // 持有 shared_ptr，保证协程执行期间对象存活
    boost::asio::co_spawn(socketExecutor(),
        [conn]() -> awaitable<void> {
            co_await conn->readLoop();  // 协程可能运行很久
        }, detached);
}
```

### 5.2 智能指针的使用模式

在 Hical 中可以看到 3 种智能指针用法：

| 用法                        | 示例                       | 目的                      |
| --------------------------- | -------------------------- | ------------------------- |
| `shared_ptr` 管理连接       | `GenericConnection::Ptr`   | 多处共享连接的所有权      |
| `unique_ptr` 管理资源       | `createRequestPool()` 返回 | 独占所有权，RAII 自动释放 |
| `shared_ptr<void>` 类型擦除 | 用户上下文 `context_`      | 存储任意类型的上下文数据  |

### 5.3 Lambda 与 std::function

Concepts 的 requires 表达式中大量使用 `std::function<void()>` 作为参数约束：

```cpp
concept EventLoopLike = requires(T loop, std::function<void()> func, double delay) {
    { loop.post(func) } -> std::same_as<void>;
};
```

实际使用中，回调多用 Lambda + `shared_ptr` 捕获列表：

```cpp
loop_->post([this, msg, self]() {
    sendInLoop(msg->data(), msg->size());
});
```

---

## 九、动手练习参考

### 练习1：写一个 concept Printable

```cpp
#include <concepts>
#include <iostream>

template <typename T>
concept Printable = requires(std::ostream& os, const T& val) {
    { os << val } -> std::same_as<std::ostream&>;
};

// 使用
template <Printable T>
void print(const T& val) {
    std::cout << val << std::endl;
}

// print(42);         // OK, int 满足 Printable
// print("hello");    // OK, const char* 满足 Printable
// print(std::vector<int>{}); // 编译错误，vector 没有 operator<<
```

### 练习2：最简协程

```cpp
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>

boost::asio::awaitable<void> myCoroutine() {
    auto executor = co_await boost::asio::this_coro::executor;
    std::cout << "协程开始" << std::endl;

    // 暂停 1 秒
    boost::asio::steady_timer timer(executor, std::chrono::seconds(1));
    co_await timer.async_wait(boost::asio::use_awaitable);

    std::cout << "协程恢复，1秒后" << std::endl;
}

int main() {
    boost::asio::io_context ioCtx;
    boost::asio::co_spawn(ioCtx, myCoroutine(), boost::asio::detached);
    ioCtx.run();
    return 0;
}
```

### 练习3：PMR 单调缓冲区

```cpp
#include <memory_resource>
#include <vector>
#include <iostream>

int main() {
    // 预分配 4KB 栈上缓冲区
    char buffer[4096];
    std::pmr::monotonic_buffer_resource pool(buffer, sizeof(buffer));

    // 用 PMR 分配器创建 vector
    std::pmr::vector<int> vec(&pool);

    for (int i = 0; i < 100; ++i) {
        vec.push_back(i);
    }

    std::cout << "分配了 " << vec.size() << " 个元素" << std::endl;
    std::cout << "所有分配都在预分配的 4KB 缓冲区内，无系统调用" << std::endl;

    // pool 析构时整体释放，不需要逐个 deallocate
    return 0;
}
```

---

## 六、C++26 反射（预览）

### 6.1 是什么

C++26 反射（P2996 提案）允许程序在**编译期**检查类型的结构信息——枚举成员变量、获取名称、遍历成员函数等。核心操作符：
- `^^T` — 获取类型 T 的反射信息（`std::meta::info`）
- `[:info:]` — 从反射信息恢复为代码实体（"反引用"）
- `template for` — 编译期遍历反射信息集合

### 6.2 Hical 中的双路线设计

**源码位置**：`src/core/Reflection.h`

由于主流编译器尚未正式支持 P2996，Hical 采用**条件编译双路线**：

```cpp
// 反射检测
#if defined(__cpp_reflection) && __cpp_reflection >= 202306L
    #define HICAL_HAS_REFLECTION 1
#else
    #define HICAL_HAS_REFLECTION 0
#endif
```

两条路线提供**完全相同的用户 API**：

| 功能        | C++20 回退                                                    | C++26 反射                       |
| ----------- | ------------------------------------------------------------- | -------------------------------- |
| JSON 序列化 | `HICAL_JSON(Type, field1, field2)` 宏标注                     | 自动枚举所有字段，无需标注       |
| 路由注册    | `HICAL_HANDLER` + `HICAL_ROUTES` 宏标注                       | `[[hical::route(...)]]` 属性标注 |
| 对外 API    | `meta::toJson(obj)` / `meta::registerRoutes(router, handler)` | 完全相同                         |

### 6.3 JSON 自动序列化示例

**源码位置**：`src/core/MetaJson.h`

```cpp
// C++20 回退：需要宏标注
struct UserDTO {
    std::string name;
    int age;
    HICAL_JSON(UserDTO, name, age)
};

// 使用（两条路线完全一样）
boost::json::object json = meta::toJson(user);     // 序列化
auto user = meta::fromJson<UserDTO>(jsonValue);     // 反序列化
```

C++26 反射可用时，`HICAL_JSON` 宏变为空操作，`toJson` 内部通过 `^^T` 自动枚举字段。

### 6.4 与 if constexpr 的关系

`if constexpr` 实现编译期**分支**，反射实现编译期**自省**。两者互补：

```cpp
// MetaJson.h 中的类型分发就是 if constexpr + 反射检测的组合
template <typename T>
boost::json::value valueToJson(const T& val) {
    if constexpr (std::is_same_v<T, std::string>) { ... }
    else if constexpr (std::is_integral_v<T>) { ... }
    else if constexpr (HasJsonFields<T>::value) { return toJson(val); }  // 嵌套结构体
}
```

---

## 七、核心要点总结

| 特性             | 核心价值                       | Hical 中的应用                            |
| ---------------- | ------------------------------ | ----------------------------------------- |
| **Concepts**     | 编译期类型约束，清晰的错误信息 | 4 个 Concept 约束网络后端接口             |
| **Coroutines**   | 异步代码线性化，告别回调地狱   | readLoop/writeLoop/sslHandshake 协程      |
| **PMR**          | 分配策略与容器解耦，零碎片     | 三级内存池 + PmrBuffer 网络缓冲区         |
| **if constexpr** | 编译期分支消除，零运行开销     | GenericConnection 统一 TCP/SSL 处理       |
| **C++26 反射**   | 编译期自省，自动化样板代码     | 自动 JSON 序列化 + 自动路由注册（双路线） |

---

## 八、关键问题思考与回答

**Q1: 为什么 `Awaitable<T>` 只是类型别名？**
> 因为 Boost.Asio 的 `awaitable<T>` 已经完整实现了协程框架（Promise Type + Awaiter），与 `io_context` 调度器深度集成。自定义协程类型反而会失去与 Asio 生态的兼容性。

**Q2: `monotonic_buffer_resource` 为什么适合请求级分配？**
> HTTP 请求有明确的生命周期（接收→处理→响应→销毁）。请求内的所有临时对象在请求结束后都不再需要。monotonic 只分配不释放的策略恰好匹配：分配极快（只移动指针），请求结束后一次性释放整个缓冲区。

**Q3: 线程本地池为什么不需要加锁？**
> 因为它是 `thread_local` 的——每个线程独享一份实例，不存在跨线程访问。配合 Hical 的"1线程:1事件循环"模型，同一事件循环上的所有连接都在同一线程中处理，天然无竞争。

**Q4: `if constexpr` 相比虚函数有什么优势？**
> 编译期分支消除 = 零运行开销。更关键的是，被丢弃的分支不需要编译通过，这允许在同一个模板中混用只有特定类型才有的 API（如 SSL 的 `async_handshake`）。虚函数做不到这一点。

---

*下一课：第1课 - 抽象接口与 Concepts 设计，将深入阅读 EventLoop、Timer、TcpConnection 三大核心接口。*
