+++
title = '连接级 Atomic 时间戳超时的实现决策'
date = '2026-05-12'
draft = false
tags = ["C++", "性能优化", "Hical", "Boost.Asio", "无锁编程", "atomic"]
categories = ["Hical框架"]
description = "用 atomic 时间戳 + 定期扫描替代 per-request steady_timer，消除 keep-alive 场景下每请求 2 次 epoll_ctl 系统调用。从方案选型到实现细节的完整决策记录。"
+++

## 起因

最初 Hical 的空闲超时实现就是传统做法：每个 HTTP 请求/每次 keep-alive 读等待都注册一个 `steady_timer`，读完成后取消，超时则关闭连接。实现上用的是 `shared_ptr<function>` 自引用环做回调链续期——每连接 2 次堆分配（shared_ptr 控制块 + function 对象），且每次续期都要重新构造回调。

v2.5.2 压测到 132K QPS 时，做热路径Review代码发现这个 timer 机制的问题：
- 每请求 2 次 `epoll_ctl`（注册 + 取消 timer）
- `shared_ptr<function>` 自引用环本身就有堆分配开销
- 140K QPS 下整体约产生 100 万次 `epoll_ctl/sec`，38% CPU 花在内核 `_raw_spin_unlock_irqrestore`（TCP spin_lock），而用户态框架代码只占不到 5%

瓶颈已经从用户态转移到内核态，**减少进内核的次数**成为核心策略。空闲超时的 timer 是明确可以砍掉的——30-60s 的超时精度要求本来就极低。

---

## 改良过程

分两步走：

**第一步**：先把 `shared_ptr<function>` 回调链改为独立协程 `idleTimerLoop`，消除自引用环和 2 次堆分配。这一步还是 per-connection 一个 timer 协程，只是实现更干净了。

**第二步**：发现即便使用协程，per-connection timer 仍然意味着每次 timer 到期时要走 scheduler 调度 + `epoll_ctl`。最终演化为"TcpServer 统一扫描"的设计——整个 server 只需要一个扫描协程，连接侧只写一个 atomic 值。

---

## 方案选型

考虑过三个方案：

### 1. 时间轮（Hierarchical Timing Wheel）

- 优点：O(1) 注册/取消，精确到期
- 缺点：实现复杂，需要自己管理 slot，对框架侵入性大
- 结论：空闲超时精度要求低（±数秒完全可接受），杀鸡用牛刀

### 2. 全局 timer + 有序队列

- 优点：一个 timer 驱动所有超时
- 缺点：每次 I/O 都要重排序（`priority_queue`），多 io_context 架构下锁竞争严重
- 结论：与 SO_REUSEPORT 多 acceptor 架构不兼容

### 3. Atomic 时间戳 + 定期扫描（最终选择）

- 优点：写入路径零开销（relaxed store），无锁，无系统调用，实现简单
- 缺点：超时精度降低（±扫描间隔）
- 结论：对 HTTP idle timeout 完全够用，且与多 io_context 架构天然兼容

选 3 的决定因素：**与已有的 SO_REUSEPORT 多 acceptor 架构零冲突，热路径开销最低，实现最简单。**

---

## 实现细节备忘

### 为什么用 `int64_t` 而不是 `time_point`

`steady_clock::time_point` 内部是 `int64_t`（纳秒），但 `std::atomic<time_point>` 在某些平台上不是 lock-free。直接存毫秒级 `int64_t` 保证所有平台 lock-free，且毫秒精度对超时检测绰绰有余。

### 为什么 `alignas(64)`

`lastActiveTimeMs_` 在 readLoop/writeLoop 中高频写入，而 `reading_`（也是 atomic）在相邻位置被其他路径读取。如果共享同一 cache line，读写会互相 invalidate。`alignas(64)` 独占一条 cache line 消除 false sharing。

后续 v2.6.2 把同样的做法扩展到了 `MemoryPool TrackedResource` 的四个 atomic 计数器和 `MiddlewareTimingStats` 的热计数器——同一个思路的复用。

### 扫描间隔选择

```cpp
auto intervalSec = (std::max)(1.0, idleTimeout_ / 4.0);
```

- `idleTimeout = 60s` → 每 15s 扫描一次
- `idleTimeout = 5s` → 每 1.25s 扫描一次（clamp 到 1s）
- 最坏情况：连接在超时后最多存活 1 个扫描间隔才被清理

选 `/4` 是经验值——既不能太频繁（高连接数时锁竞争），也不能太稀疏（超时后连接存活太久浪费 fd）。

### `connections_` 的锁设计

扫描时需要遍历所有连接，用了 `std::mutex` + `lock_guard`。当前 1K-10K 连接规模下足够。

如果未来连接数到 100K 级别，可以改为 per-loop 的连接集合独立扫描（每个 io_context 维护自己的连接表），无需全局锁。这与现有的多 io_context 架构是自然契合的。

---

## 踩过的坑

### 1. 从回调链到协程再到统一扫描的演化

最初只是想解决 `shared_ptr<function>` 自引用环的问题，改成了 per-connection 的 `idleTimerLoop` 协程。改完发现虽然代码干净了，但本质上还是每个连接一个 timer——进内核的次数没减少。这才进一步想到"根本不需要 per-connection timer"，演化出统一扫描方案。

教训：第一次"改良"不一定到位，要追问"这一步之后瓶颈在哪"。

### 2. 扫描协程的生命周期

`idleCheckLoop` 是 `co_spawn` 到 acceptor 的 executor 上的。`TcpServer` 析构时 `alive_` 置 false，但协程可能还在 `co_await timer.async_wait`。需要在 `gracefulStop()` 中 cancel 这个 timer，协程收到 `operation_aborted` 后检查 `alive_` 退出。

### 3. Windows 下的适用性

Windows IOCP 没有 epoll_ctl，但 Boost.Asio 的 `steady_timer` 底层用 `CreateThreadpoolTimer`，注册/取消同样有内核调用开销。消除 timer 在 Windows 下同样有收益。

---

## 效果

atomic 时间戳作为 v2.6.0 多项优化之一（与去 Beast、同步快速路径、热路径微优化并行实施），整体从 132K 提升到 159K（+20%）。逻辑上的收益：

- keep-alive 场景每请求减少 2 次 `epoll_ctl`（timer ADD + DEL）
- 每连接减少 1 个 timer 对象的内存
- DbConnectionPool 的 `idleCheckLoop` 也采用了同样的定期扫描模式（验证了方案的通用性）

---

## 启发

这个优化的思维模式可以复用到很多场景：

1. **游戏心跳检测**：不需要为每个玩家设一个 timer，定期扫描 `lastPacketTime` 即可（远征Online 的服务器框架也能用）
2. **连接池健康检查**：Hical 的 `DbConnectionPool` 已经这么做了
3. **缓存过期**：不需要精确 TTL 回调，定期扫描 + 惰性删除就够

本质：**当精度要求远低于事件驱动机制的固有开销时，轮询反而是更优解。** 这和直觉（"事件驱动优于轮询"）相反，但在特定约束下就是成立的。
