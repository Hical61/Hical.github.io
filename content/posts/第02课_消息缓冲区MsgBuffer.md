+++
date = '2026-03-06'
draft = false
title = '消息缓冲区 MsgBuffer'
categories = ["网络编程"]
tags = ["C++", "trantor", "MsgBuffer", "缓冲区", "学习笔记"]
description = "trantor 消息缓冲区 MsgBuffer 设计解析，TCP 流式协议下的读写缓冲队列实现。"
+++


# 第 2 课：消息缓冲区 MsgBuffer

> 对应源文件：
> - `trantor/utils/MsgBuffer.h` / `MsgBuffer.cc` — 核心读写缓冲区
> - `trantor/net/inner/BufferNode.h` — 发送队列多态节点

---

## 一、为什么需要 MsgBuffer？

TCP 是**流式协议**，`recv()` 一次调用不一定能读完一个完整消息，也可能读到多个消息粘在一起（粘包）。

需要一个**弹性缓冲区**来：
1. 接收数据时：把内核 socket 缓冲区的数据读进来，等凑够一个完整包再交给上层
2. 发送数据时：把待发送数据先攒在缓冲区，等 socket 可写时分批发出
3. 支持在**头部预留空间**（prepend），方便后填消息长度字段

---

## 二、内存布局

### 2.1 物理结构

```
buffer_ (std::vector<char>，初始大小 = 2048 + 8)

索引：  0    8                    tail_             buffer_.size()
        │    │                      │                    │
        ▼    ▼                      ▼                    ▼
        [prepend区][─── 可读数据 ───][──── 可写空间 ────]
        └── 8字节 ┘
             ↑                      ↑
           head_                  tail_
```

- **prepend 区** `[0, head_)`：保留 8 字节，用于在数据头部插入字段（不需要移动数据）
- **可读区** `[head_, tail_)`：已接收但未被消费的数据，大小 = `tail_ - head_`
- **可写区** `[tail_, buffer_.size())`：空闲空间，大小 = `buffer_.size() - tail_`

### 2.2 关键常量与初始状态

```cpp
// MsgBuffer.cc 第 31 行
static constexpr size_t kBufferOffset{8};  // prepend 预留大小

// 构造函数（MsgBuffer.cc 第 34-37 行）
MsgBuffer::MsgBuffer(size_t len)
    : head_(kBufferOffset),    // head_ 从 8 开始
      initCap_(len),           // 记录初始容量，用于 retrieveAll 的缩容
      buffer_(len + head_),    // 总容量 = 用户要求 + 8字节 prepend
      tail_(head_)             // tail_ 也从 8 开始，初始可读字节为 0
```

初始状态（`len = 2048`）：
```
总大小: 2056 字节
head_ = tail_ = 8
可读字节: 0
可写字节: 2048
```

---

## 三、核心 API 详解

### 3.1 读数据的两个层次

```
peek()   ── 只看，不消费（不移动 head_）
read()   ── 读取并消费（移动 head_）
```

```cpp
// 正确的网络包解析流程
while (buf.readableBytes() >= sizeof(PacketHeader)) {
    // 1. 先 peek 解析包头，检查包是否完整
    auto header = *(PacketHeader*)buf.peek();
    uint32_t packetLen = buf.peekInt32();  // 自动做大端转换

    if (buf.readableBytes() < packetLen) break; // 包还没收全，等下一次

    // 2. 包完整了，才 retrieve 消费掉
    buf.retrieve(packetLen);
}
```

**为什么分 peek 和 retrieve 两步？**
因为 TCP 包可能分片到达，peek 看了头部发现包体还没收全，此时不能消费——下次数据到来时还需要从头重新解析。

### 3.2 整数读写（大端序自动转换）

```cpp
// 写入时自动转换为网络字节序（大端）
buf.appendInt32(0x12345678);   // 实际写入: 12 34 56 78

// 读取时自动转换回主机字节序
uint32_t val = buf.peekInt32();  // 内部调用 ntohl()
```

这是**网络编程的标准做法**：网络传输统一用大端序，发送时用 `hton*`，接收时用 `ntoh*`，对应 Linux 下的 `htons/htonl/ntohs/ntohl`。

### 3.3 `addInFront` — prepend 区的用途

经典网络协议场景：先填充包体，再在头部插入包长度。

```
场景：发送一个自定义协议包
  ┌───────┬─────────────────┐
  │ 4字节  │    包体数据      │
  │ 包长度 │  (先写入这部分)  │
  └───────┴─────────────────┘
```

```cpp
MsgBuffer buf;
// 1. 先写包体（可能很复杂）
buf.append(payloadData, payloadLen);

// 2. 再在头部插入包长度（利用 prepend 区，不需要移动 payloadData）
buf.addInFrontInt32(buf.readableBytes());
```

`addInFront` 实现的三级策略（`MsgBuffer.cc` 第 208-232 行）：

```
情况1: head_ >= len（prepend 区够用）
  → 直接写入 head_ - len 处，head_ -= len
  → O(1)，零拷贝！

情况2: head_ 不够，但 writableBytes() >= len
  → 把可读数据往后挪，腾出头部空间
  → O(n) memmove，但无需分配内存

情况3: 完全不够
  → 创建新 buffer，拷贝所有数据
  → O(n)，最慢
```

**图示情况 1（最优路径）**：
```
操作前：
[  8字节 prepend  ][  可读数据  ][  可写  ]
 ↑               ↑ head_=8
 0

addInFrontInt32(packetLen)：
[4字节][4字节][  可读数据  ][  可写  ]
       ↑ head_=4
写入包长度
```

### 3.4 `retrieve` / `retrieveAll` — 消费数据

```cpp
void MsgBuffer::retrieve(size_t len) {
    if (len >= readableBytes()) { retrieveAll(); return; }
    head_ += len;  // 只移动指针，O(1)，不移动数据
}

void MsgBuffer::retrieveAll() {
    // 如果 buffer 因扩容变得很大，此时趁机缩容回初始大小
    if (buffer_.size() > (initCap_ * 2)) {
        buffer_.resize(initCap_ + kBufferOffset);
    }
    tail_ = head_ = kBufferOffset;  // 重置指针
}
```

**自动缩容**：如果曾经发生过大数据传输使 buffer 扩容到很大，`retrieveAll` 会在清空时把 vector 缩回初始大小，防止内存长期占用。

---

## 四、readFd — 最关键的读取函数

### 4.1 问题

从 socket 读数据时，不知道本次到底有多少字节可读。如果 buffer 剩余空间不够，就会发生：
- 要么先 `ensureWritableBytes` 把 buffer 扩容（可能多余）
- 要么只读部分数据（需要再次调用 read）

### 4.2 trantor 的解法：`readv` + 栈上临时缓冲区

```cpp
// MsgBuffer.cc 第 148-173 行
ssize_t MsgBuffer::readFd(int fd, int *retErrno)
{
    char extBuffer[8192];   // 栈上 8 KB 临时缓冲区
    struct iovec vec[2];

    size_t writable = writableBytes();
    vec[0].iov_base = begin() + tail_;  // 第一块：buffer 的可写区
    vec[0].iov_len  = writable;
    vec[1].iov_base = extBuffer;         // 第二块：栈上临时区
    vec[1].iov_len  = sizeof(extBuffer);

    // 如果 buffer 可写空间 >= 8192，只用一块（不需要 extBuffer）
    const int iovcnt = (writable < sizeof extBuffer) ? 2 : 1;
    ssize_t n = ::readv(fd, vec, iovcnt);  // 一次系统调用读入两块

    if (n < 0) {
        *retErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        // 数据全在 buffer 里，直接移动 tail_
        tail_ += n;
    } else {
        // 数据溢出到了 extBuffer，把 extBuffer 的部分 append 进来
        tail_ = buffer_.size();
        append(extBuffer, n - writable);  // append 内部会 ensureWritableBytes
    }
    return n;
}
```

### 4.3 图示流程

```
socket 内核缓冲区有 10KB 数据，buffer 可写空间只有 3KB

readv 调用：
  vec[0] → buffer 可写区 3KB
  vec[1] → 栈上 extBuffer 8KB

读取结果 n = 10KB：
  vec[0] 填满：3KB → 直接 tail_ = buffer_.size()
  vec[1] 填充：7KB → append(extBuffer, 7KB) → 触发 ensureWritableBytes 扩容

最终：buffer 扩容，所有 10KB 数据都在 buffer 里
```

**核心优势**：
1. **只用一次 `readv` 系统调用**，无论数据多大都能全部读入
2. **栈上 extBuffer 零堆分配**——8KB 在栈上，不用 `new`
3. **buffer 不预先扩容**——按需扩容，不浪费内存

---

## 五、`ensureWritableBytes` — 智能扩容

```cpp
// MsgBuffer.cc 第 39-60 行
void MsgBuffer::ensureWritableBytes(size_t len)
{
    if (writableBytes() >= len) return;  // 够用，直接返回

    // 策略1：整理碎片（head_ 前面有空间 + 尾部可写 >= len）
    if (head_ + writableBytes() >= (len + kBufferOffset))
    {
        // 把可读数据向前移动到 kBufferOffset 处
        std::copy(begin() + head_, begin() + tail_, begin() + kBufferOffset);
        tail_ = kBufferOffset + (tail_ - head_);
        head_ = kBufferOffset;
        return;  // 不分配新内存！
    }

    // 策略2：真正扩容
    size_t newLen;
    if ((buffer_.size() * 2) > (kBufferOffset + readableBytes() + len))
        newLen = buffer_.size() * 2;  // 双倍扩容（避免频繁 realloc）
    else
        newLen = readableBytes() + len;  // 刚好够用
    MsgBuffer newbuffer(newLen);
    newbuffer.append(*this);
    swap(newbuffer);  // 交换指针，O(1)
}
```

**三级决策树**：

```
需要写入 len 字节
      │
      ├─ 尾部可写 >= len ──────────────► 直接写，什么都不做
      │
      ├─ 头部空闲 + 尾部可写 >= len+8 ── memmove 整理碎片，无需分配内存
      │    （数据消费后 head_ 往右移，头部积累了空闲空间）
      │
      └─ 真的不够 ─────────────────────► 创建新 buffer，2倍扩容
```

**为什么策略1能不分配内存？**

随着数据被消费（`retrieve`），`head_` 不断右移，头部积累大量空闲空间：
```
[  大量空闲  ][少量可读数据][小量可写]
 ↑            ↑head_      ↑tail_
```
把可读数据往左移到 offset=8 处，可写空间就够了。

---

## 六、BufferNode — 发送队列的多态节点

`MsgBuffer` 是通用读写缓冲区，但**发送端**需要支持多种数据来源：
- 内存数据（`std::string` / `char*`）
- 磁盘文件（零拷贝 `sendfile`）
- 流式回调（用户按需生成数据）
- 异步流（WebSocket 流式响应）

`BufferNode` 是这些数据源的**抽象基类**：

```cpp
// BufferNode.h 第 31-84 行
class BufferNode : public NonCopyable {
public:
    // 多态识别（默认 false，文件节点重写为 true）
    virtual bool isFile()   const { return false; }
    virtual bool isStream() const { return false; }
    virtual bool isAsync()  const { return false; }

    // 核心接口：获取当前可发送的数据指针和长度
    virtual void getData(const char *&data, size_t &len) = 0;

    // 标记已发送了 len 字节（移动内部读指针）
    virtual void retrieve(size_t len) = 0;

    // 还剩多少字节没发
    virtual long long remainingBytes() const = 0;

    // 仅内存节点支持
    virtual void append(const char *, size_t) { LOG_FATAL << "Not a memory buffer node"; }

    // 仅文件节点支持（用于 sendfile）
    virtual int getFd() const { LOG_FATAL << "Not a file buffer node"; return -1; }
};
```

### 四种子类

| 子类                    | 创建方法                                | 用途                             |
| ----------------------- | --------------------------------------- | -------------------------------- |
| `MemBufferNode`         | `newMemBufferNode()`                    | 内存数据，内部持有 `MsgBuffer`   |
| `FileBufferNodeUnix`    | `newFileBufferNode(path, offset, len)`  | Linux 文件，用 `sendfile` 零拷贝 |
| `FileBufferNodeWin`     | `newFileBufferNode(wpath, offset, len)` | Windows 文件                     |
| `AsyncStreamBufferNode` | `newAsyncStreamBufferNode()`            | 异步流（WebSocket 推送）         |

### 发送队列示意

```
TcpConnection 的发送队列：
┌──────────────────────────────────────────────────┐
│  std::list<BufferNodePtr> sendNodeList_           │
│                                                  │
│  [MemBufferNode]─►[FileBufferNode]─►[MemBufferNode]│
│   "HTTP 头部"        "静态文件内容"    "trailing data"│
└──────────────────────────────────────────────────┘
```

发送文件时的流程：
1. HTTP 响应头：`MemBufferNode`（内存数据）
2. 文件内容：`FileBufferNodeUnix`（`sendfile` 系统调用，内核直接传输，不经用户空间）
3. 节点发完后自动从队列移除

---

## 七、MsgBuffer vs LogStream FixedBuffer 对比

| 特性       | `MsgBuffer`         | `FixedBuffer`       |
| ---------- | ------------------- | ------------------- |
| 存储位置   | 堆（`std::vector`） | 栈（`char[SIZE]`）  |
| 大小       | 动态扩容            | 编译期固定          |
| 读写指针   | `head_` + `tail_`   | 只有 `cur_`（只写） |
| 用途       | TCP 收发缓冲        | 单条日志组装        |
| 头部预留   | 8字节 prepend 区    | 无                  |
| 字节序转换 | 有（`htons/ntohl`） | 无                  |
| 生命周期   | 随连接存在          | Logger 析构即销毁   |

---

## 八、完整使用示例（模拟游戏服务器解包）

```cpp
// 假设自定义协议：
// [4字节 包总长度][2字节 消息ID][N字节 消息体]

void onReceiveData(const TcpConnectionPtr &conn, MsgBuffer *buf)
{
    while (buf->readableBytes() >= 6)  // 最小包头 6 字节
    {
        // 1. peek 包长度（不消费）
        uint32_t packetLen = buf->peekInt32();

        // 2. 检查完整性
        if (buf->readableBytes() < packetLen) break;

        // 3. 读消息 ID
        buf->retrieve(4);  // 跳过包长度字段
        uint16_t msgId = buf->readInt16();  // 读并消费 2 字节

        // 4. 读消息体
        size_t bodyLen = packetLen - 6;
        std::string body = buf->read(bodyLen);

        // 5. 分发处理
        dispatchMessage(conn, msgId, body);
    }
}

// 构造响应包（在头部插入长度）
MsgBuffer buildResponse(uint16_t msgId, const std::string &body)
{
    MsgBuffer buf;
    buf.appendInt16(msgId);
    buf.append(body.c_str(), body.size());
    // 在头部插入总长度（利用 prepend 区）
    buf.addInFrontInt32(buf.readableBytes() + 4);
    return buf;
}
```

---

## 九、关键设计模式总结

| 设计                 | 体现                        | 好处                             |
| -------------------- | --------------------------- | -------------------------------- |
| **双指针滑动窗口**   | `head_` + `tail_`           | 消费数据 O(1)，不移动数据        |
| **prepend 区**       | 8 字节头部预留              | 填充包头无需 memmove             |
| **碎片整理优先扩容** | `ensureWritableBytes` 策略1 | 减少内存分配次数                 |
| **readv 双 iovec**   | `readFd` 用栈缓冲兜底       | 一次系统调用读完所有数据         |
| **自动缩容**         | `retrieveAll`               | 大数据传输后内存不长期占用       |
| **多态 BufferNode**  | 4 种节点类型                | 统一发送接口，支持零拷贝文件传输 |

---

## 核心收获

- 双指针 `_readIndex` / `_writeIndex`：中间是可读数据，右侧是可写空间，左侧 8 字节 prepend 区预留报头
- `prepend` 区的价值：插入包头无需移动数据，直接向左写即可（游戏协议常见操作）
- `readFd()` 用 `readv` + 栈上 65536 字节备用缓冲：单次 syscall 读完内核数据，再按需扩展 MsgBuffer
- `BufferNode` 4 种子类（内存/Unix文件/Win文件/异步流）支撑 `writeBufferList_` 的零拷贝发送

---

## 十、思考题

1. `retrieve` 只移动 `head_` 指针而不清除数据，有没有数据泄露风险？（提示：考虑 `peek` 后的旧数据）
2. `readFd` 中 `extBuffer` 是栈上 8192 字节，如果一次读到超过 `writable + 8192` 的数据会怎样？
3. `addInFront` 情况 2 为什么要向后移动数据而不是向前？（提示：看 head_ 和 writable 的关系）
4. 为什么 `appendInt16/32/64` 调用 `htons/htonl`，而 `appendInt8` 不调用？

---

## 十一、思考题参考答案

### 1. `retrieve` 只移动 `head_` 指针而不清除数据，有没有数据泄露风险？

**在正常使用下没有数据泄露风险**，但需要理解"为什么没有"。

首先看 `retrieve` 的实现（MsgBuffer.cc 第 131-139 行）：

```cpp
void MsgBuffer::retrieve(size_t len) {
    if (len >= readableBytes()) { retrieveAll(); return; }
    head_ += len;  // 只移动指针，旧数据仍残留在 [旧head_, 新head_) 区间
}
```

`head_` 右移后，`[旧head_, 新head_)` 区间的数据仍然物理存在于 `buffer_` 中，但**外部代码无法访问到这些数据**，原因如下：

1. **API 封闭性**：`peek()` 返回的是 `begin() + head_`，即新 `head_` 位置；`readableBytes()` 返回 `tail_ - head_`（不包含旧数据区间）。所有读取 API（`peekInt32`、`read`、`operator[]` 等）都以 `head_` 作为起始，**不可能读到 `head_` 左边的数据**。

2. **数据会被自然覆盖**：当 `ensureWritableBytes` 执行碎片整理策略时（策略1），会调用 `std::copy` 把可读数据移动到 `kBufferOffset` 位置，旧数据区间会被覆盖。当 `retrieveAll` 执行时，`head_` 和 `tail_` 都重置为 `kBufferOffset`，之后新数据写入会覆盖旧区域。

3. **不做 memset 是性能优化**：网络 IO 是高频操作，每次 `retrieve` 都对已消费区域做 `memset` 清零完全没有必要——这些字节马上就会被新数据覆盖。

**但有一个理论上的边界情况**：如果 `MsgBuffer` 对象被序列化（比如把整个 `buffer_` 的内存 dump 出去）或者通过内存调试工具查看，确实能看到已消费但未清零的旧数据。对于网络协议数据这通常不是问题；但如果缓冲区中曾经存放过密码等敏感信息，理论上存在残留风险。在游戏服务器场景下，MsgBuffer 存放的是协议包，不包含明文密码，所以不构成实际风险。

---

### 2. `readFd` 中 `extBuffer` 是栈上 8192 字节，如果一次读到超过 `writable + 8192` 的数据会怎样？

**不会出现这种情况**，因为 `readv` 的语义保证了单次调用最多读入 `vec[0].iov_len + vec[1].iov_len` 字节。

看 `readFd` 的实现（MsgBuffer.cc 第 148-173 行）：

```cpp
vec[0].iov_len = writable;          // buffer 可写区大小
vec[1].iov_len = sizeof(extBuffer); // 8192 字节
const int iovcnt = (writable < sizeof extBuffer) ? 2 : 1;
ssize_t n = ::readv(fd, vec, iovcnt);
```

`readv` 是**散布读取**（scatter read），内核会按照 `iovec` 数组的顺序依次填充，**总共最多填充 `vec[0].iov_len + vec[1].iov_len` 字节**（当 `iovcnt == 2` 时）。即使 socket 内核接收缓冲区里有 100KB 数据，这次 `readv` 也最多只读 `writable + 8192` 字节。

**剩余的数据怎么办？**

内核接收缓冲区中未读完的数据会继续保留，下一次 epoll/IOCP 通知 socket 可读时，EventLoop 会再次调用 `readFd`。也就是说，大量数据不是一次 `readFd` 读完的，而是分多次读取，每次最多 `writable + 8192` 字节。

**这个设计的合理性**：

- 8192 字节作为栈上临时缓冲区是一个经验值，既不会爆栈（Linux 默认栈大小 8MB），又能减少小包场景下不必要的 buffer 扩容
- 如果 buffer 可写空间 >= 8192（`writable >= sizeof(extBuffer)`），则 `iovcnt = 1`，不使用 `extBuffer`，直接读入 buffer——这种情况下单次读取上限就是 `writable`
- 对于 LT（Level Triggered）模式，数据没读完内核会持续通知；对于 ET（Edge Triggered）模式，通常会在循环中反复调用 `readFd` 直到返回 `EAGAIN`

---

### 3. `addInFront` 情况 2 为什么要向后移动数据而不是向前？

先看情况 2 触发的条件（MsgBuffer.cc 第 216-221 行）：

```cpp
// 进入情况2的前提：head_ < len（prepend区不够），但 writableBytes() >= len
if (len <= writableBytes()) {
    std::copy(begin() + head_, begin() + tail_, begin() + head_ + len);
    memcpy(begin() + head_, buf, len);
    tail_ += len;
    return;
}
```

**为什么不向前移动？** 关键在于理解此时的内存布局：

```
情况2的前提条件：head_ < len，且尾部可写 >= len

[  head_字节  ][── 可读数据 ──][──── 可写空间(>= len) ────]
0            head_            tail_                    size()
```

- `head_` 前面只有不到 `len` 字节的空间，即使把数据向前移到 offset=0，前面也不够放下 `len` 字节的新数据
- 尾部可写空间 >= `len`，所以把**可读数据整体右移 `len` 字节**，就能在 `[head_, head_+len)` 腾出恰好 `len` 字节的空间来写入新数据

如果向前移动数据（移到 `kBufferOffset` 处），头部空间反而会变得更小（最多 `kBufferOffset` 字节），而 `head_` 已经小于 `len`，`kBufferOffset`（= 8）大概率也小于 `len`，向前移动后**头部仍然不够**。

向后移动的精确逻辑是：

```
操作前：
[小空间][─── 可读数据 ───][──── 足够的可写空间 ────]
       ↑head_            ↑tail_

操作后（右移 len 字节）：
[小空间][新数据 len字节][─── 可读数据 ───][剩余可写]
       ↑head_                            ↑tail_(+=len)
```

新数据写入 `[head_, head_+len)` 处，原有可读数据被推到 `[head_+len, tail_+len)`，`tail_` 增加 `len`。整个操作只需要一次 `std::copy` + 一次 `memcpy`，无需分配新内存。

---

### 4. 为什么 `appendInt16/32/64` 调用 `htons/htonl`，而 `appendInt8` 不调用？

**因为单字节没有字节序的概念。**

字节序（Byte Order / Endianness）是指**多字节整数**在内存中的存放顺序：

- **大端序**（Big Endian / 网络字节序）：高位字节在低地址，如 `0x1234` 存为 `[12, 34]`
- **小端序**（Little Endian / x86）：低位字节在低地址，如 `0x1234` 存为 `[34, 12]`

对于 `uint8_t`（1 个字节），只有一个字节，不存在"高位字节放哪、低位字节放哪"的问题——无论大端小端，内存中就是那一个字节值。所以 `appendInt8` 直接把这个字节写入缓冲区即可：

```cpp
// MsgBuffer.h 第 190-193 行
void appendInt8(const uint8_t b) {
    append(static_cast<const char *>((void *)&b), 1);  // 直接写入，无需转换
}
```

而多字节整数需要转换：

```cpp
// MsgBuffer.cc 第 80-84 行
void MsgBuffer::appendInt16(const uint16_t s) {
    uint16_t ss = htons(s);  // 主机序 → 网络序（大端）
    append(static_cast<const char *>((void *)&ss), 2);
}
```

同理，`peekInt8` 也不调用 `ntoh` 系列函数，直接读取那一个字节的值即可。这是网络编程的基本约定——只有 2 字节及以上的整数才需要做字节序转换。

---
*学习日期：2026-03-06 | 上一课：[第01课_日志系统](第01课_日志系统.md) | 下一课：[第03课_日期时间与工具函数](第03课_日期时间与工具函数.md)*
