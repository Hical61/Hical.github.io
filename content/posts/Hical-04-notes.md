+++
title = '第4课：PMR 内存管理'
date = '2026-04-15'
draft = false
tags = ["C++17", "PMR", "内存池", "内存管理", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "深入理解三级内存池架构的设计动机与实现细节，掌握 TrackedResource 零开销统计原理和 PmrBuffer 的读写模型。"
+++

# 第4课：PMR 内存管理 - 学习笔记

> 深入理解三级内存池架构的设计动机与实现细节，掌握 TrackedResource 零开销统计原理和 PmrBuffer 的读写模型。

---

## 一、为什么需要自定义内存管理

### 1.1 默认分配器的问题

C++ 默认的 `new/delete` (即 `malloc/free`) 在高频网络服务器场景下有几个痛点：

| 问题             | 说明                                                |
| ---------------- | --------------------------------------------------- |
| **锁竞争**       | 多线程同时 malloc/free 需要争抢全局堆锁             |
| **碎片化**       | 高频小块分配→释放→再分配，产生大量内存碎片          |
| **系统调用**     | 大块分配可能触发 `mmap/munmap` 系统调用，延迟不可控 |
| **Cache 不友好** | 碎片化的分配导致数据分散在不同的内存页上            |

### 1.2 PMR 的解决思路

PMR (Polymorphic Memory Resource) 把**分配策略**从**容器类型**中解耦：

```
传统方式：容器类型 = 数据结构 + 分配器（编译期绑定）
          std::vector<int>                    → 固定用 new/delete
          std::vector<int, MyAllocator>       → 换分配器 = 换类型

PMR 方式：容器类型固定 + 运行时选择分配器
          std::pmr::vector<int>               → 运行时选择 memory_resource
```

核心类型关系：

```
std::pmr::memory_resource (抽象基类)
    │
    ├── synchronized_pool_resource    线程安全的池
    ├── unsynchronized_pool_resource  非线程安全的池（单线程更快）
    ├── monotonic_buffer_resource     只分配不释放，整体销毁
    ├── new_delete_resource()         封装 new/delete 的单例
    └── TrackedResource (Hical 自定义)  统计包装层
           │
           └──→ polymorphic_allocator<T>  分配器适配器，传给 pmr 容器
```

---

## 二、Hical 三级内存池架构

### 2.1 架构总览

**源码位置**：`src/core/MemoryPool.h` / `src/core/MemoryPool.cpp`

```
┌─────────────────────────────────────────────────────┐
│                 new_delete_resource                   │  系统堆（malloc/free）
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│              TrackedResource（统计层）                │  原子计数：分配次数/字节/峰值
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│  第1级：synchronized_pool_resource（全局同步池）      │  线程安全，跨线程共享
│  配置：max_blocks_per_chunk=128, largest_block=1MB    │
└───────────────────────┬─────────────────────────────┘
                        │ (作为上游)
          ┌─────────────┼─────────────┐
          │             │             │
┌─────────▼──┐  ┌──────▼───┐  ┌─────▼──────┐
│ 第2级：    │  │ 第2级：   │  │ 第2级：     │  每个线程一个
│ unsync_pool│  │ unsync_pool│ │ unsync_pool │  thread_local 无锁
│ (Thread 0) │  │ (Thread 1) │ │ (Thread 2)  │  配置：largest_block=512KB
└──────┬─────┘  └──────┬─────┘ └──────┬──────┘
       │               │              │
       │               │              │ (作为上游，通过全局池)
  ┌────▼─────┐   ┌─────▼────┐   ┌────▼─────┐
  │ 第3级：  │   │ 第3级：  │   │ 第3级：  │  每个请求一个
  │monotonic │   │monotonic │   │monotonic │  只分配不释放
  │(Request) │   │(Request) │   │(Request) │  默认初始 4KB
  └──────────┘   └──────────┘   └──────────┘
```

### 2.2 每级解决什么问题

| 级别      | 类型                           | 线程安全       | 生命周期       | 解决的问题               |
| --------- | ------------------------------ | -------------- | -------------- | ------------------------ |
| **第1级** | `synchronized_pool_resource`   | 是（内部有锁） | 全局（进程级） | 跨线程共享数据的分配     |
| **第2级** | `unsynchronized_pool_resource` | 否（无锁）     | 线程级         | 单线程内高频分配的锁竞争 |
| **第3级** | `monotonic_buffer_resource`    | 否             | 请求级         | 请求内临时对象的碎片化   |

**为什么不能只用一级？**

- 只用全局同步池 → 多线程锁竞争严重
- 只用线程本地池 → 跨线程的共享数据无法处理
- 只用单调池 → 内存只增不减，长生命周期对象会 OOM

三级配合：全局池做兜底、线程本地池做加速、请求级池做极致优化。

---

## 三、MemoryPool 实现详解

### 3.1 构造 — 分层初始化

```cpp
MemoryPool::MemoryPool()
    : trackedResource_(std::pmr::new_delete_resource())  // 统计层 → 系统堆
    , globalPool_(pool_options{
          .max_blocks_per_chunk = config_.globalMaxBlocksPerChunk,      // 128
          .largest_required_pool_block = config_.globalLargestPoolBlock  // 1MB
      }, &trackedResource_)                                // 全局池 → 统计层
{}
```

**上游链**：`globalPool_` → `trackedResource_` → `new_delete_resource()` → `malloc/free`

`pool_options` 的两个参数：
- `max_blocks_per_chunk`：池一次从上游申请的最大块数。128 意味着一次性预分配 128 块同大小内存，减少向上游请求的频率
- `largest_required_pool_block`：池管理的最大单块尺寸。超过 1MB 的分配直接交给上游处理

### 3.2 单例模式

```cpp
MemoryPool& MemoryPool::instance() {
    static MemoryPool instance;   // C++11 保证 static 局部变量线程安全初始化
    return instance;
}
```

Meyers 单例模式：首次调用时构造，线程安全，生命周期到程序结束。

### 3.3 线程本地池 — 代际缓存

这是 MemoryPool 中最精巧的部分：

```cpp
std::pmr::unsynchronized_pool_resource* MemoryPool::getOrCreateThreadPool() {
    // thread_local 缓存结构
    struct ThreadCache {
        std::pmr::unsynchronized_pool_resource* pool = nullptr;
        uint64_t generation = 0;
    };

    thread_local ThreadCache cache;

    // 检查代际：如果 configure() 被调用过，缓存自动失效
    auto currentGen = generation_.load(std::memory_order_acquire);
    if (cache.pool != nullptr && cache.generation == currentGen) {
        return cache.pool;            // 快速路径：直接返回缓存的池
    }

    // 慢速路径：创建新的线程本地池
    auto pool = std::make_unique<std::pmr::unsynchronized_pool_resource>(
        pool_options{...}, &globalPool_);   // 上游 = 全局同步池

    auto* poolPtr = pool.get();

    {
        std::lock_guard<std::mutex> lock(threadPoolsMutex_);
        threadPools_.push_back(std::move(pool));  // 所有权转移给 MemoryPool
    }

    cache.pool = poolPtr;
    cache.generation = currentGen;
    return poolPtr;
}
```

**关键设计点**：

**1. thread_local 缓存**

每个线程有自己的 `ThreadCache`，访问不需要加锁。这是线程本地池"无锁"的关键。

**2. 代际计数器（Generation）**

```
configure() 调用前：generation_ = 0
  Thread A 获取池 → cache.generation = 0 → 命中缓存
  Thread B 获取池 → cache.generation = 0 → 命中缓存

configure() 被调用：generation_ = 1
  Thread A 获取池 → cache.generation=0 ≠ currentGen=1 → 缓存失效 → 创建新池
  Thread B 获取池 → cache.generation=0 ≠ currentGen=1 → 缓存失效 → 创建新池
```

这避免了 `configure()` 后继续使用旧的线程本地池。

**3. 所有权管理**

`thread_local` 变量的析构顺序不可控。如果线程本地池在 MemoryPool 单例之后析构，就会访问已释放的全局池（dangling reference）。

解决方案：线程本地池的所有权归 MemoryPool 持有（`threadPools_` vector），`thread_local` 只缓存裸指针。这样析构顺序是：MemoryPool 析构时主动清理所有线程本地池。

### 3.4 请求级单调池

```cpp
std::unique_ptr<std::pmr::monotonic_buffer_resource>
MemoryPool::createRequestPool(size_t initialSize) {
    if (initialSize == 0) initialSize = config_.requestPoolInitialSize;  // 默认 4KB
    return std::make_unique<std::pmr::monotonic_buffer_resource>(
        initialSize, &globalPool_);  // 上游 = 全局同步池
}
```

**monotonic_buffer_resource 的工作原理**：

```
初始状态（4KB 预分配）：
┌──────────────────────────────────┐
│  空闲空间 (4096 字节)             │
└──────────────────────────────────┘
  ^ next

第1次分配 128 字节：
┌────────┬─────────────────────────┐
│ 已分配  │  空闲空间               │
│ 128B    │  (3968 字节)            │
└────────┴─────────────────────────┘
          ^ next (只前移，不回退)

第2次分配 2048 字节：
┌────────┬─────────┬───────────────┐
│ 128B   │ 2048B   │  空闲 (1920B) │
└────────┴─────────┴───────────────┘
                    ^ next

deallocate → 什么都不做！（只记录，不释放）

池析构 → 整块 4KB 一次性归还给上游
```

**为什么适合 HTTP 请求？**

HTTP 请求有明确的生命周期：接收 → 解析 → 路由 → 处理 → 响应 → 销毁。在此期间分配的 header、body、JSON 解析结果等对象，请求结束后全部不再需要。monotonic 的"只分配不释放，最后整体销毁"恰好匹配。

分配性能接近 O(1)——仅移动指针，没有空闲链表搜索、没有合并操作。

### 3.5 configure — 运行时重配置

```cpp
void MemoryPool::configure(const PoolConfig& config) {
    config_ = config;

    // 1. 清理所有线程本地池
    {
        std::lock_guard<std::mutex> lock(threadPoolsMutex_);
        threadPools_.clear();
    }

    // 2. 重建全局池（placement new）
    globalPool_.~synchronized_pool_resource();
    new (&globalPool_) std::pmr::synchronized_pool_resource(
        pool_options{...}, &trackedResource_);

    // 3. 递增代际，使 thread_local 缓存失效
    generation_.fetch_add(1, std::memory_order_release);
}
```

**Placement new 技巧**：在已有内存上原地重建对象。先手动调析构函数释放旧池，再 placement new 构建新池。这样 `globalPool_` 的内存地址不变，但内容被完全重建。

---

## 四、TrackedResource — 零开销统计

**源码位置**：`src/core/MemoryPool.h`（内联实现）

### 4.1 拦截分配与释放

```cpp
class TrackedResource : public std::pmr::memory_resource {
    std::pmr::memory_resource* upstream_;           // 真正的上游分配器

    std::atomic<size_t> totalAllocations_{0};       // 总分配次数
    std::atomic<size_t> totalDeallocations_{0};     // 总释放次数
    std::atomic<size_t> currentBytes_{0};           // 当前已分配字节
    std::atomic<size_t> peakBytes_{0};              // 历史峰值字节

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* p = upstream_->allocate(bytes, alignment);  // 转发给上游
        totalAllocations_.fetch_add(1, std::memory_order_relaxed);
        auto current = currentBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;

        // 无锁 CAS 更新峰值
        auto peak = peakBytes_.load(std::memory_order_relaxed);
        while (current > peak &&
               !peakBytes_.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {}

        return p;
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        upstream_->deallocate(p, bytes, alignment);  // 转发给上游
        totalDeallocations_.fetch_add(1, std::memory_order_relaxed);
        currentBytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }
};
```

### 4.2 CAS 更新峰值的原理

`peakBytes_` 的更新不能用简单的 `store`，因为多线程可能同时更新：

```
Thread A: current = 1000, peak = 800 → 需要更新为 1000
Thread B: current = 1200, peak = 800 → 需要更新为 1200

如果用 store：
  Thread A: peakBytes_.store(1000)
  Thread B: peakBytes_.store(1200)  ← OK
  但如果顺序反过来：
  Thread B: peakBytes_.store(1200)
  Thread A: peakBytes_.store(1000)  ← 错了！峰值被错误降低
```

CAS (Compare-And-Swap) 解决方案：

```cpp
auto peak = peakBytes_.load(relaxed);
while (current > peak &&
       !peakBytes_.compare_exchange_weak(peak, current, relaxed)) {}
```

逐步解释：
1. 读取当前 peak
2. 如果 current > peak，尝试用 CAS 将 peak 更新为 current
3. CAS 失败 = 其他线程先更新了 peak，`peak` 变量被刷新为最新值
4. 循环重试，直到 current ≤ peak（不需要更新）或 CAS 成功

### 4.3 memory_order_relaxed 的选择

所有统计操作都用 `relaxed` 内存序。原因：

- 统计数据是**近似值**，不需要精确的跨线程一致性
- `relaxed` 是最轻量的内存序，在 x86 上等同于普通 load/store
- 统计不参与任何同步逻辑，不需要 acquire/release 语义

---

## 五、PmrBuffer — 网络缓冲区

### 5.1 内存布局

**源码位置**：`src/core/PmrBuffer.h`

```
buffer_ (pmr::vector<std::byte>)
┌──────────┬──────────────────┬──────────────────────┐
│ prepend  │  readable data   │  writable space      │
│ (8 bytes)│                  │                      │
└──────────┴──────────────────┴──────────────────────┘
0      hPrependSize     readIndex_          writeIndex_       buffer_.size()
           (8)

readableBytes() = writeIndex_ - readIndex_
writableBytes() = buffer_.size() - writeIndex_
prependableBytes() = readIndex_  (≥ 8)
```

### 5.2 关键操作流程

**写入 (append)**：

```
初始状态 (2048+8 字节)：
[prepend:8][======================== writable:2048 ========================]
            ^ readIndex_ = 8
            ^ writeIndex_ = 8

append("Hello, World!") 后：
[prepend:8][Hello, World!][=========== writable:2035 ====================]
            ^ readIndex_   ^ writeIndex_ = 21

再 append("Test"):
[prepend:8][Hello, World!Test][======= writable:2031 ===================]
            ^ readIndex_       ^ writeIndex_ = 25
```

**读取 (retrieve/read)**：

```
retrieve(13) 后（消费 "Hello, World!"）：
[prepend:8][xxxxxxxxxxxxx][Test][======= writable:2031 =================]
                           ^ readIndex_ = 21
                                ^ writeIndex_ = 25

readableBytes() = 25 - 21 = 4  ("Test")
```

**空间回收 (makeSpace)**：

当 writable 不够，但 prepend + 已读空间足够时，**数据前移**而非扩容：

```
前移前：
[prepend:8][xxxxxxx(已读:1000)][data(100)][writable:940]
                                ^ readIndex_=1008
                                           ^ writeIndex_=1108

前移后：
[prepend:8][data(100)][=============== writable:1940 ====================]
            ^ readIndex_=8
                       ^ writeIndex_=108
```

```cpp
void makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + hPrependSize) {
        buffer_.resize(writeIndex_ + len);   // 真正扩容
    } else {
        // 数据前移：把 readable 部分移到 prepend 之后
        size_t readable = readableBytes();
        std::copy(begin() + readIndex_, begin() + writeIndex_, begin() + hPrependSize);
        readIndex_ = hPrependSize;
        writeIndex_ = readIndex_ + readable;
    }
}
```

### 5.3 Prepend 区域的用途

8 字节的 prepend 区域用于在**数据前面**插入协议头，避免数据搬移。

**网络协议常见模式**：消息格式 = [长度头 4字节] + [消息体 N字节]

```
传统方式（需要搬移）：
1. 先写入消息体 → [消息体]
2. 要在前面加长度头 → 需要把整个消息体后移 4 字节
3. 再在前面写长度 → [长度][消息体]

PmrBuffer 方式（零搬移）：
1. 先写入消息体 → [prepend:8][消息体]
2. readIndex_ 前面有 8 字节 prepend 空间
3. 直接在 readIndex_-4 的位置写入长度 → [unused:4][长度:4][消息体]
```

这是游戏服务器网络层的经典优化：发送消息时先组装消息体，最后再补上长度字段。

### 5.4 查找功能

```cpp
const char* findCRLF() const;   // 查找 "\r\n"（HTTP 行结束符）
const char* findEOL() const;    // 查找 "\n"
```

HTTP 协议解析中，头部以 `\r\n` 分隔，需要从缓冲区中定位行边界。

### 5.5 底层容器选择

```cpp
std::pmr::vector<std::byte> buffer_;
```

使用 `pmr::vector` 而非裸数组的原因：
- 自动扩容（resize）
- 与 PMR 分配器无缝集成
- 移动语义支持（swap 操作）

---

## 六、从测试看完整用法

### 6.1 内存池测试

**源码位置**：`tests/test_memory_pool.cpp`

| 测试                    | 验证点                                      |
| ----------------------- | ------------------------------------------- |
| `Singleton`             | 两次 instance() 返回同一地址                |
| `GlobalAllocator`       | 全局分配器能正常创建 pmr::vector            |
| `ThreadLocalAllocator`  | 线程本地分配器能 resize 4096 字节           |
| `RequestPool`           | 单调池能分配 100 个 int，析构自动释放       |
| **`MultiThreadSafety`** | **8 线程各 1000 次分配，无崩溃无数据竞争**  |
| `Stats`                 | 直接通过 TrackedResource 分配，验证统计准确 |

**多线程安全测试解读**：

```cpp
TEST(MemoryPoolTest, MultiThreadSafety) {
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([iterations]() {
            // 每个线程用自己的 threadLocalAllocator
            auto allocator = MemoryPool::instance().threadLocalAllocator();
            for (int j = 0; j < 1000; ++j) {
                std::pmr::vector<char> buffer(allocator);
                buffer.resize(256);  // 从线程本地池分配，无锁
            }
        });
    }
}
```

每个线程使用自己的 `unsynchronized_pool_resource`，完全无锁。只有当线程本地池需要从全局池补充内存时，才会走到 `synchronized_pool_resource` 的锁路径。

### 6.2 PmrBuffer 测试

| 测试                   | 验证点                                              |
| ---------------------- | --------------------------------------------------- |
| `AppendAndRead`        | 写入 → 部分读取 → 读完，readableBytes 正确变化      |
| `Peek`                 | peek 不消费数据                                     |
| `FindCRLF` / `FindEOL` | HTTP 行分隔符定位                                   |
| `EnsureWritableBytes`  | 扩容后可写空间 ≥ 请求大小                           |
| `LargeData`            | 100KB 数据写入和读取正确                            |
| `Swap`                 | 两个 buffer 交换后大小互换                          |
| **`SpaceReclaim`**     | **先写后读制造空闲空间 → 再写触发数据前移而非扩容** |
| `HasWritten`           | 手动写入 + hasWritten 标记，用于零拷贝场景          |

**SpaceReclaim 测试是核心**：

```cpp
TEST(PmrBufferTest, SpaceReclaim) {
    PmrBuffer buffer({}, 32);                    // 初始仅 32 字节

    buffer.append("12345678901234567890");        // 写入 20 字节
    buffer.retrieve(18);                          // 消费 18，剩余 "90"

    // 此时 readIndex 前面有 8+18=26 字节空闲空间
    // 追加 "new data"(8字节) 应该触发数据前移
    buffer.append("new data");
    EXPECT_EQ(buffer.readableBytes(), 10);        // "90" + "new data"
    EXPECT_EQ(std::string(buffer.peek(), 2), "90");
}
```

---

## 七、PoC 示例分析

### 7.1 pmr_poc — 四种场景验证

**源码位置**：`examples/pmr_poc.cpp`

**场景一：复用缓冲区**

```
std::vector  vs  pmr::vector (threadLocal)
```

PMR 的线程本地池在重复分配/释放同大小块时，可以直接从池缓存中取出，无需访问系统堆。

**场景二：请求级批量分配**

```
std::allocator (每个对象独立 new/delete)
  vs
pmr::monotonic (3 个对象共享一块内存，一次性释放)
```

monotonic 的优势在于：
- 分配 = 移动指针，O(1)
- 释放 = 什么都不做（deallocate 是空操作）
- 销毁 = 整块归还上游，O(1)

### 7.2 pmr_benchmark — 性能对比

**源码位置**：`examples/pmr_benchmark.cpp`

对比 5 种分配策略：

| 策略                  | 线程安全                | 分配速度 | 适用场景   |
| --------------------- | ----------------------- | -------- | ---------- |
| `new/delete`          | 是（全局堆锁）          | 基准     | 通用       |
| `synchronized_pool`   | 是                      | 快       | 多线程共享 |
| `unsynchronized_pool` | 否                      | 很快     | 单线程     |
| `monotonic`           | 否                      | 极快     | 只分配场景 |
| `hical threadLocal`   | 是（thread_local 隔离） | 很快     | Hical 默认 |

期望性能排序：`monotonic` > `unsync_pool` ≈ `hical threadLocal` > `sync_pool` > `new/delete`

---

## 八、内存分配在框架中的流转

### 8.1 一次 HTTP 请求的内存分配路径

```
客户端发来请求
    │
    ▼
TcpServer::acceptLoop 创建连接
    │
    ▼
GenericConnection 构造 inputBuffer_
    └── PmrBuffer(MemoryPool::instance().threadLocalAllocator())
        └── 从线程本地池分配 (第2级，无锁)
    │
    ▼
readLoop 收到数据 → inputBuffer_.append(...)
    └── 缓冲区可能扩容，从线程本地池分配
    │
    ▼
HttpServer::session 协程创建请求级池
    └── createRequestPool(4096)  (第3级)
    │
    ▼
在请求级池上解析 HTTP 头、JSON body 等
    └── 所有临时对象从 monotonic 分配
    │
    ▼
路由匹配 → 中间件 → 处理函数 → 生成响应
    │
    ▼
发送响应 → 清理
    └── 请求级池析构 → 整块内存一次性归还
```

### 8.2 与游戏服务器的对比

| Hical 概念      | 游戏服务器等价物                                |
| --------------- | ----------------------------------------------- |
| 全局同步池      | 全局对象池（如 Packet Pool、Entity Pool）       |
| 线程本地池      | 工作线程私有的内存池（无锁加速）                |
| 请求级单调池    | 帧级临时分配器（每帧结束后整体释放）            |
| PmrBuffer       | 网络收发缓冲区（RecvBuffer / SendBuffer）       |
| Prepend 区域    | 消息头预留空间（先组装 body，后补 header）      |
| TrackedResource | 内存统计监控（当前分配量、峰值，用于 OOM 预警） |

---

## 九、关键问题思考与回答

**Q1: 为什么需要三级内存池？每级分别解决什么问题？**

> - **第1级（全局同步池）**：解决跨线程共享数据的分配需求，有锁保护，作为其他两级的上游
> - **第2级（线程本地池）**：解决单线程内高频分配的锁竞争问题，`thread_local` 实现无锁访问
> - **第3级（请求级单调池）**：解决请求生命周期内临时对象的碎片化问题，只分配不释放，极致快速

**Q2: monotonic_buffer_resource 为什么适合请求级分配？**

> HTTP 请求有明确的"创建→使用→销毁"生命周期。请求内分配的所有对象在请求结束后同时失效。monotonic 的特性完美匹配：
> - 分配 = 移动指针（O(1)，无搜索空闲块）
> - deallocate = 空操作（不回收单个对象）
> - 析构 = 整块归还（一次操作释放所有内存）
>
> 代价是不能单独释放某个对象，但请求级场景不需要这个能力。

**Q3: PmrBuffer 的 prepend 区域有什么实际用途？**

> 网络协议通常是 `[长度头][消息体]` 格式。开发时先组装消息体、再补长度头更方便。prepend 区域（8字节）允许在 readIndex 前面写入数据，避免整个 body 后移。8 字节可以容纳 4 字节 int32 长度或 8 字节 int64 长度。

**Q4: 线程本地池为什么不需要加锁？**

> 因为使用了 `thread_local` 修饰。每个线程有独立的缓存实例（`ThreadCache`），读写完全在本线程内部完成，不存在跨线程访问。配合 Hical 的 1 Thread : 1 EventLoop 模型，同一个 EventLoop 上的所有连接都在同一个线程中处理，天然无竞争。
>
> 唯一需要加锁的是 `threadPools_` vector（存储线程本地池的所有权），但这只在创建线程本地池时才会访问，不在热路径上。

---

*下一课：第5课 - TCP 连接与服务器，将深入 GenericConnection 模板、连接状态机和 TcpServer 的 accept 循环。*
