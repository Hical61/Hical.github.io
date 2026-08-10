+++
title = '拆开 Hical：你的协程内存去哪里了？三层 PMR 内存池设计'
date = 2026-08-10T00:00:00+08:00
draft = false
tags = ["Hical", "PMR", "内存池", "协程", "源码分析"]
categories = ["Hical源码精读"]
description = "拆开 Hical 第 6 篇：协程内存去哪了？三层 PMR 内存池设计详解。"
+++

# [Hical] 你的协程内存去哪里了？三层 PMR 内存池设计

> 本专栏文章：拆开 Hical · 第 6 篇

前几篇我们都在聊"怎么做"，这篇聊"内存去哪了"。

HTTP 服务器处理一个请求时，至少有这些内存分配：读缓冲（8KB）、请求头解析、URL decode、body 字符串、JSON 对象、响应序列化缓冲、中间件属性注入。每秒 10 万请求 → 每秒数百万次的 malloc/free。

C++17 的 PMR（多态内存资源）给了我们一个机会——**换掉默认的 new/delete，用分层的分配器策略**。Hical 把这个机会用到了极致。

---

## 1. PMR 的最简入门

如果你没用过 PMR，这里 30 秒快速理解：

```cpp
// 传统方式：全局 new/delete（不知道这内存在哪、持续多久）
std::string s = "hello";  // malloc → free

// PMR 方式：你指定分配器的"上游"
char buf[1024];  // 栈上的一块内存
std::pmr::monotonic_buffer_resource pool(buf, sizeof(buf));
std::pmr::string s("hello", &pool);  // 分配在栈上的 buf 里！
// pool 析构 → buf 回收 → 不需要 free！
```

PMR 的核心思想：**分离"我要内存"和"从哪拿内存"。** 不同的分配器适合不同的场景：
- `monotonic_buffer_resource`：一次分配一大块，从这块里切小份出去，只增不减。适合请求级临时数据（请求处理完就全丢）
- `unsynchronized_pool_resource`：内存池，单线程用——池里的内存块可回收复用
- `synchronized_pool_resource`：同上，但线程安全（有全局锁）
- `new_delete_resource`：就是普通的 new/delete

Hical 的灵感：**把三个 PMR 分配器叠成三级，分别对应进程、线程、请求三个生命周期。**

---

## 2. 三级 PMR 架构

```
请求处理中（~100μs 生命周期）：
  jsonObj → monotonic_buffer_resource ─┐
  url decode → 同上                     ├─ 只增不减，请求结束整块丢弃
  body copy → 同上                     ┘

线程生命周期（~分钟到小时）：
  PMR string → unsynchronized_pool_resource ─┐ 无锁池，回收复用
  小 vector → 同上                            ┘

进程生命周期：
  synchronized_pool_resource → 上游（线程安全，但加锁）
    └─ 上游的 TrackedResource → 统计追踪
      └─ 最终上游：new_delete_resource（或 mimalloc）
```

---

## 3. 第一级：全局池（synchronized_pool_resource）

```cpp
// 启动时
MemoryPool::instance().configure({
    .globalMaxBlocksPerChunk = 128,
    .globalLargestPoolBlock = 1024 * 1024   // 1MB
});

// 全局池的 pool_options 控制：
// - max_blocks_per_chunk: 一次从上游申请多少块（128）
// - largest_required_pool_block: 最大单块大小（1MB）
//
// 超过 1MB 的分配直接透传到上游 new_delete_resource
// 免得大对象的短期占用撑大了全局池
```

全局池是"兜底"的——所有不在线程本地池或请求级缓冲中的 PMR 分配最终都经过它。它是 synchronized 的（多线程安全），所以被它保护的内存操作有锁开销。这就是为什么大多数分配要进一步下沉到线程池和请求级缓冲中。

---

## 4. 第二级：线程本地池（unsynchronized_pool_resource）

```cpp
// 每个 IO 线程自动创建自己的线程本地池
auto* pool = MemoryPool::threadLocalPool();
// 单线程使用 → unsynchronized → 无锁！

// pool_options:
// - max_blocks_per_chunk: 64
// - largest_required_pool_block: 512KB
//
// 因为单线程用，块可以少点（不用为竞争预留）
```

**线程本地池的 GC 机制**：

```cpp
struct ThreadPoolEntry 
{
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> pool;
    std::atomic<std::chrono::steady_clock::time_point> lastAllocTime;
    std::atomic<bool> needsRelease{false};
};

// 创建时注册到全局列表
threadPools_.push_back(std::move(entry));

// GC 时（定期触发）：
void gc(std::chrono::seconds maxIdleSeconds) 
{
    for (auto& entry : threadPools_) 
    {
        if (now - entry->lastAllocTime > maxIdleSeconds) 
        {
            // 给拥有者线程发信号"需要 release"
            entry->needsRelease.store(true);
        }
    }

    // 下次拥有者线程调用 threadLocalPool() 时：
    // cache->pool->release();  ← 由拥有者自己释放！线程安全
}
```

> 💡 GC 的标记-延迟释放模式：GC 线程不直接 delete 其他线程的池，只设置 `needsRelease` 标志。下一次拥有者线程路过时会自己调 `release()`——因为只有拥有者才能安全销毁单线程池。

**generation 代际计数器**：

```cpp
// configure() 重建全局池时：
globalPool_.~synchronized_pool_resource();
new (&globalPool_) ...;  // placement new 重建
generation_.fetch_add(1);  // 代际 +1

// 线程路过时：
if (cache.generation != currentGen) 
{
    // 全局池重建了 → 我的 thread_local 池也要重建
    // 因为它引用了旧的 globalPool_ 作为上游（已析构！）
}
```

---

## 5. 第三级：请求级单调缓冲（monotonic_buffer_resource）

```cpp
auto requestPool = MemoryPool::createRequestPool(4096);
// └─ 初始大小 4KB，上游用线程本地无锁池

// 使用：
std::pmr::string body = req.readBody(pmrAlloc());
//  ↑ body 的内存由 requestPool 分配

// 请求结束：
// requestPool 析构 → 整块内存还给线程本地池
```

**不可释放的特征是特性，不是 bug**：

单调缓冲只增不减——分配出去的块不能单独 free。但这在请求场景下恰好是优点：请求处理完整个缓冲就丢了，不需要逐个释放。从 4KB 开始如果不够用，自动扩容——上游是线程本地池，比全局 new/delete 快得多。

> 💡 关键约束：请求级单调缓冲分配的对象不得逃逸请求作用域——因为缓冲在请求结束后就析构了。这在协程中是自然保证的：请求开始→分配→请求结束→析构，全在同一协程作用域内。

---

## 6. 配套的 StringPool：字符串对象的回收复用

三级 PMR 解决了"原始内存如何分配"的问题。但 `std::string` 本身作为对象容器也需要复用。HTTP 服务器中的字符串分配特征：(1) 很多小字符串（500-2000 字节响应 body），(2) 频繁创建/销毁，(3) 大小基本可预测。

```cpp
class StringPool 
{
    // 5 档大小：256 / 512 / 1K / 2K / 4K
    static constexpr size_t kClassSizes[5] = {256, 512, 1024, 2048, 4096};

    // 每档最多缓存 32 个
    static constexpr size_t kMaxPooled = 32;

    // thread_local 池
    static Pool& threadPool() 
    {
        thread_local Pool pool;
        return pool;
    }

    // 拿：找刚好能装下 requiredSize 的档位
    static std::shared_ptr<std::string> acquire(size_t requiredSize) 
    {
        auto classIdx = findClass(requiredSize);
        if (classIdx == kNumClasses) return std::make_shared<std::string>();  // >4K 不池化

        auto& pool = threadPool();
        auto& cls = pool.classes[classIdx];

        std::string* raw;
        if (cls.count > 0) 
        {
            raw = cls.slots[--cls.count];  // 池里还有 → 复用
        } 
        else 
        {
            raw = new std::string();
            raw->reserve(kClassSizes[classIdx]);  // 新建 → pre-reserve
        }
        return std::shared_ptr<std::string>(raw,[classIdx](std::string* str) { StringPool::release(str, classIdx); });
    }

    // 还：清空后回到对应的档位
    static void release(std::string* str, size_t classIdx) 
    {
        auto& pool = threadPool();
        auto& cls = pool.classes[classIdx];
        str->clear();
        if (cls.count < kMaxPooled) 
        {
            cls.slots[cls.count++] = str;  // 池还没满 → 回收
        } 
        else 
        {
            delete str;  // 满了 → 真的 delete
        }
    }
};
```

**跨线程释放降级为 new/delete**：StringPool 的 acquire/release 都是 thread_local 的。如果 string 在 IO 线程分配、在另一个线程释放——custom deleter 里 `release()` 回到另一个线程的池——这是安全的（因为不涉及跨线程的池状态修改），但 string 回不到了原线程的池，降级为普通 delete。不过 HTTP server 的主流场景是同一个线程上 send → writeLoop，跨线程的情况很少。

驻留内存上限：32 × 5 × 4KB = 640KB/线程。

---

## 7. ReadBufferPool：8KB 缓冲的请求级借还

这个池在第 1 篇已经讲过，这里简要回顾：

```cpp
class ReadBufferPool 
{
    static constexpr size_t kBufferSize = 8192;    // 8KB
    static constexpr size_t kMaxReturnSize = 65536; // 超过 64KB 不归还
    static constexpr size_t kMaxPooled = 32;        // 最多 32 个

    // 借
    static BufferHandle acquire() 
    {
        if (thread_local 池非空) return 入池的 buffer;
        else return new string(reserve 8KB);
    }

    // 还（BufferHandle 的析构函数自动调）
    static void release(string* buf) 
    {
        buf->clear();
        if (thread_local 池还没满 && buf->size() <= kMaxReturnSize) 
        {
            放回池;
        } 
        else 
        {
            delete buf;  // 异常大的请求不污染池子
        }
    }
};
```

`kMaxReturnSize = 65536` 的保护：偶尔有请求触发了 8KB→64KB 的扩容（比如超大的请求体），归还时检查容量——超标的直接 delete，不撑大池子。

---

## 回顾一下你学会了什么

1. 三级 PMR 对应三级生命周期：进程 → 线程 → 请求
2. 全局池是有锁的兜底，大多数分配应下沉到线程池或请求级缓冲
3. 线程本地池无锁（unsynchronized），带 GC 延迟释放——GC 不直接 delete 别的线程的池
4. 请求级单调缓冲只增不减——请求结束整块丢弃，恰是 HTTP 请求的特征优势
5. StringPool 5 级分档回收 string 对象，跨线程释放降级为普通 delete
6. ReadBufferPool 的 kMaxReturnSize 保护——异常大的请求不撑大池子
7. generation 代际计数器让 configure() 重建全局池时所有 thread_local 缓存自动失效

---

> 下一篇：[一套 API，两套实现：C++26 反射双轨是怎么设计的]({{< relref "posts/hical-source-study/07-反射双轨.md" >}})
