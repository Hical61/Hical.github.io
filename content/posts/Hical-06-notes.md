+++
title = '第6课：HTTP 协议与路由'
date = '2026-04-15'
draft = false
tags = ["C++", "HTTP", "路由", "Boost.Beast", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "理解 HttpRequest/HttpResponse 对 Boost.Beast 的封装，掌握 Router 的双策略设计：O(1) 静态路由 + 参数路由。"
+++

# 第6课：HTTP 协议与路由 - 学习笔记

> 理解 HttpRequest / HttpResponse 对 Boost.Beast 的封装，掌握 Router 的双策略设计：O(1) 静态路由 + 参数路由。

---

## 一、HttpRequest — 请求封装

### 1.1 设计思想

**源码位置**：`src/core/HttpRequest.h` / `src/core/HttpRequest.cpp`

HttpRequest 对 `boost::beast::http::request<string_body>` 做了一层 **门面封装**，隐藏 Beast 的复杂接口：

```cpp
class HttpRequest {
    using BeastRequest = boost::beast::http::request<boost::beast::http::string_body>;

    BeastRequest req_;                                              // 底层 Beast 请求
    std::unordered_map<std::string, std::string> pathParams_;       // 路径参数
};
```

**对外暴露的简洁接口 vs Beast 原始接口**：

| Hical                               | Beast 原始                                | 说明                   |
| ----------------------------------- | ----------------------------------------- | ---------------------- |
| `req.method()` → `HttpMethod::hGet` | `req.method()` → `beast::http::verb::get` | 使用框架自有枚举       |
| `req.path()` → `"/api/users"`       | 需要手动从 `target()` 中切分              | 自动去除查询参数       |
| `req.query()` → `"page=1"`          | 需要手动查找 `?` 位置                     | 自动提取               |
| `req.param("id")` → `"42"`          | 不支持                                    | 路径参数由 Router 注入 |
| `req.jsonBody()` → `json::value`    | 需要手动 `json::parse(req.body())`        | 一步到位               |

### 1.2 路径与查询参数分离

```cpp
std::string HttpRequest::path() const {
    std::string t(req_.target());     // 例："/api/users?page=1&size=10"
    auto pos = t.find('?');
    if (pos != std::string::npos)
        return t.substr(0, pos);      // → "/api/users"
    return t;
}

std::string HttpRequest::query() const {
    std::string t(req_.target());
    auto pos = t.find('?');
    if (pos != std::string::npos)
        return t.substr(pos + 1);     // → "page=1&size=10"
    return "";
}
```

### 1.3 路径参数系统

路径参数是 Router 在分发时注入到 HttpRequest 中的：

```cpp
// Router 匹配成功后：
for (const auto& [name, value] : params) {
    req.setParam(name, value);        // 注入参数
}

// 处理函数中使用：
router.get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
    auto userId = req.param("id");    // 提取参数 → "42"
    return HttpResponse::ok("User: " + userId);
});
```

底层是 `unordered_map<string, string>`，哈希查找 O(1)。

### 1.4 JSON 解析

```cpp
boost::json::value HttpRequest::jsonBody() const {
    boost::system::error_code ec;
    auto val = boost::json::parse(req_.body(), ec);
    if (ec) return nullptr;   // 解析失败返回 null，不抛异常
    return val;
}
```

**安全设计**：解析失败返回 `nullptr` 而不是抛异常，让上层可以检查 `json.is_null()` 并返回 400 Bad Request。

### 1.5 方法映射

```cpp
HttpMethod HttpRequest::method() const {
    switch (req_.method()) {
        case beast::http::verb::get:     return HttpMethod::hGet;
        case beast::http::verb::post:    return HttpMethod::hPost;
        case beast::http::verb::delete_: return HttpMethod::hDelete;  // C++ 关键字冲突加 _
        // ...
    }
}
```

Beast 用 `verb` 枚举，Hical 用自定义 `HttpMethod` 枚举。两套映射通过 switch-case 桥接，使上层代码不依赖 Beast。

---

## 二、HttpResponse — 响应封装

### 2.1 基础结构

**源码位置**：`src/core/HttpResponse.h` / `src/core/HttpResponse.cpp`

```cpp
class HttpResponse {
    using BeastResponse = boost::beast::http::response<boost::beast::http::string_body>;
    BeastResponse res_;
};
```

默认构造为 HTTP/1.1 200 OK：

```cpp
HttpResponse::HttpResponse() {
    res_.version(11);   // HTTP/1.1
    res_.result(boost::beast::http::status::ok);
}
```

### 2.2 setBody 与 prepare_payload

```cpp
void HttpResponse::setBody(const std::string& body, const std::string& contentType) {
    res_.body() = body;
    res_.set(boost::beast::http::field::content_type, contentType);
    res_.prepare_payload();   // 自动计算 Content-Length
}
```

`prepare_payload()` 是 Beast 的关键方法：根据 body 自动设置 `Content-Length` 或 `Transfer-Encoding`。不调用它，HTTP 响应的长度头就会缺失。

### 2.3 工厂方法 — 减少样板代码

```cpp
// 不用工厂方法（繁琐）：
HttpResponse res;
res.setStatus(HttpStatusCode::hNotFound);
res.setBody("Not Found");

// 用工厂方法（一行）：
auto res = HttpResponse::notFound();
```

**5 个工厂方法**：

| 方法              | 状态码 | Content-Type     | 用途           |
| ----------------- | ------ | ---------------- | -------------- |
| `ok(body)`        | 200    | text/plain       | 通用成功       |
| `json(value)`     | 200    | application/json | JSON 响应      |
| `notFound()`      | 404    | text/plain       | 路由未匹配     |
| `badRequest(msg)` | 400    | text/plain       | 参数校验失败   |
| `serverError()`   | 500    | text/plain       | 服务器内部错误 |

### 2.4 JSON 响应

```cpp
void HttpResponse::setJsonBody(const boost::json::value& json) {
    res_.body() = boost::json::serialize(json);                    // JSON → 字符串
    res_.set(boost::beast::http::field::content_type, "application/json");
    res_.prepare_payload();
}

// 工厂简写：
auto res = HttpResponse::json({{"status", "ok"}, {"count", 42}});
```

`boost::json::value` 支持初始化列表构造，`{{"key", "value"}}` 直接创建 JSON 对象，非常简洁。

---

## 三、Router — 路由系统

### 3.1 双策略设计

**源码位置**：`src/core/Router.h` / `src/core/Router.cpp`

Router 将路由分为两类，采用不同的查找策略：

```
                    dispatch(req)
                        │
                        ▼
              ┌── 路径深度检查 ──┐
              │  > 32 段？      │
              │  → 400          │
              └────────┬────────┘
                       │
          ┌────────────▼────────────┐
          │ 1. 静态路由查找 (O(1)) │
          │    unordered_map        │
          │    key = {method, path} │
          └─────────┬───────────────┘
                    │ 未命中
                    ▼
          ┌─────────────────────────┐
          │ 2. 参数路由匹配 (O(N)) │
          │    vector 线性扫描      │
          │    逐段比较 + 参数提取  │
          └─────────┬───────────────┘
                    │ 未命中
                    ▼
              404 Not Found
```

### 3.2 静态路由 — 哈希表 O(1)

```cpp
struct RouteKey {
    HttpMethod method;
    std::string path;

    bool operator==(const RouteKey& other) const {
        return method == other.method && path == other.path;
    }
};

struct RouteKeyHash {
    size_t operator()(const RouteKey& key) const {
        auto h1 = std::hash<int>{}(static_cast<int>(key.method));
        auto h2 = std::hash<std::string>{}(key.path);
        return h1 ^ (h2 << 1);   // 组合哈希
    }
};

std::unordered_map<RouteKey, RouteHandler, RouteKeyHash> staticRoutes_;
```

**组合键设计**：`{method, path}` 作为哈希键，这样 `GET /api/users` 和 `POST /api/users` 是两条不同的路由。

**查找过程**：
```
请求 GET /api/users
    → 计算 hash({hGet, "/api/users"})
    → 在 unordered_map 中 O(1) 查找
    → 命中 → 调用处理函数
```

### 3.3 参数路由 — 线性匹配

```cpp
struct ParamRouteEntry {
    HttpMethod method;
    std::string path;         // 如 "/users/{id}/posts/{postId}"
    RouteHandler handler;
};

std::vector<ParamRouteEntry> paramRoutes_;
```

**为什么不用 Trie 树？**

| 方案         | 优点                                  | 缺点                        |
| ------------ | ------------------------------------- | --------------------------- |
| **线性扫描** | 实现简单，参数路由通常很少（< 50 条） | O(N)                        |
| Trie 树      | O(路径深度)，更快                     | 实现复杂，参数节点处理困难  |
| 正则表达式   | 最灵活                                | 性能最差，安全隐患（ReDoS） |

实际场景：大多数 Web 应用的参数路由不超过几十条，线性扫描的绝对耗时在微秒级，不是瓶颈。

### 3.4 路由注册 — 自动分类

```cpp
void Router::route(HttpMethod method, const std::string& path, RouteHandler handler) {
    if (isParamRoute(path)) {
        paramRoutes_.push_back({method, path, std::move(handler)});  // 有 {param} → 参数路由
    } else {
        staticRoutes_[{method, path}] = std::move(handler);         // 无 {param} → 静态路由
    }
}

static bool isParamRoute(const std::string& path) {
    return path.find('{') != std::string::npos;   // 简单判断
}
```

### 3.5 同步处理器自动包装

```cpp
// 同步处理器
void Router::route(HttpMethod method, const std::string& path, SyncRouteHandler handler) {
    auto asyncHandler = [h = std::move(handler)](const HttpRequest& req) -> Awaitable<HttpResponse> {
        co_return h(req);   // 同步函数被包装为协程
    };
    route(method, path, std::move(asyncHandler));
}
```

这样用户可以同时使用两种风格：

```cpp
// 同步（简单场景）
router.get("/api/ping", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::ok("pong");
});

// 协程（需要异步操作）
router.get("/api/data", [](const HttpRequest&) -> Awaitable<HttpResponse> {
    co_await sleep(0.01);  // 模拟异步数据库查询
    co_return HttpResponse::json({{"data", "value"}});
});
```

### 3.6 dispatch — 分发流程

```cpp
Awaitable<HttpResponse> Router::dispatch(HttpRequest& req) {
    auto reqMethod = req.method();
    auto reqPath = req.path();

    // 安全检查：路径深度限制
    size_t segmentCount = 0;
    for (char c : reqPath) {
        if (c == '/') ++segmentCount;
    }
    if (segmentCount > hMaxPathSegments) {     // > 32 段
        co_return HttpResponse::badRequest("Path too deep");
    }

    // 1. 优先静态路由（O(1)）
    auto it = staticRoutes_.find({reqMethod, reqPath});
    if (it != staticRoutes_.end()) {
        co_return co_await it->second(req);
    }

    // 2. 回退参数路由（O(N)）
    for (const auto& entry : paramRoutes_) {
        if (entry.method != reqMethod) continue;   // 方法不匹配，跳过

        std::unordered_map<std::string, std::string> params;
        if (matchParamPath(entry.path, reqPath, params)) {
            for (const auto& [name, value] : params) {
                req.setParam(name, value);          // 注入路径参数
            }
            co_return co_await entry.handler(req);
        }
    }

    co_return HttpResponse::notFound();
}
```

### 3.7 matchParamPath — 参数匹配算法

```cpp
// 模式: "/users/{id}/posts/{postId}"
// 路径: "/users/123/posts/456"
// 结果: {"id": "123", "postId": "456"}

static bool matchParamPath(string_view pattern, string_view path,
                           unordered_map<string, string>& params) {
    // 跳过前导 '/'
    if (!pattern.empty() && pattern.front() == '/') pattern.remove_prefix(1);
    if (!path.empty() && path.front() == '/')       path.remove_prefix(1);

    size_t segmentCount = 0;
    while (!pattern.empty() && !path.empty()) {
        if (++segmentCount > hMaxPathSegments) return false;   // 段数限制

        // 提取当前段
        auto patSeg = pattern.substr(0, pattern.find('/'));
        auto reqSeg = path.substr(0, path.find('/'));

        // 推进到下一段...

        if (patSeg 是 {param} 格式) {
            if (reqSeg.size() > hMaxParamValueLength) return false;  // 值长度限制
            params[paramName] = string(reqSeg);    // 提取参数
        } else if (patSeg != reqSeg) {
            return false;                           // 静态段不匹配
        }
    }

    return pattern.empty() && path.empty();  // 两边必须同时用完
}
```

**零分配优化**：使用 `string_view` 做原地切分，不产生临时 string 对象。只在提取参数值时才创建 string。

### 3.8 安全限制

| 限制                          | 值                   | 防御目标             |
| ----------------------------- | -------------------- | -------------------- |
| `hMaxPathSegments = 32`       | 最大 32 段           | 防止超深路径 DoS     |
| `hMaxParamValueLength = 1024` | 参数值最长 1024 字符 | 防止超长参数消耗内存 |

---

## 四、WebSocket 路由

```cpp
struct WsRoute {
    std::string path;
    WsMessageCallback onMessage;
    WsConnectCallback onConnect;
};

void Router::ws(const std::string& path, WsMessageCallback onMessage, WsConnectCallback onConnect) {
    wsRoutes_.push_back({path, std::move(onMessage), std::move(onConnect)});
}

const Router::WsRoute* Router::findWsRoute(const std::string& path) const {
    for (const auto& route : wsRoutes_) {
        if (route.path == path) return &route;
    }
    return nullptr;
}
```

WebSocket 路由独立存储，通过 `findWsRoute` 查找。HttpServer 在接收到 WebSocket 升级请求时调用此方法。

---

## 五、HICAL_ROUTE 宏

```cpp
#define HICAL_ROUTE(router, method, path, handler) \
    (router).route(::hical::HttpMethod::h##method, path, handler)

// 使用：
HICAL_ROUTE(router, Get, "/api/test", myHandler);
// 展开为：
(router).route(::hical::HttpMethod::hGet, "/api/test", myHandler);
```

`##` 是 token 粘贴操作符，将 `h` 和 `Get` 拼接为 `hGet`。

---

## 五-B、反射驱动的自动路由注册与 JSON 序列化

### 自动路由注册（MetaRoutes.h）

手动逐条注册路由虽然直观，但在路由多时容易遗漏。`MetaRoutes.h` 提供了**自动注册**机制：

```cpp
// 1. 定义 Handler struct，标注路由
struct UserHandler
{
    HttpResponse listUsers(const HttpRequest&) { ... }
    HICAL_HANDLER(Get, "/api/users", listUsers)

    HttpResponse getUser(const HttpRequest& req) { ... }
    HICAL_HANDLER(Get, "/api/users/{id}", getUser)

    HICAL_ROUTES(UserHandler, listUsers, getUser)
};

// 2. 一行注册所有路由
UserHandler handler;
meta::registerRoutes(router, handler);
```

`HICAL_HANDLER` 为每个成员函数生成一个 `static constexpr RouteInfo`，`HICAL_ROUTES` 收集它们为 tuple，`registerRoutes` 遍历 tuple 逐个注册到 Router。

### 自动 JSON 反序列化（readJson）

```cpp
struct UserDTO {
    std::string name;
    int age;
    HICAL_JSON(UserDTO, name, age)
};

// 在路由处理器中：一行反序列化请求体为结构体
router.post("/api/users", [](const HttpRequest& req) -> HttpResponse {
    auto user = req.readJson<UserDTO>();  // 需要 #include "MetaJson.h"
    return HttpResponse::json({{"created", user.name}});
});
```

`readJson<T>()` 内部调用 `meta::fromJson<T>()`，自动将 JSON 字段映射到 struct 成员，带类型安全检查。

这些功能在第9课中会深入讲解。

---

## 六、从测试看完整用法

### 6.1 路由功能测试

**源码位置**：`tests/test_router.cpp`

| 测试                            | 验证点                                           |
| ------------------------------- | ------------------------------------------------ |
| `EmptyRouterReturns404`         | 空路由器任何请求返回 404                         |
| `GetRoute`                      | GET 路由注册和匹配                               |
| `PostRoute`                     | POST 路由接收 body                               |
| `MethodMismatchReturns404`      | GET 路由不匹配 POST 请求                         |
| `PathMismatchReturns404`        | 路径不存在返回 404                               |
| `MultipleRoutes`                | 同路径不同方法共存                               |
| `AsyncRouteHandler`             | 协程处理器（带 sleep）正常工作                   |
| `JsonRoute`                     | JSON 响应 Content-Type 正确                      |
| `HicalRouteMacro`               | HICAL_ROUTE 宏正常展开                           |
| `PathParameter`                 | `/users/{id}` 匹配 `/users/42`，param("id")="42" |
| `MultiplePathParameters`        | 多个参数同时提取                                 |
| `PathParameterMismatchSegments` | 段数不匹配返回 404                               |
| `MixedStaticAndParam`           | 静态段 + 参数段混合匹配                          |

**runCoroutine 辅助函数**：

测试中需要运行协程（`dispatch` 返回 `Awaitable`），所以封装了一个辅助函数：

```cpp
template <typename F>
auto runCoroutine(AsioEventLoop& loop, F&& f) {
    std::optional<ReturnType> result;
    std::atomic<bool> done{false};

    coSpawn(loop.getIoContext(), [&]() -> Awaitable<void> {
        result = co_await f();
        done = true;
    });

    std::thread loopThread([&loop]() { loop.run(); });

    // 轮询等待完成
    for (int i = 0; i < 100 && !done.load(); ++i)
        std::this_thread::sleep_for(10ms);

    loop.stop();
    loopThread.join();
    return result;
}
```

### 6.2 路由性能测试

**源码位置**：`tests/test_router_perf.cpp`

| 测试                  | 路由数 | 场景                 | 性能要求  |
| --------------------- | ------ | -------------------- | --------- |
| `StaticRouteFirstHit` | 100    | 命中第 0 条          | < 10us/次 |
| `StaticRouteLastHit`  | 100    | 命中第 99 条         | < 10us/次 |
| `StaticRouteMiss`     | 100    | 未命中               | < 10us/次 |
| `ParamRouteMatch`     | 20     | 命中第 10 条参数路由 | < 50us/次 |
| `LargeRouteSet`       | 1000   | 命中第 500 条        | < 10us/次 |

**关键验证**：

1. **哈希表的 O(1) 特性**：首条命中、末条命中、1000 条路由命中，性能应该接近
2. **参数路由 vs 静态路由**：参数路由允许更高延迟（< 50us vs < 10us），因为需要线性扫描 + 参数提取
3. **规模无关**：100 条和 1000 条静态路由的查找性能应该相当

---

## 七、设计模式总结

| 模式           | 应用                                           | 说明                       |
| -------------- | ---------------------------------------------- | -------------------------- |
| **门面模式**   | HttpRequest / HttpResponse                     | 简化 Beast 复杂接口        |
| **工厂方法**   | `HttpResponse::ok()` / `json()` / `notFound()` | 一行创建常用响应           |
| **策略模式**   | 静态路由 vs 参数路由                           | 不同类型路由用不同查找策略 |
| **适配器**     | 同步处理器 → 协程包装                          | `co_return handler(req)`   |
| **组合键哈希** | `RouteKey{method, path}`                       | method + path 组合为唯一键 |
| **零分配匹配** | `matchParamPath` 用 `string_view`              | 减少路径匹配的内存分配     |

---

## 八、关键问题思考与回答

**Q1: 静态路由用哈希表，为什么参数路由不也用 Trie 树？**

> 1. **复杂度不值**：参数路由通常 < 50 条，线性扫描的绝对耗时在微秒级
> 2. **Trie 对参数节点支持复杂**：`{id}` 是通配段，Trie 需要特殊处理"匹配任意值"
> 3. **维护成本**：线性扫描代码 ~60 行，Trie 实现可能 200+ 行
> 4. **实际场景**：参数路由的性能瓶颈不在查找上，而在 I/O 和业务逻辑上

**Q2: HttpResponse::json() 工厂方法的设计有什么好处？**

> 1. **减少样板**：一行代码完成状态码 + Content-Type + JSON 序列化 + Content-Length
> 2. **一致性**：所有 JSON 响应都设置正确的 Content-Type，不会遗漏
> 3. **可读性**：`HttpResponse::json({{"key", "val"}})` 比分步操作更直观
> 4. **避免错误**：不会忘记调用 `prepare_payload()`

**Q3: 路由为什么要限制最大 32 段和每段 1024 字符？**

> 防止 DoS 攻击：
> - 超深路径（`/a/b/c/.../z` 几千层）会导致路径匹配函数消耗大量 CPU
> - 超长参数值（如 1GB 的 `{id}`）会消耗大量内存
> - 32 段 × 1024 字符 ≈ 32KB，是合理的 URL 上限（大多数浏览器限制 URL 在 2KB-8KB）

**Q4: 同步处理函数是如何被自动包装成异步的？**

> 通过 Lambda + `co_return`：
> ```cpp
> auto asyncHandler = [h = std::move(handler)](const HttpRequest& req) -> Awaitable<HttpResponse> {
>     co_return h(req);  // 调用同步函数，结果通过 co_return 返回
> };
> ```
> `co_return` 使 Lambda 变成协程。同步函数在协程内部同步执行完毕后，结果通过协程机制返回。对调用者来说，两种处理器的接口统一为 `Awaitable<HttpResponse>`。

---

## 九、与游戏服务器的对比

| Hical 概念             | 游戏服务器等价物                         |
| ---------------------- | ---------------------------------------- |
| `Router::dispatch()`   | 消息分发器（根据消息 ID 分发到处理函数） |
| 静态路由哈希表         | 消息 ID → 处理函数的 map                 |
| 参数路由               | 子命令解析（如 `/gm kick {playerId}`）   |
| `HttpRequest`          | 网络消息包（Packet）                     |
| `HttpResponse`         | 响应消息包                               |
| `HttpResponse::json()` | 通用回包构建工具                         |
| `req.param("id")`      | 从消息包中解析字段                       |
| 路径深度/长度限制      | 消息包最大长度限制（防 DoS）             |

---

*下一课：第7课 - 中间件与 WebSocket，将学习洋葱模型中间件管道和 WebSocket 会话管理。*
