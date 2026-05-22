+++
title = 'Hical 性能优化全记录'
date = '2026-05-22'
draft = false
tags = ["C++", "性能优化", "Hical", "Web框架", "火焰图", "协程", "Boost.Asio"]
categories = ["Hical框架"]
description = "Hical C++20/26 Web 框架的完整性能优化过程——从 27K QPS 追平 Cinatra/Drogon 的 159K QPS，6 个阶段的火焰图驱动优化实录，涵盖协程帧削减、原生 HTTP 栈、syscall 削减等核心手段。"
+++

## 优化背景

Hical 是我写的 C++20/26 Web 框架，跑 Hello World 压测时起初只有 27K QPS，而同类框架（Cinatra 165K、Drogon 170K）差了将近一个数量级。目标很明确：**追平 Cinatra/Drogon 的水平**。

整个优化过程分 6 个阶段，不是拍脑袋乱改，每一步都是 **perf + 火焰图定位瓶颈 → 想方案 → 写代码 → 跑压测验证** 的循环。能看到数字变化才算数。

---

## 阶段 1：协程帧削减（v2.5.1-v2.5.2）

### 发现问题

perf 火焰图第一个大头：**14.5% CPU 在 `scheduler::wake_one_thread_and_unlock` + `pthread_cond_signal`**。

一开始以为是跨线程调度问题，仔细一看不是——是 Boost.Asio scheduler **每次 co_await resume 都要走的内部调度流程**太重了。一个 Hello World 请求居然走了 **4 个协程帧**：

```
handleSession:
  co_await async_read           → 帧 1（必需，I/O 等待）
  co_await router_.dispatch()   → 帧 2（Router 本身是协程）
    co_await handler(req)       → 帧 3（同步 handler 被包装成协程，不必要！）
  co_await async_write          → 帧 4（必需，I/O 等待）
```

帧 1 和 4 是真正的 I/O 等待不可消除，但帧 2 和 3 完全是浪费——一个同步的 `return HttpResponse("Hello")` 被裹了两层协程。

### 解决方案：Router 同步快速路径

思路很直接——Router 同时存同步和异步两种 handler，加一个 `dispatchSync()` 方法：

- 同步 handler 注册时**保留原始函数**，不包装成协程
- `handleSession` 先试 `dispatchSync()`，返回 `optional<HttpResponse>` 就直接用
- 返回 `nullopt` 才 fallback 到 `co_await dispatch()`

顺手做了 `resolveRoute()` 统一查找，`dispatch()` 和 `dispatchSync()` 共用，砍掉 40 行重复代码。

### 延伸：SyncMiddleware 零协程帧中间件（v2.5.1）

中间件也有同样问题——10 层 sync before/after 包装成 10 个协程帧。设计了 `buildOptimizedChain()` 算法：连续 Sync 条目合并为单个协程帧，N 层同步中间件仅 1 次堆分配。

实测 10 层同步中间件仅比无中间件低 2.1%。

### 效果

协程帧从 4 降到 2，scheduler 调度开销降了一半。但这时 QPS 从 27K 提到约 46K，还远远不够。

---

## 阶段 2：多 io_context + SO_REUSEPORT（v2.5.1-v2.5.2）

### 发现问题

单 io_context 就一个 epoll，所有连接挤在一起，高并发时锁竞争严重。

### 解决方案

- **多 io_context**：1 线程 1 个 io_context，`EventLoopPool` round-robin 分连接
- **SO_REUSEPORT 多 acceptor**：每个 worker 自己 accept 自己处理，accept 和 I/O 同线程，零跨线程 post
- Windows 不支持 SO_REUSEPORT，自动回退单 acceptor

### 效果

QPS：46K → 132K（2.83x）。但距 Cinatra 165K 还差 ~20%。

---

## 阶段 3：彻底去除 Beast，自研 HTTP/WebSocket 栈（v2.6.0）

### 发现问题

132K 时的火焰图里框架代码已经几乎看不见了（<5%），剩余差距全在 **Beast 那套重量级 HTTP parser/serializer** 上。Beast 的 `http::request_parser` 和 `http::async_write` 模板展开一大堆，内存分配也多。而 Cinatra 用的是 picohttpparser——纯 C 写的，极致轻量。差距就在这里。

### 实施过程

这一步工程量最大，相当于把框架的网络底层重写了一遍。分 4 个阶段执行：

**Phase 1 — picohttpparser 替换 Beast HTTP parser：**
- 集成 picohttpparser（~800 行纯 C 代码）
- `NativeRequest` 改为零拷贝设计：`string_view` 引用连接级 `readBuf`，栈上 `RequestHeaders`（`array<Entry, 64>`），零堆分配
- CPU 占比从 10% → 0.08%

**Phase 2 — 自研 HTTP 响应序列化：**
- `NativeResponse::serialize()` 手工拼接 HTTP 响应
- `serializeHeadTo(FixedBuffer<512>&)` 零堆分配栈缓冲
- scatter-gather I/O（head + body 两段 buffer 一次 `async_write`）
- 200 OK 状态行预计算（1 次 append vs 7 次）

**Phase 3 — 自研 WebSocket 栈（RFC 6455）：**
- `WsFrame.h`：帧解析/构造
- `WsHandshake.h`：握手协议（自研 Base64 + OpenSSL SHA1）
- `WsDeflate.h/cpp`：permessage-deflate（pimpl 封装 zlib）
- 完整支持消息分片重组 + 控制帧穿插

**Phase 4 — 清理 Beast 依赖：**
- 13 个文件全部清理干净
- 自研测试客户端 `TestHttpClient.h` 替代 Beast HTTP/WS 客户端

### 踩坑记录

1. **readBuf 残留数据丢弃 BUG**：for 循环头部 `bufUsed = 0` 会丢弃上一请求 body 消费后 readBuf 中的残留数据。TCP 粘包/keep-alive 紧密发送时解析错误。修复：`bufUsed` 移到循环外，body 消费后 memmove 残留数据。

2. **NativeRequest 的 string_view 生命周期**：WebSocket 升级时 `handleWebSocket` 入口必须立即复制 `Sec-WebSocket-Key` 等头部值，因为原始 `readBuf` 后续会被覆盖。

3. **zlib 链接变化**：之前 Beast 内嵌 zlib，去掉 Beast 后需要显式 `find_package(ZLIB)`。

---

## 阶段 4：热路径微优化（v2.6.0）

### 7 项修复清单

去掉 Beast 后拿到 140K，还有一些微优化空间：

| #   | 问题                                   | 方案                                 | 收益             |
| --- | -------------------------------------- | ------------------------------------ | ---------------- |
| 1   | readBuf keep-alive 残留数据丢弃（BUG） | bufUsed 移到循环外 + memmove         | 正确性 + 性能    |
| 2   | FixedBuffer<4096> 大响应触发 heap      | 改 FixedBuffer<512> + scatter-gather | 减少堆分配       |
| 3   | shared_ptr<function> 自引用环 timer    | 改为协程 `idleTimerLoop`             | -2 次堆分配/连接 |
| 4   | serializeHeadTo 多次 append            | 200 OK 预计算字面量（1 次 vs 7 次）  | 减少分支         |
| 5   | 响应通用头 `set()` O(N) 查找           | 改 `insert()` O(1) push_back         | 消除循环         |
| 6   | header 解析循环每个头都做 iequals      | 长度+首字符快速过滤                  | 减少比较         |
| 7   | HeaderMap reserve(16) 过大             | 改 reserve(8)                        | 减少初始分配     |

### 连接级 Atomic 时间戳超时

把 per-request `steady_timer`（每请求 2 次 `epoll_ctl`）替换为 `atomic<int64_t> lastActiveTimeMs_` + 后台扫描协程。keep-alive 连接的 timer 系统调用从 2N 次降到 0 次。

设计细节：
- `alignas(64)` 独占 cache line 避免 false sharing
- `memory_order_relaxed`：x86 下编译为普通 MOV
- 扫描间隔 `max(1s, idleTimeout/4)`：空闲超时 60s 时每 15s 扫一次

---

## 阶段 5：减少写入开销（v2.6.0）

### 发现问题

140K QPS 时火焰图：
- **38%** 内核 `_raw_spin_unlock_irqrestore`（TCP sendmsg spin_lock）
- **<5%** 用户态框架代码

瓶颈已经从用户态转移到**内核态**了——syscall 太多。接下来要做的就是想办法少进内核。

### 解决方案：writeResponse 三档自适应

`writeResponse` 根据响应大小自动选路径：

1. **仅头部路径**（HEAD 方法或空 body）：单次 `async_write` 发送 `FixedBuffer<512>` 栈缓冲
2. **小响应合并路径**（head + body ≤ 512 字节）：头部和 body 合并到同一个 `FixedBuffer<512>`，单次 `async_write`，零堆分配
3. **大响应 scatter-gather 路径**：`std::array<const_buffer, 2>` 两段 buffer 一次 `async_write`（内核 writev），head 在栈上 + body 零拷贝引用

Hello World 这种小响应（~150 字节）走路径 2，全在栈上搞定，一次 syscall 完事。

> **注**：最初想过 `socket.write_some()` 同步写入快速路径，但没采用。Asio 的 `async_write` 在 reactor 模式下如果 socket 可写本身就直接完成了，再加一层同步尝试收益有限还增加复杂度。最终靠 parse-before-read（阶段 6）和 atomic 时间戳（阶段 4）来减少 syscall。

---

## 阶段 6：HTTP Pipelining 快速路径

### 发现问题

TFB plaintext pipeline 测试（wrk -c 1000 -t 8，16 pipeline），Hical 只跑出 82K QPS——跟 json 的 71K 差不多。Pipeline 完全没发挥作用。

根因很蠢：解析循环**总是先 `co_await async_read_some()` 再解析**。哪怕 bufUsed > 0（上一个请求消费完后 memmove 保留了后续请求的完整数据），也要先挂起协程等新数据。16 pipeline = 15 次白白的 syscall + 15 次协程挂起/恢复。

### 解决方案：parse-before-read

修复很简单——内层循环加个 `bufUsed > prevBufLen` 检查：缓冲区里已有的数据能不能解析出完整请求？能就直接 break，不能再读 socket。

```cpp
if (bufUsed > prevBufLen)
{
    parseResult = phr_parse_request(readBuf.data(), bufUsed, ..., prevBufLen);
    prevBufLen = bufUsed;
    if (parseResult > 0) break;  // 零 syscall 完成解析
}
```

16 pipeline 场景：15/16 的请求跳过 `async_read_some`。

---

## 总结：QPS 演进时间线

| 版本           | QPS         | 关键优化                                                              |
| -------------- | ----------- | --------------------------------------------------------------------- |
| v2.5.0（基线） | ~27K        | 无                                                                    |
| v2.5.1         | ~35K        | SyncMiddleware + 多 io_context                                        |
| v2.5.2         | ~46K → 132K | Router 同步快速路径 + SO_REUSEPORT + 编译防火墙                       |
| v2.6.0         | **159K**    | 去 Beast + picohttpparser + atomic 超时 + syscall 削减 + 热路径微优化 |

### 几个教训

**1. 先看火焰图，不要凭直觉优化。**

最初我以为瓶颈在路由查找或中间件链上，实际上首先该砍的是不必要的协程帧——一个看不见的开销。没有火焰图就不知道 14.5% CPU 花在了 Asio scheduler 的条件变量上。

**2. 大块头优化 > 微优化。**

去掉 Beast 带来的收益（132K → 140K+）看起来只有 6%，但它**解锁了后续所有微优化的空间**。Beast 的模板展开和内存分配把很多热路径优化都遮蔽了。

**3. "事件驱动一定优于轮询"是有前提的。**

连接级 atomic 时间戳替代 per-request timer，本质是用**粗粒度轮询替代精确事件驱动**。当精度要求（±15s）远低于事件机制的固有开销（`epoll_ctl` 系统调用）时，轮询反而更优。

**4. 同步快速路径是"廉价保险"。**

写入/读取的同步快速路径代码量极小（~30 行），但在高并发 keep-alive 场景下收益显著。核心思路：先试一下最可能成功的廉价操作，失败了再走昂贵的完整路径。

**5. 用户态开销优化到极致后，瓶颈转移到内核。**

140K 时 38% CPU 在内核 spin_lock。此时能做的就是**减少进内核的次数**——同步快速路径、parse-before-read、atomic 时间戳，本质都是在用户态解决问题、避免系统调用。

**6. Pipeline 优化的关键是"别主动阻塞"。**

parse-before-read 的思路极其简单——先看看缓冲区有没有数据，有就直接解析。但不加这个检查，16 pipeline 完全没有发挥作用。有时候"不做多余的事"就是最大的优化。

---

## 最终成绩

```
Hical v2.6.0:  159,000 QPS
Cinatra:       165,000 QPS
Drogon:        170,000 QPS
```

基本持平，差距 5% 以内。考虑到 Hical 还带着完整的中间件管道、WebSocket、OpenAPI、数据库中间件这一堆功能，这个成绩我觉得可以了。

---

## 后续优化（v2.6.1 — v2.6.2）

v2.6.0 之后陆续做了些打磨，没有数倍级 QPS 提升了，更多是稳定性修复和细节优化。

### v2.6.1：HTTP Date 头 thread_local 缓存

HTTP/1.1 规范要求响应带 `Date` 头，但 `strftime` / `gmtime` 每次调用都有开销。同一秒内的所有响应其实 Date 值是一样的——那就缓存起来呗。

`thread_local DateCache` 缓存当前秒的格式化结果，`time(nullptr)` 变了才重新格式化。

```cpp
struct DateCache
{
    time_t cachedSec {0};
    char buf[30] {};
    size_t len {0};
};
thread_local DateCache dateTlsCache;
```

单个调用省的不多，但 159K QPS 下一秒省 159K 次 `gmtime_r` + `strftime`，积少成多。

### v2.6.2：GenericConnection WriteEntry 去虚函数化

原来写队列是 `deque<shared_ptr<WriteNode>>`，`WriteNode` 多态基类带虚函数。最常见的内存数据写入每次要走三层间接：`shared_ptr → control block → WriteNode → vtable`。明明就是发个 string，搞这么复杂。

改成 `WriteEntry` 标签联合：

```cpp
struct WriteEntry
{
    enum class Type : uint8_t { hMemory, hNode };
    Type type;
    std::shared_ptr<std::string> memData; // hMemory 快速路径
    std::shared_ptr<WriteNode> node;      // hNode 慢路径（File 等）
};
```

内存快速路径直接内联 `shared_ptr<string>`，1 次解引用拿 buffer。File 写入走虚函数慢路径，反正低频无所谓。

顺便把 `GenericConnection` 字段按访问频率重排了，热字段靠前，`lastActiveTimeMs_` 用 `alignas(64)` 隔离（跟阶段 4 的 atomic 时间戳设计呼应）。

### v2.6.2：MemoryPool TrackedResource alignas(64)

内存池的分配统计有四个 atomic 计数器，多核并发分配时 false sharing 互相打架。

```cpp
struct alignas(64) AlignedCounter
{
    std::atomic<size_t> value {0};
};
AlignedCounter totalAllocations_;
AlignedCounter totalDeallocations_;
AlignedCounter currentBytes_;
AlignedCounter peakBytes_;
```

每个计数器 `alignas(64)` 独占一条 cache line。另外加了 `HICAL_ENABLE_MEMORY_TRACKING` 条件编译，生产环境可以直接关掉，零开销。

### v2.6.2：AsioEventLoop::stop() 数据竞争修复

TSan 检出来的——并发调用 `stop()` 时有数据竞争。修复很简洁：

```cpp
void AsioEventLoop::stop()
{
    if (quit_.exchange(true)) return;  // first-caller-wins
    workGuard_.reset();
    ioContext_.stop();
}
```

`exchange(true)` 一次性门卫，谁先到谁执行，后来的直接 return。同一版本还顺手修了 TSan 检出的其他竞争：`threadId_` 改 `std::atomic`、`AsioTimer::cancel()` post 到 executor 线程、`TcpServer::stop()` 的 acceptor 关闭 post 到 `baseLoop_` 线程。Asio 对象跨线程操作必须 post，这是铁律。

### v2.6.2：WebSocket 广播和写串行化

- `WsHub` 广播管理器上线：连接注册/移除、房间分组、`shared_mutex + weak_ptr` 广播
- `acquireWrite()` / `releaseWrite()` 协程互斥锁——之前 Ping 和消息并发 `async_write` 偶尔会 crash，加了这个就稳了

---

## 回顾：方案设计 vs 最终实现的差异

做完回头看，有几个当初方案设计时想过但最终没用上的东西：

| 设计阶段的想法                         | 最终决策 | 原因                                                                        |
| -------------------------------------- | -------- | --------------------------------------------------------------------------- |
| `socket.write_some()` 同步写入快速路径 | 未采用   | Asio reactor 已在内部优化，额外同步尝试收益有限                             |
| `socket.available()` 同步读取快速路径  | 未采用   | pipelining parse-before-read 已覆盖此场景（检查缓冲区比检查 socket 更直接） |
| 响应批量写入（write batching）         | 暂缓     | pipelining 方案中考虑过，但改变延迟特性，先观察快速路径效果                 |

教训：**想法 ≠ 实现**。很多方案设计阶段觉得很有道理的东西，实测后发现没必要。不是每个 idea 都值得写进代码的。

---

## 补充：各模块持续优化记录

以下优化不在 QPS 主线上，但属于框架整体性能工程的重要组成部分。按模块分组记录。

### 内存池（v2.5.1）

**请求池生命周期修复**

- **原问题**：`handleSession` 的 `monotonic_buffer_resource` 放在 `for(;;)` 循环外面。keep-alive 连接复用同一个session协程处理多个请求，而 monotonic pool 的 `deallocate()` 是空操作——分配出去的内存块永远不归还。连接上请求越多，内存占用单调递增，2x 扩容产生的废块也永远占位。长连接跑久了进程 RSS 只增不减，本质上就是慢性内存泄漏。
- **优化方案**：把 `monotonic_buffer_resource` 移进 `for(;;)` 循环内部，每轮请求结束 pool 析构，统一归还到 thread-local upstream。
- **优化后效果**：连接内存占用变成"请求结束即归零"，RSS 长期稳定。monotonic pool 的速度优势保留——单个请求内本来就是只分配不释放的模式。

### 日志系统（v2.3/v2.5）

**COW snapshot + 锁外格式化**

- **原问题**：v2.2 的 `Logger::emit()` 把格式化、写入、flush 全放在一把 mutex 里做完，临界区耗时 ~500ns-2μs。Hical 是 1 Thread : 1 io_context 模型，多 IO 线程并发写日志时直接互相卡死。每个请求至少一条 access log，QPS 一上来 mutex 竞争就成瓶颈。

```
v2.2: lock { format + write + flush }         临界区 ~500ns-2μs
v2.3: lock { snapshot vector<shared_ptr> }     临界区 ~80-160ns
v2.5: lock { COW shared_ptr atomic_inc × 2 }   临界区 ~10-20ns
```

- **优化方案**：v2.3 把格式化+写入移到锁外，锁内只做 vector snapshot；v2.5 进一步改 COW——Sink 列表存为 `shared_ptr<const vector<...>>`，`emit()` 加锁只拷贝一个 shared_ptr（2 次 atomic_inc）就解锁。增删 Sink 走 copy-on-write 重建。`LogChannel` 同理。
- **优化后效果**：临界区从 μs 级降到 ~10-20ns，多线程日志不再互相阻塞。access log 不再是吞吐瓶颈。

**trace-id 生成从 CSPRNG 改 thread_local PRNG**

- **原问题**：每个请求都要生成 trace-id，原来用 `RAND_bytes()`（~1-3μs/call），内部有全局 DRBG 锁。多线程一上来就互相等锁。但 trace-id 只要全局唯一就行，根本不需要密码学安全（Session ID 才需要）。
- **优化方案**：改 `thread_local std::mt19937_64`，种子 `random_device` 初始化，2 次 PRNG call 搞定（~10ns），零锁。
- **优化后效果**：DRBG 锁竞争彻底消除。128-bit 空间 + thread_local 保证唯一性没问题。

**TextFormatter 消除堆分配**

- **原问题**：`TextFormatter::format()` 用 `std::to_string()` 格式化 threadId 和行号，每次构造临时 `std::string`（堆分配 ~50-100ns/次）。每条日志白白 2 次 malloc/free。
- **优化方案**：改 `std::to_chars` + 栈上 `char[24]`缓冲区，直接 append 到输出 buffer，零堆分配。
- **优化后效果**：`format()` 路径彻底消除临时 string。配合 COW 优化，整个日志热路径零动态分配。

**AsyncFileSink flush 语义修正（v2.5）**

- **原问题**：`AsyncFileSink` 使用双缓冲异步写盘，`flush()` 的初始实现仅调用 `yield()` 唤醒后台线程，**但不等待后台线程实际完成写入就返回**。测试里 flush 完立刻断言文件内容会出竞态，程序退出时也可能丢日志。
- **优化方案**：`flush()` 改 promise/future 同步握手，后台线程写完后 fulfill，调用方才返回。
- **优化后效果**：`flush()` 返回 = 数据已落盘。平时 flush 极少被调用（只有 Fatal 和 shutdown），不影响吞吐。

### Session 与 Cookie（v2.0-v2.3）

**双层 `shared_mutex` 读写分离**

- **原问题**：Session 典型的读多写少——绝大多数请求只读 session（验证登录态、取用户信息），写的很少（登录、设偏好）。v1.x 用的普通 `mutex`，读也互相阻塞。多 IO 线程并发处理已登录用户时，session 锁直接成串行瓶颈。
- **优化方案**：
  - v2.0：`SessionManager` 从 `mutex` 改为 `shared_mutex`，`find()` 用 `shared_lock` 实现读并发
  - v2.1：`Session` 对象自身也改为 `shared_mutex`，`get()`/`has()`/`isDirty()` 等只读方法用 `shared_lock`
- **优化后效果**：并发只读零阻塞。写操作（`set()`/`remove()`）仍走 `unique_lock` 保证一致性，但写本来就少。

**`any_cast` 异常路径消除**

- **原问题**：`Session::get<T>()` 和 `HttpRequest::getAttribute<T>()` 靠 `any_cast` 抛异常来判断类型不匹配。但实际使用中类型不匹配是正常控制流（中间件不一定设置了某属性），异常展开代价 ~1-10μs，拿来当 if/else 用太贵了。
- **优化方案**：先 `any::type()` 和 `typeid(T)` 比一下，匹配了再 cast（保证不抛），不匹配直接返回 `nullopt`。
- **优化后效果**：正常控制流不再走异常路径。中间件链里频繁 getAttribute（检查 DB 连接、trace-id 等），开销忽略不计。

**Cookie/queryParam/formParam 透明哈希（v2.3）**

- **原问题**：`cookies_`/`queryParams_`/`formParams_` 用标准 `unordered_map<string, string>`，`find()` 只接受 `const string&`。调用方拿着 `string_view` 每次都要先构造一个临时 `string` 才能查——纯浪费（~50-100ns/次）。
- **优化方案**：改透明哈希（`StringHash` + `is_transparent`），`find(string_view)` 直接查。方法签名也全改 `string_view`。
- **优化后效果**：所有参数查找操作实现零临时对象构造，`find(string_view)` 直接查找。每次省一次 malloc/free。

**`setCookie()` 去 ostringstream**

- **原问题**：`setCookie()` 用 `ostringstream` 拼 Set-Cookie 头。ostringstream 构造析构一整套（locale 查找、MSVC 上还有锁），一个响应设多个 cookie 就累积起来了。
- **优化方案**：改 `string::reserve()` + `append()`，特殊字符查表 hex 编码。
- **优化后效果**：干掉 ostringstream 的全部开销。

### 静态文件服务（v2.1）

**PathCache LRU 重构**

- **原问题**：`PathCache`（TTL 60s，上限 4096 条目）原来用 `unordered_map` + 过期优先驱逐。缓存满的时候遍历整个 map 找过期条目，O(N)。持着写锁扫几千条目，其他静态文件请求全排队等着。
- **优化方案**：经典 LRU 实现——`list<CacheEntry>`（双向链表记录访问顺序） + `unordered_map<string, list::iterator>`，（O(1) 查找 + O(1) 驱逐末尾元素）。
- **优化后效果**：驱逐 O(1)，命中路径 shared_lock + map 查找 + splice。写锁持有时间大幅缩短。

### CORS 中间件（v2.2）

**预计算不变量**

- **原问题**：每次 preflight 请求都动态 `joinStrings()` 拼 methods/headers 列表，`to_string()` 转 max-age。但这些值配置确定后就不会变了，每次重新拼纯属浪费。
- **优化方案**：`makeCorsMiddleware()` 的 lambda 捕获时一次性算好 `methodsStr`/`headersStr`/`maxAgeStr`/`exposeHeadersStr`，运行时直接用。
- **优化后效果**：preflight 路径零字符串拼接、零动态分配，直接 set header 完事。

### 路由系统（v2.0-v2.5.2）

除主线已记录的同步快速路径外，还有一系列零分配优化：

**参数路由零分配匹配**

- **原问题**：参数路由匹配时（复杂度 O(N_per_method × M)），原实现每试一条路由都先把路径段构造成 `std::string` 再比——不匹配的那些全白分配了。
- **优化方案**：
  - 匹配过程全程使用 `string_view`，仅确认完全匹配成功后才 `emplace_back` 拥有语义的 string 到参数列表
  - `ParamList` 提到循环外复用底层 vector 内存（匹配失败 `clear()` 不释放 capacity）
  - `findWsRoute()` 从 `const string&` 改为 `string_view` 参数
  - 405 检测加 `staticPathMethods_` 反向索引，方法不允许时 O(1) 判断（避免遍历所有路由）
- **优化后效果**：匹配路径零堆分配，只有最终命中了才 emplace_back 一份拥有所有权的 string。

### 数据库连接池（v2.1-v2.5）

**DbResult 列名分离式存储**

- **原问题**：`DbResult` 原来是 `vector<unordered_map<string, string>>`，每行一个 map。1000 行 × 10 列，列名 "id"/"name"/"email" 重复存 1000 遍，每个 map 还有 bucket 数组分配，总计约 31,000 次堆分配。
- **优化方案**：改为 `columns`（`vector<string>`，列名仅存一份）+ `rows`（`vector<vector<string>>`，按行×列索引）分离设计。提供 `columnIndex(name)` 按名查找列号。数字类型用 `std::to_chars` + 栈缓冲避免 `to_string()` 堆分配（v2.5）。
- **优化后效果**：1000 行 × 10 列场景堆分配从 ~31,000 次降为 ~11,000 次（-65%）。内存占用减少（列名不重复）。`columnIndex()` 保留了按名访问的便利性。

**StmtCache 透明哈希**

- **原问题**：StmtCache 的 map key 是 `std::string`，但调用方通常拿的是 `string_view`。每次查缓存都要先构造临时 string，命中率又高（同一 SQL 反复执行），等于绝大多数情况这个分配都是白费。
- **优化方案**：map 改用 `StringHash`（`is_transparent`）+ `StringEqual`，支持 `string_view` 直接查找。
- **优化后效果**：缓存命中路径零堆分配，find 命中仅需 ~50ns（透明哈希 O(1) + list splice O(1)）。

**健康检查计数器修复**

- **原问题**：连接池后台 `healthCheckLoop` 定期将空闲连接取出做 ping 时，这些连接既不在 idle 队列也不在 active 计数里。高并发 `acquire()` 时池子算出来还有容量就新建连接，实际数超过 `maxConnections`，可能触发 MySQL 拒绝。
- **优化方案**：取出检查时计入 `m_activeCount`，归还时减回。
- **优化后效果**：任何时刻都精确追踪连接数，`acquire()` 不会超发。

### WebSocket（v2.6.2-v2.6.3）

**写串行化**

- **原问题**：`sendFrame()` 可能被多个协程并发调用（消息循环的 `send()` 和 `wsPingLoop` 的 `sendPing()` 同时来）。两个 `async_write` 交错的话 TCP 流上帧就坏了，对面解析出错直接断连。
- **优化方案**：基于 `steady_timer::cancel_one` 做协程互斥锁（无竞争时 timer 不 arm，零开销）。v2.6.3 改 RAII `WriteGuard`。
- **优化后效果**：并发写自动串行化。无竞争 ~0ns（`m_writePending=false` 直接过），有竞争一次 `async_wait`。WriteGuard 保证异常也能释放锁。

**WsHub 广播 cache 友好设计（v2.6.2）**

- **原问题**：room 成员存 `unordered_set<WsConnectionId>`，广播时遍历 hash set 内存访问随机跳（bucket 指针链），cache 不友好。而且每个成员都要回 `m_connections` map 查一次 WsConnection 指针，N 人房间 = N 次 hash map 查找。
- **优化方案**：room 改 `vector<RoomMember>`，冗余存 `weak_ptr<WsConnection>`。广播顺序遍历 vector，直接 `lock()` 拿连接，零 map 查找。
- **优化后效果**：顺序内存访问 cache 友好，消除 N 次 map 查找。`weak_ptr` 自动跳过已断开的连接。

**WsHub 透明哈希（v2.6.3）**

- **原问题**：`m_rooms` 和 `ConnectionEntry::rooms` 用普通 `unordered_map/set<string>`，调用方传的是 `string_view`，每次 join/leave/broadcast 都要先构造临时 string。`sendTo()` 单目标发送还用了 `make_shared<string>` 共享所有权——只有一个接收者，共享个啥。
- **优化方案**：容器全改透明哈希，参数全改 `string_view`。`sendTo()` 单目标直接 move string 进 lambda。
- **优化后效果**：room 操作零临时 string。单目标发送省一次 `make_shared`（控制块 ~32 字节分配+原子计数器）。

**`receiveInternal()` 共用内核（v2.6.3）**

- **原问题**：`receive()` 和 `receiveMessage()` 有 ~200 行几乎一样的代码（帧解析、mask、控制帧处理等）。改一处忘另一处，而且两份相同机器码占 icache。
- **优化方案**：提取 `receiveInternal()` 共用内核，两个 API 传不同的分片策略进去。
- **优化后效果**：代码量减半，维护统一，icache 占用减少。

**零拷贝 close 帧回复（v2.6.3）**

- **原问题**：回复 close 帧时，把 `m_readBuf` 里已经 unmask 好的 payload 拷贝到新 `std::string` 再传给 `sendCloseFrame()`。数据明明就在 readBuf 里，close 又是连接最后一个操作，完全没必要拷贝。
- **优化方案**：直接传 `string_view` 引用 readBuf 中的载荷。
- **优化后效果**：close 握手零额外分配。低频操作，但大量短连接批量断开时有体感。

**`computeWsAcceptKey()` 栈缓冲（v2.6.3）**

- **原问题**：WS 握手要把 clientKey（24B）+ GUID（36B）拼起来做 SHA-1。用 `std::string` 拼的话总长 60 字节超过 SSO（MSVC 15B），每次握手必堆分配。
- **优化方案**：栈上 `char[64]` + `memcpy`，直接传给 SHA-1。
- **优化后效果**：握手路径省 1 次 malloc/free。大量短连接场景下有意义。

**子协议协商去 vector（v2.6.3）**

- **原问题**：子协议协商先把客户端 offer 解析到 `vector<string_view>` 再嵌套匹配。大多数应用只配 1-2 个协议，建个 vector 再匹配有点杀鸡用牛刀。
- **优化方案**：边解析边匹配，每解析出一个 token 立即比对，命中就返回。
- **优化后效果**：零堆分配，典型场景第一个 token 就命中，后面都不用解析了。

### Multipart 解析（v2.5）

**boundary 搜索算法**

- **原问题**：boundary 搜索用 `string_view::find()`，朴素算法最坏 O(n×m)。攻击者构造特殊 body（全是 boundary 前缀重复）就能触发二次复杂度，算法复杂度 DoS。
- **优化方案**：改 `std::boyer_moore_horspool_searcher`，预处理跳转表后搜索 O(n)，跟 body 内容无关。
- **优化后效果**：最坏从 O(n×m) 降为 O(n)，堵死复杂度攻击。正常请求也略快（跳转表能跳过更多字符）。

**惰性缓存**

- **原问题**：`getFile(req)` / `getField(req)` 每次调用都重新解析整个 multipart body。5 个字段就解析 5 遍同样的 body，大文件时尤其浪费。
- **优化方案**：`cachedParse()` 首次解析后缓存到 `HttpRequest::m_cachedMultipartParts`，后续复用。
- **优化后效果**：N 次字段访问只解析 1 次。5 个字段 = 开销降到 1/5。

### 中间件管道（v2.5.2-v2.6.2）

**去除冗余存储**

- **原问题**：重构过程中留下了历史债——`MiddlewarePipeline` 同时有 `middlewares_`、`middlewareNames_` 和 `entries_` 三份存储，`RouteGroup` 也有类似的 `m_middlewares`。新的 `entries_` 已经包含全部信息了，旧字段纯属冗余。
- **优化方案**：砍掉所有旧字段，统一用 `entries_`。`use()` 直接 move 进去。
- **优化后效果**：内存占用减少，代码更干净。

**MiddlewareTimingStats 缓存行优化（v2.6.2）**

- **原问题**：`MiddlewareTimingStats` 4 个 atomic 挤在一起（~64B），多 IO 线程并发 `record()` 时 false sharing——写一个计数器把别的核心 cache line 也失效了。
- **优化方案**：每个 atomic `alignas(64)` 独占 cache line，`name` 移末尾（冷数据）。
- **优化后效果**：并发 record 无 false sharing。代价从 ~64B 涨到 ~320B，但中间件就 5-10 个，无所谓。

### 编译期优化

**MetaJsonError 非模板外提（v2.5.2）**

- **原问题**：`fromJson<T>()` 的 throw 带格式化逻辑，每个 `HICAL_JSON(Type, ...)` 实例化都复制一份错误处理代码。50 个 JSON 类型 = 50 份几乎一样的 throw 代码，挤占 icache，热路径和冷路径代码混在一起。
- **优化方案**：错误函数提取到独立 `.cpp`，标 `[[noreturn]]`。编译器会把调用点优化成 unconditional jump，不保存寄存器。
- **优化后效果**：错误处理代码全局就一份，每个实例化体积减小，icache 利用率提升。

**GenericConnection 编译防火墙**

- **原问题**：`GenericConnection` ~780 行模板实现，include 它的所有翻译单元改一行都要重编译整个模板。test 文件改个注释也要等它编完。
- **优化方案**：实现移到 `.hci` 文件，header 只留声明 + `extern template`。显式实例化集中在 `GenericConnection.cpp`。
- **优化后效果**：用户代码改动不再触发连接模板重编译，增量编译快多了。

---

## 下一步方向

如果要继续压榨：
- **io_uring**（Linux 5.1+）：用 submission queue 批量提交 I/O，彻底消除 epoll_ctl
- **内存池进一步优化**：请求级 monotonic buffer 的 upstream 切换策略
- **mimalloc 集成**：替换系统 malloc，对小对象频繁分配/释放场景可能有显著收益
- **连接表分片**：per-loop 独立连接集合，idle 扫描无需全局锁

---

如果这篇文章对你有帮助，欢迎去 GitHub 给 Hical 点个 Star 支持一下：

👉 [https://github.com/Hical61/Hical.git](https://github.com/Hical61/Hical.git)
