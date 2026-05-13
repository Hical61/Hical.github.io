+++
title = 'Hical VS2022 快速上手 Demo'
date = '2026-05-12'
draft = false
tags = ["C++20", "Hical", "VS2022", "vcpkg", "快速上手"]
categories = ["Hical框架"]
description = "一个最小化的 Hical HTTP Server 示例，VS2022 + vcpkg 编译即跑，含中文 JSON 响应和 WebSocket Echo。"
+++

# Hical VS2022 快速上手 Demo

一个最小化的 Hical HTTP Server 示例，VS2022 编译即可运行，访问即返回含中文的 JSON 响应。

---

## 环境要求

| 项目               | 版本要求                            |
| ------------------ | ----------------------------------- |
| Visual Studio 2022 | 17.6+ (需勾选"C++桌面开发"工作负载) |
| vcpkg              | 最新版                              |
| CMake              | >= 3.20 (VS2022 自带即可)           |

---

## 第一步：安装依赖 (vcpkg)

```powershell
# 如果还没装 vcpkg
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# 安装 Hical 所需依赖（x64-windows 默认 triplet）
.\vcpkg install boost-asio boost-system boost-json boost-beast openssl zlib --triplet=x64-windows
```

---

## 第二步：获取 Hical 源码

```powershell
git clone https://github.com/user/hical.git
cd hical
```

---

## 第三步：CMake 构建

```powershell
# 配置（替换为你的 vcpkg 实际路径，-DHICAL_BUILD_TESTS=OFF 跳过测试编译，加快构建速度）
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DHICAL_BUILD_TESTS=OFF

# 编译
cmake --build build --config Release
```

编译成功后，可执行文件位于：
```
build\examples\Release\http_server.exe
```

---

## 第四步：运行 & 测试

```powershell
# 启动服务器（默认监听 8080 端口）
.\build\examples\Release\http_server.exe

# 或指定端口
.\build\examples\Release\http_server.exe 9090
```

打开浏览器或用 curl 测试：

```bash
# 首页 — 返回含中文的 JSON
curl http://localhost:8080/

# 状态查询 — 中文 JSON
curl http://localhost:8080/api/status

# 中文问候
curl "http://localhost:8080/api/hello?name=张三"

# 用户查询 — 中文 JSON
curl http://localhost:8080/users/123

# Echo POST
curl -X POST -d "你好Hical" http://localhost:8080/api/echo
```

---

## Demo 源码 (examples/http_server.cpp)

> **注意**：本文档中的源码与项目仓库中的 `examples/http_server.cpp` 不完全相同——本文档版本在 JSON 响应中添加了中文内容（如 `"你好，世界！"`），用于演示框架对 UTF-8 中文的支持。若想体验中文 JSON 响应，请将以下代码覆盖到 `examples/http_server.cpp` 后重新编译。

以下是含中文 JSON 响应的完整代码：

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <iostream>

using namespace hical;

int main(int argc, char* argv[])
{
    try
    {
        auto port = static_cast<uint16_t>(argc >= 2 ? std::atoi(argv[1]) : 8080);

        HttpServer server(port);

        // ============ 中间件 ============

        // 日志中间件（每个请求自动打印方法+路径+状态码）
        server.use(
            [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
            {
                std::cout << httpMethodToString(req.method()) << " " << req.path() << std::endl;
                auto res = co_await next(req);
                std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
                co_return res;
            });

        // ============ HTTP 路由 ============

        // GET / — 首页（含中文 JSON）
        server.router().get("/",
                            [](const HttpRequest&) -> HttpResponse
                            {
                                boost::json::object obj;
                                obj["message"] = "你好，世界！";
                                obj["framework"] = "Hical";
                                obj["version"] = "2.6.0";
                                obj["description"] = "基于C++20/26的高性能Web框架";
                                return HttpResponse::json(obj);
                            });

        // GET /api/status — 状态查询（中文 JSON）
        server.router().get("/api/status",
                            [](const HttpRequest&) -> HttpResponse
                            {
                                boost::json::object obj;
                                obj["status"] = "运行中";
                                obj["version"] = "2.6.0";
                                obj["framework"] = "Hical";
                                obj["author"] = "Hical Team";
                                return HttpResponse::json(obj);
                            });

        // POST /api/echo — Echo 回写（原样返回请求体）
        server.router().post("/api/echo",
                             [](const HttpRequest& req) -> HttpResponse
                             {
                                 return HttpResponse::ok(req.body());
                             });

        // GET /api/hello?name=xxx — 中文问候
        server.router().get("/api/hello",
                            [](const HttpRequest& req) -> HttpResponse
                            {
                                boost::json::object obj;
                                auto name = req.queryParam("name");
                                if (!name.has_value() || name->empty())
                                {
                                    obj["greeting"] = "你好，世界！";
                                }
                                else
                                {
                                    obj["greeting"] = "你好，" + name.value() + "！";
                                }
                                return HttpResponse::json(obj);
                            });

        // GET /users/{id} — 路径参数（中文 JSON）
        server.router().get("/users/{id}",
                            [](const HttpRequest& req) -> HttpResponse
                            {
                                boost::json::object obj;
                                obj["userId"] = req.param("id");
                                obj["name"] = "玩家" + req.param("id");
                                obj["server"] = "C++开发";
                                return HttpResponse::json(obj);
                            });

        // ============ WebSocket 路由 ============

        // WebSocket Echo（连接后发送消息会原样返回）
        server.router().ws(
            "/ws/echo",
            [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void>
            {
                co_await ws.send("Echo: " + msg);
            },
            [](WebSocketSession& ws) -> Awaitable<void>
            {
                co_await ws.send("Connected to hical WebSocket!");
            });

        // ============ 启动 ============

        std::cout << "Hical HTTP Server v2.6.0" << std::endl;
        std::cout << "监听端口: " << port << std::endl;
        std::cout << "路由:" << std::endl;
        std::cout << "  GET    /              — 首页(中文JSON)" << std::endl;
        std::cout << "  GET    /api/status    — 状态查询(中文JSON)" << std::endl;
        std::cout << "  POST   /api/echo      — Echo 回写" << std::endl;
        std::cout << "  GET    /api/hello     — 中文问候(?name=xxx)" << std::endl;
        std::cout << "  GET    /users/{id}    — 用户查询(中文JSON)" << std::endl;
        std::cout << "  WS     /ws/echo       — WebSocket Echo" << std::endl;
        std::cout << std::endl;
        std::cout << "测试: curl http://localhost:" << port << "/" << std::endl;

        server.start();
    }
    catch (const std::exception& e)
    {
        std::cerr << "异常: " << e.what() << std::endl;
    }

    return 0;
}
```

---

## 响应示例

### GET /

```json
{"message":"你好，世界！","framework":"Hical","version":"2.6.0","description":"基于C++20/26的高性能Web框架"}
```

### GET /api/status

```json
{"status":"运行中","version":"2.6.0","framework":"Hical","author":"Hical Team"}
```

### GET /api/hello?name=张三

```json
{"greeting":"你好，张三！"}
```

### GET /users/42

```json
{"userId":"42","name":"玩家42","server":"C++开发"}
```

### POST /api/echo (body: "你好Hical")

```
你好Hical
```

---

## 框架特性一览

- **C++20 协程异步 I/O** — 基于 Boost.Asio `co_await`，无回调地狱
- **PMR 三级内存池** — 全局/线程级/请求级，零碎片高吞吐
- **洋葱模型中间件** — 请求前后处理，支持链式组合
- **路径参数路由** — `/users/{id}` 自动提取参数
- **WebSocket** — 原生协程 WebSocket 支持
- **JSON 响应** — `HttpResponse::json()` 直接返回 `application/json; charset=utf-8`
- **零拷贝 HTTP 解析** — picohttpparser + `string_view`，栈分配无堆开销

---

## 常见问题

### Q: 编译报错找不到 Boost

确保 `CMAKE_TOOLCHAIN_FILE` 指向正确的 vcpkg 路径，且已安装 `boost-asio`、`boost-system`、`boost-json`、`boost-beast`。

### Q: 运行时端口被占用

换个端口：`.\http_server.exe 9090`

### Q: 终端 curl 中文显示乱码

Windows 终端默认使用 GBK 编码，而 Hical 返回的是 UTF-8。解决方法：

```powershell
# 方法一：在当前终端临时切换为 UTF-8（推荐）
chcp 65001

# 然后再执行 curl
curl http://localhost:8080/
```

也可以直接用浏览器访问 `http://localhost:8080/`，浏览器会自动识别 UTF-8，不会乱码。

### Q: PowerShell 服务器启动时中文乱码

服务器 `std::cout` 输出的中文（如"监听端口"）在 PowerShell 中也可能乱码，同样先执行 `chcp 65001` 再启动服务器即可。

---

## 想了解更多？

- 反射路由示例：`examples/reflection_server.cpp`
- OpenAPI 自动生成：`examples/openapi_server.cpp`
- 完整文档：项目 `docs/` 目录
