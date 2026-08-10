+++
title = '拆开 Hical：为什么不用每个连接一个 timer 协程？IdleScanner 集中式空闲扫描设计'
date = 2026-08-10T00:00:00+08:00
draft = false
tags = ["Hical", "IdleScanner", "空闲连接", "定时器", "源码分析"]
categories = ["Hical源码精读"]
description = "拆开 Hical 第 5 篇：为什么不用每个连接一个 timer 协程？IdleScanner 集中式空闲扫描设计。"
+++

# [Hical] 为什么不用每个连接一个 timer 协程？IdleScanner 的集中式空闲扫描设计

> 本专栏文章：拆开 Hical · 第 5 篇

本篇讲一个大多数 HTTP 框架都有的、但很少被拿出来单独聊的功能：**空闲连接超时断开**。

功能本身不复杂——如果一个 keep-alive 连接 60 秒没有新请求，就关掉它。但"怎么实现"有两种完全不同的路线，方案选择直接影响内存和 CPU 成本。

---

## 1. 方案 A：每连接一个 timer 协程（大多数框架的做法）

```
为每个连接创建一个 steady_timer，然后 co_await 在它上面：

co_spawn(io_context, [self]() -> Awaitable<void> 
{
    steady_timer timer(60s);
    auto result = co_await (timer.async_wait() || socket.async_read(...));
    // 如果 timer 先到 → 超时关闭
    // 如果 socket 先到 → 取消 timer，处理请求
});
```

**问题在哪？**

每个 timer 是一个内核对象（epoll fd 上的一个节点）：
- 10,000 个 keep-alive 连接 → 10,000 个 `steady_timer` 的内核注册 + 10,000 个协程帧（每个 ~200 字节）→ ~2MB 的协程帧持续占用
- 每个连接一个新请求过来时要 cancel 旧 timer + 重设新 timer → 两次 epoll_ctl 系统调用
- 最要命的是：99.9% 的连接在 timeout 之前就已经收到了新请求，timer 根本没触发——白分配了协程帧和白注册了内核事件

Hical 问了一个和大多数框架不同的问题：**能不能把 10,000 个 timer 合并成 1 个？**

---

## 2. 方案 B：集中式扫描（Hical 的做法）

把 10,000 个独立 timer 替换成 1 个周期性扫描器 + 1 条空闲连接链表：

```
[扫描器 coroutine (1个)]
    │ 每 (timeout/4) 秒扫描一次
    ▼
┌─────────────────────────────────────────────┐
│         双 向 链 表  (无 锁)                  │
│                                              │
│  sentinel ←→ conn1 ←→ conn2 ←→ conn3 ←→ ... │
│     ↑                                    ↑    │
│     └────────────────────────────────────┘    │
│          循环链表（环形）                       │
└─────────────────────────────────────────────┘
```

每次扫描时遍历链表，把"上次活跃时间到现在超过了超时值"的连接关闭。省掉了：
- 10,000 个 timer 协程帧 → 只剩 1 个扫描器协程
- 10,000 次 epoll_ctl 系统调用 → 只剩 1 个 timer 的周期性 reset
- 连接级别的 timer 管理 → 只剩原子写 `lastActiveMs`

---

## 3. 核心实现

### 3.1 Entry：嵌入协程栈的链表节点

```cpp
struct Entry 
{
    std::atomic<int64_t> lastActiveMs {0};
    boost::asio::ip::tcp::socket* socket; // non-owning
    Entry* prev;
    Entry* next;
    int64_t customTimeoutMs = 0;  // 0 = 用 scanner 的全局默认超时
};
```

Entry 是栈分配的协程局部变量，在线程的协程栈内——**零堆分配**。`lastActiveMs` 用 `relaxed` 原子顺序（只有这个线程在扫描器中读它，写也只来自这个线程的 IO 协程）。

```cpp
void touch() 
{
    lastActiveMs.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);
}
```

每次 HTTP 请求读取数据时调用一次 `touch()`，更新活跃时间戳。

### 3.2 Guard：RAII 注册/注销

```cpp
class Guard 
{
    IdleScanner* scanner_;  // nullptr = no-op
    Entry& entry_;

    Guard(IdleScanner* scannerPtr, Entry& entry): scanner_(scannerPtr), entry_(entry) 
    {
        if (scanner_ != nullptr) 
        {
            scanner_->registerEntry(entry_);  // 插到链表头
        }
    }

    ~Guard() 
    {
        if (scanner_ != nullptr) 
        {
            scanner_->unregisterEntry(entry_);  // 从链表摘掉
        }
    }
};
```

> 💡 当 `idleTimeout_ == 0`（不启用空闲超时），scanner 是 nullptr → Guard 变成 no-op → 零编译期开销，零运行时开销。

### 3.3 扫描器的 run() 协程

```cpp
Awaitable<void> IdleScanner::run() 
{
    tls_scanner = this;  // thread_local 指针

    auto intervalMs = std::max(int64_t{1000}, timeoutMs_ / 4);
    // 扫描间隔 = max(1秒, timeout/4)
    // 例: 60s 超时 → 每 15s 扫描一次

    while (running_ && timer_.has_value()) 
    {
        timer_->expires_after(std::chrono::milliseconds(intervalMs));
        co_await timer_->async_wait();

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        Entry* curr = sentinel_.next;
        while (curr != &sentinel_) 
        {
            Entry* next = curr->next;  // 先存 next（close 可能销毁 curr）
            auto elapsed = now - curr->lastActiveMs.load(std::memory_order_relaxed);
            auto timeout = curr->customTimeoutMs > 0 ? curr->customTimeoutMs : timeoutMs_;

            if (elapsed >= timeout && curr->socket) 
            {
                curr->socket->close();  // 触发 pending async_read 的 error
                // 协程感知到 error → 优雅退出
            }
            curr = next;
        }
    }
}
```

### 3.4 扫描间隔公式为什么是 `max(1s, timeout/4)`？

```
timeout = 60s  → interval = 15s  (连接最多超时 75s 后才被关——最坏情况 60+15)
timeout = 30s  → interval = 7.5s
timeout = 10s  → interval = 2.5s
timeout = 1s   → interval = 1s   (最小值 1s，再小也没意义)
```

`timeout / 4` 是一个折中：太频繁浪费 CPU，太稀有延迟释放。实际线上 60s 超时 + 15s 扫描间隔意味着连接在 60-75 秒后被关——误差在可接受范围内。

### 3.5 customTimeoutMs：SSE 长连接的特殊处理

普通 keep-alive 的 HTTP 连接超时 60 秒，但 SSE 推送连接可能活跃 30 分钟：

```cpp
// 普通 HTTP handler 中的 Guard
IdleScanner::Entry idleEntry;  // customTimeoutMs = 0 → 用 scanner 的全局 60s

// SSE handler 中的 Guard
IdleScanner::Entry sseEntry;
sseEntry.customTimeoutMs = 30 * 60 * 1000;  // 30 分钟
```

同一个 scanner、同一条链表，不同的超时值。每个条目自己决定什么时候算"空闲"。

---

## 4. shutdown()：解决 timer 和 io_context 的循环依赖

这是一个踩坑填过来的实战细节。`steady_timer` 内部持有指向 io_context 的 `timer_service` 的引用。如果 io_context 先析构，timer 的析构会访问已销毁的 service → SEGFAULT。

但如果 timer 先析构呢？timer 析构需要它关联的操作（`async_wait`）先完成。而 `async_wait` 是 `run()` 协程中的 `co_await`——它需要通过 io_context 来处理。

这就是循环依赖。Hical 的解法：

```cpp
// 1. stop()：通过 post cancel timer
//    让 run() 协程退出 (async_wait 返回 operation_aborted)
void IdleScanner::stop() 
{
    running_.store(false);
    timer_->cancel();  // 触发 async_wait 返回 operation_aborted
}

// 2. shutdown()：cancel + 但不 reset timer
//    ~IdleScanner 析构时 timer 成员自然销毁,
//    此时 baseLoop_ (io_context) 还活着 (声明顺序保证)
void IdleScanner::shutdown() 
{
    timer_->cancel();  // 只 cancel、不销毁
    // timer 的析构在 ~IdleScanner 的成员析构阶段自动发生
}
```

关键在 `HttpServer.h` 中的声明顺序——`idleScanners_` 在 `baseLoop_` 之前声明，析构时 `idleScanners_` 先销毁（在 io_context 还活着的时候）。

---

---

## 回顾一下你学会了什么

1. 每连接一个 timer 协程的资源浪费：timer 内核注册 + 协程帧 + epoll_ctl 调用
2. 集中式扫描用一条双向链表 + 一个周期性 timer 替换了 10,000 个独立 timer
3. Entry 嵌在协程栈上（零堆分配），Guard RAII 自动注册/注销
4. `idleTimeout_ == 0` 时 Guard 是 no-op（零开销）
5. 扫描间隔 `max(1s, timeout/4)` 的折中公式
6. customTimeoutMs 让 SSE 长连接在同一扫描器中独立设超时
7. shutdown() 通过声明顺序解决 timer↔io_context 循环依赖

---

> 下一篇：[你的协程内存去哪里了？三层 PMR 内存池设计]({{< relref "posts/hical-source-study/06-PMR内存池.md" >}})
