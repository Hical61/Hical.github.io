+++
title = '第9课：Cookie、Session 与文件服务'
date = '2026-04-15'
draft = false
tags = ["C++", "Cookie", "Session", "文件服务", "Multipart", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "理解 Web 应用的最后一公里：Cookie 状态记忆、Session 有状态会话、静态文件安全托管、Multipart 文件上传。"
+++

# 第9课：Cookie、Session 与文件服务 - 学习笔记

> 理解 Web 应用的"最后一公里"：Cookie 状态记忆、Session 有状态会话、静态文件安全托管、Multipart 文件上传。

---

## 一、为什么需要这四个模块？

经过第8课的 HttpServer 整合，hical 已具备完整的 HTTP 服务器骨架。但真正搭建 Web 应用时会发现缺少几样东西：

| 问题                   | 解决方案                   | 核心文件                                          |
| ---------------------- | -------------------------- | ------------------------------------------------- |
| 用户刷新页面后状态丢失 | Cookie（客户端状态存储）   | `Cookie.h`、`HttpRequest.cpp`、`HttpResponse.cpp` |
| 服务端需要跟踪用户会话 | Session（服务端会话管理）  | `Session.h`、`Session.cpp`                        |
| 无法提供前端静态资源   | StaticFiles（文件托管）    | `StaticFiles.h`（header-only）                    |
| 用户无法上传文件       | Multipart（RFC 7578 解析） | `Multipart.h`、`Multipart.cpp`                    |

它们在 hical 架构中的位置：

```
客户端请求
    │
    ▼
TcpServer (Accept)
    │
    ▼
HttpServer::handleSession()
    │
    ├── Cookie 解析 ← req.cookie() 惰性触发
    │
    ▼
MiddlewarePipeline
    │
    ├── Session 中间件 ← makeSessionMiddleware()
    │   ├── 读 Cookie → 查找/创建 Session
    │   └── 请求完成后 → 若 dirty 则写 Set-Cookie
    │
    ▼
Router dispatch
    │
    ├── 静态文件路由 ← serveStatic() 返回的处理器
    ├── 文件上传路由 ← MultipartParser::parse() 解析请求体
    └── 业务路由
```

---

## 二、Cookie — 无状态协议的状态记忆

### 2.1 CookieOptions 结构体

**源码位置**：`src/core/Cookie.h`

```cpp
struct CookieOptions
{
    std::string path = "/";     // Cookie 作用路径
    std::string domain;         // 作用域名，空表示当前域
    int maxAge = -1;            // 有效期（秒），-1 表示会话 Cookie
    bool httpOnly = false;      // 禁止 JavaScript 访问（防 XSS）
    bool secure = false;        // 仅 HTTPS 传输
    std::string sameSite;       // "Strict" / "Lax" / "None"
};
```

**每个字段的安全含义**：

| 字段       | 作用                                       | 防御目标               |
| ---------- | ------------------------------------------ | ---------------------- |
| `httpOnly` | JavaScript 无法通过 `document.cookie` 读取 | XSS 窃取 Cookie        |
| `secure`   | 仅在 HTTPS 下发送                          | 中间人嗅探 Cookie      |
| `sameSite` | 限制第三方网站携带 Cookie                  | CSRF 跨站请求伪造      |
| `maxAge`   | Cookie 过期时间                            | 防止永久有效的泄露风险 |

### 2.2 请求端：Cookie 解析

**源码位置**：`src/core/HttpRequest.h` / `src/core/HttpRequest.cpp`

**核心 API**：

```cpp
class HttpRequest {
    std::string cookie(const std::string& name) const;    // 获取单个 Cookie
    const std::unordered_map<std::string, std::string>&
        cookies() const;                                   // 获取全部 Cookie
    bool hasCookie(const std::string& name) const;         // 是否存在

private:
    void parseCookies() const;                             // 惰性解析
    mutable std::optional<std::unordered_map<std::string, std::string>> cookies_;
};
```

**设计亮点 1：惰性解析**

```cpp
std::string HttpRequest::cookie(const std::string& name) const
{
    if (!cookies_)       // 首次访问才解析
    {
        parseCookies();  // 结果缓存到 cookies_
    }
    auto it = cookies_->find(name);
    return (it != cookies_->end()) ? it->second : "";
}
```

为什么是惰性的？大多数 API 路由不需要 Cookie（如 `/api/status`），如果每个请求都预先解析 Cookie 头，是无意义的开销。用 `mutable std::optional` 实现按需解析 + 结果缓存。

**设计亮点 2：首胜策略（First-Wins）**

```cpp
void HttpRequest::parseCookies() const
{
    cookies_.emplace();
    auto cookieHeader = header("Cookie");
    // 解析 "name1=value1; name2=value2; ..." 格式
    // ...
    if (!name.empty())
    {
        // RFC 6265：同名 Cookie 以先出现的值为准
        (*cookies_).try_emplace(std::string(name), std::string(value));
    }
}
```

`try_emplace` 不覆盖已有键——如果浏览器发送了两个同名 Cookie（如主域和子域各设了一个），第一个优先。这是 RFC 6265 的推荐行为。

### 2.3 响应端：Cookie 设置

**源码位置**：`src/core/HttpResponse.cpp`

```cpp
void HttpResponse::setCookie(const std::string& name,
                              const std::string& value,
                              const CookieOptions& options)
{
    // 1. CRLF 注入防护
    auto containsCRLF = [](const std::string& s) -> bool {
        return s.find('\r') != std::string::npos ||
               s.find('\n') != std::string::npos;
    };
    if (containsCRLF(name) || containsCRLF(value))
    {
        return;  // 静默拒绝
    }

    // 2. RFC 6265 百分号编码
    auto encodeCookieValue = [](const std::string& raw) -> std::string {
        // 合法字符: %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
        // 其余百分号编码
    };

    // 3. 构建 Set-Cookie 头
    std::ostringstream oss;
    oss << name << "=" << encodeCookieValue(value);
    if (!options.path.empty())     oss << "; Path=" << options.path;
    if (!options.domain.empty())   oss << "; Domain=" << options.domain;
    if (options.maxAge >= 0)       oss << "; Max-Age=" << options.maxAge;
    if (options.httpOnly)          oss << "; HttpOnly";
    if (options.secure)            oss << "; Secure";
    if (!options.sameSite.empty()) oss << "; SameSite=" << options.sameSite;

    // 4. 使用 insert 而非 set，支持同一响应设置多个 Cookie
    res_.insert(boost::beast::http::field::set_cookie, oss.str());
}
```

**三个安全防护层**：

1. **CRLF 注入防护**：如果 name 或 value 包含 `\r` 或 `\n`，直接拒绝。否则攻击者可以注入额外的 HTTP 头（HTTP Response Splitting）。
2. **RFC 6265 编码**：非安全字符百分号编码，防止 Cookie 值破坏 HTTP 头格式。
3. **insert 而非 set**：Beast 的 `set()` 会覆盖同名头，但一个响应可能需要设置多个 Cookie（如 session ID + 用户偏好），`insert()` 追加多个 `Set-Cookie` 头。

---

## 三、Session — 从 Cookie 到有状态会话

### 3.1 Session 数据类

**源码位置**：`src/core/Session.h`

```cpp
class Session
{
    std::string id_;                                     // 构造后不变
    mutable std::mutex mutex_;                           // 保护所有可变字段
    std::unordered_map<std::string, std::any> data_;     // 键值存储
    bool dirty_ = false;                                 // 是否被修改
    std::chrono::steady_clock::time_point lastAccess_;   // 最后访问时间
};
```

**API 设计要点**：

| 方法                      | 用途                                 | 线程安全         |
| ------------------------- | ------------------------------------ | ---------------- |
| `set(key, value)`         | 存储任意类型值，自动标记 dirty       | 是（lock_guard） |
| `get<T>(key)`             | 类型安全读取，类型不匹配返回 nullopt | 是               |
| `has(key)`                | 检查属性是否存在                     | 是               |
| `remove(key)` / `clear()` | 删除属性                             | 是               |
| `isDirty()`               | 是否需要刷新 Cookie                  | 是               |
| `touch()`                 | 更新最后访问时间（续期）             | 是               |

**为什么用 `std::any` 而不是 `std::variant`？**

`std::variant` 需要编译期确定所有可能类型——Session 可能存 `string`（用户名）、`int`（权限等级）、`vector<string>`（角色列表）等任意业务类型。`std::any` 提供运行时类型擦除，`get<T>()` 通过 `any_cast` 做类型检查，类型不匹配返回 `nullopt` 而不是抛异常。

**为什么不用 `void*`？**

`void*` 丢失类型信息，`any` 保留了类型元数据，`any_cast` 会检查类型匹配，更安全。

### 3.2 SessionManager — 会话管理器

**源码位置**：`src/core/Session.cpp`

```cpp
class SessionManager
{
    SessionOptions opts_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> store_;
    std::chrono::steady_clock::time_point lastGc_;
};
```

**SessionOptions 配置**：

```cpp
struct SessionOptions
{
    std::string cookieName = "HICAL_SESSION";  // Cookie 名
    int maxAge = 3600;                         // 有效期 1 小时
    bool httpOnly = true;                      // 防 XSS
    bool secure = false;                       // 仅 HTTPS
    std::string sameSite = "Lax";              // 防 CSRF
    std::string path = "/";
    int gcInterval = 300;                      // GC 间隔 5 分钟
};
```

#### 3.2.1 128-bit 安全 ID 生成

```cpp
std::string SessionManager::generateId()
{
    // thread_local 避免加锁，每个线程独立的 RNG
    thread_local std::mt19937_64 rng(std::random_device {}());
    std::uniform_int_distribution<uint64_t> dist;

    // 两个 64 位随机数拼成 128 位 → 32 位十六进制字符
    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(16) << hi << std::setw(16) << lo;
    return oss.str();
}
```

128 位空间 = 2^128 ≈ 3.4 × 10^38 种可能，暴力猜测不可行。`thread_local` RNG 避免了多线程共享随机引擎的锁竞争。

#### 3.2.2 查找与过期检查

```cpp
std::shared_ptr<Session> SessionManager::find(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(id);
    if (it == store_.end()) return nullptr;

    // 检查是否已过期（毫秒精度）
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - it->second->lastAccess()).count();
    if (opts_.maxAge > 0 &&
        elapsedMs >= static_cast<long long>(opts_.maxAge) * 1000LL)
    {
        store_.erase(it);  // 过期则删除
        return nullptr;
    }

    return it->second;
}
```

**双重过期检查**：`find()` 时检查单个 Session 过期 + `create()` 时批量 GC。

#### 3.2.3 惰性 GC（垃圾回收）

```cpp
std::shared_ptr<Session> SessionManager::create()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 惰性 GC：每隔 gcInterval 秒在 create() 时顺带清理
    if (opts_.gcInterval > 0)
    {
        auto now = std::chrono::steady_clock::now();
        auto sinceGcMs = duration_cast<milliseconds>(now - lastGc_).count();
        if (sinceGcMs >= static_cast<long long>(opts_.gcInterval) * 1000LL)
        {
            lastGc_ = now;
            // 遍历删除过期 Session
            for (auto it = store_.begin(); it != store_.end();)
            {
                if (expired(it->second)) it = store_.erase(it);
                else ++it;
            }
        }
    }

    auto id = generateId();
    while (store_.count(id)) id = generateId();  // 碰撞保护
    auto session = std::make_shared<Session>(id);
    store_[id] = session;
    return session;
}
```

**为什么是"惰性"而不是定时器？**

- 不需要额外的后台线程，减少复杂度
- 只在有新 Session 创建时触发 GC，空闲时不浪费 CPU
- 过期 Session 在 `find()` 时也会被单独清理，不会被错误使用

### 3.3 Session 中间件

**源码位置**：`src/core/Session.h`（`makeSessionMiddleware` 工厂函数）

```cpp
inline MiddlewareHandler makeSessionMiddleware(std::shared_ptr<SessionManager> manager)
{
    return [manager](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
    {
        const auto& opts = manager->options();

        // 1. 从 Cookie 中读取 Session ID
        auto sessionId = req.cookie(opts.cookieName);
        std::shared_ptr<Session> session;

        if (!sessionId.empty())
            session = manager->find(sessionId);
        if (!session)
            session = manager->create();  // 新建
        session->touch();                 // 续期

        // 2. 注入到请求属性
        req.setAttribute(SessionManager::hSessionKey, session);

        // 3. 执行后续中间件/路由
        auto res = co_await next(req);

        // 4. dirty 或 ID 变更时刷新 Cookie
        if (session->isDirty() || sessionId != session->id())
        {
            CookieOptions cookieOpts;
            cookieOpts.maxAge = opts.maxAge;
            cookieOpts.httpOnly = opts.httpOnly;
            cookieOpts.secure = opts.secure;
            cookieOpts.sameSite = opts.sameSite;
            cookieOpts.path = opts.path;
            res.setCookie(opts.cookieName, session->id(), cookieOpts);
        }

        co_return res;
    };
}
```

**洋葱模型在 Session 中的体现**：

```
请求进入
    │
    ▼ Session 中间件 Pre：读 Cookie → find/create Session → 注入 attribute
    │
    ▼ 业务路由：session->set("user", "alice")  → dirty = true
    │
    ▼ Session 中间件 Post：dirty? → 写 Set-Cookie
    │
响应返回
```

**Dirty Flag 优化**：只有 `set()` 被调用过（或 Session ID 变更）时才写 `Set-Cookie`。避免每个请求都刷新 Cookie，减少响应头大小和浏览器 Cookie 写入开销。

### 3.4 在路由处理器中使用 Session

```cpp
// 配置
auto sessionMgr = std::make_shared<hical::SessionManager>();
server.use(hical::makeSessionMiddleware(sessionMgr));

// 登录
server.router().post("/login", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<hical::Session>>(
        hical::SessionManager::hSessionKey);
    if (session) {
        (*session)->set("user", std::string("alice"));
        return HttpResponse::ok("Logged in");
    }
    return HttpResponse::serverError();
});

// 需要鉴权的路由
server.router().get("/profile", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<hical::Session>>(
        hical::SessionManager::hSessionKey);
    if (session && (*session)->has("user")) {
        auto user = (*session)->get<std::string>("user");
        return HttpResponse::ok("Hello " + *user);
    }
    return HttpResponse::badRequest("Not logged in");
});
```

---

## 四、StaticFiles — 安全地托管静态资源

### 4.1 serveStatic() 工厂函数

**源码位置**：`src/core/StaticFiles.h`（header-only）

```cpp
inline std::function<HttpResponse(const HttpRequest&)> serveStatic(
    const std::string& rootDir,     // 本地目录
    const std::string& urlPrefix,   // URL 前缀
    std::uintmax_t maxFileSize = 64ULL * 1024 * 1024  // 64MB 限制
);
```

**使用方式**：

```cpp
// 将 /static/... 映射到 ./public 目录
server.router().get("/static/{path}", hical::serveStatic("./public", "/static/"));
```

### 4.2 完整处理流程

```
请求: GET /static/css/style.css
    │
    ├── 1. 去除 URL 前缀 → "css/style.css"
    │
    ├── 2. 拼接根目录 → ./public/css/style.css
    │
    ├── 3. canonical() 规范化（解析符号链接）
    │
    ├── 4. isSafePath() 路径安全检查 ← 路径穿越防护
    │
    ├── 5. 目录？→ 追加 index.html，重新 canonical + 安全检查
    │
    ├── 6. file_size() 检查 → 超过 64MB 返回 413
    │
    ├── 7. ETag 缓存验证 → 匹配 If-None-Match 返回 304
    │
    ├── 8. 读取文件 + MIME 类型检测
    │
    └── 9. 返回 200 + Content-Type + ETag
```

### 4.3 安全防护：路径穿越攻击

攻击者可能尝试：`GET /static/../../etc/passwd`

**防护策略（纵深防御）**：

**第一层：`std::filesystem::canonical()`**

```cpp
fs::path target = root / std::string(relPath);
target = fs::canonical(target, ec2);  // 解析 ".."、符号链接
```

`canonical()` 会把 `./public/../../etc/passwd` 解析为 `/etc/passwd`，消除所有 `..` 和符号链接。

**第二层：`isSafePath()` 迭代器比较**

```cpp
inline bool isSafePath(const fs::path& root, const fs::path& target)
{
    // 逐段迭代器比较：root 的每段必须是 target 的前缀
    auto rootIt = root.begin();
    auto targetIt = target.begin();
    for (; rootIt != root.end(); ++rootIt, ++targetIt)
    {
        if (targetIt == target.end() || *rootIt != *targetIt)
            return false;
    }
    return true;
}
```

**为什么不用字符串前缀匹配？**

字符串前缀 `"/pub"` 会错误匹配 `"/public"`。逐段迭代器比较确保路径分量完全匹配：`/home/user/public` 的迭代器是 `["/", "home", "user", "public"]`，精确比对每一段。

**第三层：目录 index.html 后再次验证**

```cpp
if (fs::is_directory(target))
{
    target /= "index.html";
    target = fs::canonical(target, ec2);   // 重新规范化
    if (ec2 || !detail::isSafePath(root, target))  // 再次检查
        return HttpResponse::notFound();
}
```

如果 `index.html` 是指向 root 外的符号链接，第二次 canonical + isSafePath 会拦截。

### 4.4 ETag 缓存验证

```cpp
// 生成 ETag：文件大小 + 最后修改时间
std::string etag = detail::makeEtag(fileSize, lastWrite);
// 格式: "12345-1709012345000000000"

// 客户端缓存命中时返回 304
std::string ifNoneMatch = req.header("If-None-Match");
if (!ifNoneMatch.empty() && ifNoneMatch == etag)
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hNotModified);
    res.setHeader("ETag", etag);
    res.native().prepare_payload();  // Content-Length: 0
    return res;
}
```

**ETag vs 文件内容哈希**：

使用文件大小 + 修改时间组合作为 ETag，而非读取文件内容计算 MD5/SHA。这样在 304 场景下完全不需要读文件内容，性能更优。缺点是覆盖式写入（大小和时间碰巧相同）可能导致误判，但实际场景中极罕见。

### 4.5 MIME 类型检测

```cpp
static const std::unordered_map<std::string, std::string> table = {
    {".html", "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".svg",  "image/svg+xml"},
    {".woff2","font/woff2"},
    {".pdf",  "application/pdf"},
    // ... 共 26 种
};
```

文本类型自动附加 `charset=utf-8`，二进制类型不附加。未知扩展名返回 `application/octet-stream`。

### 4.6 竞态条件处理

```cpp
std::string content(fileSize, '\0');
ifs.read(content.data(), static_cast<std::streamsize>(fileSize));
auto bytesRead = ifs.gcount();

// 文件在 file_size() 和 read() 之间被截短
if (bytesRead <= 0)
    return HttpResponse::serverError();
if (static_cast<std::uintmax_t>(bytesRead) < fileSize)
    content.resize(static_cast<std::size_t>(bytesRead));
```

在 `file_size()` 和 `read()` 之间，文件可能被其他进程截短。代码检测实际读取字节数，截断到实际长度而非返回零填充内容。

---

## 五、Multipart — RFC 7578 文件上传

### 5.1 MultipartPart 结构体

**源码位置**：`src/core/Multipart.h`

```cpp
struct MultipartPart
{
    std::unordered_map<std::string, std::string> headers; // 键已转小写
    std::string name;         // form 字段名
    std::string filename;     // 上传文件名（无则为空）
    std::string contentType;  // Part 的 Content-Type
    std::string data;         // 数据（文件内容或字段值）

    bool isFile() const { return !filename.empty(); }
};
```

### 5.2 MultipartParser 静态 API

```cpp
class MultipartParser
{
public:
    // 解析全部 Part
    static std::optional<std::vector<MultipartPart>> parse(const HttpRequest& req);
    // 便捷方法：按字段名获取文件
    static std::optional<MultipartPart> getFile(const HttpRequest& req,
                                                 const std::string& fieldName);
    // 便捷方法：按字段名获取文本字段
    static std::optional<std::string> getField(const HttpRequest& req,
                                                const std::string& fieldName);
};
```

### 5.3 解析流程

一个典型的 multipart/form-data 请求体：

```
Content-Type: multipart/form-data; boundary=----WebKitFormBoundary

------WebKitFormBoundary\r\n
Content-Disposition: form-data; name="username"\r\n
\r\n
alice\r\n
------WebKitFormBoundary\r\n
Content-Disposition: form-data; name="avatar"; filename="photo.jpg"\r\n
Content-Type: image/jpeg\r\n
\r\n
<二进制文件内容>\r\n
------WebKitFormBoundary--\r\n
```

**解析步骤**：

```
1. 检查 Content-Type 是否包含 "multipart/form-data"
2. 提取 boundary（限 70 字符，RFC 2046）
3. 按 "--" + boundary 分割 body
4. 每个 Part：
   a. 找 "\r\n\r\n" 分割头部和数据
   b. 解析头部（转小写存储）
   c. 从 Content-Disposition 提取 name/filename
5. 检查 Part 数量 ≤ 256
```

### 5.4 Boundary 提取与验证

```cpp
std::string MultipartParser::extractBoundary(const std::string& contentType)
{
    auto pos = contentType.find("boundary=");
    if (pos == std::string::npos) return "";
    pos += 9;

    std::string_view rest(contentType.c_str() + pos, contentType.size() - pos);
    rest = trim(rest);
    rest = unquote(rest);  // 处理 boundary="xxx" 的引号

    auto semi = rest.find(';');
    if (semi != std::string_view::npos) rest = rest.substr(0, semi);

    auto boundary = std::string(trim(rest));
    if (boundary.size() > 70) return "";  // RFC 2046 限制
    return boundary;
}
```

### 5.5 DoS 防护：256 Part 限制

```cpp
static constexpr std::size_t hMaxMultipartParts = 256;
if (parts.size() >= hMaxMultipartParts)
{
    return std::nullopt;
}
```

**为什么需要这个限制？**

`maxBodySize`（默认 1MB）限制了请求体总大小，但攻击者可以在 1MB 内构造数千个只有 1 字节数据的 Part。每个 Part 需要解析头部、创建 `MultipartPart` 对象，大量小 Part 会消耗 CPU 和内存。256 的上限在满足正常使用的同时防止滥用。

### 5.6 Header 大小写归一化

```cpp
inline std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return s;
}

// 存储时转小写
std::string key = toLower(std::string(trim(line.substr(0, colon))));
part.headers[key] = val;
```

HTTP 头是大小写不敏感的（RFC 7230），统一存为小写后查找时不需要处理 `Content-Type` vs `content-type` vs `CONTENT-TYPE`。

### 5.7 使用示例

```cpp
// 文件上传路由
server.router().post("/upload", [](const HttpRequest& req) -> HttpResponse {
    // 方式 1：获取指定文件
    auto file = MultipartParser::getFile(req, "avatar");
    if (file) {
        // file->filename  → "photo.jpg"
        // file->data      → 文件二进制内容
        // file->contentType → "image/jpeg"
        saveToFile(file->filename, file->data);
        return HttpResponse::ok("Uploaded: " + file->filename);
    }

    // 方式 2：解析全部 Part
    auto parts = MultipartParser::parse(req);
    if (parts) {
        for (const auto& part : *parts) {
            if (part.isFile()) {
                // 处理文件
            } else {
                // 处理表单字段：part.name + part.data
            }
        }
    }

    return HttpResponse::badRequest("No file uploaded");
});
```

---

## 六、四个模块的协同工作

### 6.1 完整的带登录文件管理服务

```cpp
// 全局 Session 管理器
auto sessionMgr = std::make_shared<SessionManager>();

HttpServer server(8080);

// 中间件：Session 管理
server.use(makeSessionMiddleware(sessionMgr));

// 登录
server.router().post("/login", [](const HttpRequest& req) -> HttpResponse {
    auto body = req.body();  // 简单示例：body = "username=alice"
    auto session = req.getAttribute<std::shared_ptr<Session>>(
        SessionManager::hSessionKey);
    if (session) {
        (*session)->set("user", std::string("alice"));
        return HttpResponse::ok("Login OK");
    }
    return HttpResponse::serverError();
});

// 文件上传（需登录）
server.router().post("/upload", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<Session>>(
        SessionManager::hSessionKey);
    if (!session || !(*session)->has("user"))
        return HttpResponse::unauthorized("Please login");

    auto file = MultipartParser::getFile(req, "file");
    if (!file)
        return HttpResponse::badRequest("No file");

    // 保存文件...
    return HttpResponse::ok("Uploaded: " + file->filename);
});

// 静态文件下载
server.router().get("/files/{path}", serveStatic("./uploads", "/files/"));

// 登出
server.router().post("/logout", [&sessionMgr](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<Session>>(
        SessionManager::hSessionKey);
    if (session) {
        sessionMgr->destroy((*session)->id());
    }
    HttpResponse res = HttpResponse::ok("Logged out");
    CookieOptions opts;
    opts.maxAge = 0;  // 清除 Cookie
    res.setCookie("HICAL_SESSION", "", opts);
    return res;
});

server.start();
```

### 6.2 模块间的依赖关系

```
Cookie（底层基础）
   │
   └── Session 中间件依赖 Cookie 读写 Session ID
          │
          ├── 文件上传路由依赖 Session 做鉴权
          │
          └── 静态文件路由可不依赖 Session（公开资源）

Multipart（独立模块）
   │
   └── 只依赖 HttpRequest 的 body() 和 contentType()
```

---

## 七、从测试看关键行为

### 7.1 Cookie 测试（test_cookie.cpp）

| 测试                  | 验证点                                      |
| --------------------- | ------------------------------------------- |
| `ParseSingle`         | 单个 Cookie 解析                            |
| `ParseMultiple`       | 多个 Cookie 分号分隔                        |
| `ParseWithSpaces`     | Cookie 值含空格                             |
| `CacheParsing`        | 惰性解析结果缓存，第二次不重复解析          |
| `SetSimple`           | 基本 Set-Cookie 生成                        |
| `SetWithOptions`      | MaxAge/HttpOnly/Secure/SameSite/Domain 组合 |
| `SetMultiple`         | 同一响应多个 Set-Cookie                     |
| `CRLFInjection`       | name/value 含 `\r\n` 被拒绝                 |
| `DuplicateFirstWins`  | 同名 Cookie 首胜策略                        |
| `SpecialCharEncoding` | 非安全字符百分号编码                        |

### 7.2 Session 测试（test_session.cpp）

| 测试                | 验证点                              |
| ------------------- | ----------------------------------- |
| `BasicSetGet`       | set/get 基本操作                    |
| `TypeSafety`        | get<int> 拿 string 返回 nullopt     |
| `KeyExistence`      | has/remove/clear                    |
| `DirtyFlag`         | set() 后 isDirty() = true           |
| `UniqueIds`         | 100 个 Session ID 全部唯一          |
| `SessionExpiration` | maxAge=1 后 Session 过期            |
| `LazyGC`            | gcInterval 到期后 create() 触发清理 |
| `ThreadSafety`      | 多线程并发 create + get             |

### 7.3 StaticFiles 测试（test_static_files.cpp）

| 测试                    | 验证点                       |
| ----------------------- | ---------------------------- |
| `MimeTypes`             | 各扩展名 MIME 正确           |
| `ServeHtml/Css/Js/Json` | 文件内容和 Content-Type 正确 |
| `DirectoryIndex`        | 目录自动返回 index.html      |
| `EtagPresent`           | 响应包含 ETag 头             |
| `NotModified304`        | If-None-Match 匹配返回 304   |
| `PathTraversal`         | `../` 攻击返回 403 或 404    |
| `InvalidRoot`           | 不存在的根目录返回 404       |
| `LargeFileRejection`    | 超限返回 413                 |

### 7.4 Multipart 测试（test_multipart.cpp）

| 测试                      | 验证点                             |
| ------------------------- | ---------------------------------- |
| `ParseTextField`          | 文本字段解析                       |
| `ParseFileUpload`         | 文件上传解析（name/filename/data） |
| `MixedFieldsAndFiles`     | 混合表单字段和文件                 |
| `WrongContentType`        | 非 multipart 请求返回 nullopt      |
| `MissingBoundary`         | 缺少 boundary 返回 nullopt         |
| `EmptyBody`               | 空 body 返回 nullopt               |
| `GetField/GetFile`        | 便捷方法正确工作                   |
| `PartCountLimit`          | 超过 256 Part 返回 nullopt         |
| `HeaderCaseNormalization` | 头部键统一小写                     |

---

## 八、与游戏服务器架构的对比

| Hical 概念                   | 游戏服务器等价物                          |
| ---------------------------- | ----------------------------------------- |
| Cookie                       | 客户端本地缓存的登录 Token                |
| Session（内存存储）          | 玩家在线会话（PlayerSession），存角色数据 |
| Session dirty flag           | 玩家数据变更标记，决定是否需要存盘        |
| Session GC                   | 定期清理断线超时的玩家会话                |
| `generateId()` 128-bit       | 玩家 Session Key 生成（防猜测）           |
| `serveStatic()` 路径穿越防护 | 补丁下载服务防止读取服务器敏感文件        |
| `setCookie()` CRLF 防护      | 消息包字段注入防护                        |
| Multipart 256 Part 限制      | 背包/邮件操作的批量数量上限               |
| ETag 304                     | 资源版本号比对，避免重复下载              |

---

## 九、关键问题思考与回答

**Q1: Cookie 为什么采用惰性解析？**

> 不是所有请求都需要 Cookie。`GET /api/health` 这样的路由不关心 Cookie，预先解析是浪费。惰性解析确保只有实际调用 `req.cookie()` 时才执行字符串解析。结果缓存到 `mutable std::optional` 中，后续访问直接查 map。

**Q2: CRLF 注入攻击的原理？**

> HTTP 头用 `\r\n` 分隔各行。如果 Cookie 值包含 `\r\n`，攻击者可以注入额外的 HTTP 头甚至伪造响应体：
> ```
> Set-Cookie: name=value\r\nX-Injected: malicious\r\n\r\n<html>fake body</html>
> ```
> `setCookie()` 检测到 `\r` 或 `\n` 直接拒绝，从根源阻断。

**Q3: 128-bit Session ID 够安全吗？**

> 128-bit = 2^128 种可能 ≈ 3.4 × 10^38。假设攻击者每秒尝试 10 亿次，需要约 10^22 年才能遍历。实际中搭配 maxAge 过期、httpOnly、sameSite 等措施，安全性足够。比较：UUID v4 也是 128-bit。

**Q4: std::any vs std::variant vs void***

> - `std::variant<string, int, double>`：编译期确定所有类型，Session 存储的业务类型不可预知
> - `void*`：丢失类型信息，取值时无法校验类型，容易 crash
> - `std::any`：运行时类型擦除 + 类型安全的 `any_cast`，`get<T>()` 类型不匹配返回 nullopt

**Q5: 为什么路径安全检查用迭代器比较而非字符串前缀？**

> 字符串前缀 `"/pub".starts_with("/pub")` 会误匹配 `"/public"`。路径迭代器逐段比较：
> - root = `["/", "home", "user", "public"]`
> - target = `["/", "home", "user", "public", "css", "style.css"]`
> - 每段精确匹配，不会被子串混淆

**Q6: 为什么限制 256 个 Multipart Part？**

> `maxBodySize`（1MB）只限总大小。攻击者可以构造 1MB 内的上万个小 Part，每个只有几字节数据但都要解析头部、创建对象。256 的上限在允许正常表单（通常 < 20 个字段）的同时，防止 CPU/内存被大量空 Part 耗尽。

**Q7: Session dirty flag 优化了什么？**

> 如果每个请求都刷新 Set-Cookie：
> 1. 响应头多了约 100 字节（Cookie 名 + 32 字符 ID + 各属性）
> 2. 浏览器每次都要写入 Cookie 存储
> 3. CDN/代理无法缓存带 Set-Cookie 的响应
>
> dirty flag 确保只在 Session 数据变更时才写 Cookie，大多数读请求的响应更轻量。

---

## 十、从第0课到第9课的知识脉络

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
Cookie / Session / 文件服务（第9课） ← 你在这里
    Cookie 解析/设置 → Session 中间件 → StaticFiles → Multipart
         │
C++26 反射层（第10课）
    Reflection.h / MetaJson.h / MetaRoutes.h
```

---

## 十一、课程总结

本课学习了 hical Web 框架"最后一公里"的四个模块：

| 模块            | 核心收获                                                                                     |
| --------------- | -------------------------------------------------------------------------------------------- |
| **Cookie**      | 惰性解析 + 结果缓存、RFC 6265 编码、CRLF 注入防护、首胜策略、insert 多值                     |
| **Session**     | `std::any` 类型安全存储、128-bit 安全 ID、惰性 GC、dirty flag 优化、中间件洋葱模型集成       |
| **StaticFiles** | canonical + 迭代器路径穿越防护、ETag 304 缓存验证、MIME 自动检测、竞态条件处理、大文件限制   |
| **Multipart**   | RFC 7578 boundary 解析、256 Part DoS 防护、Header 大小写归一化、便捷 API（getFile/getField） |

**安全意识总结**：每个模块都内置了对应的防护——这不是事后补丁，而是设计时就考虑的安全边界。

**接下来进入第10课 C++26 反射与自动化**，学习反射驱动的自动路由注册和 JSON 序列化。

---

*上一课：[第8课 - HttpServer 整合]({{< relref "Hical-08-notes" >}}) | 下一课：[第10课 - C++26 反射与自动化]({{< relref "Hical-10-notes" >}})*
