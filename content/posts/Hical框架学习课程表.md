+++
title = 'Hical 框架学习课程表'
date = '2026-04-15'
draft = false
tags = ["C++20", "Hical", "学习路线", "Web框架", "课程"]
categories = ["Hical框架"]
description = "从零开始系统掌握 Hical 现代 C++20 高性能 Web 框架的设计与实现，涵盖 13 节课 + 2 个综合项目。"
+++

# Hical 框架学习课程表

> 从零开始，系统掌握一个现代 C++20 高性能 Web 框架的设计与实现。

## 课程概览

| 阶段   | 课程   | 主题                       | 预计时长 |
| ------ | ------ | -------------------------- | -------- |
| 预备   | 第0课  | C++20 核心特性速览         | 2~3h     |
| 基础层 | 第1课  | 抽象接口与 Concepts 设计   | 2~3h     |
| 基础层 | 第2课  | 错误处理与网络地址         | 1~2h     |
| 实现层 | 第3课  | Asio 事件循环与定时器      | 3~4h     |
| 实现层 | 第4课  | PMR 内存管理               | 3~4h     |
| 网络层 | 第5课  | TCP 连接与服务器           | 3~4h     |
| 协议层 | 第6课  | HTTP 协议与路由            | 3~4h     |
| 协议层 | 第7课  | 中间件与 WebSocket         | 2~3h     |
| 应用层 | 第8课  | HttpServer 整合            | 2~3h     |
| 应用层 | 第9课  | Cookie、Session 与文件服务 | 3~4h     |
| 反射层 | 第10课 | C++26 反射与自动化         | 3~4h     |
| 综合   | 项目A  | 性能压测与分析             | 3~4h     |
| 综合   | 项目B  | 动手扩展新功能             | 4~6h     |

---

## 第0课：C++20 核心特性速览

### 学习目标
- 理解 Concepts 的语法与用途（`requires` 表达式、`concept` 定义）
- 理解 Coroutines 的基本模型（`co_await`、`co_return`、Promise Type）
- 理解 PMR（Polymorphic Memory Resource）的分配器模型
- 了解 `if constexpr` 在模板中的编译期分支

### 需要掌握的前置知识
- C++ 模板基础（函数模板、类模板、模板特化）
- 智能指针（`shared_ptr`、`unique_ptr`、`enable_shared_from_this`）
- Lambda 表达式与 `std::function`
- 基本的网络编程概念（TCP/IP、Socket）

### 推荐学习资源
- [cppreference - Concepts](https://en.cppreference.com/w/cpp/language/constraints)
- [cppreference - Coroutines](https://en.cppreference.com/w/cpp/language/coroutines)
- [cppreference - PMR](https://en.cppreference.com/w/cpp/memory/polymorphic_allocator)
- Boost.Asio 官方文档中的 Coroutine 示例

### 动手练习
1. 写一个 `concept Printable`，约束类型必须支持 `operator<<`
2. 写一个最简协程，使用 `co_await` 挂起并恢复
3. 用 `std::pmr::monotonic_buffer_resource` 分配一组对象，观察内存行为

---

## 第1课：抽象接口与 Concepts 设计

### 学习目标
- 理解两层架构设计：`core`（抽象）与 `asio`（实现）的分离思想
- 掌握 `EventLoop`、`Timer`、`TcpConnection` 三大核心接口的职责
- 理解 `NetworkBackend` Concept 如何在编译期约束后端实现

### 核心阅读清单

| 文件                       | 重点关注                                                                    |
| -------------------------- | --------------------------------------------------------------------------- |
| `src/core/EventLoop.h`     | 纯虚接口设计：run/stop、post/dispatch、runAfter/runEvery                    |
| `src/core/Timer.h`         | 定时器抽象：cancel、isActive、isRepeating                                   |
| `src/core/TcpConnection.h` | 连接抽象：send 多重载、回调链（onMessage/onClose）、用户上下文              |
| `src/core/Concepts.h`      | C++20 Concepts：EventLoopLike、TcpConnectionLike、TimerLike、NetworkBackend |
| `src/core/Coroutine.h`     | 协程工具：Awaitable\<T\> 别名、sleepFor、coSpawn                            |

### 配套测试
- `tests/test_basic.cpp` — 基础编译验证
- `tests/test_concepts.cpp` — Concepts 编译期约束验证

### 关键问题思考
1. 为什么要用纯虚接口而不是直接用 Boost.Asio 的类型？
2. `Concepts` 和传统的虚函数继承各有什么优劣？
3. `TcpConnection` 为什么继承 `enable_shared_from_this`？
4. `Awaitable<T>` 为什么只是一个类型别名而非自定义类？

### 动手练习
1. 画出三大接口（EventLoop、Timer、TcpConnection）的方法清单脑图
2. 阅读 `Concepts.h`，尝试写一个不满足 `EventLoopLike` 的类，观察编译报错
3. 运行 `test_concepts` 测试，确认所有 Concept 检查通过

---

## 第2课：错误处理与网络地址

### 学习目标
- 理解框架如何将 Boost 错误码映射为统一的 `ErrorCode` 枚举
- 掌握 `InetAddress` 对 IPv4/IPv6 地址的封装
- 理解 HTTP 类型枚举的设计

### 核心阅读清单

| 文件                       | 重点关注                                            |
| -------------------------- | --------------------------------------------------- |
| `src/core/Error.h`         | ErrorCode 枚举（25+ 错误类型）、NetworkError 结构体 |
| `src/core/Error.cpp`       | fromBoostError 映射逻辑、errorCodeToString          |
| `src/core/InetAddress.h`   | IPv4/IPv6 构造、toIp/toIpPort 转换                  |
| `src/core/InetAddress.cpp` | sockaddr_in/sockaddr_in6 底层操作                   |
| `src/core/HttpTypes.h`     | HttpMethod 与 HttpStatusCode 枚举                   |

### 配套测试
- `tests/test_error.cpp` — 错误码转换测试
- `tests/test_http_types.cpp` — HTTP 枚举双向转换测试

### 关键问题思考
1. 为什么不直接暴露 `boost::system::error_code` 给上层？
2. `NetworkError` 的 `ok()`、`isEof()`、`isCancelled()` 方法在哪些场景下使用？
3. `InetAddress` 为什么要同时支持 `sockaddr_in` 和 `sockaddr_in6`？

### 动手练习
1. 在 `test_error.cpp` 中新增一个测试用例，验证 SSL 相关错误码的映射
2. 用 `InetAddress` 构造一个 IPv6 地址，打印 `toIpPort()` 的输出
3. 运行 `test_error` 和 `test_http_types` 测试

---

## 第3课：Asio 事件循环与定时器

### 学习目标
- 理解 `AsioEventLoop` 如何封装 `boost::asio::io_context`
- 掌握「1线程:1事件循环」的线程模型
- 理解 `EventLoopPool` 的 round-robin 连接分发机制
- 掌握 `AsioTimer` 基于 `steady_timer` 的实现

### 核心阅读清单

| 文件                         | 重点关注                                                  |
| ---------------------------- | --------------------------------------------------------- |
| `src/asio/AsioEventLoop.h`   | io_context 封装、work guard 防止退出、定时器注册表        |
| `src/asio/AsioEventLoop.cpp` | run/stop 实现、post/dispatch 转发、runAfter/runEvery 逻辑 |
| `src/asio/AsioTimer.h`       | steady_timer 封装、单次/重复模式                          |
| `src/asio/AsioTimer.cpp`     | scheduleOnce/scheduleRepeating、handleTimeout 回调        |
| `src/asio/EventLoopPool.h`   | 线程池管理、round-robin nextIndex_                        |
| `src/asio/EventLoopPool.cpp` | start 启动线程、stop 优雅关闭、getNextLoop 分发           |

### 配套测试与示例
- `tests/test_asio_event_loop.cpp` — 事件循环功能测试
- `tests/test_asio_timer.cpp` — 定时器功能测试
- `examples/echo_server.cpp` — 纯 Asio 协程回声服务器

### 关键问题思考
1. `work_guard` 的作用是什么？如果不用会怎样？
2. `post()` 和 `dispatch()` 的区别在哪里？什么时候用哪个？
3. `EventLoopPool` 为什么采用 round-robin 而不是最少连接数？
4. 定时器回调中如何处理 `operation_aborted` 错误码？

### 动手练习
1. 编译运行 `echo_server` 示例，用 `telnet` 或 `nc` 连接测试
2. 修改 `echo_server`，添加一个每 5 秒打印连接数的定时器
3. 写一段代码创建 `EventLoopPool`（4线程），分别向不同 loop 投递任务
4. 运行 `test_asio_event_loop` 和 `test_asio_timer` 测试

---

## 第4课：PMR 内存管理

### 学习目标
- 理解三级内存池架构：全局同步池 → 线程局部池 → 请求级单调缓冲区
- 掌握 `TrackedResource` 零开销内存统计的实现
- 理解 `PmrBuffer` 的 prepend 区域设计与读写接口

### 核心阅读清单

| 文件                    | 重点关注                                                      |
| ----------------------- | ------------------------------------------------------------- |
| `src/core/MemoryPool.h` | 三级策略、PoolConfig 配置、单例模式、线程安全设计             |
| `src/core/PmrBuffer.h`  | prepend 区域（8字节）、读写指针模型、ensureWritableBytes 扩容 |

### 配套测试与示例
- `tests/test_memory_pool.cpp` — PMR 分配/回收、线程局部行为、PmrBuffer 操作
- `examples/pmr_poc.cpp` — 四种内存场景对比验证
- `examples/pmr_benchmark.cpp` — 分配策略性能基准

### 关键问题思考
1. 为什么需要三级内存池？每级分别解决什么问题？
2. `monotonic_buffer_resource` 为什么适合请求级分配？
3. `PmrBuffer` 的 prepend 区域有什么实际用途？（提示：网络协议头）
4. 线程局部池为什么不需要加锁？

### 动手练习
1. 编译运行 `pmr_poc` 示例，观察四种场景的输出
2. 运行 `pmr_benchmark`，对比不同分配策略的性能差异
3. 写一段代码：用 `PmrBuffer` 先写入消息体，再 prepend 4 字节长度头
4. 运行 `test_memory_pool` 测试

---

## 第5课：TCP 连接与服务器

### 学习目标
- 理解 `GenericConnection` 模板如何用 `if constexpr` 统一 TCP 和 SSL 连接
- 掌握连接状态机：Connecting → Connected → Disconnecting → Disconnected
- 理解 `TcpServer` 的协程 accept 循环与连接管理
- 了解 SSL/TLS 握手在框架中的集成方式

### 核心阅读清单

| 文件                             | 重点关注                                                |
| -------------------------------- | ------------------------------------------------------- |
| `src/asio/GenericConnection.h`   | 模板参数 SocketType、IsSslStream 类型萃取、状态枚举     |
| `src/asio/GenericConnection.cpp` | async_readLoop/async_writeLoop 协程、SSL 握手、回调触发 |
| `src/asio/TcpServer.h`           | 监听/接受/分发、SSL 启用、连接跟踪                      |
| `src/asio/TcpServer.cpp`         | acceptLoop 协程、连接生命周期管理                       |
| `src/core/SslContext.h`          | SSL 上下文配置、证书加载                                |
| `src/core/SslContext.cpp`        | loadCertificate/loadPrivateKey 实现                     |

### 配套测试
- `tests/test_asio_tcp_connection.cpp` — 连接接口测试
- `tests/test_tcp_server.cpp` — 服务器集成测试
- `tests/test_ssl_connection.cpp` — SSL 连接测试

### 关键问题思考
1. `GenericConnection` 为什么用模板而不是运行时多态？
2. `if constexpr (hIsSslStream<SocketType>)` 相比虚函数有什么优势？
3. 连接的读写循环为什么用协程而不是回调？
4. `TcpServer` 如何通过 `EventLoopPool` 实现多线程处理？
5. 高水位线回调（`onHighWaterMark`）解决什么问题？

### 动手练习
1. 阅读 `GenericConnection.cpp`，画出读写协程的完整流程图
2. 用 `TcpServer` 写一个简单的 TCP 回声服务器（不用 HTTP）
3. 修改回声服务器，添加 SSL 支持（可以用自签名证书）
4. 运行 `test_tcp_server` 和 `test_ssl_connection` 测试

---

## 第6课：HTTP 协议与路由

### 学习目标
- 理解 `HttpRequest` / `HttpResponse` 对 Boost.Beast 的封装
- 掌握 Router 的双策略设计：O(1) 静态路由 + 参数路由
- 理解路由参数提取与安全限制

### 核心阅读清单

| 文件                        | 重点关注                                               |
| --------------------------- | ------------------------------------------------------ |
| `src/core/HttpRequest.h`    | Beast request 封装、method/path/query/body/jsonBody    |
| `src/core/HttpRequest.cpp`  | 构建器模式：setMethod/setTarget/addHeader/setBody      |
| `src/core/HttpResponse.h`   | 工厂方法：ok()/json()/notFound()/internalServerError() |
| `src/core/HttpResponse.cpp` | setJsonBody、状态码设置                                |
| `src/core/Router.h`         | 路由键 SRouteKey、静态路由哈希表、参数路由线性扫描     |
| `src/core/Router.cpp`       | dispatch 逻辑、路径参数提取、安全限制（32段/1024字符） |

### 配套测试
- `tests/test_router.cpp` — 路由匹配全面测试
- `tests/test_router_perf.cpp` — 路由性能基准（P50/P99/P999）

### 关键问题思考
1. 静态路由用哈希表，为什么参数路由不也用 Trie 树？
2. `HttpResponse::json()` 工厂方法的设计有什么好处？
3. 路由为什么要限制最大 32 段和每段 1024 字符？
4. 同步处理函数是如何被自动包装成异步的？

### 动手练习
1. 运行 `test_router_perf`，记录 1000 个路由下的 P99 延迟
2. 给 Router 添加 3 条路由（GET、POST、带参数），写单元测试验证
3. 尝试构造一个超长路径，观察安全限制是否生效
4. 运行 `test_router` 测试

---

## 第7课：中间件与 WebSocket

### 学习目标
- 理解洋葱模型（Onion Model）中间件管道的执行顺序
- 掌握 `MiddlewareNext` 链式调用机制
- 理解 WebSocket 会话的升级、消息收发与生命周期

### 核心阅读清单

| 文件                      | 重点关注                                        |
| ------------------------- | ----------------------------------------------- |
| `src/core/Middleware.h`   | MiddlewareHandler 签名、MiddlewarePipeline 管理 |
| `src/core/Middleware.cpp` | execute 递归构建调用链、洋葱模型实现            |
| `src/core/WebSocket.h`    | Beast websocket::stream 封装、send/receive 接口 |
| `src/core/WebSocket.cpp`  | 异步消息收发、close 与 isOpen                   |

### 配套测试
- `tests/test_middleware.cpp` — 中间件执行顺序、拦截、链式调用
- `tests/test_websocket.cpp` — WebSocket 升级、消息收发

### 关键问题思考
1. 洋葱模型中，中间件 A → B → C 的 pre/post 执行顺序是什么？
2. 中间件如何实现「提前返回」（不调用 next）？
3. WebSocket 的 `receive()` 返回 `std::optional` 的设计意图是什么？
4. HTTP 到 WebSocket 的升级过程发生在哪一层？

### 动手练习
1. 写三个中间件（日志、鉴权、限流），在每个的 pre/post 阶段打印日志，验证执行顺序
2. 写一个鉴权中间件，检查 Header 中的 Token，无效时直接返回 401
3. 阅读 `examples/http_server.cpp` 中的 WebSocket 路由部分
4. 运行 `test_middleware` 和 `test_websocket` 测试

---

## 第8课：HttpServer 整合

### 学习目标
- 理解 `HttpServer` 如何整合 TcpServer + Router + MiddlewarePipeline
- 掌握完整的 HTTP 请求生命周期：接收 → 解析 → 路由 → 中间件 → 处理 → 响应
- 了解 SSL 启用方式与配置

### 核心阅读清单

| 文件                       | 重点关注                                         |
| -------------------------- | ------------------------------------------------ |
| `src/core/HttpServer.h`    | 高层门面：router()、use()、enableSsl、start/stop |
| `src/core/HttpServer.cpp`  | acceptLoop、session 协程、HTTP/WebSocket 分流    |
| `examples/http_server.cpp` | 完整示例：中间件 + 多路由 + WebSocket            |

### 配套测试
- `tests/test_http_server.cpp` — HTTP 服务器端到端测试
- `tests/test_integration.cpp` — 全系统集成测试

### 关键问题思考
1. `HttpServer` 的 `start()` 是阻塞的，框架内部如何处理并发请求？
2. HTTP 请求和 WebSocket 升级请求是在哪里分流的？
3. `setMaxBodySize` 和 `setMaxHeaderSize` 解决什么安全问题？
4. 如果想支持 HTTP/2，架构上需要改动哪些部分？

### 动手练习
1. 编译运行 `examples/http_server.cpp`，用 curl 测试所有路由：
   ```bash
   curl http://localhost:8080/
   curl http://localhost:8080/api/status
   curl -X POST -d '{"msg":"hi"}' http://localhost:8080/api/echo
   curl http://localhost:8080/users/42
   ```
2. 给示例添加一个 `GET /api/time` 路由，返回当前时间的 JSON
3. 添加一个 CORS 中间件，为所有响应添加跨域头
4. 运行 `test_http_server` 和 `test_integration` 测试

---

## 第9课：Cookie、Session 与文件服务

### 学习目标
- 理解 HTTP Cookie 的解析与设置机制（RFC 6265）
- 掌握服务端 Session 的生命周期管理（创建、查找、过期、GC）
- 理解 `serveStatic()` 静态文件服务的安全防护（路径穿越、ETag 缓存验证）
- 掌握 `MultipartParser` 对 RFC 7578 multipart/form-data 的解析
- 理解这四个模块如何通过中间件管道协同工作

### 核心阅读清单

| 文件                        | 重点关注                                                                           |
| --------------------------- | ---------------------------------------------------------------------------------- |
| `src/core/Cookie.h`         | CookieOptions 结构体：Path/Domain/MaxAge/HttpOnly/Secure/SameSite                  |
| `src/core/HttpRequest.cpp`  | Cookie 惰性解析：`parseCookies()` 首次访问触发、结果缓存                           |
| `src/core/HttpResponse.cpp` | `setCookie()` 实现：RFC 6265 编码、CRLF 注入防护                                   |
| `src/core/Session.h`        | Session 类（`std::any` 类型安全存储）、SessionManager（128-bit ID 生成、惰性 GC）  |
| `src/core/Session.cpp`      | `generateId()` 安全随机数、`find()` 过期检查、`gc()` 清理逻辑                      |
| `src/core/StaticFiles.h`    | `serveStatic()` 工厂函数：canonical 路径验证、ETag 304、MIME 检测、目录 index.html |
| `src/core/Multipart.h`      | MultipartPart 结构体、MultipartParser 静态 API                                     |
| `src/core/Multipart.cpp`    | RFC 7578 解析：boundary 提取、Part 头解析、256 Part DoS 限制                       |

### 配套测试
- `tests/test_cookie.cpp` — Cookie 解析与设置（惰性缓存、RFC 编码、CRLF 注入防护、重复 Cookie 首胜策略）
- `tests/test_session.cpp` — Session 生命周期（类型安全存取、dirty flag、128-bit ID 唯一性、过期淘汰、线程安全）
- `tests/test_static_files.cpp` — 静态文件服务（MIME 检测、ETag 304、路径穿越防护、大文件 413、目录 index.html）
- `tests/test_multipart.cpp` — Multipart 解析（文本字段、文件上传、混合解析、256 Part DoS 限制、Header 大小写归一化）

### 关键问题思考
1. Cookie 为什么采用惰性解析（首次访问才解析）？这对性能有什么影响？
2. `setCookie()` 为什么要检查 `\r\n` 字符？CRLF 注入攻击的原理是什么？
3. Session 的 `generateId()` 为什么用 `thread_local std::mt19937_64`？128-bit 够不够安全？
4. Session 为什么用 `std::any` 而不是 `std::variant` 或 `void*` 存储数据？
5. `serveStatic()` 的路径安全检查为什么要用 `std::filesystem::canonical()` + 迭代器比较，而不是简单的字符串前缀匹配？
6. Multipart 为什么要限制 256 个 Part？如果没有这个限制，攻击者可以怎样利用？
7. Session 中间件的 dirty flag 优化了什么？如果每次都写 Set-Cookie 有什么代价？

### 动手练习
1. 阅读 `test_cookie.cpp`，理解 CRLF 注入测试用例，尝试手动构造恶意 Cookie 值
2. 运行 `test_session`，修改 `SessionOptions::maxAge` 为 1 秒，观察 Session 过期行为
3. 在本地创建一个测试目录（放入 HTML/CSS/JS 文件），用 `serveStatic()` 挂载并用浏览器访问，验证 ETag 304 行为（F5 刷新 vs Ctrl+F5 强刷）
4. 用 curl 上传一个文件，断点调试 `MultipartParser::parse()` 的解析过程：
   ```bash
   curl -X POST -F "file=@test.txt" -F "desc=hello" http://localhost:8080/upload
   ```
5. 综合练习：写一个带登录的文件管理服务（Session 鉴权 + 静态文件 + 文件上传），验证四个模块的协同工作
6. 运行 `test_cookie`、`test_session`、`test_static_files`、`test_multipart` 全部测试

---

## 第10课：C++26 反射与自动化

### 学习目标
- 理解 C++26 反射（P2996）的基本概念：`^^T`、`[:..:]`、`std::meta::info`
- 掌握 Hical 的双路线策略：C++26 反射 + C++20 宏回退，相同用户 API
- 理解自动 JSON 序列化/反序列化的实现原理（`toJson` / `fromJson`）
- 掌握反射驱动的自动路由注册（`registerRoutes`）
- 理解条件编译检测机制（`HICAL_HAS_REFLECTION`）

### 核心阅读清单

| 文件                             | 重点关注                                                                                                           |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| `src/core/Reflection.h`          | 反射检测宏 `HICAL_HAS_REFLECTION`、`RouteInfo` 类型、`HasJsonFields` / `HasRouteTable` 编译期检测                  |
| `src/core/MetaJson.h`            | `toJson` / `fromJson` 双路线实现、`valueToJson` / `valueFromJson` 类型分发、`HICAL_JSON` 宏、`readJson<T>()`       |
| `src/core/MetaRoutes.h`          | `registerRoutes` 双路线实现、`HICAL_HANDLER` / `HICAL_ROUTES` 宏、`RouteRegistrar` 模板、`shared_ptr` 生命周期管理 |
| `examples/reflection_server.cpp` | 完整示例：DTO 自动序列化 + Handler 自动路由注册                                                                    |

### 配套测试
- `tests/test_reflection.cpp` — 17 个测试覆盖 JSON 序列化、路由注册、类型检测

### 关键问题思考
1. 为什么用条件编译双路线而不是只支持 C++26？
2. `HICAL_JSON` 宏的 FOR_EACH 展开是如何实现可变参数的？
3. `registerRoutes` 为什么用 `shared_ptr` 管理 handler 而不是引用？
4. `valueFromJson` 中的类型检查为什么重要？（提示：恶意 JSON 输入）
5. C++26 反射路线中 `template for` 循环是什么含义？

### 动手练习
1. 定义一个 `ProductDTO`（name, price, stock），使用 `HICAL_JSON` 宏标注，测试序列化/反序列化
2. 定义一个 `ProductHandler`，包含 CRUD 四个路由，使用 `HICAL_HANDLER` / `HICAL_ROUTES` 标注，验证自动注册
3. 编译运行 `examples/reflection_server.cpp`，用 curl 测试所有路由：
   ```bash
   curl http://localhost:8080/api/status
   curl http://localhost:8080/api/users/42
   curl -X POST -H "Content-Type: application/json" -d '{"name":"test","age":20,"email":"a@b.com"}' http://localhost:8080/api/users
   ```
4. 运行 `test_reflection` 测试，确认 17 个测试全部通过

---

## 综合项目A：性能压测与分析

### 学习目标
- 学会使用框架自带的 benchmark 工具进行压力测试
- 能够分析 QPS、延迟分位数（P50/P99/P999）等性能指标
- 理解 PMR 内存池对性能的实际影响

### 实践步骤

1. **编译运行压测工具**
   - 编译 `examples/http_benchmark.cpp` 和 `examples/pmr_benchmark.cpp`
   - 先启动 `http_server` 示例，再用 `http_benchmark` 压测

2. **性能指标收集**
   - 记录不同并发数下的 QPS
   - 记录延迟分位数（P50、P99、P999）
   - 对比开启/关闭 PMR 内存池的性能差异

3. **路由性能分析**
   - 运行 `test_router_perf`，分析静态路由 vs 参数路由的性能差距
   - 尝试注册 1000/5000/10000 条路由，观察查找性能变化

4. **输出报告**
   - 整理压测数据，生成性能对比表格
   - 分析性能瓶颈所在，提出优化建议

### 参考文件
- `examples/http_benchmark.cpp`
- `examples/pmr_benchmark.cpp`
- `examples/benchmark.cpp`
- `tests/test_router_perf.cpp`

---

## 综合项目B：动手扩展新功能

### 学习目标
- 将前面所学融会贯通，独立完成一个完整功能
- 体验从接口设计到实现、测试的完整开发流程

### 可选题目（任选其一）

#### 题目1：实现 Rate Limiter 中间件
- 基于令牌桶或滑动窗口算法
- 按 IP 限流，超限返回 429 Too Many Requests
- 配置项：每秒请求数、突发容量
- 编写单元测试验证限流逻辑

#### 题目2：实现带鉴权的文件管理系统
- 基于 Session 实现用户登录/登出（POST /login、POST /logout）
- 登录后可查看文件列表（GET /files）、上传文件（POST /upload，Multipart 解析）
- 用 `serveStatic()` 托管已上传文件的下载
- 未登录访问受保护路由返回 401 Unauthorized
- 编写集成测试验证完整流程（登录 → 上传 → 下载 → 登出）

#### 题目3：实现 JSON RPC 路由扩展
- 在现有 Router 基础上扩展 JSON-RPC 2.0 支持
- 解析 `method` 字段分发到对应处理函数
- 支持批量请求
- 正确处理错误响应（-32600/-32601/-32602 等）

### 开发流程建议
1. 先在 `src/core/` 定义接口
2. 在 `src/asio/` 或 `src/core/` 实现
3. 在 `tests/` 编写单元测试
4. 在 `examples/` 编写使用示例
5. 确保所有测试通过：`ctest --test-dir build --output-on-failure`

---

## 附录：Hical框架文件速查表

### 源码文件

```
src/core/
├── Concepts.h          ← C++20 Concepts 定义
├── Coroutine.h         ← 协程工具（Awaitable、sleepFor、coSpawn）
├── Error.h/.cpp        ← 错误码映射
├── EventLoop.h         ← 事件循环接口
├── HttpRequest.h/.cpp  ← HTTP 请求封装
├── HttpResponse.h/.cpp ← HTTP 响应封装（含工厂方法）
├── HttpServer.h/.cpp   ← HTTP 服务器门面
├── HttpTypes.h         ← HTTP 方法/状态码枚举
├── InetAddress.h/.cpp  ← 网络地址封装
├── MemoryPool.h        ← 三级 PMR 内存池
├── Middleware.h/.cpp    ← 洋葱模型中间件管道
├── PmrBuffer.h         ← PMR 缓冲区
├── Reflection.h        ← C++26 反射检测与基础设施
├── MetaJson.h          ← 反射驱动 JSON 自动序列化（toJson/fromJson）
├── MetaRoutes.h        ← 反射驱动自动路由注册（registerRoutes）
├── Router.h/.cpp       ← 路由系统（静态+参数）
├── Cookie.h            ← Cookie 选项结构体（CookieOptions）
├── Session.h/.cpp      ← Session 会话管理（Session/SessionManager/中间件工厂）
├── StaticFiles.h       ← 静态文件服务（serveStatic 工厂函数，header-only）
├── Multipart.h/.cpp    ← RFC 7578 multipart/form-data 解析器
├── SslContext.h/.cpp   ← SSL/TLS 上下文
├── TcpConnection.h     ← TCP 连接接口
├── Timer.h             ← 定时器接口
└── WebSocket.h/.cpp    ← WebSocket 会话

src/asio/
├── AsioEventLoop.h/.cpp     ← io_context 封装
├── AsioTimer.h/.cpp         ← steady_timer 封装
├── EventLoopPool.h/.cpp     ← 多线程事件循环池
├── GenericConnection.h/.cpp ← 模板化 TCP/SSL 连接
└── TcpServer.h/.cpp         ← TCP 服务器
```

### 测试文件

```
tests/
├── test_basic.cpp              ← 基础编译验证
├── test_error.cpp              ← 错误码测试
├── test_concepts.cpp           ← Concepts 编译期检查
├── test_http_types.cpp         ← HTTP 枚举测试
├── test_asio_event_loop.cpp    ← 事件循环测试
├── test_asio_timer.cpp         ← 定时器测试
├── test_asio_tcp_connection.cpp← TCP 连接测试
├── test_memory_pool.cpp        ← 内存池测试
├── test_tcp_server.cpp         ← TCP 服务器测试
├── test_ssl_connection.cpp     ← SSL 连接测试
├── test_router.cpp             ← 路由测试
├── test_router_perf.cpp        ← 路由性能测试
├── test_middleware.cpp         ← 中间件测试
├── test_http_server.cpp        ← HTTP 服务器测试
├── test_websocket.cpp          ← WebSocket 测试
├── test_coroutine.cpp          ← 协程工具测试
├── test_cookie.cpp             ← Cookie 解析与设置测试
├── test_session.cpp            ← Session 生命周期与线程安全测试
├── test_static_files.cpp       ← 静态文件服务测试（ETag/路径穿越/MIME）
├── test_multipart.cpp          ← Multipart 解析测试（DoS限制/文件上传）
├── test_reflection.cpp         ← 反射层测试（JSON序列化+路由注册）
├── test_helpers.h              ← 测试辅助函数（runCoroutine）
└── test_integration.cpp        ← 全系统集成测试
```

### 示例文件

```
examples/
├── echo_server.cpp     ← 入门：纯 Asio 协程回声服务器
├── pmr_poc.cpp         ← 进阶：PMR 内存场景验证
├── benchmark.cpp       ← 工具：TCP 回声压测
├── http_server.cpp     ← 核心：完整 HTTP 服务器示例
├── http_benchmark.cpp      ← 工具：HTTP 性能压测
├── pmr_benchmark.cpp       ← 工具：内存分配策略对比
└── reflection_server.cpp   ← 反射：自动路由注册+JSON序列化示例
```

---

## 推荐学习路径

```
第0课(C++20基础) → 第1课(接口设计) → 第2课(错误/地址)
                                          ↓
          第4课(内存管理) ← 第3课(Asio实现层)
                ↓
          第5课(TCP网络层) → 第6课(HTTP/路由) → 第7课(中间件/WS)
                                                      ↓
                                                第8课(HttpServer)
                                                      ↓
                                          第9课(Cookie/Session/文件服务)
                                                      ↓
                                               第10课(C++26反射)
                                                      ↓
                                          项目A(压测) + 项目B(扩展)
```

---
## Hical 框架的 [GitHub](https://github.com/Hical61/Hical.git) 仓库


> 每完成一课，务必运行对应的测试用例确认理解正确。遇到不理解的代码，善用断点调试逐行跟踪。
