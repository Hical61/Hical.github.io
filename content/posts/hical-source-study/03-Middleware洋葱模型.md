+++
title = '拆开 Hical：中间件洋葱模型怎么做到连续 N 个同步中间件只分配一次协程帧？'
date = 2026-08-10T00:00:00+08:00
draft = false
tags = ["Hical", "中间件", "洋葱模型", "协程", "源码分析"]
categories = ["Hical源码精读"]
description = "拆开 Hical 第 3 篇：中间件洋葱模型如何实现连续 N 个同步中间件只分配一次协程帧。"
+++

# [Hical] 中间件洋葱模型怎么做到"连续 N 个同步中间件只分配一次协程帧"？

> 本专栏文章：拆开 Hical · 第 3 篇

上一篇文章讲了 Router 的 dispatchSync——当 handler 是同步的，跳过协程帧分配。同样的思路也应用在中间件上。

大多数框架的中间件是"一层一个协程"——3 个中间件 = 3 个协程帧 = 3 次堆分配。但 Hical 问了一个问题：**如果 N 个中间件都是同步的（不用 co_await），为什么不能把它们合并成一个协程帧？**

答案是：可以。而且 Hical 实现了。

---

## 1. 先理解洋葱模型

### 1.1 标准中间件的三个角色

一个 HTTP 请求经过中间件管道时，有三种类型的操作：

```
请求进来 ──→ before1 ──→ before2 ──→ handler ──→ after2 ──→ after1 ──→ 响应出去
              ↑              ↑                        ↑           ↑
           前置拦截      前置处理                  后置修改     后置处理
```

- **Before**：在 handler 之前执行。可以拦截请求、直接返回响应（如认证失败返回 401）
- **Handler**：核心业务逻辑
- **After**：在 handler 之后执行。可以修改响应头（如加安全头、压缩 body）

### 1.2 传统方案：一层一个协程

```cpp
// 全异步中间件链：N 个中间件 = N 个协程 lambda
MiddlewareNext makeChain(asyncHandler1, asyncHandler2, asyncHandler3, finalHandler) 
{
    // 从内到外构建
    auto inner = [h3, finalHandler](req) -> Awaitable<HttpResponse> 
    {
        co_return co_await h3(req, finalHandler);
    };
    auto middle = [h2, inner](req) -> Awaitable<HttpResponse> 
    {
        co_return co_await h2(req, inner);
    };
    auto outer = [h1, middle](req) -> Awaitable<HttpResponse> 
    {
        co_return co_await h1(req, middle);
    };
    return outer;
}
```

3 个协程 lambda → 3 个协程帧 → 3 次堆分配。

### 1.3 但你想想：RateLimiter 需要协程吗？

```cpp
// RateLimiter 的 before：纯同步操作
SyncMiddlewareResult rateLimiter(HttpRequest& req) 
{
    auto ip = req.clientIp();
    if (!tokenBucket.consume(ip)) 
    {
        return HttpResponse::tooManyRequests();  // 拦截
    }
    return std::nullopt;  // 放行
}

// Helmet 的 after：纯同步操作
void helmet(HttpRequest& req, HttpResponse& res) 
{
    res.setHeader("X-Content-Type-Options", "nosniff");
    res.setHeader("X-Frame-Options", "DENY");
    // ... 5 个更多安全头
}
```

RateLimiter 和 Helmet 都不需要 `co_await` 任何东西。把它们套进协程 lambda 里是浪费——白分配了 2 个协程帧。

---

## 2. 解法：tagged union + 连续 Sync 合并

### 2.1 MiddlewareEntry 的 tagged union

```cpp
struct MiddlewareEntry 
{
    enum class Type { hAsync, hSync };

    Type type;
    std::string name;

    // Async：完整洋葱模型（一层一个协程帧）
    MiddlewareHandler asyncHandler;

    // Sync：前/后分离（零协程帧）
    SyncBeforeHandler before;
    SyncAfterHandler after;   // 可为空
};
```

注册时：

```cpp
// Async 中间件：走标准路径
pipeline.use([](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> 
{
    auto start = std::chrono::steady_clock::now();
    auto res = co_await next(req);                     // ← 有 co_await
    auto elapsed = std::chrono::steady_clock::now() - start;
    res.setHeader("X-Response-Time", fmt::format("{}us", elapsed.count() / 1000));
    co_return res;
});

// Sync 中间件：零协程帧
pipeline.use
(
    // before：前置处理（可拦截）
    [](HttpRequest& req) -> SyncMiddlewareResult 
    {
        if (!checkAuth(req)) 
        {
            return HttpResponse::unauthorized();  // 拦截
        }
        return std::nullopt;  // 放行
    },
    // after：后置处理（可选）
    [](HttpRequest& req, HttpResponse& res) 
    {
        res.setHeader("X-Frame-Options", "DENY");
    }
);
```

### 2.2 buildOptimizedChain：合并算法

```cpp
MiddlewareNext buildOptimizedChain(const std::vector<MiddlewareEntry>& entries,MiddlewareNext finalHandler) 
{
    if (entries.empty()) return finalHandler;

    // 快捷路径：如果全是 Async，直接退化为 buildChainFrom
    // （避免无意义的 while 循环遍历）
    if (/* entries 中没有 hSync 条目 */) 
    {
        std::vector<MiddlewareHandler> asyncHandlers;
        for (const auto& e : entries)
            asyncHandlers.push_back(e.asyncHandler);
        return buildChainFrom(asyncHandlers, std::move(finalHandler));
    }

    MiddlewareNext current = std::move(finalHandler);

    int i = entries.size() - 1;
    while (i >= 0) 
    {
        if (entries[i].type == MiddlewareEntry::Type::hAsync) 
        {
            // Async 中间件：独立协程 lambda
            auto mw = entries[i].asyncHandler;
            current = [mw, next = current](HttpRequest& r) -> Awaitable<HttpResponse> 
            {
                // 直接 return 转发 awaitable，不创建额外协程帧
                return mw(r, next);
            };
            --i;
        } 
        else 
        {
            // 收集连续 Sync 中间件
            int syncEnd = i;
            while (i >= 0 && entries[i].type == MiddlewareEntry::Type::hSync) --i;
            int syncStart = i + 1;

            // 提取这组 Sync 的 before/after
            std::vector<SyncBeforeHandler> befores;
            std::vector<SyncAfterHandler> afters;
            for (int j = syncStart; j <= syncEnd; ++j) 
            {
                if (entries[j].before) befores.push_back(entries[j].before);
                if (entries[j].after) afters.push_back(entries[j].after);
            }

            // 合并为单个协程 lambda：N 个 Sync → 1 个协程帧！
            current = [befores, afters, next = current](HttpRequest& r) -> Awaitable<HttpResponse> 
            {
                // 前置：按注册顺序执行（外层先执行）
                for (const auto& before : befores) 
                {
                    auto intercepted = before(r);
                    if (intercepted) co_return std::move(*intercepted);
                }

                // 唯一的协程挂起点
                auto res = co_await next(r);

                // 后置：按注册逆序执行（洋葱模型语义）
                for (int k = afters.size() - 1; k >= 0; --k) 
                {
                    afters[k](r, res);
                }

                co_return res;
            };
        }
    }
    return current;
}
```

> 💡 两个关键洞察：
> 1. **Async 条目的 `return mw(r, next)` 不是笔误**——lambda 本身不需要是协程，直接转发 awaitable 给外层的 `co_await`，比 `co_return co_await` 少分配一个协程帧。这就是"转发协程消除"优化。
> 2. 连续 Sync 中间件的 before 按注册顺序执行（外层先），after 按注册逆序执行（后注册的后置先退出）。这就是洋葱模型——进入时穿透到最内层，退出时从最内层逐层返回。

### 2.3 效果对比

```
中间件注册顺序:
  JwtAuth (Sync) → RateLimiter (Sync) → LogMiddleware (Async) → Helmet (Sync)

传统方案:
  协程帧 #1: JwtAuth
  协程帧 #2: RateLimiter
  协程帧 #3: LogMiddleware (有 co_await)
  协程帧 #4: Helmet
  协程帧 #5: finalHandler
  共 5 个协程帧

Hical 优化链:
  合并组 #1: JwtAuth + RateLimiter → 1 个协程帧
  协程帧 #2: LogMiddleware (不可合并，有 co_await)
  合并组 #3: Helmet → 但 Helmet 接着 LogMiddleware 之后...
  
  等等，Helmet 是 Sync 但和 JwtAuth/RateLimiter 不是连续的——
  中间被 LogMiddleware (Async) 隔开了。
  所以 Helmet 单独成一个协程帧。
  
  最终: 合并组 #1 (JwtAuth+RateLimiter, 1 帧)
       + LogMiddleware (1 帧)
       + Helmet (1 帧)
       + finalHandler (1 帧)
  共 4 个协程帧 (省了 1 个)
```

**优化比例取决于 Sync 中间件的连续性。** 如果 JwtAuth + RateLimiter + Helmet 三个 Sync 连在一起注册（没有 Async 中间件隔开），就是 1 个协程帧——省了 2 个。

---

## 3. SyncAfterHandler 为什么禁止修改 body？

你可能会注意到文档中的警告：

```
@warning 禁止修改响应体（body）。因为 Content-Length 已由 setBody/setJsonBody 中的
prepare_payload() 固定，修改 body 会导致 Content-Length 与实际不匹配，
在反向代理后可能引发 HTTP 响应走私或客户端截断。
```

这不是一个随意加的限制。原因是：`Content-Length` 在 body 设置时已经通过 `to_chars` 计算好了。如果 after handler 修改了 body 但没有重新计算 Content-Length，响应头中的长度就和实际 body 长度对不上。

在反向代理（nginx/envoy）后面，这可能导致 HTTP 响应走私漏洞——攻击者利用长度不匹配把一个响应的 body 注入到另一个响应中。

**如果你需要在 after 阶段改 body**，用完整的 Async 中间件（MiddlewareHandler），在 `co_await next(req)` 之后修改响应再 `co_return`。

---

## 4. MiddlewareProfiling：零侵入的计时统计

当用 `HICAL_ENABLE_MIDDLEWARE_PROFILING=ON` 编译时，每个中间件自动记录耗时且不造成 false sharing：

```cpp
struct MiddlewareTimingStats 
{
    // 热数据：每次请求都写入，各自独占 cache line
    alignas(64) std::atomic<size_t> callCount {0};
    alignas(64) std::atomic<int64_t> totalTimeUs {0};
    alignas(64) std::atomic<int64_t> maxTimeUs {0};
    alignas(64) std::atomic<int64_t> minTimeUs {std::numeric_limits<int64_t>::max()};

    // 冷数据：仅查询统计时读取
    std::string name;
};
```

四个原子变量各自独占 64 字节 cache line。如果四个挤在同一条 64 字节中（总共才 32 字节），多核写的时候互相刷 cache line。分开后，4 个核同时更新 4 个中间件的统计不冲突。

> 💡 四个原子变量各自独占 64 字节 cache line。如果四个挤在同一条 64 字节中（总共才 32 字节），多核写的时候互相刷 cache line。分开后，4 个核同时更新 4 个中间件的统计不冲突——这就是 false sharing 的经典解法。

---

## 5. After 的逆序执行——洋葱模型的完整语义

注意 buildOptimizedChain 中的这行代码：

```cpp
// 后置：按注册逆序执行
for (int k = static_cast<int>(afters.size()) - 1; k >= 0; --k) 
{
    afters[k](r, res);
}
```

假设你注册了三个 Sync 中间件：

```cpp
pipeline.use("timing", timingBefore, timingAfter);    // 最先注册
pipeline.use("cors", corsBefore, corsAfter);          // 第二个注册
pipeline.use("helmet", helmetBefore, helmetAfter);    // 最后注册
```

执行顺序是：

```
请求进来
  → timingBefore()      (1)
    → corsBefore()      (2)
      → helmetBefore()  (3)
        → handler()     (核心业务)
      → helmetAfter()   (3 - 最先退出)
    → corsAfter()       (2)
  → timingAfter()       (1 - 最后退出)
响应出去
```

**进入时 outer→inner，退出时 inner→outer。** 这就是"洋葱模型"——和 ASP.NET Core、Express.js 的行为完全一致。

---

## 回顾一下你学会了什么

1. MiddlewareEntry 的 tagged union 区分 Async（一层一帧）和 Sync（零帧）
2. buildOptimizedChain 合并连续 Sync 中间件——N 个 Sync = 1 个协程帧
3. Sync 中间件的 before 按注册顺序执行，after 按注册逆序执行（洋葱语义）
4. SyncAfterHandler 禁止改 body——Content-Length 已固定，改了会引发 HTTP 响应走私
5. MiddlewareProfiling 的 alignas(64) 消除 false sharing——每个原子变量独占一条 cache line
6. 合并不是"无脑的"——被 Async 隔开的 Sync 中间件不能合并

---

> 下一篇：[Vyukov MPSC 无锁队列在 HTTP 服务器上的实战：GenericConnection 的写路径]({{< relref "posts/hical-source-study/04-GenericConnection无锁队列.md" >}})
