+++
date = '2026-04-05'
draft = false
title = 'trantor 网络库学习总结'
categories = ["网络编程"]
tags = ["C++", "网络库", "trantor", "IOCP", "异步IO", "学习笔记"]
description = "近一个月深入学习 trantor 网络库的总结，涵盖核心架构、事件循环、异步 IO 等关键设计。"
+++


# trantor 网络库学习总结

> 学习周期：近一个月
> 覆盖范围：trantor 全部核心模块，共 18 课

---

## 一、整体架构鸟瞰

```
┌─────────────────────────────────────────────────────────┐
│                    用户代码 / Drogon 框架                  │
├─────────────────────────────────────────────────────────┤
│  TcpServer / TcpClient                                   │
│    • 连接管理（connSet_）    • Round-Robin 分配           │
│    • TimingWheel 超时        • promise/future 优雅停止    │
├────────────────┬────────────────────────────────────────┤
│  TcpConnection │  TaskQueue（Serial / Concurrent）       │
│    • 状态机    │    • 卸载阻塞操作                         │
│    • 发送队列  │    • SerialTaskQueue = EventLoopThread  │
│    • TLS透明层 │    • ConcurrentTaskQueue = 线程池        │
├────────────────┴────────────────────────────────────────┤
│  EventLoopThread / EventLoopThreadPool                   │
│    • 3阶段 promise/future 启动协议                        │
│    • atomic round-robin 无锁分配                          │
├──────────┬──────────────┬──────────────────────────────┤
│ Acceptor │  Connector   │  Resolver（DNS 异步解析）      │
│  idleFd_ │  EINPROGRESS │    • NormalResolver（线程池）  │
│  EMFILE  │  指数退避     │    • AresResolver（c-ares）   │
├──────────┴──────────────┴──────────────────────────────┤
│              EventLoop（Reactor 核心）                    │
│    loop() ← Channel ← Poller（epoll/kqueue/IOCP）        │
│    runInLoop / queueInLoop / runAfter / runEvery          │
│    MpscQueue<Func>：无锁任务投递                           │
├──────────────────────────┬──────────────────────────────┤
│     定时器系统             │       工具层                  │
│  TimerQueue（最小堆）      │  MsgBuffer / Logger           │
│  TimingWheel（O(1) 超时） │  ObjectPool / MpscQueue       │
│  timerfd / wakeupFd 驱动  │  Hash / secureRandomBytes    │
└──────────────────────────┴──────────────────────────────┘
                              ↓
              OS：epoll / kqueue / IOCP / wepoll
```

---

## 二、18 课核心知识点速查

### 阶段一：基础工具层（第 1-4 课）

#### 第 1 课 — 日志系统
- `LOG_INFO << "msg"` 展开为 `Logger(__FILE__, __LINE__).stream()`，析构时刷出
- `FixedBuffer<N>`：栈上固定缓冲，避免日志路径的堆分配
- `AsyncFileLogger`：前台线程写入内存队列，后台线程批量刷盘（异步、不阻塞 I/O）
- 自定义输出：`Logger::setOutputFunction()`，可对接 ELK、syslog 等

#### 第 2 课 — 消息缓冲区 MsgBuffer
- 双指针设计：`_readIndex` / `_writeIndex`，中间是可读数据，右侧是可写空间
- `prepend` 区域（8字节）：预留报头空间，避免插入时移动数据
- `readFd()`：`readv` + 栈上 65536 字节备用缓冲，单次 syscall 读取大量数据
- `BufferNode` 4种子类：`MemBufferNode`、`FileBufferNodeUnix`、`FileBufferNodeWin`、`AsyncStreamBufferNode`

#### 第 3 课 — 日期时间与工具函数
- `Date`：微秒精度时间点（`int64_t microSecondsSinceEpoch_`），可作定时器 key
- `Date::now()` → `gettimeofday` / `GetSystemTimeAsFileTime`
- `NonCopyable`：`= delete` 拷贝构造和赋值，所有核心类的基类

#### 第 4 课 — 回调类型定义
- `ConnectionCallback`：连接建立/断开
- `RecvMessageCallback`：收到数据（`TcpConnectionPtr` + `MsgBuffer*`）
- `WriteCompleteCallback`：发送缓冲区清空
- `TimerCallback`：定时器触发

---

### 阶段二：Reactor 核心（第 5-8 课）

#### 第 5 课 — EventLoop
- 核心循环：`epoll_wait` → 分发 Channel 事件 → 执行 `pendingFunctors_`
- `wakeupFd_`（eventfd/pipe）：跨线程唤醒阻塞的 `epoll_wait`
- `runInLoop(f)`：当前线程直接执行；其他线程 → `queueInLoop` → 唤醒 → 下轮执行
- `MpscQueue<Func> funcs_`：任务队列用无锁 MPSC 队列，多线程投递无锁

**关键不变量**：EventLoop 是单线程的，所有网络操作必须在其线程执行。

#### 第 6 课 — Channel
- Channel 不拥有 fd，是 fd 的事件管理代理
- `enableReading/Writing()` → `update()` → `Poller::updateChannel()`
- `tie(shared_ptr)`：防止 Channel 在 `handleEvent()` 期间被析构（持有所有者的弱引用）
- 三种状态：`kNew`（未注册）/ `kAdded`（已注册）/ `kDeleted`（已移除）

#### 第 7 课 — Poller（I/O 多路复用）
- 抽象接口：`poll()` / `updateChannel()` / `removeChannel()`
- `EpollPoller`：`epoll_create1(EPOLL_CLOEXEC)` + `epoll_ctl` + `epoll_wait`，ET/LT 可配
- `KQueuePoller`：BSD/macOS，`kqueue()` + `kevent()`
- Windows：`wepoll`（IOCP 模拟 epoll），对上层 100% 透明
- `PollPoller`：POSIX 兜底，性能较低

#### 第 8 课 — 定时器系统
- `TimerQueue`：最小堆（`std::priority_queue`），到期时间最早的排堆顶
- Linux 用 `timerfd_create` 接入 epoll；非 Linux 用 `wakeupFd` 定期唤醒
- `TimerID = (Timer*, seq)`：防止同地址重用的定时器误删
- `TimingWheel`：时间轮，O(1) 插入/删除，专为大量连接的**心跳/空闲超时**设计
  - `EntryPtr`（`shared_ptr`）放入槽位，引用计数归零 = 超时触发回调
  - `extendLife()` 把条目移到最新槽位（重置超时）

---

### 阶段三：TCP 网络通信（第 9-12 课）

#### 第 9 课 — 网络地址与 Socket 封装
- `InetAddress`：`union { sockaddr_in, sockaddr_in6 }` + `isIpV6_` 标志
- `getSockAddr()` 始终返回 `addr6_` 指针（两个结构起始地址相同）
- `isIntranetIp()`：RFC 1918 位移检测（`10.x.x.x` / `172.16-31.x.x` / `192.168.x.x`）
- `Socket`：RAII fd 包装，析构自动 `close(fd_)`
- Linux 原子创建非阻塞 socket：`SOCK_NONBLOCK | SOCK_CLOEXEC`（防 fork 泄漏）
- `accept4()`：Linux 原子接受并设置非阻塞，其他平台 `accept()` + `fcntl`

#### 第 10 课 — Acceptor & Connector
- `Acceptor::idleFd_`：预先打开 `/dev/null`，EMFILE 时关闭→接受→立即关闭→重新打开，优雅拒绝连接
- `beforeListenSetSockOptCallback_` / `afterAcceptSetSockOptCallback_`：两个钩子，灵活配置 socket 选项
- 非阻塞 `connect()` 返回 `EINPROGRESS`：注册写事件，写事件触发后用 `getsockopt(SO_ERROR)` 检查真实结果
- errno 分类：可重试（`EINTR/EAGAIN/EADDRINUSE/...`）vs 致命（`ECONNREFUSED/ENETUNREACH/...`）
- 指数退避重试：500ms → 1s → 2s → ... → 30s 封顶，`shared_from_this()` 保活

#### 第 11 课 — TcpConnection 连接生命周期
```
Connecting → Connected → Disconnecting → Disconnected
```
- `connectEstablished()`：`runInLoop` 确保在 Loop 线程，`tie()` 防析构，`enableReading()`
- `sendInLoop()` 快速路径：先尝试直接 `write()`，无需经过缓冲区（减少一次拷贝）
- `writeBufferList_`：`list<BufferNodePtr>` 多态节点（内存/文件/流/异步流）
- Linux `sendfile()` 零拷贝：内核直接 fd→fd，跳过用户态
- `shutdown()`：半关闭（`SHUT_WR`），等待对端关闭后收到 FIN 再 `close()`
- `forceClose()`：立即关闭，无论缓冲区是否清空
- `KickoffEntry`：RAII，析构 = 调用 `forceClose()`，放入 `TimingWheel` 实现连接超时

#### 第 12 课 — TcpServer & TcpClient
- TcpServer 三层线程：Accept 线程 + I/O 线程池 + 业务线程
- Round-Robin：`nextLoopIdx_++ % loopNum` 分配新连接到各 I/O EventLoop
- `connSet_`：`set<TcpConnectionPtr>` 持有 `shared_ptr`，是连接的所有权持有者
- `connectionClosed` 双 `queueInLoop`：I/O 线程 → Accept 线程，保证 `connSet_.erase` 在正确线程
- `TcpClient::connection_` 由 `mutex_` 保护（供跨线程安全读取）
- `retry_`（用户意图）vs `connect_`（当前状态），两个标志共同控制重连逻辑
- Connector 回调中用 `weak_ptr<TcpClient>`，防止 Client 析构后 use-after-free

---

### 阶段四：线程模型（第 13-14 课）

#### 第 13 课 — 多线程 EventLoop
- `EventLoopThread` 三阶段启动协议：
  1. `promiseForLoopPointer_`：Loop 对象创建完成，返回指针
  2. `promiseForRun_`：主线程信号，允许进入 `loop()`
  3. `promiseForLoop_`（可选）：`queueInLoop` 回调确认 Loop 已在运行
- `thread_local static shared_ptr<EventLoop>`：在 Loop 线程上可用 `EventLoop::getEventLoopOfCurrentThread()`
- `std::call_once`：防止 `run()` 被多次调用

- `EventLoopThreadPool`：`vector<shared_ptr<EventLoopThread>>`
- `atomic<size_t> loopIndex_` + `fetch_add(memory_order_relaxed)`：无锁 Round-Robin

#### 第 14 课 — 任务队列
- `TaskQueue` 基类：`syncTaskInQueue` 用 `promise/future` 实现同步等待，子类免费获得
- `SerialTaskQueue` = `EventLoopThread` 的包装，~50 行代码，全部委托给 `runInLoop`
  - 串行保证来自单线程 EventLoop，不需要额外锁
  - `waitAllTasksFinished()` = `syncTaskInQueue(空任务)`
- `ConcurrentTaskQueue` = 经典线程池（`mutex + condition_variable + queue`）
  - `notify_one()` 防惊群
  - `while(!stop_ && queue.empty())` 防虚假唤醒
  - 任务在**锁外**执行（先移出队列再释放锁）

|          | SerialTaskQueue  | ConcurrentTaskQueue |
| -------- | ---------------- | ------------------- |
| 底层     | EventLoopThread  | 传统线程池          |
| 并发度   | 严格串行         | N 线程并发          |
| 任务顺序 | 保证             | 不保证              |
| 用途     | 同一玩家 DB 操作 | 独立的批量任务      |

---

### 阶段五：高级特性（第 15-18 课）

#### 第 15 课 — TLS 安全通信
- `TLSProvider` 策略模式：OpenSSL / Botan 可互换，`newTLSProvider()` 工厂选择
- 热路径用**原始函数指针**（非 `std::function`），避免虚函数 + 堆分配开销
- `TLSPolicy` Builder 模式：链式 setter，`defaultServerPolicy()` vs `defaultClientPolicy()`
- 双缓冲：`recvBuffer_`（已解密明文）+ `writeBuffer_`（待发送密文）
- TLS 透明层：TcpConnection 通过静态函数指针插入加解密，对上层完全透明
- 热重载：`reloadSSL()` 不中断现有连接更换证书

#### 第 16 课 — DNS 解析
- `Resolver` 抽象工厂：`newResolver(loop, timeout)` 编译期选择实现
- `NormalResolver`：`getaddrinfo`（阻塞）投入 `ConcurrentTaskQueue`（8线程池）
  - 全局 Meyers Singleton 缓存 + 工作队列
  - ⚠️ **回调在工作线程触发**，必须 `conn->getLoop()->runInLoop(cb)` 回归 EventLoop
- `AresResolver`：c-ares 非阻塞，DNS socket 封装为 `Channel` 注册到 epoll
  - **回调在 EventLoop 线程触发**，无需手动切回线程
  - `timeout=0` 意为永不过期（反直觉！）

#### 第 17 课 — 并发工具与对象池
- `MpscQueue<T>`：多生产者单消费者无锁队列
  - 入队两步：① `head_.exchange(node, acq_rel)`（原子，多线程安全）② `prevhead->next_.store(node, release)`
  - 出队：`tail->next_.load(acquire)` 配对，单线程专用
  - trantor 实际用途：`EventLoop::funcs_` 任务投递队列
- `ObjectPool<T>`：`shared_ptr` 自定义 deleter 实现归还语义
  - 引用计数归零 → 触发 deleter → 归还到 `objs_` 而非 `delete`
  - `weak_ptr` 持有 Pool 防循环引用
  - 使用约束：Pool 必须通过 `make_shared` 创建

#### 第 18 课 — 密码学工具
- 编译期三路后端：`USE_OPENSSL` → EVP API / `USE_BOTAN` → Botan / 无后端 → 内置纯 C
- 哈希类型：`Hash128`（MD5）/ `Hash160`（SHA1）/ `Hash256`（SHA256/SHA3/BLAKE2b）
- SHA1 保留原因：WebSocket 握手（RFC 6455）强制要求
- SHA3 降级链：OpenSSL 3.x → OpenSSL 1.x → 内置 Keccak
- `secureRandomBytes`：OpenSSL `RAND_bytes` / Botan `AutoSeeded_RNG` / 自实现 BLAKE2b-CSPRNG（Dan Kaminsky 设计）
- `toHexString`：字符查表法（`"0123456789ABCDEF"[c>>4]`），输出大写十六进制

---

## 三、核心设计模式

| 模式              | 体现位置                              | 解决的问题                  |
| ----------------- | ------------------------------------- | --------------------------- |
| **Reactor**       | EventLoop + Channel + Poller          | 单线程处理大量 I/O 事件     |
| **策略模式**      | TLSProvider / Resolver / Poller       | 后端可替换，接口稳定        |
| **RAII**          | Socket / TcpConnection / KickoffEntry | 资源自动释放，无泄漏        |
| **生产者-消费者** | ConcurrentTaskQueue                   | 解耦任务投递与执行          |
| **对象池**        | ObjectPool\<T\>                       | 减少 new/delete，降低碎片   |
| **无锁队列**      | MpscQueue\<T\>                        | 多线程投递任务无 mutex 开销 |
| **Builder**       | TLSPolicy                             | 复杂对象的链式配置          |
| **状态机**        | TcpConnectionImpl::ConnStatus         | 连接生命周期管理            |
| **时间轮**        | TimingWheel                           | O(1) 管理海量连接超时       |
| **半关闭**        | TcpConnection::shutdown()             | 优雅 TCP 四次挥手           |

---

## 四、关键"反直觉"设计备忘

这些设计初看奇怪，但有充分理由：

1. **`getSockAddr()` 始终返回 `addr6_` 的地址**
   → `sockaddr_in` 和 `sockaddr_in6` 的起始字段布局兼容，同一指针能被系统调用正确识别

2. **`Acceptor::idleFd_` 预占一个 `/dev/null` fd**
   → EMFILE（fd 耗尽）时临时借用 idleFd_ 接受连接再立即关闭，让客户端收到干净的 RST 而非卡住

3. **`SerialTaskQueue::waitAllTasksFinished()` 投递一个空任务**
   → 串行执行保证：空任务执行时，它前面所有任务都已完成；`syncTaskInQueue` 等空任务完成即等到了"清空"

4. **`ConcurrentTaskQueue` 任务在锁外执行**
   → 持锁执行任务会死锁（任务内无法再投递任务），也会阻塞其他工作线程取任务

5. **`EventLoopThread` 三阶段启动**
   → 分离"对象创建"与"开始 loop()"，允许在 loop 开始前安全配置 EventLoop

6. **`NormalResolver` 回调在工作线程，`AresResolver` 回调在 EventLoop 线程**
   → `NormalResolver` 用 `getaddrinfo`（阻塞）必须在线程池执行，但 EventLoop 不能直接跨线程访问；`AresResolver` 的 DNS socket 通过 Channel 注册到 epoll，自然在 EventLoop 线程触发

7. **`AresResolver` 的 `timeout=0` 意味着永不过期**
   → c-ares 的 timeout 是 TTL 的最大值，0 表示不设上限（cache entries never expire）

8. **`TcpClient` 两个布尔：`retry_` 和 `connect_`**
   → `retry_`：用户是否开启断线重连；`connect_`：当前是否"应该"处于连接状态（用户是否调用 `disconnect()`）。两者分开才能正确区分"用户主动断开"和"网络异常断开"

---

## 五、游戏服务器常用模式

### 5.1 标准异步 DB 操作模式

```cpp
// I/O 线程收到请求 → 投递到 DB 串行队列 → 完成后回调回 I/O 线程
void onPlayerRequest(const TcpConnectionPtr &conn, const Request &req) {
    dbQueue_.runTaskInQueue([conn, req]() {       // DB 线程执行（可阻塞）
        auto result = db.query(req.sql);
        conn->getLoop()->runInLoop([conn, result]() {  // 回到 I/O 线程发送
            conn->send(buildResponse(result));
        });
    });
}
```

### 5.2 连接心跳超时

```cpp
// TcpServer 内置支持
server.setIoLoopNum(4);
server.kickoffIdleConnections(60);  // 60秒无数据则踢掉

// 手动方案：客户端每次收到数据时延续生命
void onMessage(const TcpConnectionPtr &conn, MsgBuffer *buf) {
    conn->extendLife();   // 重置超时计时器
    // ... 处理消息
}
```

### 5.3 同一玩家操作串行化

```cpp
class PlayerSession {
    SerialTaskQueue dbQueue_{"Player-DB"};  // 该玩家的独占串行队列

    void saveInventory(const Inventory &inv) {
        dbQueue_.runTaskInQueue([inv, conn = conn_]() {
            db.update("inventory", inv);
            conn->getLoop()->runInLoop([conn]() {
                conn->send(buildAck());
            });
        });
    }
};
```

### 5.4 批量任务并发处理

```cpp
ConcurrentTaskQueue compressQueue(4, "Compress");
for (auto &logFile : pendingLogs) {
    compressQueue.runTaskInQueue([logFile]() {
        gzipCompress(logFile);   // CPU 密集，4线程并发
    });
}
```

---

## 六、学习路径回顾

```
阶段一（工具层）：Logger → MsgBuffer → Date → Callbacks
    ↓ 理解基础数据结构
阶段二（Reactor）：EventLoop → Channel → Poller → Timer
    ↓ 掌握单线程事件驱动
阶段三（TCP通信）：InetAddress → Socket → Acceptor/Connector → TcpConnection → TcpServer/Client
    ↓ 完整 TCP 生命周期
阶段四（线程模型）：EventLoopThread → EventLoopThreadPool → TaskQueue
    ↓ 多线程扩展能力
阶段五（高级特性）：TLS → DNS → MpscQueue/ObjectPool → 密码学工具
    ↓ 生产级特性
```

**trantor 的核心哲学**：
- **单线程 EventLoop 是基础**，所有 I/O 操作在其上执行，无需为 I/O 操作加锁
- **阻塞操作必须卸载**到 TaskQueue，EventLoop 只做非阻塞 I/O 和轻量计算
- **跨线程通信通过投递任务**（`runInLoop`），而非共享数据加锁
- **生命周期通过 `shared_ptr` 管理**，RAII 确保无泄漏
- **性能敏感路径避免虚函数和动态分配**（函数指针、栈缓冲、无锁队列）

---

*最后更新：2026-04-05*
