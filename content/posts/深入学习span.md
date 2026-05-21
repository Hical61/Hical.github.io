+++
date = '2022-05-25'
draft = false
title = '深入学习 std::span'
categories = ["C++"]
tags = ["C++", "C++20", "span", "STL", "零拷贝", "学习笔记"]
description = "深入理解 std::span 的设计哲学——非拥有、轻量级的连续内存视图，统一 vector/array/C数组的传参方式，实现零拷贝的安全缓冲区操作。"
+++


# 深入学习 std::span

> 头文件：`<span>`
> 命名空间：`std`
> 编译器要求：C++20 起

---

## 一、设计动机：统一连续内存的访问接口

### 1.1 C++ 中连续内存的 N 种传参方式

在没有 span 之前，传递"一段连续内存"的方式五花八门：

```cpp
// 方式1：C 风格——指针 + 长度（容易出错，长度可能传错）
void process(const int* data, size_t len);

// 方式2：模板——编译膨胀，每种容器实例化一份
template <typename Container>
void process(const Container& c);

// 方式3：特化 vector 引用——不接受 array 或 C 数组
void process(const std::vector<int>& v);

// 方式4：迭代器对——语法啰嗦，不直观
template <typename Iter>
void process(Iter begin, Iter end);
```

**核心问题：** 没有一种统一的、类型安全的方式说"我只需要一段连续内存的只读/可写视图"。

### 1.2 span 的解法

```cpp
#include <span>
#include <vector>
#include <array>
#include <cstdio>

// ✅ 一个函数接受所有连续内存容器
void process(std::span<const int> data)
{
    for (int val : data) {
        printf("%d ", val);
    }
    printf("\n");
}

int main()
{
    // span 能从任何连续内存容器隐式构造
    std::vector<int> vec = {1, 2, 3, 4, 5};
    std::array<int, 3> arr = {10, 20, 30};
    int cArr[] = {100, 200, 300, 400};

    process(vec);        // vector → span：隐式转换
    process(arr);        // array → span：隐式转换
    process(cArr);       // C 数组 → span：隐式转换
    process({vec.data() + 1, 3});  // 子区间：手动指定 {ptr, count}
}
```

**一句话总结：span 是连续内存的"通用视图"——不拥有数据、不分配内存、只是指针+长度的薄封装。**

---

## 二、span 的本质：指针 + 长度

### 2.1 内部结构

```cpp
// span 内部（简化）
template <typename T, size_t Extent = std::dynamic_extent>
class span {
    T* ptr_;          // 指向数据起始位置
    size_t size_;     // 元素个数（动态 extent 时才有此成员）
    // 静态 extent 时 size_ 不存在——大小是类型的一部分
};
```

```
std::span<int>（动态 extent）：
┌──────────┬──────────┐
│  ptr_    │  size_   │    = 16 bytes on x64
└──────────┴──────────┘
      │
      ▼
┌───┬───┬───┬───┬───┐
│ 1 │ 2 │ 3 │ 4 │ 5 │  ← 实际数据（不属于 span）
└───┴───┴───┴───┴───┘

std::span<int, 5>（静态 extent）：
┌──────────┐
│  ptr_    │              = 8 bytes on x64（size 是编译期常量）
└──────────┘
      │
      ▼
┌───┬───┬───┬───┬───┐
│ 1 │ 2 │ 3 │ 4 │ 5 │
└───┴───┴───┴───┴───┘
```

### 2.2 与 string_view 的对比

| 维度 | std::span\<T\> | std::string_view |
|------|---------------|-----------------|
| 数据类型 | 任意类型 T | 仅 char |
| 可变性 | span\<T\> 可修改；span\<const T\> 只读 | 始终只读 |
| 用途 | 通用连续内存视图 | 字符串只读视图 |
| 空终止 | 不要求 | 不要求（但底层可能是） |
| sizeof | 16 bytes（动态）/ 8 bytes（静态） | 16 bytes |

**span 是 string_view 的泛化版本——string_view ≈ span\<const char\>（语义上）。**

---

## 三、静态 Extent vs 动态 Extent

### 3.1 动态 Extent（默认）

```cpp
#include <span>
#include <vector>

// 默认：动态 extent，大小在运行时确定
void dynamicSpan(std::span<int> s)  // 等价于 std::span<int, std::dynamic_extent>
{
    // s.size() 是运行时值
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] *= 2;  // 可以修改（非 const）
    }
}

int main()
{
    std::vector<int> v = {1, 2, 3, 4, 5};
    dynamicSpan(v);                      // size = 5
    dynamicSpan({v.data(), 3});          // size = 3（只看前3个）
    // 同一个函数接受任意长度——灵活
}
```

### 3.2 静态 Extent

```cpp
#include <span>
#include <array>

// 静态 extent：大小是类型的一部分，编译期检查
void fixedSpan(std::span<int, 4> s)
{
    // s.size() 是编译期常量 4
    static_assert(s.size() == 4);  // 可以 static_assert！

    // 编译器可以做更多优化（循环展开、向量化）
    for (auto& val : s) {
        val += 10;
    }
}

int main()
{
    std::array<int, 4> arr = {1, 2, 3, 4};
    fixedSpan(arr);       // ✅ 大小匹配

    int cArr[4] = {5, 6, 7, 8};
    fixedSpan(cArr);      // ✅ C 数组大小匹配

    std::array<int, 5> big = {1, 2, 3, 4, 5};
    // fixedSpan(big);    // ❌ 编译错误！大小不匹配（5 != 4）

    // 从动态 span 显式转换到静态 span
    std::vector<int> v = {1, 2, 3, 4, 5, 6};
    auto sub = std::span<int>(v).first<4>();  // 取前4个，返回 span<int, 4>
    fixedSpan(sub);       // ✅
}
```

### 3.3 选择建议

| 场景 | 选择 | 理由 |
|------|------|------|
| 通用接口、大小运行时确定 | 动态 extent | 灵活性 |
| 固定大小的协议字段 | 静态 extent | 编译期检查 + 优化 |
| 接受任意子区间 | 动态 extent | 不限制调用者 |
| 嵌入式/实时系统 | 静态 extent | 零运行时开销 |

---

## 四、核心 API

### 4.1 构造

```cpp
#include <span>
#include <vector>
#include <array>

int main()
{
    // 从 vector
    std::vector<int> v = {1, 2, 3, 4, 5};
    std::span<int> s1(v);                    // 整个 vector
    std::span<int> s2(v.data(), 3);          // 前 3 个元素
    std::span<int> s3(v.begin(), v.end());   // 迭代器区间（C++20 要求连续迭代器）

    // 从 array
    std::array<int, 5> a = {10, 20, 30, 40, 50};
    std::span<int, 5> s4(a);                 // 静态 extent
    std::span<int> s5(a);                    // 也可以动态 extent

    // 从 C 数组
    int c[] = {100, 200, 300};
    std::span<int> s6(c);                    // 自动推导大小

    // 空 span
    std::span<int> empty;                    // size = 0, data = nullptr
}
```

### 4.2 子视图操作

```cpp
#include <span>
#include <vector>
#include <cstdio>

void subviewDemo()
{
    std::vector<int> v = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::span<int> s(v);

    // first(n)：取前 n 个元素
    auto head = s.first(3);       // [0, 1, 2]

    // last(n)：取后 n 个元素
    auto tail = s.last(3);        // [7, 8, 9]

    // subspan(offset, count)：从 offset 开始取 count 个
    auto mid = s.subspan(3, 4);   // [3, 4, 5, 6]

    // subspan(offset)：从 offset 到末尾
    auto rest = s.subspan(5);     // [5, 6, 7, 8, 9]

    // 静态模板版本（编译期确定大小）
    auto staticHead = s.first<3>();     // 返回 span<int, 3>
    auto staticMid  = s.subspan<2, 4>(); // 返回 span<int, 4>

    // 子视图不拷贝数据——只是调整 ptr_ 和 size_
    // 修改子视图会修改原始数据
    mid[0] = 999;  // v[3] 也变成 999
}
```

### 4.3 元素访问

```cpp
#include <span>

void accessDemo(std::span<int> s)
{
    // operator[]：无边界检查（同 vector）
    int first = s[0];
    int last  = s[s.size() - 1];

    // front() / back()
    int f = s.front();  // 等价于 s[0]
    int b = s.back();   // 等价于 s[s.size()-1]

    // data()：底层指针
    int* ptr = s.data();

    // size() / size_bytes() / empty()
    size_t count = s.size();                // 元素个数
    size_t bytes = s.size_bytes();          // 总字节数 = size() * sizeof(T)
    bool isEmpty = s.empty();              // size() == 0

    // 迭代器
    for (auto it = s.begin(); it != s.end(); ++it) {
        *it += 1;
    }

    // 范围 for
    for (int& val : s) {
        val *= 2;
    }
}
```

### 4.4 类型转换：as_bytes / as_writable_bytes

```cpp
#include <span>
#include <cstdio>

void byteLevelAccess()
{
    int data[] = {0x12345678, 0xAABBCCDD};
    std::span<int> intSpan(data);

    // as_bytes：将 span<T> 转为 span<const std::byte>
    // 用于序列化、网络发送等需要字节级访问的场景
    std::span<const std::byte> byteView = std::as_bytes(intSpan);
    printf("byte count: %zu\n", byteView.size());  // 8（2个int × 4字节）

    // 查看第一个 int 的字节表示（小端序）
    for (auto b : byteView.first(4)) {
        printf("%02x ", std::to_integer<uint8_t>(b));
    }
    // 输出（小端）: 78 56 34 12

    // as_writable_bytes：可写版本
    std::span<std::byte> writableBytes = std::as_writable_bytes(intSpan);
    writableBytes[0] = std::byte{0xFF};  // 修改第一个字节
    // data[0] 变成 0x123456FF（小端）
}
```

---

## 五、生命周期——span 最重要的注意事项

### 5.1 span 不拥有数据

```cpp
#include <span>
#include <vector>

// ❌ 危险：返回指向临时对象的 span
std::span<int> dangerous()
{
    std::vector<int> local = {1, 2, 3};
    return std::span<int>(local);  // local 析构后 span 悬垂！
}

// ❌ 危险：span 存活时底层数据被修改
void alsoaDangerous()
{
    std::vector<int> v = {1, 2, 3};
    std::span<int> s(v);

    v.push_back(4);  // 可能触发 reallocation！
    // s 现在指向已释放的内存——悬垂！

    // 甚至 v.clear() 后 s 也会悬垂
}

// ✅ 安全：span 的生命周期短于数据
void safe(std::span<const int> data)
{
    // data 只在函数内使用，调用者保证数据有效
    for (int val : data) { /* ... */ }
}

// ✅ 安全：span 作为参数（最常见用法）
void processBuffer(std::span<const std::byte> buffer);
```

### 5.2 经验法则

1. **span 主要用作函数参数**——调用者拥有数据，函数内使用 span 访问
2. **不要把 span 存入类成员**——除非能保证数据生命周期更长
3. **不要返回指向局部变量的 span**——与返回指针/引用同理
4. **span 生存期必须 ≤ 底层数据生存期**

---

## 六、实战场景

### 6.1 网络包零拷贝解析

```cpp
#include <span>
#include <cstdint>
#include <cstring>
#include <optional>
#include <cstdio>

struct PacketHeader {
    uint16_t msgId;
    uint32_t bodyLen;
};

// 解析网络包——零拷贝，只在原始缓冲区上移动视图
class PacketParser {
public:
    explicit PacketParser(std::span<const uint8_t> buffer)
        : remaining_(buffer)  // span 赋值只是拷贝指针+长度，不拷贝数据
    {}

    // 读取 N 个字节并前进
    std::optional<std::span<const uint8_t>> consume(size_t n)
    {
        if (remaining_.size() < n) {
            return std::nullopt;  // 数据不足
        }
        auto result = remaining_.first(n);    // 取前 n 字节视图
        remaining_ = remaining_.subspan(n);   // 移动视图起点（零拷贝）
        return result;
    }

    // 解析包头
    std::optional<PacketHeader> parseHeader()
    {
        auto headerBytes = consume(6);  // 2 + 4 = 6 字节
        if (!headerBytes) return std::nullopt;

        PacketHeader hdr;
        // 从 span 读取（注意字节序，这里简化为小端直接读）
        std::memcpy(&hdr.msgId, headerBytes->data(), 2);
        std::memcpy(&hdr.bodyLen, headerBytes->data() + 2, 4);
        return hdr;
    }

    // 解析包体
    std::optional<std::span<const uint8_t>> parseBody(uint32_t len)
    {
        return consume(len);  // 返回 body 区间的视图——不拷贝数据
    }

    size_t remaining() const { return remaining_.size(); }

private:
    std::span<const uint8_t> remaining_;  // 剩余未解析数据的视图
};

void handlePacket(std::span<const uint8_t> rawData)
{
    PacketParser parser(rawData);

    while (parser.remaining() >= 6) {
        auto hdr = parser.parseHeader();
        if (!hdr) break;

        auto body = parser.parseBody(hdr->bodyLen);
        if (!body) break;

        printf("Msg %u, body %u bytes\n", hdr->msgId, hdr->bodyLen);
        // 处理 body...（body 是原始缓冲区的视图，零拷贝）
    }
}
```

### 6.2 安全的缓冲区传递

```cpp
#include <span>
#include <vector>
#include <cstring>
#include <cstdio>

// 传统 C 接口——不安全
// void fillBuffer(char* buf, size_t len);  // 调用者可能传错 len

// ✅ span 版本——大小自描述，类型安全
void fillBuffer(std::span<char> buf)
{
    // span 自带大小信息，不可能越界（只要正确使用 API）
    size_t toWrite = std::min(buf.size(), size_t(13));
    std::memcpy(buf.data(), "Hello, span!", toWrite);
}

// 接受只读数据的通用接口
size_t computeChecksum(std::span<const std::byte> data)
{
    size_t sum = 0;
    for (auto b : data) {
        sum += std::to_integer<size_t>(b);
    }
    return sum;
}

int main()
{
    // 各种容器都能无缝传入
    char stackBuf[64];
    fillBuffer(stackBuf);  // C 数组 → span<char>
    printf("%s\n", stackBuf);

    std::vector<char> heapBuf(128);
    fillBuffer(heapBuf);  // vector → span<char>

    // 计算校验和——接受任何连续内存的字节视图
    std::vector<uint8_t> packet = {0x01, 0x02, 0x03};
    auto checksum = computeChecksum(std::as_bytes(std::span(packet)));
    printf("checksum: %zu\n", checksum);  // 6
}
```

### 6.3 多维数据的切片

```cpp
#include <span>
#include <vector>
#include <cstdio>

// 2D 矩阵按行存储（row-major）
class Matrix {
    std::vector<float> data_;
    size_t rows_, cols_;

public:
    Matrix(size_t rows, size_t cols)
        : data_(rows * cols, 0.0f), rows_(rows), cols_(cols) {}

    // 返回第 i 行的视图——零拷贝
    std::span<float> row(size_t i)
    {
        return std::span<float>(data_).subspan(i * cols_, cols_);
    }

    std::span<const float> row(size_t i) const
    {
        return std::span<const float>(data_).subspan(i * cols_, cols_);
    }

    // 对某一行执行操作
    void scaleRow(size_t i, float factor)
    {
        for (float& val : row(i)) {
            val *= factor;  // 直接修改原始数据
        }
    }

    size_t rows() const { return rows_; }
    size_t cols() const { return cols_; }
};

void matrixDemo()
{
    Matrix m(3, 4);  // 3行4列

    // 通过 span 设置行数据
    auto r0 = m.row(0);
    for (size_t j = 0; j < r0.size(); ++j) {
        r0[j] = static_cast<float>(j + 1);  // 第0行 = [1, 2, 3, 4]
    }

    m.scaleRow(0, 2.0f);  // 第0行 ×2 → [2, 4, 6, 8]

    // 遍历打印
    for (size_t i = 0; i < m.rows(); ++i) {
        for (float val : m.row(i)) {
            printf("%.1f ", val);
        }
        printf("\n");
    }
}
```

---

## 七、span vs 其他传参方式对比

| 方式 | 适用场景 | 优点 | 缺点 |
|------|----------|------|------|
| `span<T>` | 通用连续内存视图 | 统一接口、零拷贝 | 不拥有数据，需注意生命周期 |
| `const vector<T>&` | 只接受 vector | 明确所有权 | 排斥 array/C数组 |
| `T*, size_t` | C 互操作 | 兼容 C | 类型不安全，易传错长度 |
| `string_view` | 字符串只读 | 零拷贝 | 仅限字符类型 |
| 模板 `Container&` | 需要完整容器功能 | 可调用 push_back 等 | 编译膨胀 |

**经验法则：** 如果函数只需要"读/写一段连续内存"，用 `span`。

---

## 八、最佳实践总结

1. **函数参数用 span 替代 `const vector<T>&`**——接受更多类型的调用者
2. **span 主要用作参数和局部变量**——不要存入类成员（除非能管理生命周期）
3. **只读访问用 `span<const T>`**——传达意图，防止意外修改
4. **已知大小用静态 extent**——编译期检查 + 更好的优化
5. **网络/IO 层优先用 `span<const std::byte>`**——类型安全的字节视图
6. **子区间操作用 first/last/subspan**——零拷贝切片
7. **不要对可能 realloc 的容器长期持有 span**——push_back 后 span 可能悬垂
8. **与 C API 互操作用 `.data()` + `.size()`**——span 到 C 接口的桥梁
