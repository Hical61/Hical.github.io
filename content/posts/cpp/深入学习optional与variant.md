+++
date = '2022-05-26'
draft = false
title = '深入学习 std::optional 与 std::variant'
categories = ["C++"]
tags = ["C++", "C++17", "optional", "variant", "STL", "学习笔记"]
description = "深入理解 std::optional 与 std::variant 的设计动机、内存布局、核心 API 与实战模式——告别空指针和裸 union，拥抱类型安全的值语义。"
+++


# 深入学习 std::optional 与 std::variant

> 头文件：`<optional>` / `<variant>`
> 命名空间：`std`
> 编译器要求：C++17 起（C++23 新增 optional 单子操作）

---

## 一、std::optional — "可能没有值"

### 1.1 设计动机：告别哨兵值

在没有 optional 之前，表示"函数可能失败/无结果"的常见手段：

```cpp
// ❌ 方法1：返回指针（语义不清：谁拥有这块内存？要不要 delete？）
Player* findPlayer(uint64_t id);  // 返回 nullptr 表示没找到

// ❌ 方法2：输出参数（调用者被迫声明变量，代码臃肿）
bool findPlayer(uint64_t id, Player& out);

// ❌ 方法3：哨兵值（-1 表示无效？如果 -1 是合法值呢？）
int getScore(const std::string& name);  // 返回 -1 表示没找到

// ❌ 方法4：抛异常（"没找到"不是异常情况，不应该用异常控制流）
Player& findPlayer(uint64_t id);  // 抛 std::runtime_error
```

**每种方法都有缺陷：** 语义不清、容易误用、性能差。

### 1.2 optional 的解法

```cpp
#include <optional>
#include <string>

// ✅ 返回值语义清晰：要么有值，要么没有
std::optional<Player> findPlayer(uint64_t id)
{
    auto it = playerMap.find(id);
    if (it != playerMap.end()) {
        return it->second;     // 隐式构造 optional（有值）
    }
    return std::nullopt;       // 显式表示"无值"
}

void example()
{
    auto result = findPlayer(12345);

    // 检查是否有值
    if (result.has_value()) {        // 或直接 if (result)
        printf("Found: %s\n", result->name.c_str());  // -> 解引用
    }

    // value_or：提供默认值，避免 if-else
    Player p = result.value_or(Player{"Unknown", 0, 0});
}
```

---

## 二、optional 核心 API

### 2.1 构造与赋值

```cpp
#include <optional>
#include <string>

int main()
{
    // 构造方式
    std::optional<int> a;                    // 默认构造：无值（nullopt）
    std::optional<int> b = std::nullopt;     // 显式无值
    std::optional<int> c = 42;              // 有值：42
    std::optional<int> d{std::in_place, 42}; // 原地构造

    // 复杂类型用 emplace 避免临时对象
    std::optional<std::string> name;
    name.emplace("Hello, Optional!");  // 直接在 optional 内部构造 string
                                        // 不会构造临时 string 再移动

    // 重置为无值
    name.reset();          // 析构内部对象，变回 nullopt
    name = std::nullopt;   // 同上

    // 赋新值
    name = "World";        // 内部构造新 string
}
```

### 2.2 访问值

```cpp
#include <optional>
#include <cstdio>

void accessPatterns(std::optional<int> opt)
{
    // 方式1：bool 转换 + 解引用
    if (opt) {
        printf("value = %d\n", *opt);   // * 解引用，不检查（未定义行为如果无值）
    }

    // 方式2：has_value() + value()
    if (opt.has_value()) {
        printf("value = %d\n", opt.value());  // value() 会检查，无值时抛 bad_optional_access
    }

    // 方式3：value_or — 最安全、最简洁
    int val = opt.value_or(-1);  // 有值返回值，无值返回默认值 -1
    printf("value = %d\n", val);
}
```

### 2.3 C++23 单子操作（Monadic Operations）

```cpp
#include <optional>
#include <string>
#include <charconv>

// C++23 新增：transform / and_then / or_else
// 允许链式处理 optional，不需要层层 if 判断

// 辅助函数：字符串转 int
std::optional<int> parseInt(std::string_view sv)
{
    int val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc{}) return val;
    return std::nullopt;
}

// 辅助函数：验证范围
std::optional<int> validateRange(int val)
{
    if (val >= 1 && val <= 100) return val;
    return std::nullopt;
}

void monadicExample()
{
    std::optional<std::string> input = "42";

    // transform：对 optional 内的值应用函数，结果仍是 optional
    // 如果 input 无值，直接返回 nullopt（不调用 lambda）
    auto len = input.transform([](const std::string& s) {
        return s.size();  // optional<string> → optional<size_t>
    });

    // and_then：对值应用返回 optional 的函数（flatMap）
    // 适合链接多个可能失败的操作
    auto result = input
        .and_then([](const std::string& s) { return parseInt(s); })   // optional<int>
        .and_then([](int val) { return validateRange(val); })          // optional<int>
        .transform([](int val) { return val * 2; });                   // optional<int>
    // result = optional<int>(84)

    // or_else：无值时提供备选逻辑
    auto final_val = result.or_else([]() -> std::optional<int> {
        printf("All parsing failed, using default\n");
        return 0;  // 提供默认值
    });
}
```

---

## 三、optional 内存布局

### 3.1 零开销？几乎

```cpp
#include <optional>
#include <cstdio>

int main()
{
    printf("sizeof(int):              %zu\n", sizeof(int));               // 4
    printf("sizeof(optional<int>):    %zu\n", sizeof(std::optional<int>)); // 8
    printf("sizeof(double):           %zu\n", sizeof(double));            // 8
    printf("sizeof(optional<double>): %zu\n", sizeof(std::optional<double>)); // 16

    // optional 多了一个 bool 标记 + 对齐填充
    // 内存布局（简化）：
    // ┌──────────────────────┬────────┐
    // │     T value           │ bool   │ + padding
    // └──────────────────────┴────────┘
}
```

### 3.2 与指针对比

```cpp
// optional<T> vs T*

// optional<T>：
// - 值语义（可拷贝、可赋值、析构自动清理）
// - 栈上存储，cache 友好
// - 大小 = sizeof(T) + 1 byte（+ alignment padding）
// - 无堆分配

// T*：
// - 指针语义（谁 new 谁 delete？悬垂风险）
// - 指向堆内存，可能 cache miss
// - 大小 = 8 bytes（x64）
// - 可能需要堆分配

// 结论：小对象用 optional，大对象或需要多态时用 unique_ptr
```

---

## 四、std::variant — 类型安全的 union

### 4.1 设计动机：告别裸 union

```cpp
// ❌ C 风格 union——类型不安全
union Value {
    int i;
    double d;
    char* s;  // 谁来管理 s 的内存？析构时怎么知道当前存的什么？
};

struct TaggedValue {
    enum Type { Int, Double, String } type;
    union {
        int i;
        double d;
        char* s;
    };
    // 问题1：type 和 union 内容可能不一致（程序员忘了更新 type）
    // 问题2：如果存了 string，析构时必须手动 free(s)——容易忘
    // 问题3：不能存有构造/析构函数的类型（如 std::string）
};
```

### 4.2 variant 的解法

```cpp
#include <variant>
#include <string>
#include <cstdio>

// variant：编译器帮你管理类型标签、析构和构造
using ConfigValue = std::variant<int, double, std::string, bool>;
//                               ↑ 索引 0   ↑ 索引 1   ↑ 索引 2   ↑ 索引 3

int main()
{
    ConfigValue val = 42;            // 当前存储 int
    val = 3.14;                      // 自动析构 int，构造 double
    val = std::string("hello");      // 自动析构 double，构造 string
    // val 析构时自动调用 string 的析构函数——零泄漏

    // 编译期检查：不能存放未声明的类型
    // val = std::vector<int>{};     // 编译错误！vector<int> 不在类型列表中
}
```

---

## 五、variant 核心 API

### 5.1 类型查询

```cpp
#include <variant>
#include <string>
#include <cstdio>

using Value = std::variant<int, double, std::string>;

void inspect(const Value& v)
{
    // index()：返回当前激活类型的索引（0-based）
    printf("index = %zu\n", v.index());  // int→0, double→1, string→2

    // holds_alternative<T>：检查当前是否存储特定类型
    if (std::holds_alternative<int>(v)) {
        printf("It's an int: %d\n", std::get<int>(v));
    }

    // get<T>：获取值（类型不匹配时抛 bad_variant_access）
    // get<I>：按索引获取

    // get_if<T>：安全版本，返回指针（不匹配返回 nullptr）
    if (auto* p = std::get_if<std::string>(&v)) {
        printf("It's a string: %s\n", p->c_str());
    }
}
```

### 5.2 std::visit — 模式匹配

```cpp
#include <variant>
#include <string>
#include <cstdio>

using Value = std::variant<int, double, std::string>;

// visit：对 variant 当前持有的值执行对应的处理函数
// 编译器确保所有类型都被处理——遗漏会编译错误

// 方法1：函数对象（重载 operator()）
struct ValuePrinter {
    void operator()(int val) const         { printf("int: %d\n", val); }
    void operator()(double val) const      { printf("double: %.2f\n", val); }
    void operator()(const std::string& val) const { printf("string: %s\n", val.c_str()); }
};

void example1()
{
    Value v = 3.14;
    std::visit(ValuePrinter{}, v);  // 输出: double: 3.14
}

// 方法2：overloaded 惯用法（C++17 lambda + 模板推导）
template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
// C++17 需要推导指引（C++20 起不需要）
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

void example2()
{
    Value v = std::string("hello");

    // 用多个 lambda 覆盖所有类型——最优雅的模式匹配
    std::visit(overloaded{
        [](int val)               { printf("int: %d\n", val); },
        [](double val)            { printf("double: %.2f\n", val); },
        [](const std::string& val){ printf("string: %s\n", val.c_str()); }
    }, v);
}

// 方法3：返回值
std::string toString(const Value& v)
{
    return std::visit(overloaded{
        [](int val)               { return std::to_string(val); },
        [](double val)            { return std::to_string(val); },
        [](const std::string& val){ return val; }
    }, v);
}
```

### 5.3 visit 多个 variant（笛卡尔积）

```cpp
#include <variant>
#include <string>
#include <cstdio>

using Operand = std::variant<int, double>;

// visit 可以同时接受多个 variant——编译器生成所有组合的分发表
Operand add(const Operand& lhs, const Operand& rhs)
{
    return std::visit(overloaded{
        [](int a, int b)       -> Operand { return a + b; },
        [](int a, double b)    -> Operand { return a + b; },
        [](double a, int b)    -> Operand { return a + b; },
        [](double a, double b) -> Operand { return a + b; },
    }, lhs, rhs);
    // 2×2 = 4 种组合，都必须覆盖
}
```

---

## 六、variant 内存布局与异常安全

### 6.1 内存布局

```cpp
#include <variant>
#include <string>
#include <cstdio>

int main()
{
    // variant 的大小 = max(sizeof(各类型)) + 类型索引字段 + 对齐
    printf("sizeof(variant<int, double, string>): %zu\n",
           sizeof(std::variant<int, double, std::string>));
    // 通常 = 40 (string=32 + index=8 with alignment)

    printf("sizeof(variant<int, double>): %zu\n",
           sizeof(std::variant<int, double>));
    // 通常 = 16 (double=8 + index=8 with alignment)

    // 对比手动 tagged union：大小相同，但 variant 是类型安全的
}
```

```
variant<int, double, string> 内存布局：

┌────────────────────────────────────────────┬─────────┐
│          存储区（对齐到最大类型）              │ index_  │
│  max(sizeof(int), sizeof(double),          │ (size_t)│
│       sizeof(string)) = 32 bytes           │         │
└────────────────────────────────────────────┴─────────┘
│◄──────────── sizeof(variant) ─────────────────────────►│
```

### 6.2 valueless_by_exception

```cpp
#include <variant>
#include <string>
#include <stdexcept>

struct ThrowOnCopy {
    ThrowOnCopy() = default;
    ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("boom"); }
};

int main()
{
    std::variant<int, ThrowOnCopy> v = 42;  // 当前存 int

    try {
        ThrowOnCopy t;
        v = t;  // 赋值过程：
                // 1. 析构旧值（int）  ← 已执行
                // 2. 拷贝构造新值     ← 抛异常！
                // 此时 variant 处于"无值"状态
    } catch (...) {}

    // variant 进入特殊状态：valueless_by_exception
    printf("valueless: %d\n", v.valueless_by_exception());  // 1 (true)
    printf("index: %zu\n", v.index());                       // variant::npos

    // 这是唯一能让 variant "无值"的方式——赋值过程中异常
    // 实践中很少发生：避免在 variant 类型中使用可能抛异常的拷贝/移动构造
}
```

---

## 七、optional vs variant vs 指针

| 维度   | optional\<T\>    | variant\<Ts...\>       | unique_ptr\<T\>      |
| ------ | ---------------- | ---------------------- | -------------------- |
| 语义   | 值可能不存在     | 值是多种类型之一       | 堆上的唯一对象       |
| 存储   | 栈/内嵌          | 栈/内嵌                | 堆                   |
| 多态   | 否               | 编译期多态             | 运行时多态（虚函数） |
| 空状态 | nullopt          | 无（除非 valueless）   | nullptr              |
| 开销   | sizeof(T)+1+pad  | max(sizeof(Ts...))+pad | 指针大小+堆分配      |
| 适用   | 函数可能无返回值 | 有限类型集的和类型     | 需要多态/大对象      |

---

## 八、实战场景

### 8.1 协议消息分发

```cpp
#include <variant>
#include <string>
#include <vector>
#include <cstdint>

// 定义协议消息类型
struct LoginRequest {
    std::string username;
    std::string password;
};

struct MoveRequest {
    float x, y, z;
    uint32_t timestamp;
};

struct ChatMessage {
    uint32_t channelId;
    std::string content;
};

// variant 作为消息容器——类型安全，无堆分配
using GameMessage = std::variant<LoginRequest, MoveRequest, ChatMessage>;

// 消息处理器——编译期保证所有消息类型都被处理
class MessageHandler {
public:
    void handle(const GameMessage& msg)
    {
        std::visit(overloaded{
            [this](const LoginRequest& req) {
                // 验证用户名密码
                printf("Login: %s\n", req.username.c_str());
                authenticatePlayer(req.username, req.password);
            },
            [this](const MoveRequest& req) {
                // 更新玩家位置
                printf("Move to (%.1f, %.1f, %.1f)\n", req.x, req.y, req.z);
                updatePosition(req.x, req.y, req.z);
            },
            [this](const ChatMessage& msg) {
                // 广播聊天
                printf("Chat [%u]: %s\n", msg.channelId, msg.content.c_str());
                broadcastChat(msg.channelId, msg.content);
            }
        }, msg);
    }

private:
    void authenticatePlayer(const std::string&, const std::string&) { /* ... */ }
    void updatePosition(float, float, float) { /* ... */ }
    void broadcastChat(uint32_t, const std::string&) { /* ... */ }
};
```

### 8.2 配置系统：多类型值

```cpp
#include <variant>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// 配置值：可能是 int / double / string / bool / 数组
using ConfigValue = std::variant<int, double, std::string, bool, std::vector<std::string>>;

class Config {
    std::unordered_map<std::string, ConfigValue> data_;

public:
    // 获取指定类型的配置值
    template <typename T>
    std::optional<T> get(const std::string& key) const
    {
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;  // key 不存在

        // get_if 返回指针：类型匹配返回非空，不匹配返回 nullptr
        if (auto* val = std::get_if<T>(&it->second)) {
            return *val;  // 类型匹配，返回值
        }
        return std::nullopt;  // key 存在但类型不匹配
    }

    // 获取值并提供默认值
    template <typename T>
    T getOr(const std::string& key, T defaultVal) const
    {
        return get<T>(key).value_or(std::move(defaultVal));
    }

    void set(const std::string& key, ConfigValue val)
    {
        data_.insert_or_assign(key, std::move(val));
    }
};

void example()
{
    Config cfg;
    cfg.set("port", 8080);
    cfg.set("host", std::string("0.0.0.0"));
    cfg.set("debug", true);
    cfg.set("workers", 4);

    // 类型安全的获取
    int port = cfg.getOr<int>("port", 3000);             // 8080
    std::string host = cfg.getOr<std::string>("host", "localhost");  // "0.0.0.0"
    bool debug = cfg.getOr<bool>("debug", false);        // true

    // 类型不匹配返回 nullopt
    auto wrong = cfg.get<double>("port");  // nullopt（port 是 int 不是 double）
}
```

### 8.3 错误处理：Expected 模式（variant 实现 Result 类型）

```cpp
#include <variant>
#include <string>

// C++23 有 std::expected，C++17 可以用 variant 模拟
template <typename T, typename E = std::string>
class Result {
    std::variant<T, E> data_;

public:
    // 成功构造
    static Result ok(T value) {
        Result r;
        r.data_ = std::move(value);
        return r;
    }

    // 失败构造
    static Result err(E error) {
        Result r;
        r.data_ = std::move(error);
        return r;
    }

    bool isOk() const { return data_.index() == 0; }
    bool isErr() const { return data_.index() == 1; }

    T& value() { return std::get<0>(data_); }
    const T& value() const { return std::get<0>(data_); }

    E& error() { return std::get<1>(data_); }
    const E& error() const { return std::get<1>(data_); }
};

// 使用示例
Result<int> divide(int a, int b)
{
    if (b == 0) {
        return Result<int>::err("Division by zero");
    }
    return Result<int>::ok(a / b);
}

void example()
{
    auto result = divide(10, 0);
    if (result.isOk()) {
        printf("Result: %d\n", result.value());
    } else {
        printf("Error: %s\n", result.error().c_str());
    }
}
```

---

## 九、最佳实践总结

### optional

1. **函数可能无结果时返回 optional**——比指针、哨兵值、输出参数都更清晰
2. **小对象直接用 optional**——栈上存储，零堆分配
3. **大对象考虑 optional\<unique_ptr\<T\>\>**——避免 optional 体积膨胀
4. **优先用 value_or**——减少 if-else 分支
5. **C++23 用 and_then/transform 链式处理**——告别嵌套 if
6. **不要用 optional\<reference\>**——标准不支持，用指针或 `std::reference_wrapper`

### variant

1. **有限类型集的和类型用 variant**——替代继承多态（无虚函数开销）
2. **用 visit + overloaded 做模式匹配**——编译期保证完备性
3. **避免在 variant 类型中使用可能抛异常的拷贝构造**——防止 valueless 状态
4. **variant 替代 union + tag**——类型安全、自动析构
5. **消息分发/事件系统优先考虑 variant**——编译期检查优于运行时 dynamic_cast
6. **类型数量过多（>10）时考虑继承多态**——variant visit 的编译时间随类型数指数增长
