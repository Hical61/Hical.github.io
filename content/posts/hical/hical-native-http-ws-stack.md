+++
title = 'Hical v2.6.0：移除 Boost.Beast，自研 HTTP/WebSocket 栈全记录'
date = '2026-05-11'
draft = false
tags = ["C++20", "Hical", "HTTP栈", "WebSocket", "picohttpparser", "RFC 6455"]
categories = ["Hical框架"]
description = "完整记录 Hical 移除 Boost.Beast、自研零拷贝 HTTP 解析栈和 RFC 6455 WebSocket 实现的工程实践，框架层 CPU 开销从 5.5% 降至 2.5%。"
+++

# Hical v2.6.0：移除 Boost.Beast，自研 HTTP/WebSocket 栈全记录

> Hical v2.6.0 完成了一次关键架构升级——彻底移除 Boost.Beast 依赖，HTTP 解析/序列化和 WebSocket 全部替换为自研实现。本文从动机、架构设计、关键技术细节、性能数据四个维度，完整记录这次"去 Beast"的工程实践。

---

## 目录

- [Hical v2.6.0：移除 Boost.Beast，自研 HTTP/WebSocket 栈全记录](#hical-v260移除-boostbeast自研-httpwebsocket-栈全记录)
  - [目录](#目录)
  - [1. 为什么移除 Boost.Beast](#1-为什么移除-boostbeast)
  - [2. 新架构总览](#2-新架构总览)
  - [3. 自研 HTTP 解析栈](#3-自研-http-解析栈)
    - [3.1 picohttpparser：极致轻量的 C 解析器](#31-picohttpparser极致轻量的-c-解析器)
    - [3.2 零拷贝 NativeRequest](#32-零拷贝-nativerequest)
    - [3.3 HeaderMap：L1 友好的头部容器](#33-headermapl1-友好的头部容器)
  - [4. 自研 HTTP 响应序列化](#4-自研-http-响应序列化)
    - [4.1 栈缓冲 + Scatter-Gather I/O](#41-栈缓冲--scatter-gather-io)
    - [4.2 状态行预计算](#42-状态行预计算)
  - [5. 自研 WebSocket 栈（RFC 6455）](#5-自研-websocket-栈rfc-6455)
    - [5.1 帧解析器：WsFrame](#51-帧解析器wsframe)
    - [5.2 握手协议：WsHandshake](#52-握手协议wshandshake)
    - [5.3 Permessage-Deflate 压缩](#53-permessage-deflate-压缩)
    - [5.4 消息分片重组](#54-消息分片重组)
    - [5.5 协议安全校验](#55-协议安全校验)
  - [6. 编译防火墙与工程化](#6-编译防火墙与工程化)
  - [7. 性能对比数据](#7-性能对比数据)
    - [火焰图逐项对比](#火焰图逐项对比)
    - [QPS 端到端对比](#qps-端到端对比)
    - [与同类框架对比](#与同类框架对比)
  - [8. 迁移影响与 Breaking Changes](#8-迁移影响与-breaking-changes)
    - [对框架使用者](#对框架使用者)
    - [对框架使用者透明的改动](#对框架使用者透明的改动)
  - [9. 新增依赖](#9-新增依赖)
    - [picohttpparser](#picohttpparser)
    - [zlib](#zlib)
  - [10. 总结](#10-总结)

---

## 1. 为什么移除 Boost.Beast

Beast 是一个优秀的 HTTP/WebSocket 库，但在 Hical 的高性能场景中，它成了主要瓶颈：

| 问题          | 具体表现                                                | 火焰图占比 |
| ------------- | ------------------------------------------------------- | ---------- |
| HTTP 解析开销 | `basic_parser::put` + `parse_fields`                    | 0.63%      |
| Header 堆分配 | `basic_fields::new_element`，每个头部一次 `new`         | 0.95%      |
| 响应序列化    | `serializer::next` + `write_op` + scatter-gather 重模板 | 1.9%       |
| **合计**      | **Beast 相关 CPU 占用**                                 | **~3.5%**  |

3.5% 看起来不大，但在内核 TCP 栈已占 65% 的情况下，用户态可优化空间只有 ~35%。Beast 独占了其中 10% 的可优化空间。

更关键的是**间接成本**：

- **编译时间**：Beast 的重模板设计导致包含 `<boost/beast.hpp>` 的翻译单元编译极慢
- **二进制膨胀**：`basic_parser`、`serializer`、`basic_fields` 的模板实例化代码体积巨大
- **API 限制**：Beast 的 `basic_fields`（HTTP header 容器）是链表实现，无法做零拷贝优化
- **耦合风险**：Beast 的 parser 和 serializer 深度绑定其内部类型，阻碍进一步定制

在 v2.5.2 火焰图分析完成后，"去 Beast"成为必然选择。

---

## 2. 新架构总览

v2.6.0 的 HTTP/WebSocket 栈分为四层：

```
┌─────────────────────────────────────────────────────┐
│  HttpServer（门面层）                                │
│  TcpServer + Router + MiddlewarePipeline            │
├─────────────────────────────────────────────────────┤
│  HttpSessionImpl（会话层，编译防火墙）               │
│  连接级 readBuf 复用 / keep-alive / upgrade         │
├──────────────────────┬──────────────────────────────┤
│  HTTP 解析           │  WebSocket 栈                │
│  picohttpparser      │  WsFrame + WsHandshake       │
│  NativeRequest       │  WsDeflate (zlib)            │
│  HeaderMap           │  消息分片重组                 │
├──────────────────────┼──────────────────────────────┤
│  HTTP 响应           │  WebSocket 发送              │
│  NativeResponse      │  帧构造 + masking            │
│  FixedBuffer<512>    │  scatter-gather              │
│  Scatter-Gather I/O  │                              │
└──────────────────────┴──────────────────────────────┘
```

依赖关系：

- **移除**：`boost/beast/http.hpp`、`boost/beast/websocket.hpp`
- **保留**：Boost.Asio（网络 I/O + 协程调度）、Boost.JSON（JSON 处理）
- **新增**：picohttpparser（vendored，~1500 行 C）、zlib（WebSocket 压缩）

---

## 3. 自研 HTTP 解析栈

### 3.1 picohttpparser：极致轻量的 C 解析器

[picohttpparser](https://github.com/h2o/picohttpparser) 是 H2O 项目提取的高性能 HTTP/1.x 解析器，核心特点：

- 纯 C 实现，~1500 行，零依赖
- 增量解析友好：数据不足时返回 `-2`，调用方继续 `recv` 后重试
- 零拷贝：输出的 method/path/headers 全是指向输入 buffer 的指针+长度
- 被 H2O、Rust hyper、Node.js llhttp 等项目采用

在 Hical 中的集成方式：

```cpp
// HttpSessionImpl.cpp
extern "C" {
#include "picohttpparser.h"
}

// 增量解析主循环
const char* method;
size_t methodLen;
const char* path;
size_t pathLen;
int minorVersion;
phr_header headers[64];  // 栈上分配，零堆分配
size_t numHeaders = 64;

int parseResult = phr_parse_request(
    m_readBuf.data(), m_readBuf.size(),
    &method, &methodLen,
    &path, &pathLen,
    &minorVersion,
    headers, &numHeaders,
    m_prevBufLen  // 增量解析偏移
);

if (parseResult == -2) {
    // 数据不足，继续读取
    co_await asyncReadSome();
    continue;
}
```

火焰图占比：**0.06%**（vs Beast 的 0.63%，降低 90%）。

### 3.2 零拷贝 NativeRequest

Beast 的 `http::request<http::string_body>` 在解析完成后拥有所有数据的独立拷贝。Hical 的 `NativeRequest` 则完全零拷贝：

```cpp
struct NativeRequest {
    std::string_view method;    // → readBuf 中的 "GET"
    std::string_view target;    // → readBuf 中的 "/api/users?page=1"
    RequestHeaders headers;     // 栈上数组，string_view → readBuf
    std::string body;           // Body 单独拥有（可能跨多次 read）
};
```

关键设计：

- **连接级 `readBuf`**：一个 `std::string`，跨 keep-alive 请求复用，不重复分配
- **`string_view` 直接引用**：method、path、每个 header 的 name/value 都是 `readBuf` 上的视图
- **栈上 `RequestHeaders`**：固定大小 `array<Entry, 64>`，覆盖 99.9% 的实际请求
- **生命周期保证**：`NativeRequest` 仅在当前请求处理期间有效，handler 返回后 `readBuf` 可安全推进

与 Beast 的对比：

| 维度        | NativeRequest            | Beast request              |
| ----------- | ------------------------ | -------------------------- |
| Header 存储 | 栈上数组                 | 链表（每 header 一次 new） |
| 数据所有权  | string_view 引用 readBuf | 独立 string 拷贝           |
| 内存分配    | 0 次堆分配               | N+1 次（N=header数）       |
| 缓存友好    | 连续内存                 | 指针追踪                   |

### 3.3 HeaderMap：L1 友好的头部容器

HTTP 响应头使用自研 `HeaderMap`：

```cpp
class HeaderMap {
    std::vector<std::pair<std::string, std::string>> m_headers;
public:
    // 大小写不敏感查找
    std::string_view find(std::string_view name) const;
    // O(1) 追加（不检查重复，适用于响应头构建）
    void insert(std::string_view name, std::string_view value);
    // O(N) 设置（查找+替换或追加）
    void set(std::string_view name, std::string_view value);
};
```

为什么不用 `unordered_map`：

- 典型 HTTP 响应只有 5-15 个头部
- 线性扫描 15 个 pair 约 240 字节，完全在 L1 缓存行（64B × 4）内
- hash map 的桶数组 + 节点指针追踪在小 N 下反而更慢
- `vector` 连续内存对 prefetcher 友好

---

## 4. 自研 HTTP 响应序列化

### 4.1 栈缓冲 + Scatter-Gather I/O

Beast 的 `http::serializer` 使用重模板实现，支持各种 body 类型和 chunk 编码，代价是火焰图中 1.9% 的 CPU 占用。

Hical 的序列化策略极度简洁：

```cpp
// 头部序列化到栈缓冲
FixedBuffer<512> headBuf;
response.serializeHeadTo(headBuf);
// 输出示例："HTTP/1.1 200 OK\r\nContent-Length: 13\r\n...\r\n"

if (headBuf.size() + body.size() <= 512) {
    // 小响应：合并到一个缓冲区，单次 write
    headBuf.append(body.data(), body.size());
    co_await async_write(socket, buffer(headBuf.data(), headBuf.size()));
} else {
    // 大响应：scatter-gather，避免 body memcpy
    std::array<const_buffer, 2> bufs = {
        buffer(headBuf.data(), headBuf.size()),
        buffer(body.data(), body.size())
    };
    co_await async_write(socket, bufs);
}
```

设计考量：

- **FixedBuffer<512>**：典型 HTTP 响应头 200-400 字节，512B 栈空间足够覆盖 99% 场景
- **单次 write vs scatter-gather**：小响应（API JSON 通常 <100B）合并后一次系统调用；大响应避免拷贝
- **无模板膨胀**：整个序列化逻辑约 50 行，编译后几百字节机器码

### 4.2 状态行预计算

常见状态码的响应行是编译期常量，运行时直接 `memcpy`：

```cpp
// 预计算的状态行
static constexpr std::string_view STATUS_200 = "HTTP/1.1 200 OK\r\n";
static constexpr std::string_view STATUS_404 = "HTTP/1.1 404 Not Found\r\n";
// ...

void NativeResponse::serializeHeadTo(FixedBuffer<512>& buf) {
    buf.append(statusLine());  // 直接拷贝预计算字符串
    for (auto& [name, value] : m_headers) {
        buf.append(name);
        buf.append(": ");
        buf.append(value);
        buf.append("\r\n");
    }
    buf.append("\r\n");
}
```

---

## 5. 自研 WebSocket 栈（RFC 6455）

Beast WebSocket 的移除比 HTTP 更有挑战——WebSocket 协议本身就复杂（帧格式、分片、masking、压缩扩展），需要完整重新实现。

### 5.1 帧解析器：WsFrame

WebSocket 帧格式（RFC 6455 §5.2）：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)    |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+-------------------------------+
|                Masking-key (if MASK == 1)                      |
+-------------------------------+-------------------------------+
|                         Payload Data                           |
+---------------------------------------------------------------+
```

自研解析器：

```cpp
struct WsFrameHeader {
    bool fin;
    bool rsv1, rsv2, rsv3;
    WsOpcode opcode;       // Text/Binary/Close/Ping/Pong/Continuation
    bool masked;
    uint64_t payloadLength;
    uint8_t maskKey[4];
    size_t headerSize;     // 帧头总长度：2~14 字节
};

// 增量解析：数据不足返回 nullopt
std::optional<WsFrameHeader> parseWsFrameHeader(
    const uint8_t* data, size_t size);
```

Masking XOR 优化——批量 4 字节处理：

```cpp
inline void unmaskPayload(uint8_t* data, size_t len, const uint8_t mask[4]) {
    uint32_t mask32;
    std::memcpy(&mask32, mask, 4);

    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t* p = reinterpret_cast<uint32_t*>(data + i);
        *p ^= mask32;  // 单条指令处理 4 字节
    }
    for (; i < len; ++i) {
        data[i] ^= mask[i % 4];
    }
}
```

### 5.2 握手协议：WsHandshake

RFC 6455 §4.2 定义的 WebSocket 升级握手：

```
客户端请求：
GET /chat HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
Sec-WebSocket-Extensions: permessage-deflate

服务端响应：
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover
```

`Sec-WebSocket-Accept` 的计算：

```cpp
std::string computeAcceptKey(std::string_view clientKey) {
    // RFC 6455 §4.2.2: SHA1(clientKey + GUID)
    static constexpr auto GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string input(clientKey);
    input += GUID;

    unsigned char sha1Hash[20];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()),
         input.size(), sha1Hash);

    return base64Encode(sha1Hash, 20);
}
```

### 5.3 Permessage-Deflate 压缩

WebSocket permessage-deflate 扩展（RFC 7692）允许对消息 payload 进行 zlib 压缩，典型压缩率 60-80%。

设计采用 **Pimpl 模式**隔离 zlib 依赖：

```cpp
// WsDeflate.h — 公开接口，不暴露 zlib 头文件
struct WsDeflateConfig {
    bool enabled = false;
    int serverMaxWindowBits = 15;
    int clientMaxWindowBits = 15;
    bool serverNoContextTakeover = false;
    bool clientNoContextTakeover = false;
    int compressionLevel = 1;       // 速度优先
    int memLevel = 8;
};

class WsDeflateContext {
public:
    WsDeflateContext(const WsDeflateConfig& config);
    ~WsDeflateContext();

    // 压缩（发送前）
    std::string compress(std::string_view payload);
    // 解压（接收后）
    std::string decompress(std::string_view payload, size_t maxSize);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  // 隔离 zlib z_stream
};
```

为什么默认 `compressionLevel = 1`：

- WebSocket 场景通常是小消息（<4KB JSON）
- Level 1 压缩速度是 Level 6 的 3-5 倍，压缩率差距仅 5-10%
- 高频小消息场景下 CPU 节省 > 带宽节省

### 5.4 消息分片重组

WebSocket 允许将一条消息拆分为多个帧发送（Fragmentation），接收端需要重组：

```cpp
// 会话级分片状态
class WebSocketSession {
    std::string m_fragmentBuf;           // 累积分片数据
    WsOpcode m_fragmentOpcode;           // 首帧操作码（Text/Binary）
    bool m_fragmentCompressed = false;   // 首帧 RSV1 标记

    Awaitable<std::optional<WsMessage>> receive() {
        while (true) {
            auto frame = co_await readFrame();

            if (frame.opcode == WsOpcode::Continuation) {
                // 续帧：追加到缓冲
                m_fragmentBuf.append(frame.payload);
                if (frame.fin) {
                    // 最后一帧：返回完整消息
                    auto msg = assembleMessage();
                    m_fragmentBuf.clear();
                    co_return msg;
                }
            } else if (!frame.fin) {
                // 首帧但非 FIN：开始分片
                m_fragmentOpcode = frame.opcode;
                m_fragmentCompressed = frame.rsv1;
                m_fragmentBuf = std::move(frame.payload);
            } else {
                // 单帧完整消息
                co_return makeMessage(frame);
            }
        }
    }
};
```

控制帧（Ping/Pong/Close）可以穿插在分片消息中间，解析器正确处理这种情况而不打断分片重组。

### 5.5 协议安全校验

自研栈实现了严格的 RFC 6455 协议校验：

```cpp
// 1. 客户端帧必须 masked
if (!hdr->masked) {
    co_await sendClose(WsCloseCode::ProtocolError,
                       "Client frames must be masked");
    co_return std::nullopt;
}

// 2. 控制帧不允许分片，且 payload ≤ 125B
if (isControlFrame(hdr->opcode)) {
    if (!hdr->fin) {
        co_await sendClose(WsCloseCode::ProtocolError,
                           "Fragmented control frame");
    }
    if (hdr->payloadLength > 125) {
        co_await sendClose(WsCloseCode::ProtocolError,
                           "Control frame payload too large");
    }
}

// 3. RSV2/RSV3 禁止（未协商对应扩展）
if (hdr->rsv2 || hdr->rsv3) {
    co_await sendClose(WsCloseCode::ProtocolError,
                       "Unexpected RSV2/RSV3 bits");
}

// 4. 消息大小限制（zip bomb 防护）
if (totalSize > MAX_MESSAGE_SIZE) {  // 默认 1MB
    co_await sendClose(WsCloseCode::MessageTooBig,
                       "Message exceeds size limit");
}
```

---

## 6. 编译防火墙与工程化

所有 HTTP 解析和 WebSocket 实现都隔离在 `HttpSessionImpl.cpp` 中：

```
src/core/
├── HttpRequest.h       ← 公开 API（string_view 取值方法）
├── HttpResponse.h      ← 公开 API（set header / body）
├── HttpSessionImpl.cpp ← 编译防火墙：picohttpparser + WsFrame + WsDeflate
├── WsFrame.h           ← WebSocket 帧解析（仅 impl 内部使用）
├── WsHandshake.h       ← WebSocket 握手（仅 impl 内部使用）
└── WsDeflate.h/cpp     ← zlib 压缩（Pimpl 隔离）
```

好处：

- 修改 Router、Middleware、业务 handler **不触发** HTTP/WS 栈重编译
- picohttpparser 的 C 代码通过 `extern "C"` 包含，不污染 C++ 命名空间
- zlib 的 `z_stream` 通过 Pimpl 隔离，使用方不需要 `#include <zlib.h>`

---

## 7. 性能对比数据

### 火焰图逐项对比

| 组件             | Beast 路径 | 自研路径         | 节省       |
| ---------------- | ---------- | ---------------- | ---------- |
| HTTP 解析        | 0.63%      | 0.06%            | **-0.57%** |
| Header 存储/分配 | 0.95%      | ~0% (栈)         | **-0.95%** |
| 响应序列化       | 1.9%       | 含在框架 2.5% 内 | **-1.9%**  |
| 框架总计         | ~5.5%      | **2.5%**         | **-3%**    |

### QPS 端到端对比

测试环境：Docker (Ubuntu 24.04, GCC 14, -O2)，wrk 4 线程 keep-alive

| 版本                           | QPS         | 相对基线  |
| ------------------------------ | ----------- | --------- |
| v2.5.1 (Beast)                 | 27,493      | 基线      |
| v2.5.2 (Beast + dispatchSync)  | 27,493      | +0%       |
| v2.6.0 (自研栈 + SO_REUSEPORT) | **159,000** | **+478%** |

> 注：QPS 提升不完全来自去 Beast，SO_REUSEPORT 多 acceptor + 连接级原子超时贡献了主要增量。去 Beast 单独贡献约 132K → 140K 的 6% 提升，但更重要的价值是：消除了编译膨胀、解锁了后续微优化空间。

### 与同类框架对比

| 框架             | QPS       | 备注         |
| ---------------- | --------- | ------------ |
| Cinatra          | ~165K     | 参考对象     |
| Drogon           | ~160K     | 参考对象     |
| **Hical v2.6.0** | **~159K** | 追平第一梯队 |

从 6 倍差距到基本追平，"去 Beast"是关键的一步。

---

## 8. 迁移影响与 Breaking Changes

### 对框架使用者

| 变更                | 影响                                           | 迁移方式                             |
| ------------------- | ---------------------------------------------- | ------------------------------------ |
| `native()` 返回类型 | 从 `beast::http::request` 变为 `NativeRequest` | 通常无需调用 native()，公开 API 不变 |
| WebSocket API       | 内部实现替换                                   | `WsOptions` 公开接口不变             |
| 新增 zlib 依赖      | 构建时需要 zlib                                | 几乎所有平台已自带                   |

### 对框架使用者透明的改动

- `req.method()` / `req.path()` / `req.header()` 等公开 API **完全不变**
- Router 注册方式不变
- Middleware 接口不变
- `HttpResponse` 构建方式不变

---

## 9. 新增依赖

### picohttpparser

- 来源：[h2o/picohttpparser](https://github.com/h2o/picohttpparser)
- 方式：vendored（直接包含源码，2 个文件）
- 体积：~1500 行 C
- 许可：MIT

### zlib

- 用途：WebSocket permessage-deflate 压缩
- 体积：~15,000 行 C，编译后 ~100KB
- 状态：几乎所有操作系统和包管理器都自带
- 构建：CMake `find_package(ZLIB)` 自动查找

两个依赖加起来不到 2 万行 C 代码，远小于被移除的 Boost.Beast 的头文件体积。

---

## 10. 总结

v2.6.0 的"去 Beast"不是为了技术炫耀，而是性能优化的必然产物：

1. **火焰图驱动**：先量化 Beast 的开销（3.5%），确认值得替换
2. **分阶段实施**：HTTP 解析 → 响应序列化 → WebSocket，逐步验证
3. **零拷贝优先**：`string_view` 引用连接级 buffer，消除堆分配
4. **安全不妥协**：RFC 6455 完整校验，zip bomb 防护，协议合规
5. **编译隔离**：Pimpl + 编译防火墙，使用者无感知

最终效果：

- **CPU 占用**：框架层 5.5% → 2.5%（-55%）
- **QPS**：27K → 159K（+478%，配合调度模型优化）
- **编译依赖**：移除 `boost/beast/` 整个目录
- **新增依赖**：仅 picohttpparser（vendored）+ zlib（系统库）

Hical 现在拥有完全自主可控的 HTTP/WebSocket 栈，后续的每一个字节、每一次系统调用都在我们的优化视野之内。

---

> **Hical** — 基于 C++20 的现代高性能 Web 框架 | [GitHub](https://github.com/panyingself/Hical)
