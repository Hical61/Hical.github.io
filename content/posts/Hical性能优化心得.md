+++
title = 'Hical v2.6.0 性能优化心得：从 27K 到 159K QPS 的完整旅程'
date = '2026-05-11'
draft = false
tags = ["C++20", "性能优化", "Hical", "火焰图", "perf", "picohttpparser"]
categories = ["性能优化"]
description = "记录 Hical 从 27K 到 159K QPS 的完整优化历程：调度模型重构、去 Beast 自研 HTTP 栈、热路径微优化，以及走过的弯路。"
+++

# Hical v2.6.0 性能优化心得：从 27K 到 159K QPS 的完整旅程

> 这篇文章记录了 Hical 从 v2.5.2 到 v2.6.0 的完整性能优化历程。不是罗列"我做了什么改动"，而是分享**怎么发现问题、怎么思考方案、怎么验证效果**——以及那些"看起来应该有用但实际没用"的弯路。希望对做 C++ 高性能服务器开发的同学有参考价值。

---

## 目录

- [Hical v2.6.0 性能优化心得：从 27K 到 159K QPS 的完整旅程](#hical-v260-性能优化心得从-27k-到-159k-qps-的完整旅程)
  - [目录](#目录)
  - [1. 起点：27K QPS，差距 6 倍](#1-起点27k-qps差距-6-倍)
  - [2. 第一个教训：不要猜，要量](#2-第一个教训不要猜要量)
  - [3. 找对方向：火焰图告诉你真相](#3-找对方向火焰图告诉你真相)
  - [4. 三阶段优化路线](#4-三阶段优化路线)
  - [5. 阶段一：调度模型重构（27K → 132K）](#5-阶段一调度模型重构27k--132k)
    - [5.1 SO\_REUSEPORT：消除跨线程调度](#51-so_reuseport消除跨线程调度)
    - [5.2 连接级 Timer + atomic 时间戳](#52-连接级-timer--atomic-时间戳)
    - [5.3 结果](#53-结果)
  - [6. 阶段二：去 Beast，自研 HTTP/WS 栈（132K → 140K）](#6-阶段二去-beast自研-httpws-栈132k--140k)
    - [6.1 四个 Phase](#61-四个-phase)
    - [6.2 零拷贝请求解析](#62-零拷贝请求解析)
    - [6.3 结果](#63-结果)
  - [7. 阶段三：热路径微优化（140K → 159K）](#7-阶段三热路径微优化140k--159k)
    - [7.1 修复 readBuf 残留数据丢弃（功能 BUG + 性能）](#71-修复-readbuf-残留数据丢弃功能-bug--性能)
    - [7.2 scatter-gather I/O 替代单 buffer 合并](#72-scatter-gather-io-替代单-buffer-合并)
    - [7.3 其他微优化（含后续延迟分配优化）](#73-其他微优化含后续延迟分配优化)
    - [7.4 结果](#74-结果)
  - [8. 最终火焰图：确认优化到位](#8-最终火焰图确认优化到位)
  - [9. 走过的弯路](#9-走过的弯路)
    - [弯路 1：优化不是瓶颈的代码](#弯路-1优化不是瓶颈的代码)
    - [弯路 2：FixedBuffer 栈缓冲区太大](#弯路-2fixedbuffer-栈缓冲区太大)
    - [弯路 3：过早放弃](#弯路-3过早放弃)
  - [10. 总结：性能优化的方法论](#10-总结性能优化的方法论)
    - [原则一：Profiling 驱动，不靠直觉](#原则一profiling-驱动不靠直觉)
    - [原则二：按占比排序，从大到小](#原则二按占比排序从大到小)
    - [原则三：每步验证，不要积累](#原则三每步验证不要积累)
    - [原则四：知道何时停手](#原则四知道何时停手)
    - [最终成绩单](#最终成绩单)

---

## 1. 起点：27K QPS，差距 6 倍

v2.5.1 的 Hical 在 Docker 环境（Ubuntu 24.04, GCC 14, 4 线程）下跑 Hello World benchmark，wrk 报出 **~27K QPS**。

同样的环境下：
- Cinatra：~165K QPS
- Drogon：~160K QPS

差距 **6 倍**。作为一个使用 Boost.Asio 协程 + Beast HTTP 的框架，这个数字让人很不甘心。

但差距就是差距，光靠"感觉哪里慢"是没法优化的。

---

## 2. 第一个教训：不要猜，要量

拿到 27K 这个数字后，我的第一反应是优化 Router。因为路由分发每请求都走，直觉上应该是热点。

于是实现了 `Router::dispatchSync()`——当 handler 是同步注册时，跳过协程帧分配，直接调用返回。理论上每请求省 ~40-130ns。

结果：

| 版本                | QPS    |
| ------------------- | ------ |
| v2.5.1 基线         | 27,493 |
| v2.5.2 dispatchSync | 20,940 |

**零提升**，甚至因为系统波动看起来还降了。

事后看火焰图才知道，`Router::dispatch` 仅占 **0.24%** CPU。就算优化到零开销，也只能提升 0.24%。这是一个重要的教训：

> **性能优化的第一步永远是测量，不是猜测。** 一个占 0.24% 的函数，即使优化 100%，对总体也毫无感知。

---

## 3. 找对方向：火焰图告诉你真相

放下直觉，用 `perf record` + [FlameGraph](https://github.com/brendangregg/FlameGraph) 做了完整的 CPU profiling。

火焰图揭示的真相：

| 热点                                    | CPU 占比 | 性质                 |
| --------------------------------------- | -------- | -------------------- |
| `sendmsg`（内核 socket 发送）           | 53.8%    | 不可优化（I/O 本身） |
| `epoll_ctl`                             | 12.5%    | **可优化**           |
| `scheduler::wake_one_thread_and_unlock` | 9.0%     | **可优化**           |
| `pthread_cond_signal`                   | 5.5%     | **可优化**           |
| Hical 框架代码                          | <2%      | 不是瓶颈             |

答案很清楚：**框架代码不是瓶颈，Boost.Asio 的调度模型才是。** 27% 的 CPU 花在了 epoll 事件注册和跨线程唤醒上。

这意味着无论怎么优化 Router、Middleware、序列化，QPS 都不会有实质提升。必须从更底层的调度模型入手。

---

## 4. 三阶段优化路线

基于火焰图分析，制定了三阶段路线：

```
阶段一：调度模型重构 ← 解决 27% 的 Asio 调度开销
  ├── SO_REUSEPORT 消除跨线程 accept 分发
  └── 连接级 atomic 时间戳消除 per-request timer

阶段二：去 Beast ← 解决 ~10% 的 Beast parser/serializer 开销
  ├── picohttpparser 替代 Beast HTTP parser
  ├── 自研 NativeResponse 序列化
  └── 自研 WebSocket 帧协议

阶段三：热路径微优化 ← 榨取最后几个百分点
  ├── scatter-gather I/O
  ├── 200 OK 快速路径
  └── 各种减少分配/比较的细节优化
```

每个阶段完成后都重跑 benchmark + 火焰图验证，确保优化方向正确。

---

## 5. 阶段一：调度模型重构（27K → 132K）

### 5.1 SO_REUSEPORT：消除跨线程调度

**问题**：原来的单 acceptor 模型中，baseLoop 线程 accept 后通过 `co_spawn(workerIoCtx, ...)` 将 socket 分发到 worker 线程。每次分发触发：

1. `eventfd_write(1)` 唤醒 worker 的 `epoll_wait`
2. socket fd 从 baseLoop 的 epoll 迁移到 worker 的 epoll（`EPOLL_CTL_ADD`）
3. 跨核 cache line bouncing（MESI 协议）

火焰图中这三项合计 **14.5%** CPU。

**方案**：每个 worker loop 持有独立的 acceptor，设置 `SO_REUSEPORT`，内核自动均衡分发连接。Accept 和 I/O 在同一线程完成，**零跨线程调度**。

```cpp
// Linux/macOS：SO_REUSEPORT 多 acceptor
for (auto* loop : allLoops)
{
    auto acc = std::make_unique<tcp::acceptor>(loop->getIoContext());
    acc->set_option(reuse_port(true));
    acc->bind(endpoint);
    acc->listen();
    acceptors_.push_back(std::move(acc));
}

// 每个 acceptor 在自己的 loop 上运行 acceptLoop
// accept 到的 socket 天然在当前线程，零 post
```

Windows 不支持 SO_REUSEPORT，自动回退到单 acceptor + 跨线程分发（但借鉴 Cinatra 策略：socket 创建在目标 worker 的 io_context 上，减少迁移开销）。

### 5.2 连接级 Timer + atomic 时间戳

**问题**：每个 HTTP 请求处理前后都调用 `timer.expires_after()` + `timer.cancel()`，每次调用触发 `epoll_ctl(EPOLL_CTL_MOD)` 修改 timerfd 的超时时间。Keep-alive 连接上 100 个请求 = 200 次 `epoll_ctl`。

火焰图中 `epoll_ctl` 占 **12.5%** CPU。

**方案**：timer 每连接只启动一次，以协程形式循环检查 atomic 时间戳：

```cpp
// 请求处理只更新原子时间戳（零系统调用）
lastActiveMs->store(now_ms, std::memory_order_relaxed);

// 独立协程循环检查
static Awaitable<void> idleTimerLoop(timer, socket, alive, lastActive, timeoutMs)
{
    while (alive->load())
    {
        timer.expires_after(timeoutMs);
        co_await timer.async_wait(redirect_error(...));
        if (now - lastActive >= timeoutMs) { socket.close(); break; }
        // 有活动：继续循环（协程自然续期）
    }
}
```

100 个 keep-alive 请求：原来 200 次 `epoll_ctl` → 现在 **0 次**（只有 timer 到期检查时才有系统调用）。

### 5.3 结果

| 版本                 | QPS        | 变化      |
| -------------------- | ---------- | --------- |
| v2.5.2 基线          | ~46K（VM） | —         |
| SO_REUSEPORT + Timer | **~132K**  | **+183%** |

延迟从 3.03ms 降到 1.72ms（**-43%**）。单纯靠调度模型优化就获得了近 3 倍提升。

---

## 6. 阶段二：去 Beast，自研 HTTP/WS 栈（132K → 140K）

132K 已经很不错，但火焰图显示 Beast 的 HTTP parser 仍占约 10% CPU。Cinatra 用的是 [picohttpparser](https://github.com/h2o/picohttpparser)（H2O 出品的 C 库），纯手工优化的 HTTP/1.1 解析器，极其轻量。

这一步是工程量最大的改动——**完全移除 Boost.Beast 依赖**。

### 6.1 四个 Phase

| Phase | 内容                                  | 关键设计                                                            |
| ----- | ------------------------------------- | ------------------------------------------------------------------- |
| 1     | picohttpparser 替代 Beast HTTP parser | 零拷贝 `NativeRequest`：headers 用 `string_view` 引用连接级 readBuf |
| 2     | 自研 `NativeResponse` 序列化          | `FixedBuffer` 栈缓冲区 + `std::to_chars` 设置 Content-Length        |
| 3     | 自研 WebSocket 帧协议（RFC 6455）     | 手写帧解析/构造 + permessage-deflate 压缩                           |
| 4     | 清理所有 Beast include                | 去除 `<boost/beast.hpp>`，`native()` 返回自研类型                   |

### 6.2 零拷贝请求解析

这是性能收益最大的设计决策。Beast 的 `request_parser` 会将所有头部解析到自有的 `fields` 容器中（涉及字符串拷贝和堆分配）。我们的方案：

```cpp
struct NativeRequest
{
    HttpMethod method;
    std::string_view target;        // 零拷贝，指向 readBuf
    RequestHeaders headers;          // 栈上 array<Entry, 64>，零堆分配
    std::string body;                // 仅 POST/PUT 时分配
};

// RequestHeaders：64 个 Entry 的栈数组
// 每个 Entry = 2 个 string_view = 32 字节
// 总计 2KB 栈空间，零 malloc
class RequestHeaders
{
    std::array<Entry, 64> entries_;
    size_t size_ = 0;
};
```

连接级 `readBuf`（8KB `std::string`）跨 keep-alive 请求复用。picohttpparser 直接在 readBuf 上原地解析，输出的 `phr_header` 数组包含指向 readBuf 的指针，我们直接转为 `string_view` 存入 `RequestHeaders`。**整个请求头解析过程零堆分配**。

### 6.3 结果

```
Beast HTTP parser: ~10% CPU  →  picohttpparser: 0.08% CPU  (-99%)
QPS: ~132K  →  ~140K (Docker)
```

提升幅度不如阶段一大，因为 132K 时框架代码已经只占 ~5%，parser 在其中又只是一部分。但去掉 Beast 还有一个重要的副作用：**编译速度大幅提升，二进制体积显著减小**（Beast 是重模板库）。

---

## 7. 阶段三：热路径微优化（140K → 159K）

去掉 Beast 后重新做火焰图，发现框架代码约 4.5% CPU。虽然每个函数占比很小，但累加起来仍有优化空间。这一阶段做了 7 项微优化：

### 7.1 修复 readBuf 残留数据丢弃（功能 BUG + 性能）

这是代码审查中发现的一个**正确性问题**。for 循环头部 `bufUsed = 0` 丢弃了上一请求 body 消费后 readBuf 中的残留数据。TCP 粘包场景下（客户端紧密发送 keep-alive 请求），下一个请求的头部数据可能已经在 readBuf 中但被丢弃，导致解析错误。

修复：`bufUsed` 移到 for 循环外，Content-Length / Chunked / 无 body 三条路径都正确保留残留数据。

### 7.2 scatter-gather I/O 替代单 buffer 合并

原实现将响应头和 body 合并到一个 `FixedBuffer<4096>` 中。当 head + body > 4KB 时，FixedBuffer 触发堆 fallback，导致 body 被完整拷贝一次。

改为三路径分发：

```cpp
if (skipBody || body.empty())
    // HEAD/空 body：仅发送 headBuf
    async_write(socket, buffer(headBuf));
else if (headBuf.size() + body.size() <= 512)
    // 小响应：合并到栈缓冲区，零堆分配
    headBuf.append(body);
    async_write(socket, buffer(headBuf));
else
    // 大响应：scatter-gather，零 body 拷贝
    async_write(socket, {buffer(headBuf), buffer(body)});
```

`FixedBuffer` 从 4096 降到 512（响应头通常 150-300 字节）。小响应在栈上完成，大响应用 `writev` 零拷贝。

### 7.3 其他微优化（含后续延迟分配优化）

| 优化                                                | 原开销                             | 优化后                                           |
| --------------------------------------------------- | ---------------------------------- | ------------------------------------------------ |
| Timer 回调链 `shared_ptr<function>` 自引用环        | 每连接 2 次堆分配                  | 协程 `idleTimerLoop()`，零额外堆分配             |
| 200 OK 状态行                                       | 7 次 FixedBuffer append            | 1 次 append（预计算字面量）                      |
| Server/Connection 头 `set()` O(N) 扫描              | 每请求 2 次线性扫描                | `insert()` O(1) 直接 push_back                   |
| header 解析中 Content-Length/Transfer-Encoding 检测 | 每 header 2 次 iequals             | 按长度+首字符快速过滤，~2 次 iequals             |
| HeaderMap 默认构造 `reserve(8)`                     | 每次构造 512 字节堆分配            | 延迟 reserve：默认构造零分配，首次写入时 reserve |
| HttpRequest::attributes_ 默认构造                   | 每请求 unordered_map bucket 堆分配 | unique_ptr 按需创建，热路径零开销                |
| `boost::asio::co_spawn` + `detached` 散落各处       | 协程异常被静默吞掉                 | 统一 `hical::coSpawn`，异常输出到 stderr         |

最后两项（HeaderMap 延迟 reserve、attributes_ 延迟构造）是火焰图中 0.61%/0.64% 的热点，分析后确认热路径上存在不必要的堆分配。`HeaderMap` 默认构造函数中 `entries_.reserve(8)` 会触发 512 字节堆分配，但 `HttpRequest::m_ownedHeaders` 在 `fromParsed()` 热路径下从未使用，白白浪费。改为默认构造不分配，首次 `set()`/`insert()` 时自动 reserve。`unordered_map<string, any>` 默认构造即分配 bucket array，但纯 HTTP 请求不调 `setAttribute()`，改为 `unique_ptr` 按需创建。

`coSpawn` 收敛不是性能优化，而是**可靠性改进**：原来 9 处 `boost::asio::co_spawn` 使用 `detached` 完成 token，协程内未捕获异常会被静默吞掉，导致连接泄漏等问题在生产环境中无从追踪。统一走 `hical::coSpawn()` 后，未捕获异常输出到 stderr。同时扩展了 `coSpawn` 支持任意 executor 参数（`requires` 约束排除 `io_context&` 重载歧义），覆盖了 `socketExecutor()`、`co_await this_coro::executor` 等调用场景。

### 7.4 结果

```
QPS: ~140K (Docker)  →  ~159K (VM 一体化测试)
```

7 项微优化累计 **+14%**。单独每项 1-3%，但叠加效果显著。

分离模式（server/wrk 独立容器，60s 持续时间）全场景数据进一步验证了优化效果：

| 场景                   | QPS (分离模式) | 说明                           |
| ---------------------- | -------------- | ------------------------------ |
| Hello World (c=100)    | 148K           | Docker bridge 网络有额外延迟   |
| JSON API (c=100)       | 156K           | JSON 序列化开销极低            |
| POST JSON Echo (c=100) | 129K           | 反序列化+序列化仍然高效        |
| 0 层中间件             | 161K           | 无中间件基线                   |
| 10 层异步中间件        | 91K            | 每层 ~0.43us 开销              |
| **10 层同步中间件**    | **158K**       | **仅 -2.1%，接近零开销**       |
| 高并发 c=1000          | 155K           | 仅比 c=100 下降 2.5%，非常稳定 |

---

## 8. 最终火焰图：确认优化到位

v2.6.0 最终火焰图（VM 直接测试，159K QPS）：

| 类别               | CPU 占比  |
| ------------------ | --------- |
| 内核 TCP 发送/接收 | ~55%      |
| 网络软中断         | ~10-15%   |
| Boost.Asio reactor | ~8-12%    |
| 调度器             | ~5-8%     |
| **Hical 框架代码** | **~4.5%** |

框架代码明细（前 5 名）：

| 函数                         | 占比      | 说明                    |
| ---------------------------- | --------- | ----------------------- |
| `handleSession`              | 1.20%     | 会话循环                |
| `writeResponse`              | 0.77%     | scatter-gather 响应写入 |
| `HttpRequest::HttpRequest`   | 0.64%     | 请求构造                |
| `HttpResponse::HttpResponse` | 0.61%     | 响应构造                |
| `HttpRequest::fromParsed`    | 0.58%     | NativeRequest 移动      |
| `phr_parse_request`          | **0.08%** | HTTP 解析（几乎不可见） |

**结论**：框架代码已优化到极限。即使全部消除也只能再提升 ~4.7%。进一步 QPS 提升需要 io_uring 替代 epoll。

---

## 9. 走过的弯路

### 弯路 1：优化不是瓶颈的代码

Router 同步快速路径就是典型。"每请求都走路由分发，所以路由一定很慢"——这种直觉是错的。0.24% CPU 的函数，优化到极致也无法被感知。

**教训**：永远先看火焰图，找到 >5% CPU 的热点再动手。

### 弯路 2：FixedBuffer 栈缓冲区太大

最初 writeResponse 用 `FixedBuffer<4096>` 把 head + body 全部塞进去。对 "Hello, World!" 这种 150 字节的响应完美运行。但对稍大的 JSON 响应（>4KB），反而触发了堆 fallback，比不用 FixedBuffer 还慢。

**教训**：栈缓冲区的大小要和实际数据匹配。头部用 512 就够了，body 不该往栈上放。

### 弯路 3：过早放弃

132K 时看到框架代码只占 ~5%，一度以为"已经优化到头了"。但实际上去 Beast + 微优化又额外榨出了 20%。在高 QPS 场景下，每个 1-2% 的优化叠加起来效果显著。

**教训**：单项 <3% 不代表没价值，7 项叠加就是 14%。

---

## 10. 总结：性能优化的方法论

回顾整个优化过程，总结几条方法论：

### 原则一：Profiling 驱动，不靠直觉

每一步优化都应该由 profiling 数据驱动。perf + 火焰图是最有效的工具组合。没有数据支持的"优化"可能是负优化。

### 原则二：按占比排序，从大到小

| 阶段             | 目标占比                 | QPS 提升  |
| ---------------- | ------------------------ | --------- |
| 调度模型（27%）  | 14.5% + 12.5%            | **+183%** |
| 去 Beast（~10%） | HTTP parser + serializer | **+6%**   |
| 微优化（~4.5%）  | 多个 0.1%-0.7% 的函数    | **+14%**  |

占比最大的先做，收益最高。占比 <1% 的函数留到最后批量做。

### 原则三：每步验证，不要积累

每完成一项优化就重跑 benchmark + 火焰图。如果某项优化没有预期效果（如 Router 同步快速路径），立刻止损换方向，不要在错误方向上继续投入。

### 原则四：知道何时停手

当框架代码占比降到 <5%，且剩余热点在内核态时，微优化的性价比急剧下降。此时应该转向系统层面的改变（如 io_uring），而不是继续在用户态打磨。

### 最终成绩单

| 版本                          | QPS       | 累计提升     |
| ----------------------------- | --------- | ------------ |
| v2.5.2 基线                   | ~27K      | —            |
| + SO_REUSEPORT + Timer        | ~132K     | +389%        |
| + 去 Beast                    | ~140K     | +419%        |
| + 热路径微优化                | **~159K** | **+489%**    |
| + 延迟分配优化 (分离模式 60s) | ~148K     | 分离模式基线 |

从 27K 到 159K，**接近 6 倍提升**。Hical 已经与 Cinatra/Drogon 持平，而且保留了 C++20 协程 + PMR 内存池 + 反射层等现代特性。

分离模式全场景测试（60s，4 线程，Docker bridge）的额外发现：
- **SyncMiddleware 快速路径验证成功**：10 层同步中间件 158K QPS，仅比无中间件（161K）低 2.1%，而 10 层异步中间件降到 91K（-43.5%）。同步 10 层比异步 10 层快 **73%**。
- **高并发极其稳定**：c=1000 时 QPS 仅下降 2.5%（159K→155K），说明 SO_REUSEPORT + 连接级 timer 在高并发下表现优异。

下一步：io_uring 后端，目标 200K+。
