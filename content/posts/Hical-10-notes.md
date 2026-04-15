+++
title = '第10课：C++26 反射与自动化'
date = '2026-04-15'
draft = false
tags = ["C++26", "反射", "元编程", "JSON序列化", "路由注册", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "理解 C++26 反射的双路线策略，掌握反射驱动的自动 JSON 序列化和自动路由注册。"
+++

# 第10课：C++26 反射与自动化 - 学习笔记

> 理解 C++26 反射的双路线策略，掌握反射驱动的自动 JSON 序列化和自动路由注册。

---

## 一、为什么需要反射层？

在第6-8课中，路由注册和 JSON 处理都是手动的：

```cpp
// 手动路由注册（第6-8课方式）
server.router().get("/api/users", listUsersHandler);
server.router().get("/api/users/{id}", getUserHandler);
server.router().post("/api/users", createUserHandler);

// 手动 JSON 处理
auto json = boost::json::parse(req.body());
auto name = json.at("name").as_string();
```

当路由增多到 50+、DTO 结构体有 10+ 字段时，手动维护成为负担且容易出错（路径拼错、字段遗漏）。

反射层的目标：**一行代码注册所有路由，一行代码完成 JSON 序列化/反序列化**。

```cpp
// 反射方式
meta::registerRoutes(server.router(), handler);  // 一行注册所有路由
auto user = req.readJson<UserDTO>();              // 一行反序列化
return HttpResponse::json(meta::toJson(user));    // 一行序列化
```

---

## 二、双路线策略 — 核心设计理念

### 2.1 为什么是"双路线"？

C++26 反射（P2996）是实验性特性，2026 年还没有任何主流编译器完整支持。如果只支持 C++26，框架当前无法使用。如果只用宏，未来编译器支持反射后用户还得手动标注。

hical 的解决方案：

```
                    ┌─ 编译器支持 P2996 ──→ C++26 反射路线（零宏，全自动）
HICAL_HAS_REFLECTION ─┤
                    └─ 不支持          ──→ C++20 宏回退路线（用户标注宏）

                    两条路线提供 **完全相同的用户 API**
```

### 2.2 特性检测机制

**源码位置**：`src/core/Reflection.h`

```cpp
// C++26 反射特性检测
#if defined(__cpp_reflection) && __cpp_reflection >= 202306L
    #define HICAL_HAS_REFLECTION 1      // 编译器原生支持
#elif defined(HICAL_FORCE_REFLECTION)
    #define HICAL_HAS_REFLECTION 1      // CMake 手动强制开启
#else
    #define HICAL_HAS_REFLECTION 0      // 回退到宏方案
#endif
```

三种启用方式：
1. **自动检测**：编译器定义 `__cpp_reflection >= 202306L`（P2996 标准特性测试宏）
2. **手动强制**：CMake `-DHICAL_FORCE_REFLECTION=ON`（用于实验性编译器）
3. **默认关闭**：使用 C++20 宏回退方案

### 2.3 编译期类型检测

```cpp
// 检测类型是否注册了 HICAL_JSON
template <typename T, typename = void>
struct HasJsonFields : std::false_type {};

template <typename T>
struct HasJsonFields<T, std::void_t<decltype(T::hicalJsonFields())>> : std::true_type {};

// 检测类型是否注册了 HICAL_ROUTES
template <typename T, typename = void>
struct HasRouteTable : std::false_type {};

template <typename T>
struct HasRouteTable<T, std::void_t<decltype(T::hicalRouteTable())>> : std::true_type {};
```

SFINAE 检测：如果类型 T 有 `hicalJsonFields()` 静态方法（由 `HICAL_JSON` 宏生成），`HasJsonFields<T>::value` 为 true。同理 `HasRouteTable`。

这使得 `toJson` 和 `registerRoutes` 可以在编译期静态断言，而不是运行时报错：

```cpp
static_assert(HasJsonFields<T>::value,
    "Type must use HICAL_JSON() macro or have C++26 reflection support");
```

---

## 三、MetaJson — 自动 JSON 序列化

### 3.1 用户 API

**源码位置**：`src/core/MetaJson.h`

```cpp
// DTO 定义
struct UserDTO {
    std::string name;
    int age;
    std::string email;

    HICAL_JSON(UserDTO, name, age, email)  // C++26 下此行为空操作
};

// 序列化
UserDTO user {"Alice", 30, "alice@example.com"};
boost::json::object json = meta::toJson(user);    // → {"name":"Alice","age":30,"email":"..."}

// 反序列化
auto restored = meta::fromJson<UserDTO>(jsonValue);

// 从 HTTP 请求反序列化
auto user = req.readJson<UserDTO>();
```

### 3.2 值转换层：valueToJson / valueFromJson

**类型分发策略**：`if constexpr` 编译期分支

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
        return toJson(val);              // 递归：嵌套结构体
    else
        static_assert(sizeof(T) == 0, "Unsupported type");
}
```

支持的类型体系：

| C++ 类型                 | JSON 类型       | 判断条件                                                          |
| ------------------------ | --------------- | ----------------------------------------------------------------- |
| `std::string`            | string          | `is_same_v`                                                       |
| `bool`                   | bool            | `is_same_v`（必须在 integral 之前，因为 bool 也满足 is_integral） |
| `int`, `int64_t` 等      | number (int64)  | `is_integral_v`                                                   |
| `double`, `float`        | number (double) | `is_floating_point_v`                                             |
| `std::vector<T>`         | array           | `IsVector<T>::value`                                              |
| 带 `HICAL_JSON` 的结构体 | object          | `HasJsonFields<T>::value`（递归）                                 |

**注意 bool 判断顺序**：`bool` 满足 `is_integral_v`，所以 `is_same_v<T, bool>` 必须在 `is_integral_v<T>` 之前判断。

**反序列化的类型检查**：

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
    // ... 每种类型都先检查 JSON 值类型是否匹配
}
```

**为什么类型检查重要？** 恶意客户端可以发送 `{"age": "not_a_number"}`。没有类型检查，`as_int64()` 会未定义行为或崩溃。检查后抛异常，路由可以捕获并返回 400 Bad Request。

### 3.3 C++20 回退实现 — 宏 + 成员指针

**HICAL_JSON 宏展开过程**：

```cpp
struct UserDTO {
    std::string name;
    int age;

    HICAL_JSON(UserDTO, name, age)
};
```

宏展开为：

```cpp
struct UserDTO {
    std::string name;
    int age;

    static auto hicalJsonFields()
    {
        return std::make_tuple(
            hical::meta::detail::makeField<UserDTO>("name", &UserDTO::name),
            hical::meta::detail::makeField<UserDTO>("age",  &UserDTO::age)
        );
    }
};
```

**FieldDescriptor 结构体**：

```cpp
template <typename Class, typename FieldType>
struct FieldDescriptor
{
    std::string_view name;       // 字段名（编译期字符串）
    FieldType Class::* pointer;  // 成员指针
};
```

成员指针 `&UserDTO::name` 是 C++ 中指向类成员变量的指针，可以对任意 `UserDTO` 实例取值/赋值：

```cpp
UserDTO dto;
auto ptr = &UserDTO::name;  // 类型：std::string UserDTO::*
dto.*ptr = "Alice";          // 等价于 dto.name = "Alice"
```

**序列化过程**：

```cpp
template <typename T>
boost::json::object toJson(const T& obj)
{
    auto fields = T::hicalJsonFields();                    // 获取字段元组
    boost::json::object jsonObj;
    constexpr auto count = std::tuple_size_v<decltype(fields)>;
    detail::serializeFields(obj, jsonObj, fields,
                            std::make_index_sequence<count>{});  // 编译期展开
    return jsonObj;
}

// serializeFields 用折叠表达式展开：
template <typename T, typename Tuple, size_t... I>
void serializeFields(const T& obj, boost::json::object& jsonObj,
                     const Tuple& fields, std::index_sequence<I...>)
{
    // 折叠表达式：对每个字段执行 jsonObj[name] = valueToJson(obj.*ptr)
    ((jsonObj[std::get<I>(fields).name] =
          valueToJson(obj.*std::get<I>(fields).pointer)), ...);
}
```

整个过程在编译期确定字段数量和类型，运行时无虚函数、无反射开销。

**FOR_EACH 宏的可变参数实现**：

```cpp
// 参数计数：利用 __VA_ARGS__ 位移来确定参数个数
#define HICAL_JSON_ARG_N_(_1,_2,_3,...,_16, N, ...) N
#define HICAL_JSON_NARGS_(...) HICAL_JSON_ARG_N_(__VA_ARGS__, 16,15,...,2,1)

// 分发到对应数量的展开宏
#define HICAL_JSON_FE_1(T, a) HICAL_JSON_FIELD_(T, a)
#define HICAL_JSON_FE_2(T, a, b) HICAL_JSON_FIELD_(T, a), HICAL_JSON_FIELD_(T, b)
#define HICAL_JSON_FE_3(T, a, b, c) HICAL_JSON_FE_2(T, a, b), HICAL_JSON_FIELD_(T, c)
// ... 递归式定义，支持 1-16 个字段
```

原理：预处理器没有循环，通过参数计数 → 拼接宏名 → 递归展开来模拟 `for_each`。

### 3.4 C++26 反射实现 — 全自动

```cpp
template <typename T>
boost::json::object toJson(const T& obj)
{
    boost::json::object jsonObj;

    // template for：编译期遍历 T 的所有非静态数据成员
    template for (constexpr auto member :
                  std::meta::nonstatic_data_members_of(^^T))
    {
        constexpr auto name = std::meta::identifier_of(member);  // 字段名
        jsonObj[name] = valueToJson(obj.[:member:]);              // splice 取值
    }

    return jsonObj;
}
```

**关键语法**：

| 语法                                        | 含义                                                     |
| ------------------------------------------- | -------------------------------------------------------- |
| `^^T`                                       | 反射运算符：获取类型 T 的反射元数据（`std::meta::info`） |
| `std::meta::nonstatic_data_members_of(^^T)` | 获取 T 的所有非静态数据成员列表                          |
| `std::meta::identifier_of(member)`          | 获取成员名称字符串                                       |
| `[:member:]`                                | Splice 运算符：将反射元数据"拼接"回代码，取实际值        |
| `template for`                              | 编译期循环：对每个成员生成独立的代码                     |

C++26 路线下 `HICAL_JSON` 宏定义为空：

```cpp
#define HICAL_JSON(Type, ...)  // 空操作，反射自动发现字段
```

用户只需定义结构体，不需要任何标注。

### 3.5 readJson — HTTP 请求快捷反序列化

```cpp
// MetaJson.h 底部
namespace hical {
    template <typename T>
    T HttpRequest::readJson() const
    {
        return meta::readJson<T>(*this);
    }
}

// meta::readJson 实现
template <typename T>
T readJson(const HttpRequest& req)
{
    auto json = req.jsonBody();
    if (json.is_null())
        throw std::runtime_error("readJson: request body is not valid JSON");
    return fromJson<T>(json);
}
```

**设计要点**：`readJson<T>()` 定义在 `MetaJson.h` 而非 `HttpRequest.h` 中。这是有意的解耦——`HttpRequest.h` 不需要依赖 `MetaJson.h`，只有用户 `#include "MetaJson.h"` 后才获得 `readJson` 能力。

---

## 四、MetaRoutes — 自动路由注册

### 4.1 用户 API

**源码位置**：`src/core/MetaRoutes.h`

```cpp
struct ApiHandler
{
    HttpResponse listUsers(const HttpRequest&)
    {
        return HttpResponse::json({{"users", "..."}});
    }
    HICAL_HANDLER(Get, "/api/users", listUsers)

    HttpResponse getUser(const HttpRequest& req)
    {
        return HttpResponse::json({{"userId", req.param("id")}});
    }
    HICAL_HANDLER(Get, "/api/users/{id}", getUser)

    HttpResponse createUser(const HttpRequest& req)
    {
        auto user = req.readJson<UserDTO>();
        return HttpResponse::json(meta::toJson(user));
    }
    HICAL_HANDLER(Post, "/api/users", createUser)

    HICAL_ROUTES(ApiHandler, listUsers, getUser, createUser)
};

// 一行注册所有路由
meta::registerRoutes(server.router(), handler);
```

### 4.2 RouteInfo 路由描述

```cpp
struct RouteInfo
{
    HttpMethod method;          // HTTP 方法
    std::string_view path;      // 路由路径
    std::string_view handlerName; // 处理函数名（调试用）
};
```

`HICAL_HANDLER` 宏为每个处理函数生成一个 `constexpr RouteInfo`：

```cpp
HICAL_HANDLER(Get, "/api/users", listUsers)
// 展开为：
static constexpr RouteInfo hicalRouteInfo_listUsers {
    HttpMethod::hGet, "/api/users", "listUsers"
};
```

### 4.3 C++20 回退实现 — RouteRegistrar

**HICAL_ROUTES 宏展开**：

```cpp
HICAL_ROUTES(ApiHandler, listUsers, getUser, createUser)
// 展开为：
static auto hicalRouteTable()
{
    return std::make_tuple(
        detail::makeRegistrar<ApiHandler>(ApiHandler::hicalRouteInfo_listUsers, &ApiHandler::listUsers),
        detail::makeRegistrar<ApiHandler>(ApiHandler::hicalRouteInfo_getUser,   &ApiHandler::getUser),
        detail::makeRegistrar<ApiHandler>(ApiHandler::hicalRouteInfo_createUser, &ApiHandler::createUser)
    );
}
```

**RouteRegistrar 结构体**：

```cpp
template <typename Handler, typename MemFnPtr>
struct RouteRegistrar
{
    RouteInfo info;     // 路由描述
    MemFnPtr fnPtr;     // 成员函数指针

    void apply(Router& router, std::shared_ptr<Handler> pHandler) const
    {
        registerOneRoute(router, pHandler, info, fnPtr);
    }
};
```

**注册流程**：

```
registerRoutes(router, handler)
    │
    ├── 1. static_assert(HasRouteTable<Handler>::value)
    │
    ├── 2. auto table = Handler::hicalRouteTable()  → tuple<RouteRegistrar...>
    │
    ├── 3. registerAll(router, pHandler, table, index_sequence)
    │      └── 折叠表达式：(get<I>(table).apply(router, pHandler), ...)
    │
    └── 4. 每个 apply() 调用 registerOneRoute()
           └── router.route(method, path, [pHandler, fn](req) { return (pHandler->*fn)(req); })
```

### 4.4 生命周期管理 — 为什么用 shared_ptr？

```cpp
template <typename Handler>
void registerOneRoute(Router& router,
                      std::shared_ptr<Handler> pHandler,  // ← shared_ptr
                      const RouteInfo& info,
                      HttpResponse (Handler::*fn)(const HttpRequest&))
{
    router.route(info.method, std::string(info.path),
        [pHandler, fn](const HttpRequest& req) -> HttpResponse {
            return (pHandler.get()->*fn)(req);   // lambda 捕获 shared_ptr
        });
}
```

路由回调是一个 lambda，捕获了 `pHandler`（shared_ptr）。这个 lambda 的生命周期与 Router 相同（通常是整个服务器运行期间）。如果用引用捕获 handler，而 handler 在注册后被销毁，回调就变成悬垂引用。

**引用版本的安全约定**：

```cpp
// 引用版本：空删除器，不接管所有权
template <typename Handler>
void registerRoutes(Router& router, Handler& handler)
{
    auto pHandler = std::shared_ptr<Handler>(&handler, [](Handler*){});
    registerRoutes(router, pHandler);
}
```

空删除器 `[](Handler*){}` 意味着 shared_ptr 析构时不 delete handler。调用者必须确保 handler 在 `server.start()` 期间一直存活。典型用法：handler 定义在 main 函数的栈上，`server.start()` 阻塞时不会析构。

### 4.5 同步/协程路由兼容

```cpp
// 同步路由
template <typename Handler>
void registerOneRoute(Router& router, shared_ptr<Handler> pHandler,
                      const RouteInfo& info,
                      HttpResponse (Handler::*fn)(const HttpRequest&))  // ← 返回 HttpResponse
{
    router.route(info.method, path,
        [pHandler, fn](const HttpRequest& req) -> HttpResponse {
            return (pHandler.get()->*fn)(req);
        });
}

// 协程路由
template <typename Handler>
void registerOneRoute(Router& router, shared_ptr<Handler> pHandler,
                      const RouteInfo& info,
                      Awaitable<HttpResponse> (Handler::*fn)(const HttpRequest&))  // ← 返回 Awaitable
{
    router.route(info.method, path,
        [pHandler, fn](const HttpRequest& req) -> Awaitable<HttpResponse> {
            co_return co_await (pHandler.get()->*fn)(req);
        });
}
```

两个 `registerOneRoute` 重载通过成员函数指针的返回值类型自动分发——编译器根据 `fn` 的类型选择匹配的重载。用户的 Handler 可以混合包含同步和协程方法。

### 4.6 C++26 反射实现 — 属性驱动

```cpp
template <typename Handler>
void registerRoutes(Router& router, Handler& handler)
{
    // 遍历 Handler 的所有非静态成员函数
    template for (constexpr auto fn :
                  std::meta::nonstatic_member_functions_of(^^Handler))
    {
        // 遍历函数的属性
        constexpr auto attrs = std::meta::attributes_of(fn);
        template for (constexpr auto attr : attrs)
        {
            // 检查是否带有 [[hical::route(...)]] 属性
            if constexpr (std::meta::identifier_of(attr) == "hical::route")
            {
                constexpr auto args = std::meta::attribute_arguments_of(attr);
                constexpr auto path = std::meta::extract<const char*>(args[0]);
                constexpr auto methodStr = std::meta::extract<const char*>(args[1]);

                router.route(method, path,
                    [&handler](const HttpRequest& req) -> Awaitable<HttpResponse> {
                        // 使用 if constexpr 分支同步/异步
                        if constexpr (std::is_same_v<decltype(handler.[:fn:](req)),
                                                     HttpResponse>)
                            co_return handler.[:fn:](req);
                        else
                            co_return co_await handler.[:fn:](req);
                    });
            }
        }
    }
}
```

C++26 路线下，用户用自定义属性标注路由，不需要任何宏：

```cpp
struct ApiHandler {
    [[hical::route("/api/users", "GET")]]
    HttpResponse listUsers(const HttpRequest&) { ... }
};
```

### 4.7 手动路由与反射路由共存

反射注册的路由和手动注册的路由可以共存：

```cpp
// 反射自动注册
meta::registerRoutes(server.router(), handler);

// 手动注册（不需要 Handler 类）
server.router().get("/",
    [](const HttpRequest&) { return HttpResponse::ok("Homepage"); });
```

Router 内部都是同一个路由表，两种方式注册的路由没有优先级差异。

---

## 五、完整示例分析

### 5.1 reflection_server.cpp

**源码位置**：`examples/reflection_server.cpp`

```cpp
// 1. DTO 定义（HICAL_JSON 标注字段）
struct UserDTO {
    std::string name;
    int age;
    std::string email;
    HICAL_JSON(UserDTO, name, age, email)
};

struct StatusDTO {
    std::string status;
    std::string version;
    std::string framework;
    HICAL_JSON(StatusDTO, status, version, framework)
};

// 2. Handler 定义（HICAL_HANDLER + HICAL_ROUTES 标注路由）
struct ApiHandler {
    HttpResponse getStatus(const HttpRequest&) {
        StatusDTO status {"running", "0.2.0", "hical"};
        return HttpResponse::json(meta::toJson(status));  // DTO → JSON
    }
    HICAL_HANDLER(Get, "/api/status", getStatus)

    HttpResponse createUser(const HttpRequest& req) {
        auto user = req.readJson<UserDTO>();  // JSON → DTO
        return HttpResponse::json({
            {"message", "User created"},
            {"name", user.name}, {"age", user.age}
        });
    }
    HICAL_HANDLER(Post, "/api/users", createUser)

    HICAL_ROUTES(ApiHandler, getStatus, listUsers, getUser, createUser)
};

// 3. 启动
HttpServer server(port);
server.use(logMiddleware);             // 中间件正常使用
ApiHandler handler;
meta::registerRoutes(server.router(), handler);  // 一行注册所有路由
server.start();
```

**与 http_server.cpp 的对比**：

| 方面          | http_server.cpp（第8课）           | reflection_server.cpp（第10课）        |
| ------------- | ---------------------------------- | -------------------------------------- |
| 路由注册      | `router.get("/path", lambda)` 逐条 | `registerRoutes(router, handler)` 一行 |
| JSON 处理     | 手动构建 `boost::json::object`     | `toJson(dto)` / `readJson<T>()` 自动   |
| 代码组织      | lambda 散落在 main 函数            | 方法集中在 Handler 类                  |
| 路由/路径关系 | 代码中隐含                         | `HICAL_HANDLER` 宏显式标注             |

---

## 六、从测试看行为保证

### 6.1 JSON 序列化测试

**源码位置**：`tests/test_reflection.cpp`

| 测试                       | 验证点                                           |
| -------------------------- | ------------------------------------------------ |
| `SerializeSimple`          | 基本结构体 → JSON（name/age）                    |
| `DeserializeSimple`        | JSON → 结构体，字段正确                          |
| `RoundTrip`                | 序列化再反序列化，值不变                         |
| `NestedStruct`             | 嵌套结构体（UserWithAddress 内含 NestedAddress） |
| `VectorField`              | `vector<int>` → JSON array → `vector<int>`       |
| `DoubleAndBool`            | 浮点数和布尔值的精确转换                         |
| `MissingFieldKeepsDefault` | JSON 缺少字段 → 保持 C++ 默认值（age=0）         |
| `ExtraFieldIgnored`        | JSON 多余字段 → 静默忽略，不报错                 |
| `HttpRequestReadJson`      | 从 HTTP 请求体直接反序列化                       |

**关键测试解读**：

**MissingFieldKeepsDefault**：

```cpp
boost::json::value json = {{"name", "Frank"}};  // 缺少 age
auto dto = meta::fromJson<SimpleDTO>(json);
EXPECT_EQ(dto.age, 0);  // 默认初始化
```

`T obj{}` 值初始化确保 int 字段为 0，string 为空。`deserializeFields` 中的 `trySet` 只在字段存在时赋值，不存在的字段保持默认。

**ExtraFieldIgnored**：

```cpp
boost::json::value json = {{"name", "Grace"}, {"age", 28}, {"extra", "ignored"}};
auto dto = meta::fromJson<SimpleDTO>(json);
EXPECT_EQ(dto.name, "Grace");  // extra 字段被忽略
```

`deserializeFields` 只遍历注册的字段（`hicalJsonFields()` 返回的元组），JSON 中多余的字段被自然跳过。这使得 API 具备前向兼容性——服务端新增字段不会导致旧客户端反序列化失败。

### 6.2 路由注册测试

| 测试                       | 验证点                                         |
| -------------------------- | ---------------------------------------------- |
| `RegisterRoutes`           | 3 个路由成功注册（`router.routeCount() == 3`） |
| `DispatchGetRoute`         | GET /api/users → 200 + `{"action":"list"}`     |
| `DispatchParamRoute`       | GET /api/users/42 → 200 + `{"userId":"42"}`    |
| `DispatchPostWithJsonBody` | POST /api/users + JSON body → 反序列化 + 200   |
| `RouteNotFoundReturns404`  | DELETE /api/users（未注册） → 404              |
| `RouteInfoGeneration`      | HICAL_HANDLER 宏生成的 RouteInfo 字段正确      |

**关键测试解读**：

**RouteInfoGeneration**：

```cpp
EXPECT_EQ(TestHandler::hicalRouteInfo_listUsers.method, HttpMethod::hGet);
EXPECT_EQ(TestHandler::hicalRouteInfo_listUsers.path, "/api/users");
EXPECT_EQ(TestHandler::hicalRouteInfo_listUsers.handlerName, "listUsers");
```

验证 `HICAL_HANDLER` 宏正确生成了编译期常量 `RouteInfo`。

### 6.3 编译期类型检测

| 测试                     | 验证点                                       |
| ------------------------ | -------------------------------------------- |
| `HasJsonFieldsDetection` | `SimpleDTO` → true，`int` / `string` → false |
| `HasRouteTableDetection` | `TestHandler` → true，`SimpleDTO` → false    |

---

## 七、宏展开全流程图解

### 7.1 HICAL_JSON 展开

```
HICAL_JSON(UserDTO, name, age, email)
    │
    ├── HICAL_JSON_NARGS_(name, age, email) → 3
    │
    ├── HICAL_JSON_FOR_EACH_(UserDTO, name, age, email)
    │   └── HICAL_JSON_FE_3(UserDTO, name, age, email)
    │       └── HICAL_JSON_FE_2(UserDTO, name, age), HICAL_JSON_FIELD_(UserDTO, email)
    │           └── HICAL_JSON_FIELD_(UserDTO, name), HICAL_JSON_FIELD_(UserDTO, age), ...
    │
    └── 最终展开为:
        static auto hicalJsonFields() {
            return std::make_tuple(
                makeField<UserDTO>("name",  &UserDTO::name),
                makeField<UserDTO>("age",   &UserDTO::age),
                makeField<UserDTO>("email", &UserDTO::email)
            );
        }
```

### 7.2 HICAL_HANDLER + HICAL_ROUTES 展开

```
HICAL_HANDLER(Get, "/api/users", listUsers)
    └── static constexpr RouteInfo hicalRouteInfo_listUsers {
            HttpMethod::hGet, "/api/users", "listUsers"
        };

HICAL_ROUTES(ApiHandler, listUsers, getUser)
    │
    ├── HICAL_ROUTES_COUNT_(listUsers, getUser) → 2
    │
    └── static auto hicalRouteTable() {
            return std::make_tuple(
                makeRegistrar<ApiHandler>(
                    ApiHandler::hicalRouteInfo_listUsers, &ApiHandler::listUsers),
                makeRegistrar<ApiHandler>(
                    ApiHandler::hicalRouteInfo_getUser,   &ApiHandler::getUser)
            );
        }
```

---

## 八、与游戏服务器架构的对比

| Hical 概念                | 游戏服务器等价物                                                  |
| ------------------------- | ----------------------------------------------------------------- |
| `HICAL_JSON`              | 协议结构体的序列化/反序列化标注（类似 protobuf 的 `.proto` 定义） |
| `toJson` / `fromJson`     | 消息打包/解包（Serialize/Deserialize）                            |
| `valueFromJson` 类型检查  | 消息解包时的字段类型校验                                          |
| `HICAL_HANDLER`           | 消息 ID → 处理函数的注册宏                                        |
| `HICAL_ROUTES`            | 消息处理函数集合（类似消息分发表）                                |
| `registerRoutes`          | 启动时批量注册所有消息处理函数                                    |
| Handler 类                | 消息处理模块（如 PlayerMsgHandler、GuildMsgHandler）              |
| `shared_ptr` 生命周期管理 | 异步回调中防止 handler 野指针                                     |
| 双路线策略                | 游戏中的新旧协议兼容（新协议优先，旧协议回退）                    |

---

## 九、关键问题思考与回答

**Q1: 为什么用条件编译双路线而不是只支持 C++26？**

> C++26 反射（P2996）在 2026 年还没有任何主流编译器完整支持。如果只支持 C++26，框架当前无人可用。双路线确保：
> 1. 现在：所有 C++20 编译器都能用（宏回退）
> 2. 未来：编译器支持后自动切换到零宏方案
> 3. 用户 API 不变，迁移成本为零

**Q2: HICAL_JSON 宏的 FOR_EACH 展开原理？**

> C 预处理器没有循环能力。通过三步模拟：
> 1. **参数计数**：`HICAL_JSON_ARG_N_` 利用可变参数位移确定参数个数 N
> 2. **宏名拼接**：`HICAL_JSON_PASTE_(HICAL_JSON_FE_, N)` 拼出 `HICAL_JSON_FE_3`
> 3. **递归展开**：`FE_3` 调用 `FE_2` + 一个字段，`FE_2` 调用 `FE_1` + 一个字段...
>
> 这是 C++ 宏元编程的经典技巧，限制是参数数量有上限（这里 16 个）。

**Q3: registerRoutes 为什么用 shared_ptr 而不是引用？**

> 路由回调（lambda）的生命周期与 Router 相同，通常覆盖整个 `server.start()` 阻塞期。如果用引用捕获 handler，handler 提前析构会导致悬垂引用。`shared_ptr` 通过引用计数确保 handler 在有回调引用时不会被析构。
>
> 引用版本 `registerRoutes(router, handler&)` 内部用空删除器 `[](Handler*){}` 创建 shared_ptr，不接管所有权，适合 handler 定义在 main 函数栈上的典型场景。

**Q4: valueFromJson 类型检查为什么重要？**

> 恶意客户端可以发送格式错误的 JSON：`{"name": 123, "age": "not_a_number"}`。
> - 没有检查：`val.as_string()` 在 val 是 int 时未定义行为
> - 有检查：抛 `runtime_error("JSON type mismatch")`，路由可以捕获并返回 400

**Q5: template for 循环是什么含义？**

> `template for` 是 C++26 反射提案中的编译期循环。对于：
> ```cpp
> template for (constexpr auto member : std::meta::nonstatic_data_members_of(^^T))
> ```
> 编译器为 T 的每个数据成员生成一份独立的循环体代码。类似于 `if constexpr` 的编译期分支，`template for` 是编译期循环——运行时没有循环开销。

---

## 十、从第0课到第10课的全链路回顾

```
C++20 特性（第0课）
    │
    ├── Concepts → 后端约束（第1课）
    ├── Coroutines → 所有异步操作（贯穿始终）
    ├── PMR → 三级内存池（第4课）
    └── if constexpr → TCP/SSL 统一（第5课）
         │
抽象接口（第1课）                    错误/地址（第2课）
    EventLoop / Timer / TcpConnection   ErrorCode / InetAddress / HttpTypes
         │                                  │
Asio 实现（第3课）                         │
    AsioEventLoop / AsioTimer / EventLoopPool
         │                                  │
内存管理（第4课）                           │
    MemoryPool / TrackedResource / PmrBuffer
         │                                  │
TCP 连接与服务器（第5课）                   │
    GenericConnection / TcpServer / SslContext
         │                                  │
HTTP 协议与路由（第6课）  ←─────────────────┘
    HttpRequest / HttpResponse / Router
         │
中间件与 WebSocket（第7课）
    MiddlewarePipeline / WebSocketSession
         │
HttpServer 整合（第8课）
    acceptLoop → handleSession → middleware → router → response
         │
Cookie / Session / 文件服务（第9课）
    Cookie → Session 中间件 → StaticFiles → Multipart
         │
C++26 反射层（第10课） ← 你在这里
    Reflection.h / MetaJson.h / MetaRoutes.h
    HICAL_JSON → toJson/fromJson    自动 JSON 序列化
    HICAL_HANDLER/ROUTES → registerRoutes    自动路由注册
         │
综合项目
    项目 A：性能压测 / 项目 B：扩展新功能
```

---

## 十一、课程总结

本课学习了 hical 的反射层——框架最顶层的自动化设施：

| 模块             | 核心收获                                                                                             |
| ---------------- | ---------------------------------------------------------------------------------------------------- |
| **Reflection.h** | 特性检测宏 `HICAL_HAS_REFLECTION`、`HasJsonFields`/`HasRouteTable` SFINAE 检测                       |
| **MetaJson.h**   | `valueToJson`/`valueFromJson` if constexpr 类型分发、`FieldDescriptor` 成员指针绑定、FOR_EACH 宏展开 |
| **MetaRoutes.h** | `RouteRegistrar` + 折叠表达式批量注册、`shared_ptr` 生命周期管理、同步/协程双重载                    |
| **双路线策略**   | 相同用户 API，C++26 零宏 / C++20 宏回退，迁移成本为零                                                |

**设计原则总结**：
- **零成本抽象**：宏 + 模板在编译期展开，运行时无额外开销
- **渐进式增强**：C++20 能用，C++26 更好，API 不变
- **安全边界**：`valueFromJson` 类型检查防止恶意输入，`static_assert` 防止误用

至此，hical 框架 0-10 课全部完成。**接下来进入综合项目阶段**：
- **项目 A**：性能压测与分析
- **项目 B**：动手扩展新功能

---

*上一课：[第9课 - Cookie、Session 与文件服务]({{< relref "lesson-09-notes" >}}) | 下一步：[项目A - 性能压测与分析]({{< relref "project-a-notes" >}})*
