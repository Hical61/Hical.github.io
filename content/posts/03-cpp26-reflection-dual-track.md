+++
title = 'C++26 反射落地实战：双路线条件编译实现自动路由注册与 JSON 序列化'
date = '2026-04-12'
draft = false
tags = ["C++26", "反射", "元编程", "路由注册", "Hical"]
categories = ["Hical框架"]
description = "展示如何在 C++26 反射尚未被主流编译器完全支持的现阶段，用双路线策略实现自动路由注册与 JSON 序列化。"
+++

# C++26 反射落地实战：双路线条件编译实现自动路由注册与 JSON 序列化

> 本文以 Hical 框架为例，展示如何在 C++26 反射尚未被主流编译器完全支持的现阶段，用"C++26 反射 + C++20 宏回退"的双路线策略，让用户享受相同的 API。

---

## 问题：Web 框架中的重复样板代码

每个 Web 框架都有两大类重复劳动：

**1. 路由注册**——每个处理函数都要手写一行注册：

```cpp
router.get("/api/users", listUsers);
router.get("/api/users/{id}", getUser);
router.post("/api/users", createUser);
router.put("/api/users/{id}", updateUser);
router.del("/api/users/{id}", deleteUser);
// ... 50 个路由 = 50 行手写注册
```

**2. JSON 序列化**——每个 DTO 都要手写字段映射：

```cpp
json["name"] = user.name;
json["age"] = user.age;
json["email"] = user.email;
// ... 10 个字段 = 10 行手写映射
```

C++26 反射（P2996）可以在编译期枚举结构体的所有成员，彻底消除这两类样板代码。但主流编译器尚未正式支持。怎么办？

## 双路线策略

Hical 的解决方案：**用条件编译提供两条路线，对外 API 完全相同**。

```cpp
// Reflection.h — 反射检测
#if defined(__cpp_reflection) && __cpp_reflection >= 202306L
    #define HICAL_HAS_REFLECTION 1  // C++26 反射可用
#elif defined(HICAL_FORCE_REFLECTION)
    #define HICAL_HAS_REFLECTION 1  // 手动强制启用
#else
    #define HICAL_HAS_REFLECTION 0  // 回退到 C++20 宏方案
#endif
```

用户写的代码**长期不变**，编译器升级后自动切换到反射路线：

```cpp
// 用户代码 — 无论哪条路线都是这样写
struct UserDTO {
    std::string name;
    int age;
    HICAL_JSON(UserDTO, name, age)  // C++26 下此宏变为空操作
};

auto json = meta::toJson(user);           // 两条路线 API 完全相同
auto user = meta::fromJson<UserDTO>(json);
```

## 路线一：C++26 反射（未来）

当编译器支持 P2996 时，`toJson` 的实现极其优雅：

```cpp
template <typename T>
boost::json::object toJson(const T& obj)
{
    boost::json::object jsonObj;

    template for (constexpr auto member : std::meta::nonstatic_data_members_of(^^T))
    {
        constexpr auto name = std::meta::identifier_of(member);
        jsonObj[name] = valueToJson(obj.[:member:]);
    }

    return jsonObj;
}
```

- `^^T` — 获取类型 T 的反射元数据
- `std::meta::nonstatic_data_members_of` — 枚举所有非静态数据成员
- `[:member:]` — 从反射信息恢复为代码实体（访问成员）
- `template for` — 编译期遍历

**零宏、零标注、零运行时开销。** 编译器在编译期展开循环，生成等价于手写的代码。

路由注册同样：

```cpp
template <typename Handler>
void registerRoutes(Router& router, Handler& handler)
{
    template for (constexpr auto fn : std::meta::nonstatic_member_functions_of(^^Handler))
    {
        // 读取 [[hical::route("/path", "GET")]] 属性
        // 自动注册到 router
    }
}
```

## 路线二：C++20 宏回退（当前）

当前编译器不支持反射，Hical 用宏 + 模板元编程模拟相同的能力。

### JSON 序列化的宏方案

```cpp
struct UserDTO {
    std::string name;
    int age;
    HICAL_JSON(UserDTO, name, age)
    // 展开为：
    // static auto hicalJsonFields() {
    //     return std::make_tuple(
    //         FieldDescriptor<UserDTO, std::string>{"name", &UserDTO::name},
    //         FieldDescriptor<UserDTO, int>{"age", &UserDTO::age}
    //     );
    // }
};
```

`HICAL_JSON` 宏的核心是生成一个 `hicalJsonFields()` 静态方法，返回字段名和成员指针的 tuple。`toJson` 遍历这个 tuple：

```cpp
template <typename T>
boost::json::object toJson(const T& obj)
{
    auto fields = T::hicalJsonFields();
    boost::json::object jsonObj;
    constexpr auto count = std::tuple_size_v<decltype(fields)>;

    // 折叠表达式遍历 tuple
    detail::serializeFields(obj, jsonObj, fields, std::make_index_sequence<count>{});
    return jsonObj;
}
```

折叠表达式展开后等价于：

```cpp
jsonObj["name"] = valueToJson(obj.name);
jsonObj["age"] = valueToJson(obj.age);
```

### 路由注册的宏方案

```cpp
struct UserHandler {
    HttpResponse listUsers(const HttpRequest&) { ... }
    HICAL_HANDLER(Get, "/api/users", listUsers)
    // 展开为：
    // static constexpr RouteInfo hicalRouteInfo_listUsers{HttpMethod::hGet, "/api/users", "listUsers"};

    HICAL_ROUTES(UserHandler, listUsers, getUser)
    // 展开为：
    // static auto hicalRouteTable() {
    //     return std::make_tuple(
    //         RouteRegistrar{hicalRouteInfo_listUsers, &UserHandler::listUsers},
    //         RouteRegistrar{hicalRouteInfo_getUser, &UserHandler::getUser}
    //     );
    // }
};

// 注册
meta::registerRoutes(router, handler);
```

`registerRoutes` 遍历 `hicalRouteTable()` 返回的 tuple，逐个调用 `router.route()`。

### 可变参数宏的实现

支持 1-16 个字段的 `HICAL_JSON(Type, f1, f2, ..., f16)` 需要一个 FOR_EACH 展开器：

```cpp
// 参数计数
#define HICAL_JSON_NARGS_(...) \
    HICAL_JSON_ARG_N_(__VA_ARGS__, 16, 15, 14, ..., 2, 1)

// 分发到对应数量的展开宏
#define HICAL_JSON_FOR_EACH_(T, ...) \
    HICAL_JSON_PASTE_(HICAL_JSON_FE_, HICAL_JSON_NARGS_(__VA_ARGS__))(T, __VA_ARGS__)

// 递归展开
#define HICAL_JSON_FE_1(T, a)       HICAL_JSON_FIELD_(T, a)
#define HICAL_JSON_FE_2(T, a, b)    HICAL_JSON_FIELD_(T, a), HICAL_JSON_FIELD_(T, b)
#define HICAL_JSON_FE_3(T, a, b, c) HICAL_JSON_FE_2(T, a, b), HICAL_JSON_FIELD_(T, c)
// ... 直到 16
```

注意：MSVC 的传统预处理器对 `__VA_ARGS__` 的展开有缺陷，需要添加 `/Zc:preprocessor` 编译选项。

## 类型安全：不只是序列化

`fromJson` 在反序列化时做了完整的类型检查：

```cpp
template <typename T>
T valueFromJson(const boost::json::value& val)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        if (!val.is_string())
            throw std::runtime_error("JSON type mismatch: expected string");
        return std::string(val.as_string());
    }
    else if constexpr (std::is_integral_v<T>)
    {
        if (!val.is_int64() && !val.is_uint64())
            throw std::runtime_error("JSON type mismatch: expected integer");
        return static_cast<T>(val.as_int64());
    }
    // ...
}
```

恶意输入 `{"age": "not_a_number"}` 会得到清晰的错误信息，而不是 Boost.JSON 的底层异常。

## 生命周期安全：shared_ptr 管理 Handler

`registerRoutes` 的路由回调捕获了 handler 的成员函数指针。如果 handler 是栈上的局部变量，server 切到异步模式后 handler 被销毁，回调就会持有悬挂引用。

Hical 提供两个重载：

```cpp
// 推荐：shared_ptr 管理，生命周期安全
auto handler = std::make_shared<UserHandler>();
meta::registerRoutes(router, handler);

// 便捷：引用版本（调用者保证生命周期）
UserHandler handler;
meta::registerRoutes(router, handler);  // 内部用空删除器包装为 shared_ptr
```

## 迁移路径

```
当前 (C++20)                        →  未来 (C++26)
────────────────────────────────────────────────
HICAL_JSON(Type, f1, f2) 宏标注      →  自动枚举，宏变空操作
HICAL_HANDLER/ROUTES 宏标注          →  [[hical::route(...)]] 属性
toJson/fromJson 遍历 tuple           →  template for 遍历反射信息
用户代码无需修改                       →  删除宏标注即可（可选）
```

这就是双路线的价值：**用户代码面向未来，编译器追上来时零改动。**

## 总结

| 维度       | C++26 反射 | C++20 宏回退       |
| ---------- | ---------- | ------------------ |
| 用户体验   | 零标注     | 每个 struct 一行宏 |
| 运行时开销 | 零         | 零（编译期展开）   |
| 实现复杂度 | 简洁       | 可变参数宏较复杂   |
| 编译器要求 | P2996 支持 | C++20              |
| API        | 完全相同   | 完全相同           |

反射不只是语法糖——它是**编译期元编程的终极形态**。双路线策略让你今天就能享受它带来的 API 简洁性，同时为编译器支持到位的那天做好准备。

---

> 源码参考：[MetaJson.h](https://github.com/Hical61/Hical/blob/main/src/core/MetaJson.h) / [MetaRoutes.h](https://github.com/Hical61/Hical/blob/main/src/core/MetaRoutes.h)
> 项目地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
