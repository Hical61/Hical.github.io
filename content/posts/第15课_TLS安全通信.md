+++
date = '2026-05-01'
draft = false
title = 'TLS 安全通信'
categories = ["网络编程"]
tags = ["C++", "trantor", "TLS", "安全通信", "学习笔记"]
description = "trantor TLS 安全通信解析，OpenSSL 集成与加密连接的建立流程。"
+++


# 第 15 课：TLS 安全通信

> 对应源文件：
> - `trantor/net/TLSPolicy.h` — TLS 策略配置（证书、验证规则、ALPN 等）
> - `trantor/net/Certificate.h` — 证书抽象接口
> - `trantor/net/inner/TLSProvider.h` — TLS 提供者抽象基类（策略模式）
> - 具体实现（未深入）：`OpenSSLProvider.cc`（OpenSSL 后端）、`BotanTLSProvider.cc`（Botan 后端）

---

## 一、TLS 在 trantor 中的架构

trantor 的 TLS 是完全**透明的**——插入到 `TcpConnectionImpl` 和用户代码之间，用户几乎感知不到加密的存在：

```
用户代码（send/recvMsgCallback）
        │                ▲
        │ 明文数据        │ 解密后的明文
        ▼                │
┌───────────────────────────┐
│       TLSProvider          │  ← 透明加密/解密层
│  startEncryption()         │
│  sendData(明文) → 密文     │
│  recvData(密文) → 明文     │
└───────────────────────────┘
        │                ▲
        │ TLS 密文        │ 从 socket 读到的密文
        ▼                │
  TcpConnectionImpl（writeRaw / readBuffer_）
        │
        ▼
   TCP socket（内核）
```

### 策略模式（Strategy Pattern）

`TLSProvider` 是一个**纯虚接口**，具体的 TLS 实现（OpenSSL、Botan）是策略类：

```
TLSProvider（抽象策略）
    │
    ├── OpenSSLProvider（OpenSSL 实现）
    └── BotanTLSProvider（Botan 实现）
```

工厂函数 `newTLSProvider(conn, policy, ctx)` 根据编译时选项返回对应的实现。上层代码（`TcpConnectionImpl`）完全不知道底层用的哪个 SSL 库。

---

## 二、TLSPolicy — 配置中心

### 2.1 数据字段

```cpp
struct TLSPolicy final {
    std::string certPath_   = "";    // 证书文件路径（PEM 格式）
    std::string keyPath_    = "";    // 私钥文件路径（PEM 格式）
    std::string caPath_     = "";    // CA 证书路径（用于验证对端）
    std::string hostname_   = "";    // 用于 SNI 和证书域名验证
    std::vector<std::string> alpnProtocols_ = {};  // ALPN 协议列表
    bool useOldTLS_         = false; // 是否允许 TLS 1.0/1.1（不推荐）
    bool validate_          = true;  // 是否验证对端证书
    bool allowBrokenChain_  = false; // 允许不完整证书链（自签名证书）
    bool useSystemCertStore_= true;  // 使用系统证书库
    std::vector<std::pair<std::string, std::string>> sslConfCmds_ = {}; // OpenSSL 专用命令
};
```

### 2.2 流式 Builder 接口

所有 setter 都返回 `TLSPolicy &`，支持链式调用：

```cpp
auto policy = TLSPolicy::defaultServerPolicy("server.crt", "server.key");
policy->setUseOldTLS(false)
      .setAlpnProtocols({"http/1.1", "h2"})
      .setCaPath("/etc/ssl/ca.crt")
      .setValidate(true);
```

### 2.3 两个工厂方法

```cpp
// 服务端默认策略：不验证客户端证书，不使用旧协议
static TLSPolicyPtr defaultServerPolicy(
    const std::string &certPath, const std::string &keyPath) {
    auto policy = std::make_shared<TLSPolicy>();
    policy->setValidate(false)           // 服务端通常不验证客户端证书
          .setUseOldTLS(false)
          .setUseSystemCertStore(false)
          .setCertPath(certPath)
          .setKeyPath(keyPath);
    return policy;
}

// 客户端默认策略：验证服务端证书，使用系统证书库
static TLSPolicyPtr defaultClientPolicy(
    const std::string &hostname = "") {
    auto policy = std::make_shared<TLSPolicy>();
    policy->setValidate(true)            // 客户端必须验证服务端证书
          .setUseOldTLS(false)
          .setUseSystemCertStore(true)   // 用系统信任的 CA 列表
          .setHostname(hostname);        // 用于 SNI 和证书域名匹配
    return policy;
}
```

**服务端和客户端默认策略的差异**：

| 设置                 | 服务端默认   | 客户端默认 | 原因                                      |
| -------------------- | ------------ | ---------- | ----------------------------------------- |
| `validate`           | false        | **true**   | 客户端必须验证服务端身份，防止中间人攻击  |
| `useSystemCertStore` | false        | **true**   | 客户端用系统信任的 CA；服务端有自己的证书 |
| `certPath/keyPath`   | **必须提供** | 可选       | 服务端必须有证书；客户端双向认证时才需要  |

---

## 三、Certificate — 证书接口

```cpp
struct Certificate {
    virtual ~Certificate() = default;
    virtual std::string sha1Fingerprint() const = 0;    // SHA1 指纹
    virtual std::string sha256Fingerprint() const = 0;  // SHA256 指纹（推荐）
    virtual std::string pem() const = 0;                // PEM 格式证书文本
};
using CertificatePtr = std::shared_ptr<Certificate>;
```

极简的三方法接口。通过 `conn->peerCertificate()` 获取对端证书，可以：
- 验证证书指纹（双向认证时校验客户端身份）
- 导出 PEM 格式存档（审计日志）
- 做自定义的证书校验逻辑

---

## 四、TLSProvider — 核心抽象

### 4.1 纯虚方法（子类必须实现）

```cpp
struct TLSProvider {
    // 处理从 TCP 收到的密文（可能包含握手数据或应用数据）
    virtual void recvData(MsgBuffer *buffer) = 0;

    // 加密并发送明文数据
    virtual ssize_t sendData(const char *ptr, size_t size) = 0;

    // 发送 TLS close_notify alert（优雅关闭）
    virtual void close() = 0;

    // 启动 TLS 握手
    virtual void startEncryption() = 0;
};
```

### 4.2 回调（静态函数指针，非 std::function）

```cpp
using WriteCallback     = ssize_t (*)(TcpConnection*, const void*, size_t);
using ErrorCallback     = void (*)(TcpConnection*, SSLError);
using HandshakeCallback = void (*)(TcpConnection*);
using MessageCallback   = void (*)(TcpConnection*, MsgBuffer*);
using CloseCallback     = void (*)(TcpConnection*);
```

**为什么用裸函数指针而不是 `std::function`？**

这里的注释写得很清楚：`std::function used due to performance reasons`（实际上是用函数指针，注释说明是出于性能考虑）。

- 函数指针调用：直接跳转，无堆分配，无虚函数开销
- `std::function`：可能有堆分配（小对象优化失效时）

TLS 数据收发路径是**热路径**——每次收发数据都会经过这些回调，选用函数指针减少延迟。

### 4.3 内置发送缓冲区 `writeBuffer_`

```cpp
MsgBuffer writeBuffer_;  // TLS 层的待发送缓冲区

bool sendBufferedData() {
    if (writeBuffer_.readableBytes() == 0) return true;
    auto n = writeCallback_(conn_,
                            writeBuffer_.peek(),
                            writeBuffer_.readableBytes());
    if (n == -1) return false;  // 错误
    if ((size_t)n != writeBuffer_.readableBytes()) {
        writeBuffer_.retrieve(n);
        return false;  // 未发完（EAGAIN），等下次
    }
    writeBuffer_.retrieveAll();
    return true;
}
```

**为什么 TLS 层需要自己的发送缓冲区？**

TLS 加密不是简单的字节替换——加密后的数据长度会变（padding、MAC、握手报文），且握手期间 TLS 内部可能需要发送 `ServerHello`、`Certificate` 等控制报文，这些都不走用户的发送队列。`writeBuffer_` 是 TLS 层的内部缓冲，用于暂存这些待发的密文。

`writeCallback_` 指向 `TcpConnectionImpl::writeRaw()`，最终调用 `write(socketFd, ...)` 把密文写入内核。

### 4.4 两个缓冲区

```cpp
MsgBuffer recvBuffer_;   // 解密后的明文（交给 recvMsgCallback_）
MsgBuffer writeBuffer_;  // 待发送的密文（来自 TLS 加密结果）
```

完整的数据流：

```
接收方向：
socket → readBuffer_（TcpConnectionImpl）→ recvData() → 解密 → recvBuffer_ → messageCallback_

发送方向：
sendData(明文) → 加密 → appendToWriteBuffer(密文) → writeBuffer_ → writeCallback_ → write(socket)
```

---

## 五、TLS 握手流程

### 5.1 服务端握手

```
[客户端发起连接]
        │
        ▼
TcpConnectionImpl 构造（携带 TLSPolicy 和 SSLContext）
        │
        ▼
connectEstablished() 中：
  tlsProviderPtr_->startEncryption()   ← 服务端开始等待 ClientHello
        │
   [客户端发 ClientHello]
        │
        ▼
readCallback() → readBuffer_.readFd()
tlsProviderPtr_->recvData(&readBuffer_)  ← 处理 ClientHello
  │ TLS 内部发送 ServerHello + Certificate + ServerHelloDone
  │ (通过 writeCallback_ → writeRaw → socket)
        │
   [握手数据往返若干轮]
        │
        ▼
握手完成 → handshakeCallback_(conn_)   ← onHandshakeFinished
  → connectionCallback_(conn)          ← 通知用户"连接建立"
        │
   [正式数据传输]
        │
        ▼
recvData → 解密 → messageCallback_ → recvMsgCallback_(conn, &recvBuffer_)
sendData → 加密 → writeRaw → socket
```

### 5.2 startEncryption（运行时升级）

`TcpConnectionImpl::startEncryption()` 允许在已建立的 TCP 连接上**动态升级为 TLS**：

```cpp
void TcpConnectionImpl::startEncryption(
    TLSPolicyPtr policy, bool isServer,
    std::function<void(const TcpConnectionPtr &)> upgradeCallback)
{
    if (tlsProviderPtr_ || upgradeCallback_) {
        LOG_ERROR << "TLS is already started";
        return;
    }
    auto sslContextPtr = newSSLContext(*policy, isServer);
    tlsProviderPtr_ = newTLSProvider(this, std::move(policy), sslContextPtr);
    // 绑定回调...
    tlsProviderPtr_->startEncryption();
    upgradeCallback_ = std::move(upgradeCallback);
}
```

**典型使用场景**：STARTTLS 协议（如 SMTP/FTP），先用明文建立连接，协商后再升级为加密。

---

## 六、SSLContext vs TLSPolicy

这两个概念容易混淆：

|          | TLSPolicy                                             | SSLContext                                         |
| -------- | ----------------------------------------------------- | -------------------------------------------------- |
| 本质     | 配置参数（证书路径、验证选项等）                      | 已初始化的 SSL 上下文对象（OpenSSL 的 `SSL_CTX*`） |
| 生命周期 | 可复用，跨连接共享                                    | 已载入证书、设置完选项的重量级对象                 |
| 创建方式 | 直接构造                                              | `newSSLContext(policy, isServer)`                  |
| 共享方式 | `TcpServer` 持有一份，所有新连接共用同一个 SSLContext |

```cpp
// TcpServer 中：
policyPtr_ = TLSPolicy::defaultServerPolicy(certPath, keyPath);
sslContextPtr_ = newSSLContext(*policyPtr_, true);  // 一次性初始化

// 每次新连接时：
auto conn = make_shared<TcpConnectionImpl>(loop, fd, local, peer,
    policyPtr_, sslContextPtr_);   // 共享 SSLContext，不重新初始化
```

SSLContext 创建成本很高（需要加载证书、初始化随机数等），所以被所有连接共享。每个 `TLSProvider` 实例（每个连接一个）从共享的 SSLContext 创建独立的 `SSL*` 对象（OpenSSL 术语），拥有各自的握手状态和会话密钥。

---

## 七、完整的 TLS 数据流

以发送"Hello"为例：

```
用户代码: conn->send("Hello", 5)
        │
        ▼
TcpConnectionImpl::sendInLoop("Hello", 5)
        │
        ▼
writeInLoop("Hello", 5)
    → tlsProviderPtr_->sendData("Hello", 5)
        │  OpenSSL: SSL_write("Hello", 5) → 加密
        │  → 加密后密文 X（可能比 5 字节长）
        ▼
appendToWriteBuffer(密文X)    ← 存入 TLSProvider::writeBuffer_
        │
        ▼
writeCallback_(conn_, writeBuffer_.peek(), writeBuffer_.readableBytes())
    → TcpConnectionImpl::writeRaw(密文X, len)
        │
        ▼
write(socketFd, 密文X, len)  ← 真正写入 TCP socket
```

以接收数据为例：

```
socket 收到数据（TLS 密文）
        │
        ▼
TcpConnectionImpl::readCallback()
    readBuffer_.readFd(fd)   ← 把密文读入 readBuffer_
        │
        ▼
tlsProviderPtr_->recvData(&readBuffer_)
    → OpenSSL: SSL_read(readBuffer_) → 解密
    → 解密成功 → 填入 recvBuffer_
    → messageCallback_(conn_, &recvBuffer_)
        │
        ▼
TcpConnectionImpl::onSslMessage(conn_, &recvBuffer_)
    → recvMsgCallback_(shared_from_this(), &recvBuffer_)
        │
        ▼
用户代码: [recvMsgCallback 执行]
```

---

## 八、热重载证书

`TcpServer::reloadSSL()` 允许不重启服务器地更新证书：

```cpp
void TcpServer::reloadSSL() {
    loop_->queueInLoop([this]() {
        if (policyPtr_)
            sslContextPtr_ = newSSLContext(*policyPtr_, true);
        // 重新从 policyPtr_ 读取证书路径，加载新证书
    });
}
```

**工作原理**：
- `policyPtr_` 记录了证书文件路径（不变）
- 重新调用 `newSSLContext` 重新加载磁盘上的证书文件
- 新的 `sslContextPtr_` 替换旧的
- **已有连接**：继续使用旧的 SSL 对象（旧证书），不受影响
- **新连接**：`newConnection()` 中使用新的 `sslContextPtr_`，使用新证书

---

## 九、ALPN 协议协商

ALPN（Application-Layer Protocol Negotiation）是 TLS 的一个扩展，允许在握手期间协商应用层协议（如 HTTP/1.1 vs HTTP/2）：

```cpp
// Drogon HTTP 服务器的 ALPN 配置
auto policy = TLSPolicy::defaultServerPolicy(certPath, keyPath);
policy->setAlpnProtocols({"h2", "http/1.1"});  // 优先 HTTP/2

// 握手完成后，查询协商结果
conn->applicationProtocol()  // 返回 "h2" 或 "http/1.1"
```

trantor 把协商到的协议存在 `TLSProvider::applicationProtocol_` 里，上层通过 `TcpConnection::applicationProtocol()` 获取。

---

## 十、双向认证（mTLS）

普通 TLS：只有客户端验证服务端证书。
双向 TLS（mTLS）：服务端也验证客户端证书，常用于服务间通信。

```cpp
// 服务端：要求客户端提供证书
auto serverPolicy = TLSPolicy::defaultServerPolicy("server.crt", "server.key");
serverPolicy->setValidate(true)       // 验证客户端证书
            .setCaPath("/etc/ssl/client-ca.crt");   // 信任的客户端 CA

// 验证客户端身份（在 connectionCallback 里）
server.setConnectionCallback([](const TcpConnectionPtr &conn) {
    if (conn->connected()) {
        auto cert = conn->peerCertificate();
        if (!cert) {
            LOG_WARN << "客户端未提供证书，拒绝";
            conn->forceClose();
            return;
        }
        LOG_INFO << "客户端证书指纹: " << cert->sha256Fingerprint();
        // 验证指纹是否在白名单中
        if (!isAllowedCert(cert->sha256Fingerprint())) {
            conn->forceClose();
        }
    }
});
```

---

## 十一、游戏服务器实践

### 11.1 游戏客户端与网关的 TLS

```cpp
// 网关服务器（服务端）
TcpServer gateway(&loop, InetAddress(9443), "Gateway");
auto policy = TLSPolicy::defaultServerPolicy(
    "/etc/game/ssl/gateway.crt",
    "/etc/game/ssl/gateway.key");
policy->setAlpnProtocols({"game-protocol-v1"});
gateway.enableSSL(policy);

// 游戏客户端（客户端）
TcpClient client(&loop, serverAddr, "GameClient");
auto clientPolicy = TLSPolicy::defaultClientPolicy("game.example.com");
clientPolicy->setAlpnProtocols({"game-protocol-v1"});
client.enableSSL(clientPolicy);
```

### 11.2 服务器间内部通信（可以用自签名证书）

```cpp
// 内部服务器间通信：允许自签名证书（不走公共 CA）
auto internalPolicy = TLSPolicy::defaultClientPolicy("internal-logic-server");
internalPolicy->setValidate(true)
              .setAllowBrokenChain(true)   // 允许自签名（不完整链）
              .setCaPath("/etc/game/ssl/internal-ca.crt");  // 内部 CA
client.enableSSL(internalPolicy);
```

### 11.3 证书轮换（不停服）

```cpp
// 证书过期前更新证书文件，然后调用：
server.reloadSSL();   // 新连接使用新证书，已有连接不受影响
LOG_INFO << "证书已热重载，新连接将使用新证书";
```

---

## 十二、思考题

1. `TLSProvider` 的回调使用裸函数指针（`ssize_t (*)(TcpConnection*, const void*, size_t)`），而不是 `std::function`。这意味着**不能捕获变量的 Lambda** 作为回调。trantor 如何解决"需要访问 `TcpConnectionImpl` 成员"的需求？（提示：看 `onSslWrite` 的实现）

2. `sendBufferedData()` 返回 `false` 表示"还有数据没发完"，此时 `TcpConnectionImpl::writeCallback()` 直接 `return`。如果这个状态持续很久（网络拥塞），而用户又继续调用 `conn->send()`，`writeBufferList_` 会持续增长。哪个机制会限制这种无界增长？

3. `TLSProvider::recvBuffer_` 存放解密后的明文，这个 buffer 和 `TcpConnectionImpl::readBuffer_`（存放密文）是不同的对象。`TcpConnection::getRecvBuffer()` 根据是否有 TLS 返回不同的 buffer。为什么不能把解密后的数据直接写回 `readBuffer_` 而要用独立的 `recvBuffer_`？

4. 热重载证书（`reloadSSL()`）后，假设某个连接的 TLS 会话恰好在切换点（旧 `sslContextPtr_` 被替换，新的 `sslContextPtr_` 还没被任何连接使用），新连接用新证书，旧连接用旧证书的 SSL 对象。当旧连接最终断开时，旧的 `SSLContext` 还在内存中吗？如何确保它不会被提前释放？（提示：`shared_ptr` 引用计数）

---

*学习日期：2026-04-02 | 上一课：[第14课_任务队列](第14课_任务队列.md) | 下一课：[第16课_DNS解析](第16课_DNS解析.md)*

---

## 核心收获

- `TLSProvider` 策略模式：OpenSSL / Botan 后端可互换，`newTLSProvider()` 工厂在编译期选择实现
- 热路径用**原始函数指针**替代 `std::function`：避免虚函数 vtable 查找 + lambda 堆分配开销
- `TLSPolicy` Builder 链式配置：`defaultServerPolicy()`（不验客户端）vs `defaultClientPolicy()`（验证 + 系统证书）
- TLS 双缓冲：`recvBuffer_`（已解密明文）+ `writeBuffer_`（待发送密文），加密层对 TcpConnection 完全透明
- 热重载 `reloadSSL()`：旧连接继续用旧 SSLContext（`shared_ptr` 保活），新连接用新证书，不中断服务
