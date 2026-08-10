+++
title = '为 C++ Web 框架设计三层 PMR 内存池：从原理到实战'
date = '2026-04-12'
draft = false
tags = ["C++17", "PMR", "内存管理", "高性能", "Hical"]
categories = ["Hical框架"]
description = "以 Hical 框架为例，深入讲解如何利用 C++17 PMR（Polymorphic Memory Resource）为高并发 Web 服务器构建三层内存池架构。"
+++

# 为 C++ Web 框架设计三层 PMR 内存池：从原理到实战

> 本文以 Hical 框架为例，深入讲解如何利用 C++17 PMR（Polymorphic Memory Resource）为高并发 Web 服务器构建三层内存池架构。

---

## 为什么 Web 服务器需要自定义内存管理？

一个 HTTP 请求的生命周期中，框架需要分配大量临时对象：解析缓冲区、路径字符串、JSON 值、响应体。在高并发场景下（如 50K QPS），`new/delete` 的全局锁竞争会成为显著瓶颈：

```
50,000 请求/秒 × 每请求 ~20 次分配 = 1,000,000 次/秒 new/delete
                                       ↓
                                全局堆锁竞争 → CPU 空转
```

传统方案是自研内存池，但 C++17 提供了标准化的解决方案 —— PMR。

## PMR 速览

PMR 的核心思想：**把内存分配策略从容器类型中解耦**。

```cpp
// 传统方式：分配器绑定在类型中
std::vector<int> vec;  // 永远用 std::allocator

// PMR 方式：运行时切换分配策略
std::pmr::vector<int> vec(&myPool);  // 用自定义内存池
```

标准库提供了三种现成的内存资源：

| 资源                           | 特点                         | 线程安全 |
| ------------------------------ | ---------------------------- | -------- |
| `synchronized_pool_resource`   | 池化分配，内部分桶管理       | 是       |
| `unsynchronized_pool_resource` | 同上，但无锁                 | 否       |
| `monotonic_buffer_resource`    | 只分配不释放，析构时整体归还 | 否       |

## Hical 的三层设计

Hical 将这三种资源组合为一个三层架构，每层解决不同场景的问题：

```
┌─────────────────────────────────────────────────┐
│  第1层：全局同步池 (synchronized_pool_resource)  │
│  ├── 线程安全，跨线程共享                       │
│  ├── 上游：TrackedResource → new_delete_resource │
│  └── 用途：全局配置、共享数据结构               │
├─────────────────────────────────────────────────┤
│  第2层：线程本地池 (unsynchronized_pool_resource)│
│  ├── thread_local，零锁竞争                     │
│  ├── 上游：第1层全局同步池                      │
│  └── 用途：连接缓冲区、线程内频繁分配          │
├─────────────────────────────────────────────────┤
│  第3层：请求级单调池 (monotonic_buffer_resource) │
│  ├── 只分配不释放，请求结束整体归还             │
│  ├── 上游：第2层线程本地池                      │
│  └── 用途：HTTP 解析缓冲区、请求内临时对象      │
└─────────────────────────────────────────────────┘
```

### 为什么是三层而不是一层？

关键洞察：**不同层级的数据有不同的生命周期和访问模式**。

| 层级     | 生命周期 | 访问模式       | 最佳策略 |
| -------- | -------- | -------------- | -------- |
| 全局数据 | 进程级   | 多线程读写     | 有锁池化 |
| 连接数据 | 连接级   | 单线程         | 无锁池化 |
| 请求数据 | 毫秒级   | 单线程、一次性 | 单调分配 |

如果只用一个全局池，线程本地的高频分配也会争锁。如果只用 `thread_local`，跨线程共享数据无法处理。三层各司其职。

### 层级关系：上游级联

PMR 的强大之处在于**级联**——每个 `memory_resource` 都有一个上游。当本层无法满足分配时，向上游申请大块内存：

```
请求级单调池（4KB 初始块用完了）
    → 向线程本地池申请新的大块
        → 线程本地池内部桶没有合适的块
            → 向全局同步池申请
                → 向 TrackedResource → new_delete 申请
```

实际运行中，绝大多数分配在本层就能满足，级联很少发生。

## 关键实现细节

### TrackedResource：零开销统计

Hical 在全局池和 `new_delete_resource` 之间插入了一个统计层：

```cpp
class TrackedResource : public std::pmr::memory_resource
{
protected:
    void* do_allocate(size_t bytes, size_t alignment) override
    {
        void* p = upstream_->allocate(bytes, alignment);
        totalAllocations_.fetch_add(1, std::memory_order_relaxed);
        auto current = currentBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        // 无锁 CAS 更新峰值
        auto peak = peakBytes_.load(std::memory_order_relaxed);
        while (current > peak &&
               !peakBytes_.compare_exchange_weak(peak, current, std::memory_order_relaxed))
        {
        }
        return p;
    }
};
```

设计要点：
- `memory_order_relaxed`：统计不需要严格顺序，最大化性能
- CAS 循环更新峰值：避免额外的锁
- 只在最底层统计：线程本地池和请求池的上游都指向全局池，不重复计数

运行时监控只需一行：

```cpp
auto stats = MemoryPool::instance().getStats();
// stats.currentBytesAllocated → 当前内存使用
// stats.peakBytesAllocated   → 历史峰值
```

### 请求级单调池：为什么是最佳选择

HTTP 请求有一个关键特征：**所有临时数据在请求结束后同时失效**。这完美匹配 `monotonic_buffer_resource` 的语义——分配只移动指针（极快），释放什么都不做，析构时整块归还。

在 Hical 的 `handleSession` 协程中：

```cpp
Awaitable<void> HttpServer::handleSession(tcp::socket socket)
{
    // 创建请求级单调池
    auto requestPool = MemoryPool::instance().createRequestPool();
    std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());

    // Beast 的 flat_buffer 使用 PMR 分配器
    beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);

    for (;;)
    {
        co_await http::async_read(socket, buffer, parser, use_awaitable);
        // ... 处理请求 ...
        // buffer 的所有中间分配都来自单调池
    }
    // requestPool 析构 → 整块内存一次性归还给线程本地池
}
```

### 线程本地池：1:1 模型的天然搭档

Hical 采用「1 线程 : 1 事件循环」的线程模型。同一线程上的所有连接共享一个线程本地池：

```cpp
std::pmr::polymorphic_allocator<std::byte> MemoryPool::threadLocalAllocator()
{
    thread_local auto* pool = createThreadLocalPool();
    return std::pmr::polymorphic_allocator<std::byte>(pool);
}
```

`thread_local` 保证每个线程独享一份 `unsynchronized_pool_resource`，完全无锁。请求级单调池在析构时将大块内存归还给线程本地池，线程本地池缓存这些块供后续请求复用——形成一个高效的内存回收循环。

## 性能特征

典型的分配性能排序（由快到慢）：

```
monotonic（请求级）> unsynchronized_pool（线程本地）≈ hical threadLocal > synchronized_pool（全局）> new/delete
```

关键指标解读：
- **`totalAllocations` 远小于请求数** → 池化复用效果好
- **`currentBytesAllocated` 稳定** → 无内存泄漏
- **`peakBytesAllocated` 合理** → 没有内存飙升

## 适用场景与限制

三层 PMR 池在以下场景优势最大：
- **高并发短请求**：QPS > 10K，请求处理时间 < 10ms
- **请求间数据独立**：每个请求的数据不跨请求共享
- **多线程事件循环**：1:1 线程模型

不适用的场景：
- 长生命周期的大对象（应直接 `new`）
- 需要跨请求缓存的数据（应放全局池）

## 总结

PMR 不是银弹，但它提供了一种**标准化的方式**把正确的分配策略放到正确的层级。三层架构的核心思想是：**用数据的生命周期决定分配策略，而不是用一刀切的全局分配器**。

---

> 源码参考：[Hical/src/core/MemoryPool.h](https://github.com/Hical61/Hical/blob/main/src/core/MemoryPool.h)
> 项目地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
