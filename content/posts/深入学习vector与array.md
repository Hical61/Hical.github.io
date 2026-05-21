+++
date = '2022-05-20'
draft = false
title = '深入学习 std::vector 与 std::array'
categories = ["C++"]
tags = ["C++", "STL", "vector", "array", "容器", "学习笔记"]
description = "从内存模型、增长策略到迭代器失效，全面掌握 std::vector 与 std::array 的设计哲学、性能陷阱与实战用法。"
+++


# 深入学习 std::vector 与 std::array

> 头文件：`<vector>` / `<array>`
> 命名空间：`std`
> 编译器要求：`std::vector` — C++98 起；`std::array` — C++11 起

---

## 一、为什么 vector 是默认首选容器

### 1.1 Bjarne Stroustrup 的建议

> "Use `std::vector` by default."

这不是随便说说。现代 CPU 的性能瓶颈往往不在计算而在**内存访问**。vector 的数据在堆上连续存储，对 CPU 缓存极其友好：

| 特性 | vector | list | deque |
|------|--------|------|-------|
| 内存布局 | 连续 | 散列节点 | 分段连续 |
| 缓存命中率 | 极高 | 极低 | 中等 |
| 遍历性能 | 最优 | 最差 | 中等 |
| 随机访问 | O(1) | O(n) | O(1) |
| 尾部插入 | 摊还 O(1) | O(1) | O(1) |

### 1.2 实测：连续内存的威力

```cpp
#include <vector>
#include <list>
#include <chrono>
#include <cstdio>

int main()
{
    constexpr int N = 1'000'000;

    // vector：连续内存，CPU 预取器能猜到下一个地址
    std::vector<int> vec(N);
    // list：每个节点独立 new，内存地址随机分布
    std::list<int> lst(N);

    auto start = std::chrono::high_resolution_clock::now();
    long long sum = 0;
    for (auto& v : vec) sum += v;   // 顺序访问，缓存行一次载入 16 个 int
    auto vecTime = std::chrono::high_resolution_clock::now() - start;

    start = std::chrono::high_resolution_clock::now();
    sum = 0;
    for (auto& v : lst) sum += v;   // 每次跳转到随机地址，缓存行浪费
    auto lstTime = std::chrono::high_resolution_clock::now() - start;

    printf("vector: %lld us\n", std::chrono::duration_cast<std::chrono::microseconds>(vecTime).count());
    printf("list:   %lld us\n", std::chrono::duration_cast<std::chrono::microseconds>(lstTime).count());
    // 典型结果：vector 比 list 快 10~50 倍
}
```

---

## 二、vector 内存模型

### 2.1 三指针架构

vector 内部通常只有三个指针（24 字节 on x64）：

```
                        capacity
          ◄─────────────────────────────────────►
          ┌───┬───┬───┬───┬───┬───┬───┬───┬───┐
  堆内存   │ 0 │ 1 │ 2 │ 3 │ 4 │   │   │   │   │
          └───┴───┴───┴───┴───┴───┴───┴───┴───┘
          ↑                       ↑               ↑
        begin_                  end_           end_cap_
          │                       │               │
          │◄──── size = 5 ───────►│               │
          │◄──────── capacity = 9 ───────────────►│
```

```cpp
// vector 内部（简化示意，不同实现细节各异）
template <typename T>
class vector {
    T* begin_;      // 指向第一个元素
    T* end_;        // 指向最后一个元素之后（size = end_ - begin_）
    T* end_cap_;    // 指向已分配内存的末尾（capacity = end_cap_ - begin_）
};
```

### 2.2 增长策略

当 `size == capacity` 时再 push_back，vector 必须：
1. 分配一块更大的内存（通常是当前 capacity 的 **1.5 倍**或 **2 倍**）
2. 把所有旧元素**移动或拷贝**到新内存
3. 释放旧内存

```cpp
#include <vector>
#include <cstdio>

int main()
{
    std::vector<int> v;
    size_t lastCap = 0;

    for (int i = 0; i < 100; ++i) {
        v.push_back(i);
        if (v.capacity() != lastCap) {
            // 每次扩容时打印，观察增长因子
            printf("size=%2zu  capacity=%3zu  growth=%.2f\n",
                   v.size(), v.capacity(),
                   lastCap ? (double)v.capacity() / lastCap : 0.0);
            lastCap = v.capacity();
        }
    }
    // GCC/libstdc++：增长因子 2.0
    // MSVC：增长因子 1.5（更节省内存，但扩容更频繁）
    // Clang/libc++：增长因子 2.0
}
```

**为什么不是每次 +1？** 摊还分析保证 push_back 的平均时间复杂度为 O(1)。如果每次只扩 1 个元素，N 次 push_back 总拷贝量为 O(N²)。

---

## 三、核心操作与性能陷阱

### 3.1 reserve — 预分配的艺术

```cpp
#include <vector>

void processPlayers(int playerCount)
{
    std::vector<PlayerData> players;

    // ❌ 错误做法：不预分配，频繁扩容
    // 如果 playerCount = 10000，可能触发 14 次扩容（2^14 = 16384）
    // 每次扩容都要移动所有已有元素

    // ✅ 正确做法：已知大致数量时预分配
    players.reserve(playerCount);  // 一次性分配足够内存，后续 push_back 零扩容

    for (int i = 0; i < playerCount; ++i) {
        players.push_back(loadPlayer(i));  // 不会触发 reallocation
    }
}
```

### 3.2 shrink_to_fit — 释放多余内存

```cpp
std::vector<int> data;
data.reserve(10000);     // capacity = 10000
// ... 填充后只用了 100 个
data.resize(100);        // size = 100, capacity 仍然 = 10000！

data.shrink_to_fit();    // 请求释放多余内存，capacity 降到 ~100
// 注意：shrink_to_fit 是非绑定请求（non-binding），实现可以忽略
// 实际中主流实现都会执行，但会触发一次 reallocation
```

### 3.3 emplace_back vs push_back

```cpp
#include <vector>
#include <string>

struct Monster {
    std::string name;
    int hp;
    int level;
    Monster(std::string n, int h, int l) : name(std::move(n)), hp(h), level(l) {}
};

int main()
{
    std::vector<Monster> monsters;
    monsters.reserve(100);

    // push_back：先构造临时对象，再移动到 vector 内部
    monsters.push_back(Monster{"Goblin", 100, 5});
    //                 ^^^^^^^^^^^^^^^^^^^^^^^^^ 临时对象在这里构造
    //                                          然后移动构造到 vector 末尾
    //                                          临时对象析构

    // emplace_back：直接在 vector 的内存上原地构造，零临时对象
    monsters.emplace_back("Orc", 250, 10);
    //                    ^^^^^^^^^^^^^^^ 参数直接转发给 Monster 构造函数
    //                                    没有临时对象，没有移动

    // 对于 trivial 类型（int, double）二者无差别
    // 对于持有资源的类型（string, unique_ptr）emplace_back 更优
}
```

### 3.4 erase 的正确姿势：erase-remove 惯用法

```cpp
#include <vector>
#include <algorithm>

void removeDeadMonsters(std::vector<Monster>& monsters)
{
    // ❌ 错误做法：循环中逐个 erase
    // 每次 erase 都要移动后面所有元素，O(N²) 总复杂度
    // for (auto it = monsters.begin(); it != monsters.end(); ) {
    //     if (it->hp <= 0) it = monsters.erase(it);
    //     else ++it;
    // }

    // ✅ 正确做法：erase-remove idiom，O(N) 复杂度
    // remove_if 把"要保留的"紧凑排到前面，返回新的逻辑末尾
    auto newEnd = std::remove_if(monsters.begin(), monsters.end(),
                                 [](const Monster& m) { return m.hp <= 0; });
    // erase 从逻辑末尾到物理末尾，一次性删除
    monsters.erase(newEnd, monsters.end());

    // C++20 更简洁的写法：
    // std::erase_if(monsters, [](const Monster& m) { return m.hp <= 0; });
}
```

---

## 四、迭代器失效 — vector 最大的坑

### 4.1 哪些操作导致失效

| 操作 | 是否失效 | 原因 |
|------|----------|------|
| `push_back` / `emplace_back` | size < capacity 时不失效；扩容时**全部失效** | 扩容导致 reallocation，旧地址全部作废 |
| `insert` | 插入点之后的迭代器失效；可能全部失效 | 可能扩容；即使不扩容也要后移元素 |
| `erase` | 被删元素及之后的迭代器失效 | 后面的元素前移 |
| `clear` / `resize(0)` | 全部失效 | 元素被销毁 |
| `reserve` | 若 capacity 改变则全部失效 | 可能 reallocation |
| `operator[]` / `at` / `front` / `back` | 不影响迭代器 | 只读/写元素，不改结构 |

### 4.2 经典 Bug 示例

```cpp
#include <vector>
#include <cstdio>

int main()
{
    std::vector<int> v = {1, 2, 3, 4, 5};

    // ❌ 悬垂引用！
    int& ref = v[2];        // ref 指向 v 内部地址
    v.push_back(6);         // 如果触发扩容，v 的内存搬到新地址
    printf("%d\n", ref);    // 未定义行为！ref 指向已释放的旧内存

    // ❌ 悬垂迭代器！
    auto it = v.begin();
    v.insert(v.begin(), 0); // 可能 reallocation
    printf("%d\n", *it);    // 未定义行为！

    // ✅ 安全做法：用下标
    size_t idx = 2;
    v.push_back(7);         // 即使扩容，idx 仍然有效
    printf("%d\n", v[idx]); // 安全：通过下标间接访问
}
```

### 4.3 安全模式：地址无关的访问

```cpp
// 如果需要在修改 vector 的同时引用元素，使用下标而非迭代器/引用
// 下标是逻辑位置，不依赖物理地址

void safeIteration(std::vector<int>& v)
{
    // 逆序删除偶数（逆序避免下标移位问题）
    for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i) {
        if (v[i] % 2 == 0) {
            v.erase(v.begin() + i);  // erase 只影响 i 之后的元素
                                      // 而我们是逆序遍历，之后不会访问 i 之后的
        }
    }
}
```

---

## 五、std::array — 编译期固定大小

### 5.1 设计动机

C 风格数组的问题：

```cpp
// C 风格数组
int arr[10];
// 问题1：传参时退化为指针，丢失大小信息
void foo(int* arr);        // 调用者传了多大的数组？不知道
// 问题2：不能拷贝赋值
// int a[10], b[10]; a = b;  // 编译错误
// 问题3：没有 .size()、begin()/end()，与 STL 算法配合困难
```

`std::array` 解决了以上所有问题，且**零开销**：

```cpp
#include <array>
#include <algorithm>

int main()
{
    // 编译期固定大小，数据直接存在栈上（或嵌入结构体中）
    std::array<int, 5> arr = {3, 1, 4, 1, 5};

    // 有 .size()，编译期常量
    static_assert(arr.size() == 5);  // 大小是类型的一部分

    // 支持拷贝赋值
    std::array<int, 5> arr2 = arr;   // 完整拷贝，不是指针别名

    // 完美配合 STL 算法
    std::sort(arr.begin(), arr.end());

    // 支持结构化绑定（C++17）
    auto [a, b, c, d, e] = arr;

    // 越界检查
    // arr[10];    // 未定义行为（同 C 数组）
    // arr.at(10); // 抛出 std::out_of_range 异常（安全版本）
}
```

### 5.2 零开销抽象

```cpp
#include <array>
#include <cstdio>

int main()
{
    std::array<int, 4> a = {1, 2, 3, 4};
    int b[4] = {1, 2, 3, 4};

    // sizeof 完全相同——array 没有任何额外开销
    static_assert(sizeof(a) == sizeof(b));  // 都是 16 字节
    static_assert(sizeof(a) == 4 * sizeof(int));

    // 内存布局完全相同——可以安全地与 C API 互操作
    printf("array data: %p\n", a.data());   // .data() 返回底层裸指针
    // 等价于 &a[0]，可以传给任何接受 int* 的 C 函数
}
```

### 5.3 编译期计算（constexpr）

```cpp
#include <array>
#include <algorithm>

// C++17 起 array 的大部分操作都是 constexpr
constexpr std::array<int, 5> getSorted()
{
    std::array<int, 5> arr = {5, 3, 1, 4, 2};
    // C++20 起 std::sort 也是 constexpr
    std::sort(arr.begin(), arr.end());  // 编译期排序！
    return arr;
}

// 编译期生成查找表，运行时零开销
constexpr auto lookupTable = getSorted();
// lookupTable 在编译期就已排好序，存入 .rodata 段
static_assert(lookupTable[0] == 1);
static_assert(lookupTable[4] == 5);
```

---

## 六、vector vs array 选型对比

| 维度 | std::vector | std::array |
|------|-------------|------------|
| 大小 | 运行时可变 | 编译期固定 |
| 存储位置 | 堆（动态分配） | 栈/嵌入（零堆分配） |
| 分配开销 | 有（new/delete） | 无 |
| 适合场景 | 元素数量不确定、需要动态增长 | 元素数量编译期已知 |
| constexpr 支持 | C++20 起有限支持 | C++17 起完整支持 |
| 传参方式 | 传引用或 span | 传引用（大小是类型一部分）或 span |
| 缓存友好 | 是（连续内存） | 是（连续内存） |

**经验法则：**
- 大小编译期已知 → `std::array`
- 大小运行时确定或需要增长 → `std::vector`
- 纠结时用 `std::vector`——它是"万能"容器

---

## 七、实战场景

### 7.1 游戏服务器：对象池预分配

```cpp
#include <vector>
#include <cstdint>

struct Bullet {
    float x, y;          // 位置
    float vx, vy;        // 速度
    uint32_t ownerId;    // 发射者 ID
    bool active;         // 是否存活
};

class BulletPool {
    std::vector<Bullet> pool_;
    size_t activeCount_ = 0;

public:
    explicit BulletPool(size_t maxBullets)
    {
        // 一次性分配所有内存，运行时零分配
        // 游戏中帧内分配是性能大忌——可能触发 GC 或系统调用
        pool_.resize(maxBullets);
    }

    Bullet* acquire()
    {
        // 从池中找一个空闲的 bullet
        // 因为 pool_ 是连续内存，遍历时缓存命中率极高
        for (auto& b : pool_) {
            if (!b.active) {
                b.active = true;
                ++activeCount_;
                return &b;  // 返回指针安全：pool_ 不会 realloc（大小固定）
            }
        }
        return nullptr;  // 池满
    }

    void release(Bullet* b)
    {
        b->active = false;
        --activeCount_;
    }

    // 每帧更新所有活跃子弹
    void update(float dt)
    {
        // 连续内存遍历，CPU 预取器高效工作
        for (auto& b : pool_) {
            if (b.active) {
                b.x += b.vx * dt;
                b.y += b.vy * dt;
            }
        }
    }
};
```

### 7.2 网络包构造：reserve + emplace_back

```cpp
#include <vector>
#include <cstdint>
#include <cstring>

// 构造一个变长网络包
std::vector<uint8_t> buildPacket(uint16_t msgId, const void* payload, size_t payloadLen)
{
    // 包结构：[2字节消息ID] [4字节长度] [N字节载荷]
    constexpr size_t headerSize = 2 + 4;

    std::vector<uint8_t> packet;
    // 预计算总大小，一次 reserve 到位，避免写入过程中扩容
    packet.reserve(headerSize + payloadLen);

    // 写入消息 ID（大端序）
    packet.push_back(static_cast<uint8_t>(msgId >> 8));
    packet.push_back(static_cast<uint8_t>(msgId & 0xFF));

    // 写入载荷长度（大端序）
    auto len = static_cast<uint32_t>(payloadLen);
    packet.push_back(static_cast<uint8_t>(len >> 24));
    packet.push_back(static_cast<uint8_t>(len >> 16));
    packet.push_back(static_cast<uint8_t>(len >> 8));
    packet.push_back(static_cast<uint8_t>(len & 0xFF));

    // 写入载荷——insert 比逐字节 push_back 更高效（内部可用 memcpy）
    auto* p = static_cast<const uint8_t*>(payload);
    packet.insert(packet.end(), p, p + payloadLen);

    return packet;  // NRVO 或移动语义，零拷贝返回
}
```

### 7.3 编译期查找表：array + constexpr

```cpp
#include <array>
#include <cstdint>

// 编译期生成 CRC32 查找表——运行时直接使用，零初始化开销
constexpr std::array<uint32_t, 256> generateCrc32Table()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            // CRC32 多项式：0xEDB88320（反射形式）
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
        }
        table[i] = crc;
    }
    return table;
}

// 编译期计算完成，存入只读数据段
constexpr auto crc32Table = generateCrc32Table();

// 运行时使用——直接查表，无初始化开销
uint32_t crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        // 查表法：每字节只需一次异或 + 一次查表
        crc = (crc >> 8) ^ crc32Table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}
```

---

## 八、最佳实践总结

1. **默认用 vector**——除非有明确理由用其他容器
2. **已知大小就 reserve**——消除扩容开销，避免迭代器失效
3. **优先 emplace_back**——减少临时对象构造
4. **批量删除用 erase-remove**（C++20 用 `std::erase_if`）
5. **需要引用元素时用下标**——下标不会因 reallocation 失效
6. **编译期已知大小用 array**——零堆分配、constexpr 友好
7. **与 C API 互操作用 `.data()`**——获取底层连续内存指针
8. **小心 `vector<bool>`**——它是特化版本，不是真正的 bool 数组，需要位操作时用 `vector<uint8_t>` 或 `std::bitset`
