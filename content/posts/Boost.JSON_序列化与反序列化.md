+++
title = 'Boost.JSON 学习课程：JSON 序列化与反序列化'
date = '2026-04-15'
draft = false
tags = ["Boost", "Boost.JSON", "JSON", "PMR", "反射", "Hical"]
categories = ["Boost学习课程"]
description = "掌握 Boost.JSON 的值类型体系、安全解析与序列化，理解 PMR 分配器加速 JSON 操作，以及 Hical MetaJson 反射层的实现原理。"
+++

> **课程导航**：[学习路径]({{< relref "posts/Boost库学习课程_学习路径导航.md" >}}) | [Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}}) | [Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}}) | [Boost.Beast]({{< relref "posts/Boost.Beast_HTTP与WebSocket.md" >}}) | **Boost.JSON** | [Boost.MySQL]({{< relref "posts/Boost.MySQL_异步数据库访问.md" >}})

## 前置知识

- [课程 1: Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}})（`error_code` 用于安全解析）
- C++ 基础：模板、`if constexpr`、可变参数模板
- JSON 数据格式基础

## 学习目标

完成本课程后，你将能够：
1. 掌握 Boost.JSON 的值类型体系（`value`/`object`/`array`）
2. 安全解析和序列化 JSON 数据
3. 理解 PMR 分配器如何加速 JSON 操作
4. 读懂 Hical 的 MetaJson 反射层——自动 JSON 序列化的实现原理

---

## 目录

- [前置知识](#前置知识)
- [学习目标](#学习目标)
- [目录](#目录)
- [1. 核心概念](#1-核心概念)
  - [1.1 Boost.JSON vs 其他 JSON 库](#11-boostjson-vs-其他-json-库)
  - [1.2 值类型体系](#12-值类型体系)
  - [1.3 构造与访问](#13-构造与访问)
- [2. 基础用法](#2-基础用法)
  - [2.1 创建 JSON 值](#21-创建-json-值)
  - [2.2 解析 JSON 字符串](#22-解析-json-字符串)
  - [2.3 序列化为字符串](#23-序列化为字符串)
  - [2.4 访问和修改](#24-访问和修改)
  - [2.5 类型转换](#25-类型转换)
- [3. 进阶主题](#3-进阶主题)
  - [3.1 PMR 分配器集成](#31-pmr-分配器集成)
  - [3.2 增量解析](#32-增量解析)
  - [3.3 tag\_invoke 自定义序列化](#33-tag_invoke-自定义序列化)
  - [3.4 错误处理](#34-错误处理)
- [4. Hical 实战解读](#4-hical-实战解读)
  - [4.1 HttpRequest::jsonBody()](#41-httprequestjsonbody)
  - [4.2 HttpResponse::setJsonBody()](#42-httpresponsesetjsonbody)
  - [4.3 HttpResponse::json() 工厂](#43-httpresponsejson-工厂)
  - [4.4 MetaJson.h：反射驱动的自动序列化](#44-metajsonh反射驱动的自动序列化)
  - [4.5 PMR 与 JSON 的协同](#45-pmr-与-json-的协同)
- [5. 练习题](#5-练习题)
  - [练习 1：JSON 解析与提取](#练习-1json-解析与提取)
  - [练习 2：HICAL\_JSON 宏实战](#练习-2hical_json-宏实战)
  - [练习 3：安全 JSON 验证器](#练习-3安全-json-验证器)
  - [练习 4：PMR 性能对比](#练习-4pmr-性能对比)
  - [练习 5（挑战）：扩展 valueToJson](#练习-5挑战扩展-valuetojson)
- [参考答案](#参考答案)
  - [练习 1 参考答案：JSON 解析与提取](#练习-1-参考答案json-解析与提取)
  - [练习 2 参考答案：HICAL\_JSON 宏实战](#练习-2-参考答案hical_json-宏实战)
  - [练习 3 参考答案：安全 JSON 验证器](#练习-3-参考答案安全-json-验证器)
  - [练习 4 参考答案：PMR 性能对比](#练习-4-参考答案pmr-性能对比)
  - [练习 5 参考答案：扩展 valueToJson 支持 optional](#练习-5-参考答案扩展-valuetojson-支持-optional)
- [6. 总结与拓展阅读](#6-总结与拓展阅读)
  - [C++ 类型 ↔ JSON 类型映射表](#c-类型--json-类型映射表)
  - [API 速查表](#api-速查表)
  - [拓展阅读](#拓展阅读)
  - [课程回顾](#课程回顾)

---

## 1. 核心概念

### 1.1 Boost.JSON vs 其他 JSON 库

| 特性       | Boost.JSON             | nlohmann::json | RapidJSON        | simdjson |
| ---------- | ---------------------- | -------------- | ---------------- | -------- |
| 接口风格   | Boost 风格             | STL 风格       | SAX/DOM          | 只读     |
| PMR 支持   | **原生**               | 无             | 自定义 Allocator | 无       |
| 增量解析   | `stream_parser`        | 无             | SAX API          | 无       |
| 编译速度   | 快（header-only 可选） | 慢             | 快               | 快       |
| 可变性     | 读写                   | 读写           | 读写             | 只读     |
| Boost 集成 | 天然                   | 独立           | 独立             | 独立     |

Hical 选择 Boost.JSON 的原因：
1. **PMR 原生支持**——与框架的三层内存池无缝集成
2. **Boost 生态一致性**——与 Asio、Beast 同属一个生态
3. **增量解析**——支持大 JSON 流式处理

### 1.2 值类型体系

`boost::json::value` 是动态类型容器，可以持有以下任意类型：

```
boost::json::value
    ├── null         (nullptr)
    ├── bool         (true / false)
    ├── int64_t      (整数)
    ├── uint64_t     (无符号整数)
    ├── double       (浮点数)
    ├── string       (UTF-8 字符串)
    ├── array        (有序数组)
    └── object       (键值对)
```

**类型判断方法**：

| 方法          | 返回类型  | 行为                                     |
| ------------- | --------- | ---------------------------------------- |
| `is_null()`   | `bool`    | 是否为 null                              |
| `is_bool()`   | `bool`    | 是否为布尔值                             |
| `is_int64()`  | `bool`    | 是否为 int64                             |
| `is_string()` | `bool`    | 是否为字符串                             |
| `is_array()`  | `bool`    | 是否为数组                               |
| `is_object()` | `bool`    | 是否为对象                               |
| `as_string()` | `string&` | 获取字符串引用（类型不匹配则抛异常）     |
| `if_string()` | `string*` | 获取字符串指针（类型不匹配返回 nullptr） |

**`as_xxx()` vs `if_xxx()`**：

```cpp
// as_xxx：确信类型正确时使用，类型错误会抛异常
const auto& name = val.as_string();  // 如果不是 string 就 throws

// if_xxx：不确定类型时使用，安全检查
if (auto* str = val.if_string())
{
    // 是 string，安全使用 *str
}
```

### 1.3 构造与访问

**初始化列表语法**——直观地构建 JSON：

```cpp
// JSON object
boost::json::value user = {
    {"name", "Hical"},
    {"age", 30},
    {"active", true},
    {"scores", {95, 87, 92}},  // 嵌套 array
    {"address", {               // 嵌套 object
        {"city", "Beijing"},
        {"zip", "100000"}
    }}
};
```

等价的 JSON：
```json
{
    "name": "Hical",
    "age": 30,
    "active": true,
    "scores": [95, 87, 92],
    "address": { "city": "Beijing", "zip": "100000" }
}
```

---

## 2. 基础用法

### 2.1 创建 JSON 值

```cpp
// example_json_create.cpp
// 编译：g++ -std=c++20 example_json_create.cpp -lboost_json -o example

#include <boost/json.hpp>
#include <iostream>

namespace json = boost::json;

int main()
{
    // 方式 1：初始化列表
    json::value config = {
        {"host", "0.0.0.0"},
        {"port", 8080},
        {"debug", false},
        {"allowed_origins", {"http://localhost:3000", "https://example.com"}}
    };

    // 方式 2：动态构建
    json::object obj;
    obj["name"] = "Hical";
    obj["version"] = "2.0.0";
    obj["features"] = json::array {"coroutine", "pmr", "reflection"};

    // 方式 3：从标量值构造
    json::value v1 = 42;           // int64_t
    json::value v2 = 3.14;         // double
    json::value v3 = "hello";      // string
    json::value v4 = true;         // bool
    json::value v5 = nullptr;      // null

    // 输出
    std::cout << json::serialize(config) << "\n";
    std::cout << json::serialize(obj) << "\n";

    return 0;
}
```

### 2.2 解析 JSON 字符串

```cpp
#include <boost/json.hpp>
#include <iostream>

namespace json = boost::json;

int main()
{
    // 方式 1：直接解析（失败时抛异常）
    auto val = json::parse(R"({"name": "Hical", "age": 30})");
    std::cout << val.at("name").as_string() << "\n";  // "Hical"

    // 方式 2：带 error_code 的安全解析（推荐）
    boost::system::error_code ec;
    auto result = json::parse(R"({"invalid: json)", ec);
    if (ec)
    {
        std::cerr << "解析失败: " << ec.message() << "\n";
        // 输出：解析失败: syntax error
    }
    else
    {
        std::cout << json::serialize(result) << "\n";
    }

    // 方式 3：解析 JSON 数组
    auto arr = json::parse(R"([1, 2, 3, "hello", true])");
    for (const auto& item : arr.as_array())
    {
        std::cout << json::serialize(item) << " ";
    }
    // 输出：1 2 3 "hello" true

    return 0;
}
```

### 2.3 序列化为字符串

```cpp
json::value data = {
    {"users", json::array {
        {{"id", 1}, {"name", "Hical"}},
        {{"id", 2}, {"name", "Bob"}}
    }}
};

// serialize 输出紧凑 JSON（无空白）
std::string compact = json::serialize(data);
// {"users":[{"id":1,"name":"Hical"},{"id":2,"name":"Bob"}]}

std::cout << compact << "\n";
```

> Boost.JSON 的 `serialize` 默认输出紧凑格式。如果需要美化输出（pretty-print），需要自己实现或使用第三方工具。

### 2.4 访问和修改

```cpp
json::value data = json::parse(R"({
    "users": [
        {"name": "Hical", "age": 30},
        {"name": "Bob", "age": 25}
    ],
    "total": 2
})");

// 对象访问
auto& obj = data.as_object();

// [] 运算符（key 不存在会插入 null）
auto& total = obj["total"];
std::cout << total.as_int64() << "\n";  // 2

// at() 访问（key 不存在抛异常）
auto& users = obj.at("users");

// find() 安全查找
auto it = obj.find("missing_key");
if (it != obj.end())
{
    // 找到了
}

// contains() 判断 key 是否存在
if (obj.contains("users"))
{
    // 存在
}

// 数组遍历
for (const auto& user : users.as_array())
{
    std::cout << user.at("name").as_string() << ": "
              << user.at("age").as_int64() << "\n";
}
// 输出：
// Hical: 30
// Bob: 25

// 修改
obj["total"] = 3;
obj["users"].as_array().push_back({{"name", "Charlie"}, {"age", 28}});
```

### 2.5 类型转换

```cpp
json::value val = 42;

// 整数类型
int64_t i = val.as_int64();     // 42

// 数值类型之间的安全转换
json::value dval = 3.14;
double d = dval.as_double();     // 3.14

// 字符串
json::value sval = "hello";
const auto& s = sval.as_string();  // "hello"

// 注意：as_xxx() 在类型不匹配时会抛异常
// 安全做法：先检查类型
if (val.is_int64())
{
    auto n = val.as_int64();
}

// 或使用 if_xxx() 返回指针
if (auto* p = val.if_int64())
{
    std::cout << *p << "\n";
}
```

---

## 3. 进阶主题

### 3.1 PMR 分配器集成

Boost.JSON 原生支持 `std::pmr` 分配器——所有 JSON 值的内存分配都可以走自定义内存池。

**`storage_ptr`**：JSON 专用的分配器适配器

```cpp
#include <boost/json.hpp>
#include <memory_resource>
#include <iostream>

namespace json = boost::json;

int main()
{
    // 创建一个 1MB 的单调内存池
    char buffer[1024 * 1024];
    std::pmr::monotonic_buffer_resource pool(buffer, sizeof(buffer));

    // 通过 storage_ptr 传给 JSON
    json::storage_ptr sp = json::make_shared_resource<
        json::monotonic_resource>();

    // 使用 PMR 池解析 JSON
    json::parse_options opts;
    auto val = json::parse(R"({"key": "value"})", sp);

    // val 的所有内存分配都来自 sp 指向的池

    return 0;
}
```

**性能意义**：
- 单调池分配是 O(1)——只需移动指针
- 请求结束后整块释放，无需逐个 `delete`
- 消除内存碎片

### 3.2 增量解析

`stream_parser` 支持分块输入——适用于大 JSON 或流式数据：

```cpp
json::stream_parser parser;

// 分块输入
parser.write("[1, 2, ");
parser.write("3, 4]");
parser.finish();

// 获取结果
json::value result = parser.release();
// result = [1, 2, 3, 4]
```

**应用场景**：
- 从网络分块接收的 JSON
- 从文件逐行读取的大 JSON
- JSON Lines 格式（每行一个 JSON 对象）

### 3.3 tag_invoke 自定义序列化

Boost.JSON 使用 `tag_invoke` 模式为自定义类型定义序列化规则：

```cpp
struct Point
{
    double x;
    double y;
};

// 自定义序列化：Point → JSON
void tag_invoke(json::value_from_tag, json::value& jv, const Point& p)
{
    jv = {{"x", p.x}, {"y", p.y}};
}

// 自定义反序列化：JSON → Point
Point tag_invoke(json::value_to_tag<Point>, const json::value& jv)
{
    auto& obj = jv.as_object();
    return Point {
        obj.at("x").as_double(),
        obj.at("y").as_double()
    };
}

// 使用
Point p {1.5, 2.5};
json::value jv = json::value_from(p);           // Point → JSON
Point p2 = json::value_to<Point>(jv);            // JSON → Point
```

> Hical 没有使用 `tag_invoke`，而是实现了更强大的反射驱动方案（MetaJson），见第 4.4 节。

### 3.4 错误处理

```cpp
// 格式错误
boost::system::error_code ec;
auto val = json::parse("{invalid}", ec);
if (ec)
{
    std::cerr << ec.message() << "\n";  // "syntax error"
}

// 常见解析错误
// • 缺少引号的 key
// • 尾随逗号 [1, 2, 3,]
// • 单引号 {'key': 'value'}（JSON 要求双引号）
// • 注释 // 或 /* */（JSON 不支持注释）
```

---

## 4. Hical 实战解读

### 4.1 HttpRequest::jsonBody()

> 源码：`src/core/HttpRequest.cpp:81-90`

```cpp
boost::json::value HttpRequest::jsonBody() const
{
    boost::system::error_code ec;
    auto val = boost::json::parse(req_.body(), ec);
    if (ec)
    {
        return nullptr;  // 解析失败返回 null
    }
    return val;
}
```

> **设计选择**：不抛异常，解析失败返回 `nullptr`。调用者可以通过 `val.is_null()` 判断。这与 [课程 1]({{< relref "posts/Boost.System_错误处理基石.md" >}}) 中的 error_code 模式一致——可恢复错误用错误码/返回值，不用异常。

### 4.2 HttpResponse::setJsonBody()

> 源码：`src/core/HttpResponse.cpp:55-60`

```cpp
void HttpResponse::setJsonBody(const boost::json::value& json)
{
    res_.body() = boost::json::serialize(json);
    res_.set(http::field::content_type, "application/json");
    res_.prepare_payload();
}
```

> 三步走：`serialize` 转字符串 → 设置 Content-Type → `prepare_payload` 计算 Content-Length。

### 4.3 HttpResponse::json() 工厂

> 源码：`src/core/HttpResponse.cpp:152-158`

```cpp
HttpResponse HttpResponse::json(const boost::json::value& json)
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hOk);
    res.setJsonBody(json);
    return res;
}
```

**用法**：

```cpp
// 利用初始化列表语法，一行返回 JSON 响应
return HttpResponse::json({
    {"id", 1},
    {"name", "Hical"},
    {"scores", {95, 87, 92}}
});
```

> 初始化列表 `{{"key", "value"}}` 被隐式转换为 `boost::json::value`，然后传给 `json()` 方法。

### 4.4 MetaJson.h：反射驱动的自动序列化

> 源码：`src/core/MetaJson.h`（全文）

这是本课程的**重头戏**——Hical 如何实现 "定义一个结构体，自动获得 JSON 序列化能力"。

**用户侧用法**：

```cpp
struct UserDTO
{
    std::string name;
    int age;
    double score;

    HICAL_JSON(UserDTO, name, age, score)  // 一行标注
};

// 自动序列化
UserDTO user {"Hical", 30, 95.5};
boost::json::value json = hical::meta::toJson(user);
// → {"name":"Hical","age":30,"score":95.5}

// 自动反序列化
auto user2 = hical::meta::fromJson<UserDTO>(json);
```

**实现原理分层**：

```
用户调用 toJson(obj)
    │
    ▼
遍历所有字段 ← HICAL_JSON 宏生成的 hicalJsonFields()
    │              返回 tuple<FieldDescriptor...>
    ▼
对每个字段调用 valueToJson(field_value)
    │
    ├─ string?  → json::value(val)
    ├─ bool?    → val
    ├─ 整数?    → static_cast<int64_t>(val)
    ├─ 浮点?    → static_cast<double>(val)
    ├─ vector?  → 递归转换为 json::array
    └─ 嵌套结构体? → 递归调用 toJson()
```

**核心组件 1：valueToJson 类型分发**（MetaJson.h:54-90）

```cpp
template <typename T>
boost::json::value valueToJson(const T& val)
{
    if constexpr (std::is_same_v<T, std::string>)
        return boost::json::value(val);
    else if constexpr (std::is_same_v<T, bool>)
        return val;
    else if constexpr (std::is_integral_v<T>)
        return static_cast<int64_t>(val);
    else if constexpr (std::is_floating_point_v<T>)
        return static_cast<double>(val);
    else if constexpr (IsVector<T>::value)
    {
        boost::json::array arr;
        for (const auto& item : val)
            arr.push_back(valueToJson(item));
        return arr;
    }
    else if constexpr (HasJsonFields<T>::value)
        return toJson(val);  // 嵌套结构体递归
    else
        static_assert(sizeof(T) == 0, "Unsupported type");
}
```

> **`if constexpr` 链**：编译期类型分发，零运行时开销。每种 C++ 类型映射到对应的 JSON 类型。

**核心组件 2：HICAL_JSON 宏**（MetaJson.h:370-421）

```cpp
// 宏展开示例：
// HICAL_JSON(UserDTO, name, age, score)
// 展开为：
static auto hicalJsonFields()
{
    return std::make_tuple(
        hical::meta::detail::makeField<UserDTO>("name", &UserDTO::name),
        hical::meta::detail::makeField<UserDTO>("age", &UserDTO::age),
        hical::meta::detail::makeField<UserDTO>("score", &UserDTO::score)
    );
}
```

> 宏生成了一个 `hicalJsonFields()` 静态方法，返回字段名和成员指针的 tuple。序列化/反序列化函数通过 fold expression 遍历这个 tuple。

**核心组件 3：C++26 反射实现**（MetaJson.h:259-302）

```cpp
// C++26 反射版本——不需要 HICAL_JSON 宏
template <typename T>
boost::json::object toJson(const T& obj)
{
    boost::json::object jsonObj;

    // ^^T 获取类型的反射信息
    // nonstatic_data_members_of 枚举所有数据成员
    template for (constexpr auto member : std::meta::nonstatic_data_members_of(^^T))
    {
        constexpr auto name = std::meta::identifier_of(member);
        jsonObj[name] = valueToJson(obj.[:member:]);
    }

    return jsonObj;
}
```

> 当编译器支持 C++26 反射时（`HICAL_HAS_REFLECTION == 1`），完全不需要宏标注——编译器自动枚举结构体的所有数据成员。

**readJson 便捷接口**（MetaJson.h:315-348）：

```cpp
// 从 HTTP 请求直接反序列化
auto user = req.readJson<UserDTO>();

// 等价于：
auto json = req.jsonBody();
auto user = hical::meta::fromJson<UserDTO>(json);
```

### 4.5 PMR 与 JSON 的协同

> 源码：`src/core/HttpServer.cpp:239-242`

```cpp
// 创建请求级内存池
auto requestPool = MemoryPool::instance().createRequestPool();
std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());

// flat_buffer 使用 PMR 分配器
beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);
```

**内存流转图**：

```
请求级单调内存池 (monotonic_buffer_resource)
    │
    ├── flat_buffer 分配 ← async_read 将 HTTP 数据读入
    │
    ├── parser 解析 ← 解析 HTTP 头和 body
    │
    ├── json::parse ← 解析 JSON body（分配 value/object/array）
    │
    └── 请求处理完成 → 整块释放（一次 free，零碎片）
```

> **性能意义**：整个请求的所有内存分配（buffer、HTTP 解析、JSON 解析）都来自同一个单调池。请求结束后一次性释放，比逐个 `delete` 快得多。

---

## 5. 练习题

### 练习 1：JSON 解析与提取

编写程序，解析以下 JSON 并提取所有用户的 name 和 age：

```json
{
    "users": [
        {"name": "Hical", "age": 30, "active": true},
        {"name": "Bob", "age": 25, "active": false},
        {"name": "Charlie", "age": 28, "active": true}
    ],
    "total": 3
}
```

输出格式：`Hical (30), Bob (25), Charlie (28)`

### 练习 2：HICAL_JSON 宏实战

定义一个 `Product` 结构体：

```cpp
struct Product
{
    std::string name;
    double price;
    int stock;
    std::vector<std::string> tags;

    HICAL_JSON(Product, name, price, stock, tags)
};
```

编写测试：创建 Product 对象 → `toJson` 序列化 → `fromJson` 反序列化 → 验证所有字段相等。

### 练习 3：安全 JSON 验证器

使用 `boost::json::parse(str, ec)` 编写一个 JSON 验证器：
- 输入：字符串
- 输出：`"Valid JSON"` 或 `"Invalid: <error message>"`
- 测试用例：合法 JSON、缺少引号、尾随逗号、空字符串

### 练习 4：PMR 性能对比

使用 `std::pmr::monotonic_buffer_resource` 和默认分配器分别解析 10000 个 JSON 对象（如 `{"id": N, "name": "user_N"}`），对比耗时。

提示：使用 `<chrono>` 计时，`monotonic_buffer_resource` 需要预分配足够的 buffer。

### 练习 5（挑战）：扩展 valueToJson

仿照 MetaJson.h 的 `valueToJson`，添加对 `std::optional<T>` 的支持：
- `std::nullopt` → JSON `null`
- 有值 → 递归调用 `valueToJson`

```cpp
// 期望行为
struct Config
{
    std::string name;
    std::optional<int> maxRetry;  // 可选字段

    HICAL_JSON(Config, name, maxRetry)
};
```

---

## 参考答案

### 练习 1 参考答案：JSON 解析与提取

```cpp
#include <boost/json.hpp>
#include <iostream>
#include <string>

namespace json = boost::json;

int main()
{
    const char* input = R"({
        "users": [
            {"name": "Hical", "age": 30, "active": true},
            {"name": "Bob", "age": 25, "active": false},
            {"name": "Charlie", "age": 28, "active": true}
        ],
        "total": 3
    })";

    // 安全解析
    boost::system::error_code ec;
    auto val = json::parse(input, ec);
    if (ec)
    {
        std::cerr << "解析失败: " << ec.message() << std::endl;
        return 1;
    }

    // 类型检查 + 提取
    auto& root = val.as_object();
    auto& users = root.at("users").as_array();

    bool first = true;
    for (const auto& user : users)
    {
        auto& obj = user.as_object();
        auto name = obj.at("name").as_string();
        auto age = obj.at("age").as_int64();

        if (!first) std::cout << ", ";
        std::cout << name << " (" << age << ")";
        first = false;
    }
    std::cout << std::endl;
    // 输出: Hical (30), Bob (25), Charlie (28)

    // 额外：提取 total 并验证
    auto total = root.at("total").as_int64();
    std::cout << "Total: " << total << " (array size: " << users.size() << ")" << std::endl;

    return 0;
}
```

**要点**：`as_object()` / `as_array()` / `as_string()` / `as_int64()` 在类型不匹配时抛异常。生产代码应先用 `is_object()` / `if_contains()` 检查。`at()` 在 key 不存在时也会抛异常，比 `operator[]` 更安全（后者返回 null）。

### 练习 2 参考答案：HICAL_JSON 宏实战

```cpp
#include "core/MetaJson.h"
#include <cassert>
#include <iostream>

struct Product
{
    std::string name;
    double price {};
    int stock {};
    std::vector<std::string> tags;

    HICAL_JSON(Product, name, price, stock, tags)
};

int main()
{
    // 创建原始对象
    Product original;
    original.name = "Widget Pro";
    original.price = 29.99;
    original.stock = 150;
    original.tags = {"electronics", "sale", "new"};

    // 序列化
    auto json = hical::meta::toJson(original);
    std::cout << "序列化结果: " << boost::json::serialize(json) << std::endl;
    // → {"name":"Widget Pro","price":29.99,"stock":150,"tags":["electronics","sale","new"]}

    // 反序列化
    auto restored = hical::meta::fromJson<Product>(boost::json::value(json));

    // 验证所有字段
    assert(restored.name == original.name);
    assert(restored.price == original.price);
    assert(restored.stock == original.stock);
    assert(restored.tags.size() == original.tags.size());
    for (size_t i = 0; i < restored.tags.size(); ++i)
    {
        assert(restored.tags[i] == original.tags[i]);
    }

    std::cout << "所有字段验证通过!" << std::endl;

    // 边界测试：缺少字段
    auto partial = hical::meta::fromJson<Product>(
        boost::json::value({{"name", "Minimal"}}));
    assert(partial.name == "Minimal");
    assert(partial.price == 0.0);  // 默认值
    assert(partial.stock == 0);    // 默认值
    assert(partial.tags.empty());  // 默认值
    std::cout << "缺少字段测试通过（保持默认值）" << std::endl;

    // 边界测试：多余字段
    auto extra = hical::meta::fromJson<Product>(
        boost::json::value({{"name", "Extra"}, {"price", 1.0}, {"stock", 1},
                            {"tags", boost::json::array{}},
                            {"unknown_field", "ignored"}}));
    assert(extra.name == "Extra");
    std::cout << "多余字段测试通过（静默忽略）" << std::endl;

    return 0;
}
```

**要点**：`HICAL_JSON` 宏支持 `std::vector<std::string>` 等容器类型——`valueToJson` 中的 `IsVector<T>` 分支处理数组序列化，`valueFromJson` 对应处理反序列化。`fromJson` 对缺少字段保持默认值、多余字段静默忽略，这是框架前向/后向兼容的关键设计。

### 练习 3 参考答案：安全 JSON 验证器

```cpp
#include <boost/json.hpp>
#include <iostream>
#include <string>
#include <vector>

std::string validateJson(const std::string& input)
{
    boost::system::error_code ec;
    boost::json::parse(input, ec);

    if (ec)
    {
        return "Invalid: " + ec.message();
    }
    return "Valid JSON";
}

int main()
{
    struct TestCase
    {
        std::string input;
        std::string description;
    };

    std::vector<TestCase> tests = {
        {R"({"name": "Hical", "age": 30})", "合法 JSON 对象"},
        {R"([1, 2, 3])", "合法 JSON 数组"},
        {R"("hello")", "合法 JSON 字符串"},
        {R"(42)", "合法 JSON 数字"},
        {R"(true)", "合法 JSON 布尔"},
        {R"(null)", "合法 JSON null"},
        {R"({name: "Hical"})", "缺少引号的 key"},
        {R"({"name": "Hical",})", "尾随逗号"},
        {R"()", "空字符串"},
        {R"({)", "不完整的对象"},
        {R"([1, 2,, 3])", "多余逗号"},
        {R"({"a": undefined})", "非法值 undefined"},
    };

    for (const auto& test : tests)
    {
        auto result = validateJson(test.input);
        std::cout << "[" << (result == "Valid JSON" ? "PASS" : "FAIL")
                  << "] " << test.description << std::endl;
        std::cout << "  输入: " << (test.input.empty() ? "(空)" : test.input) << std::endl;
        std::cout << "  结果: " << result << std::endl;
        std::cout << std::endl;
    }

    return 0;
}
```

**要点**：`json::parse(str, ec)` 是 error_code 重载——解析失败不抛异常，而是设置 `ec`。这与 Hical `HttpRequest::jsonBody()` 的设计一致（解析失败返回 null 而非抛异常），让上层可以优雅地返回 400 Bad Request。Boost.JSON 的 parser 是严格模式（不允许尾随逗号、注释等非标准扩展），这对安全验证是正确的选择。

### 练习 4 参考答案：PMR 性能对比

```cpp
#include <boost/json.hpp>
#include <chrono>
#include <iostream>
#include <memory_resource>

namespace json = boost::json;

void benchDefault(int iterations)
{
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        std::string str = R"({"id": )" + std::to_string(i) + R"(, "name": "user_)"
                          + std::to_string(i) + R"("})";
        auto val = json::parse(str);
        // val 析构时用默认 new/delete 释放
    }

    auto elapsed = std::chrono::high_resolution_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cout << "默认分配器:    " << ms << " ms" << std::endl;
}

void benchMonotonic(int iterations)
{
    auto start = std::chrono::high_resolution_clock::now();

    // 预分配 1MB 缓冲区
    char buffer[1024 * 1024];
    std::pmr::monotonic_buffer_resource pool(buffer, sizeof(buffer));

    for (int i = 0; i < iterations; ++i)
    {
        std::string str = R"({"id": )" + std::to_string(i) + R"(, "name": "user_)"
                          + std::to_string(i) + R"("})";

        // 使用 PMR 分配器解析
        json::storage_ptr sp = json::make_shared_resource<json::monotonic_resource>();
        auto val = json::parse(str, sp);
        // monotonic_resource 析构时整体释放
    }

    auto elapsed = std::chrono::high_resolution_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cout << "monotonic 池:  " << ms << " ms" << std::endl;
}

void benchPoolResource(int iterations)
{
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        std::string str = R"({"id": )" + std::to_string(i) + R"(, "name": "user_)"
                          + std::to_string(i) + R"("})";

        // Boost.JSON 自带的 monotonic_resource（优化版）
        json::monotonic_resource mr;
        auto val = json::parse(str, &mr);
    }

    auto elapsed = std::chrono::high_resolution_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cout << "json::monotonic: " << ms << " ms" << std::endl;
}

int main()
{
    constexpr int kIterations = 100000;
    std::cout << "解析 " << kIterations << " 个 JSON 对象的性能对比" << std::endl;
    std::cout << "===========================================" << std::endl;

    benchDefault(kIterations);
    benchMonotonic(kIterations);
    benchPoolResource(kIterations);

    // 预期结果（Release 模式）:
    // 默认分配器:     ~150 ms
    // monotonic 池:   ~80 ms
    // json::monotonic: ~60 ms
    //
    // json::monotonic_resource 是 Boost.JSON 为 JSON 解析场景
    // 专门优化的分配器，比通用 std::pmr::monotonic_buffer_resource 更快

    return 0;
}
// 编译（必须 Release 模式）: g++ -std=c++20 -O2 bench.cpp -lboost_json -o bench
```

**要点**：`json::monotonic_resource` 是 Boost.JSON 自带的优化版单调池，比 `std::pmr::monotonic_buffer_resource` 更快（内部针对 JSON 节点大小做了对齐优化）。这与 Hical 的三级 PMR 策略互补——Hical 在 `handleSession` 中用 `std::pmr::monotonic_buffer_resource` 给 Beast 的 `flat_buffer` 使用，JSON 解析可以进一步用 `json::monotonic_resource` 加速。**必须用 Release 模式编译**，Debug 模式的性能数据没有参考价值。

### 练习 5 参考答案：扩展 valueToJson 支持 optional

```cpp
#include "core/MetaJson.h"
#include <cassert>
#include <iostream>
#include <optional>

namespace hical::meta::detail
{

// 检测 std::optional 的类型萃取
template <typename T>
struct IsOptional : std::false_type {};

template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

// valueToJson 的 optional 特化
// 需要在 MetaJson.h 的 valueToJson 中添加这个分支：
//
// else if constexpr (IsOptional<T>::value)
// {
//     if (val.has_value())
//         return valueToJson(*val);    // 递归序列化内部值
//     else
//         return boost::json::value(nullptr);  // nullopt → JSON null
// }

// valueFromJson 的 optional 特化
// 需要在 MetaJson.h 的 valueFromJson 中添加这个分支：
//
// else if constexpr (IsOptional<T>::value)
// {
//     if (val.is_null())
//         return std::nullopt;
//     return valueFromJson<typename T::value_type>(val);
// }

} // namespace hical::meta::detail

// 示例 DTO
struct Config
{
    std::string name;
    std::optional<int> maxRetry;
    std::optional<std::string> description;

    HICAL_JSON(Config, name, maxRetry, description)
};

int main()
{
    // 测试 1：所有字段都有值
    Config full {"server", 3, "主配置"};
    auto json1 = hical::meta::toJson(full);
    std::cout << "完整: " << boost::json::serialize(json1) << std::endl;
    // → {"name":"server","maxRetry":3,"description":"主配置"}

    // 测试 2：optional 字段为空
    Config partial {"client", std::nullopt, std::nullopt};
    auto json2 = hical::meta::toJson(partial);
    std::cout << "部分: " << boost::json::serialize(json2) << std::endl;
    // → {"name":"client","maxRetry":null,"description":null}

    // 测试 3：反序列化（JSON null → nullopt）
    auto restored = hical::meta::fromJson<Config>(boost::json::value(json2));
    assert(restored.name == "client");
    assert(!restored.maxRetry.has_value());
    assert(!restored.description.has_value());

    // 测试 4：反序列化（JSON 字段缺失 → nullopt）
    auto minimal = hical::meta::fromJson<Config>(
        boost::json::value({{"name", "minimal"}}));
    assert(!minimal.maxRetry.has_value());
    std::cout << "所有测试通过!" << std::endl;

    return 0;
}
```

**完整的 MetaJson.h 修改**（需要在 `valueToJson` 和 `valueFromJson` 的 `if constexpr` 链中各插入一个分支）：

```cpp
// 在 valueToJson 中，IsVector 分支之后添加：
else if constexpr (detail::IsOptional<T>::value)
{
    if (val.has_value())
        return valueToJson(*val);
    return boost::json::value(nullptr);
}

// 在 valueFromJson 中，IsVector 分支之后添加：
else if constexpr (detail::IsOptional<T>::value)
{
    if (val.is_null())
        return T {std::nullopt};
    return T {valueFromJson<typename T::value_type>(val)};
}
```

**要点**：`std::optional<T>` 的序列化语义是：`has_value()` → 递归序列化内部值；`nullopt` → JSON `null`。反序列化反过来：JSON `null` → `nullopt`；其他值 → 递归反序列化后包装为 `optional<T>`。这个扩展遵循了 MetaJson 的设计范式——用 `if constexpr` 类型萃取做编译期分发，用 `IsOptional` 模板特化检测类型。`static_assert(sizeof(T) == 0)` 兜底确保不支持的类型在编译期报错而非运行时崩溃。

---

## 6. 总结与拓展阅读

### C++ 类型 ↔ JSON 类型映射表

| C++ 类型                  | JSON 类型        | 示例                |
| ------------------------- | ---------------- | ------------------- |
| `std::string`             | string           | `"hello"`           |
| `bool`                    | boolean          | `true`              |
| `int`, `int64_t`          | number (integer) | `42`                |
| `double`                  | number (float)   | `3.14`              |
| `std::vector<T>`          | array            | `[1, 2, 3]`         |
| 结构体（HICAL_JSON 标注） | object           | `{"name": "Hical"}` |
| `nullptr`                 | null             | `null`              |

### API 速查表

| API                      | 用途                               |
| ------------------------ | ---------------------------------- |
| `json::parse(str)`       | 解析 JSON 字符串（失败抛异常）     |
| `json::parse(str, ec)`   | 安全解析（失败设 error_code）      |
| `json::serialize(val)`   | 序列化为紧凑 JSON 字符串           |
| `val.is_xxx()`           | 类型判断                           |
| `val.as_xxx()`           | 获取值引用（类型错误抛异常）       |
| `val.if_xxx()`           | 获取值指针（类型错误返回 nullptr） |
| `obj["key"]`             | 对象字段访问（不存在则插入 null）  |
| `obj.at("key")`          | 对象字段访问（不存在抛异常）       |
| `obj.find("key")`        | 安全查找（返回迭代器）             |
| `obj.contains("key")`    | 判断 key 是否存在                  |
| `arr.push_back(val)`     | 数组追加元素                       |
| `json::value_from(obj)`  | tag_invoke 序列化                  |
| `json::value_to<T>(val)` | tag_invoke 反序列化                |
| `stream_parser`          | 增量解析器                         |

### 拓展阅读

- [Boost.JSON 官方文档](https://www.boost.org/doc/libs/release/libs/json/doc/html/index.html)
- [Boost.JSON 快速入门](https://www.boost.org/doc/libs/release/libs/json/doc/html/json/quick_look.html)
- [RFC 8259 - JSON 标准](https://www.rfc-editor.org/rfc/rfc8259)
- [C++26 反射提案 P2996](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2996r0.html)

### 课程回顾

至此，4 个核心 Boost 库的学习课程全部完成：

1. **Boost.System** — 错误码基础设施
2. **Boost.Asio** — 异步 I/O 和协程引擎
3. **Boost.Beast** — HTTP/WebSocket 协议层
4. **Boost.JSON** — JSON 数据处理

这 4 层从底到顶构成了 Hical 框架的核心技术栈。理解它们，你就理解了现代 C++ 网络编程的完整链路。

> 框架设计角度的深入讲解，请参见 [Hical 框架系列]({{< relref "posts/01-pmr-memory-pool-design.md" >}})。
