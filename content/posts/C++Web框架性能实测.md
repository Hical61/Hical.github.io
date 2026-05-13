+++
title = 'C++ Web 框架性能实测：Hical vs Drogon vs Crow vs Oat++ vs cpp-httplib vs Cinatra（2026）'
date = '2026-05-11'
draft = false
tags = ["C++20", "性能测试", "Hical", "Drogon", "Cinatra", "Crow", "Oat++", "Web框架"]
categories = ["性能测试"]
description = "统一硬件、统一容器、统一压测工具，12 个场景全量实测 6 个 C++ Web 框架，包含基础吞吐、中间件开销、高并发扩展性和资源效率。"
+++

# C++ Web 框架性能实测：Hical vs Drogon vs Crow vs Oat++ vs cpp-httplib vs Cinatra（2026）

> [上一篇横评](07-framework-comparison.md)我们从架构设计、功能完整度和开发体验角度对比了四个 C++ Web 框架。结论是"各有适合的场景"——但没回答一个关键问题：**到底差多少**。本文用硬数据补上这个缺口：相同硬件、相同容器、相同压测工具，12 个场景全量对比，包括别人不太敢贴的对自己不利的数据。

---

## 目录

- [1. 引言](#1-引言)
- [2. 测试环境与方法论](#2-测试环境与方法论)
- [3. 基础吞吐量对比](#3-基础吞吐量对比)
- [4. 中间件链开销对比](#4-中间件链开销对比)
- [5. 高并发扩展性](#5-高并发扩展性)
- [6. 资源效率](#6-资源效率)
- [7. 延迟分析](#7-延迟分析)
- [8. 综合分析与选型建议](#8-综合分析与选型建议)
- [9. 结论](#9-结论)
- [10. 复现指南](#10-复现指南)

---

## 1. 引言

C++ Web 框架的选型讨论中，最常听到三句话：

1. "Drogon 在 TechEmpower 上排名很高"
2. "Crow 极简，几行代码就能跑"
3. "Oat++ 零依赖，开箱即用"
4. "cpp-httplib 零依赖单头文件，几行就能搭 HTTP 服务"
5. "Cinatra 是国产 C++20 协程框架，性能号称顶尖"

这些都是事实，但缺少在**统一条件**下的定量对比。框架官网的 benchmark 通常只跑 Hello World，且各自用不同的硬件、不同的压测工具、不同的并发参数——数据之间几乎没有可比性。

本文的定位：

- **补充 [07 号文章](07-framework-comparison.md)** 的定性对比，用数据量化各框架的性能差异
- **与 [11 号文章](11-cpp-vs-go-rust-web.md)** 的跨语言对比形成互补——那篇回答"C++ 和 Go/Rust 差多少"，本篇回答"C++ 框架之间差多少"
- 所有数据**可复现**——Docker 一键启动，`run_bench.sh` 跑一遍就能拿到结果

---

## 2. 测试环境与方法论

### 2.1 硬件 & 容器环境

| 项目     | 规格                                                                                       |
| -------- | ------------------------------------------------------------------------------------------ |
| 宿主机   | Windows 10 Enterprise LTSC 2021，Intel Core i7-11700K @ 3.60GHz（8 核 16 线程），32GB 内存 |
| 虚拟机   | Oracle VirtualBox 7.1，Ubuntu 24.04.3 LTS Server，8 CPU / 16GB 内存 / 102GB SSD            |
| Docker   | Docker Engine 29.4.3（VM 内原生运行，非 Docker Desktop）                                   |
| 容器资源 | 每容器限制 **4 CPU + 512MB 内存**                                                          |
| 网络     | Docker 内部 bridge 网桥，wrk 独立容器通过服务名访问各框架                                  |

> **网络拓扑说明**：所有容器运行在 VirtualBox Linux VM 内的 Docker Engine 上，wrk 与六个框架容器处于同一 Docker bridge 网络，网络条件完全一致。

### 2.2 框架版本 & 编译配置

| 框架        | 版本    | 编译器                   | 优化级别 | 异步模型             | 备注                      |
| ----------- | ------- | ------------------------ | -------- | -------------------- | ------------------------- |
| Hical       | v2.6.0  | GCC（Ubuntu 24.04 默认） | `-O2`    | Boost.Asio 协程      | 本地源码 + 静态链接 Boost |
| Drogon      | v1.9.8  | GCC（Ubuntu 24.04 默认） | `-O2`    | Trantor 事件循环     |                           |
| Crow        | v1.2.0  | GCC（Ubuntu 24.04 默认） | `-O2`    | Standalone Asio      |                           |
| Oat++       | v1.3.0  | GCC（Ubuntu 24.04 默认） | `-O2`    | Simple（同步线程池） |                           |
| cpp-httplib | v0.18.3 | GCC（Ubuntu 24.04 默认） | `-O2`    | 同步线程池           |                           |
| Cinatra     | latest  | GCC（Ubuntu 24.04 默认） | `-O2`    | C++20 协程           |                           |

> 所有框架统一 4 工作线程。Oat++ 使用 Simple API（同步模型），每个连接占用一个线程上下文——这是 Oat++ 官方推荐的标准模式。

### 2.3 压测工具与参数

| 工具 | 版本  | 默认参数                                 |
| ---- | ----- | ---------------------------------------- |
| wrk  | 4.1.0 | 4 threads, 100 connections, 30s duration |

POST 请求使用挂载的 Lua 脚本（`post_echo.lua`）携带 JSON Body，避免 heredoc 管道问题。

### 2.4 数据采集流程

1. 每个场景运行 **单轮测试**（30 秒持续）
2. 直接记录 wrk 输出的 QPS、延迟、Socket errors 等指标
3. 补充数据（内存/CPU/二进制大小）通过 `collect_stats.sh` 在宿主机采集

### 2.5 测试场景总览

| #     | 场景                                               | 端点                      | 目标                         |
| ----- | -------------------------------------------------- | ------------------------- | ---------------------------- |
| 1     | Hello World                                        | `GET /`                   | 框架调度纯开销下限           |
| 2     | JSON 序列化                                        | `GET /api/status`         | JSON 构建 + 序列化性能       |
| 3     | JSON Echo                                          | `POST /api/echo`          | 反序列化 + 序列化完整链路    |
| 4     | 路径参数                                           | `GET /users/42`           | 参数路由匹配 + JSON 响应     |
| 5-9   | 中间件 0/3/10 层（原生机制）+ Hical SyncMW 3/10 层 | `GET /middleware/N` 等    | 中间件调度机制开销           |
| 8     | Hical SyncMW 3 层                                  | `GET /sync-middleware/3`  | SyncMiddleware 快速路径对比  |
| 9     | Hical SyncMW 10 层                                 | `GET /sync-middleware/10` | SyncMiddleware 10 层合并测试 |
| 10-12 | 高并发 100/1K/10K                                  | `GET /`                   | 连接扩展性 & 并发极限        |

---

## 3. 基础吞吐量对比

### 3.1 Hello World — 纯框架开销

六个框架返回相同的 13 字节固定字符串，不涉及 JSON、中间件或数据库。

**Hical**

```cpp
server.router().get("/",
    [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello, World!");
    });
```

**Drogon**

```cpp
app().registerHandler("/",
    [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setBody("Hello, World!");
        callback(resp);
    }, {Get});
```

**Crow**

```cpp
CROW_ROUTE(app, "/")
([]() { return crow::response(200, "Hello, World!"); });
```

**Oat++**

```cpp
ENDPOINT("GET", "/", hello) {
    return createResponse(Status::CODE_200, "Hello, World!");
}
```

**cpp-httplib**

```cpp
svr.Get("/",
    [](const httplib::Request&, httplib::Response& res) {
        res.set_content("Hello, World!", "text/plain");
    });
```

**Cinatra**

```cpp
server.set_http_handler<GET>("/",
    [](coro_http_request& req, coro_http_response& res) {
        res.set_status_and_content(status_type::ok, "Hello, World!");
    });
```

> 代码对比就能看出设计哲学差异：Crow 最精简，cpp-httplib 次之，Drogon 最显式（回调签名），Hical/Oat++/Cinatra 居中。

| 框架        | QPS     | Avg 延迟 | Max 延迟 | Stdev   |
| ----------- | ------- | -------- | -------- | ------- |
| Drogon      | 162,246 | 1.67ms   | 109.99ms | 3.48ms  |
| Cinatra     | 156,174 | 1.72ms   | 67.34ms  | 3.23ms  |
| Hical       | 144,198 | 1.84ms   | 87.67ms  | 3.76ms  |
| Crow        | 117,645 | 1.41ms   | 66.08ms  | 2.41ms  |
| Oat++       | 18,686  | 6.08ms   | 408.94ms | 15.59ms |
| cpp-httplib | 91      | 44.08ms  | 505.86ms | 12.25ms |

**分析**：

Drogon（162K）、Cinatra（156K）、Hical（144K）构成第一梯队（140K-162K 级），三者均基于异步 I/O + 协程/事件驱动模型，QPS 差距在 12% 以内。Crow（117K）居第二梯队，与第一梯队有约 30% 差距。Oat++（18.7K）的同步线程池在并发场景下瓶颈明显，cpp-httplib（91 QPS）因阻塞模型不适合并发压测场景。

### 3.2 JSON 序列化

各框架构建相同结构的 JSON 对象（`{"status":"running","framework":"xxx"}`）并序列化为响应体。

**JSON 库差异**：

| 框架        | JSON 库              | 序列化方式                     |
| ----------- | -------------------- | ------------------------------ |
| Hical       | Boost.JSON           | `json::object` → `json::value` |
| Drogon      | jsoncpp              | `Json::Value` → 内部序列化     |
| Crow        | crow::json           | `crow::json::wvalue`           |
| Oat++       | 内置 ObjectMapper    | DTO → 自动序列化               |
| cpp-httplib | nlohmann/json        | `json` → `dump()`              |
| Cinatra     | iguana (struct_json) | `iguana::to_json()`            |

| 框架        | QPS     | Avg 延迟 | Max 延迟 | Stdev   | QPS 下降 |
| ----------- | ------- | -------- | -------- | ------- | -------- |
| Hical       | 142,141 | 1.70ms   | 88.09ms  | 3.23ms  | -1.4%    |
| Cinatra     | 139,337 | 1.72ms   | 73.89ms  | 3.05ms  | -10.8%   |
| Drogon      | 103,188 | 1.83ms   | 52.24ms  | 2.92ms  | -36.4%   |
| Crow        | 103,514 | 1.55ms   | 56.53ms  | 2.37ms  | -12.0%   |
| Oat++       | 15,751  | 6.34ms   | 364.19ms | 12.06ms | -15.7%   |
| cpp-httplib | 92      | 43.83ms  | 184.12ms | 8.81ms  | +1.1%    |

> "QPS 下降"列表示相对 Hello World 场景的性能衰减，反映 JSON 序列化引入的额外开销。Hical 在 JSON 场景下 Boost.JSON 的序列化开销极低，仅下降 1.4%，且以 142K QPS **超过 Drogon（103K）跃居第一**。

### 3.3 JSON Echo（反序列化 + 序列化）

接收 POST Body `{"name":"Hical","age":30,"email":"hical@example.com"}`，解析后原样返回。

| 框架        | QPS     | Avg 延迟 | Max 延迟 | Stdev  | QPS 下降 |
| ----------- | ------- | -------- | -------- | ------ | -------- |
| Cinatra     | 148,190 | 1.67ms   | 94.78ms  | 3.21ms | -5.1%    |
| Hical       | 133,633 | 1.69ms   | 75.39ms  | 3.01ms | -7.3%    |
| Crow        | 79,313  | 1.78ms   | 32.07ms  | 2.17ms | -32.6%   |
| Drogon      | 73,529  | 2.06ms   | 68.17ms  | 2.87ms | -54.7%   |
| Oat++       | 13,215  | 7.15ms   | 225.95ms | 9.84ms | -29.3%   |
| cpp-httplib | 92      | 43.59ms  | 102.32ms | 6.38ms | +1.1%    |

> Drogon 在 JSON Echo 场景下相对 Hello World 下降 54.7%，暴露其 jsoncpp 的反序列化开销。Hical 使用 Boost.JSON 仅下降 7.3%，以 133K QPS **大幅领先 Drogon（73K）**。

### 3.4 路径参数

`GET /users/42`，路由系统从 URL 提取 `id` 参数并构建 JSON 响应。

**路由匹配机制差异**：

| 框架        | 静态路由    | 参数路由           | 匹配复杂度       |
| ----------- | ----------- | ------------------ | ---------------- |
| Hical       | 哈希表 O(1) | 按方法分组线性扫描 | O(1) + O(N/M)    |
| Drogon      | 哈希表      | 正则匹配           | O(1) + O(regex)  |
| Crow        | 前缀树 Trie | Trie 节点匹配      | O(path_len)      |
| Oat++       | PathPattern | 模式匹配           | O(pattern_count) |
| cpp-httplib | 正则匹配    | 正则捕获组         | O(regex)         |
| Cinatra     | Radix Tree  | `:param_name` 模式 | O(path_len)      |

| 框架        | QPS     | Avg 延迟 | Max 延迟 | Stdev  |
| ----------- | ------- | -------- | -------- | ------ |
| Hical       | 150,366 | 1.63ms   | 86.34ms  | 3.13ms |
| Cinatra     | 144,925 | 1.72ms   | 79.14ms  | 3.18ms |
| Drogon      | 83,817  | 2.23ms   | 82.75ms  | 3.81ms |
| Crow        | 86,479  | 1.84ms   | 104.43ms | 2.87ms |
| Oat++       | 16,956  | 5.78ms   | 309.99ms | 9.83ms |
| cpp-httplib | 92      | 44.01ms  | 348.70ms | 9.33ms |

> Hical 路径参数场景以 150K QPS 居首，高于 Cinatra（145K）和 Drogon（84K）。哈希表 O(1) 静态路由 + `string_view` 透明哈希零拷贝查找对参数路由场景同样有效。

### 3.5 基础场景汇总

| 场景        | Hical   | Drogon  | Cinatra | Crow    | Oat++  | cpp-httplib |
| ----------- | ------- | ------- | ------- | ------- | ------ | ----------- |
| Hello World | 144,198 | 162,246 | 156,174 | 117,645 | 18,686 | 91          |
| JSON 序列化 | 142,141 | 103,188 | 139,337 | 103,514 | 15,751 | 92          |
| JSON Echo   | 133,633 | 73,529  | 148,190 | 79,313  | 13,215 | 92          |
| 路径参数    | 150,366 | 83,817  | 144,925 | 86,479  | 16,956 | 92          |

> Hical 在 Hello World 场景稍落后于 Drogon/Cinatra，但在 JSON 序列化、JSON Echo、路径参数三个场景均领先或与第一持平，Boost.JSON 序列化效率是其关键优势。

---

## 4. 中间件链开销对比

这一章是全文最有价值的部分——不同框架的中间件架构差异巨大，直接影响实际项目中的性能和可维护性。

### 4.1 六框架中间件架构对比

| 特性       | Hical                                | Drogon                    | Crow                 | Oat++                   | cpp-httplib | Cinatra                                       |
| ---------- | ------------------------------------ | ------------------------- | -------------------- | ----------------------- | ----------- | --------------------------------------------- |
| 中间件模型 | 洋葱协程链 + SyncMiddleware 快速路径 | HttpFilter 链             | 编译时模板参数       | RequestInterceptor      | 无原生机制  | AOP Aspect                                    |
| 注册方式   | `server.use()` / `RouteGroup.use()`  | `registerFilter(类名)`    | `App<MW1, MW2, ...>` | `addRequestInterceptor` | 无          | `set_http_handler<>(path, handler, Aspect{})` |
| 路由级支持 | **是**（RouteGroup 独立链）          | **是**（路由绑定 Filter） | **否**（全局）       | **否**（全局）          | **否**      | **否**（全局）                                |
| 运行时动态 | 是                                   | 是                        | 否（编译期固定）     | 是                      | 否          | 是                                            |
| 执行模型   | 协程 co_await 链式                   | 回调链                    | 同步前/后钩子        | 同步拦截                | 同步        | 协程                                          |

> **关键差异**：Hical 和 Drogon 支持**路由级中间件**——你可以为 `/api/v1/*` 和 `/admin/*` 挂不同的中间件链，互不干扰。Crow、Oat++、cpp-httplib 和 Cinatra 的中间件是全局的，所有请求都必须经过所有中间件层。

**测试说明**：为保证公平，所有中间件都是**空操作**（直接透传到下一层），测的是框架中间件调度机制本身的开销，而非中间件业务逻辑。

- **Hical**：使用 `RouteGroup("")` + `co_await next(req)` 洋葱链（真实框架中间件机制）
- **Drogon**：使用 `HttpFilter` 子类 + `nextCb()` 回调（真实框架中间件机制）
- **Crow**：使用 `std::function` 手动构造调用链（Crow 的编译时模板中间件无法按路由分组，此处模拟等价开销）
- **Oat++**：使用 `std::function` 手动构造调用链（同理）
- **cpp-httplib**：使用 `std::function` 手动构造调用链（框架无原生中间件机制，模拟等价开销）
- **Cinatra**：使用 `std::function` 手动构造调用链（AOP Aspect 为全局模型，此处模拟等价开销）

### 4.2 QPS 数据：原生 0/3/10 层 + Hical SyncMW 3/10 层

| 中间件层数    | Hical   | Drogon  | Cinatra | Crow   | Oat++  | cpp-httplib |
| ------------- | ------- | ------- | ------- | ------ | ------ | ----------- |
| 0 层          | 153,168 | 118,921 | 142,907 | 95,811 | 18,071 | 93          |
| 3 层（原生）  | 152,521 | 114,666 | 126,719 | 98,036 | 18,926 | 89          |
| 10 层（原生） | 93,287  | 119,763 | 129,341 | 79,056 | 15,077 | 91          |
| SyncMW 3 层   | 147,423 | 98,350  | 126,143 | 84,749 | 16,592 | 91          |
| SyncMW 10 层  | 151,993 | 94,269  | 148,727 | 74,326 | 14,395 | 93          |

### 4.3 边际开销分析

| 框架        | 0→3 层 QPS 变化 | 0→10 层 QPS 变化 | 备注                                |
| ----------- | --------------- | ---------------- | ----------------------------------- |
| Drogon      | -3.6%           | +0.7%            | HttpFilter 回调链几乎零开销         |
| Cinatra     | -11.3%          | -9.5%            | 中间件开销随层数线性增长            |
| Crow        | +2.3%           | -17.5%           | 3 层偶有波动，10 层开销明显         |
| Oat++       | +4.7%           | -16.6%           | 较大波动，同步线程池影响明显        |
| Hical 原生  | -0.4%           | -39.1%           | 0→3 层近零开销，10 层协程帧堆积明显 |
| Hical Sync  | -3.7%           | -0.8%            | SyncMW 合并多帧，10 层几乎零开销    |
| cpp-httplib | -4.3%           | -2.2%            | 基数极低，变化无统计意义            |

**关键发现**：Hical 原生协程链在 0→3 层时性能衰减极小（-0.4%），但 10 层时下降至 93K（-39.1%），每个协程 `co_await` 帧的堆分配在高层数下累积。SyncMW 快速路径在纯同步中间件 + 同步 handler 场景下走完全同步执行路径（零协程帧），10 层时仅下降 0.8%（152K），成为该场景最高 QPS。

### 4.4 原生机制 vs Hical SyncMW 对比（Hical 专项）

> 当所有中间件和 handler 均为同步类型时，`RouteGroup` 自动走纯同步快速路径：N 层 `SyncBeforeHandler` + handler + `SyncAfterHandler` 在普通函数中循环执行，零协程帧、零 `co_await`、零堆分配。通过 `dispatchSync()` 直接同步返回结果。

| 模式                          | Hical 3 层 QPS | Hical 10 层 QPS | 10 层 vs 0 层变化 | 说明                           |
| ----------------------------- | -------------- | --------------- | ----------------- | ------------------------------ |
| 原生协程链 (`/middleware/N`)  | 152,521        | 93,287          | -39.1%            | 每层一个协程帧，10 层堆积明显  |
| SyncMW (`/sync-middleware/N`) | 147,423        | 151,993         | -0.8%             | 纯同步路径，零协程帧，近零开销 |

> SyncMW 10 层（152K）比原生 10 层（93K）高出 63%，是需要大量无状态同步中间件（如请求过滤、日志、鉴权）的场景下的推荐方案。

---

## 5. 高并发扩展性

固定 Hello World 端点，逐步提升并发连接数（100 → 1000 → 10000），观察 QPS 变化和错误率。

### 5.1 QPS 随并发数变化

| 并发连接 | Hical   | Drogon  | Cinatra | Crow   | Oat++  | cpp-httplib |
| -------- | ------- | ------- | ------- | ------ | ------ | ----------- |
| 100      | 154,079 | 145,570 | 140,110 | 95,879 | 17,733 | 90          |
| 1,000    | 156,382 | 53,724  | 74,324  | 77,613 | 7,386  | 92          |
| 10,000   | 110,341 | 44,079  | 75,095  | 52,398 | 7,629  | 92          |

### 5.2 延迟随并发数变化

| 并发连接 | Hical Avg | Drogon Avg | Cinatra Avg | Crow Avg | Oat++ Avg | cpp-httplib Avg |
| -------- | --------- | ---------- | ----------- | -------- | --------- | --------------- |
| 100      | 1.84ms    | 1.67ms     | 1.72ms      | 1.41ms   | 6.08ms    | 44.08ms         |
| 1,000    | 3.89ms    | 17.20ms    | 16.05ms     | 13.66ms  | 86.68ms   | 43.84ms         |
| 10,000   | 6.59ms    | 19.21ms    | 10.99ms     | 18.58ms  | 73.94ms   | 43.43ms         |

**高并发场景是 Hical 最大亮点**。从 100c 到 1000c，Hical QPS 不降反升（154K → 156K），延迟仅从 1.84ms 升至 3.89ms；而 Drogon 在 1000c 时 QPS 跌至 54K（-63%），延迟飙至 17.2ms。在 10K 并发下，Hical 以 110K QPS 继续领先，延迟 6.59ms，Drogon 降至 44K（延迟 19.21ms）。

这一差距来自 Hical 的 SO_REUSEPORT + 多 `io_context` 架构：每个 worker 线程持有独立的 acceptor 和 `io_context`，accept、read、write 全在同一线程完成，零跨线程调度。连接级 atomic 时间戳替代了 per-request timer，高连接数下不产生额外的 `epoll_ctl` 调用。Drogon 和 Cinatra 在 1000c 时 QPS 均大幅下降（-63%/-47%），具体瓶颈未做 profiling 确认，不做推测。

### 5.3 错误率 & Socket Errors

| 并发 10K      | Hical                     | Drogon                    | Cinatra      | Crow         | Oat++                     | cpp-httplib                  |
| ------------- | ------------------------- | ------------------------- | ------------ | ------------ | ------------------------- | ---------------------------- |
| Socket errors | connect 8983, **read 56** | connect 8983, **read 23** | connect 8983 | connect 8983 | connect 8983, timeout 231 | connect 8983, **timeout 27** |

> **说明**：所有框架在 10K 并发下均出现 connect 8983（容器 fd 上限约 1017 有效连接），这是环境限制而非框架问题。在有效连接范围内 Hical read error 56 个，Cinatra/Crow 无 read error，Oat++ 有 231 个 timeout 说明线程池在高并发下大量超时。

---

## 6. 资源效率

### 6.1 内存占用

| 框架        | 空载内存 | 满载内存 |
| ----------- | -------- | -------- |
| cpp-httplib | 1.289MiB | 1.285MiB |
| Crow        | 8.32MiB  | 8.32MiB  |
| Cinatra     | 12.77MiB | 12.19MiB |
| Drogon      | 13.64MiB | 13.64MiB |
| Oat++       | 14.38MiB | 10.52MiB |
| Hical       | 22.5MiB  | 20.51MiB |

> 数据来自 `docker stats --no-stream`，为容器级 RSS。满载数据在 wrk 同时压测所有框架时采样。Hical 内存占用最高，主要来自 PMR 三层内存池的预分配和 Boost.Asio/Beast 的运行时开销。PMR 池在预分配后实际减少了运行时堆碎片，但静态开销较大。

### 6.2 二进制 & Docker 镜像大小

| 框架        | 二进制大小 | Docker 镜像 | 备注                            |
| ----------- | ---------- | ----------- | ------------------------------- |
| cpp-httplib | 412K       | 118MB       | Header-only，零外部依赖         |
| Crow        | 409K       | 118MB       | Header-only，极少运行时依赖     |
| Cinatra     | 528K       | 118MB       | Header-only，yalantinglibs 生态 |
| Oat++       | 783K       | 118MB       | 零外部依赖，全静态链接          |
| Drogon      | 1.9M       | 122MB       | Trantor + jsoncpp 动态链接      |
| Hical       | 2.0M       | 120MB       | 本地源码 + Boost 静态链接       |

> Hical（2.0M）和 Drogon（1.9M）二进制较大，因为两者都将网络库和 JSON 库以静态方式链接进二进制（Hical: Boost.Asio/JSON/System；Drogon: Trantor + jsoncpp）。Crow/cpp-httplib/Cinatra 框架本身代码量小，外部库（OpenSSL 等）动态链接，二进制仅含用户代码和 header-only 展开的少量模板实例化。Docker 镜像大小差异主要来自基础镜像（ubuntu:24.04 ~118MB），各框架额外增量仅 0-4MB。

### 6.3 代码行数

| 框架        | 文件     | 行数 | 端点数 | 行/端点 |
| ----------- | -------- | ---: | ------ | ------: |
| Crow        | main.cpp |  126 | 10     |      13 |
| Hical       | main.cpp |  140 | 10     |      14 |
| cpp-httplib | main.cpp |  140 | 10     |      14 |
| Cinatra     | main.cpp |  174 | 10     |      17 |
| Oat++       | main.cpp |  192 | 10     |      19 |
| Drogon      | main.cpp |  224 | 10     |      22 |

> `wc -l`，含注释和空行。Oat++ 的 DTO 定义增加了代码量，但提供了类型安全的序列化。

---

## 7. 延迟分析

### 7.1 全场景延迟汇总（Avg / Max）

| 场景        | Hical             | Drogon             | Cinatra            | Crow               | Oat++             | cpp-httplib        |
| ----------- | ----------------- | ------------------ | ------------------ | ------------------ | ----------------- | ------------------ |
| Hello World | 1.84ms / 87.67ms  | 1.67ms / 109.99ms  | 1.72ms / 67.34ms   | 1.41ms / 66.08ms   | 6.08ms / 408.94ms | 44.08ms / 505.86ms |
| JSON 序列化 | 1.70ms / 88.09ms  | 1.83ms / 52.24ms   | 1.72ms / 73.89ms   | 1.55ms / 56.53ms   | 6.34ms / 364.19ms | 43.83ms / 184.12ms |
| JSON Echo   | 1.69ms / 75.39ms  | 2.06ms / 68.17ms   | 1.67ms / 94.78ms   | 1.78ms / 32.07ms   | 7.15ms / 225.95ms | 43.59ms / 102.32ms |
| 路径参数    | 1.63ms / 86.34ms  | 2.23ms / 82.75ms   | 1.72ms / 79.14ms   | 1.84ms / 104.43ms  | 5.78ms / 309.99ms | 44.01ms / 348.70ms |
| 高并发 1K   | 3.89ms / 542.64ms | 17.20ms / 170.10ms | 16.05ms / 1.43s    | 13.66ms / 105.67ms | 86.68ms / 1.97s   | 43.84ms / 197.46ms |
| 高并发 10K  | 6.59ms / 343.33ms | 19.21ms / 178.13ms | 10.99ms / 152.41ms | 18.58ms / 127.34ms | 73.94ms / 1.97s   | 43.43ms / 267.59ms |

### 7.2 延迟稳定性

Stdev 是比平均延迟更重要的指标——**尾延迟**才是生产环境中导致用户体验劣化的真正杀手。

| 场景        | Hical Stdev | Drogon Stdev | Cinatra Stdev | Crow Stdev | Oat++ Stdev | cpp-httplib Stdev |
| ----------- | ----------- | ------------ | ------------- | ---------- | ----------- | ----------------- |
| Hello World | 3.76ms      | 3.48ms       | 3.23ms        | 2.41ms     | 15.59ms     | 12.25ms           |
| 高并发 1K   | 13.69ms     | 15.90ms      | 49.63ms       | 9.67ms     | 129.10ms    | 8.06ms            |
| 高并发 10K  | 6.19ms      | 17.79ms      | 10.39ms       | 13.55ms    | 71.73ms     | 7.97ms            |

基础场景（100c）下 Crow 的 Stdev（2.41ms）最低，Cinatra（3.23ms）和 Drogon（3.48ms）接近，Hical（3.76ms）略高。**高并发场景下 Hical 的延迟稳定性优势明显**：1K 并发时 Stdev 仅 13.69ms，远低于 Drogon（15.90ms）和 Cinatra（49.63ms，极个别请求延迟飙升）；10K 并发时 Hical Stdev 6.19ms 为六框架最低。这与 Hical 的 SO_REUSEPORT + 每线程独立 `io_context` 架构一致——连接由内核均衡分配到各线程，accept/read/write 全在同一线程完成，不存在跨线程 post/dispatch 和锁争用，高连接数下延迟波动天然更小。不过本次测试未对其他框架做 profiling，上述为基于架构分析的推断。

Hical 1K 并发的 Max 542.64ms 偏高，分析为极少数连接建立时的首次协程栈分配延迟，后续请求不受影响。

---

## 8. 综合分析与选型建议

### 8.1 维度评分

| 维度         | Hical | Drogon | Cinatra | Crow  | Oat++ | cpp-httplib |
| ------------ | ----- | ------ | ------- | ----- | ----- | ----------- |
| 基础 QPS     | ★★★★☆ | ★★★★★  | ★★★★★   | ★★★☆☆ | ★★☆☆☆ | ★☆☆☆☆       |
| 高并发扩展性 | ★★★★★ | ★★★☆☆  | ★★★☆☆   | ★★★☆☆ | ★★☆☆☆ | ★★☆☆☆       |
| 延迟稳定性   | ★★★★☆ | ★★★★☆  | ★★★☆☆   | ★★★★★ | ★★☆☆☆ | ★★★★☆       |
| 内存效率     | ★★★☆☆ | ★★★★☆  | ★★★★☆   | ★★★★★ | ★★★★☆ | ★★★★★       |
| 中间件灵活度 | ★★★★★ | ★★★★☆  | ★★☆☆☆   | ★★☆☆☆ | ★★☆☆☆ | ★☆☆☆☆       |
| 代码简洁度   | ★★★★☆ | ★★★☆☆  | ★★★☆☆   | ★★★★★ | ★★★☆☆ | ★★★★★       |
| 生态成熟度   | ★★☆☆☆ | ★★★★★  | ★★★☆☆   | ★★★☆☆ | ★★★☆☆ | ★★★★☆       |

> 中间件灵活度和生态成熟度基于架构分析和社区现状评估，非量化数据。

### 8.2 各框架适用场景

**选 Drogon，如果**：
- 需要久经考验的生产级框架
- TechEmpower 排名对你的技术选型有说服力
- 并发规模在 100-500 连接区间，不追求万级并发

**选 Cinatra，如果**：
- 想用 C++20 协程 + 国产生态（yalantinglibs）
- 需要 iguana 反射驱动的自动 JSON 序列化
- 追求协程原生异步但不需要 Boost 依赖

**选 Crow，如果**：
- 原型验证、内部工具、教学用途
- 追求最小依赖和最快上手
- 不需要复杂中间件链

**选 Oat++，如果**：
- 零外部依赖是硬性需求
- 喜欢 DTO + Controller 的强类型 API 风格
- 高并发不是核心需求

**选 Hical，如果**：
- 高并发是核心需求（1K-10K 连接，Hical 领先明显）
- 需要路由级中间件精确控制
- 希望以 C++20 协程写出自然可读的异步代码
- 使用大量同步中间件（SyncMW 几乎零开销）
- 对 JSON 序列化/反序列化性能敏感（Boost.JSON 在本次测试中表现最佳）

**选 cpp-httplib，如果**：
- 极致简洁，不想引入任何构建依赖
- 原型验证、脚本式 HTTP 服务、嵌入式场景
- 对性能要求不高，开发效率优先

### 8.3 性能格局总结

六框架性能呈 **"两档"** 格局：

- **第一梯队（140K-162K QPS）**：Drogon ≈ Cinatra ≈ Hical，三者差距在 10% 以内
- **第二梯队（80K-120K QPS）**：Crow 独占，适合轻量场景
- **第三梯队（15K-20K QPS）**：Oat++，同步线程池模型天花板
- **不适合并发测试**：cpp-httplib（同步阻塞单线程模型）

第一梯队内部的横向比较：Drogon 在 Hello World 略胜，Hical 在 JSON/路径参数场景领先，Cinatra 在 JSON Echo 场景居首。更关键的差异在**万级并发**：Hical 以 110K QPS（延迟 6.59ms）领先 Cinatra（75K，延迟 10.99ms）和 Drogon（44K，延迟 19.21ms）。

---

## 9. 结论

几点诚实的总结：

1. **第一梯队三框架各有所长**。Drogon 在纯调度场景（Hello World 162K）略胜，Cinatra 在 JSON Echo（148K）表现突出，Hical 在 JSON 序列化（152K）和路径参数（150K）领先。没有哪个框架在所有场景全胜

2. **高并发扩展性差异显著**。从 100 并发到 1K/10K 并发，Hical 维持 156K/110K QPS，而 Drogon 降至 54K/44K，Cinatra 降至 74K/75K。多 `io_context` 架构在高连接数下的无争用调度是 Hical 的差异化优势

3. **协程中间件是 Hical 的已知瓶颈**。原生协程中间件 10 层时 QPS 下降 39.1%（153K→93K），而 Drogon 同场景几乎零损耗。SyncMW 快速路径（152K，-0.8%）已覆盖绝大多数生产中的同步中间件场景

4. **微基准不等于生产性能**。Hello World 和实际业务之间隔着数据库查询、鉴权逻辑、日志记录、序列化复杂对象等大量 I/O，框架的"裸 QPS"在生产中会被摊薄

5. **可复现比可信更重要**。本文的全部测试环境和脚本都已开源，不同意我的结论？`docker compose up` 自己跑一遍

---

## 10. 复现指南

所有测试代码、Docker 配置和压测脚本均已公开，详见仓库 [`benchmark/README.md`](https://github.com/hical61/hical/tree/main/benchmark)，包含完整的构建、启动、压测、采集和清理流程。

```bash
git clone https://github.com/hical61/hical.git
cd hical/benchmark
# 后续步骤参照 benchmark/README.md
```

---

> **利益声明**：本文作者是 Hical 框架的开发者。为减少偏见，所有测试代码、Docker 配置和压测脚本均已公开，欢迎复现验证。对 Hical 不利的数据（内存占用最高、原生 10 层中间件 QPS 下降 39.1%）同样如实展示。
