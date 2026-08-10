+++
date = '2026-04-06'
draft = false
title = '深入学习 C++17 PMR（Polymorphic Memory Resource）'
categories = ["C++"]
tags = ["C++", "C++17", "PMR", "内存管理", "内存池", "学习笔记"]
description = "全面解析 C++17 PMR（多态内存资源）：从设计动机、核心类体系、内置资源实现，到自定义 memory_resource、游戏服务器实战场景，一篇掌握现代 C++ 内存分配定制化方案。"
+++


# 深入学习 C++17 PMR（Polymorphic Memory Resource）

> 头文件：`<memory_resource>`
> 命名空间：`std::pmr`
> 编译器要求：GCC 9+ / Clang 9+ / MSVC 19.13+（均需 `-std=c++17` 或以上）

---

## 一、为什么需要 PMR？

### 1.1 传统 Allocator 模型的痛点

C++98 引入的 Allocator 是**模板参数**，这意味着：

```cpp
std::vector<int, MyAlloc<int>>   vec1;
std::vector<int, std::allocator<int>> vec2;

// vec1 和 vec2 是不同类型！无法互相赋值、放进同一个容器
```

**核心问题：**

| 痛点               | 说明                                                                      |
| ------------------ | ------------------------------------------------------------------------- |
| **类型传染**       | Allocator 是模板参数，换一个 Allocator 就变了类型，所有接口签名都要跟着改 |
| **无法运行时切换** | 编译期绑定，测试时想换成 debug allocator？重新编译                        |
| **难以组合**       | 想让 `vector` 内部的 `string` 也用同一个 arena？极其繁琐                  |
| **状态传播困难**   | 有状态 allocator（如持有内存池指针）在容器拷贝/移动时语义复杂             |

### 1.2 PMR 的解法：运行时多态

PMR 用一个**虚基类** `std::pmr::memory_resource` 取代模板参数，容器统一使用 `std::pmr::polymorphic_allocator<T>`：

```cpp
// 所有 pmr 容器是同一类型，无论底层用什么内存资源
std::pmr::vector<int> vec1(&pool_resource);
std::pmr::vector<int> vec2(&monotonic_resource);

// 可以赋值！（数据拷贝，资源不传播——默认行为）
vec1 = vec2;
```

**一句话总结：PMR 把"用哪块内存"从编译期模板参数变成了运行时指针，类型不变、行为可变。**

---

## 二、核心类体系

### 2.1 架构全景

```
                    ┌───────────────────────────┐
                    │  std::pmr::memory_resource │  ← 抽象基类（虚函数接口）
                    └─────────┬─────────────────┘
                              │ 继承
            ┌─────────────────┼──────────────────────┐
            │                 │                       │
   ┌────────▼────────┐ ┌─────▼──────────────┐ ┌─────▼──────────────┐
   │ monotonic_buffer │ │ synchronized_pool  │ │ unsynchronized_pool│
   │   _resource      │ │   _resource        │ │   _resource        │
   └──────────────────┘ └────────────────────┘ └────────────────────┘

                    ┌───────────────────────────┐
                    │ polymorphic_allocator<T>   │  ← 持有 memory_resource*
                    └───────────────────────────┘
                              │ 用作
                    ┌─────────▼─────────────────┐
                    │ std::pmr::vector<T>        │  = std::vector<T, pmr::polymorphic_allocator<T>>
                    │ std::pmr::string           │  = std::basic_string<char, ..., pmr::polymorphic_allocator<char>>
                    │ std::pmr::map<K,V>         │  ...
                    └───────────────────────────┘
```

### 2.2 memory_resource — 抽象基类

```cpp
class memory_resource {
public:
    virtual ~memory_resource() = default;

    // 公共接口（非虚，内部调用虚函数）
    void* allocate(size_t bytes, size_t alignment = alignof(max_align_t));
    void  deallocate(void* p, size_t bytes, size_t alignment = alignof(max_align_t));
    bool  is_equal(const memory_resource& other) const noexcept;

protected:
    // 子类必须实现
    virtual void* do_allocate(size_t bytes, size_t alignment) = 0;
    virtual void  do_deallocate(void* p, size_t bytes, size_t alignment) = 0;
    virtual bool  do_is_equal(const memory_resource& other) const noexcept = 0;
};
```

**设计要点：**
- 经典 NVI（Non-Virtual Interface）模式——公共接口是非虚的，内部转发到 `do_xxx` 虚函数
- `allocate`/`deallocate` 的签名带 `alignment`，比传统 allocator 更灵活
- `is_equal` 用于判断两个资源是否等价（影响容器赋值行为）

### 2.3 polymorphic_allocator — 持有资源指针的分配器

```cpp
template <class T = std::byte>  // C++20 起默认 std::byte
class polymorphic_allocator {
    memory_resource* resource_;  // 内部就一个指针
public:
    polymorphic_allocator(memory_resource* r = get_default_resource());

    T* allocate(size_t n);       // 调用 resource_->allocate(n * sizeof(T), alignof(T))
    void deallocate(T* p, size_t n);

    memory_resource* resource() const;

    // 不传播：容器拷贝/移动时，新容器使用默认资源而非源容器的资源
    // 这是刻意设计——避免生命周期纠缠
};
```

所有 `std::pmr::xxx` 容器就是标准容器 + 这个 allocator 的别名：

```cpp
namespace std::pmr {
    template <class T>
    using vector = std::vector<T, polymorphic_allocator<T>>;

    using string = std::basic_string<char, std::char_traits<char>,
                                     polymorphic_allocator<char>>;
    // ...
}
```

---

## 三、三大内置 memory_resource

### 3.1 monotonic_buffer_resource — 只进不退的线性分配器

**特性：**
- 分配时指针单向递增，`deallocate()` 什么都不做
- 析构时（或调用 `release()`）一次性释放所有内存
- **速度极快**：分配只需指针加法 + 对齐

**内存布局：**

```
初始 buffer（栈上或堆上预分配）:
┌──────────────────────────────────────────┐
│██████████████████░░░░░░░░░░░░░░░░░░░░░░░│
│← 已分配区 →      ← 剩余空间 →            │
│              ↑ current_ptr                │
└──────────────────────────────────────────┘

buffer 用尽后，从 upstream 分配更大的 buffer:
┌──────────────────────┐    ┌──────────────────────────────────────┐
│ 初始 buffer（已满）    │    │ 新 buffer（容量 × 增长因子）           │
│██████████████████████│    │████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░│
└──────────────────────┘    └──────────────────────────────────────┘
                                       ↑ current_ptr
```

**典型用法：**

```cpp
#include <memory_resource>
#include <vector>

// 栈上预留 1KB 缓冲区
char buf[1024];
std::pmr::monotonic_buffer_resource pool{buf, sizeof(buf)};

// 在栈上分配的 vector！
std::pmr::vector<int> vec{&pool};
for (int i = 0; i < 100; ++i) {
    vec.push_back(i);  // 分配从 buf 中切出，超出 1KB 后 fallback 到 upstream
}
// pool 析构时释放所有 upstream 分配的内存，buf 是栈上的自动回收
```

**适用场景：**
- 函数作用域内的临时容器（请求处理、帧逻辑）
- 只需 allocate 不需 deallocate 的批量构建场景
- **游戏服务器中：每帧/每个请求的临时数据，帧结束统一回收**

### 3.2 synchronized_pool_resource — 线程安全的池式分配器

**特性：**
- 内部维护多个**大小分级的内存池**（类似 jemalloc/tcmalloc 的 slab 思路）
- 所有操作线程安全（内部有锁）
- `deallocate()` 真正归还到对应池中可复用

**池的分级逻辑（简化）：**

```
请求大小 → 对齐到最近的池级别 → 从对应池分配

池级别示例（实现定义）:
  Pool[0]:   8 字节块
  Pool[1]:  16 字节块
  Pool[2]:  32 字节块
  Pool[3]:  64 字节块
  ...
  Pool[n]: 大于最大池级别 → 直接从 upstream 分配
```

**pool_options 配置：**

```cpp
struct pool_options {
    size_t max_blocks_per_chunk;    // 每个 chunk 最多多少个块
    size_t largest_required_pool_block;  // 超过此大小直接走 upstream
};

std::pmr::pool_options opts;
opts.max_blocks_per_chunk = 256;
opts.largest_required_pool_block = 4096;

std::pmr::synchronized_pool_resource pool{opts};
```

**适用场景：**
- 多线程环境下的通用分配器替代方案
- 频繁 allocate/deallocate 且大小相近的对象

### 3.3 unsynchronized_pool_resource — 无锁版池式分配器

- 和 `synchronized_pool_resource` 完全相同的池逻辑
- **没有锁保护**，单线程使用
- 在单线程场景下性能更优

```cpp
// 单线程场景（如 EventLoop 线程内部）
std::pmr::unsynchronized_pool_resource pool;
std::pmr::vector<std::pmr::string> messages{&pool};
// 只在当前线程使用，无锁开销
```

### 3.4 全局资源函数

```cpp
// 获取/设置全局默认资源（默认是 new_delete_resource）
memory_resource* get_default_resource();
memory_resource* set_default_resource(memory_resource* r);

// 两个全局单例
memory_resource* new_delete_resource();   // 就是 new/delete
memory_resource* null_memory_resource();  // 任何分配都抛 bad_alloc
```

---

## 四、资源链式传播（Upstream 机制）

PMR 的一大亮点是**资源可以链式组合**：

```
分配请求 → monotonic_buffer_resource
              │ 自身 buffer 用尽
              ▼
           synchronized_pool_resource（upstream）
              │ 池中没有合适的块
              ▼
           new_delete_resource()（最终 upstream）
```

```cpp
// 三级资源链
std::pmr::synchronized_pool_resource level2;  // upstream 默认是 new_delete
std::pmr::monotonic_buffer_resource  level1{1024, &level2};  // upstream = level2

std::pmr::vector<int> vec{&level1};
// 分配路径：level1 的线性 buffer → 用尽后从 level2 的池中拿 → 池用尽从 new/delete 拿
```

**这让我们可以搭建多级缓存的内存分配架构。**

---

## 五、容器中的资源传播语义

### 5.1 默认行为：不传播

```cpp
std::pmr::monotonic_buffer_resource pool_a{4096};
std::pmr::monotonic_buffer_resource pool_b{4096};

std::pmr::vector<int> a{&pool_a};
a.push_back(1);

std::pmr::vector<int> b{&pool_b};
b = a;  // 拷贝数据，b 仍然使用 pool_b 分配内存！
```

这是刻意设计——如果 `pool_a` 是一个函数局部的 monotonic resource，拷贝后原始资源可能已销毁，传播资源指针会导致悬垂。

### 5.2 嵌套容器的资源传播

PMR 容器支持 `uses_allocator` 协议——**外层容器会自动把自己的 allocator 传递给内层元素：**

```cpp
std::pmr::monotonic_buffer_resource pool{65536};

// vector 内的 string 也自动使用 pool！
std::pmr::vector<std::pmr::string> names{&pool};
names.emplace_back("Hello, PMR!");
// 这个 string 的内存也从 pool 中分配，不会 new/delete
```

这是 PMR 相比传统 Allocator 的巨大优势——嵌套容器不再需要手动层层传递 allocator。

---

## 六、自定义 memory_resource 实战

### 6.1 示例：带统计的 Debug 资源

```cpp
class DebugResource : public std::pmr::memory_resource {
    std::pmr::memory_resource* upstream_;
    std::atomic<size_t> totalAllocated_{0};
    std::atomic<size_t> totalDeallocated_{0};
    std::atomic<size_t> allocationCount_{0};

public:
    explicit DebugResource(std::pmr::memory_resource* upstream
                           = std::pmr::get_default_resource())
        : upstream_(upstream) {}

    void PrintStats() const {
        printf("[DebugResource] allocs: %zu, total allocated: %zu bytes, "
               "total freed: %zu bytes, in-use: %zu bytes\n",
               allocationCount_.load(),
               totalAllocated_.load(),
               totalDeallocated_.load(),
               totalAllocated_.load() - totalDeallocated_.load());
    }

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* p = upstream_->allocate(bytes, alignment);
        totalAllocated_ += bytes;
        ++allocationCount_;
        return p;
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        upstream_->deallocate(p, bytes, alignment);
        totalDeallocated_ += bytes;
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }
};
```

**使用：**

```cpp
DebugResource debugRes;
{
    std::pmr::vector<std::pmr::string> data{&debugRes};
    for (int i = 0; i < 1000; ++i) {
        data.emplace_back("item_" + std::to_string(i));
    }
    debugRes.PrintStats();
    // [DebugResource] allocs: 1011, total allocated: 48320 bytes, ...
}
debugRes.PrintStats();
// total freed 应该等于 total allocated（无泄漏）
```

### 6.2 示例：固定大小块分配器（游戏常见）

```cpp
// 适用于大量相同大小对象的高频分配/释放（如网络消息、ECS 组件）
class FixedBlockResource : public std::pmr::memory_resource {
    size_t blockSize_;
    size_t blockAlign_;
    std::vector<void*> freeList_;
    std::vector<std::unique_ptr<char[]>> chunks_;  // 管理底层大块内存
    size_t blocksPerChunk_;

public:
    FixedBlockResource(size_t blockSize, size_t blocksPerChunk = 256,
                       size_t blockAlign = alignof(std::max_align_t))
        : blockSize_(blockSize)
        , blockAlign_(blockAlign)
        , blocksPerChunk_(blocksPerChunk) {}

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        if (bytes > blockSize_ || alignment > blockAlign_) {
            throw std::bad_alloc{};
        }
        if (freeList_.empty()) {
            ExpandPool();
        }
        void* p = freeList_.back();
        freeList_.pop_back();
        return p;
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        freeList_.push_back(p);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    void ExpandPool() {
        // 对齐 blockSize 到 blockAlign 的倍数
        size_t alignedSize = (blockSize_ + blockAlign_ - 1) & ~(blockAlign_ - 1);
        auto chunk = std::make_unique<char[]>(alignedSize * blocksPerChunk_);
        char* base = chunk.get();
        for (size_t i = 0; i < blocksPerChunk_; ++i) {
            freeList_.push_back(base + i * alignedSize);
        }
        chunks_.push_back(std::move(chunk));
    }
};
```

---

## 七、游戏服务器实战场景

### 7.1 场景一：请求处理的帧级分配

每个玩家请求的处理过程中需要大量临时对象，处理完即丢弃：

```cpp
void HandlePlayerRequest(const Packet& packet) {
    // 栈上 4KB 缓冲区 + monotonic 资源：零堆分配
    char buf[4096];
    std::pmr::monotonic_buffer_resource arena{buf, sizeof(buf)};

    // 所有临时容器共用同一个 arena
    std::pmr::vector<int> itemIds{&arena};
    std::pmr::string playerName{&arena};
    std::pmr::map<int, int> statChanges{&arena};

    // 解析和处理逻辑...
    ParsePacket(packet, itemIds, playerName, statChanges);
    ApplyChanges(statChanges);

    // 函数返回时 arena 析构，所有内存一次性释放
    // 没有任何 free/delete 调用！
}
```

### 7.2 场景二：对象池 + PMR 组合

高频创建/销毁的网络消息，用固定块 + pool 组合：

```cpp
class MessageAllocator {
    // 小消息（<= 256 字节）走固定块池
    FixedBlockResource smallPool_{256, 1024};
    // 大消息走系统池
    std::pmr::synchronized_pool_resource bigPool_;

    // 根据大小选择资源的 wrapper
    class RouterResource : public std::pmr::memory_resource {
        FixedBlockResource& small_;
        std::pmr::synchronized_pool_resource& big_;
    protected:
        void* do_allocate(size_t bytes, size_t alignment) override {
            if (bytes <= 256) return small_.allocate(bytes, alignment);
            return big_.allocate(bytes, alignment);
        }
        void do_deallocate(void* p, size_t bytes, size_t alignment) override {
            if (bytes <= 256) small_.deallocate(p, bytes, alignment);
            else big_.deallocate(p, bytes, alignment);
        }
        bool do_is_equal(const memory_resource& other) const noexcept override {
            return this == &other;
        }
    public:
        RouterResource(FixedBlockResource& s, std::pmr::synchronized_pool_resource& b)
            : small_(s), big_(b) {}
    };

    RouterResource router_{smallPool_, bigPool_};

public:
    std::pmr::memory_resource* GetResource() { return &router_; }
};
```

### 7.3 场景三：战斗系统 ECS 组件分配

```cpp
// 战斗开始时创建 arena，战斗结束统一释放
class BattleInstance {
    std::pmr::monotonic_buffer_resource arena_{1024 * 1024};  // 1MB 预分配

    // 所有战斗数据都在 arena 上分配
    std::pmr::vector<HealthComponent> healths_{&arena_};
    std::pmr::vector<PositionComponent> positions_{&arena_};
    std::pmr::vector<BuffComponent> buffs_{&arena_};
    std::pmr::vector<DamageEvent> damageLog_{&arena_};

public:
    void Tick() {
        // 每帧的临时计算也用同一个 arena
        std::pmr::vector<DamageEvent> frameDamages{&arena_};
        CalculateDamages(frameDamages);
        ApplyDamages(frameDamages);
    }

    // 析构时 arena_ 析构，所有内存一次性归还，无碎片
};
```

---

## 八、性能对比基准

以下是一个简单的 benchmark 框架，对比不同资源的分配性能：

```cpp
#include <chrono>
#include <memory_resource>
#include <vector>
#include <cstdio>

template <typename Func>
double BenchmarkMs(Func&& f) {
    auto start = std::chrono::high_resolution_clock::now();
    f();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void BenchAllResources() {
    constexpr int N = 100000;

    // 1. 默认 new/delete
    double t1 = BenchmarkMs([&] {
        std::pmr::vector<int> v{std::pmr::new_delete_resource()};
        for (int i = 0; i < N; ++i) v.push_back(i);
    });

    // 2. monotonic（无预分配 buffer）
    double t2 = BenchmarkMs([&] {
        std::pmr::monotonic_buffer_resource mono;
        std::pmr::vector<int> v{&mono};
        for (int i = 0; i < N; ++i) v.push_back(i);
    });

    // 3. monotonic（预分配 buffer）
    double t3 = BenchmarkMs([&] {
        char buf[N * sizeof(int) * 2];  // 预留足够空间
        std::pmr::monotonic_buffer_resource mono{buf, sizeof(buf)};
        std::pmr::vector<int> v{&mono};
        for (int i = 0; i < N; ++i) v.push_back(i);
    });

    // 4. unsynchronized_pool
    double t4 = BenchmarkMs([&] {
        std::pmr::unsynchronized_pool_resource pool;
        std::pmr::vector<int> v{&pool};
        for (int i = 0; i < N; ++i) v.push_back(i);
    });

    printf("new/delete:              %.3f ms\n", t1);
    printf("monotonic (no buffer):   %.3f ms\n", t2);
    printf("monotonic (pre-alloc):   %.3f ms\n", t3);
    printf("unsynchronized_pool:     %.3f ms\n", t4);
}
```

**典型结果（仅供参考，实际因平台而异）：**

```
new/delete:              1.250 ms
monotonic (no buffer):   0.380 ms    ← ~3x 提速
monotonic (pre-alloc):   0.120 ms    ← ~10x 提速
unsynchronized_pool:     0.650 ms    ← ~2x 提速
```

---

## 九、使用注意事项与陷阱

### 9.1 生命周期管理

```cpp
// 错误：resource 先于容器销毁
std::pmr::vector<int>* MakeVector() {
    std::pmr::monotonic_buffer_resource pool{1024};
    auto* v = new std::pmr::vector<int>{&pool};
    v->push_back(42);
    return v;  // pool 已析构，v 内部指针悬垂！
}
```

**铁律：memory_resource 的生命周期必须 >= 使用它的所有容器。**

### 9.2 allocator 不传播的影响

```cpp
std::pmr::monotonic_buffer_resource pool{4096};
std::pmr::vector<int> a{&pool};
a = {1, 2, 3};

std::pmr::vector<int> b = a;  // 拷贝构造：b 使用默认资源（new/delete），不是 pool！

// 若希望 b 也用 pool：
std::pmr::vector<int> c{a, &pool};  // 显式传递 allocator
```

### 9.3 monotonic_buffer_resource 的内存膨胀

`monotonic_buffer_resource` 初始 buffer 用完后，会从 upstream 申请**几何级增长**的新 buffer：

```
第1次: 初始 buffer 大小
第2次: 初始大小 × 增长因子（实现定义，通常 ×2）
第3次: 上次 × 增长因子
...
```

如果估算不准，可能申请远超实际需要的内存。**建议：根据实际使用量设置合理的初始 buffer 大小。**

### 9.4 跨编译单元的 set_default_resource

```cpp
// 危险：全局状态，初始化顺序不确定
// file_a.cpp
auto* old = std::pmr::set_default_resource(&myPool);

// file_b.cpp 中的全局 pmr 容器可能在 set 之前就初始化了
```

**建议：在 `main()` 入口处尽早设置，或者显式传递资源指针，不依赖全局默认。**

---

## 十、PMR 与传统方案对比总结

| 维度           | 传统 Allocator      | PMR                         | 手写内存池   |
| -------------- | ------------------- | --------------------------- | ------------ |
| **类型影响**   | 改变容器类型        | 不改变容器类型              | 不影响       |
| **运行时切换** | 不支持              | 支持                        | 需自己封装   |
| **嵌套传播**   | 极其繁琐            | 自动（uses_allocator）      | 需手动       |
| **标准库支持** | 完整                | 完整（所有容器有 pmr 别名） | 无           |
| **调试/监控**  | 难以插入            | 继承后轻松插入              | 需自己写     |
| **学习成本**   | 高（SFINAE 陷阱多） | 中等                        | 低           |
| **最佳场景**   | 编译期优化极致场景  | 通用内存定制                | 特定热点优化 |

---

## 十一、思考题

1. **生命周期设计**：如果一个 `std::pmr::map` 存储了 `std::pmr::string` 作为 key，且这个 map 使用 `monotonic_buffer_resource`，在 map 中删除某个 key 后，这个 string 的内存会被回收吗？这对长时间运行的游戏服务器有什么影响？

2. **线程安全选型**：游戏服务器中，消息处理线程各自有独立的 EventLoop。每个线程内部的临时对象分配应该选 `synchronized_pool_resource` 还是 `unsynchronized_pool_resource`？如果这些临时对象需要跨线程传递呢？

3. **内存碎片 vs 浪费**：`monotonic_buffer_resource` 完全没有碎片问题，但可能浪费内存（deallocate 无效）；`pool_resource` 有碎片但能复用。在游戏服务器的以下场景中，你会如何选择？
   - (a) 每帧创建大量临时 AI 路径计算结果
   - (b) 管理数万个在线玩家的背包数据
   - (c) 处理突发的大型公会战日志记录

---

## 十二、思考题参考答案

### 题 1：monotonic_buffer_resource 下删除 map key，内存会回收吗？

**答：不会。**

`monotonic_buffer_resource` 的 `do_deallocate()` 是**空操作**——它什么都不做。内存只在资源析构或调用 `release()` 时才统一归还。

**具体发生了什么：**

```cpp
std::pmr::monotonic_buffer_resource pool{65536};
std::pmr::map<std::pmr::string, int> m{&pool};

m["LongPlayerName_12345"] = 100;
// 此时 pool 内部分配了：
//   1. map 的红黑树节点（含 key-value pair）
//   2. string "LongPlayerName_12345" 的字符数据（如果超过 SSO 阈值）

m.erase("LongPlayerName_12345");
// map 调用了 deallocate()，但 monotonic 的 deallocate 是空操作
// 红黑树节点的内存 → 不回收，仍占着 pool 的空间
// string 字符数据的内存 → 不回收，仍占着 pool 的空间
```

**对长时间运行的游戏服务器的影响：**

如果在 monotonic 资源上做**频繁的增删操作**（比如玩家不断获得和丢弃道具），内存只增不减，最终会：
1. 初始 buffer 用完，不断从 upstream 申请更大的新 buffer（几何级增长）
2. 服务器内存持续上涨，表现为**内存泄漏**

**正确做法：**
- monotonic 只用于**生命周期明确、批量创建后统一销毁**的场景（如单次请求处理、单帧计算）
- 需要频繁增删的长生命周期数据（如玩家背包），应使用 `pool_resource` 或默认 `new/delete`

```cpp
// 正确：请求级 monotonic，请求结束统一释放
void HandleRequest() {
    char buf[4096];
    std::pmr::monotonic_buffer_resource arena{buf, sizeof(buf)};
    std::pmr::map<std::pmr::string, int> temp{&arena};
    // ... 处理逻辑，随便增删
}  // arena 析构，全部归还，干净利落

// 错误：长生命周期数据用 monotonic
class PlayerManager {
    std::pmr::monotonic_buffer_resource pool_{1024 * 1024};  // 永远不释放
    std::pmr::map<int, PlayerData> players_{&pool_};         // 玩家上下线 = 持续增删 = 内存泄漏
};
```

---

### 题 2：EventLoop 线程内的临时对象，选哪个 pool_resource？

**答：线程内部选 `unsynchronized_pool_resource`，跨线程传递需要额外设计。**

**分析：**

游戏服务器典型的线程模型是 one-loop-per-thread——每个 EventLoop 线程独立运行，线程内部的数据不需要锁保护。

```
Thread-1 (EventLoop-1)          Thread-2 (EventLoop-2)
┌─────────────────────┐        ┌─────────────────────┐
│ unsynchronized_pool  │        │ unsynchronized_pool  │
│ (无锁，性能最优)      │        │ (无锁，性能最优)      │
│                     │        │                     │
│ 消息解析 → 逻辑处理  │        │ 消息解析 → 逻辑处理  │
│ 全部在本线程完成     │        │ 全部在本线程完成     │
└─────────────────────┘        └─────────────────────┘
```

每个线程有自己的 `unsynchronized_pool_resource`，无锁竞争，性能最优。

**如果临时对象需要跨线程传递呢？**

这是一个容易踩坑的问题。`unsynchronized_pool_resource` 不是线程安全的，如果对象在 Thread-1 上分配、在 Thread-2 上释放，会产生**数据竞争（UB）**。

三种解决方案：

**方案 A：数据拷贝到目标线程的资源上（推荐）**

```cpp
// Thread-1: 构建消息
void OnThread1() {
    std::pmr::unsynchronized_pool_resource localPool;
    std::pmr::vector<int> data{&localPool};
    data = {1, 2, 3, 4, 5};

    // 跨线程投递时，拷贝到目标线程
    eventLoop2->runInLoop([d = std::vector<int>(data.begin(), data.end())] {
        // Thread-2 中，d 使用默认 allocator，与 Thread-1 的 pool 无关
        ProcessData(d);
    });
}
```

**方案 B：使用 `synchronized_pool_resource` 作为共享资源**

```cpp
// 全局或长生命周期的线程安全池，专门用于跨线程数据
std::pmr::synchronized_pool_resource sharedPool;

// Thread-1 上分配
auto* msg = sharedPool.allocate(sizeof(Message), alignof(Message));

// Thread-2 上释放 —— 安全，因为 synchronized 有锁保护
sharedPool.deallocate(msg, sizeof(Message), alignof(Message));
```

**方案 C：对象所有权转移，释放回原线程**

```cpp
// Thread-2 用完后，把释放操作投递回 Thread-1
eventLoop1->runInLoop([p, &pool1] {
    pool1.deallocate(p, size, align);  // 在 pool1 所属线程释放，安全
});
```

**结论：** 线程内部果断用 `unsynchronized`，跨线程场景优先用方案 A（数据拷贝，最简单安全），性能敏感时考虑方案 B 或 C。

---

### 题 3：三个游戏场景的 PMR 选型

#### (a) 每帧创建大量临时 AI 路径计算结果

**选择：`monotonic_buffer_resource`（预分配栈 buffer）**

```cpp
void AITick() {
    // 路径计算结果是纯临时数据：帧开始创建，帧结束丢弃
    char buf[32768];  // 32KB 栈上缓冲区，覆盖大多数帧的需求
    std::pmr::monotonic_buffer_resource arena{buf, sizeof(buf)};

    for (auto& npc : activeNpcs) {
        std::pmr::vector<Vec3> path{&arena};
        FindPath(npc.pos, npc.target, path);
        npc.SetPath(path);  // 拷贝到 npc 自己的存储
    }
    // 帧结束，arena 析构，零 free 调用
}
```

**理由：**
- 数据生命周期 = 一帧，天然适合 monotonic 的"只分配不释放"模式
- 路径计算密集，每帧可能有成百上千次分配，monotonic 的指针递增分配速度碾压任何池式方案
- 栈上 buffer 避免了连 upstream 都不需要访问的最优路径

#### (b) 管理数万个在线玩家的背包数据

**选择：`unsynchronized_pool_resource`（或默认 `new/delete`）**

```cpp
class PlayerBag {
    // 背包物品频繁增删：获得装备、使用药水、丢弃物品、整理排序...
    // 需要 deallocate 真正归还内存

    static thread_local std::pmr::unsynchronized_pool_resource bagPool;

    std::pmr::vector<Item> items_{&bagPool};
    std::pmr::map<int, int> itemCountCache_{&bagPool};
};
```

**理由：**
- 背包是**长生命周期 + 频繁增删**的数据结构，用 monotonic 会内存泄漏（题 1 的教训）
- 数万玩家的背包操作集中在各自的 EventLoop 线程，用 `unsynchronized` 避免锁开销
- 背包物品大小相对固定（Item 结构体），pool 的分级机制能有效复用
- 如果背包操作不是性能瓶颈，直接用默认 `new/delete` 也完全可以——**不要过早优化**

#### (c) 处理突发的大型公会战日志记录

**选择：`monotonic_buffer_resource`（大预分配 + upstream pool）**

```cpp
class GuildBattleLogger {
    // 公会战期间日志量暴涨：伤害记录、技能释放、buff变化、击杀事件...
    // 特点：写入密集、基本不删除、战斗结束后批量持久化再清空

    std::pmr::synchronized_pool_resource upstream_;  // 兜底
    std::pmr::monotonic_buffer_resource arena_{
        1024 * 1024,  // 1MB 初始，公会战日志量大
        &upstream_
    };

    std::pmr::vector<BattleLogEntry> logs_{&arena_};

public:
    void RecordEvent(const BattleLogEntry& entry) {
        logs_.push_back(entry);  // 极快的 monotonic 分配
    }

    void FlushAndReset() {
        PersistToDatabase(logs_);  // 写入 DB
        logs_.clear();
        arena_.release();  // 一次性释放所有内存，重新开始
    }
};
```

**理由：**
- 日志是典型的**追加写入（append-only）** 场景，几乎不删除单条记录
- 公会战是突发事件，短时间内产生海量日志，monotonic 的分配速度能应对峰值
- 战斗结束后调用 `release()` 一次性归还所有内存，然后进入下一场
- `synchronized_pool_resource` 作为 upstream，因为公会战日志可能被多个线程写入（多个参战玩家的消息分散在不同 EventLoop）
- 1MB 初始预分配避免频繁向 upstream 申请，但即使不够也能优雅扩展

**三个场景的选型对比：**

| 场景        | 生命周期 | 增删模式 | 选型                   | 关键考量                   |
| ----------- | -------- | -------- | ---------------------- | -------------------------- |
| AI 路径计算 | 一帧     | 只创建   | monotonic（栈 buffer） | 极致速度，零释放开销       |
| 玩家背包    | 长期     | 频繁增删 | unsynchronized_pool    | 需要真正的 deallocate      |
| 公会战日志  | 一场战斗 | 追加写入 | monotonic（大预分配）  | append-only + 批量 release |

---

## 参考资料

- [cppreference: memory_resource](https://en.cppreference.com/w/cpp/memory/memory_resource)
- [cppreference: polymorphic_allocator](https://en.cppreference.com/w/cpp/memory/polymorphic_allocator)
- [CppCon 2017: Pablo Halpern — Allocators: The Good Parts](https://www.youtube.com/watch?v=v3dz-AKOVL8)
- [C++17 — The Complete Guide (Nicolai Josuttis), Chapter 29: PMR](https://leanpub.com/cpp17)
