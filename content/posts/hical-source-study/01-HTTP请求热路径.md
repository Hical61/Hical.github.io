+++
title = '拆开 Hical：一个 HTTP 请求从 socket 字节到路由分发的完整旅程'
date = 2026-08-10T00:00:00+08:00
draft = false
tags = ["Hical", "HTTP", "请求热路径", "零拷贝", "源码分析", "picohttpparser"]
categories = ["Hical源码精读"]
description = "拆开 Hical 第 1 篇：从 socket 字节到路由分发，追踪一个 HTTP 请求的完整旅程——ReadBufferPool 请求级 borrow、picohttpparser 零拷贝解析、响应前缀模板等。"
+++

# [Hical] 一个 HTTP 请求在 Hical 里经历了什么？从 socket 字节到路由分发

> 本专栏文章：拆开 Hical · 第 1 篇

用过 Web 框架的人很多，知道"一个请求怎么从 socket 字节变成 handler 参数"的人很少。大部分框架把这层封装得严严实实，你只要写 `app.get("/", handler)` 就行。

但这篇文章要干相反的事——把 Hical 的整个请求处理链路**扒开看**。读完你能回答：请求头为什么零堆分配、响应头怎么一次 async_write 发出去、ReadBufferPool 为什么是请求级 borrow 而不是连接级持有。

---

## 1. 大图：一个请求的完整旅程

```
socket 上来了字节
     │
     ▼
readBuf = ReadBufferPool::acquire()     ← 借一块 8KB 缓冲
     │
     ▼
picohttpparser 解析 → phr_header[64]   ← 请求头解析到栈数组
     │
     ▼
构造 NativeRequest (string_view)        ← 零拷贝,指针指到 readBuf
     │
     ▼
Router::dispatch(req) ──→ handler 协程  ← 你的业务代码
     │
     ▼
NativeResponse::serializeHeadTo(buf)    ← 序列化到栈上 512 字节
     │
     ▼
async_write(socket, buf)                ← 一次异步写
     │
     ▼
readBuf 还给 ReadBufferPool::return_()  ← 归还 8KB 缓冲
```

下面每一步都拆开讲"为什么这样设计"。

---

## 2. ReadBufferPool：为什么是请求级 borrow 而不是连接级持有？

### 2.1 问题

一个 HTTP 连接在一个请求处理完后，可能还要在 keep-alive 模式下存活几十秒。如果每个连接都持有自己的 8KB 读缓冲：

- 10,000 个空闲 keep-alive 连接 → 80MB 内存被"占着不用"
- CPU cache 被这些永远不会被访问的内存污染

### 2.2 解法

```cpp
// 读缓冲池：thread_local 级别，用完归还
class ReadBufferPool 
{
    thread_local static std::vector<std::unique_ptr<std::string>> pool_;

    static std::unique_ptr<std::string> acquire() 
    {
        if (!pool_.empty()) 
        {
            auto buf = std::move(pool_.back());
            pool_.pop_back();
            buf->clear();
            return buf;
        }
        auto buf = std::make_unique<std::string>();
        buf->reserve(8192);  // 8KB
        return buf;
    }

    static void return_(std::unique_ptr<std::string> buf) 
    {
        if (pool_.size() < kMaxPoolSize)  // 上限 32
        {
            pool_.push_back(std::move(buf));
        }
        // 超了就直接销毁
    }
};
```

**请求开始时**：`acquire()` 从 thread_local 池借一块（或者新建）
**请求结束时**：`return_()` 归还

空闲连接在两次请求之间**不占任何读缓冲**。10,000 个 keep-alive 连接的内存从 80MB 降到接近零。

> 💡 为什么 thread_local 而不是全局池？省掉了锁。HTTP 请求从读到写全在同一 io_context 线程上，跨线程不需要共享读缓冲池。

### 2.3 空闲连接怎么读"第一条"数据？

当连接空闲时，读缓冲已经被归还了。Hical 用 256 字节的**栈缓冲**做首次探测：

```cpp
// 256 字节栈缓冲 → 零堆分配 → 探测新请求的第一段数据
char peek[256];
socket.async_read_some(boost::asio::buffer(peek, 256), ...);
```

收到数据后再去借 8KB 缓冲、把 256 字节拷贝进去继续读。大部分 HTTP 请求头在 500-800 字节，但探测只需要看到 method + path 就能开始 parse。

> 💡 这避免了 `async_wait` → `epoll_ctl(MOD)` 的额外系统调用开销——直接 `async_read_some` 等待数据来。

---

## 3. 零拷贝请求解析：picohttpparser + NativeRequest

### 3.1 为什么选 picohttpparser 而不是自己写？

自己写 HTTP/1.1 解析器不难，但往后你会踩到这些坑：
- 分块传输编码的边界条件
- `\r\n` vs `\n` 的兼容性
- 请求行中 method 和 URI 之间可以有多个空格
- 多个 `Transfer-Encoding` 头的优先级

picohttpparser 是 H2O HTTP/2 服务器用的同一个解析器，被验证了十年。选它不是因为写不出来，是没必要重造轮子。

### 3.2 解析流程

```cpp
int minor_version;
int status;
struct phr_header headers[64];    // ← 栈分配！零堆分配！
size_t num_headers = 64;
const char* method;
size_t method_len;
const char* path;
size_t path_len;

// phr_parse_request 直接在 readBuf 的内容上做 in-place 解析
status = phr_parse_request(
    readBuf->data(), readBuf->size(),
    &method, &method_len,    // 输出：method 起始位置和长度
    &path, &path_len,        // 输出：path 起始位置和长度
    &minor_version,
    headers, &num_headers,   // 输出：解析出来的头字段
    0);
```

解析完后：
- `method` 指针指向 `readBuf` 内部 "GET\0..." 的起始
- `path` 指针指向 readBuf 内部 "/api/users\0..." 的起始
- `headers` 数组的每个 name/value 指针都指向 readBuf 内部

**没有一次拷贝**。method、path、headers 全部是 string_view，直接引用 readBuf 里的字节。

### 3.3 NativeRequest：64 个栈分配的字段槽位

```cpp
struct NativeRequest 
{
    // 请求头字段：栈分配的 array，最多 64 个
    static constexpr size_t kMaxHeaders = 64;
    using RequestHeaders = std::array<std::pair<std::string_view, std::string_view>, kMaxHeaders>;

    RequestHeaders headers;     // 零堆分配的头部存储
    size_t headerCount = 0;

    std::string_view method;    // 直接引用 readBuf
    std::string_view target;    // 直接引用 readBuf
    std::string body;           // body 单独分配（体可能很大）

    // ...
};
```

**为什么是 64？** 实际统计显示，99.9% 的 HTTP 请求头字段数 ≤ 30。64 是一个安全的硬上限——超出时会返回 431 Request Header Fields Too Large。

**为什么用 `array<pair<string_view, string_view>, 64>` 而不是 `unordered_map`？**

- 64 个 pair 总共 ~1KB，栈分配
- 线性扫描在小 N 时比 hash 查找快（L1 cache 友好）
- 不需要处理 hash 冲突

> 💡 这就是 Hical 的设计哲学：针对典型情况（< 20 个头字段）做优化，边缘情况（> 64）直接拒绝。

---

## 4. HeaderMap：大小写不敏感的线性查找

离开 NativeRequest 后，响应头的处理用的是 HeaderMap：

```cpp
class HeaderMap 
{
    using EntryList = std::vector<std::pair<std::string, std::string>>;

    EntryList entries_;  // 存的是 owned string（响应头不是引用 readBuf）

public:
    // 大小写不敏感的查找
    auto find(std::string_view key) const 
    {
        for (auto& [name, value] : entries_) 
        {
            if (caseInsensitiveEqual(name, key)) 
            {
                return &value;
            }
        }
        return nullptr;
    }
};
```

**为什么响应头用 `vector<pair>` 而不是 `unordered_map`？**

- 响应头通常 5-15 个（Server/Date/Content-Type/Content-Length/Connection）
- 线性扫描 5-15 个元素是 O(N) 但 N 极小 → 比 hash 的常数开销更快
- 不需要处理 hash 冲突、不需要维护 bucket

**但 insert 怎么处理重复？** 大小写不敏感的覆盖：找到同名字段就替换 value，没找到就追加。同时支持多值（Set-Cookie 可以有多个）：

```cpp
void insert(std::string_view key, std::string_view value) 
{
    auto it = find(key);
    if (it) 
    {
        *it = value;  // 已有→覆盖
    } 
    else 
    {
        entries_.emplace_back(key, value);  // 新字段→追加
    }
}

void add(std::string_view key, std::string_view value) 
{
    entries_.emplace_back(key, value);  // 总是追加（Set-Cookie）
}
```

---

## 5. 响应序列化：FixedBuffer\<512\> + 响应前缀模板

### 5.1 一次 async_write 发出去

你肯定见过这种代码：

```cpp
// 常见的反模式：header 和 body 分两次 write
co_await async_write(socket, header);
co_await async_write(socket, body);
```

两次 write = 两个 TCP 段。在非 cork 的 socket 上，客户端可能收到两个 TCP 包——多了一次 round-trip。

Hical 的做法：

```cpp
// 序列化头部到栈 Buffer（512 字节够用）
FixedBuffer<512> headBuf;
nativeRes.serializeHeadTo(headBuf);

if (!nativeRes.body.empty()) 
{
    // header + body 一次 async_write
    std::array<boost::asio::const_buffer, 2> bufs = {
        boost::asio::buffer(headBuf.data(), headBuf.size()),
        boost::asio::buffer(nativeRes.body)};
    co_await async_write(socket, bufs, use_awaitable);
} 
else 
{
    co_await async_write(socket, boost::asio::buffer(headBuf.data(), headBuf.size()),
                         use_awaitable);
}
```

**FixedBuffer\<512\> 是什么？**

```cpp
template <size_t N = 512>
class FixedBuffer 
{
    char buf_[N];
    size_t size_ = 0;

public:
    // 常规大小在栈缓冲里写，溢出才上堆
    auto& operator<<(std::string_view sv) 
    {
        if (size_ + sv.size() > N) 
        {
            expandToHeap(size_ + sv.size());  // 超了→转到 std::string（堆）
        }
        memcpy(buf_ + size_, sv.data(), sv.size());
        size_ += sv.size();
        return *this;
    }
};
```

常规情况下响应头 150-300 字节——远小于 512。**零堆分配**。

### 5.2 响应前缀模板：省掉 3 次 HeaderMap::insert

每个 HTTP 响应都有三个固定头：`Server`、`Connection`、`Date`。与其每请求做 3 次 `HeaderMap::insert` + 序列化，不如**预建好送它**：

```cpp
// HttpSessionImpl 中：连接级别的前缀缓存（128 字节）
char responsePrefix[128];
size_t prefixLen;

// 预建：Server + Connection（固定部分）
memcpy(responsePrefix, "Server: Hical\r\n", 15);
prefixLen = 15;

// 每秒更新一次 Date（29 字节 memcpy）
thread_local DateCache dateCache;
auto date = cachedHttpDate();  // thread_local，每秒更新一次
memcpy(responsePrefix + prefixLen, "Date: ", 6);
memcpy(responsePrefix + prefixLen + 6, date.data(), date.size());
memcpy(responsePrefix + prefixLen + 6 + date.size(), "\r\n", 2);
prefixLen = ...;

// keep-alive 请求：用 memcpy 把前缀粘到响应头前面
// 省了 3 次 HeaderMap::insert + 3 次序列化循环
FixedBuffer<512> headBuf;
headBuf << "HTTP/1.1 ";
headBuf << statusCodeStr;
headBuf << ' ';
headBuf << reason;
headBuf << "\r\n";
headBuf.append(responsePrefix, prefixLen);  // ← 一条 memcpy 搞定
headBuf << "Content-Type: text/html\r\n";
headBuf << "Content-Length: 1024\r\n";
headBuf << "\r\n";
```

> 💡 对 keep-alive 请求，这省了 3×HeaderMap::insert（每次 insert 有 case-insensitive 字符串比较）+ 3×序列化（循环写入 `key: value\r\n`）。对 P99 延迟有可测量的收益。

---

## 6. 异常路径：sendRawResponse 的栈分配

框架在路由分发之前可能遇到各种错误——请求头太大（431）、body 太大（413）、路由不匹配（404）。这些错误响应必须快，因为它们是**请求处理之外的开销**。

```cpp
// 栈上构造错误响应，零堆分配
Awaitable<void> sendRawResponse(socket, 413, "Payload Too Large", body)
{
    FixedBuffer<512> buf;
    buf << "HTTP/1.1 ";
    buf << "413";           // to_chars
    buf << " Payload Too Large\r\n";
    buf << "Content-Length: ";
    buf << body.size();     // to_chars
    buf << "\r\nConnection: close\r\n\r\n";
    buf << body;

    co_await async_write(socket, boost::asio::buffer(buf.data(), buf.size()),
                         redirect_error(use_awaitable, ec));
}
```

`sendRawResponse` 是完全自足的——从 status code 到 body，全部在栈上的 FixedBuffer\<512\> 中构造。不需要分配堆内存，不需要查 HeaderMap。

---

## 7. FileBody：大文件的零拷贝发送

对于静态文件（通常是大于内存的），把整个文件读进内存再发回是浪费的。Hical 的 FileBody 直接走异步文件读取：

```cpp
struct FileBody 
{
    std::string filePath;
    int64_t offset;
    int64_t length;  // Range 请求时可能只是文件的一部分
};

// writeFileResponse 协程
Awaitable<void> writeFileResponse(socket, nativeRes) 
{
    auto& file = nativeRes.fileBody;

    TcpCorkGuard cork(socket);  // Linux: TCP_CORK, 合并头+首块

    // 先发头部
    co_await async_write(socket, ..., use_awaitable);

    // 分 64KB 块异步读+写文件
    char chunk[65536];
    for (int64_t pos = file->offset; pos < file->offset + file->length; ) 
    {
        auto bytes = co_await readFile(file->filePath, pos, chunk, 65536);
        co_await async_write(socket, boost::asio::buffer(chunk, bytes), use_awaitable);
        pos += bytes;
    }
}
```

**TcpCorkGuard** 是 RAII 包装——Linux 用 `TCP_CORK` 把响应头 + 第一块 64KB 合并成一个 TCP 段，省了一次 round-trip。macOS 用 `TCP_NOPUSH`，Windows 上 no-op（应用层的 scatter-gather 已经做了合并）。

---

## 回顾一下你学会了什么

1. 一个 HTTP 请求在 Hical 中的轨迹：readBuf 借 → parse → dispatch → 序列化 → async_write → readBuf 还
2. ReadBufferPool 为什么是请求级 borrow——空闲连接零读缓冲，消除了 keep-alive 连接的 80MB 内存浪费
3. picohttpparser 产出的 `method`/`path`/`headers` 全是 string_view，零拷贝引用 readBuf
4. NativeRequest 的 `array<pair<string_view,string_view>, 64>` 栈分配——典型请求 ≤ 30 个头字段,线性扫描对 < 20 个字段比 hash 快
5. 响应序列化：`FixedBuffer<512>` 栈分配 + header + body 一次 async_write
6. 响应前缀模板（Server/Connection/Date 的 wire bytes）省了每请求 3 次 HeaderMap::insert
7. 错误响应走 `sendRawResponse`——全部在栈上完成，不查 HeaderMap
8. FileBody 走异步文件读取 + TCP_CORK = 零拷贝大文件发送

---

> 下一篇：[Router 是怎么做到 O(1) 路由匹配的？兼谈 ~40ns 的 dispatchSync 快速路径]({{< relref "posts/hical-source-study/02-Router路由匹配.md" >}})
