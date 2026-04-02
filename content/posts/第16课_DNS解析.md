+++
date = '2026-05-03'
draft = false
title = 'DNS 解析 — Resolver、NormalResolver、AresResolver'
categories = ["网络编程"]
tags = ["C++", "trantor", "DNS", "Resolver", "学习笔记"]
description = "trantor DNS 解析模块解析，Resolver、NormalResolver、AresResolver 三种实现对比。"
+++


# 第 16 课：DNS 解析 — Resolver、NormalResolver、AresResolver

> 对应源文件：
> - `trantor/net/Resolver.h` — 异步 DNS 解析抽象接口
> - `trantor/net/inner/NormalResolver.h` / `NormalResolver.cc` — 基于 `getaddrinfo` 的线程池实现
> - `trantor/net/inner/AresResolver.h` — 基于 c-ares 的真异步实现

---

## 一、为什么 DNS 解析需要异步？

`getaddrinfo()` 是系统标准 DNS 解析函数，但它是**阻塞的**——在 DNS 服务器响应之前，调用线程会一直挂起。

在 EventLoop 单线程模型中，如果直接调用 `getaddrinfo()`：
- EventLoop 线程被阻塞
- 该线程上所有其他连接的 I/O 事件、定时器全部停止响应
- 哪怕只是 100ms 的 DNS 查询，对游戏服务器来说都是灾难性的抖动

trantor 提供两种解决方案：

```
DNS 解析请求
    │
    ├── NormalResolver（默认）
    │     → 投递到 ConcurrentTaskQueue（阻塞线程池）
    │     → getaddrinfo() 在工作线程里阻塞
    │     → 完成后回调（在工作线程里直接调用）
    │
    └── AresResolver（c-ares 可用时）
          → 在 EventLoop 线程里全程非阻塞
          → c-ares 管理 DNS socket，注册到 epoll
          → EventLoop 处理 DNS socket 的读写事件
          → 完成后在 EventLoop 线程里调用回调
```

---

## 二、Resolver — 统一抽象接口

```cpp
class Resolver {
  public:
    using Callback = std::function<void(const trantor::InetAddress&)>;
    using ResolverResultsCallback =
        std::function<void(const std::vector<trantor::InetAddress>&)>;

    // 工厂函数：根据编译配置选择实现
    static std::shared_ptr<Resolver> newResolver(
        EventLoop *loop = nullptr,
        size_t timeout = 60);   // timeout：DNS 缓存有效期（秒）

    // 解析单个地址
    virtual void resolve(const std::string& hostname,
                         const Callback& callback) = 0;

    // 解析所有地址（A/AAAA 记录，可能有多个 IP）
    virtual void resolve(const std::string& hostname,
                         const ResolverResultsCallback& callback) = 0;

    static bool isCAresUsed();   // 当前是否在用 c-ares
};
```

**两个回调的区别**：

| 回调类型                  | 返回                  | 适用场景                          |
| ------------------------- | --------------------- | --------------------------------- |
| `Callback`                | 单个 `InetAddress`    | 只需要一个 IP（普通连接）         |
| `ResolverResultsCallback` | `vector<InetAddress>` | 需要所有 IP（负载均衡、健康检查） |

**工厂函数的编译时路由**：

```
编译时是否有 c-ares？
    │
    ├── 有 c-ares → newResolver() 返回 AresResolver
    └── 没有 c-ares → newResolver() 返回 NormalResolver（NormalResolver.cc 里实现）
```

`NormalResolver.cc` 里的 `newResolver` 只创建 `NormalResolver`；如果编译了 c-ares，对应的 cc 文件里会覆盖这个工厂函数，返回 `AresResolver`。

---

## 三、NormalResolver — 线程池方案

### 3.1 核心：`getaddrinfo` + `ConcurrentTaskQueue`

```cpp
void NormalResolver::resolve(const std::string &hostname,
                             const Callback &callback)
{
    // ① 先查全局缓存（加锁）
    {
        std::lock_guard<std::mutex> guard(globalMutex());
        auto iter = globalCache().find(hostname);
        if (iter != globalCache().end()) {
            auto &cachedAddr = iter->second;
            // 缓存未过期？
            if (timeout_ == 0 ||
                cachedAddr.second.after(timeout_) > Date::date()) {
                callback(cachedAddr.first);   // ← 直接从缓存返回
                return;
            }
        }
    }

    // ② 缓存未命中：投递到线程池
    concurrentTaskQueue().runTaskInQueue(
        [thisPtr = shared_from_this(), callback, hostname]() {
            // ③ 在工作线程里再查一次缓存（防止并发重复查询）
            {
                std::lock_guard<std::mutex> guard(thisPtr->globalMutex());
                // ... 同①的缓存检查 ...
                if (hit) { callback(cachedAddr.first); return; }
            }

            // ④ 调用阻塞的系统 DNS 解析
            struct addrinfo hints, *res = nullptr;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = PF_UNSPEC;      // 支持 IPv4 和 IPv6
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_PASSIVE;
            auto error = getaddrinfo(hostname.data(), nullptr, &hints, &res);

            if (error != 0 || res == nullptr) {
                callback(InetAddress{});      // 解析失败：返回空地址
                return;
            }

            // ⑤ 提取第一个地址（IPv4 优先）
            InetAddress inet;
            if (res->ai_family == AF_INET) {
                inet = InetAddress(*reinterpret_cast<sockaddr_in*>(res->ai_addr));
            } else if (res->ai_family == AF_INET6) {
                inet = InetAddress(*reinterpret_cast<sockaddr_in6*>(res->ai_addr));
            }
            freeaddrinfo(res);

            // ⑥ 直接调用回调（在工作线程里！）
            callback(inet);

            // ⑦ 更新全局缓存
            {
                std::lock_guard<std::mutex> guard(thisPtr->globalMutex());
                auto &addrItem = thisPtr->globalCache()[hostname];
                addrItem.first = inet;
                addrItem.second = Date::date();   // 记录缓存时间
            }
        });
}
```

### 3.2 双重缓存检查（Double-Check）

```
调用 resolve()
    │
    ├─ 加锁检查缓存（快速路径）
    │   命中 → 直接返回
    │   未命中 → 投递到线程池
    │
    └─ 线程池里再次检查缓存（防止并发重复查询）
         命中 → 直接返回（多个相同 hostname 的查询，第一个完成后，后续命中缓存）
         未命中 → getaddrinfo（真正的网络查询）
```

这是典型的**双重检验锁（Double-Checked Locking）**思路：
1. 第一次检查：快速，在调用者线程（可能是 EventLoop 线程），避免不必要的线程切换
2. 第二次检查：在工作线程里，防止两个相同的 hostname 查询同时穿透缓存

### 3.3 全局静态资源（Meyers' Singleton）

```cpp
// 所有 NormalResolver 实例共享同一个缓存和线程池
static std::unordered_map<std::string, std::pair<InetAddress, Date>>&
globalCache() {
    static std::unordered_map<std::string, std::pair<InetAddress, Date>> dnsCache_;
    return dnsCache_;
}

static std::mutex& globalMutex() {
    static std::mutex mutex_;
    return mutex_;
}

static ConcurrentTaskQueue& concurrentTaskQueue() {
    static ConcurrentTaskQueue queue(
        std::thread::hardware_concurrency() < 8
            ? 8                               // 最少 8 个线程
            : std::thread::hardware_concurrency(),  // 或 CPU 核心数
        "Dns Queue");
    return queue;
}
```

**全局共享**的原因：
- DNS 缓存是全进程共享的（相同 hostname 只需查一次）
- 线程池全局共享，避免每个 `NormalResolver` 实例创建独立线程，浪费资源

**`getaddrinfo` 的线程安全**：`getaddrinfo` 是**线程安全**的（POSIX 保证），多个线程并发调用不同 hostname 没有问题，所以可以用多线程并发执行。

### 3.4 回调在工作线程里调用！

**重要注意**：`NormalResolver` 的回调**在工作线程里直接调用**，而不是回调到 EventLoop 线程。

```cpp
// 工作线程里：
callback(inet);  // ← 直接调用，此时在 ConcurrentTaskQueue 的某个线程
```

这意味着如果回调里访问 EventLoop 上的对象（如 `TcpConnection`），**必须用 `runInLoop` 跳回 EventLoop 线程**！

```cpp
// 正确用法：
resolver->resolve("game.server.com", [loop, conn](const InetAddress& addr) {
    loop->runInLoop([conn, addr]() {
        // 现在在 EventLoop 线程，可以安全操作 conn
        auto client = TcpClient(loop, addr, "client");
        client.connect();
    });
});
```

---

## 四、AresResolver — 真正的异步方案

### 4.1 c-ares 是什么？

c-ares 是一个 C 语言异步 DNS 解析库。它不创建自己的线程，而是把 DNS socket 的 I/O 事件注册到外部事件循环（这里是 trantor 的 EventLoop），实现真正的非阻塞 DNS 解析。

### 4.2 核心结构

```cpp
class AresResolver {
  private:
    trantor::EventLoop *loop_;   // c-ares 运行在这个 EventLoop 上
    ares_channel ctx_;           // c-ares 内部 channel（管理 DNS 请求状态）
    bool timerActive_{false};    // c-ares 超时定时器是否在运行
    ChannelList channels_;       // c-ares 创建的 DNS socket → trantor::Channel 映射
    // map<int sockfd, unique_ptr<Channel>>
};
```

**`ChannelList channels_`**：c-ares 发起 DNS 查询时会创建 socket，通过回调告诉 AresResolver 这些 socket。AresResolver 把每个 socket 包装成 `trantor::Channel` 注册到 EventLoop，这样 EventLoop 的 epoll 就能监听 DNS socket 的可读/可写事件。

### 4.3 工作流程

```
AresResolver::resolve(hostname, cb)
    │
    ├─ 查缓存（命中直接 cb）
    │
    └─ 未命中 → resolveInLoop(hostname, cb)
         │
         ▼
    ares_getaddrinfo(ctx_, hostname, ...)
    ← c-ares 内部发起 DNS 查询（创建 UDP/TCP socket）
         │
    ares_sock_createcallback_()  ← c-ares 通知"新 socket 创建了"
    onSockCreate(sockfd, type)
         │
         ▼
    channels_[sockfd] = make_unique<Channel>(loop_, sockfd)
    channel->setReadCallback([this, sockfd]() { onRead(sockfd); })
    channel->enableReading()    ← 注册到 EventLoop 的 epoll
         │
    [DNS 服务器响应，sockfd 可读]
         │
         ▼
    epoll_wait() 返回 sockfd 可读
    Channel::handleEvent() → onRead(sockfd)
         │
         ▼
    ares_process_fd(ctx_, sockfd, ARES_SOCKET_BAD)  ← 让 c-ares 处理读事件
         │
    [c-ares 解析 DNS 响应]
         │
         ▼
    ares_hostcallback_(data, status, timeouts, result)
    onQueryResult(status, result, hostname, callback)
         │
         ▼
    更新缓存 → callback(解析结果)   ← 在 EventLoop 线程里回调！
```

### 4.4 AresResolver 与 NormalResolver 的回调线程对比

|              | NormalResolver                         | AresResolver                   |
| ------------ | -------------------------------------- | ------------------------------ |
| DNS 查询线程 | ConcurrentTaskQueue 工作线程           | EventLoop 线程（完全非阻塞）   |
| 回调线程     | **工作线程**（需手动回调到 EventLoop） | **EventLoop 线程**（直接安全） |
| 适用场景     | 无 c-ares 时的兜底方案                 | 高频 DNS 查询，追求极低延迟    |

### 4.5 全局 EventLoop（AresResolver）

```cpp
static EventLoop* getLoop() {
    static EventLoopThread loopThread;
    loopThread.run();
    return loopThread.getLoop();
}
```

如果没有传入 `loop`（`newResolver(nullptr)`），AresResolver 使用一个全局静态的 `EventLoopThread`——这是 Meyers' Singleton 模式，线程安全的惰性初始化（C++11 保证）。这意味着所有"无 loop"的 AresResolver 共用同一个后台 EventLoop 线程。

### 4.6 超时处理

c-ares 有内部超时机制，通过定期调用 `ares_process_fd(ctx_, ARES_SOCKET_BAD, ARES_SOCKET_BAD)` 触发超时检查。AresResolver 用 `loop_->runAfter()` 注册定时器实现这个周期性检查：

```cpp
void AresResolver::onTimer() {
    ares_process_fd(ctx_, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    // 如果 c-ares 还有活跃查询，继续调度下一次 timer
}
```

---

## 五、缓存机制对比

两种实现都有全局 DNS 缓存，但结构略有不同：

|           | NormalResolver                                     | AresResolver                                                         |
| --------- | -------------------------------------------------- | -------------------------------------------------------------------- |
| 缓存类型  | `unordered_map<string, pair<InetAddress, Date>>`   | `unordered_map<string, pair<shared_ptr<vector<InetAddress>>, Date>>` |
| 支持多 IP | 否（只缓存第一个）                                 | **是**（缓存完整结果列表）                                           |
| 超时判断  | `cachedAddr.second.after(timeout_) > Date::date()` | 相同逻辑                                                             |
| timeout=0 | 永不过期                                           | 永不过期                                                             |

**缓存设计的细节**：

```cpp
// 时间判断：缓存时间 + 超时时长 > 当前时间 → 缓存有效
cachedAddr.second.after(timeout_) > trantor::Date::date()
// 等价于：(缓存时间 + timeout_秒) > 现在
// 即：距缓存时间已过 < timeout_ 秒 → 有效
```

---

## 六、选择建议

```
游戏服务器是否需要频繁 DNS 解析？
    │
    ├─ 否（服务器间连接用 IP，只在启动时解析一次）
    │   → NormalResolver 足够，不必依赖 c-ares
    │
    └─ 是（动态服务发现、跨数据中心路由）
        → 安装 c-ares，使用 AresResolver，彻底不阻塞 EventLoop
```

---

## 七、游戏服务器实践

### 7.1 启动时解析服务器地址

```cpp
// 游戏服启动时解析逻辑服地址（一次性）
auto resolver = Resolver::newResolver();
resolver->resolve("logic.internal.game.com",
    [&loop](const InetAddress &addr) {
        if (!addr.isUnspecified()) {
            // 注意：NormalResolver 的回调在工作线程！
            loop.runInLoop([addr, &loop]() {
                auto client = std::make_shared<TcpClient>(
                    &loop, addr, "LogicSrv");
                client->connect();
            });
        } else {
            LOG_ERROR << "DNS 解析失败：logic.internal.game.com";
        }
    });
```

### 7.2 缓存超时配置

```cpp
// 内部服务：IP 基本不变，缓存 1 小时（3600秒）
auto resolver = Resolver::newResolver(nullptr, 3600);

// 外部服务：可能有 CDN 动态调度，缓存 60 秒
auto resolver = Resolver::newResolver(nullptr, 60);

// 调试/测试：不缓存
auto resolver = Resolver::newResolver(nullptr, 0);   // timeout=0 → 永不过期
// ⚠️ 注意：timeout=0 在代码里是"永不过期"而不是"不缓存"！
```

### 7.3 获取所有 IP（负载均衡）

```cpp
// 解析所有 A 记录（用于负载均衡）
resolver->resolve("api.game.com",
    [](const std::vector<InetAddress> &addrs) {
        LOG_INFO << "解析到 " << addrs.size() << " 个地址";
        for (auto &addr : addrs) {
            LOG_INFO << "  " << addr.toIpPort();
        }
        // 随机选一个，或轮询
        auto &selected = addrs[rand() % addrs.size()];
        // ... 连接 ...
    });
```

---

## 八、思考题

1. `NormalResolver` 的回调在 `ConcurrentTaskQueue` 的工作线程里直接调用，而 `AresResolver` 的回调在 `EventLoop` 线程里调用。如果用户代码不清楚这个区别，在回调里直接操作 `TcpConnection`，会发生什么问题？trantor 的 API 设计是否应该统一这个行为？

2. `NormalResolver::concurrentTaskQueue()` 最少创建 8 个线程（`hardware_concurrency() < 8 ? 8 : ...`）。考虑一个服务器只有 4 个 CPU 核心，却创建了 8 个线程，为什么 DNS 查询线程数多于 CPU 核心数是合理的？（提示：`getaddrinfo` 的主要开销是什么？）

3. 缓存的 `timeout=0` 含义是"永不过期"。如果游戏服务器做了 DNS 轮转（DNS Round Robin，同一 hostname 轮流返回不同 IP 实现负载均衡），使用永不过期的缓存会有什么问题？如何在 trantor 框架下解决？

4. `AresResolver` 使用全局静态 `EventLoopThread`（通过 `getLoop()` 惰性初始化）。如果用户在主线程的析构阶段（`main()` 结束后的静态析构顺序）调用 DNS 解析，这个全局 EventLoopThread 可能已经被析构，会发生什么？C++ 的"静态析构顺序灾难"如何影响到这里？

---

*学习日期：2026-04-02 | 上一课：[第15课_TLS安全通信](第15课_TLS安全通信.md) | 下一课：[第17课_并发工具与对象池](第17课_并发工具与对象池.md)*

---

## 核心收获

- `NormalResolver`：`getaddrinfo`（阻塞）→ `ConcurrentTaskQueue`（8线程池）→ **回调在工作线程**，必须 `runInLoop` 回归 EventLoop 再操作连接
- `AresResolver`：c-ares DNS socket 封装为 `Channel` 注册到 epoll，**回调直接在 EventLoop 线程**，无需切回
- 全局 Meyers Singleton 缓存：`NormalResolver` 所有实例共享一个缓存和线程池，避免重复解析
- `timeout=0` 反直觉：c-ares 中 timeout=0 意为 TTL 最大值无上限（永不过期），而非"立即超时"
- 高频 DNS 场景用 `AresResolver`；简单低频查询用 `NormalResolver`（更简单，依赖更少）
