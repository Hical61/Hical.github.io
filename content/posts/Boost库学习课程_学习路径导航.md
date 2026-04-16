+++
title = 'Boost 库学习课程 — 学习路径导航'
date = 2026-04-14T00:00:00+08:00
draft = false
tags = ["Boost", "C++20", "学习路径", "Hical"]
categories = ["Boost学习课程"]
description = "以 Hical 框架源码为实战案例，系统讲解 Boost.System、Asio、Beast、JSON 四大核心库的学习路径。"
+++

> 本系列以 **Hical** 框架源码为实战案例，系统讲解项目使用的 4 个核心 Boost 库。

## 本系列与 Hical 框架系列的关系

| 系列                                                                           | 视角           | 目标读者                |
| ------------------------------------------------------------------------------ | -------------- | ----------------------- |
| [Hical 框架系列（01-05）]({{< relref "posts/01-pmr-memory-pool-design.md" >}}) | 框架怎么设计   | 想理解 Hical 架构的人   |
| **Boost 学习课程**                                                             | Boost 库怎么用 | 想掌握 Boost 库本身的人 |

同一段源码（如 `TcpServer::acceptLoop()`），Blog 讲 *"为什么用协程做 accept 循环"*，本系列讲 *"`async_accept` + `use_awaitable` 的 API 语义是什么"*。

---

## 学习路径

```
课程 1: Boost.System    ← 最基础，error_code 是所有 I/O 操作的返回值
    │
    ▼
课程 2: Boost.Asio      ← 核心 I/O 引擎，依赖 error_code
    │
    ▼
课程 3: Boost.Beast     ← HTTP/WebSocket 协议层，构建在 Asio 之上
    │
    ▼
课程 4: Boost.JSON      ← 数据层，与 HTTP 请求/响应配合
```

---

## 课程概览

| #   | 课程                                                                 | 核心主题                                  | 前置依赖 | 预计时长 |
| --- | -------------------------------------------------------------------- | ----------------------------------------- | -------- | -------- |
| 1   | [Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}})  | 统一错误码、error_category、跨平台映射    | C++ 基础 | 1-2 小时 |
| 2   | [Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}})      | io_context、协程、TCP、定时器、多线程模型 | 课程 1   | 4-6 小时 |
| 3   | [Boost.Beast]({{< relref "posts/Boost.Beast_HTTP与WebSocket.md" >}}) | HTTP 消息模型、Parser、WebSocket、SSL     | 课程 1+2 | 3-4 小时 |
| 4   | [Boost.JSON]({{< relref "posts/Boost.JSON_序列化与反序列化.md" >}})  | 值类型体系、解析/序列化、PMR 集成、反射   | 课程 1   | 2-3 小时 |

---

## 各课程一句话摘要

- **课程 1 — Boost.System**：理解 `error_code` + `error_category` 体系，掌握 I/O 操作的两种错误处理模式（错误码 vs 异常）。
- **课程 2 — Boost.Asio**：从 `io_context` 出发，掌握 C++20 协程式异步 I/O，学会 TCP 服务器、定时器和多线程模型。
- **课程 3 — Boost.Beast**：在 Asio 之上构建 HTTP/WebSocket 协议层，学会请求解析、响应构建和 WebSocket 消息循环。
- **课程 4 — Boost.JSON**：掌握 JSON 值类型操作、PMR 高性能分配，以及 Hical 反射层如何实现自动序列化。

---

## 开发环境准备

### 编译器要求

| 编译器 | 最低版本 | 推荐版本 |
| ------ | -------- | -------- |
| GCC    | 13       | 14+      |
| Clang  | 16       | 18+      |
| MSVC   | 2022     | 最新     |

### Boost 安装

**Linux (apt)**：
```bash
sudo apt install libboost-all-dev
```

**MSYS2 (MINGW64)**：
```bash
pacman -S mingw-w64-x86_64-boost
```

**vcpkg**：
```bash
vcpkg install boost-asio boost-beast boost-json boost-system
```

### 编译独立示例

课程中的独立示例仅依赖 Boost（不依赖 Hical），可单文件编译：

```bash
# Linux / MSYS2
g++ -std=c++20 -O2 example.cpp -lboost_system -lpthread -o example

# 带 SSL 的示例
g++ -std=c++20 -O2 ssl_example.cpp -lboost_system -lssl -lcrypto -lpthread -o ssl_example
```

---

> 有兴趣可查看 Hical 框架源码地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
