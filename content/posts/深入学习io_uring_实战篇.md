+++
title = '深入学习 io_uring（三）：C++ 封装、协程集成与高性能架构'
date = '2025-10-03'
draft = false
tags = ["Linux", "io_uring", "C++", "C++20", "协程", "高性能", "Hical"]
categories = ["io_uring学习"]
description = "用现代 C++20 封装 io_uring 的 RAII 管理和 Awaitable 协程集成，实现协程式 TCP 服务器，并总结生产级架构的最佳实践与性能调优要点。"
+++

> **系列导航**：[入门篇]({{< relref "posts/深入学习io_uring_入门篇.md" >}}) | [进阶篇]({{< relref "posts/深入学习io_uring_进阶篇.md" >}}) | **实战篇**

## 前置知识

- 已阅读入门篇和进阶篇，掌握 io_uring 双环形缓冲区和 liburing API
- 了解 C++20 协程基础（`co_await`、`coroutine_handle`、`promise_type`）
- 建议先阅读 [深入学习 Boost.Asio（三）：实战篇]({{< relref "posts/深入学习Boost.Asio_实战篇.md" >}}) 中的协程部分作为对照

---

## 1. RAII 封装：安全管理 io_uring 资源

### 1.1 为什么需要 C++ 封装

直接使用 liburing 的 C API 有三个痛点：
1. `io_uring_queue_init` / `io_uring_queue_exit` 手动配对，容易遗漏
2. `user_data` 是 `void*` 或 `uint64_t`，类型安全全靠人肉
3. 提交→收割的事件循环代码高度模板化，每个项目重写一遍

```
封装层次：

  应用代码（协程/回调）
       │
       ▼
  ┌──────────────────────┐
  │  IoUringAwaitable    │  ← 协程集成层（co_await 一个 I/O 操作）
  └──────────────────────┘
       │
       ▼
  ┌──────────────────────┐
  │  IoUringContext       │  ← 事件循环层（submit / wait / dispatch）
  └──────────────────────┘
       │
       ▼
  ┌──────────────────────┐
  │  IoUring (RAII)       │  ← 资源管理层（init / exit）
  └──────────────────────┘
       │
       ▼
    liburing C API
```

### 1.2 IoUring：RAII 包装

```cpp
// IoUring.hpp — RAII 封装 io_uring 实例
// 编译：g++ -std=c++20 -O2 xxx.cpp -luring -o xxx

#pragma once
#include <liburing.h>
#include <stdexcept>
#include <string>
#include <cstring>

class IoUring
{
public:
    // 构造时初始化 io_uring，指定 SQ 大小和可选标志
    explicit IoUring(unsigned entries, unsigned flags = 0)
    {
        int ret = io_uring_queue_init(entries, &ring_, flags);
        if (ret < 0) {
            throw std::runtime_error(
                "io_uring_queue_init 失败: " + std::string(strerror(-ret)));
        }
    }

    // 禁止拷贝（io_uring 资源不可共享）
    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;

    // 允许移动
    IoUring(IoUring&& other) noexcept : ring_(other.ring_)
    {
        other.moved_ = true;
    }

    // 析构时自动清理
    ~IoUring()
    {
        if (!moved_) {
            io_uring_queue_exit(&ring_);
        }
    }

    // 获取 SQE（SQ 满时自动 submit 腾出空间）
    io_uring_sqe* getSqe()
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // SQ 满了，先提交当前积压的请求
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                throw std::runtime_error("SQ 空间不足，即使 submit 后仍无法获取 SQE");
            }
        }
        return sqe;
    }

    int submit() { return io_uring_submit(&ring_); }

    // 阻塞等待至少一个 CQE
    io_uring_cqe* waitCqe()
    {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            throw std::runtime_error(
                "io_uring_wait_cqe 失败: " + std::string(strerror(-ret)));
        }
        return cqe;
    }

    // 非阻塞查看 CQE
    io_uring_cqe* peekCqe()
    {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_peek_cqe(&ring_, &cqe);
        if (ret == -EAGAIN) return nullptr;  // 无就绪 CQE
        if (ret < 0) {
            throw std::runtime_error(
                "io_uring_peek_cqe 失败: " + std::string(strerror(-ret)));
        }
        return cqe;
    }

    // 标记 CQE 已消费
    void seenCqe(io_uring_cqe* cqe)
    {
        io_uring_cqe_seen(&ring_, cqe);
    }

    // 访问底层 io_uring（高级用法需要）
    io_uring* raw() { return &ring_; }

private:
    io_uring ring_{};
    bool moved_ = false;
};
```

> **设计原则**：RAII 保证 `io_uring_queue_exit` 一定被调用，即使异常传播也不会泄漏内核资源。`getSqe()` 中自动 submit 是防御性编程——避免 SQ 满导致的隐性 bug。

---

## 2. 协程集成：co_await 一个 I/O 操作

### 2.1 目标：让 io_uring 操作可 co_await

我们希望写出这样的代码：

```cpp
// 目标：协程式的线性 I/O 代码
Task<void> handleClient(IoUringContext& ctx, int fd)
{
    char buf[1024];

    while (true) {
        // co_await recv —— 协程挂起，直到数据到达
        int n = co_await ctx.asyncRecv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        // co_await send —— 协程挂起，直到发送完成
        co_await ctx.asyncSend(fd, buf, n, 0);
    }

    close(fd);
}
```

对比进阶篇中的回调/状态机版本，协程版本**像同步代码一样直观**。

### 2.2 IoUringAwaitable：桥接协程与 CQE

C++20 协程的核心是 `await_suspend` 中保存 `coroutine_handle`，在 CQE 就绪后恢复：

```cpp
// IoUringAwaitable.hpp — io_uring 的 Awaitable 适配器

#pragma once
#include <coroutine>
#include <cstdint>
#include <liburing.h>

// 所有 io_uring 异步操作的 Awaitable 基类
struct IoUringAwaitable
{
    // CQE 的结果（在 resume 前由事件循环写入）
    int result = 0;
    // 等待恢复的协程句柄（由 await_suspend 设置）
    std::coroutine_handle<> handle = nullptr;

    // 是否可以不挂起直接返回？io_uring 操作总是异步的，所以总返回 false
    bool await_ready() const noexcept { return false; }

    // 挂起时：将 coroutine_handle 编码到 SQE 的 user_data 中
    // 事件循环在收割 CQE 时用 user_data 恢复对应的协程
    void await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle = h;
    }

    // 恢复后：返回 CQE 的 res 字段
    int await_resume() const noexcept { return result; }
};
```

### 2.3 IoUringContext：事件循环 + 协程调度

```cpp
// IoUringContext.hpp — 集成协程的 io_uring 事件循环

#pragma once
#include "IoUring.hpp"
#include "IoUringAwaitable.hpp"
#include <functional>
#include <vector>

class IoUringContext
{
public:
    explicit IoUringContext(unsigned entries = 256)
        : ring_(entries)
    {
    }

    // === 异步操作：返回 Awaitable，可被 co_await ===

    // 异步 recv
    IoUringAwaitable asyncRecv(int fd, void* buf, unsigned len, int flags)
    {
        IoUringAwaitable awaitable;
        pendingOps_.push_back(&awaitable);

        io_uring_sqe* sqe = ring_.getSqe();
        io_uring_prep_recv(sqe, fd, buf, len, flags);
        // 把 Awaitable 的地址编码到 user_data
        io_uring_sqe_set_data(sqe, &awaitable);

        return awaitable;
    }

    // 异步 send
    IoUringAwaitable asyncSend(int fd, const void* buf, unsigned len, int flags)
    {
        IoUringAwaitable awaitable;
        pendingOps_.push_back(&awaitable);

        io_uring_sqe* sqe = ring_.getSqe();
        io_uring_prep_send(sqe, fd, buf, len, flags);
        io_uring_sqe_set_data(sqe, &awaitable);

        return awaitable;
    }

    // 异步 accept
    IoUringAwaitable asyncAccept(int listenFd, sockaddr* addr,
                                  socklen_t* addrlen, int flags)
    {
        IoUringAwaitable awaitable;
        pendingOps_.push_back(&awaitable);

        io_uring_sqe* sqe = ring_.getSqe();
        io_uring_prep_accept(sqe, listenFd, addr, addrlen, flags);
        io_uring_sqe_set_data(sqe, &awaitable);

        return awaitable;
    }

    // 异步 read（文件）
    IoUringAwaitable asyncRead(int fd, void* buf, unsigned len, uint64_t offset)
    {
        IoUringAwaitable awaitable;
        pendingOps_.push_back(&awaitable);

        io_uring_sqe* sqe = ring_.getSqe();
        io_uring_prep_read(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, &awaitable);

        return awaitable;
    }

    // 异步 close
    IoUringAwaitable asyncClose(int fd)
    {
        IoUringAwaitable awaitable;
        pendingOps_.push_back(&awaitable);

        io_uring_sqe* sqe = ring_.getSqe();
        io_uring_prep_close(sqe, fd);
        io_uring_sqe_set_data(sqe, &awaitable);

        return awaitable;
    }

    // === 事件循环：提交并处理完成事件 ===

    void run()
    {
        running_ = true;
        while (running_) {
            // 提交所有积压的 SQE
            ring_.submit();

            // 等待至少一个 CQE
            io_uring_cqe* cqe = ring_.waitCqe();

            // 从 user_data 恢复 Awaitable
            auto* awaitable = static_cast<IoUringAwaitable*>(
                io_uring_cqe_get_data(cqe));

            // 将 CQE 结果写入 Awaitable
            awaitable->result = cqe->res;

            // 标记 CQE 已消费（必须在恢复协程前，因为协程可能提交新的 SQE）
            ring_.seenCqe(cqe);

            // 恢复等待的协程
            if (awaitable->handle && !awaitable->handle.done()) {
                awaitable->handle.resume();
            }

            // 批量处理：检查是否有更多就绪的 CQE
            while (auto* extraCqe = ring_.peekCqe()) {
                auto* extra = static_cast<IoUringAwaitable*>(
                    io_uring_cqe_get_data(extraCqe));
                extra->result = extraCqe->res;
                ring_.seenCqe(extraCqe);
                if (extra->handle && !extra->handle.done()) {
                    extra->handle.resume();
                }
            }
        }
    }

    void stop() { running_ = false; }

    IoUring& ring() { return ring_; }

private:
    IoUring ring_;
    bool running_ = false;
    std::vector<IoUringAwaitable*> pendingOps_;
};
```

### 2.4 Task 协程类型

```cpp
// Task.hpp — 最小化的协程 Task 类型

#pragma once
#include <coroutine>
#include <exception>
#include <utility>

template <typename T = void>
class Task
{
public:
    struct promise_type
    {
        T value{};
        std::exception_ptr exception = nullptr;

        Task get_return_object()
        {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // 协程创建后立即挂起（lazy start）
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(T v) { value = std::move(v); }

        void unhandled_exception() { exception = std::current_exception(); }
    };

    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    ~Task()
    {
        if (handle_) handle_.destroy();
    }

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task(const Task&) = delete;

    // 启动协程
    void start() { handle_.resume(); }

    // 获取结果
    T result()
    {
        if (handle_.promise().exception) {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(handle_.promise().value);
    }

    std::coroutine_handle<promise_type> handle() { return handle_; }

private:
    std::coroutine_handle<promise_type> handle_;
};

// Task<void> 特化
template <>
struct Task<void>::promise_type
{
    std::exception_ptr exception = nullptr;

    Task get_return_object()
    {
        return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() {}
    void unhandled_exception() { exception = std::current_exception(); }
};
```

---

## 3. 协程式 TCP Echo Server

### 3.1 完整实现

```cpp
// coroutine_echo.cpp — 协程式 io_uring Echo Server
// 编译：g++ -std=c++20 -O2 coroutine_echo.cpp -luring -o coroutine_echo

#include "IoUringContext.hpp"
#include "Task.hpp"
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <vector>
#include <memory>

// 处理单个客户端连接的协程
// 注意：协程的生命周期由 IoUringContext 管理
Task<void> handleClient(IoUringContext& ctx, int clientFd)
{
    char buf[1024];
    printf("[+] 协程启动: fd=%d\n", clientFd);

    while (true) {
        // 异步接收 —— 协程在此挂起，直到数据到达
        int n = co_await ctx.asyncRecv(clientFd, buf, sizeof(buf), 0);

        if (n <= 0) {
            if (n < 0) {
                printf("[!] recv 错误 fd=%d: res=%d\n", clientFd, n);
            }
            break;
        }

        printf("[>] fd=%d 收到 %d 字节\n", clientFd, n);

        // 异步发送 —— 协程在此挂起，直到发送完成
        int sent = co_await ctx.asyncSend(clientFd, buf, n, 0);
        if (sent < 0) {
            printf("[!] send 错误 fd=%d: res=%d\n", clientFd, sent);
            break;
        }

        printf("[<] fd=%d 回显 %d 字节\n", clientFd, sent);
    }

    printf("[-] 连接关闭: fd=%d\n", clientFd);
    close(clientFd);
    co_return;
}

// 接受新连接的协程
Task<void> acceptLoop(IoUringContext& ctx, int listenFd)
{
    // 保存所有客户端协程，管理其生命周期
    std::vector<std::unique_ptr<Task<void>>> clients;

    while (true) {
        // 异步 accept —— 协程在此挂起，直到有新连接
        int clientFd = co_await ctx.asyncAccept(listenFd, nullptr, nullptr, 0);

        if (clientFd < 0) {
            printf("[!] accept 错误: res=%d\n", clientFd);
            continue;
        }

        printf("[+] 新连接 fd=%d\n", clientFd);

        // 为每个客户端启动一个协程
        auto task = std::make_unique<Task<void>>(handleClient(ctx, clientFd));
        task->start();  // 启动协程（运行到第一个 co_await）
        clients.push_back(std::move(task));

        // 清理已完成的协程
        clients.erase(
            std::remove_if(clients.begin(), clients.end(),
                [](const auto& t) { return t->handle().done(); }),
            clients.end());
    }
}

int main()
{
    // 创建监听 socket
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listenFd, 128);

    printf("协程 Echo Server 监听端口 9000...\n");

    // 创建 io_uring 上下文
    IoUringContext ctx(256);

    // 启动 accept 协程
    auto acceptTask = acceptLoop(ctx, listenFd);
    acceptTask.start();

    // 运行事件循环（阻塞）
    ctx.run();

    close(listenFd);
    return 0;
}
```

### 3.2 对比三种编程模型

```
回调/状态机版（进阶篇）：

  switch (ctx->eventType) {
    case EVENT_READ:
      // 处理读取
      submitSend(...);    ← 手动管理状态转换
      break;
    case EVENT_WRITE:
      // 处理写入
      submitRecv(...);    ← 容易遗漏转换
      break;
  }

协程版（本篇）：

  int n = co_await ctx.asyncRecv(fd, buf, len, 0);  ← 线性流程
  co_await ctx.asyncSend(fd, buf, n, 0);             ← 无需手动状态管理
  // 编译器自动管理协程帧的状态
```

| 维度             | 回调/状态机                        | **协程**                         |
| ---------------- | ---------------------------------- | -------------------------------- |
| 代码可读性       | 分散在多个 case 中                 | **线性，像同步代码**             |
| 状态管理         | 手动维护 `eventType`              | **编译器自动管理协程帧**         |
| 错误处理         | 每个回调独立检查                   | **try/catch 或检查返回值**       |
| 性能             | 极致（无额外开销）                 | **接近回调**（协程帧分配可优化） |
| 适合场景         | 极端性能或简单协议                 | **复杂协议，生产代码**           |

---

## 4. 性能调优要点

### 4.1 SQ/CQ 大小调优

```
SQ 大小选择指南：

  SQ 太小 → io_uring_get_sqe() 频繁返回 NULL → 被迫提前 submit
  SQ 太大 → 浪费内存（每个 SQE 64 字节）

  推荐公式：
    SQ entries = max(活跃连接数, 预期并发 I/O 数) × 2

  典型值：
    低并发（< 100 连接）  → 64~128
    中并发（100~10K）     → 256~1024
    高并发（10K~100K）    → 4096~16384

  CQ 大小：
    默认 = 2 × SQ 大小（因为可能有 multishot 操作）
    可通过 IORING_SETUP_CQSIZE 自定义
```

### 4.2 批量提交策略

```cpp
// ❌ 每个操作单独 submit（频繁系统调用）
for (auto& conn : connections) {
    auto* sqe = ring.getSqe();
    io_uring_prep_recv(sqe, conn.fd, conn.buf, BUF_SIZE, 0);
    ring.submit();  // 每次循环一次系统调用
}

// ✅ 先填满 SQE，最后一次 submit
for (auto& conn : connections) {
    auto* sqe = ring.getSqe();
    io_uring_prep_recv(sqe, conn.fd, conn.buf, BUF_SIZE, 0);
}
ring.submit();  // 只有一次系统调用
```

### 4.3 批量收割 CQE

```cpp
// ❌ 每次只处理一个 CQE
while (running) {
    auto* cqe = ring.waitCqe();
    process(cqe);
    ring.seenCqe(cqe);
}

// ✅ 批量收割：一次处理所有就绪的 CQE
while (running) {
    // 至少等一个
    auto* cqe = ring.waitCqe();
    process(cqe);
    ring.seenCqe(cqe);

    // 继续处理所有已就绪的 CQE（非阻塞）
    while (auto* extra = ring.peekCqe()) {
        process(extra);
        ring.seenCqe(extra);
    }

    // 批量提交本轮产生的新请求
    ring.submit();
}
```

### 4.4 内存池化

```cpp
// 连接上下文的对象池（避免频繁 malloc/free）
class ConnCtxPool
{
public:
    explicit ConnCtxPool(std::size_t initialSize)
    {
        pool_.reserve(initialSize);
        for (std::size_t i = 0; i < initialSize; ++i) {
            pool_.push_back(std::make_unique<ConnCtx>());
            freeList_.push_back(pool_.back().get());
        }
    }

    ConnCtx* acquire()
    {
        if (freeList_.empty()) {
            // 池耗尽，扩容
            pool_.push_back(std::make_unique<ConnCtx>());
            return pool_.back().get();
        }
        ConnCtx* ctx = freeList_.back();
        freeList_.pop_back();
        return ctx;
    }

    void release(ConnCtx* ctx)
    {
        ctx->reset();  // 重置状态
        freeList_.push_back(ctx);
    }

private:
    std::vector<std::unique_ptr<ConnCtx>> pool_;
    std::vector<ConnCtx*> freeList_;
};
```

> **性能提示**：在高频连接场景（如短连接 HTTP），对象池可以将每连接的内存分配开销从 ~1μs 降到 ~10ns——两个数量级的差异。

### 4.5 NUMA 亲和性

```cpp
// 在 NUMA 架构上，将 io_uring 线程绑定到特定 CPU
// 确保内存访问不跨 NUMA 节点

#include <sched.h>

void bindToCpu(int cpuId)
{
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(cpuId, &cpuSet);
    sched_setaffinity(0, sizeof(cpuSet), &cpuSet);
}

// 多线程 io_uring 架构：每个线程独立 ring，绑定独立 CPU
void workerThread(int cpuId)
{
    bindToCpu(cpuId);

    // 每个线程独享一个 io_uring 实例（无锁）
    IoUring ring(4096);

    // 本线程的事件循环...
}
```

---

## 5. 生产级架构设计

### 5.1 多线程 io_uring 架构

```
多 ring 架构（推荐）：

  ┌──────────────────────────────────────────────┐
  │                  主线程                        │
  │  ┌─────────────────┐                          │
  │  │  listen socket   │                          │
  │  │  accept ring     │  ← 只负责接受连接        │
  │  └────────┬────────┘                          │
  │           │ 分发新连接（round-robin）           │
  │     ┌─────┼─────┐                              │
  │     ▼     ▼     ▼                              │
  └──────────────────────────────────────────────┘
  ┌──────┐ ┌──────┐ ┌──────┐
  │Worker│ │Worker│ │Worker│     每个 Worker 线程：
  │  0   │ │  1   │ │  2   │     - 独享一个 io_uring 实例
  │      │ │      │ │      │     - 绑定特定 CPU
  │ring_0│ │ring_1│ │ring_2│     - 处理分配给它的连接
  └──────┘ └──────┘ └──────┘     - 无锁（线程间不共享 ring）

  线程间通信：eventfd + io_uring 的 IORING_OP_READ 监听
```

### 5.2 跨线程任务分发

```cpp
// 用 eventfd 实现跨线程唤醒
#include <sys/eventfd.h>

class WorkerThread
{
public:
    WorkerThread() : eventFd_(eventfd(0, EFD_NONBLOCK))
    {
        // 在 io_uring 中注册 eventfd 的 multishot read
        // 当其他线程写入 eventfd 时，本线程的事件循环会被唤醒
    }

    // 其他线程调用此函数分发新连接
    void dispatchConnection(int clientFd)
    {
        {
            std::lock_guard lock(mutex_);
            pendingFds_.push_back(clientFd);
        }
        // 写入 eventfd 唤醒 Worker 的事件循环
        uint64_t val = 1;
        write(eventFd_, &val, sizeof(val));
    }

private:
    int eventFd_;
    std::mutex mutex_;
    std::vector<int> pendingFds_;
};
```

### 5.3 优雅关闭

```cpp
// 优雅关闭的三阶段策略

// 阶段 1：停止接受新连接
void gracefulShutdownPhase1(int listenFd)
{
    // 关闭 listen socket，不再 accept
    close(listenFd);
    printf("阶段1: 停止接受新连接\n");
}

// 阶段 2：等待现有连接处理完成
void gracefulShutdownPhase2(std::vector<ConnCtx*>& connections,
                            std::chrono::seconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!connections.empty()) {
        if (std::chrono::steady_clock::now() > deadline) {
            printf("阶段2: 超时，强制关闭 %zu 个连接\n", connections.size());
            break;
        }
        // 继续处理事件循环，让正在进行的 I/O 完成
        processEvents();
    }
    printf("阶段2: 所有连接已关闭\n");
}

// 阶段 3：清理 io_uring 资源
void gracefulShutdownPhase3(IoUring& ring)
{
    // ring 的析构函数自动调用 io_uring_queue_exit
    printf("阶段3: io_uring 资源已释放\n");
}
```

---

## 6. io_uring 与 Boost.Asio 的关系

### 6.1 Asio 已支持 io_uring 后端

从 Boost 1.78 开始，Boost.Asio 在 Linux 上支持使用 io_uring 作为后端（替代 epoll）：

```cpp
// 在编译时启用 io_uring 后端
// g++ -DBOOST_ASIO_HAS_IO_URING -luring ...

// 或在代码中设置
#define BOOST_ASIO_HAS_IO_URING 1
#include <boost/asio.hpp>

// 此后 io_context 内部使用 io_uring 而非 epoll
// 上层 API（co_spawn、async_read、async_write）完全不变！
```

### 6.2 选择策略

```
何时用什么？

  ┌─────────────────────────────────────────────┐
  │ 需要跨平台（Linux + macOS + Windows）？       │
  │  ├─ 是 → Boost.Asio（自动适配各平台后端）     │
  │  └─ 否 → 仅 Linux？                          │
  │       ├─ 需要极致性能 → 直接 io_uring          │
  │       └─ 需要丰富生态 → Boost.Asio + io_uring │
  └─────────────────────────────────────────────┘
```

| 方案                      | 性能     | 跨平台 | 生态         | 控制力   |
| ------------------------- | -------- | ------ | ------------ | -------- |
| 直接 io_uring + liburing  | **极致** | ❌      | 需自建       | **完全** |
| Boost.Asio + epoll 后端   | 优秀     | **✅**  | **丰富**     | 中等     |
| **Boost.Asio + io_uring** | **极致** | **✅**  | **丰富**     | 中等     |
| libev / libuv             | 良好     | **✅**  | 中等         | 低       |

> **Hical 框架的选择**：Hical 使用 Boost.Asio 作为异步框架，在 Linux 上通过 `-DBOOST_ASIO_HAS_IO_URING` 启用 io_uring 后端——既享受 Asio 的协程生态和跨平台能力，又获得 io_uring 的内核级性能。

---

## 7. 完整知识图谱

```
io_uring 知识体系
│
├── 基础层（入门篇）
│   ├── Linux I/O 演进：select → poll → epoll → io_uring
│   ├── 双环形缓冲区：SQ（提交）+ CQ（完成）
│   ├── SQE 结构：opcode + fd + buf + user_data
│   ├── CQE 结构：user_data + res
│   ├── 三大系统调用：setup / enter / register
│   ├── liburing API：prep → submit → wait → seen
│   └── 文件异步 I/O：read / write / 批量提交
│
├── 进阶层（进阶篇）
│   ├── SQPOLL：内核轮询线程，零系统调用提交
│   ├── Fixed Buffers：预注册缓冲区，跳过 pin/unpin
│   ├── Fixed Files：预注册 fd，跳过文件表查找
│   ├── Linked SQE：LINK / HARDLINK 保证执行顺序
│   ├── Multishot：一次提交多次完成（accept / recv）
│   ├── TCP Echo Server：单连接 → 多连接状态机
│   └── 常见陷阱：CQE seen / SQ 满 / 缓冲区生命周期
│
└── 实战层（实战篇）
    ├── C++ RAII 封装：IoUring / IoUringContext
    ├── 协程集成：IoUringAwaitable + Task<T>
    ├── 协程 Echo Server：co_await recv / send
    ├── 性能调优：批量提交/收割、内存池、NUMA
    ├── 生产架构：多 ring 多线程、eventfd 跨线程通信
    ├── 优雅关闭：三阶段策略
    └── Boost.Asio 集成：-DBOOST_ASIO_HAS_IO_URING
```

---

## 本篇小结

| 概念                 | 要点                                                              |
| -------------------- | ----------------------------------------------------------------- |
| RAII 封装            | IoUring 类自动管理 init/exit，getSqe 自动 submit 防 SQ 满          |
| IoUringAwaitable     | 将 `coroutine_handle` 编码到 `user_data`，CQE 就绪时恢复协程       |
| Task\<T\>            | 最小化协程类型，lazy start + final_suspend                          |
| 协程 Echo Server     | `co_await asyncRecv / asyncSend`，代码像同步一样线性                |
| 批量策略             | 先填充所有 SQE 再一次 submit；收割时 peek 处理所有就绪 CQE          |
| 内存池               | 对象池避免高频 malloc/free，两个数量级的延迟差异                     |
| 多 ring 架构         | 每线程独享 io_uring，绑定 CPU，eventfd 跨线程唤醒                    |
| Asio + io_uring      | `-DBOOST_ASIO_HAS_IO_URING` 在 Asio 框架下启用 io_uring 后端       |

---

## 进一步学习

- [io_uring 官方文档](https://kernel.dk/io_uring.pdf) — Jens Axboe 的设计文档
- [liburing 源码](https://github.com/axboe/liburing) — 官方封装库和示例
- [Lord of the io_uring](https://unixism.net/loti/) — 社区教程（从入门到高级）
