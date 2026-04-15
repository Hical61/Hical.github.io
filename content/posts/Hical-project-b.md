+++
title = '综合项目B：动手扩展新功能'
date = '2026-04-15'
draft = false
tags = ["C++", "限流", "JSON-RPC", "CRUD", "中间件", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "将 Hical 框架所学融会贯通，逐一分析 Rate Limiter、静态文件服务、JSON-RPC、反射 CRUD 四个扩展题目的设计与实现。"
+++

# 综合项目B：动手扩展新功能 - 学习笔记

> 将前8课所学融会贯通，逐一分析三个扩展题目的设计思路、核心算法和完整实现方案。

---

## 题目1：Rate Limiter 中间件

### 1.1 需求分析

| 要求     | 说明                                  |
| -------- | ------------------------------------- |
| 算法     | 令牌桶（Token Bucket）或滑动窗口      |
| 粒度     | 按客户端 IP 限流                      |
| 超限行为 | 返回 `429 Too Many Requests`          |
| 配置项   | 每秒请求数（rate）、突发容量（burst） |
| 测试     | 编写单元测试验证限流逻辑              |

### 1.2 算法选型：令牌桶 vs 滑动窗口

| 算法         | 原理                                                | 优点                   | 缺点                           |
| ------------ | --------------------------------------------------- | ---------------------- | ------------------------------ |
| **令牌桶**   | 桶以固定速率填充令牌，每次请求消耗 1 个，桶空则拒绝 | 允许突发流量、实现简单 | 需要定时补充令牌（或惰性计算） |
| **滑动窗口** | 记录最近 N 秒内的请求数，超过阈值拒绝               | 精确统计               | 需要维护时间戳列表，内存开销大 |

**推荐：令牌桶（惰性计算版）**——不需要定时器，每次请求时根据时间差计算应补充的令牌数。

### 1.3 核心数据结构

```cpp
// src/core/RateLimiter.h

#pragma once
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hical
{

/**
 * @brief 令牌桶限流器配置
 */
struct RateLimitConfig
{
    double rate {10.0};    // 每秒补充的令牌数（即允许的 QPS）
    double burst {20.0};   // 桶容量（允许突发的最大请求数）
};

/**
 * @brief 单个 IP 的令牌桶
 */
struct TokenBucket
{
    double tokens;                                     // 当前令牌数
    std::chrono::steady_clock::time_point lastRefill;  // 上次补充时间
};

/**
 * @brief 按 IP 限流的令牌桶管理器
 *
 * 惰性补充：不用定时器，每次请求时按时间差计算应补充的令牌。
 * 线程安全：内部加锁保护 IP→桶的映射表。
 */
class RateLimiter
{
public:
    explicit RateLimiter(const RateLimitConfig& config = {});

    /**
     * @brief 尝试消耗一个令牌
     * @param ip 客户端 IP
     * @return true 如果允许通过，false 如果被限流
     */
    bool tryAcquire(const std::string& ip);

    /**
     * @brief 获取配置
     */
    const RateLimitConfig& config() const { return config_; }

private:
    RateLimitConfig config_;
    std::unordered_map<std::string, TokenBucket> buckets_;
    std::mutex mutex_;
};

} // namespace hical
```

### 1.4 核心算法：惰性令牌补充

```cpp
// src/core/RateLimiter.cpp

bool RateLimiter::tryAcquire(const std::string& ip)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto& bucket = buckets_[ip];

    // 首次访问：初始化为满桶
    if (bucket.lastRefill == std::chrono::steady_clock::time_point{})
    {
        bucket.tokens = config_.burst;
        bucket.lastRefill = now;
    }

    // 惰性补充：根据时间差计算应补充的令牌
    double elapsed = std::chrono::duration<double>(now - bucket.lastRefill).count();
    bucket.tokens = std::min(config_.burst, bucket.tokens + elapsed * config_.rate);
    bucket.lastRefill = now;

    // 尝试消耗
    if (bucket.tokens >= 1.0)
    {
        bucket.tokens -= 1.0;
        return true;   // 允许
    }

    return false;      // 拒绝
}
```

**惰性补充的优势**：
- 不需要定时器线程
- 不需要后台任务
- 每次 `tryAcquire` 的时间复杂度 O(1)（哈希查找 + 简单计算）

### 1.5 集成为中间件

```cpp
/**
 * @brief 创建 Rate Limiter 中间件
 *
 * 用法：
 *   auto limiter = std::make_shared<RateLimiter>(RateLimitConfig{100.0, 200.0});
 *   server.use(makeRateLimitMiddleware(limiter));
 */
inline MiddlewareHandler makeRateLimitMiddleware(std::shared_ptr<RateLimiter> limiter)
{
    return [limiter](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
    {
        // 从请求中提取客户端 IP
        // 注意：实际中可能需要检查 X-Forwarded-For 头（反向代理场景）
        auto ip = req.header("X-Real-IP");
        if (ip.empty())
        {
            ip = "unknown";  // 回退（实际应从 socket 获取 peerAddr）
        }

        if (!limiter->tryAcquire(ip))
        {
            HttpResponse res;
            res.setStatus(HttpStatusCode::hTooManyRequests);
            res.setBody("Rate limit exceeded");
            res.setHeader("Retry-After", "1");
            co_return res;
        }

        co_return co_await next(req);
    };
}
```

**与框架的集成点**：
- 实现为 `MiddlewareHandler`，通过 `server.use()` 注册
- 利用洋葱模型的前置逻辑做限流检查
- 被拒绝时不调用 `next`（拦截），直接返回 429

### 1.6 测试方案

```cpp
// tests/test_rate_limiter.cpp

TEST(RateLimiterTest, AllowWithinLimit)
{
    RateLimiter limiter(RateLimitConfig{10.0, 10.0});  // 10 req/s, 桶容量 10

    // 前 10 个请求应该全部通过（消耗初始令牌）
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_TRUE(limiter.tryAcquire("192.168.1.1"));
    }
}

TEST(RateLimiterTest, RejectOverLimit)
{
    RateLimiter limiter(RateLimitConfig{10.0, 10.0});

    // 耗尽令牌
    for (int i = 0; i < 10; ++i)
    {
        limiter.tryAcquire("192.168.1.1");
    }

    // 第 11 个应该被拒绝
    EXPECT_FALSE(limiter.tryAcquire("192.168.1.1"));
}

TEST(RateLimiterTest, RefillAfterTime)
{
    RateLimiter limiter(RateLimitConfig{10.0, 10.0});

    // 耗尽令牌
    for (int i = 0; i < 10; ++i)
    {
        limiter.tryAcquire("192.168.1.1");
    }
    EXPECT_FALSE(limiter.tryAcquire("192.168.1.1"));

    // 等待 200ms → 应补充约 2 个令牌
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(limiter.tryAcquire("192.168.1.1"));
}

TEST(RateLimiterTest, DifferentIpsIndependent)
{
    RateLimiter limiter(RateLimitConfig{1.0, 1.0});  // 1 req/s

    EXPECT_TRUE(limiter.tryAcquire("ip_a"));
    EXPECT_FALSE(limiter.tryAcquire("ip_a"));  // ip_a 被限流

    EXPECT_TRUE(limiter.tryAcquire("ip_b"));   // ip_b 不受影响
}
```

### 1.7 生产环境注意事项

| 问题                    | 解决方案                                  |
| ----------------------- | ----------------------------------------- |
| 内存增长（IP 越来越多） | 定期清理长时间未访问的桶（LRU 淘汰）      |
| 多线程性能              | 可改为分片锁（按 IP hash 分片）减少锁竞争 |
| 分布式限流              | 当前是单机方案，分布式需 Redis 等共享存储 |
| 反向代理                | 需要从 `X-Forwarded-For` 头获取真实 IP    |

---

## 题目2：静态文件服务

### 2.1 需求分析

| 要求         | 说明                                  |
| ------------ | ------------------------------------- |
| 基本功能     | 从指定目录读取文件返回给客户端        |
| Content-Type | 根据文件扩展名设置正确的 MIME 类型    |
| Range 请求   | 支持断点续传（`Range: bytes=0-1023`） |
| 安全         | 防止路径穿越攻击（`../` 检查）        |
| 测试         | 验证安全性和正确性                    |

### 2.2 核心数据结构

```cpp
// src/core/StaticFileHandler.h

#pragma once
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <filesystem>
#include <string>
#include <unordered_map>

namespace hical
{

class StaticFileHandler
{
public:
    /**
     * @brief 构造静态文件处理器
     * @param rootDir 文件根目录（如 "./public"）
     * @param urlPrefix URL 前缀（如 "/static"）
     */
    StaticFileHandler(const std::string& rootDir, const std::string& urlPrefix = "/static");

    /**
     * @brief 处理文件请求
     * @param req HTTP 请求
     * @return HTTP 响应（文件内容或错误）
     */
    HttpResponse handle(const HttpRequest& req);

private:
    std::filesystem::path rootDir_;
    std::string urlPrefix_;

    // MIME 类型映射
    static const std::unordered_map<std::string, std::string>& mimeTypes();

    /**
     * @brief 根据文件扩展名获取 Content-Type
     */
    static std::string getMimeType(const std::string& extension);

    /**
     * @brief 安全路径解析（防止路径穿越）
     * @return 安全的绝对路径，穿越攻击时返回 nullopt
     */
    std::optional<std::filesystem::path> resolveSafePath(const std::string& requestPath);

    /**
     * @brief 处理 Range 请求
     */
    HttpResponse handleRangeRequest(const HttpRequest& req,
                                     const std::filesystem::path& filePath,
                                     size_t fileSize);
};

} // namespace hical
```

### 2.3 MIME 类型映射

```cpp
const std::unordered_map<std::string, std::string>& StaticFileHandler::mimeTypes()
{
    static const std::unordered_map<std::string, std::string> types = {
        {".html", "text/html"},
        {".css",  "text/css"},
        {".js",   "application/javascript"},
        {".json", "application/json"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".svg",  "image/svg+xml"},
        {".ico",  "image/x-icon"},
        {".txt",  "text/plain"},
        {".pdf",  "application/pdf"},
        {".woff", "font/woff"},
        {".woff2","font/woff2"},
        {".mp4",  "video/mp4"},
    };
    return types;
}

std::string StaticFileHandler::getMimeType(const std::string& ext)
{
    auto& types = mimeTypes();
    auto it = types.find(ext);
    return it != types.end() ? it->second : "application/octet-stream";
}
```

### 2.4 路径穿越防护 — 最关键的安全设计

```cpp
std::optional<std::filesystem::path> StaticFileHandler::resolveSafePath(
    const std::string& requestPath)
{
    // 1. 去掉 URL 前缀，得到相对路径
    //    如 "/static/images/logo.png" → "images/logo.png"
    auto relativePath = requestPath.substr(urlPrefix_.size());

    // 2. 拒绝包含 ".." 的路径（快速检查）
    if (relativePath.find("..") != std::string::npos)
    {
        return std::nullopt;
    }

    // 3. 构造绝对路径并规范化
    auto fullPath = std::filesystem::weakly_canonical(rootDir_ / relativePath);

    // 4. 最终检查：规范化后的路径必须以 rootDir_ 开头
    //    这是最可靠的穿越检查（防止编码绕过）
    auto rootStr = std::filesystem::weakly_canonical(rootDir_).string();
    auto fullStr = fullPath.string();

    if (fullStr.substr(0, rootStr.size()) != rootStr)
    {
        return std::nullopt;  // 穿越检测到！
    }

    return fullPath;
}
```

**为什么需要两层检查？**

| 攻击方式                       | `..` 检查 | 规范化前缀检查       |
| ------------------------------ | --------- | -------------------- |
| `/static/../etc/passwd`        | 拦截      | 拦截                 |
| `/static/..%2f..%2fetc/passwd` | 可能绕过  | 拦截（规范化后判断） |
| `/static/images/../../secret`  | 拦截      | 拦截                 |

第一层快速拒绝明显的 `..`，第二层 `weakly_canonical` 处理编码绕过和符号链接。

### 2.5 Range 请求（断点续传）

```cpp
HttpResponse StaticFileHandler::handleRangeRequest(
    const HttpRequest& req,
    const std::filesystem::path& filePath,
    size_t fileSize)
{
    auto rangeHeader = req.header("Range");
    // 解析 "bytes=0-1023" 或 "bytes=1024-" 或 "bytes=-512"

    size_t start = 0;
    size_t end = fileSize - 1;

    // 解析 Range 头...
    // 如果范围无效 → 返回 416 Range Not Satisfiable

    // 读取文件指定范围
    std::ifstream file(filePath, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(start));

    size_t length = end - start + 1;
    std::string content(length, '\0');
    file.read(content.data(), static_cast<std::streamsize>(length));

    HttpResponse res;
    res.setStatus(HttpStatusCode::hPartialContent);  // 206
    res.setBody(content, getMimeType(filePath.extension().string()));
    res.setHeader("Content-Range",
                  "bytes " + std::to_string(start) + "-" + std::to_string(end)
                  + "/" + std::to_string(fileSize));
    res.setHeader("Accept-Ranges", "bytes");
    return res;
}
```

**Range 请求的典型场景**：
- 大文件下载中断后恢复
- 视频播放器拖动进度条
- 多线程下载工具分段下载

### 2.6 主处理函数

```cpp
HttpResponse StaticFileHandler::handle(const HttpRequest& req)
{
    // 1. 安全路径解析
    auto safePath = resolveSafePath(req.path());
    if (!safePath)
    {
        return HttpResponse::badRequest("Invalid path");
    }

    // 2. 检查文件是否存在
    if (!std::filesystem::exists(*safePath) || !std::filesystem::is_regular_file(*safePath))
    {
        return HttpResponse::notFound();
    }

    auto fileSize = std::filesystem::file_size(*safePath);
    auto ext = safePath->extension().string();

    // 3. Range 请求
    if (!req.header("Range").empty())
    {
        return handleRangeRequest(req, *safePath, fileSize);
    }

    // 4. 完整文件响应
    std::ifstream file(*safePath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    auto res = HttpResponse::ok(content);
    res.setHeader("Content-Type", getMimeType(ext));
    res.setHeader("Accept-Ranges", "bytes");   // 告知支持 Range
    return res;
}
```

### 2.7 注册到 Router

```cpp
// 方式1：注册为路由
StaticFileHandler fileHandler("./public", "/static");
server.router().get("/static/{path}",
    [&fileHandler](const HttpRequest& req) -> HttpResponse {
        return fileHandler.handle(req);
    });

// 方式2：注册为中间件（捕获所有未匹配路由的请求）
server.use([&fileHandler](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    if (req.path().starts_with("/static/"))
    {
        co_return fileHandler.handle(req);
    }
    co_return co_await next(req);
});
```

### 2.8 测试方案

```cpp
TEST(StaticFileTest, ServeExistingFile)       // 读取存在的文件
TEST(StaticFileTest, NotFoundForMissing)      // 文件不存在返回 404
TEST(StaticFileTest, CorrectMimeType)         // .html → text/html, .png → image/png
TEST(StaticFileTest, PathTraversalBlocked)     // "../etc/passwd" 返回 400
TEST(StaticFileTest, EncodedTraversalBlocked)  // "%2e%2e" 编码绕过被拦截
TEST(StaticFileTest, RangeRequestPartial)      // Range: bytes=0-99 → 206 + 100字节
TEST(StaticFileTest, RangeRequestSuffix)       // Range: bytes=-100 → 最后100字节
TEST(StaticFileTest, InvalidRangeReturns416)   // Range: bytes=999-0 → 416
```

---

## 题目3：JSON RPC 路由扩展

### 3.1 需求分析

| 要求 | 说明                                   |
| ---- | -------------------------------------- |
| 协议 | JSON-RPC 2.0 规范                      |
| 分发 | 解析 `method` 字段，分发到对应处理函数 |
| 批量 | 支持批量请求（数组格式）               |
| 错误 | 标准错误码（-32600/-32601/-32602 等）  |

### 3.2 JSON-RPC 2.0 协议速览

**请求格式**：

```json
{
    "jsonrpc": "2.0",
    "method": "subtract",
    "params": [42, 23],
    "id": 1
}
```

**响应格式（成功）**：

```json
{
    "jsonrpc": "2.0",
    "result": 19,
    "id": 1
}
```

**响应格式（失败）**：

```json
{
    "jsonrpc": "2.0",
    "error": {"code": -32601, "message": "Method not found"},
    "id": 1
}
```

**批量请求**：

```json
[
    {"jsonrpc": "2.0", "method": "add", "params": [1, 2], "id": 1},
    {"jsonrpc": "2.0", "method": "sub", "params": [5, 3], "id": 2}
]
```

**标准错误码**：

| 码     | 含义             | 触发条件          |
| ------ | ---------------- | ----------------- |
| -32700 | Parse error      | JSON 解析失败     |
| -32600 | Invalid Request  | 缺少必要字段      |
| -32601 | Method not found | method 未注册     |
| -32602 | Invalid params   | 参数类型/数量错误 |
| -32603 | Internal error   | 服务器内部错误    |

### 3.3 核心数据结构

```cpp
// src/core/JsonRpc.h

#pragma once
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <boost/json.hpp>
#include <functional>
#include <string>
#include <unordered_map>

namespace hical
{

namespace json = boost::json;

/**
 * @brief JSON-RPC 方法处理器
 *
 * 接收 params（可能是数组或对象），返回 result 值。
 * 抛异常表示错误。
 */
using JsonRpcHandler = std::function<json::value(const json::value& params)>;

/**
 * @brief JSON-RPC 错误
 */
struct JsonRpcError
{
    int code;
    std::string message;
    json::value data;    // 可选的附加数据
};

/**
 * @brief JSON-RPC 2.0 路由器
 *
 * 在现有 Router 基础上扩展，处理 POST /rpc 请求。
 * 解析 JSON-RPC 请求，按 method 字段分发到处理函数。
 */
class JsonRpcRouter
{
public:
    /**
     * @brief 注册 RPC 方法
     * @param method 方法名
     * @param handler 处理函数
     */
    void registerMethod(const std::string& method, JsonRpcHandler handler);

    /**
     * @brief 处理 JSON-RPC 请求
     * @param req HTTP 请求（body 为 JSON-RPC）
     * @return HTTP 响应（body 为 JSON-RPC 响应）
     */
    HttpResponse handle(const HttpRequest& req);

private:
    std::unordered_map<std::string, JsonRpcHandler> methods_;

    // 处理单个 JSON-RPC 请求
    json::value handleSingle(const json::value& request);

    // 构建成功响应
    static json::value makeResult(const json::value& id, const json::value& result);

    // 构建错误响应
    static json::value makeError(const json::value& id, int code, const std::string& message);
};

} // namespace hical
```

### 3.4 核心实现

```cpp
// src/core/JsonRpc.cpp

void JsonRpcRouter::registerMethod(const std::string& method, JsonRpcHandler handler)
{
    methods_[method] = std::move(handler);
}

HttpResponse JsonRpcRouter::handle(const HttpRequest& req)
{
    // 1. 解析 JSON
    auto body = req.body();
    boost::system::error_code ec;
    auto parsed = json::parse(body, ec);
    if (ec)
    {
        auto errResp = makeError(nullptr, -32700, "Parse error");
        return HttpResponse::json(errResp);
    }

    // 2. 判断是批量还是单个
    if (parsed.is_array())
    {
        // 批量请求
        auto& arr = parsed.as_array();
        if (arr.empty())
        {
            return HttpResponse::json(makeError(nullptr, -32600, "Invalid Request"));
        }

        json::array results;
        for (const auto& item : arr)
        {
            auto result = handleSingle(item);
            if (!result.is_null())   // 通知（无 id）不返回响应
            {
                results.push_back(std::move(result));
            }
        }
        return HttpResponse::json(json::value(std::move(results)));
    }
    else if (parsed.is_object())
    {
        // 单个请求
        auto result = handleSingle(parsed);
        return HttpResponse::json(result);
    }
    else
    {
        return HttpResponse::json(makeError(nullptr, -32600, "Invalid Request"));
    }
}

json::value JsonRpcRouter::handleSingle(const json::value& request)
{
    // 验证必要字段
    if (!request.is_object())
    {
        return makeError(nullptr, -32600, "Invalid Request");
    }

    auto& obj = request.as_object();

    // 检查 jsonrpc 版本
    auto* version = obj.if_contains("jsonrpc");
    if (!version || version->as_string() != "2.0")
    {
        return makeError(nullptr, -32600, "Invalid Request: missing jsonrpc 2.0");
    }

    // 检查 method
    auto* methodPtr = obj.if_contains("method");
    if (!methodPtr || !methodPtr->is_string())
    {
        return makeError(nullptr, -32600, "Invalid Request: missing method");
    }
    std::string method(methodPtr->as_string());

    // 获取 id（可能不存在 = 通知）
    auto* idPtr = obj.if_contains("id");
    json::value id = idPtr ? *idPtr : json::value(nullptr);

    // 获取 params（可选）
    auto* paramsPtr = obj.if_contains("params");
    json::value params = paramsPtr ? *paramsPtr : json::value(nullptr);

    // 查找方法
    auto it = methods_.find(method);
    if (it == methods_.end())
    {
        return makeError(id, -32601, "Method not found: " + method);
    }

    // 调用处理函数
    try
    {
        auto result = it->second(params);

        // 通知（无 id）不返回响应
        if (id.is_null() && !idPtr)
        {
            return json::value(nullptr);  // 不返回
        }

        return makeResult(id, result);
    }
    catch (const JsonRpcError& e)
    {
        return makeError(id, e.code, e.message);
    }
    catch (const std::exception& e)
    {
        return makeError(id, -32603, std::string("Internal error: ") + e.what());
    }
}

json::value JsonRpcRouter::makeResult(const json::value& id, const json::value& result)
{
    return {{"jsonrpc", "2.0"}, {"result", result}, {"id", id}};
}

json::value JsonRpcRouter::makeError(const json::value& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"},
            {"error", {{"code", code}, {"message", message}}},
            {"id", id}};
}
```

### 3.5 注册到 Router

```cpp
// 创建 JSON-RPC 路由器
JsonRpcRouter rpc;

// 注册方法
rpc.registerMethod("add", [](const json::value& params) -> json::value {
    auto& arr = params.as_array();
    return arr[0].as_int64() + arr[1].as_int64();
});

rpc.registerMethod("subtract", [](const json::value& params) -> json::value {
    auto& arr = params.as_array();
    return arr[0].as_int64() - arr[1].as_int64();
});

rpc.registerMethod("echo", [](const json::value& params) -> json::value {
    return params;
});

// 注册到 HTTP 路由
server.router().post("/rpc", [&rpc](const HttpRequest& req) -> HttpResponse {
    return rpc.handle(req);
});
```

### 3.6 测试方案

```cpp
TEST(JsonRpcTest, SingleCall)
{
    // {"jsonrpc":"2.0","method":"add","params":[1,2],"id":1}
    // → {"jsonrpc":"2.0","result":3,"id":1}
}

TEST(JsonRpcTest, MethodNotFound)
{
    // {"jsonrpc":"2.0","method":"nonexistent","params":[],"id":1}
    // → {"error":{"code":-32601,"message":"Method not found: nonexistent"}}
}

TEST(JsonRpcTest, ParseError)
{
    // 发送非法 JSON → {"error":{"code":-32700,"message":"Parse error"}}
}

TEST(JsonRpcTest, BatchRequest)
{
    // [{"method":"add","params":[1,2],"id":1},{"method":"sub","params":[5,3],"id":2}]
    // → [{"result":3,"id":1},{"result":2,"id":2}]
}

TEST(JsonRpcTest, Notification)
{
    // 无 id 的请求 → 不返回响应（或返回空数组）
}

TEST(JsonRpcTest, InvalidRequest)
{
    // 缺少 jsonrpc 字段 → -32600
}

TEST(JsonRpcTest, InternalError)
{
    // 处理函数抛异常 → -32603
}
```

---

## 题目4：基于反射的自动 CRUD 服务

### 4.1 需求分析

| 要求     | 说明                                                                        |
| -------- | --------------------------------------------------------------------------- |
| 核心功能 | 给定一个 DTO 结构体，自动生成 CRUD 的 4 个路由（Create/Read/Update/Delete） |
| 反射层   | 使用 `HICAL_JSON` / `HICAL_HANDLER` / `HICAL_ROUTES` 宏                     |
| 存储     | 内存中的 `std::unordered_map<int, T>` 模拟数据库                            |
| JSON     | 自动序列化/反序列化，无需手动构建 JSON                                      |
| 测试     | 端到端测试 4 个 CRUD 操作                                                   |

### 4.2 核心思路

利用 `MetaJson.h` 的 `toJson` / `fromJson` 和 `MetaRoutes.h` 的 `registerRoutes`，为任意 DTO 自动生成 RESTful API：

```cpp
// 1. 定义 DTO
struct Product {
    std::string name;
    double price;
    int stock;
    HICAL_JSON(Product, name, price, stock)
};

// 2. 定义 CrudHandler 模板
template <typename T>
struct CrudHandler {
    std::unordered_map<int, T> store;
    int nextId = 1;

    // POST /api/products — 创建
    HttpResponse create(const HttpRequest& req) {
        auto item = req.readJson<T>();
        int id = nextId++;
        store[id] = item;
        return HttpResponse::json({{"id", id}, {"created", true}});
    }
    HICAL_HANDLER(Post, "/api/products", create)

    // GET /api/products/{id} — 读取
    HttpResponse read(const HttpRequest& req) {
        int id = std::stoi(req.param("id"));
        auto it = store.find(id);
        if (it == store.end()) return HttpResponse::notFound();
        return HttpResponse::json(meta::toJson(it->second));
    }
    HICAL_HANDLER(Get, "/api/products/{id}", read)

    // PUT /api/products/{id} — 更新
    HttpResponse update(const HttpRequest& req) {
        int id = std::stoi(req.param("id"));
        auto it = store.find(id);
        if (it == store.end()) return HttpResponse::notFound();
        it->second = req.readJson<T>();
        return HttpResponse::json({{"id", id}, {"updated", true}});
    }
    HICAL_HANDLER(Put, "/api/products/{id}", update)

    // DELETE /api/products/{id} — 删除
    HttpResponse remove(const HttpRequest& req) {
        int id = std::stoi(req.param("id"));
        if (store.erase(id) == 0) return HttpResponse::notFound();
        return HttpResponse::json({{"id", id}, {"deleted", true}});
    }
    HICAL_HANDLER(Delete, "/api/products/{id}", remove)

    HICAL_ROUTES(CrudHandler, create, read, update, remove)
};
```

### 4.3 使用方式

```cpp
HttpServer server(8080);

CrudHandler<Product> productHandler;
meta::registerRoutes(server.router(), productHandler);

server.start();
```

```bash
# 创建
curl -X POST -d '{"name":"Widget","price":9.99,"stock":100}' http://localhost:8080/api/products
# 读取
curl http://localhost:8080/api/products/1
# 更新
curl -X PUT -d '{"name":"Widget Pro","price":19.99,"stock":50}' http://localhost:8080/api/products/1
# 删除
curl -X DELETE http://localhost:8080/api/products/1
```

### 4.4 测试方案

```cpp
TEST(CrudTest, CreateAndRead)       // POST 创建 → GET 读取，数据一致
TEST(CrudTest, UpdateExisting)      // PUT 更新 → GET 验证更新后的数据
TEST(CrudTest, DeleteExisting)      // DELETE 删除 → GET 返回 404
TEST(CrudTest, ReadNotFound)        // GET 不存在的 ID → 404
TEST(CrudTest, DeleteNotFound)      // DELETE 不存在的 ID → 404
TEST(CrudTest, MultipleItems)       // 创建多个 → 逐一读取验证
```

### 4.5 扩展方向

| 扩展     | 说明                                        |
| -------- | ------------------------------------------- |
| 分页查询 | `GET /api/products?page=1&size=10` 返回列表 |
| 搜索过滤 | `GET /api/products?name=Widget` 按字段过滤  |
| 线程安全 | 用 `shared_mutex` 实现读写锁保护 store      |
| 持久化   | 将 store 序列化到文件或 SQLite              |

### 4.6 框架知识覆盖

| 知识点                           | 课程来源      |
| -------------------------------- | ------------- |
| `HICAL_JSON` 宏                  | 第9课（反射） |
| `HICAL_HANDLER` / `HICAL_ROUTES` | 第9课（反射） |
| `meta::registerRoutes`           | 第9课（反射） |
| `req.readJson<T>()`              | 第9课（反射） |
| `meta::toJson(obj)`              | 第9课（反射） |
| Router 路径参数                  | 第6课（路由） |
| HttpResponse 工厂方法            | 第6课（路由） |

---

## 四、四个题目的架构对比

| 维度         | Rate Limiter                 | 静态文件                  | JSON RPC                   | 反射 CRUD             |
| ------------ | ---------------------------- | ------------------------- | -------------------------- | --------------------- |
| **集成方式** | 中间件                       | 路由 / 中间件             | 路由                       | 反射路由              |
| **新增文件** | RateLimiter.h/cpp            | StaticFileHandler.h/cpp   | JsonRpc.h/cpp              | CrudHandler.h（模板） |
| **核心算法** | 令牌桶                       | 路径规范化 + MIME映射     | JSON 解析 + method 分发    | 反射自动序列化 + 路由 |
| **安全重点** | IP 隔离                      | 路径穿越防护              | 输入验证（JSON 格式）      | JSON 类型安全         |
| **状态管理** | IP → 令牌桶映射              | 无状态                    | 无状态                     | 内存 store            |
| **线程安全** | mutex 保护映射表             | 天然线程安全（只读文件）  | 天然线程安全（只读映射表） | 需加锁保护 store      |
| **框架知识** | 中间件、HttpRequest/Response | Router、HttpResponse 工厂 | Router、boost::json        | 反射层（第9课）全部   |

### 开发流程

三个题目都遵循同一个流程：

```
1. src/core/ 定义接口（.h 头文件）
       │
2. src/core/ 实现逻辑（.cpp）
       │
3. tests/ 编写单元测试
       │
4. examples/ 编写使用示例
       │
5. CMakeLists.txt 添加新文件
       │
6. ctest --test-dir build --output-on-failure  ← 确保通过
```

---

## 五、与游戏服务器的对比

| 扩展功能         | 游戏服务器等价物                    |
| ---------------- | ----------------------------------- |
| Rate Limiter     | 频率限制（防刷、防 DDoS、操作冷却） |
| 令牌桶算法       | 技能冷却系统（CD 恢复 ≈ 令牌补充）  |
| 静态文件服务     | 资源下载服务器（补丁包、配置文件）  |
| 路径穿越防护     | 防止客户端请求非法资源文件          |
| Range 断点续传   | 大型补丁包下载的断点续传            |
| JSON-RPC         | GM 命令接口 / 管理后台 API          |
| 批量请求         | 批量执行 GM 命令                    |
| 标准错误码       | 统一的错误码体系（给管理工具使用）  |
| 反射 CRUD        | 道具/配置管理后台的自动 CRUD API    |
| 自动 JSON 序列化 | 自动将游戏数据结构体打包为网络消息  |

---

## 六、关键问题思考与回答

**Q1: Rate Limiter 为什么用惰性令牌补充而不是定时器？**

> 1. **简单**：不需要后台线程或定时器
> 2. **精确**：根据实际经过的时间计算，精度取决于 `steady_clock`
> 3. **零开销**：没有定时器回调的 CPU 消耗
> 4. **天然适配**：HTTP 请求驱动——只有请求到来时才需要判断

**Q2: 路径穿越防护为什么要两层检查？**

> 第一层 `..` 字符串检查是快速过滤，拦截 95% 的攻击。但攻击者可能用 URL 编码（`%2e%2e`）、double encoding、符号链接等绕过字符串检查。第二层 `weakly_canonical` 将路径规范化为绝对路径后做前缀匹配，这是最终的安全屏障。

**Q3: JSON-RPC 为什么选择同步处理而不是协程？**

> JSON-RPC 的处理函数通常是纯计算（加减、查表），不涉及 IO。同步版本更简单。如果需要异步（如查数据库），可以改为 `AsyncJsonRpcHandler = std::function<Awaitable<json::value>(const json::value&)>`，与框架的协程体系统一。

**Q4: 三个扩展各用了框架的哪些知识？**

> | 知识点 | Rate Limiter | 静态文件 | JSON RPC | 反射 CRUD |
> |--------|-------------|---------|----------|-----------|
> | 中间件（第7课） | 核心集成方式 | 可选集成方式 | - | - |
> | Router（第6课） | - | 路由注册 | 路由注册 | 反射路由注册 |
> | HttpRequest（第6课） | header("X-Real-IP") | header("Range"), path() | body(), jsonBody() | readJson<T>(), param() |
> | HttpResponse（第6课） | 429 状态码 | 206 + Content-Range | json() 工厂方法 | json() + toJson() |
> | 协程（第0课） | MiddlewareHandler 签名 | - | 可选扩展 | - |
> | 反射层（第9课） | - | - | - | 全部（核心） |

---

*至此 Hical 框架的全部课程（第0课~第8课 + 项目A + 项目B）学习笔记全部完成。*
