+++
date = '2026-04-20'
draft = false
title = 'TcpConnection — 连接生命周期'
categories = ["网络编程"]
tags = ["C++", "trantor", "TcpConnection", "学习笔记"]
description = "trantor TcpConnection 连接生命周期全解析，从建立到销毁的状态机管理。"
+++


# 第 11 课：TcpConnection — 连接生命周期

> 对应源文件：
> - `trantor/net/TcpConnection.h` — 公共抽象接口（用户使用）
> - `trantor/net/inner/TcpConnectionImpl.h` / `TcpConnectionImpl.cc` — 内部实现

---

## 一、设计：接口与实现分离

```
TcpConnection（纯虚基类）
    │  定义公共 API：send/sendFile/shutdown/forceClose/setContext...
    │  存储回调：recvMsgCallback_/connectionCallback_/closeCallback_...
    │
    └── TcpConnectionImpl（具体实现）
            继承 TcpConnection + NonCopyable + enable_shared_from_this
            │
            ├── Channel（ioChannelPtr_）— fd 事件分发
            ├── Socket（socketPtr_）   — RAII fd 管理
            ├── MsgBuffer readBuffer_  — 接收缓冲区
            ├── list<BufferNodePtr> writeBufferList_  — 发送队列
            └── TLSProvider（可选）    — 透明 TLS 加密层
```

**为什么分离接口和实现？**
- 用户代码只持有 `TcpConnectionPtr`（指向基类），不需要知道 TLS/非 TLS 的实现细节
- `TLSProvider` 可以透明地插在中间，上层代码完全不感知加密
- 便于测试 mock：只需要替换实现，不用改用户代码

---

## 二、连接状态机

```cpp
enum class ConnStatus {
    Disconnected,    // 初始/关闭完成
    Connecting,      // 构造后，connectEstablished() 之前
    Connected,       // 正常运行
    Disconnecting    // shutdown() 已调用，等发送缓冲区清空
};
```

**完整状态转移**：

```
构造 TcpConnectionImpl
        │ status_ = Connecting
        ▼
connectEstablished()
        │ status_ = Connected
        │ ioChannelPtr_->tie(shared_from_this())   ← 绑定生命周期
        │ ioChannelPtr_->enableReading()           ← 注册到 epoll
        │ connectionCallback_(conn)                ← 通知上层"连接建立"
        ▼
    [正常运行：数据收发]
        │
   用户调用 shutdown() ──────────────────────────────────────────────┐
        │ status_ = Disconnecting                                     │
        │ 若缓冲区空 → socketPtr_->closeWrite()（SHUT_WR）            │
        │ 若缓冲区非空 → closeOnEmpty_=true，等 writeCallback 清空后再关 │
        │                                                             │
   用户调用 forceClose() ──────────────────────────────────────────►handleClose()
        │ status_ = Disconnecting                                     │
        │ 直接调用 handleClose()                                      │
        ▼                                                             │
   对端关闭 → readCallback() n==0 → handleClose()                    │
        │                                                             │
        ◄────────────────────────────────────────────────────────────┘
handleClose()
        │ status_ = Disconnected
        │ ioChannelPtr_->disableAll()              ← 取消所有 epoll 事件
        │ connectionCallback_(conn)                ← 通知上层"连接断开"
        │ closeCallback_(conn)                     ← TcpServer 清理连接 map
        ▼
connectDestroyed()（由 TcpServer 调用）
        │ ioChannelPtr_->remove()                  ← 从 Poller 彻底注销
```

---

## 三、构造：初始化但不激活

```cpp
TcpConnectionImpl::TcpConnectionImpl(EventLoop *loop, int socketfd,
    const InetAddress &localAddr, const InetAddress &peerAddr, ...)
    : loop_(loop),
      ioChannelPtr_(new Channel(loop, socketfd)),   // 把 fd 包装成 Channel
      socketPtr_(new Socket(socketfd)),             // RAII 管理 fd
      localAddr_(localAddr),
      peerAddr_(peerAddr)
{
    ioChannelPtr_->setReadCallback([this]() { readCallback(); });
    ioChannelPtr_->setWriteCallback([this]() { writeCallback(); });
    ioChannelPtr_->setCloseCallback([this]() { handleClose(); });
    ioChannelPtr_->setErrorCallback([this]() { handleError(); });
    socketPtr_->setKeepAlive(true);   // 默认开启 TCP 保活
    name_ = localAddr.toIpPort() + "--" + peerAddr.toIpPort();
}
```

**注意**：构造时只注册回调，**不** `enableReading()`。Channel 还没有注册到 epoll。激活在 `connectEstablished()` 里完成，确保所有回调都设置好了再开始接收数据。

---

## 四、`connectEstablished()` — 激活连接

```cpp
void TcpConnectionImpl::connectEstablished()
{
    auto thisPtr = shared_from_this();
    loop_->runInLoop([thisPtr]() {
        assert(thisPtr->status_ == ConnStatus::Connecting);
        thisPtr->ioChannelPtr_->tie(thisPtr);        // ① 生命周期绑定
        thisPtr->ioChannelPtr_->enableReading();      // ② 注册到 epoll，开始接收
        thisPtr->status_ = ConnStatus::Connected;
        if (thisPtr->tlsProviderPtr_)
            thisPtr->tlsProviderPtr_->startEncryption();  // ③ TLS 握手
        else if (thisPtr->connectionCallback_)
            thisPtr->connectionCallback_(thisPtr);        // ③ 通知"连接建立"
    });
}
```

**`runInLoop` 而不是直接调用**：`connectEstablished()` 可能从非 EventLoop 线程调用，`runInLoop` 保证 Channel 操作在正确的线程执行。

**`tie()` 的意义**：把 `TcpConnectionImpl` 的 `shared_ptr` 绑到 `Channel`，保证 `handleEvent()` 执行期间 `TcpConnectionImpl` 不被析构（见第 6 课）。

---

## 五、发送数据：send() 的两条路径

`send()` 是最常用的接口，它有两条路径：

```
send(data, len)
        │
        ├─ 在 EventLoop 线程？
        │   YES → 直接 sendInLoop(data, len)
        │   NO  → 拷贝数据 → queueInLoop(sendInLoop)   ← 跨线程安全
        ▼
sendInLoop(buffer, length)
        │
        ├─ 没有写事件 && 发送队列为空？
        │   YES → 尝试直接 write(fd)                    ← 零拷贝快速路径
        │          写完了？→ 触发 writeCompleteCallback
        │          没写完？→ 剩余数据追加到 writeBufferList_
        │
        └─ 有未发完的数据
            → 追加到 writeBufferList_.back()（内存节点）
            → 检查 highWaterMark，超过就触发 HighWaterMarkCallback
```

### 5.1 快速路径：直接写

```cpp
void TcpConnectionImpl::sendInLoop(const void *buffer, size_t length)
{
    if (!ioChannelPtr_->isWriting() && writeBufferList_.empty()) {
        // 没有积压数据：尝试直接写入内核
        ssize_t sendLen = writeInLoop(buffer, length);
        length -= sendLen;
    }
    if (length > 0) {
        // 没写完：放入发送队列，等 epoll 通知可写再继续
        writeBufferList_.back()->append(buffer + sendLen, length);
        // 检查高水位...
    }
}
```

**优化意义**：在大多数情况下（内核发送缓冲区有足够空间），数据可以直接写入，不需要经过 `writeBufferList_`，减少一次数据拷贝和内存分配。

### 5.2 跨线程发送：数据拷贝是必要的

```cpp
// 跨线程调用 send(const char*, len)
auto buffer = std::make_shared<std::string>(msg, len);
loop_->queueInLoop([thisPtr = shared_from_this(), buffer = std::move(buffer)]() {
    thisPtr->sendInLoop(buffer->data(), buffer->length());
});
```

为什么要拷贝？调用 `send()` 的线程的栈空间（`msg` 指针指向的内存）在 lambda 执行时可能已经失效，必须把数据拷贝进 `shared_ptr<string>` 延长生命周期。

`send(shared_ptr<string>)` 和 `send(shared_ptr<MsgBuffer>)` 则不需要额外拷贝，因为 `shared_ptr` 本身就管理了生命周期。

---

## 六、`writeCallback()` — 发送缓冲区排空

当 epoll 通知"fd 可写"时调用：

```cpp
void TcpConnectionImpl::writeCallback()
{
    if (ioChannelPtr_->isWriting()) {
        // TLS 优先：先把 TLS 内部缓冲的数据发出去
        if (tlsProviderPtr_) {
            bool sentAll = tlsProviderPtr_->sendBufferedData();
            if (!sentAll) return;  // 还没发完，等下次
        }

        // 遍历 writeBufferList_，依次发送节点
        while (!writeBufferList_.empty()) {
            auto &nodePtr = writeBufferList_.front();
            if (nodePtr->remainingBytes() == 0) {
                writeBufferList_.pop_front();     // 节点发完，移除
            } else {
                auto n = sendNodeInLoop(nodePtr); // 继续发
                if (nodePtr->remainingBytes() > 0 || n < 0)
                    return;  // 内核缓冲区满（EAGAIN）或出错，等下次
            }
        }

        // 所有数据发完
        ioChannelPtr_->disableWriting();          // 取消写事件（防止 busy loop）
        if (writeCompleteCallback_)
            writeCompleteCallback_(shared_from_this());
        if (closeOnEmpty_)                        // 等发完再关（graceful shutdown）
            shutdown();
    }
}
```

**发送队列为 `list<BufferNodePtr>` 而不是单一 buffer 的原因**：

`BufferNode` 有四种子类型（第 2 课讲过），它们的数据来源不同：
- `MemBufferNode`：内存数据（用户 `send()` 的普通数据）
- `FileBufferNode`：文件（用于 `sendFile()`，Linux 用 `sendfile()` 零拷贝）
- `StreamBufferNode`：流式数据（回调函数按需生成）
- `AsyncStreamBufferNode`：异步流（外部异步推送数据）

用 `list` 而不是 `vector` 是因为**头部删除频繁**，`list` 是 O(1)，`vector` 是 O(n)。

---

## 七、`sendFile()` — Linux 零拷贝

```cpp
// sendNodeInLoop() 中对 FileNode 的特殊处理（仅 Linux）
#ifdef __linux__
if (nodePtr->isFile() && !tlsProviderPtr_) {
    auto bytesSent = sendfile(socketPtr_->fd(),  // 目标 fd
                              nodePtr->getFd(),   // 源文件 fd
                              nullptr,            // offset（由 node 内部管理）
                              toSend);
    // sendfile() 直接在内核中完成"文件 → socket 缓冲区"的数据传输
    // 不经过用户空间，零拷贝！
}
#endif
```

**`sendfile()` 的零拷贝原理**：

普通文件发送路径：
```
磁盘 → [DMA] → 内核页缓存 → [CPU拷贝] → 用户缓冲区 → [CPU拷贝] → socket缓冲区 → [DMA] → 网卡
```

`sendfile()` 路径：
```
磁盘 → [DMA] → 内核页缓存 → [DMA描述符传递] → socket缓冲区 → [DMA] → 网卡
```

减少了两次 CPU 拷贝，对大文件传输（如静态资源服务）有显著性能提升。

**TLS 情况下不能用 `sendfile()`**：加密需要在用户空间处理数据，必须先把数据读到内存，再加密，再写出。

---

## 八、关闭连接的两种方式

### 8.1 `shutdown()` — 优雅关闭（半关闭）

```cpp
void TcpConnectionImpl::shutdown()
{
    loop_->runInLoop([thisPtr]() {
        if (thisPtr->status_ == ConnStatus::Connected) {
            if (!thisPtr->writeBufferList_.empty()) {
                thisPtr->closeOnEmpty_ = true;   // 发完再关
                return;
            }
            thisPtr->status_ = ConnStatus::Disconnecting;
            thisPtr->socketPtr_->closeWrite();   // SHUT_WR
        }
    });
}
```

`shutdown()` 只关闭**写方向**（发送 FIN），不关闭读方向。对端收到 FIN 后知道我们不再发送，但可以继续发数据过来。当对端也 `shutdown()` 后，我们收到 FIN，触发 `readCallback()` 返回 0，再调用 `handleClose()` 完成完整关闭。

**`closeOnEmpty_` 标志**：如果发送缓冲区还有数据，不能立刻 SHUT_WR，标记 `closeOnEmpty_=true`，等 `writeCallback()` 把缓冲区清空后再调用 `shutdown()`。

### 8.2 `forceClose()` — 强制立刻关闭

```cpp
void TcpConnectionImpl::forceClose()
{
    loop_->runInLoop([thisPtr]() {
        if (status_ == Connected || status_ == Disconnecting) {
            thisPtr->status_ = ConnStatus::Disconnecting;
            thisPtr->handleClose();   // 直接触发关闭，不等缓冲区
        }
    });
}
```

`forceClose()` 丢弃发送缓冲区里的数据，立刻触发 `handleClose()`，对端会收到 RST（因为 socket 关闭时发送缓冲区非空）。

|                | `shutdown()`                             | `forceClose()`         |
| -------------- | ---------------------------------------- | ---------------------- |
| 等待发送缓冲区 | 是                                       | 否（丢弃）             |
| 对端感知       | 收到 FIN                                 | 可能收到 RST           |
| 适用场景       | 正常断线（如心跳超时后发完最后一帧再断） | 紧急踢出（作弊、异常） |

---

## 九、`KickoffEntry` — 空闲超时踢出机制

```cpp
class KickoffEntry {
  public:
    explicit KickoffEntry(const std::weak_ptr<TcpConnection> &conn)
        : conn_(conn) {}
    void reset() { conn_.reset(); }   // 取消踢出（调用 keepAlive() 时）

    ~KickoffEntry() {
        auto conn = conn_.lock();
        if (conn) conn->forceClose();  // RAII：析构时踢出连接
    }
  private:
    std::weak_ptr<TcpConnection> conn_;
};
```

**工作原理**（配合 TimingWheel）：

```
enableKickingOff(timeout=30, timingWheel)
    ↓
entry = make_shared<KickoffEntry>(weak_ptr<this>)
kickoffEntry_ = weak_ptr<entry>
timingWheel->insertEntry(30, entry)
    ↓
   [每次收到数据时 extendLife()]
       ↓
       entry = kickoffEntry_.lock()
       timingWheel->insertEntry(30, entry)  ← 重新插入，延长30秒
    ↓
   [30秒内没有收到数据]
       ↓
   entry 从 TimingWheel 中被移除（引用计数归零）
       ↓
   ~KickoffEntry() → conn->forceClose()   ← 踢出！
```

### `extendLife()` 的节流优化

```cpp
void TcpConnectionImpl::extendLife()
{
    if (idleTimeout_ > 0) {
        auto now = Date::date();
        if (now < lastTimingWheelUpdateTime_.after(1.0))
            return;   // 1秒内不重复更新，避免高频收包时频繁操作 TimingWheel
        lastTimingWheelUpdateTime_ = now;
        auto entry = kickoffEntry_.lock();
        if (entry) {
            timingWheelPtr->insertEntry(idleTimeout_, entry);
        }
    }
}
```

`extendLife()` 最多每秒更新一次 TimingWheel，防止高频收包（如心跳包每100ms一次）时产生大量 TimingWheel 操作。

---

## 十、TLS 透明集成

`TLSProvider` 像一个"过滤器"插在 TCP 连接和用户代码之间：

```
用户代码
    │ recvMsgCallback_(conn, buf)
    ▼
TLSProvider::onSslMessage       ← 解密后的数据
    │ 解密
    ▼
readBuffer_（TLS 密文）
    │
    ▲ writeRaw() ← 直接写密文到 socket
TLSProvider::sendData           ← 加密用户数据
    ▲
用户代码 conn->send(plaintext)
```

**关键回调绑定**（静态函数避免虚函数开销）：

```cpp
tlsProviderPtr_->setWriteCallback(onSslWrite);        // 加密后的数据 → writeRaw()
tlsProviderPtr_->setErrorCallback(onSslError);        // TLS 错误 → forceClose()
tlsProviderPtr_->setHandshakeCallback(onHandshakeFinished); // 握手完成 → connectionCallback_
tlsProviderPtr_->setMessageCallback(onSslMessage);    // 解密数据 → recvMsgCallback_
tlsProviderPtr_->setCloseCallback(onSslCloseAlert);   // 对端 close_alert → shutdown()
```

用户完全感知不到 TLS 的存在，`send()` / `recvMsgCallback_` 使用方式与非加密连接完全相同。

---

## 十一、完整数据接收流程

```
[网络数据到达]
        │
        ▼
epoll_wait() 返回 → Channel::handleEvent() → TcpConnectionImpl::readCallback()
        │
        ▼
readBuffer_.readFd(fd)      ← 调用 readv()，可能用栈上临时缓冲区（见第2课）
        │
   n == 0 → handleClose()   ← 对端关闭连接
   n < 0  → 错误处理
   n > 0  → bytesReceived_ += n
             extendLife()    ← 更新空闲超时计时器
             │
             ├─ 有 TLS → tlsProviderPtr_->recvData(&readBuffer_)
             │             → TLS 解密 → onSslMessage → recvMsgCallback_
             │
             └─ 无 TLS → recvMsgCallback_(shared_from_this(), &readBuffer_)
```

---

## 十二、游戏服务器实践

### 12.1 典型连接使用模式

```cpp
// 在 ConnectionCallback 里初始化连接（建立时）
server.setConnectionCallback([](const TcpConnectionPtr &conn) {
    if (conn->connected()) {
        // 关 Nagle，降低延迟
        conn->setTcpNoDelay(true);
        // 绑定玩家会话
        conn->setContext(std::make_shared<PlayerSession>());
        // 开启空闲踢出（60秒无数据自动断线）
        // TcpServer 内部调用：conn->enableKickingOff(60, timingWheel)
    } else {
        // 清理玩家状态
        auto session = conn->getContext<PlayerSession>();
        if (session) onPlayerDisconnect(session);
    }
});
```

### 12.2 主动踢出玩家

```cpp
// 检测到作弊：立刻强踢
void kickPlayer(const TcpConnectionPtr &conn) {
    conn->forceClose();  // 发 RST，立刻断线
}

// 正常下线：发完最后的数据包再断
void gracefulKick(const TcpConnectionPtr &conn) {
    conn->send(buildKickPacket());  // 先发踢出通知
    conn->shutdown();               // 发完后关写端
}
```

### 12.3 发送大文件（如补丁包）

```cpp
// 发送游戏客户端更新包（零拷贝，不占用内存）
conn->sendFile("/path/to/patch_v1.2.3.zip");
// Linux 内部用 sendfile()，直接从磁盘到网卡，对服务器 CPU 几乎没有开销
```

## 核心收获

- 状态机 `Connecting→Connected→Disconnecting→Disconnected`，每个状态有严格的合法操作集合
- `sendInLoop()` 快速路径：先尝试直接 `write()`，缓冲区空且无 EAGAIN 时无需经过写队列（减少一次拷贝）
- `writeBufferList_` 多态节点：内存/文件/流/异步流均可混合排队，`sendfile()` 在 Linux 实现零拷贝
- `shutdown()` 半关闭（`SHUT_WR`）vs `forceClose()` 立即关闭：优雅关闭等对端 FIN，强制关闭不等
- `KickoffEntry` RAII：析构 = 调用 `forceClose()`，放入 `TimingWheel` 自动实现连接超时踢人
- TLS 透明层：通过静态函数指针替换 read/write 路径，上层代码完全不感知加密

---

## 十三、思考题

1. `connectEstablished()` 用 `runInLoop` 保证在 EventLoop 线程执行，而 `forceClose()` 也用了 `runInLoop`。但 `handleClose()` 是直接在 EventLoop 线程里被调用的（来自 `readCallback()`）。如果用户从非 IO 线程调用 `forceClose()`，会不会和 IO 线程的 `readCallback()` 竞争 `status_`？为什么不会？

2. `closeOnEmpty_` 标志让 `shutdown()` 在缓冲区清空后才真正关闭。如果用户调用了 `shutdown()`，然后又调用了 `send()`，会发生什么？（提示：看 `sendInLoop()` 里 `status_` 的检查）

3. `extendLife()` 有一个 1 秒节流：如果超时时间设置为 2 秒，而每 0.5 秒收到一次数据，理论上应该永远不超时，但节流会不会导致实际超时时间变成 3 秒？（提示：分析最坏情况下 `lastTimingWheelUpdateTime_` 的更新时机）

4. `KickoffEntry` 析构时调用 `conn->forceClose()`，而此时持有的是 `weak_ptr`。如果 `TcpConnectionImpl` 已经在正常关闭流程中（`Disconnected` 状态），`lock()` 返回的指针不为空，`forceClose()` 会做什么？会出现双重关闭的问题吗？

---

## 十四、思考题参考答案

### 1. 从非 IO 线程调用 `forceClose()` 是否会和 IO 线程的 `readCallback()` 竞争 `status_`？

**不会竞争**，原因在于 `forceClose()` 内部使用了 `runInLoop` 机制。

来看源码：

```cpp
void TcpConnectionImpl::forceClose()
{
    auto thisPtr = shared_from_this();
    loop_->runInLoop([thisPtr]() {
        if (thisPtr->status_ == ConnStatus::Connected ||
            thisPtr->status_ == ConnStatus::Disconnecting)
        {
            thisPtr->status_ = ConnStatus::Disconnecting;
            thisPtr->handleClose();
            // ...
        }
    });
}
```

关键在于 `loop_->runInLoop(...)` 的语义：
- 如果当前已在 IO 线程（`isInLoopThread() == true`），lambda 会**立即同步执行**
- 如果当前不在 IO 线程，lambda 会被放入 `pendingFunctors_` 队列，等待 IO 线程在本次或下次 `poll` 返回后按序执行

而 `readCallback()` 也只在 IO 线程运行（开头有 `loop_->assertInLoopThread()`）。所以 `forceClose()` 中对 `status_` 的读写和 `readCallback()` 中对 `status_` 的读写**必然串行**——它们都在同一个 IO 线程的事件循环中按序执行，不存在并发访问。

这就是 trantor（以及 muduo）"one loop per thread"模型的核心保证：**所有对连接内部状态的操作都在其所属的 IO 线程中串行执行**。`runInLoop` / `queueInLoop` 是跨线程安全投递操作的唯一入口，它把并发问题转化为了队列消费问题。`status_` 不需要加锁、不需要 `atomic`，因为它只有一个线程会读写。

### 2. 调用 `shutdown()` 后又调用 `send()` 会发生什么？

需要分两种情况分析。

**情况一：`shutdown()` 的 lambda 先于 `send()` 的 lambda 执行**

`shutdown()` 执行后，如果发送缓冲区为空，`status_` 会被置为 `Disconnecting`，并调用 `socketPtr_->closeWrite()`（即 `SHUT_WR`）。之后 `send()` 投递的 lambda 执行 `sendInLoop()` 时，开头就会检查状态：

```cpp
void TcpConnectionImpl::sendInLoop(const void *buffer, size_t length)
{
    loop_->assertInLoopThread();
    if (status_ != ConnStatus::Connected)
    {
        LOG_DEBUG << "Connection is not connected,give up sending";
        return;   // ← 直接返回，数据被丢弃
    }
    // ...
}
```

`status_` 已经是 `Disconnecting`，不等于 `Connected`，所以 `sendInLoop` 直接返回，**数据被静默丢弃**，并打印 DEBUG 日志。

**情况二：`shutdown()` 时发送缓冲区非空**

此时 `shutdown()` 只设置 `closeOnEmpty_ = true` 并直接返回，`status_` 仍然是 `Connected`。那么后续 `send()` 的数据会正常追加到 `writeBufferList_` 中。当 `writeCallback()` 把所有数据发完后，检测到 `closeOnEmpty_`，再调用 `shutdown()`。此时缓冲区为空（刚发完），`shutdown()` 才真正执行 `SHUT_WR`。

**也就是说：`shutdown()` 后再 `send()` 的数据也会被发出去**——这看起来可能不符合预期，但实际上 `shutdown()` 的语义就是"等发完再关"，而不是"从现在起拒绝发送"。

**游戏服务器实践建议**：如果调用 `shutdown()` 后就不应该再发数据，应在业务层自行维护一个"正在关闭"标志，在 `send()` 前检查。不要依赖 trantor 的 `status_` 来阻止发送，因为 `closeOnEmpty_` 路径会让 `status_` 保持 `Connected`。

### 3. `extendLife()` 节流是否会导致超时时间从 2 秒变成 3 秒？

**最坏情况确实会延长约 1 秒，但不会到 3 秒。**

分析最坏时序，假设超时 = 2 秒，数据每 0.5 秒来一次：

```
T=0.00s  第一次收包，extendLife() 执行
         lastTimingWheelUpdateTime_ = 0.00
         TimingWheel 插入 entry，deadline = T+2 = 2.00s

T=0.50s  收包，now(0.50) < last(0.00)+1.0 = 1.00  → 被节流，不更新

T=0.99s  收包，now(0.99) < last(0.00)+1.0 = 1.00  → 仍被节流

T=1.00s  收包，now(1.00) >= last(0.00)+1.0 = 1.00  → 通过节流！
         lastTimingWheelUpdateTime_ = 1.00
         TimingWheel 插入 entry，deadline = T+2 = 3.00s

T=1.50s  收包，now(1.50) < last(1.00)+1.0 = 2.00  → 被节流

T=1.99s  收包，now(1.99) < last(1.00)+1.0 = 2.00  → 被节流

T=2.00s  收包，now(2.00) >= last(1.00)+1.0 = 2.00  → 通过节流！
         TimingWheel 插入 entry，deadline = T+2 = 4.00s
         ...
```

可以看到，在 `T=1.00s` 之前的数据包（`T=0.50`, `T=0.99`）都被节流了，但 `T=1.00s` 时更新了 deadline 到 `3.00s`。只要数据持续到来，连接永远不会被超时踢出。

**真正的风险场景是：最后一次收包恰好被节流。** 例如：

```
T=0.00s  extendLife() 更新，deadline = 2.00s
T=0.50s  最后一次收包，被节流（此后再无数据）
T=2.00s  TimingWheel 触发超时踢出
```

实际超时是"最后一次真实收包"后 1.5 秒（而不是精确的 2 秒）。最坏情况下，从最后一次收包到被踢出的时间范围是 `[timeout-1, timeout]` 秒，即 `[1, 2]` 秒。

**不会变成 3 秒**——因为节流只是推迟了"更新 TimingWheel"的操作，而不是增加了额外的超时周期。只要在 `[lastUpdate, lastUpdate+1)` 期间有数据来，`lastUpdate+1` 时刻的那次收包就会更新 deadline。

**设计取舍**：1 秒节流在高频收包场景（如每帧心跳 30-60 Hz）下可以减少大量的 TimingWheel 操作（从每秒 60 次降到 1 次），精度损失最多 1 秒，对于通常 30-300 秒的超时设置完全可以接受。但如果超时设置极短（如 2 秒），精度损失的比例就比较大（最多 50%），需要酌情调整。

### 4. `KickoffEntry` 析构时 `forceClose()` 是否会导致双重关闭？

**不会**，原因有两层保护。

**第一层保护：`forceClose()` 的状态检查**

```cpp
void TcpConnectionImpl::forceClose()
{
    auto thisPtr = shared_from_this();
    loop_->runInLoop([thisPtr]() {
        if (thisPtr->status_ == ConnStatus::Connected ||
            thisPtr->status_ == ConnStatus::Disconnecting)
        {
            thisPtr->status_ = ConnStatus::Disconnecting;
            thisPtr->handleClose();
            // ...
        }
    });
}
```

如果连接已经在正常关闭流程中走过了 `handleClose()`，`status_` 已经变为 `Disconnected`。`forceClose()` 的 lambda 在 IO 线程执行时检查到 `status_ == Disconnected`，if 条件不满足，直接跳过，什么也不做。

**第二层保护：`weak_ptr` 可能已经过期**

`KickoffEntry` 持有的是 `weak_ptr<TcpConnection>`。如果连接已经完全关闭并析构（`TcpServer` 的 `connSet_` 已经 erase，`connectDestroyed()` 已经执行，所有 `shared_ptr` 都已释放），那么 `conn_.lock()` 返回空指针，`~KickoffEntry()` 中的 `if (conn)` 判断为 false，不会调用 `forceClose()`。

不过需要注意一个细微情况：**连接可能正处于关闭流程中，`shared_ptr` 还没完全释放**。比如 `handleClose()` 已经将 `status_` 置为 `Disconnected`，但 `connectDestroyed()` 还在 `queueInLoop` 队列里等待执行，此时 `connSet_` 中还持有 `shared_ptr`。这种情况下 `weak_ptr::lock()` 成功，但 `forceClose()` 的 lambda 进入 IO 线程后发现 `status_ == Disconnected`，不做任何操作——依靠第一层保护。

**总结**：`forceClose()` 是**幂等**的（多次调用等价于一次调用），因为它内部有状态检查守卫。`KickoffEntry` 析构触发的 `forceClose()` 在"连接已关闭"的情况下是一个安全的空操作（no-op）。这是 trantor 中很多关闭操作的通用模式——**状态机保证幂等性，`runInLoop` 保证线程安全性**。

---

*学习日期：2026-04-20 | 上一课：[第10课_Acceptor与Connector](第10课_Acceptor与Connector.md) | 下一课：[第12课_TcpServer与TcpClient](第12课_TcpServer与TcpClient.md)*
