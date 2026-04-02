+++
date = '2026-04-15'
draft = false
title = '网络地址与 Socket 封装'
categories = ["网络编程"]
tags = ["C++", "trantor", "Socket", "网络地址", "学习笔记"]
description = "trantor 网络地址（InetAddress）与 Socket 封装解析，跨平台网络原语抽象。"
+++


# 第 9 课：网络地址与 Socket 封装

> 对应源文件：
> - `trantor/net/InetAddress.h` / `InetAddress.cc` — IPv4/IPv6 地址封装
> - `trantor/net/inner/Socket.h` / `Socket.cc` — 跨平台 Socket RAII 封装

---

## 一、两个类在架构中的位置

```
TcpServer / TcpClient
        │
        ▼
  Acceptor / Connector
        │
        ├─ InetAddress ← 描述"连谁/绑哪里"
        └─ Socket      ← 持有实际的系统 fd，负责创建/配置/关闭
```

这两个类是"最底层的 C++ 包装"：
- **InetAddress**：把 `struct sockaddr_in/in6` 包成一个类型安全的 C++ 对象
- **Socket**：RAII 管理 socket fd，把 `setsockopt/bind/listen/accept` 包成成员函数

---

## 二、InetAddress — 双协议地址封装

### 2.1 核心存储

```cpp
// InetAddress.h（精简）
union {
    struct sockaddr_in  addr_;   // IPv4：16 字节
    struct sockaddr_in6 addr6_;  // IPv6：28 字节
};
bool isIpV6_;       // 区分当前存的是哪种
bool isUnspecified_; // 是否是"未指定地址"（0.0.0.0 / ::）
```

**为什么用 union？**

IPv4 地址结构 16 字节，IPv6 地址结构 28 字节。用 union 可以：
1. 统一存储，不浪费空间
2. `getSockAddr()` 永远返回 `addr6_` 的指针——因为 IPv4 结构是 IPv6 结构的子集（前 16 字节对齐），强制转换合法
3. 不需要虚函数，零开销

```cpp
// 永远从 addr6_ 取地址（union 内存对齐保证安全）
const struct sockaddr *InetAddress::getSockAddr() const
{
    return static_cast<const struct sockaddr *>(
        static_cast<const void *>(&addr6_));
}
```

### 2.2 构造方式

```cpp
// 1. 只指定端口（绑定到所有接口）
InetAddress(uint16_t port = 0,
            bool loopbackOnly = false,   // true = 127.0.0.1
            bool ipv6 = false);          // true = ::1 / ::

// 2. 指定 IP 字符串 + 端口
InetAddress(const std::string &ip, uint16_t port, bool ipv6 = false);

// 3. 从原始结构体构造（内核 accept() 返回时使用）
explicit InetAddress(const struct sockaddr_in &addr);
explicit InetAddress(const struct sockaddr_in6 &addr6);
```

**IP 字符串 → 二进制**：使用 `inet_pton`（POSIX/WinSock2 均支持），支持 IPv4 点分十进制和 IPv6 冒号十六进制。

### 2.3 `toIp()` — 快速 IPv4 字符串化

```cpp
std::string InetAddress::toIp() const
{
    if (isIpV6_) {
        char buf[64];
        ::inet_ntop(AF_INET6, &addr6_.sin6_addr, buf, sizeof(buf));
        return buf;
    }
    // IPv4 走特化快速路径 iptos()
    return iptos(addr_.sin_addr.s_addr);
}
```

**`iptos()` 原理**：把 4 字节 IPv4 地址直接手写拆成十进制字符，避免 `inet_ntoa` 的 `sprintf` 格式化开销。在高频打印连接日志时有实际收益。

### 2.4 `isIntranetIp()` — 内网地址检测

```cpp
bool InetAddress::isIntranetIp() const
{
    if (isIpV6_) {
        // fe80::/10（链路本地）、fc00::/7（唯一本地）、::1（loopback）
        const auto *addr6 = &addr6_.sin6_addr;
        // 检查前缀...
        return isLinkLocal || isUniqueLocal || isLoopback;
    }

    // IPv4：RFC 1918 私有地址段
    uint32_t ip = ntohl(addr_.sin_addr.s_addr);
    return
        (ip >> 24 == 10) ||                            // 10.0.0.0/8
        (ip >> 20 == (172 << 4 | 1)) ||               // 172.16.0.0/12
        (ip >> 16 == (192 << 8 | 168)) ||             // 192.168.0.0/16
        (ip >> 24 == 127);                             // 127.0.0.0/8（loopback）
}
```

**位运算技巧**：`ip >> 24 == 10` 等价于"最高字节为 10"，一次整数比较比 `sscanf` 快得多。

**使用场景**：游戏服务器常需要区分内网/外网连接，内网连接可以跳过某些安全检查（如速率限制），或优先路由到内网专用端口。

### 2.5 `toIpPort()` — 完整地址字符串

```cpp
// 返回 "192.168.1.1:8080" 或 "[::1]:8080"（IPv6 需加方括号）
std::string InetAddress::toIpPort() const
{
    std::string str = toIp();
    if (isIpV6_) str = "[" + str + "]";
    return str + ":" + std::to_string(port());
}
```

---

## 三、Socket — RAII fd 管理

### 3.1 RAII 所有权

```cpp
class Socket : public NonCopyable
{
  public:
    explicit Socket(int sockfd) : sockFd_(sockfd) {}
    ~Socket() {
#ifndef _WIN32
        close(sockFd_);
#else
        closesocket(sockFd_);
#endif
    }
    int fd() const { return sockFd_; }
  private:
    int sockFd_;
};
```

**NonCopyable 继承**：Socket 不可拷贝（fd 不能共享所有权），但不需要移动（通常直接用 `unique_ptr<Socket>`）。

### 3.2 `createNonblockingSocketOrDie()` — 创建非阻塞 Socket

```cpp
// Linux：一次系统调用搞定，原子操作
int fd = ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);

// 其他平台：两步走
int fd = ::socket(domain, SOCK_STREAM, IPPROTO_TCP);
setNonBlockAndCloseOnExec(fd);  // fcntl(fd, F_SETFL, O_NONBLOCK | O_CLOEXEC)
```

| 标志            | 作用                                                    |
| --------------- | ------------------------------------------------------- |
| `SOCK_NONBLOCK` | connect/read/write 不阻塞，立即返回 EAGAIN              |
| `SOCK_CLOEXEC`  | `fork+exec` 后子进程自动关闭该 fd，防止 fd 泄漏到子进程 |

**为什么"die"（失败直接终止）？**

Socket 创建失败通常是系统资源极度匮乏（fd 耗尽），此时大概率无法正常运行，直接终止比假装能运行更安全。

### 3.3 `accept()` — 跨平台 accept

```cpp
// Linux：accept4() 一步设置 NONBLOCK | CLOEXEC
int connFd = ::accept4(sockFd_, addr, &len,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);

// 其他平台：accept() + 手动 fcntl
int connFd = ::accept(sockFd_, addr, &len);
if (connFd >= 0)
    setNonBlockAndCloseOnExec(connFd);
```

**Linux `accept4` 的优势**：
1. 减少一次系统调用（只调用一次而非两次）
2. 在多线程服务器中，`accept()` 后、`fcntl()` 前的窗口期间如果 `fork()`，子进程会意外继承 fd，`accept4` 消除此竞争窗口

### 3.4 socket 选项配置

```cpp
// TCP_NODELAY：禁用 Nagle 算法
// Nagle 算法：小包延迟发送（攒满 MSS 再发），游戏服务器通常要关掉
void Socket::setTcpNoDelay(bool on)
{
    int optval = on ? 1 : 0;
    ::setsockopt(sockFd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

// SO_REUSEADDR：允许绑定 TIME_WAIT 状态的端口
// 服务器重启后立刻能绑定同一端口，不用等 2MSL
void Socket::setReuseAddr(bool on)
{
    ::setsockopt(sockFd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

// SO_REUSEPORT：多个 socket 可以绑定同一端口（内核负载均衡）
// 多线程服务器每个线程开一个 acceptor，无锁竞争
void Socket::setReusePort(bool on)
{
    ::setsockopt(sockFd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

// SO_KEEPALIVE：TCP 保活
// 空闲连接定期发送探测包，检测"死连接"
void Socket::setKeepAlive(bool on)
{
    ::setsockopt(sockFd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}
```

**游戏服务器配置建议**：

| 选项         | 推荐   | 原因                                     |
| ------------ | ------ | ---------------------------------------- |
| TCP_NODELAY  | **开** | 消除指令延迟（玩家操作要立刻发出去）     |
| SO_REUSEADDR | **开** | 服务器可快速重启，不用等端口释放         |
| SO_REUSEPORT | 视情况 | 多线程 acceptor 时开，提升 accept 吞吐量 |
| SO_KEEPALIVE | **开** | 检测断线玩家（网络突然断开不会发 FIN）   |

### 3.5 `closeWrite()` — TCP 半关闭

```cpp
void Socket::closeWrite()
{
#ifndef _WIN32
    ::shutdown(sockFd_, SHUT_WR);   // 只关闭写方向
#else
    ::shutdown(sockFd_, SD_SEND);
#endif
}
```

**半关闭的语义**：
- 发送 FIN 给对端（告诉对端：我不再发数据了）
- 但还能**继续接收**对端的数据
- 用于优雅关闭：先把所有数据发完，再关写端，等对端也关写端，连接才真正结束

### 3.6 `isSelfConnect()` — 自连接检测

```cpp
bool Socket::isSelfConnect()
{
    // 比较本地地址和对端地址是否相同
    struct sockaddr_in6 localAddr = getLocalAddr(sockFd_);
    struct sockaddr_in6 peerAddr  = getPeerAddr(sockFd_);
    // 如果 IP + 端口完全相同 → 自连接
    return memcmp(&localAddr, &peerAddr, sizeof(localAddr)) == 0;
}
```

**为什么需要检测自连接？**

在 `TcpClient` 发起连接时，如果服务器不存在，操作系统可能会把 `connect()` 的目标地址分配给本机（尤其是连接到 `127.0.0.1:xxxx` 时），导致客户端连上了自己。自连接会造成数据循环，必须检测并断开。

---

## 四、两个类协作的完整链路

```
[TcpServer 启动]
        │
        ▼
InetAddress listenAddr(8080)        ← ① 创建监听地址
        │
        ▼
Socket serverSocket(                ← ② 创建非阻塞 socket
    createNonblockingSocketOrDie(AF_INET))
        │
serverSocket.setReuseAddr(true)     ← ③ 配置 socket 选项
serverSocket.setReusePort(true)
serverSocket.setTcpNoDelay(true)
        │
serverSocket.bindAddress(listenAddr)← ④ 绑定地址
        │
serverSocket.listen()               ← ⑤ 开始监听
        │
   [客户端连入]
        │
        ▼
InetAddress peerAddr                ← ⑥ 存储客户端地址
int connFd = serverSocket.accept(&peerAddr)  ← ⑦ accept（Linux 用 accept4）
        │
        ▼
TcpConnectionImpl(loop, connFd, localAddr, peerAddr)
        ← ⑧ 把 fd 交给 TcpConnection 管理
```

---

## 五、跨平台处理总结

| 操作                 | Linux                         | Windows           | macOS/BSD        |
| -------------------- | ----------------------------- | ----------------- | ---------------- |
| 创建非阻塞 socket    | `SOCK_NONBLOCK\|SOCK_CLOEXEC` | `fcntl` 模拟      | `fcntl`          |
| accept 并设 NONBLOCK | `accept4()`                   | `accept()+fcntl`  | `accept()+fcntl` |
| 关闭写端             | `SHUT_WR`                     | `SD_SEND`         | `SHUT_WR`        |
| 关闭 fd              | `close()`                     | `closesocket()`   | `close()`        |
| 地址字符串化         | `inet_ntop`                   | `inet_ntop` (WS2) | `inet_ntop`      |

所有差异都被封装在 `Socket.cc` 的 `#ifdef` 里，上层代码完全感知不到平台差异。

---

## 六、设计亮点

### 6.1 InetAddress 的无继承双协议设计

没有用虚函数多态（`IPv4Address extends InetAddress`），而是用 **union + isIpV6_ 标志**。

优点：
- 对象可以**值语义传递**（不需要 `shared_ptr`，栈上分配）
- 没有虚函数调用开销
- `sizeof(InetAddress)` = 28（sockaddr_in6 大小）+ 2 bool = 30 字节（对齐后 32），非常紧凑

### 6.2 Socket 的极简 RAII

Socket 只做一件事：**保证 fd 被关闭**。不持有 EventLoop 指针，不知道有没有 Channel，职责极单一。

上层的 `Acceptor` 和 `TcpConnectionImpl` 各自拥有一个 `Socket`（或者 `unique_ptr<Socket>`），fd 的所有权随对象生命周期自动管理。

### 6.3 "die" 语义的系统 fd 创建

如果 `socket()` 失败，立刻 `abort()`。这是一个**设计决策**，而非懒惰：
- socket 失败意味着系统资源耗尽
- 继续运行只会制造更难排查的问题
- 快速 crash 让 supervisor（如 systemd）快速重启，比僵尸运行更健康

---

## 七、完整调用链（游戏服务器场景）

```
[玩家客户端发起连接]
        │
        ▼
[OS 内核：TCP 三次握手完成，连接进入 accept 队列]
        │
        ▼
[Acceptor::handleRead()]
  peerAddr = InetAddress()           ← 临时存放对端地址
  int connFd = socket_.accept(&peerAddr)
  // socket_ 是 Socket(serverFd) 的 RAII 对象
  // accept() 内部用 accept4(NONBLOCK|CLOEXEC) on Linux
        │
        ▼
[newConnectionCallback_(connFd, peerAddr)]
  → TcpServer::newConnection(connFd, peerAddr)
        │
        ▼
[创建 TcpConnectionImpl]
  localAddr = InetAddress(getLocalAddr(connFd))
  TcpConnectionImpl conn(loop, connFd, localAddr, peerAddr)
  // conn 内部持有 Socket(connFd)，RAII 管理生命周期
        │
        ▼
[conn->setTcpNoDelay(true)]         ← 配置新连接的 socket 选项
[conn->connectEstablished()]        ← 触发 ConnectionCallback
        │
        ▼
[玩家会话开始]
```

---

## 八、思考题

1. `InetAddress::getSockAddr()` 永远返回 `addr6_` 的地址，在 IPv4 情况下 `addr_` 和 `addr6_` 内存上有什么关系？为什么这样转换是合法的？

2. `Socket::setReusePort()` 开启后，多个线程可以各自 `bind` 同一个端口并 `listen`，内核如何决定把新连接交给哪个 socket？这种设计有什么副作用？

3. `accept4()` 的 `SOCK_CLOEXEC` 标志保证了什么？如果不设这个标志，游戏服务器调用 `system()` 启动外部脚本时会发生什么？

4. `isIntranetIp()` 使用的 RFC1918 地址段检测，能正确处理 `10.0.0.0` 这个边界地址吗？IPv6 的 `fe80::` 地址（链路本地）为什么不适合用于跨路由器的服务器连接？

---

*学习日期：2026-04-01 | 上一课：[第08课_定时器系统](第08课_定时器系统.md) | 下一课：[第10课_Acceptor与Connector](第10课_Acceptor与Connector.md)*

---

## 核心收获

- `InetAddress` 用 `union { sockaddr_in, sockaddr_in6 }` 统一 IPv4/IPv6，`getSockAddr()` 始终返回 `addr6_` 指针（两结构体起始字段布局兼容）
- `isIntranetIp()` 用位移运算检测 RFC1918 私有地址段，比字符串解析快
- `Socket` RAII 封装：析构自动 `close(fd_)`，不会因异常导致 fd 泄漏
- Linux 原子创建非阻塞 socket：`SOCK_NONBLOCK | SOCK_CLOEXEC`，避免 `fcntl` 的竞态窗口
- `accept4()` 原子接受并设置 NONBLOCK（Linux）；其他平台 `accept()` + `fcntl` 两步
