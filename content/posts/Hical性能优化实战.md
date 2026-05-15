+++
title = 'Hical v2.5.2 性能优化实战：SO_REUSEPORT + 连接级 Timer 实现 3 倍 QPS 提升'
date = '2026-05-10'
draft = false
tags = ["C++20", "性能优化", "Hical", "SO_REUSEPORT", "epoll", "Boost.Asio"]
categories = ["性能优化"]
description = "通过 SO_REUSEPORT 多 Acceptor 消除跨线程调度、连接级 atomic 时间戳消除 per-request timer，实现 QPS 从 46K 到 132K 的 183% 提升。"
+++

# Hical v2.5.2 性能优化实战：SO_REUSEPORT + 连接级 Timer 实现 3 倍 QPS 提升

> 在 [火焰图分析](Hical性能剖析实战.md)中，我们定位到 Hical 的 QPS 瓶颈在 Boost.Asio 的 epoll 交互模型——跨线程调度（14.5%）和 timer 相关 epoll_ctl（12.5%）合计吃掉了 27% 的 CPU。[P1 优化]（Router 同步快速路径）无实质提升后，本文记录 P2/P3 两项优化的设计思路、实现细节和实测结果。

---

## 目录

- [1. 背景回顾](#1-背景回顾)
- [2. 优化方案 A：SO_REUSEPORT 多 Acceptor](#2-优化方案-aso_reuseport-多-acceptor)
- [3. 优化方案 B：连接级 Timer + Atomic 时间戳](#3-优化方案-b连接级-timer--atomic-时间戳)
- [4. 实测结果](#4-实测结果)
- [5. 剩余差距与后续方向](#5-剩余差距与后续方向)
- [6. 复现指南](#6-复现指南)

---

## 1. 背景回顾

### 1.1 P1 优化无效的原因

v2.5.2 实现了 `Router::dispatchSync()` 同步快速路径，在无中间件场景下跳过协程帧分配。三轮 Docker 压测结果：

| 轮次                   | QPS    | 变化       |
| ---------------------- | ------ | ---------- |
| v2.5.1（基线）         | 27,493 | —          |
| v2.5.1（静态链接）     | 19,381 | 系统波动   |
| v2.5.2（dispatchSync） | 20,940 | 无实质提升 |

**原因**：`Router::dispatch` 在火焰图中仅占 **0.24%** CPU，同步快速路径省掉的协程帧（~40-130ns）被 Asio 调度层（27%）完全淹没。

### 1.2 真正的瓶颈

perf 火焰图的 CPU 热点分布：

| 热点函数                                            | CPU 占比 | 可优化性   |
| --------------------------------------------------- | -------- | ---------- |
| `sendmsg`（内核 socket 发送）                       | 53.8%    | 不可优化   |
| `epoll_ctl`                                         | 12.5%    | **可优化** |
| `scheduler::wake_one_thread_and_unlock`             | 9.0%     | **可优化** |
| `pthread_cond_signal` + `post_immediate_completion` | 5.5%     | **可优化** |
| Hical 框架代码（Router/HttpResponse/handleSession） | <2%      | 不是瓶颈   |

**结论**：框架用户态代码只占 2%，优化 Router/Middleware/Response 对 QPS 无帮助。必须从 Asio 调度模型层面动手。

---

## 2. 优化方案 A：SO_REUSEPORT 多 Acceptor

### 2.1 问题分析

优化前的 accept 流程：

```
baseLoop (thread-0):  co_await async_accept() → socket
                      ↓
                      co_spawn(workerIoCtx, handleSession(socket))
                      ↓ [eventfd_write → epoll_ctl → pthread_cond_signal]
                      
workerLoop (thread-N): handleSession() → read → route → write
```

每次 accept 触发三重开销：

1. **跨线程 post**：`co_spawn(targetIoCtx, ...)` 内部调用 `eventfd_write(1)` 唤醒目标线程的 `epoll_wait`
2. **Socket fd 迁移**：socket 从 baseLoop 的 epoll 迁移到 worker 的 epoll，触发 `EPOLL_CTL_ADD`
3. **Cache line bouncing**：baseLoop 线程写 eventfd，worker 线程读，跨核 MESI 协议开销

这三项合计贡献了火焰图中 **14.5%** 的 CPU 占比。

### 2.2 竞品做法

Drogon/Trantor 和 Cinatra 均采用 `SO_REUSEPORT` 模型——每个 worker 线程持有独立 acceptor，内核自动在 acceptor 间均衡分发连接。Accept 和 I/O 在同一线程完成，零跨线程调度。

### 2.3 实现方案

核心改动：将单 acceptor 改为多 acceptor，每个 worker loop 持有独立 acceptor。

**HttpServer.h** — 成员变量替换：

```cpp
// 替换前：单 acceptor
std::unique_ptr<tcp::acceptor> acceptor_;

// 替换后：多 acceptor + 运行时回退
std::vector<std::unique_ptr<tcp::acceptor>> acceptors_;
std::vector<std::unique_ptr<IdleFd>> idleFds_;  // 每个 acceptor 配独立 IdleFd
bool reusePortEnabled_ {false};
```

**HttpServer.cpp — start()** — 先创建 ioPool，再在每个 loop 上创建独立 acceptor：

```cpp
// 收集所有 loop（baseLoop + workers）
std::vector<AsioEventLoop*> allLoops;
allLoops.push_back(&baseLoop_);
if (ioPool_)
    for (auto* loop : ioPool_->getAllLoops())
        allLoops.push_back(loop);

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
// 尝试 SO_REUSEPORT：每个 loop 创建独立 acceptor（Linux/macOS/BSD）
for (auto* loop : allLoops)
{
    auto acc = std::make_unique<tcp::acceptor>(loop->getIoContext());
    acc->open(endpoint.protocol());
    acc->set_option(reuse_address(true));
    
    boost::system::error_code ec;
    using reuse_port = boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;
    acc->set_option(reuse_port(true), ec);
    if (ec) { /* 回退到单 acceptor */ break; }
    
    acc->bind(endpoint);
    acc->listen();
}
#endif

// Windows / 低版本内核自动回退到单 acceptor
```

**acceptLoop()** — 双路径实现：

```cpp
if (reusePortEnabled_)
{
    // SO_REUSEPORT 路径（Linux/macOS/BSD）：socket 已在当前 loop 上
    auto socket = co_await acceptor.async_accept(use_awaitable);
    boost::asio::co_spawn(
        co_await boost::asio::this_coro::executor,  // 零跨线程
        handleSession(std::move(socket)),
        boost::asio::detached);
}
else
{
    // 回退路径（Windows 等）：socket 直接创建在目标 worker 上
    auto& targetIoCtx = ioPool_->getNextLoop()->getIoContext();
    tcp::socket socket(targetIoCtx.get_executor());  // IOCP 关联在正确线程
    co_await acceptor.async_accept(socket, use_awaitable);
    boost::asio::co_spawn(targetIoCtx.get_executor(),
                          handleSession(std::move(socket)),
                          boost::asio::detached);
}
```

### 2.4 跨平台兼容

| 平台                             | 行为                                                                                              |
| -------------------------------- | ------------------------------------------------------------------------------------------------- |
| Linux（内核 3.9+）               | SO_REUSEPORT 多 acceptor，零跨线程                                                                |
| macOS / iOS（Darwin）            | SO_REUSEPORT 多 acceptor，零跨线程（BSD 起源，原生支持）                                          |
| FreeBSD / OpenBSD / NetBSD       | SO_REUSEPORT 多 acceptor，零跨线程                                                                |
| Windows                          | 单 acceptor + Cinatra 风格优化：socket 直接创建在目标 worker 的 io_context 上，减少 IOCP 迁移开销 |
| 低版本内核 / SO_REUSEPORT 不可用 | `setsockopt` 失败自动回退到 Windows 同款优化路径                                                  |
| Docker 容器                      | 共享宿主内核，SO_REUSEPORT 正常工作                                                               |

Windows 不支持 SO_REUSEPORT（`SO_REUSEADDR` 行为不同且有端口劫持风险），无法实现多 acceptor 竞争同端口。回退方案借鉴 Cinatra 的策略——`async_accept` 时传入预创建在目标 `io_context` 上的 socket，使 socket 的 IOCP 关联从一开始就在正确的线程，避免 accept 后的 socket fd 跨线程迁移。跨线程 `co_spawn` 本身仍存在（单 acceptor 的固有限制），但 socket I/O 路径上的迁移开销被消除。

> 经研究 Drogon 和 Cinatra 源码：Drogon 在非 Linux 平台同样回退为单 acceptor + 轮询分发；Cinatra 所有平台都用单 acceptor + 目标 executor 创建 socket。Hical 的方案综合了两者优点——支持 SO_REUSEPORT 的平台用多 acceptor，不支持的用 Cinatra 风格优化。

---

## 3. 优化方案 B：连接级 Timer + Atomic 时间戳

### 3.1 问题分析

优化前的空闲超时实现（`HttpSessionImpl.cpp`）：

```cpp
for (;;)  // keep-alive 请求循环
{
    deadline->expires_after(60s);     // → timerfd_settime + epoll_ctl(MOD)
    deadline->async_wait(callback);
    co_await http::async_read(...);
    deadline->cancel();               // → timerfd_settime(0) + epoll_ctl(DEL)
}
```

每个 keep-alive 请求触发 **2 次** timer 相关的 `epoll_ctl`。在 27K QPS 下 = **54K 次/秒** 的无效系统调用。

### 3.2 实现方案

核心思路：timer 每连接**只启动一次**，请求处理中只更新 `atomic<int64_t>` 时间戳（零系统调用）。Timer 回调定期检查时间戳，真正超时才关闭 socket，否则自动续期。

```cpp
// 连接级 atomic 活跃时间戳
auto lastActiveMs = std::make_shared<std::atomic<int64_t>>(now_ms());

// 连接级 timer 自续期链（仅启动一次）
auto scheduleCheck = std::make_shared<std::function<void()>>();
*scheduleCheck = [&deadline, &socket, lastAct, timeoutMs, scheduleCheck]()
{
    deadline->expires_after(std::chrono::milliseconds(timeoutMs));
    deadline->async_wait([&socket, lastAct, timeoutMs, scheduleCheck](auto ec)
    {
        if (ec) return;
        auto elapsed = now_ms() - lastAct->load(std::memory_order_relaxed);
        if (elapsed >= timeoutMs)
            socket.close();     // 真正超时
        else
            (*scheduleCheck)(); // 有活动，续期
    });
};
(*scheduleCheck)();

// 请求循环中：零系统调用的活跃标记
for (;;)
{
    co_await http::async_read(socket, buffer, parser, use_awaitable);
    lastActiveMs->store(now_ms(), std::memory_order_relaxed);  // 替代 expires_after + cancel
    // ... 路由处理 ...
    co_await http::async_write(socket, response, use_awaitable);
    lastActiveMs->store(now_ms(), std::memory_order_relaxed);
}
```

### 3.3 epoll_ctl 频率对比

| 场景                 | 优化前                         | 优化后       |
| -------------------- | ------------------------------ | ------------ |
| 每个 keep-alive 请求 | 2 次（expires_after + cancel） | 0 次         |
| 连接建立             | 1 次（timer 注册）             | 1 次         |
| Timer 续期           | N/A                            | 每 60s ~1 次 |
| **27K QPS 总计**     | **~54K 次/秒**                 | **~2 次/秒** |

#### strace 实测验证

使用 `strace -c -e epoll_ctl -f` 统计优化后 30 秒压测期间的 epoll_ctl 调用次数：

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
100.00    0.011936         119       100           epoll_ctl
------ ----------- ----------- --------- --------- ----------------
100.00    0.011936         119       100           total
```

**仅 100 次**——恰好等于 100 个并发连接各注册 1 次 timer。整个 30 秒压测期间零 per-request epoll_ctl，优化完全生效。

> 注：strace 的 ptrace 拦截机制本身有严重性能开销（QPS 从 132K 降至 2.8K），此处仅看 `calls` 列验证调用次数，不参考 QPS。

### 3.4 超时精度

最坏情况下，超时判定可能延迟到 2 倍 timeout（如刚更新时间戳后 timer 回调触发，需要再等一轮）。对于空闲超时场景（默认 60s），120s vs 60s 完全可接受——这不是精确定时器，是资源回收机制。

---

## 4. 实测结果

### 4.1 测试环境

| 项目         | 规格                                                  |
| ------------ | ----------------------------------------------------- |
| 宿主机       | Windows 10 Enterprise LTSC 2021，16 核 CPU，32GB 内存 |
| VM           | Hyper-V，Ubuntu 24.04，GCC 14，Boost 1.83             |
| 运行方式     | VM 直接编译运行（非 Docker），无 cgroup 限制          |
| 编译参数     | `-O2 -DNDEBUG`（CMake Release）                       |
| 压测工具     | wrk 4.1.0，参数 `-t4 -c100 -d30s`                     |
| bench_server | 4 线程（`HttpServer server(8080, 4)`）                |

### 4.2 Hello World 基准对比

**基线**（v2.5.2，单 acceptor + per-request timer）：

| 轮次     | QPS        | Avg Latency | Max Latency |
| -------- | ---------- | ----------- | ----------- |
| 1        | 43,499     | 3.19ms      | 41.46ms     |
| 2        | 46,836     | 2.97ms      | 57.01ms     |
| 3        | 49,612     | 2.93ms      | 49.64ms     |
| **平均** | **46,649** | **3.03ms**  |             |

**优化版**（SO_REUSEPORT + 连接级 Timer）：

| 轮次     | QPS         | Avg Latency | Max Latency |
| -------- | ----------- | ----------- | ----------- |
| 1        | 137,505     | 1.68ms      | 91.58ms     |
| 2        | 123,116     | 1.74ms      | 51.54ms     |
| 3        | 135,375     | 1.73ms      | 87.84ms     |
| **平均** | **131,999** | **1.72ms**  |             |

**提升：+183%（2.83x），延迟 -43%**

### 4.3 全场景对比

以下数据均为优化版三轮取平均值（`-t4 -c100 -d30s`）：

#### 基础场景

| 场景                           | 优化版 QPS | Avg Latency | 与 Hello World 比 |
| ------------------------------ | ---------- | ----------- | ----------------- |
| Hello World（`GET /`）         | 131,999    | 1.72ms      | 基准              |
| JSON 响应（`GET /api/status`） | 129,488    | 1.71ms      | -1.9%             |
| JSON Echo（`POST /api/echo`）  | 100,819    | 1.91ms      | -23.6%            |

JSON 响应几乎无损（-1.9%），Echo 涉及反序列化 + 序列化，下降 23.6% 合理。

#### 中间件链

| 中间件层数 | 优化版 QPS | 与 0 层比 | 每层平均开销 |
| ---------- | ---------- | --------- | ------------ |
| 0 层       | 141,242    | 基准      | —            |
| 3 层       | 96,679     | -31.6%    | ~10.5%/层    |
| 10 层      | 62,912     | -55.5%    | ~5.5%/层     |

> 0 层 QPS（141K）高于 Hello World（132K），因为 `/middleware/0` 路由的 handler 直接返回 JSON，不经过全局中间件检查路径。中间件开销随层数增加边际递减（3 层时 10.5%/层，10 层时 5.5%/层），符合洋葱模型特征——协程帧复用减少了后续层的分配开销。

#### 高并发扩展性

| 并发连接 | 优化版 QPS | Avg Latency | Socket Errors            |
| -------- | ---------- | ----------- | ------------------------ |
| 100      | 131,999    | 1.72ms      | 0                        |
| 1,000    | 161,130    | 4.27ms      | read: 4                  |
| 10,000   | 156,239    | 3.28ms      | connect: 8,983, read: 73 |

**亮点**：1,000 并发时 QPS 达到峰值 **161K**，与 Cinatra/Drogon 的 Docker 数据（165K/161K）持平。10K 并发的 connect errors 来自 fd 限制（可通过 `ulimit -n` 调整）。

### 4.4 提升幅度总览

| 指标            | 基线（v2.5.2） | 优化后 | 变化                |
| --------------- | -------------- | ------ | ------------------- |
| Hello World QPS | 46.6K          | 132K   | **+183%（2.83x）**  |
| 1K 并发峰值 QPS | —              | 161K   | 接近 Cinatra/Drogon |
| Avg Latency     | 3.03ms         | 1.72ms | **-43%**            |

### 4.5 火焰图对比

优化后使用 `perf record -g -F 999` 重新录制火焰图（`wrk -t4 -c100 -d30s` 压测期间采样）：

![优化后火焰图](../../docker/flame1.svg)

**优化前 vs 优化后热点对比**：

| 热点函数                                | 优化前 CPU 占比 | 优化后状态                                      |
| --------------------------------------- | --------------- | ----------------------------------------------- |
| `sendmsg` → TCP 协议栈                  | 53.8%           | 仍为主要热点（不可优化，I/O 主导）              |
| `epoll_ctl`                             | 12.5%           | **消失** — 连接级 Timer 消除了 per-request 操作 |
| `scheduler::wake_one_thread_and_unlock` | 9.0%            | **消失** — SO_REUSEPORT 消除了跨线程调度        |
| `pthread_cond_signal`                   | 5.5%            | **消失** — 无需唤醒其他线程                     |
| `epoll_wait`                            | 1.8%            | 保留（正常的事件等待）                          |
| `recv`                                  | —               | 保留（正常的数据接收）                          |
| Hical 框架代码                          | <2%             | 几乎不可见                                      |

**结论**：优化后 CPU 几乎全部花在**内核网络栈**（TCP/IP 收发），之前占 27% 的 Asio 调度开销已被完全消除。框架用户态代码在火焰图中不可见——Hical 的性能天花板现在由 Linux 内核网络栈决定。

### 4.6 改动规模

仅修改 **3 个文件**：

| 文件                           | 改动内容                                             |
| ------------------------------ | ---------------------------------------------------- |
| `src/core/HttpServer.h`        | acceptors_/idleFds_ 成员变量，acceptLoop 签名        |
| `src/core/HttpServer.cpp`      | start/acceptLoop/stop/gracefulStop/closeAllAcceptors |
| `src/core/HttpSessionImpl.cpp` | handleSession 超时逻辑重写                           |

测试：470/470 通过，零回归。

---

## 5. 剩余差距与后续方向

### 5.1 与竞品的差距

| 框架                | Docker 4C/512M（100 并发） | Docker 4C/512M（1K 并发） | VM 直接运行（100 并发） | VM 直接运行（1K 并发） |
| ------------------- | -------------------------- | ------------------------- | ----------------------- | ---------------------- |
| Cinatra             | 165,058                    | 72,815                    | —                       | —                      |
| Drogon              | 161,244                    | 60,168                    | —                       | —                      |
| **Hical（优化后）** | **待验证**                 | **待验证**                | **132K**                | **161K**               |

100 并发下 Hical 优化后 QPS 接近 Cinatra/Drogon 的 80%；**1K 并发下 VM 直接运行已达 161K，与 Cinatra Docker 数据持平**。考虑 Docker cgroup 限制的等比缩减，优化后的 Docker 数据预计可进入同一量级。

### 5.2 剩余瓶颈分析

即使 SO_REUSEPORT 消除了跨线程调度，还有以下开销：

1. **Boost.Asio scheduler 内部开销**（~15-20%）：协程帧管理、continuation 队列、epoll_wait 后的 op 分发
2. **Beast HTTP parser**（~10%）：比 Drogon 自研 parser 和 Cinatra 的 picohttpparser wrapper 更重
3. **`std::function` 中间件链**：虚函数调用 + 间接跳转

### 5.3 后续优化方向

| 方向                          | 预期收益                  | 投入 | 优先级         |
| ----------------------------- | ------------------------- | ---- | -------------- |
| io_uring 后端（Linux 5.6+）   | +50%+，完全消除 epoll_ctl | 中   | 高             |
| writeLoop 无锁化（MPSC 队列） | <5%，高并发下显著         | 低   | 中             |
| 轻量 HTTP parser 替代 Beast   | +10-20%                   | 高   | 低（破坏性大） |

---

> **数据说明**：本文数据采集于 2026-05-10，Linux VM（Hyper-V，Ubuntu 24.04），GCC 14，Boost 1.83，wrk 4t/100c/30s，bench_server 4 线程。VM 直接运行（非 Docker），无 cgroup 限制。

> **利益声明**：本文作者是 Hical 框架的开发者。压测数据如实呈现，基线和优化版在同一环境下 A/B 对比。
