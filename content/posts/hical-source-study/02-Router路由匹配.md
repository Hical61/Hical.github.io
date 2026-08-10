+++
title = '拆开 Hical：Router 是怎么做到 O(1) 路由匹配的？'
date = 2026-08-10T00:00:00+08:00
draft = false
tags = ["Hical", "Router", "路由匹配", "源码分析", "性能"]
categories = ["Hical源码精读"]
description = "拆开 Hical 第 2 篇：Router 怎么做 O(1) 路由匹配？dispatchSync 快速路径为什么能做到 ~40ns。"
+++

# [Hical] Router 是怎么做到 O(1) 路由匹配的？兼谈 ~40ns 的 dispatchSync 快速路径

> 本专栏文章：拆开 Hical · 第 2 篇

上一篇我们跟踪了一个 HTTP 请求从 socket 字节到响应序列化的全过程。走到 Router.dispatch() 这一步时我们跳过去了——现在把它展开。

一个 HTTP 框架的 Router 基本只有一件事要做：**给定 method + path，找到对应的 handler**。听起来简单，但"怎么找"的差异可以把延迟拉开一个数量级。

---

## 1. 三种路由、三种策略

Hical 的 Router 支持三种路由：

```
静态路由:   GET /api/users         → hash map O(1)
参数路由:   GET /api/users/{id}    → per-method vector 线性匹配
通配符路由: GET /static/*path      → 优先级最低，兜底匹配
```

匹配优先级：**静态 > 参数 > 通配符**。为什么是这个顺序？静态路由一 hash 命中就返回，参数路由需要逐个匹配，通配符是兜底的——先试最快的。

---

## 2. 静态路由：透明哈希消除 string_view→string 转换

### 2.1 问题

静态路由的 key 是 `method + path` 的组合。如果存在 `unordered_map` 里：

```cpp
// 每次查找需要构造 string key
std::string key = std::string(method) + ":" + std::string(path);
auto it = staticRoutes_.find(key);  // 产生两次临时 string
```

每个请求都分配两次 string。每秒 10 万请求 → 每秒 20 万次堆分配 → 这不是"优化"，这是"灾难"。

### 2.2 解法：RouteKeyView + is_transparent

```cpp
// 组合键：method + path
struct RouteKey 
{
    HttpMethod method;
    std::string path;  // owned
};

// 轻量级 view：不做拷贝，直接引用 raw 数据
struct RouteKeyView 
{
    HttpMethod method;
    std::string_view path;  // non-owning
};

// 自定义 hash（对 Key 和 KeyView 都可用）
struct RouteKeyHash 
{
    using is_transparent = void;  // ← 关键！启用异构查找

    size_t operator()(const RouteKey& k) const 
    {
        return hashMethod(k.method) ^ hashString(k.path);
    }

    size_t operator()(RouteKeyView k) const 
    {
        return hashMethod(k.method) ^ hashString(k.path);
        // string_view 和 string 用同样的 hash 算法
    }
};

// 自定义 equal
struct RouteKeyEqual 
{
    using is_transparent = void;  // ← 关键！

    bool operator()(const RouteKey& a, const RouteKey& b) const 
    {
        return a.method == b.method && a.path == b.path;
    }

    bool operator()(RouteKeyView a, const RouteKey& b) const 
    {
        return a.method == b.method && a.path == b.path;
        // string_view 可以和 string 做相等比较
    }
};

// 最终使用
using StaticRouteMap = std::unordered_map<RouteKey, RouteHandler,
                                          RouteKeyHash, RouteKeyEqual>;

// 查找时：零 string 构造
auto it = staticRoutes_.find(RouteKeyView{method, path});
//                string_view 直接传进去，不构造临时 string
```

> 💡 `is_transparent` 是 C++14 引入的魔法标记——告诉 `unordered_map`："我的 hash/equal 能处理与 key 不同类型但等价的参数"。不开这个标记，`find(string_view)` 会先构造一个 `string`，再传进去——那透明哈希就白费了。

---

## 3. 参数路由：per-method vector + 路径段匹配

参数路由的格式是 `/api/users/{id}/posts/{postId}`。花括号里的部分是路径参数，匹配时提取出来。

```cpp
// 每个 method 维护自己的参数路由列表
struct ParamRoute 
{
    std::string pattern;          // "/api/users/{id}"
    std::vector<std::string> segments;  // ["api", "users", "{id}"]
    RouteHandler handler;
};

std::unordered_map<HttpMethod, std::vector<ParamRoute>> paramRoutes_;
```

匹配时：

```cpp
for (auto& route : paramRoutes_[method]) 
{
    if (tryMatch(route, pathSegments, params)) {
        return &route.handler;  // 找到了，params 已填充
    }
}
return nullptr;  // 404
```

为什么参数路由不是 O(1) 的？因为同一个 method 下可能有多个参数路由（`/api/{type}/list` 和 `/api/{type}/detail`），必须逐个尝试。但通常情况下同一 method 的参数路由数量是 O(1) 级别的（5-20 个），线性匹配不是瓶颈。

### 3.1 路径段数预检

在匹配之前先检查路径段数：

```cpp
// 参数路由匹配
static constexpr size_t kMaxPathSegments = 32;
static constexpr size_t kMaxParamValueLength = 1024;

bool resolveRoute(method, path, ...) 
{
    auto segments = splitPath(path);  // 按 '/' 分割

    // 段数不匹配 → 直接跳过
    for (auto& route : paramRoutes_[method]) 
    {
        if (segments.size() != route.segments.size()) {
            continue;  // O(1) 排除
        }

        // 逐段匹配
        if (matchSegments(segments, route.segments, params)) 
        {
            return true;
        }
    }
    return false;
}
```

`kMaxPathSegments = 32` 是路径段数的硬上限——防止攻击者用超长路径耗尽 CPU 或内存。`kMaxParamValueLength = 1024` 限制单个参数值（`{id}` 部分）的长度。

---

## 4. 通配符路由：兜底匹配

通配符路由的格式是 `/static/*path`——所有以 `/static/` 开头的请求都被路由到这里，剩下的路径段作为参数传入。

```cpp
// 通配符路由：`*path` 捕获所有剩余段
struct WildcardRoute 
{
    std::string prefix;   // "/static/"
    RouteHandler handler;
};

std::vector<WildcardRoute> wildcardRoutes_;

// 匹配：优先级最低
for (auto& route : wildcardRoutes_) 
{
    if (path.starts_with(route.prefix)) 
    {
        // 提取通配符部分
        auto wildcardValue = path.substr(route.prefix.size());
        req.setAttribute("wildcard", wildcardValue);
        return &route.handler;
    }
}
```

通配符路由的关键：**它的 `*path` 捕获的是 URL 编码之前的原始路径**。因为 decode 可能改变路径结构（`%2F` → `/`），这会破坏路径段分割的语义。

---

## 5. resolveRoute：统一入口

```cpp
std::optional<HttpResponse> Router::dispatchSync(HttpRequest& req) 
{
    auto result = resolveRoute(req.method(), req.path());
    //         ↑ 统一入口：URL decode → 段数检查 → 静态 → 参数 → 通配符

    if (!result.handler) 
    {
        return HttpResponse::notFound();
    }

    if (result.type == RouteType::hSync) 
    {
        // sync handler → 直接调用，无协程帧
        auto& syncHandler = *static_cast<SyncRouteHandler*>(result.handler);
        return syncHandler(req);  // ~40-130ns
    }

    // async handler → 需要协程执行，dispatchSync 搞不定
    return std::nullopt;  // 让调用方 fallback 到 co_await dispatch()
}
```

---

## 6. dispatchSync：~40-130ns 的零协程帧快速路径

这是 Hical Router 最精妙的设计之一。

大多数 Web 框架中，所有请求都走协程调度——即使 handler 内部完全没有 `co_await`（比如返回一个静态 JSON）。协程帧分配不是免费的：每次几 KB 的堆分配 + future/promise 的构造/析构。

Hical 的做法：

```cpp
// HttpSessionImpl 中的请求处理循环
auto syncResult = router_.dispatchSync(req);  // 先试同步路径
if (syncResult) 
{
    // 同步命中 → 直接返回，零协程帧！
    co_await writeResponse(socket, *syncResult);
} 
else 
{
    // 需要协程 → fallback
    auto res = co_await router_.dispatch(req);
    co_await writeResponse(socket, res);
}
```

**dispatchSync 能省多少？** 一次协程帧分配大约：
- `operator new`：~20-50ns（取决于分配器状态和大小）
- 协程帧初始化（promise_type 构造）：~10-30ns
- future/promise 同步：~10-50ns

合计约 40-130ns。对于同步 handler（验证端点、健康检查、静态配置返回），这 40-130ns 是纯浪费。

> 💡 dispatchSync 的核心思想很简单："不调度"也是调度。当 handler 不需要协程时，Router 就不分配协程帧——跳过 ~40-130ns 的无用开销。

---

## 7. URL 解码的路径穿越保护

```cpp
std::string Router::urlDecode(std::string_view encoded) 
{
    std::string result;
    result.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i) 
    {
        if (encoded[i] == '%' && i + 2 < encoded.size()) 
        {
            int high = hexValue(encoded[i + 1]);
            int low = hexValue(encoded[i + 2]);
            char decoded = static_cast<char>((high << 4) | low);

            // 路径穿越检测：解码后的 '/' 或 '\' 拒绝
            if (decoded == '/' || decoded == '\\') {
                throw std::runtime_error("Path traversal detected");
            }

            result.push_back(decoded);
            i += 2;
        } 
        else if (encoded[i] == '+') 
        {
            result.push_back(' ');  // application/x-www-form-urlencoded 约定
        } 
        else 
        {
            result.push_back(encoded[i]);
        }
    }
    return result;
}
```

路径穿越是 Web 框架的经典漏洞——攻击者用 `%2e%2e%2f`（URL 编码的 `../`）试图访问父目录的文件。Hical 在 URL decode 阶段就检测了——一旦解码出 `/` 或 `\`，直接拒绝请求。

---

## 回顾一下你学会了什么

1. 三种路由策略：静态（hash O(1)）→ 参数（vector 线性）→ 通配符（starts_with）
2. 透明哈希 `is_transparent` 消除 string_view→string 的临时构造
3. 路径段数预检 O(1) 排除不匹配的参数路由
4. 通配符路由在 URL decode 之前匹配——防止 `%2F` 破坏段分割
5. dispatchSync 零协程帧快速路径：同步 handler 40-130ns，跳过协程帧分配
6. URL decode 时检测路径穿越——`%2f`→`/` 直接拒绝
7. 安全约束：最多 32 个路径段、参数值最长 1024 字节

---

> 下一篇：[中间件洋葱模型怎么做到"连续 N 个同步中间件只分配一次协程帧"？]({{< relref "posts/hical-source-study/03-Middleware洋葱模型.md" >}})
