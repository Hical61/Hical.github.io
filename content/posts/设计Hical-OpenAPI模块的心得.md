+++
title = '设计 Hical OpenAPI 模块的心得'
date = '2026-04-30'
draft = false
tags = ["C++20", "OpenAPI", "Swagger", "反射", "JSON Schema", "Hical"]
categories = ["Hical 框架笔记"]
description = "为 Hical C++20 Web 框架设计 OpenAPI 3.0 自动生成模块的完整复盘：四层分离架构、零侵入设计、C++20 编译期类型推导、$ref 策略，以及宏与模板的取舍。"
+++

# 设计 Hical OpenAPI 模块的心得

## 一、为什么要做这件事

Hical 框架从一开始就内建了两套反射基础设施：MetaJson（DTO 字段反射）和 MetaRoutes（路由反射）。这意味着框架在编译期就已经知道"每个结构体有哪些字段、叫什么名字、什么类型、是否必填"以及"每个 Handler 有哪些路由、什么方法、什么路径"。

这些信息恰好是生成 OpenAPI spec 所需要的全部输入。

在 C++ Web 框架领域，Drogon 通过插件支持 Swagger，Oat++ 有内建的 API 文档生成，但没有任何 C++ 框架能做到"从 C++20 宏反射层自动导出 OpenAPI 3.0 spec"这件事。这是 Hical 反射层设计的一个天然延伸，也是最有潜力的差异化卖点。

做之前我最担心的问题是：C++20 的宏回退路径能不能提取足够的类型信息来生成 JSON Schema？C++26 的 `jsonSchema<T>()` 可以用 `std::meta::type_of(member)` 直接获取字段类型，但 C++20 只有 `FieldDescriptor<Class, FieldType>` 的成员指针。后来发现，通过 `decltype(std::declval<T>().*(field.pointer))` 在 fold expression 的每次展开中推导出具体的 `FieldType`，完全可以做到。这个验证结果让我决定动手。

---

## 二、架构决策

### 四层分离

最终的架构是四层设计：

```
Layer 4: OpenApiEndpoint.h        — serveOpenApi() / Swagger UI
Layer 3: OpenApiDocument.h/cpp    — 文档组装
Layer 2: OpenApiRegistry.h/cpp    — 路由元数据收集 + 标注宏
Layer 1: OpenApiSchema.h          — JSON Schema 生成
```

这不是一开始就想好的。最初的想法是把所有东西塞进一个 `OpenApi.h` 里——Schema 生成、路由收集、文档组装、端点注册全放一起。但很快发现几个问题：

1. Schema 生成是纯模板代码（编译期），而 Registry 和 Document 需要运行时状态（`.cpp` 实现），混在一起会让头文件既重又难以理解
2. 用户可能只想用 `jsonSchema<T>()` 生成 Schema 而不需要完整的文档服务，单一文件意味着不必要的编译依赖
3. 端点暴露（Swagger UI HTML）是纯配置代码，和核心逻辑混在一起影响可读性

分层之后，每一层只依赖下一层，用户可以按需引入。

### 零侵入设计

这是从项目一开始就确定的硬约束：**不修改 MetaJson.h、MetaRoutes.h、Router.h 中的任何一行代码**。

原因很务实——这三个文件是框架的核心，已经有完善的测试覆盖。如果为了 OpenAPI 功能去改动它们，不仅增加回归风险，还会让 OpenAPI 模块和核心逻辑产生耦合。后续任何一方的修改都可能牵连另一方。

最终的方案是：OpenAPI 模块只**读取**现有反射层的元数据（通过 `T::hicalJsonFields()` 和 `T::hicalRouteTable()`），不写入也不修改。这意味着关闭 `-DHICAL_WITH_OPENAPI=OFF` 后，核心框架完全不受影响。

### 路由信息收集：三个方案的取舍

如何拿到"所有已注册路由的 method + path 列表"是一个关键决策点。分析了三个方案：

**方案 A：从 HICAL_HANDLER 的静态常量收集。** 可行但不够优雅——`RouteInfo` 分散在 Handler 类中的 `static constexpr` 成员里，需要额外的宏来收集。

**方案 B：给 Router 添加 `listRoutes()` API。** 侵入性最强——Router 的路由存储在私有的 `staticRoutes_` 和 `paramRoutesByMethod_` 中，暴露它们破坏封装，且违反了零侵入约束。

**方案 C：在 `registerRoutes()` 过程中旁路收集。** 提供 `registerRoutesWithOpenApi()` 包装函数，内部先调用 `registerRoutes()` 完成正常注册，然后遍历 `Handler::hicalRouteTable()` 提取元数据到 Registry。

选了方案 C 的变体。它的好处是：不需要修改任何现有文件，不需要暴露 Router 的内部结构，用户只需要把 `registerRoutes` 换成 `registerRoutesWithOpenApi` 就能启用 OpenAPI。

---

## 三、技术难点与解法

### 难点 1：C++20 回退路径的编译期类型推导

C++26 反射中，获取字段类型很直接：

```cpp
using FT = typename[:std::meta::type_of(member):];
```

但 C++20 回退路径中，字段信息存储在 `FieldDescriptor<Class, FieldType>` 里，`FieldType` 是模板参数。在 fold expression 遍历 tuple 时，需要从成员指针中推导出类型：

```cpp
auto processOne = [&](const auto& field)
{
    using FT = std::remove_reference_t<decltype(std::declval<T>().*(field.pointer))>;
    props[field.name] = typeToSchema<FT>();
};
(processOne(std::get<I>(fields)), ...);
```

关键在于 `std::get<I>(fields)` 对每个 `I` 返回不同的具体类型，所以 `processOne` 的每次实例化都有独立的 `FT`。这个模式在 MetaJson.h 的 `serializeFields` 和 `deserializeFields` 中已经验证过了，直接复用。

### 难点 2：handlerName 到 API 标注的编译期关联

`HICAL_HANDLER` 宏生成的 `RouteInfo` 包含 `handlerName`（`string_view`），但这是一个运行时值，没法在编译期拼接成 `hicalApiInfo_<funcName>` 这样的成员名。

最初的设计是在 `HICAL_OA_INFO_` 宏中用 `requires` 表达式检测成员是否存在：

```cpp
#define HICAL_OA_INFO_(T, func)                     \
    []() -> RouteApiInfo {                           \
        if constexpr (requires { T::hicalApiInfo_##func; }) \
            return T::hicalApiInfo_##func;           \
        else                                         \
            return RouteApiInfo{.operationId = #func}; \
    }()
```

这在概念上很优雅——有标注就用标注，没标注就生成默认值。但实际编译时 GCC 报了一个让我困惑的错误：`'hicalApiInfo_plain' is not a member of 'MixedHandler'`。

原因是 GCC 在展开 `requires { T::hicalApiInfo_##func; }` 时，如果 `T` 是一个正在定义中的不完整类型（宏在类内部展开），`requires` 表达式的求值行为和 SFINAE 不同——它不是"替换失败不是错误"，而是直接报硬错误。

最终的解法是放弃在宏层面做成员存在性检测，改为要求用户显式声明：如果某个路由不需要详细标注，就用 `HICAL_API_DEFAULT(func)` 生成一个空标注。这个方案虽然多了一行代码，但语义更清晰，编译器行为也更可预测。

### 难点 3：$ref 与内联的决策

OpenAPI 的 Schema Object 支持两种引用嵌套类型的方式：`$ref`（引用 `components/schemas` 中的定义）和内联展开。C++20 中类型没有天然的"名字"（`typeid` 返回的是 mangled name），所以需要用户显式注册：

```cpp
HICAL_SCHEMA_NAME(UserDTO, "UserDTO")
```

这个宏展开为 `SchemaName<UserDTO>` 的模板特化。在 `typeToSchema<T>()` 中，通过 `if constexpr (SchemaName<T>::value != nullptr)` 在编译期决定生成 `$ref` 还是内联。

当初考虑过自动从类名推导（比如用 `__PRETTY_FUNCTION__` 提取类型名），但跨编译器的行为不一致，且 mangled name 对用户不友好。最终还是选择了显式注册，一行代码换来的是确定性和可读性。

---

## 四、设计中的取舍

### 用宏还是用模板

`HICAL_API()` 的标注语法最终选择了"宏体内执行 builder 函数"的 DSL 风格：

```cpp
HICAL_API(listUsers,
    builder::summary(info, "List all users");
    builder::tags(info, {"users"});
    builder::response<UserDTO>(info, 200, "User list"))
```

这里 `info` 是宏体内的局部变量名，`builder::*` 是普通的 inline 函数。考虑过纯模板的方案（类似 `std::tuple` 式的声明式 API），但那样会让用户写法变得非常啰嗦，且错误信息难以理解。宏方案的缺点是 `info` 这个名字和用户代码可能冲突，但在 `[]() { ... }()` 的 lambda 体内，冲突概率极低。

### 缓存策略

`OpenApiDocument::generateString()` 采用了最简单的"生成一次，永久缓存"策略，通过 `mutex + bool flag` 实现。考虑过 `std::call_once`，但它不支持 `invalidate()` 后重新生成。也考虑过 `shared_mutex`（读写锁），但 OpenAPI 文档的读写比极端偏向读（通常整个生命周期只写一次），`mutex + bool` 足够了。

### 默认开启还是关闭

`HICAL_WITH_OPENAPI` 默认 ON。理由是：OpenAPI 只增加了两个 `.cpp` 文件的编译开销，不引入新的外部依赖（复用 Boost.JSON），对不使用的用户几乎零成本。这和 DB 中间件（默认 OFF）不同——DB 需要额外的 Boost.MySQL + Boost.charconv 依赖。

---

## 五、最终实现

| 指标         | 数值                                                                                |
| ------------ | ----------------------------------------------------------------------------------- |
| 新增头文件   | 4 个（OpenApiSchema.h / OpenApiRegistry.h / OpenApiDocument.h / OpenApiEndpoint.h） |
| 新增实现文件 | 2 个（OpenApiRegistry.cpp / OpenApiDocument.cpp）                                   |
| 新增测试     | 35 个（覆盖 Schema / Registry / Document / Endpoint / 集成）                        |
| 修改现有文件 | 0 个核心头文件（零侵入）                                                            |


用户端的最小使用代码：

```cpp
// 定义 DTO（已有的 HICAL_JSON 不变）
struct UserDTO { ... HICAL_JSON(UserDTO, REQUIRED(name), age) };
HICAL_SCHEMA_NAME(UserDTO, "UserDTO")

// Handler 标注（在 HICAL_HANDLER 后加一行 HICAL_API）
HICAL_API(listUsers,
    builder::summary(info, "List users");
    builder::response<UserDTO>(info, 200, "OK"))
HICAL_ROUTES_WITH_API(MyHandler, listUsers, ...)

// 注册 + 暴露（两行代码）
registerRoutesWithOpenApi(router, handler, *registry);
serveOpenApi(router, doc);  // → /openapi.json + /docs
```

---

## 六、如果重来一次

1. **一开始就写 `HICAL_API_DEFAULT`**：而不是试图在宏层面做成员存在性检测。在 C++ 的宏/模板交互中，"显式声明"几乎总是比"隐式推导"更可靠
2. **先写测试再写实现**：Schema 生成的测试用例非常适合 TDD——类型映射表就是测试用例列表，先写测试可以更早发现 `boost::json::array::push_back(std::string)` 这类 API 兼容性问题
3. **从 Review 开始就考虑返回值类型**：`routes()` 返回引用 vs 副本的决策应该在 API 设计阶段就确定，而不是在 Review 修复线程安全问题时被动改变
