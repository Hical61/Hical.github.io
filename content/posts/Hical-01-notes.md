+++
title = '第1课：抽象接口与 Concepts 设计'
date = '2026-04-15'
draft = false
tags = ["C++20", "Concepts", "接口设计", "EventLoop", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "深入理解 Hical 两层架构的分离思想，掌握 EventLoop/Timer/TcpConnection 三大核心接口的职责划分和 NetworkBackend Concept 的编译期约束。"
+++

# 第1课：抽象接口与 Concepts 设计 - 学习笔记

> 深入理解 Hical 两层架构的分离思想，掌握三大核心接口的职责划分和 NetworkBackend Concept 的编译期约束。

---

## 一、两层架构设计思想

### 1.1 为什么要分层

Hical 的源码分为两层：

```
src/core/    <-- 抽象层：纯虚接口 + 类型定义 + 通用逻辑
src/asio/    <-- 实现层：Boost.Asio 的具体实现
```

**核心原则：面向接口编程，实现可替换。**

这意味着：
- `core/` 层不依赖任何具体网络库（不 include Boost 头文件）
- `asio/` 层依赖 Boost.Asio，实现 `core/` 定义的接口
- 未来如果要换成 libuv、io_uring 等后端，只需新增一个实现层，`core/` 不变

### 1.2 层级依赖关系

```
                  core/（纯接口）
                 /     |      \
          EventLoop  Timer  TcpConnection
              |        |        |
              v        v        v
          AsioEventLoop AsioTimer GenericConnection   <-- asio/（具体实现）
                 \       |       /
                  AsioBackend（打包）
                      |
                  NetworkBackend concept（编译期验证）
```

**关键设计决策**：
- **接口层用虚函数**：`EventLoop`、`Timer`、`TcpConnection` 都是纯虚类，保留运行时多态能力
- **后端约束用 Concepts**：`NetworkBackend` 在编译期验证后端类型是否完整实现了所有接口
- **两者互补**：虚函数给运行时灵活性，Concepts 给编译期安全保障

---

## 二、EventLoop — 事件循环接口

**源码位置**：`src/core/EventLoop.h`

### 2.1 接口方法总览

EventLoop 是框架的心脏，分为 5 个功能组：

```
EventLoop（抽象基类）
│
├── 生命周期管理
│   ├── run()              阻塞运行事件循环
│   ├── stop()             停止事件循环
│   └── isRunning()        查询运行状态
│
├── 任务调度
│   ├── dispatch(Func)     智能调度：同线程直接执行，跨线程投递队列
│   └── post(Func)         总是异步投递到队列
│
├── 定时器
│   ├── runAfter(delay, cb)     延迟执行（单次）
│   ├── runEvery(interval, cb)  周期执行（重复）
│   └── cancelTimer(id)         取消定时器
│
├── 线程属性
│   ├── isInLoopThread()    当前是否在事件循环线程
│   ├── index() / setIndex()  事件循环索引（多线程池场景）
│   └── runOnQuit(cb)       注册退出钩子
│
└── PMR 支持
    └── allocator()         获取线程关联的 pmr 分配器
```

### 2.2 关键设计解读

#### dispatch vs post

这是最重要的区别之一：

```cpp
// dispatch：智能调度
// 如果当前线程就是事件循环线程 → 直接同步执行回调
// 如果当前线程不是事件循环线程 → 投递到队列异步执行
virtual void dispatch(Func cb) = 0;

// post：总是异步
// 无论当前是否在事件循环线程，都投递到队列异步执行
virtual void post(Func cb) = 0;
```

**什么时候用哪个？**

| 场景                 | 选择       | 原因                           |
| -------------------- | ---------- | ------------------------------ |
| 保证线程安全执行     | `dispatch` | 同线程可避免不必要的队列开销   |
| 延迟执行（避免递归） | `post`     | 确保回调在当前调用栈返回后执行 |
| 跨线程通信           | 两者皆可   | 都会投递到目标线程的队列       |

**游戏服务器类比**：`dispatch` 类似于"如果在主线程就直接处理消息，否则丢到消息队列"；`post` 类似于"总是丢到消息队列下一帧处理"。

#### 定时器的返回值设计

```cpp
using TimerId = uint64_t;
enum : TimerId { hInvalidTimerId = 0 };  // 0 表示无效 ID

TimerId runAfter(double delay, Func cb);   // 返回 ID 用于后续取消
TimerId runEvery(double interval, Func cb);
void cancelTimer(TimerId id);
```

为什么用 `uint64_t` 而不是指针或对象？
- **轻量**：拷贝代价极低
- **安全**：不持有定时器的所有权，避免悬挂指针
- **简洁**：用户只需要保存一个整数 ID 即可取消

#### chrono 便捷重载

```cpp
// 基础版本（纯虚，子类必须实现）
virtual TimerId runAfter(double delay, Func cb) = 0;

// 便捷版本（非虚，直接在基类实现，转调基础版本）
TimerId runAfter(const std::chrono::duration<double>& delay, Func cb) {
    return runAfter(delay.count(), std::move(cb));
}
```

这是一个常见的**NVI（Non-Virtual Interface）变体**：提供类型安全的 chrono 接口，内部转调秒数版本。子类只需实现一个版本。

#### PMR 分配器支持

```cpp
virtual std::pmr::polymorphic_allocator<std::byte> allocator() const = 0;
```

每个 EventLoop 关联一个 PMR 分配器（通常是线程本地池）。这样在事件循环中创建的缓冲区、字符串等对象，都可以使用高性能的线程本地内存池。

---

## 三、Timer — 定时器接口

**源码位置**：`src/core/Timer.h`

### 3.1 接口方法总览

Timer 是从 EventLoop 中独立出来的定时器对象：

```
Timer（抽象基类）
│
├── cancel()         取消定时器
├── isActive()       是否仍然活跃（未取消且未过期）
├── getLoop()        获取所属事件循环
├── isRepeating()    是否为周期性定时器
└── interval()       获取间隔时间（秒），单次返回 0
```

### 3.2 Timer vs EventLoop 定时器接口的关系

Hical 提供了**两种定时器使用方式**：

| 方式            | API                          | 返回值            | 使用场景                 |
| --------------- | ---------------------------- | ----------------- | ------------------------ |
| EventLoop 方法  | `loop.runAfter(delay, cb)`   | `TimerId`（整数） | 轻量级，只需取消功能     |
| 独立 Timer 对象 | `std::shared_ptr<AsioTimer>` | Timer 对象        | 需要查询状态、获取间隔等 |

实际上 `EventLoop::runAfter` 内部就是创建 Timer 对象并管理，TimerId 是 Timer 的索引 key。

### 3.3 为什么要从 EventLoop 中分离 Timer

- **单一职责**：EventLoop 管事件循环，Timer 管定时逻辑
- **可独立测试**：Timer 可以单独测试，不依赖完整的事件循环
- **更丰富的接口**：Timer 对象可以查询 `isActive()`、`isRepeating()` 等状态

---

## 四、TcpConnection — TCP 连接接口

**源码位置**：`src/core/TcpConnection.h`

### 4.1 接口方法总览

TcpConnection 是最复杂的接口，覆盖连接生命周期的所有方面：

```
TcpConnection（抽象基类，继承 enable_shared_from_this）
│
├── 数据发送（7 个 send 重载）
│   ├── send(const char*, size_t)           原始数据
│   ├── send(const string&)                 字符串引用
│   ├── send(string&&)                      字符串移动
│   ├── send(const PmrBuffer&)              PMR 缓冲区引用
│   ├── send(PmrBuffer&&)                   PMR 缓冲区移动
│   ├── send(shared_ptr<string>)            共享字符串
│   └── send(shared_ptr<PmrBuffer>)         共享缓冲区
│
├── 连接控制
│   ├── shutdown()                  半关闭（只关闭写端）
│   ├── close()                     强制关闭
│   ├── setTcpNoDelay(bool)         Nagle 算法开关
│   ├── startRead() / stopRead()    读取控制
│
├── 状态查询
│   ├── connected() / disconnected()  连接状态
│   ├── localAddr() / peerAddr()      地址信息
│   ├── getLoop()                     所属事件循环
│   └── bytesSent() / bytesReceived() 流量统计
│
├── 回调设置（5 种回调）
│   ├── onMessage(cb)               收到数据
│   ├── onConnection(cb)            连接建立/断开
│   ├── onClose(cb)                 连接关闭
│   ├── onWriteComplete(cb)         写入完成
│   └── onHighWaterMark(cb, size)   高水位线告警
│
└── 用户上下文
    ├── setContext(shared_ptr<void>) 设置上下文
    ├── getContext<T>()              获取上下文（模板方法）
    ├── hasContext()                 是否有上下文
    └── clearContext()              清除上下文
```

### 4.2 关键设计解读

#### 为什么继承 enable_shared_from_this

```cpp
class TcpConnection : public std::enable_shared_from_this<TcpConnection>
```

核心原因：**连接的生命周期管理**。

TCP 连接对象需要在异步操作中保持存活。例如：
1. 用户调用 `conn->send("hello")`
2. send 内部将数据投递到写队列
3. 写协程异步发送数据
4. 在数据发送完成之前，连接对象不能被销毁

通过 `shared_from_this()` 获取指向自身的 `shared_ptr`，可以在异步操作（协程、回调）中持有引用，保证对象不会被提前析构。

```cpp
// GenericConnection.h 中的实际使用
void startRead() {
    auto self = sharedThis();  // 引用计数 +1，协程运行期间对象不会被销毁
    boost::asio::co_spawn(socketExecutor(),
        [conn]() -> awaitable<void> { co_await conn->readLoop(); },
        detached);
}
```

#### 7 个 send 重载的设计意图

为什么需要这么多重载？**避免不必要的内存拷贝**。

| 重载                        | 场景         | 拷贝次数            |
| --------------------------- | ------------ | ------------------- |
| `send(const char*, size_t)` | C 风格数据   | 1次（拷贝到写队列） |
| `send(const string&)`       | 临时字符串   | 1次                 |
| `send(string&&)`            | 可移动字符串 | 0次（直接移动）     |
| `send(shared_ptr<string>)`  | 多连接广播   | 0次（共享所有权）   |

**游戏服务器场景示例**：
- 单个玩家发消息 → `send(string&&)` 移动语义，零拷贝
- 全服广播 → `send(shared_ptr<string>)` 所有连接共享同一份数据，避免 N 次拷贝

#### shutdown vs close

```cpp
virtual void shutdown() = 0;  // 半关闭：关闭写端，仍可读取
virtual void close() = 0;     // 强制关闭：双向都关闭
```

**游戏服务器类比**：
- `shutdown()`：通知客户端"我不会再发数据了"，但还可以接收客户端的最后一批数据（优雅断开）
- `close()`：直接断开，踢人

#### 5 种回调的触发时机

```
连接建立 ──→ onConnection(conn)     conn->connected() == true
    │
    ├── 收到数据 ──→ onMessage(conn, buffer)
    ├── 发完数据 ──→ onWriteComplete(conn)
    ├── 写队列满 ──→ onHighWaterMark(conn, size)
    │
连接关闭 ──→ onConnection(conn)     conn->connected() == false
         ──→ onClose(conn)
```

注意：`onConnection` 在建立和断开时**都会触发**，通过 `conn->connected()` 区分方向。

#### 用户上下文 — 类型擦除模式

```cpp
// 设置上下文：存储为 shared_ptr<void>（类型擦除）
virtual void setContext(const std::shared_ptr<void>& context) = 0;

// 获取上下文：模板方法，转回具体类型
template <typename T>
std::shared_ptr<T> getContext() const {
    return std::static_pointer_cast<T>(getContextInternal());
}
```

**为什么这样设计？**

框架不知道用户会在连接上附带什么数据（玩家信息、会话状态等），所以用 `shared_ptr<void>` 做类型擦除。用户通过模板 `getContext<T>()` 安全地转回具体类型。

**游戏服务器使用场景**：

```cpp
// 连接建立时绑定玩家数据
struct PlayerSession { uint64_t playerId; std::string name; };
conn->setContext(std::make_shared<PlayerSession>(playerId, name));

// 收到消息时获取玩家数据
auto session = conn->getContext<PlayerSession>();
std::cout << "收到来自 " << session->name << " 的消息" << std::endl;
```

---

## 五、Concepts 如何约束后端实现

### 5.1 回顾：四个 Concept 的层级关系

```
EventLoopLike   TcpConnectionLike   TimerLike
      \               |               /
       \              |              /
        -------→ NetworkBackend ←-------
```

`NetworkBackend` 是顶层组合 Concept，要求后端提供三个关联类型，每个都满足对应的子 Concept。

### 5.2 Concept 与虚接口的协作方式

这是 Hical 设计中最精妙的部分——**双层约束**：

```
编译期（Concepts）                    运行时（虚函数）
───────────────                      ──────────────
NetworkBackend concept               EventLoop 纯虚基类
  要求 EventLoopType 有 run()          定义 virtual run() = 0
  要求 ConnectionType 有 send()        定义 virtual send() = 0
  ...                                 ...
        ↓                                   ↓
AsioBackend 满足 NetworkBackend       AsioEventLoop 继承 EventLoop
```

- **虚函数**确保：每个方法在运行时有正确的实现
- **Concepts**确保：后端整体在编译期就是完整的、一致的

### 5.3 实际使用场景

```cpp
// 用 Concept 约束模板参数，编译期保证后端完整性
template <NetworkBackend Backend>
class GenericServer {
    using Loop = typename Backend::EventLoopType;
    using Conn = typename Backend::ConnectionType;
    using Timer = typename Backend::TimerType;
};

// 编译通过 ← AsioBackend 满足 NetworkBackend
GenericServer<AsioBackend> server;

// 编译失败 ← IncompleteBackend 不满足 NetworkBackend
// GenericServer<IncompleteBackend> bad;  // 清晰的编译错误
```

### 5.4 从测试看验证方式

**源码位置**：`tests/test_concepts.cpp`

测试文件展示了 3 种验证方式：

**1. static_assert 编译期验证（最重要）**

```cpp
// 正向：满足约束
static_assert(EventLoopLike<AsioEventLoop>);
static_assert(TcpConnectionLike<PlainConnection>);
static_assert(TcpConnectionLike<SslConnection>);

// 反向：不满足约束
static_assert(!NetworkBackend<IncompleteBackend>);
static_assert(!EventLoopLike<int>);
```

**2. 运行时验证类型别名**

```cpp
TEST(ConceptsTest, AsioBackendTypeAliases) {
    EXPECT_TRUE((std::is_same_v<AsioBackend::EventLoopType, AsioEventLoop>));
    EXPECT_TRUE((std::is_same_v<AsioBackend::TimerType, AsioTimer>));
}
```

**3. 模板参数验证**

```cpp
template <NetworkBackend Backend>
struct BackendTraits { static constexpr bool hValid = true; };

TEST(ConceptsTest, NetworkBackendAsTemplateParam) {
    EXPECT_TRUE(BackendTraits<AsioBackend>::hValid);  // 编译通过即验证
}
```

---

## 六、AsioEventLoop — 具体实现一览

**源码位置**：`src/asio/AsioEventLoop.h`

AsioEventLoop 继承 EventLoop，用 Boost.Asio 实现所有接口：

```cpp
class AsioEventLoop : public EventLoop {
private:
    boost::asio::io_context ioContext_;          // 核心：Asio 事件循环
    std::unique_ptr<work_guard> workGuard_;       // 防止空闲时退出
    std::thread::id threadId_;                    // 记录所属线程 ID
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    size_t index_{0};                             // 在线程池中的索引

    std::atomic<TimerId> nextTimerId_{1};          // 定时器 ID 自增器
    std::map<TimerId, shared_ptr<AsioTimer>> timers_;  // 定时器注册表
    std::mutex timersMutex_;                       // 定时器注册表锁

    std::vector<Func> quitCallbacks_;              // 退出钩子
    std::mutex quitMutex_;
};
```

### 关键实现细节

**work_guard 的作用**：
`io_context::run()` 在没有待处理任务时会自动退出。`work_guard` 相当于一个"假任务"，让 `run()` 保持阻塞状态，直到显式调用 `stop()`。

```
没有 work_guard:
io_context.run() → 无任务 → 立即返回（不是我们想要的）

有 work_guard:
io_context.run() → 有 work_guard 占位 → 持续等待 → 调用 stop() 才退出
```

**线程模型**：1 Thread : 1 io_context
- 每个 AsioEventLoop 拥有自己的 `io_context`
- `threadId_` 记录运行线程的 ID，用于 `isInLoopThread()` 判断
- 保证同一个 EventLoop 上的所有操作都在同一个线程执行 → **天然线程安全**

---

## 七、三大接口的设计模式总结

| 模式                | 应用                                         | 说明                                |
| ------------------- | -------------------------------------------- | ----------------------------------- |
| **纯虚接口**        | EventLoop / Timer / TcpConnection            | 定义抽象契约，实现层必须完整实现    |
| **NVI（非虚接口）** | EventLoop 的 chrono 重载                     | 基类提供便捷封装，转调虚函数        |
| **CRTP 变体**       | TcpConnection 的 enable_shared_from_this     | 允许基类方法获取派生类的 shared_ptr |
| **类型擦除**        | TcpConnection 的 context（shared_ptr<void>） | 存储任意类型的用户数据              |
| **观察者模式**      | 5 种回调（onMessage/onClose 等）             | 连接事件通知机制                    |
| **关联类型**        | AsioBackend 的 EventLoopType 等              | 后端打包三个类型，供 Concept 验证   |

---

## 八、关键问题思考与回答

**Q1: 为什么要用纯虚接口而不是直接用 Boost.Asio 的类型？**

> 1. **解耦**：上层代码（Router、Middleware、HttpServer）不依赖 Boost.Asio，只依赖抽象接口
> 2. **可替换**：未来可以用 libuv、io_uring 等替换 Asio，不影响上层逻辑
> 3. **可测试**：可以 mock EventLoop 进行单元测试，不需要真正的网络环境
> 4. **编译隔离**：Boost.Asio 头文件庞大，只让 `asio/` 层引入，减少编译时间

**Q2: Concepts 和传统的虚函数继承各有什么优劣？**

> | 维度 | Concepts | 虚函数 |
> |------|----------|--------|
> | 检查时机 | 编译期 | 运行时 |
> | 性能 | 零开销 | vtable 开销 |
> | 灵活性 | 无需继承，鸭子类型 | 必须继承基类 |
> | 错误提示 | 清晰说明哪个约束失败 | 链接期才报"未实现纯虚函数" |
> | 运行时多态 | 不支持 | 支持 |
>
> Hical 的选择是**两者并用**：虚函数给运行时多态，Concepts 给编译期完整性校验。

**Q3: TcpConnection 为什么继承 enable_shared_from_this？**

> 因为 TCP 连接的异步操作（协程读写循环、跨线程投递）需要持有连接的 `shared_ptr`，防止在异步操作完成前对象被析构。`enable_shared_from_this` 允许在成员函数中安全地获取指向自身的 `shared_ptr`。
>
> 这与游戏服务器中"玩家对象在异步 DB 操作期间不能被销毁"是同一个问题。

**Q4: Awaitable<T> 为什么只是一个类型别名而非自定义类？**

> `Awaitable<T>` 定义为 `boost::asio::awaitable<T>` 的别名。原因：
> 1. Boost.Asio 的 awaitable 已经完整实现了 Promise Type，与 io_context 调度器深度集成
> 2. 自定义协程类型需要实现复杂的 Promise Type + Awaiter，且无法与 Asio 的 `co_spawn`、`use_awaitable` 配合
> 3. 别名在不引入额外开销的同时，隐藏了底层实现细节，如果未来换用其他协程框架，只需修改别名

---

## 九、接口方法脑图

### EventLoop 方法清单

```
EventLoop
├── 生命周期
│   ├── run()                   启动（阻塞）
│   ├── stop()                  停止
│   └── isRunning() → bool      查询状态
│
├── 任务调度
│   ├── dispatch(Func)          智能调度（同线程直接执行）
│   └── post(Func)              总是异步投递
│
├── 定时器
│   ├── runAfter(double, Func) → TimerId      单次延迟
│   ├── runAfter(chrono::duration, Func)       ↑ chrono 便捷版
│   ├── runEvery(double, Func) → TimerId       周期重复
│   ├── runEvery(chrono::duration, Func)       ↑ chrono 便捷版
│   └── cancelTimer(TimerId)                   取消
│
├── 线程属性
│   ├── isInLoopThread() → bool     判断当前线程
│   ├── index() → size_t            获取索引
│   ├── setIndex(size_t)            设置索引
│   └── runOnQuit(Func)             退出钩子
│
└── PMR
    └── allocator() → polymorphic_allocator   获取内存分配器
```

### Timer 方法清单

```
Timer
├── cancel()                取消
├── isActive() → bool       是否活跃
├── getLoop() → EventLoop*  所属循环
├── isRepeating() → bool    是否周期
└── interval() → double     间隔（秒）
```

### TcpConnection 方法清单

```
TcpConnection（继承 enable_shared_from_this）
├── 发送（7 个重载）
│   ├── send(const char*, size_t)
│   ├── send(const string&)
│   ├── send(string&&)               移动语义
│   ├── send(const PmrBuffer&)
│   ├── send(PmrBuffer&&)
│   ├── send(shared_ptr<string>)     共享，适合广播
│   └── send(shared_ptr<PmrBuffer>)
│
├── 连接控制
│   ├── shutdown()           半关闭（只关写端）
│   ├── close()              强制关闭
│   ├── setTcpNoDelay(bool)  Nagle 开关
│   ├── startRead()          开始读
│   └── stopRead()           停止读
│
├── 状态查询
│   ├── connected() → bool
│   ├── disconnected() → bool
│   ├── localAddr() → InetAddress
│   ├── peerAddr() → InetAddress
│   ├── getLoop() → EventLoop*
│   ├── bytesSent() → size_t
│   └── bytesReceived() → size_t
│
├── 回调
│   ├── onMessage(cb)          数据到达
│   ├── onConnection(cb)       建立/断开
│   ├── onClose(cb)            关闭
│   ├── onWriteComplete(cb)    写完成
│   └── onHighWaterMark(cb, size)  写队列过大
│
└── 用户上下文
    ├── setContext(shared_ptr<void>)
    ├── getContext<T>() → shared_ptr<T>   模板方法
    ├── hasContext() → bool
    └── clearContext()
```

---

## 十、与游戏服务器架构的对比

| Hical 概念      | 游戏服务器等价物       | 说明                                             |
| --------------- | ---------------------- | ------------------------------------------------ |
| EventLoop       | 主循环 / 消息循环      | 游戏服务器的"心跳循环"                           |
| dispatch / post | SendMsg / PostMsg      | 线程间消息投递                                   |
| TcpConnection   | 客户端 Session         | 每个连接代表一个客户端                           |
| onMessage       | 消息处理函数           | 收到网络包的回调                                 |
| onClose         | 断线处理               | 客户端掉线                                       |
| Timer           | 定时器 / 心跳          | 定时任务（如心跳检测、定时存档）                 |
| 高水位回调      | 发送队列告警           | 防止某个连接的发送队列无限增长（客户端不收数据） |
| 用户上下文      | Session 绑定的玩家数据 | 连接绑定玩家 ID、角色信息等                      |

---

*下一课：第2课 - 错误处理与网络地址，将学习 ErrorCode 枚举设计和 InetAddress 封装。*
