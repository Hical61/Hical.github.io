+++
title = 'Boost.MySQL 学习课程：异步数据库访问'
date = '2026-04-29'
draft = false
tags = ["Boost", "Boost.MySQL", "数据库", "连接池", "协程", "C++20", "Hical"]
categories = ["Boost学习课程"]
description = "掌握 Boost.MySQL 的协程式异步数据库访问，学会连接管理、参数化查询、PreparedStatement、结果集处理和事务控制，结合 Hical 框架实战解读连接池与中间件设计。"
+++

> **课程导航**：[学习路径]({{< relref "posts/Boost库学习课程_学习路径导航.md" >}}) | [Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}}) | [Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}}) | [Boost.Beast]({{< relref "posts/Boost.Beast_HTTP与WebSocket.md" >}}) | [Boost.JSON]({{< relref "posts/Boost.JSON_序列化与反序列化.md" >}}) | **Boost.MySQL**

## 前置知识

- [课程 1: Boost.System]({{< relref "posts/Boost.System_错误处理基石.md" >}})（`error_code`、`system_error`）
- [课程 2: Boost.Asio]({{< relref "posts/Boost.Asio_异步IO与协程.md" >}})（`io_context`、协程、`co_await` + `use_awaitable`）
- SQL 基础（SELECT/INSERT/UPDATE/DELETE、事务）
- MySQL 数据库基本操作

## 学习目标

完成本课程后，你将能够：
1. 理解 Boost.MySQL 的类型擦除连接模型（`any_connection`）
2. 使用 C++20 协程执行异步数据库操作
3. 掌握参数化查询和 PreparedStatement 防 SQL 注入
4. 理解结果集类型体系（`results` / `static_results`）
5. 实现事务控制（BEGIN/COMMIT/ROLLBACK）
6. 读懂 Hical 的连接池、Statement 缓存和数据库中间件设计

---

## 目录

- [前置知识](#前置知识)
- [学习目标](#学习目标)
- [目录](#目录)
- [1. 核心概念](#1-核心概念)
  - [1.1 Boost.MySQL 的定位](#11-boostmysql-的定位)
  - [1.2 连接类型体系](#12-连接类型体系)
  - [1.3 查询执行模型](#13-查询执行模型)
  - [1.4 结果集类型体系](#14-结果集类型体系)
- [2. 基础用法](#2-基础用法)
  - [2.1 建立连接](#21-建立连接)
  - [2.2 执行文本查询](#22-执行文本查询)
  - [2.3 参数化查询（客户端格式化）](#23-参数化查询客户端格式化)
  - [2.4 PreparedStatement](#24-preparedstatement)
  - [2.5 结果集遍历](#25-结果集遍历)
  - [2.6 事务控制](#26-事务控制)
- [3. 进阶主题](#3-进阶主题)
  - [3.1 类型擦除连接 any\_connection](#31-类型擦除连接-any_connection)
  - [3.2 静态类型结果集 static\_results](#32-静态类型结果集-static_results)
  - [3.3 多结果集（存储过程）](#33-多结果集存储过程)
  - [3.4 连接池 connection\_pool](#34-连接池-connection_pool)
  - [3.5 错误处理与诊断](#35-错误处理与诊断)
- [4. Hical 实战解读](#4-hical-实战解读)
  - [4.1 MysqlConnection：any\_connection 的框架封装](#41-mysqlconnectionany_connection-的框架封装)
  - [4.2 StmtCache：LRU PreparedStatement 缓存](#42-stmtcachelru-preparedstatement-缓存)
  - [4.3 DbConnectionPool：协程式连接池](#43-dbconnectionpool协程式连接池)
  - [4.4 DbMiddleware：请求级连接生命周期](#44-dbmiddleware请求级连接生命周期)
  - [4.5 DbQueryLog：查询日志装饰器](#45-dbquerylog查询日志装饰器)
  - [4.6 完整请求处理流程](#46-完整请求处理流程)
- [5. 练习题](#5-练习题)
  - [练习 1：协程式 CRUD](#练习-1协程式-crud)
  - [练习 2：参数化查询实战](#练习-2参数化查询实战)
  - [练习 3：事务与错误处理](#练习-3事务与错误处理)
  - [练习 4：LRU 缓存设计](#练习-4lru-缓存设计)
  - [练习 5（挑战）：连接池实现](#练习-5挑战连接池实现)
- [6. 总结与拓展阅读](#6-总结与拓展阅读)
  - [核心 API 速查表](#核心-api-速查表)
  - [查询方式对比](#查询方式对比)
  - [拓展阅读](#拓展阅读)
  - [课程回顾](#课程回顾)

---

## 1. 核心概念

### 1.1 Boost.MySQL 的定位

Boost.MySQL 是一个**纯异步**的 MySQL 客户端库，直接实现 MySQL 客户端/服务器协议（不依赖 libmysqlclient），天然集成 Boost.Asio 的异步模型。

```
协议栈层次：

┌─────────────────────────────────┐
│  应用层 (Hical)                  │  连接池、中间件、查询日志
├─────────────────────────────────┤
│  Boost.MySQL                     │  MySQL 协议解析、查询执行
├─────────────────────────────────┤
│  Boost.Asio                      │  异步 I/O、协程、事件循环
├─────────────────────────────────┤
│  TCP / SSL                       │  网络传输
└─────────────────────────────────┘
```

**与 libmysqlclient 的关键区别**：

| 特性     | Boost.MySQL                 | libmysqlclient (C API)       |
| -------- | --------------------------- | ---------------------------- |
| 异步模型 | 原生 Asio 异步（协程/回调） | 同步阻塞（或自行封装线程池） |
| 依赖     | 仅 Boost + OpenSSL          | MySQL 官方 C 库              |
| 分配器   | 支持 PMR / 自定义           | 内部分配                     |
| 编译     | Header-only（大部分）       | 需链接 .so/.dll              |
| 类型安全 | `static_results` 编译期校验 | 运行时手动转换               |
| 协程集成 | `co_await` 一等公民         | 无                           |
| SSL      | 共享 Asio 的 ssl::context   | 独立 SSL 配置                |

### 1.2 连接类型体系

Boost.MySQL 提供两类连接：

```
连接类型体系：

┌──────────────────────────────────────────────────┐
│  connection<Stream>                               │
│  模板化连接，Stream 固定为特定类型                  │
│  • connection<tcp_ssl_socket>   ← SSL + TCP       │
│  • connection<tcp_socket>       ← 明文 TCP        │
│  • connection<unix_socket>      ← Unix 域套接字    │
│  编译期确定，零运行时开销                           │
└──────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────┐
│  any_connection                                   │
│  类型擦除连接（推荐）                              │
│  • 运行时选择传输层（TCP/SSL/Unix）                │
│  • 支持自动重连                                    │
│  • 支持连接池 (connection_pool)                    │
│  • 微小的运行时开销（虚函数调用）                   │
└──────────────────────────────────────────────────┘
```

**Hical 选择**：使用 `any_connection`，因为框架需要在运行时根据配置决定是否启用 SSL，且 `any_connection` 是连接池的前提。

### 1.3 查询执行模型

Boost.MySQL 支持三种查询方式：

```
查询方式：

1. 文本查询 (Text Query)
   conn.async_execute("SELECT * FROM users WHERE id = 1", results)
   ├─ 适合 DDL、SET、简单查询
   └─ ⚠ 拼接用户输入有 SQL 注入风险

2. 客户端格式化 (Client-side Formatting)  [Boost 1.85+]
   conn.async_execute(
       mysql::with_params("SELECT * FROM users WHERE id = {}", userId),
       results)
   ├─ 客户端拼接，自动转义
   ├─ 一次网络往返
   └─ ⚠ 对复杂场景转义可能不够安全

3. PreparedStatement（推荐）
   auto stmt = conn.async_prepare_statement("SELECT * FROM users WHERE id = ?")
   conn.async_execute(stmt.bind(userId), results)
   ├─ 服务器端预编译 + 参数绑定
   ├─ 最安全，彻底防 SQL 注入
   ├─ 可复用（缓存 statement 避免重复 prepare）
   └─ 需要两次往返（prepare + execute，但可缓存抵消）
```

**Hical 选择**：参数化查询**强制使用 PreparedStatement**，配合 `StmtCache` 消除重复 prepare 开销。

### 1.4 结果集类型体系

```
结果集类型：

results (动态类型)
├─ rows() → rows_view：行集合
├─ 每行 → row_view：列集合
├─ 每列 → field_view：变体类型
│  ├─ int64, uint64, double
│  ├─ string_view, blob_view
│  ├─ date, datetime, time
│  └─ null (is_null())
├─ meta() → 列元信息（名称、类型）
├─ affected_rows(), last_insert_id()
└─ 适合动态 SQL、通用框架

static_results<Row...> (静态类型)  [Boost 1.84+]
├─ 编译期绑定 C++ 结构体
├─ 类型不匹配 → 编译错误
└─ 适合固定结构的业务查询
```

---

## 2. 基础用法

### 2.1 建立连接

```cpp
// example_mysql_connect.cpp
// 编译：g++ -std=c++20 example_mysql_connect.cpp -lboost_charconv -lssl -lcrypto -lpthread -o example

#include <boost/mysql.hpp>
#include <boost/asio.hpp>
#include <iostream>

namespace mysql = boost::mysql;
namespace asio = boost::asio;

asio::awaitable<void> run()
{
    auto executor = co_await asio::this_coro::executor;

    // 1. 创建类型擦除连接
    mysql::any_connection conn(executor);

    // 2. 配置连接参数
    mysql::connect_params params;
    params.server_address.emplace_host_and_port("127.0.0.1", 3306);
    params.username = "root";
    params.password = "secret";
    params.database = "test_db";
    params.ssl = mysql::ssl_mode::enable;  // 优先 SSL，不可用时降级

    // 3. 异步连接
    co_await conn.async_connect(params, asio::use_awaitable);
    std::cout << "连接成功！服务器版本: " << conn.server_info() << "\n";

    // 4. 关闭连接
    co_await conn.async_close(asio::use_awaitable);
}

int main()
{
    asio::io_context ioCtx;
    asio::co_spawn(ioCtx, run(), asio::detached);
    ioCtx.run();
    return 0;
}
```

**SSL 模式说明**：

| 模式                | 说明                               |
| ------------------- | ---------------------------------- |
| `ssl_mode::enable`  | 优先 SSL，服务器不支持时降级为明文 |
| `ssl_mode::require` | 强制 SSL，不支持则报错             |
| `ssl_mode::disable` | 禁用 SSL，始终明文                 |

### 2.2 执行文本查询

```cpp
asio::awaitable<void> textQuery(mysql::any_connection& conn)
{
    mysql::results result;

    // 文本查询——适合 DDL 和静态 SQL
    co_await conn.async_execute("CREATE TABLE IF NOT EXISTS users ("
                                "  id INT AUTO_INCREMENT PRIMARY KEY,"
                                "  name VARCHAR(64) NOT NULL,"
                                "  email VARCHAR(128),"
                                "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
                                ")", result, asio::use_awaitable);

    std::cout << "表创建完成\n";

    // 插入数据
    co_await conn.async_execute("INSERT INTO users (name, email) "
                                "VALUES ('Hical', 'Hical@example.com')",
                                result, asio::use_awaitable);

    std::cout << "插入成功，ID = " << result.last_insert_id()
              << "，影响 " << result.affected_rows() << " 行\n";
}
```

> ⚠ **安全警告**：文本查询不要拼接用户输入！`"WHERE name = '" + userName + "'"` 是经典的 SQL 注入漏洞。

### 2.3 参数化查询（客户端格式化）

Boost 1.85+ 引入了 `with_params`，在客户端安全格式化 SQL：

```cpp
asio::awaitable<void> clientFormatQuery(mysql::any_connection& conn,
                                        std::string_view userName)
{
    mysql::results result;

    // with_params 会自动转义参数，防止 SQL 注入
    co_await conn.async_execute(
        mysql::with_params("SELECT id, email FROM users WHERE name = {}", userName),
        result,
        asio::use_awaitable);

    for (auto row : result.rows())
    {
        std::cout << "ID: " << row[0].as_int64()
                  << ", Email: " << row[1].as_string() << "\n";
    }
}
```

**特点**：一次网络往返、自动转义、语法类似 `std::format`。

### 2.4 PreparedStatement

PreparedStatement 是最安全的查询方式——参数在服务器端绑定，从协议层面杜绝注入：

```cpp
asio::awaitable<void> preparedQuery(mysql::any_connection& conn)
{
    // 1. 预编译 SQL（服务器端解析并缓存执行计划）
    mysql::statement stmt = co_await conn.async_prepare_statement(
        "SELECT id, name, email FROM users WHERE id = ?",
        asio::use_awaitable);

    std::cout << "Statement ID: " << stmt.id()
              << "，参数数量: " << stmt.num_params() << "\n";

    // 2. 绑定参数并执行
    mysql::results result;
    co_await conn.async_execute(
        stmt.bind(42),  // 绑定 id = 42
        result,
        asio::use_awaitable);

    // 3. 遍历结果
    for (auto row : result.rows())
    {
        std::cout << row[0].as_int64() << " | "
                  << row[1].as_string() << " | "
                  << row[2].as_string() << "\n";
    }

    // 4. 复用同一 statement（不同参数）
    co_await conn.async_execute(
        stmt.bind(100),
        result,
        asio::use_awaitable);

    // 5. 不再使用时关闭（释放服务器资源）
    co_await conn.async_close_statement(stmt, asio::use_awaitable);
}
```

**多参数绑定**：

```cpp
// 多个参数按 ? 顺序绑定
auto stmt = co_await conn.async_prepare_statement(
    "INSERT INTO users (name, email) VALUES (?, ?)",
    asio::use_awaitable);

mysql::results result;
co_await conn.async_execute(
    stmt.bind("Bob", "bob@example.com"),
    result,
    asio::use_awaitable);

std::cout << "新用户 ID: " << result.last_insert_id() << "\n";
```

### 2.5 结果集遍历

```cpp
asio::awaitable<void> resultTraversal(mysql::any_connection& conn)
{
    mysql::results result;
    co_await conn.async_execute("SELECT id, name, email, created_at FROM users",
                                result, asio::use_awaitable);

    // 1. 列元信息
    for (const auto& col : result.meta())
    {
        std::cout << col.column_name() << "("
                  << static_cast<int>(col.type()) << ") ";
    }
    std::cout << "\n";

    // 2. 行遍历
    for (auto row : result.rows())
    {
        // field_view 是变体类型，需按实际类型访问
        int64_t id = row[0].as_int64();
        std::string_view name = row[1].as_string();

        // 处理可能为 NULL 的列
        if (row[2].is_null())
        {
            std::cout << id << " | " << name << " | (NULL)\n";
        }
        else
        {
            std::cout << id << " | " << name << " | " << row[2].as_string() << "\n";
        }
    }

    // 3. 按列名查找（遍历 meta）
    auto findCol = [&](std::string_view colName) -> size_t
    {
        const auto& meta = result.meta();
        for (size_t i = 0; i < meta.size(); ++i)
        {
            if (meta[i].column_name() == colName) return i;
        }
        throw std::runtime_error("Column not found: " + std::string(colName));
    };

    size_t emailIdx = findCol("email");
    for (auto row : result.rows())
    {
        if (!row[emailIdx].is_null())
        {
            std::cout << "Email: " << row[emailIdx].as_string() << "\n";
        }
    }
}
```

**field_view 类型访问速查表**：

| MySQL 类型                 | field_view 方法 | C++ 类型          |
| -------------------------- | --------------- | ----------------- |
| TINYINT, SMALLINT, INT ... | `as_int64()`    | `int64_t`         |
| INT UNSIGNED ...           | `as_uint64()`   | `uint64_t`        |
| FLOAT, DOUBLE              | `as_double()`   | `double`          |
| VARCHAR, TEXT              | `as_string()`   | `string_view`     |
| BLOB, BINARY               | `as_blob()`     | `blob_view`       |
| DATE                       | `as_date()`     | `mysql::date`     |
| DATETIME, TIMESTAMP        | `as_datetime()` | `mysql::datetime` |
| TIME                       | `as_time()`     | `mysql::time`     |
| NULL                       | `is_null()`     | `bool`            |

### 2.6 事务控制

```cpp
asio::awaitable<void> transactionExample(mysql::any_connection& conn)
{
    mysql::results result;

    // 开启事务
    co_await conn.async_execute("START TRANSACTION", result, asio::use_awaitable);

    try
    {
        // 扣减金币
        co_await conn.async_execute(
            mysql::with_params(
                "UPDATE players SET gold = gold - {} WHERE id = {} AND gold >= {}",
                100, 1001, 100),
            result, asio::use_awaitable);

        if (result.affected_rows() == 0)
        {
            // 金币不足，回滚
            co_await conn.async_execute("ROLLBACK", result, asio::use_awaitable);
            std::cout << "金币不足，交易取消\n";
            co_return;
        }

        // 增加道具
        co_await conn.async_execute(
            mysql::with_params(
                "INSERT INTO player_items (player_id, item_id, count) "
                "VALUES ({}, {}, {}) "
                "ON DUPLICATE KEY UPDATE count = count + {}",
                1001, 5001, 1, 1),
            result, asio::use_awaitable);

        // 提交事务
        co_await conn.async_execute("COMMIT", result, asio::use_awaitable);
        std::cout << "交易成功\n";
    }
    catch (...)
    {
        // 异常时回滚
        co_await conn.async_execute("ROLLBACK", result, asio::use_awaitable);
        throw;
    }
}
```

> **游戏服务器要点**：涉及经济系统（金币、道具）的操作**必须**在事务中执行，且需要防重入保护——避免同一玩家并发触发导致数据不一致。

---

## 3. 进阶主题

### 3.1 类型擦除连接 any_connection

`any_connection` 是 Boost 1.84 引入的推荐连接类型，它将传输层（TCP/SSL/Unix）的选择推迟到运行时：

```cpp
// 不需要在编译期决定是否用 SSL
mysql::any_connection conn(executor);

mysql::connect_params params;
params.server_address.emplace_host_and_port("127.0.0.1", 3306);

// 运行时配置 SSL 模式
if (config.useSsl)
{
    params.ssl = mysql::ssl_mode::require;
}
else
{
    params.ssl = mysql::ssl_mode::disable;
}

co_await conn.async_connect(params, asio::use_awaitable);
```

**与模板化连接的对比**：

| 特性        | `connection<Stream>` | `any_connection`       |
| ----------- | -------------------- | ---------------------- |
| 传输层选择  | 编译期               | 运行时                 |
| SSL context | 用户传入并管理       | 内部自动创建           |
| 连接池支持  | ❌                    | ✅（`connection_pool`） |
| 自动重连    | ❌                    | ✅                      |
| 运行时开销  | 零（静态分发）       | 极小（虚函数）         |
| 推荐场景    | 嵌入式、极致性能     | 应用服务器、Web 框架   |

### 3.2 静态类型结果集 static_results

`static_results` 让查询结果在编译期与 C++ 结构体绑定：

```cpp
#include <boost/mysql/static_results.hpp>
#include <boost/describe.hpp>

// 定义行结构体
struct User
{
    int64_t id;
    std::string name;
    std::optional<std::string> email;  // 可空列用 optional
};

// Boost.Describe 注册字段（供 static_results 做编译期映射）
BOOST_DESCRIBE_STRUCT(User, (), (id, name, email))

asio::awaitable<void> staticQuery(mysql::any_connection& conn)
{
    // 结果直接映射到 User 结构体
    mysql::static_results<User> result;

    co_await conn.async_execute(
        "SELECT id, name, email FROM users WHERE id < 100",
        result,
        asio::use_awaitable);

    // 类型安全的访问——编译期校验
    for (const User& user : result.rows())
    {
        std::cout << user.id << " | " << user.name;
        if (user.email.has_value())
        {
            std::cout << " | " << *user.email;
        }
        std::cout << "\n";
    }
}
```

**优势**：列数量/类型不匹配时编译报错，不必运行时手动 `as_int64()` / `as_string()`。

### 3.3 多结果集（存储过程）

MySQL 存储过程可以返回多个结果集：

```cpp
asio::awaitable<void> callProcedure(mysql::any_connection& conn)
{
    mysql::results result;

    // 调用存储过程（可能返回多个 SELECT 结果）
    co_await conn.async_execute("CALL get_player_info(1001)", result, asio::use_awaitable);

    // 遍历所有结果集
    // result.rows() 返回第一个结果集的行
    // result.at(0).rows() 也是第一个结果集
    // result.at(1).rows() 是第二个结果集
    std::cout << "结果集数量: " << result.size() << "\n";

    for (size_t i = 0; i < result.size(); ++i)
    {
        std::cout << "--- 结果集 " << i << " ---\n";
        for (auto row : result.at(i).rows())
        {
            for (size_t j = 0; j < row.size(); ++j)
            {
                if (row[j].is_null())
                    std::cout << "NULL";
                else if (row[j].is_int64())
                    std::cout << row[j].as_int64();
                else if (row[j].is_string())
                    std::cout << row[j].as_string();
                if (j + 1 < row.size()) std::cout << " | ";
            }
            std::cout << "\n";
        }
    }
}
```

### 3.4 连接池 connection_pool

Boost 1.85+ 内置了 `connection_pool`，管理 `any_connection` 的生命周期：

```cpp
#include <boost/mysql/connection_pool.hpp>

asio::awaitable<void> poolExample(asio::io_context& ioCtx)
{
    // 1. 配置连接池
    mysql::pool_params poolParams;
    poolParams.server_address.emplace_host_and_port("127.0.0.1", 3306);
    poolParams.username = "root";
    poolParams.password = "secret";
    poolParams.database = "test_db";
    poolParams.max_size = 16;             // 最大连接数
    poolParams.initial_size = 2;          // 初始连接数
    poolParams.ssl = mysql::ssl_mode::enable;

    // 2. 创建连接池
    mysql::connection_pool pool(ioCtx, std::move(poolParams));

    // 3. 启动后台维护（健康检查、空闲回收）
    pool.async_run(asio::detached);

    // 4. 获取连接（RAII，离开作用域自动归还）
    {
        auto conn = co_await pool.async_get_connection(asio::use_awaitable);

        mysql::results result;
        co_await conn->async_execute("SELECT 1", result, asio::use_awaitable);
        std::cout << "Ping 成功: " << result.rows()[0][0].as_int64() << "\n";

        // conn 析构时自动归还到连接池
    }

    // 5. 关闭连接池
    pool.cancel();
}
```

> **为什么 Hical 没用内置 connection_pool？**  因为 Hical 的 `DbConnectionPool` 需要：(1) `DbConnection` 抽象接口（支持未来切换 PostgreSQL 等后端）；(2) 与框架中间件深度集成（请求属性注入）；(3) 自定义 LIFO 复用策略和 PreparedStatement 缓存联动。内置 pool 更适合简单场景。

### 3.5 错误处理与诊断

```cpp
asio::awaitable<void> errorHandling(mysql::any_connection& conn)
{
    try
    {
        mysql::results result;
        co_await conn.async_execute("SELECT * FROM nonexistent_table",
                                    result, asio::use_awaitable);
    }
    catch (const mysql::error_with_diagnostics& err)
    {
        // MySQL 服务器返回的错误信息
        std::cerr << "MySQL 错误码: " << err.code().value() << "\n"
                  << "错误消息: " << err.code().message() << "\n"
                  << "服务器诊断: " << err.get_diagnostics().server_message() << "\n";
        // 输出类似：
        // MySQL 错误码: 1146
        // 错误消息: ...
        // 服务器诊断: Table 'test_db.nonexistent_table' doesn't exist
    }
    catch (const boost::system::system_error& err)
    {
        // 网络层错误（连接断开、超时等）
        std::cerr << "系统错误: " << err.code().message() << "\n";
    }
}
```

**错误分类**：

| 异常类型                        | 来源          | 场景                             |
| ------------------------------- | ------------- | -------------------------------- |
| `mysql::error_with_diagnostics` | MySQL 服务器  | SQL 语法错误、表不存在、权限不足 |
| `boost::system::system_error`   | 网络层 / Asio | 连接断开、超时、DNS 解析失败     |

---

## 4. Hical 实战解读

### 4.1 MysqlConnection：any_connection 的框架封装

Hical 的 `MysqlConnection`（`src/db/MysqlConnection.h`）封装了 `boost::mysql::any_connection`，实现 `DbConnection` 抽象接口：

```
MysqlConnection 类结构：

┌─────────────────────────────────────────────────────┐
│  MysqlConnection : public DbConnection              │
├─────────────────────────────────────────────────────┤
│  m_conn       : boost::mysql::any_connection        │ ← 底层连接
│  m_stmtCache  : StmtCache                           │ ← Statement 缓存
│  m_alive      : bool                                │ ← 连接状态
│  m_inTransaction : bool                             │ ← 事务状态
│  m_lastActive : steady_clock::time_point            │ ← 最近活跃时间
│  m_lastPing   : steady_clock::time_point            │ ← 最近 ping 时间
├─────────────────────────────────────────────────────┤
│  static create(ioCtx, config) → Awaitable<shared_ptr>│
│  query(sql, params) → Awaitable<DbResult>           │
│  execute(sql, params) → Awaitable<DbResult>         │
│  beginTransaction() / commit() / rollback()         │
│  ping() → Awaitable<bool>                           │
│  static makeFactory() → DbConnectionFactory         │
└─────────────────────────────────────────────────────┘
```

**关键设计 1：工厂模式解耦**

```cpp
// 工厂函数——连接池不知道具体实现类
using DbConnectionFactory = std::function<
    Awaitable<std::shared_ptr<DbConnection>>(asio::io_context&, const DbConfig&)>;

// MysqlConnection 提供工厂
static DbConnectionFactory makeFactory()
{
    return [](asio::io_context& ioCtx, const DbConfig& config)
               -> Awaitable<std::shared_ptr<DbConnection>>
    {
        co_return co_await MysqlConnection::create(ioCtx, config);
    };
}

// 使用时
auto pool = std::make_shared<DbConnectionPool>(
    ioCtx, config, MysqlConnection::makeFactory());
```

连接池通过工厂函数创建连接，完全不依赖 `MysqlConnection` 类型——未来替换为 PostgreSQL 只需提供新工厂。

**关键设计 2：参数化查询流程**

```
query(sql, params) 执行流程：

1. getOrPrepare(sql)
   ├─ m_stmtCache.find(sql)
   │  └─ 命中 → 返回缓存的 statement
   └─ 未命中
      └─ async_prepare_statement(sql)
      └─ m_stmtCache.insert(sql, stmt)
         └─ 缓存满 → 淘汰 LRU，异步 close 被淘汰的 statement

2. 构建参数列表
   └─ 将 std::span<const std::string> 转为 vector<field_view>

3. 执行
   └─ async_execute(stmt.bind(fields...), results)
   
4. 异常处理
   └─ 执行失败 → erase 缓存的 statement → 重新 prepare → 重试
      （应对 MySQL 服务器重启导致的 statement 失效）

5. 结果转换
   └─ convertResults(boost::mysql::results → DbResult)
      └─ 遍历行列，统一转为 std::string
```

**关键设计 3：字符集安全验证**

```cpp
// 防止通过 charset 参数注入 SQL
static void validateCharset(const std::string& charset)
{
    // 白名单：仅允许字母、数字、下划线
    for (char ch : charset)
    {
        if (!std::isalnum(ch) && ch != '_')
        {
            throw std::invalid_argument("Invalid charset: " + charset);
        }
    }
}

// 连接建立后设置字符集
co_await conn.async_execute("SET NAMES '" + config.charset + "'", ...);
```

### 4.2 StmtCache：LRU PreparedStatement 缓存

`StmtCache`（`src/db/StmtCache.h`）是**每连接**的 LRU 缓存，避免同一 SQL 重复 prepare：

```
StmtCache 内部结构：

┌─────────────────────────────────────────────────────┐
│  m_lruList : list<pair<string, statement>>           │
│  ┌───┐   ┌───┐   ┌───┐   ┌───┐                     │
│  │MRU│──→│   │──→│   │──→│LRU│  ← 淘汰方向         │
│  └───┘   └───┘   └───┘   └───┘                     │
│                                                      │
│  m_map : unordered_map<string, list::iterator>       │
│  ┌─────────┬──────────┐                              │
│  │ SQL_A   │ → iter_1 │  O(1) 查找                   │
│  │ SQL_B   │ → iter_2 │                              │
│  │ SQL_C   │ → iter_3 │                              │
│  └─────────┴──────────┘                              │
│                                                      │
│  m_maxSize : 64（默认）                               │
└─────────────────────────────────────────────────────┘

操作复杂度：
  find()   : O(1) 查找 + O(1) 链表提升到头部
  insert() : O(1) 插入 + 满时 O(1) 淘汰尾部
  erase()  : O(1)
```

**为什么需要 Statement 缓存？**

每次 `async_prepare_statement` 都是一次到 MySQL 服务器的网络往返。对于 Web 框架来说，同一 SQL 模板（如 `SELECT * FROM users WHERE id = ?`）会被大量请求反复使用。缓存后，只有首次执行需要 prepare，后续直接复用。

```cpp
// StmtCache 的核心用法
StmtCache cache(64);  // 最多缓存 64 条

// 查找
mysql::statement* cached = cache.find("SELECT * FROM users WHERE id = ?");
if (cached)
{
    // 命中——直接使用，同时该条目被提升到 MRU
    conn.async_execute(cached->bind(42), results, ...);
}
else
{
    // 未命中——prepare 后存入缓存
    auto stmt = co_await conn.async_prepare_statement(sql, ...);
    auto evicted = cache.insert(sql, stmt);  // 可能淘汰一条

    if (evicted.has_value())
    {
        // 被淘汰的 statement 需要异步关闭
        co_await conn.async_close_statement(*evicted, ...);
    }
}
```

### 4.3 DbConnectionPool：协程式连接池

Hical 的连接池（`src/db/DbConnectionPool.h`）是为协程设计的——不使用 `condition_variable`（会阻塞事件循环线程），而是用 `steady_timer` 作为协程信号量：

```
连接池状态机：

             init()
               │
               ▼
   ┌──────────────────────┐
   │    创建 minConnections│ 个连接
   │    启动后台循环        │
   └──────────┬───────────┘
              │
              ▼
   ┌──────────────────────┐
   │      RUNNING          │
   │  ┌────────────────┐  │
   │  │  空闲栈 (LIFO)  │  │  ← release() 压栈
   │  │  ┌──┐┌──┐┌──┐  │  │  ← acquire() 弹栈
   │  │  │C3││C2││C1│  │  │
   │  │  └──┘└──┘└──┘  │  │
   │  └────────────────┘  │
   │                       │
   │  ┌────────────────┐  │
   │  │  等待队列       │  │  ← 池满时 acquire() 挂起协程
   │  │  [timer][timer] │  │  ← release() 时 cancel timer 唤醒
   │  └────────────────┘  │
   └──────────┬───────────┘
              │ shutdown()
              ▼
   ┌──────────────────────┐
   │   SHUTDOWN            │
   │   关闭所有连接         │
   └──────────────────────┘
```

**为什么用 steady_timer 当信号量？**

```cpp
// 传统方式——阻塞 io_context 线程，不可用！
std::mutex mtx;
std::condition_variable cv;
cv.wait(lock, [&]{ return !idle.empty(); });  // ❌ 阻塞

// Hical 方式——协程友好
boost::asio::steady_timer waiter(ioCtx, std::chrono::steady_clock::time_point::max());
co_await waiter.async_wait(asio::use_awaitable);  // ✅ 挂起协程，不阻塞线程

// release() 时唤醒
waiter.cancel();  // 触发等待者的 async_wait 返回 operation_aborted
```

**LIFO vs FIFO 策略**：

连接池使用 LIFO（后进先出）而非 FIFO 复用空闲连接：
- 最近归还的连接更可能还在 TCP keepalive 内、MySQL 线程缓存热
- 不活跃的连接自然沉底，被 `idleCheckLoop` 回收
- 减少总活跃连接数——更少的服务器资源消耗

### 4.4 DbMiddleware：请求级连接生命周期

`DbMiddleware`（`src/db/DbMiddleware.h`）遵循 Hical 的洋葱模型，管理每个 HTTP 请求的数据库连接：

```
洋葱模型执行流程：

请求进入
  │
  ▼
┌─────────────────────────────────────────────┐
│  DbMiddleware 前置                            │
│  1. pool->acquire()  获取连接                  │
│  2. req.setAttribute("hical.db.conn", conn)  │
│  3. req.setAttribute("hical.db.pool", pool)  │
│  4. 若 autoTransaction → BEGIN TRANSACTION    │
│     ┌─────────────────────────────────────┐  │
│     │  QueryLogMiddleware（可选）           │  │
│     │  装饰连接 → LoggingDbConnection      │  │
│     │     ┌─────────────────────────────┐  │  │
│     │     │  业务路由处理器              │  │  │
│     │     │  auto conn = getDbConn(req) │  │  │
│     │     │  conn->query("...", params) │  │  │
│     │     └─────────────────────────────┘  │  │
│     │  收集查询日志，触发回调              │  │
│     └─────────────────────────────────────┘  │
│  5. 正常 → COMMIT                            │
│  6. 异常 → ROLLBACK                          │
│  7. pool->release(conn) 归还连接              │
└─────────────────────────────────────────────┘
  │
  ▼
响应返回
```

**使用方式**：

```cpp
// 服务器配置
auto pool = std::make_shared<DbConnectionPool>(
    ioCtx, dbConfig, MysqlConnection::makeFactory());
co_await pool->init();

// 注册全局中间件
server.use(makeDbMiddleware(pool, {.autoTransaction = true}));
server.use(makeQueryLogMiddleware({
    .slowQueryThreshold = std::chrono::milliseconds(100),
    .onSlowQuery = [](const QueryLogEntry& entry) {
        LOG_WARN("慢查询: {} ({}μs)", entry.sql, entry.duration.count());
    }
}));

// 路由处理器中使用
server.get("/api/user/{id}", [](HttpRequest& req) -> Awaitable<HttpResponse>
{
    auto conn = getDbConnection(req);
    std::array<std::string, 1> params = {req.param("id")};
    auto result = co_await conn->query(
        "SELECT name, email FROM users WHERE id = ?", params);

    if (result.empty())
    {
        co_return HttpResponse::notFound();
    }

    co_return HttpResponse::json({
        {"name", result[0][0]},
        {"email", result[0][1]}
    });
});
```

### 4.5 DbQueryLog：查询日志装饰器

`DbQueryLog`（`src/db/DbQueryLog.h`）使用**装饰器模式**，透明地包装真实连接：

```
装饰器模式：

┌───────────────────────────────────┐
│  LoggingDbConnection              │
│  ├─ m_real : shared_ptr<DbConn>  │ ← 真实连接
│  ├─ m_entries : vector<LogEntry> │ ← 查询日志
│  │                                │
│  │  query(sql, params):           │
│  │    start = now()               │
│  │    result = m_real->query(...) │
│  │    elapsed = now() - start     │
│  │    m_entries.push_back({       │
│  │      sql, elapsed, rowCount    │
│  │    })                          │
│  │    if elapsed > threshold:     │
│  │      onSlowQuery(entry)        │
│  │    return result               │
│  └────────────────────────────────┘
```

业务代码完全无感——`getDbConnection(req)` 返回的是装饰后的连接，所有 query/execute 调用都被自动记录。

### 4.6 完整请求处理流程

以 `GET /api/user/42` 为例，数据在各层间的流转：

```
[客户端] GET /api/user/42
    │
    ▼
[DbMiddleware] acquire() → MysqlConnection#7
    │
    ▼
[QueryLogMiddleware] 包装为 LoggingDbConnection
    │
    ▼
[路由处理器]
    │ getDbConnection(req) → LoggingDbConnection
    │ conn->query("SELECT ... WHERE id = ?", {"42"})
    │     │
    │     ▼
    │ [LoggingDbConnection] 转发给 MysqlConnection#7
    │     │
    │     ▼
    │ [MysqlConnection#7]
    │     │ StmtCache.find("SELECT ... WHERE id = ?")
    │     │     └─ 命中！返回 statement#15
    │     │ async_execute(stmt#15.bind("42"), results)
    │     │     └─ MySQL 服务器执行，返回 1 行
    │     │ convertResults() → DbResult
    │     └─ 返回 DbResult
    │     
    │ [LoggingDbConnection] 记录: sql="SELECT...", 230μs, 1行
    │
    │ HttpResponse::json({...})
    │
    ▼
[QueryLogMiddleware] 触发 onRequestComplete 回调
    │
    ▼
[DbMiddleware] COMMIT + release(MysqlConnection#7)
    │
    ▼
[客户端] 200 OK {"name": "Hical", "email": "Hical@example.com"}
```

---

## 5. 练习题

### 练习 1：协程式 CRUD

编写一个完整的协程式 CRUD 程序，对 `users` 表执行增删改查：

```cpp
// 要求：
// 1. 使用 any_connection 连接 MySQL
// 2. 创建 users 表（id, name, email, age）
// 3. 插入 3 条记录
// 4. 查询所有记录并打印
// 5. 按 name 更新某条记录的 email
// 6. 删除 age < 18 的记录
// 7. 查询剩余记录数量

// 提示：所有操作使用 co_await + use_awaitable
```

<details>
<summary>参考答案</summary>

```cpp
// exercise1_crud.cpp
// 编译：g++ -std=c++20 exercise1_crud.cpp -lboost_charconv -lssl -lcrypto -lpthread -o exercise1

#include <boost/mysql.hpp>
#include <boost/asio.hpp>
#include <iostream>

namespace mysql = boost::mysql;
namespace asio = boost::asio;

asio::awaitable<void> run()
{
    auto executor = co_await asio::this_coro::executor;
    mysql::any_connection conn(executor);

    // 1. 连接 MySQL
    mysql::connect_params params;
    params.server_address.emplace_host_and_port("127.0.0.1", 3306);
    params.username = "root";
    params.password = "";
    params.database = "test_db";
    co_await conn.async_connect(params, asio::use_awaitable);
    conn.set_meta_mode(mysql::metadata_mode::full);
    std::cout << "已连接 MySQL\n";

    mysql::results result;

    // 2. 创建表
    co_await conn.async_execute(
        "DROP TABLE IF EXISTS users", result, asio::use_awaitable);
    co_await conn.async_execute(
        "CREATE TABLE users ("
        "  id INT AUTO_INCREMENT PRIMARY KEY,"
        "  name VARCHAR(64) NOT NULL,"
        "  email VARCHAR(128),"
        "  age INT NOT NULL"
        ")", result, asio::use_awaitable);
    std::cout << "表已创建\n";

    // 3. 插入 3 条记录（使用 PreparedStatement）
    auto insertStmt = co_await conn.async_prepare_statement(
        "INSERT INTO users (name, email, age) VALUES (?, ?, ?)",
        asio::use_awaitable);

    co_await conn.async_execute(
        insertStmt.bind("Hical", "Hical@example.com", 25),
        result, asio::use_awaitable);
    std::cout << "插入 Hical, ID = " << result.last_insert_id() << "\n";

    co_await conn.async_execute(
        insertStmt.bind("Bob", "bob@example.com", 16),
        result, asio::use_awaitable);
    std::cout << "插入 Bob, ID = " << result.last_insert_id() << "\n";

    co_await conn.async_execute(
        insertStmt.bind("Charlie", "charlie@example.com", 30),
        result, asio::use_awaitable);
    std::cout << "插入 Charlie, ID = " << result.last_insert_id() << "\n";

    // 4. 查询所有记录并打印
    co_await conn.async_execute(
        "SELECT id, name, email, age FROM users ORDER BY id",
        result, asio::use_awaitable);

    std::cout << "\n--- 所有用户 ---\n";
    for (auto row : result.rows())
    {
        std::cout << "ID: " << row[0].as_int64()
                  << " | Name: " << row[1].as_string()
                  << " | Email: " << row[2].as_string()
                  << " | Age: " << row[3].as_int64() << "\n";
    }

    // 5. 按 name 更新 email
    auto updateStmt = co_await conn.async_prepare_statement(
        "UPDATE users SET email = ? WHERE name = ?",
        asio::use_awaitable);
    co_await conn.async_execute(
        updateStmt.bind("Hical_new@example.com", "Hical"),
        result, asio::use_awaitable);
    std::cout << "\n更新 Hical 的 email，影响 " << result.affected_rows() << " 行\n";

    // 6. 删除 age < 18 的记录
    auto deleteStmt = co_await conn.async_prepare_statement(
        "DELETE FROM users WHERE age < ?",
        asio::use_awaitable);
    co_await conn.async_execute(
        deleteStmt.bind(18),
        result, asio::use_awaitable);
    std::cout << "删除 age < 18 的用户，影响 " << result.affected_rows() << " 行\n";

    // 7. 查询剩余记录数量
    co_await conn.async_execute(
        "SELECT COUNT(*) AS cnt FROM users",
        result, asio::use_awaitable);
    std::cout << "剩余用户数: " << result.rows()[0][0].as_int64() << "\n";

    // 查看剩余用户
    co_await conn.async_execute(
        "SELECT id, name, email, age FROM users ORDER BY id",
        result, asio::use_awaitable);
    std::cout << "\n--- 剩余用户 ---\n";
    for (auto row : result.rows())
    {
        std::cout << "ID: " << row[0].as_int64()
                  << " | Name: " << row[1].as_string()
                  << " | Email: " << row[2].as_string()
                  << " | Age: " << row[3].as_int64() << "\n";
    }

    // 清理 statement
    co_await conn.async_close_statement(insertStmt, asio::use_awaitable);
    co_await conn.async_close_statement(updateStmt, asio::use_awaitable);
    co_await conn.async_close_statement(deleteStmt, asio::use_awaitable);

    co_await conn.async_close(asio::use_awaitable);
    std::cout << "\n连接已关闭\n";
}

int main()
{
    asio::io_context ioCtx;
    asio::co_spawn(ioCtx, run(), [](std::exception_ptr ep)
    {
        if (ep)
        {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) { std::cerr << "错误: " << e.what() << "\n"; }
        }
    });
    ioCtx.run();
    return 0;
}
```

**预期输出**：

```
已连接 MySQL
表已创建
插入 Hical, ID = 1
插入 Bob, ID = 2
插入 Charlie, ID = 3

--- 所有用户 ---
ID: 1 | Name: Hical | Email: Hical@example.com | Age: 25
ID: 2 | Name: Bob | Email: bob@example.com | Age: 16
ID: 3 | Name: Charlie | Email: charlie@example.com | Age: 30

更新 Hical 的 email，影响 1 行
删除 age < 18 的用户，影响 1 行
剩余用户数: 2

--- 剩余用户 ---
ID: 1 | Name: Hical | Email: Hical_new@example.com | Age: 25
ID: 3 | Name: Charlie | Email: charlie@example.com | Age: 30

连接已关闭
```

</details>

### 练习 2：参数化查询实战

对比文本查询和 PreparedStatement 的安全性：

```cpp
// 要求：
// 1. 编写一个函数 unsafeQuery(conn, userInput)，使用文本拼接
// 2. 编写一个函数 safeQuery(conn, userInput)，使用 PreparedStatement
// 3. 用输入 "'; DROP TABLE users; --" 测试两者
// 4. 观察并解释为什么 PreparedStatement 是安全的

// 思考：客户端格式化 (with_params) 和 PreparedStatement 的安全性有什么区别？
```

<details>
<summary>参考答案</summary>

```cpp
// exercise2_sql_injection.cpp
// 编译：g++ -std=c++20 exercise2_sql_injection.cpp -lboost_charconv -lssl -lcrypto -lpthread -o exercise2

#include <boost/mysql.hpp>
#include <boost/asio.hpp>
#include <iostream>

namespace mysql = boost::mysql;
namespace asio = boost::asio;

// ❌ 危险：文本拼接查询——永远不要这么做！
asio::awaitable<void> unsafeQuery(mysql::any_connection& conn,
                                   std::string_view userInput)
{
    // 直接拼接用户输入到 SQL 中
    std::string sql = "SELECT id, name FROM users WHERE name = '" +
                      std::string(userInput) + "'";

    std::cout << "[UNSAFE] 执行 SQL: " << sql << "\n";

    try
    {
        mysql::results result;
        co_await conn.async_execute(sql, result, asio::use_awaitable);
        std::cout << "[UNSAFE] 返回 " << result.rows().size() << " 行\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "[UNSAFE] 错误: " << e.what() << "\n";
    }
}

// ✅ 安全：PreparedStatement 参数化查询
asio::awaitable<void> safeQuery(mysql::any_connection& conn,
                                 std::string_view userInput)
{
    std::cout << "[SAFE] 输入: \"" << userInput << "\"\n";

    try
    {
        auto stmt = co_await conn.async_prepare_statement(
            "SELECT id, name FROM users WHERE name = ?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(
            stmt.bind(userInput),
            result, asio::use_awaitable);

        std::cout << "[SAFE] 返回 " << result.rows().size() << " 行";
        if (!result.rows().empty())
        {
            std::cout << " (第一行: " << result.rows()[0][1].as_string() << ")";
        }
        std::cout << "\n";

        co_await conn.async_close_statement(stmt, asio::use_awaitable);
    }
    catch (const std::exception& e)
    {
        std::cout << "[SAFE] 错误: " << e.what() << "\n";
    }
}

asio::awaitable<void> run()
{
    auto executor = co_await asio::this_coro::executor;
    mysql::any_connection conn(executor);

    mysql::connect_params params;
    params.server_address.emplace_host_and_port("127.0.0.1", 3306);
    params.username = "root";
    params.password = "";
    params.database = "test_db";
    co_await conn.async_connect(params, asio::use_awaitable);

    // 准备测试数据
    mysql::results r;
    co_await conn.async_execute("DROP TABLE IF EXISTS users", r, asio::use_awaitable);
    co_await conn.async_execute(
        "CREATE TABLE users (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64))",
        r, asio::use_awaitable);
    co_await conn.async_execute(
        "INSERT INTO users (name) VALUES ('Hical'), ('Bob')",
        r, asio::use_awaitable);
    std::cout << "=== 测试数据准备完成 ===\n\n";

    // 正常输入测试
    std::cout << "--- 正常输入: \"Hical\" ---\n";
    co_await unsafeQuery(conn, "Hical");
    co_await safeQuery(conn, "Hical");

    // SQL 注入攻击测试
    std::string malicious = "'; DROP TABLE users; --";
    std::cout << "\n--- 恶意输入: \"" << malicious << "\" ---\n";

    // 先用安全方式测试（不会破坏表）
    co_await safeQuery(conn, malicious);

    // 验证表仍然存在
    co_await conn.async_execute("SELECT COUNT(*) FROM users", r, asio::use_awaitable);
    std::cout << "[验证] 安全查询后 users 表仍有 "
              << r.rows()[0][0].as_int64() << " 行\n";

    // 再用危险方式测试（可能破坏表！）
    co_await unsafeQuery(conn, malicious);

    // 验证表是否还存在
    try
    {
        co_await conn.async_execute("SELECT COUNT(*) FROM users", r, asio::use_awaitable);
        std::cout << "[验证] 不安全查询后 users 表有 "
                  << r.rows()[0][0].as_int64() << " 行\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "[验证] 表已被删除！错误: " << e.what() << "\n";
    }

    co_await conn.async_close(asio::use_awaitable);
}

int main()
{
    asio::io_context ioCtx;
    asio::co_spawn(ioCtx, run(), [](std::exception_ptr ep)
    {
        if (ep)
        {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) { std::cerr << "致命错误: " << e.what() << "\n"; }
        }
    });
    ioCtx.run();
    return 0;
}
```

**关键解释**：

- **文本拼接**：`"SELECT ... WHERE name = '' ; DROP TABLE users; --'"` —— 分号让 MySQL 执行第二条 SQL，`--` 注释掉尾部引号。Boost.MySQL 的 `async_execute` 默认不支持多语句，因此这个特定攻击可能失败，但其他注入形式（如 `' OR 1=1 --`）仍然有效。

- **PreparedStatement**：参数值在 MySQL 协议层面作为**数据**传输，而非 SQL 语句的一部分。无论用户输入什么内容（包含引号、分号、注释符），都只会被当成 `name` 列的查找值，永远不会被解释为 SQL 语法。

- **`with_params` vs PreparedStatement**：`with_params` 在客户端做转义后拼接成完整 SQL 发送，安全性依赖转义逻辑的正确性。PreparedStatement 在协议层面分离 SQL 结构和数据，是更根本的安全保障。对于处理用户输入的场景，PreparedStatement 始终是首选。

</details>

### 练习 3：事务与错误处理

实现一个转账功能，要求正确处理各种异常场景：

```cpp
// 要求：
// 1. 实现 transfer(conn, fromId, toId, amount)
// 2. 使用事务保证原子性
// 3. 检查余额不足、账户不存在等情况
// 4. 网络异常时确保事务被回滚
// 5. 返回 TransferResult{success, message}

// 思考：如何防止同一用户并发转账导致超额扣款？（提示：SELECT ... FOR UPDATE）
```

<details>
<summary>参考答案</summary>

```cpp
// exercise3_transaction.cpp
// 编译：g++ -std=c++20 exercise3_transaction.cpp -lboost_charconv -lssl -lcrypto -lpthread -o exercise3

#include <boost/mysql.hpp>
#include <boost/asio.hpp>
#include <iostream>

namespace mysql = boost::mysql;
namespace asio = boost::asio;

struct TransferResult
{
    bool success;
    std::string message;
};

// 转账函数——事务 + FOR UPDATE 行锁 防并发超额扣款
asio::awaitable<TransferResult> transfer(mysql::any_connection& conn,
                                          int64_t fromId,
                                          int64_t toId,
                                          int64_t amount)
{
    if (amount <= 0)
    {
        co_return TransferResult{false, "转账金额必须大于 0"};
    }
    if (fromId == toId)
    {
        co_return TransferResult{false, "不能给自己转账"};
    }

    mysql::results r;

    // 开启事务
    co_await conn.async_execute("START TRANSACTION", r, asio::use_awaitable);

    try
    {
        // 使用 SELECT ... FOR UPDATE 加行锁
        // 按 ID 顺序锁定，防止 A→B 和 B→A 并发时产生死锁
        int64_t firstId = std::min(fromId, toId);
        int64_t secondId = std::max(fromId, toId);

        auto lockStmt = co_await conn.async_prepare_statement(
            "SELECT id, gold FROM accounts WHERE id IN (?, ?) ORDER BY id FOR UPDATE",
            asio::use_awaitable);

        co_await conn.async_execute(
            lockStmt.bind(firstId, secondId),
            r, asio::use_awaitable);

        co_await conn.async_close_statement(lockStmt, asio::use_awaitable);

        // 检查两个账户是否都存在
        if (r.rows().size() < 2)
        {
            co_await conn.async_execute("ROLLBACK", r, asio::use_awaitable);
            co_return TransferResult{false, "转出或转入账户不存在"};
        }

        // 找出转出方余额
        int64_t fromGold = 0;
        for (auto row : r.rows())
        {
            if (row[0].as_int64() == fromId)
            {
                fromGold = row[1].as_int64();
                break;
            }
        }

        // 余额检查
        if (fromGold < amount)
        {
            co_await conn.async_execute("ROLLBACK", r, asio::use_awaitable);
            co_return TransferResult{false,
                "余额不足（当前: " + std::to_string(fromGold) +
                "，需要: " + std::to_string(amount) + "）"};
        }

        // 扣款
        auto deductStmt = co_await conn.async_prepare_statement(
            "UPDATE accounts SET gold = gold - ? WHERE id = ?",
            asio::use_awaitable);
        co_await conn.async_execute(
            deductStmt.bind(amount, fromId),
            r, asio::use_awaitable);
        co_await conn.async_close_statement(deductStmt, asio::use_awaitable);

        // 加款
        auto addStmt = co_await conn.async_prepare_statement(
            "UPDATE accounts SET gold = gold + ? WHERE id = ?",
            asio::use_awaitable);
        co_await conn.async_execute(
            addStmt.bind(amount, toId),
            r, asio::use_awaitable);
        co_await conn.async_close_statement(addStmt, asio::use_awaitable);

        // 提交事务
        co_await conn.async_execute("COMMIT", r, asio::use_awaitable);

        co_return TransferResult{true,
            "转账成功：" + std::to_string(fromId) + " → " +
            std::to_string(toId) + "，金额 " + std::to_string(amount)};
    }
    catch (...)
    {
        // 网络异常或其他错误——回滚事务
        try
        {
            co_await conn.async_execute("ROLLBACK", r, asio::use_awaitable);
        }
        catch (...)
        {
            // 回滚本身也可能失败（连接已断开），忽略
        }
        throw;  // 向上层传播异常
    }
}

asio::awaitable<void> run()
{
    auto executor = co_await asio::this_coro::executor;
    mysql::any_connection conn(executor);

    mysql::connect_params params;
    params.server_address.emplace_host_and_port("127.0.0.1", 3306);
    params.username = "root";
    params.password = "";
    params.database = "test_db";
    co_await conn.async_connect(params, asio::use_awaitable);
    conn.set_meta_mode(mysql::metadata_mode::full);

    // 准备测试数据
    mysql::results r;
    co_await conn.async_execute("DROP TABLE IF EXISTS accounts", r, asio::use_awaitable);
    co_await conn.async_execute(
        "CREATE TABLE accounts ("
        "  id INT PRIMARY KEY,"
        "  name VARCHAR(32) NOT NULL,"
        "  gold BIGINT NOT NULL DEFAULT 0"
        ")", r, asio::use_awaitable);
    co_await conn.async_execute(
        "INSERT INTO accounts VALUES (1, 'Hical', 1000), (2, 'Bob', 500)",
        r, asio::use_awaitable);

    auto printBalances = [&]() -> asio::awaitable<void>
    {
        co_await conn.async_execute(
            "SELECT id, name, gold FROM accounts ORDER BY id",
            r, asio::use_awaitable);
        for (auto row : r.rows())
        {
            std::cout << "  " << row[1].as_string()
                      << "(ID:" << row[0].as_int64()
                      << ") 余额: " << row[2].as_int64() << "\n";
        }
    };

    std::cout << "=== 初始余额 ===\n";
    co_await printBalances();

    // 测试 1：正常转账
    std::cout << "\n--- 测试 1: Hical 转给 Bob 200 ---\n";
    auto result1 = co_await transfer(conn, 1, 2, 200);
    std::cout << (result1.success ? "✅ " : "❌ ") << result1.message << "\n";
    co_await printBalances();

    // 测试 2：余额不足
    std::cout << "\n--- 测试 2: Hical 转给 Bob 10000（余额不足）---\n";
    auto result2 = co_await transfer(conn, 1, 2, 10000);
    std::cout << (result2.success ? "✅ " : "❌ ") << result2.message << "\n";
    co_await printBalances();

    // 测试 3：账户不存在
    std::cout << "\n--- 测试 3: Hical 转给不存在的账户 999 ---\n";
    auto result3 = co_await transfer(conn, 1, 999, 100);
    std::cout << (result3.success ? "✅ " : "❌ ") << result3.message << "\n";

    // 测试 4：非法金额
    std::cout << "\n--- 测试 4: 负数金额 ---\n";
    auto result4 = co_await transfer(conn, 1, 2, -100);
    std::cout << (result4.success ? "✅ " : "❌ ") << result4.message << "\n";

    co_await conn.async_close(asio::use_awaitable);
}

int main()
{
    asio::io_context ioCtx;
    asio::co_spawn(ioCtx, run(), [](std::exception_ptr ep)
    {
        if (ep)
        {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) { std::cerr << "错误: " << e.what() << "\n"; }
        }
    });
    ioCtx.run();
    return 0;
}
```

**预期输出**：

```
=== 初始余额 ===
  Hical(ID:1) 余额: 1000
  Bob(ID:2) 余额: 500

--- 测试 1: Hical 转给 Bob 200 ---
✅ 转账成功：1 → 2，金额 200
  Hical(ID:1) 余额: 800
  Bob(ID:2) 余额: 700

--- 测试 2: Hical 转给 Bob 10000（余额不足）---
❌ 余额不足（当前: 800，需要: 10000）
  Hical(ID:1) 余额: 800
  Bob(ID:2) 余额: 700

--- 测试 3: Hical 转给不存在的账户 999 ---
❌ 转出或转入账户不存在

--- 测试 4: 负数金额 ---
❌ 转账金额必须大于 0
```

**防并发超额扣款的关键**：`SELECT ... FOR UPDATE` 会对选中的行加排他锁。其他事务在试图锁定同一行时会被阻塞，直到当前事务提交或回滚。按 ID 顺序加锁可以避免 A→B 和 B→A 并发时的死锁。

</details>

### 练习 4：LRU 缓存设计

参考 Hical 的 `StmtCache`，实现一个通用 LRU 缓存：

```cpp
// 要求：
// 1. 模板类 LruCache<Key, Value>，支持 maxSize
// 2. find(key) → Value*，命中时提升到 MRU
// 3. insert(key, value) → optional<Value>，满时淘汰 LRU
// 4. erase(key) → optional<Value>
// 5. 所有操作 O(1)

// 进阶：支持透明哈希（string_view 查找 string key）
```

<details>
<summary>参考答案</summary>

```cpp
// exercise4_lru_cache.cpp
// 编译：g++ -std=c++20 exercise4_lru_cache.cpp -o exercise4

#include <cassert>
#include <iostream>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// ============ 通用 LRU 缓存 ============

template <typename Key, typename Value,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class LruCache
{
public:
    explicit LruCache(size_t maxSize) : m_maxSize(maxSize) {}

    /// 查找，命中时提升到 MRU。返回 nullptr 表示未命中。
    Value* find(const Key& key)
    {
        auto it = m_map.find(key);
        if (it == m_map.end())
        {
            return nullptr;
        }
        // 提升到 MRU（链表头部）
        m_list.splice(m_list.begin(), m_list, it->second);
        return &(it->second->second);
    }

    /// 插入。满时淘汰 LRU，返回被淘汰的 Value。
    std::optional<Value> insert(const Key& key, Value value)
    {
        std::optional<Value> evicted;

        // 已存在 → 更新值并提升
        auto it = m_map.find(key);
        if (it != m_map.end())
        {
            it->second->second = std::move(value);
            m_list.splice(m_list.begin(), m_list, it->second);
            return evicted; // nullopt
        }

        // 满了 → 淘汰 LRU（链表尾部）
        if (m_list.size() >= m_maxSize && m_maxSize > 0)
        {
            auto& back = m_list.back();
            evicted = std::move(back.second);
            m_map.erase(back.first);
            m_list.pop_back();
        }

        // 插入到 MRU 位置（链表头部）
        m_list.emplace_front(key, std::move(value));
        m_map[key] = m_list.begin();
        return evicted;
    }

    /// 移除指定 key
    std::optional<Value> erase(const Key& key)
    {
        auto it = m_map.find(key);
        if (it == m_map.end())
        {
            return std::nullopt;
        }
        auto value = std::move(it->second->second);
        m_list.erase(it->second);
        m_map.erase(it);
        return value;
    }

    size_t size() const { return m_list.size(); }
    size_t maxSize() const { return m_maxSize; }
    bool empty() const { return m_list.empty(); }

private:
    size_t m_maxSize;
    using Entry = std::pair<Key, Value>;
    std::list<Entry> m_list; // front = MRU, back = LRU
    std::unordered_map<Key, typename std::list<Entry>::iterator, Hash, Equal> m_map;
};

// ============ 进阶：支持透明哈希的特化版本 ============

struct TransparentHash
{
    using is_transparent = void;
    size_t operator()(std::string_view sv) const
    {
        return std::hash<std::string_view> {}(sv);
    }
    size_t operator()(const std::string& s) const
    {
        return std::hash<std::string_view> {}(s);
    }
};

struct TransparentEqual
{
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const
    {
        return a == b;
    }
};

// string key 但支持 string_view 查找
using StringLruCache = LruCache<std::string, int, TransparentHash, TransparentEqual>;

// ============ 测试 ============

int main()
{
    std::cout << "=== 基本功能测试 ===\n";

    LruCache<int, std::string> cache(3);

    // 插入 3 个元素
    assert(!cache.insert(1, "one").has_value());
    assert(!cache.insert(2, "two").has_value());
    assert(!cache.insert(3, "three").has_value());
    assert(cache.size() == 3);
    std::cout << "✅ 插入 3 个元素，size = " << cache.size() << "\n";

    // 查找命中
    auto* val = cache.find(1);
    assert(val && *val == "one");
    std::cout << "✅ find(1) = \"" << *val << "\"\n";

    // 查找未命中
    assert(cache.find(999) == nullptr);
    std::cout << "✅ find(999) = nullptr\n";

    // 插入第 4 个，应淘汰 LRU（key=2，因为 key=1 刚被 find 提升到 MRU）
    auto evicted = cache.insert(4, "four");
    assert(evicted.has_value() && *evicted == "two");
    std::cout << "✅ 插入 4 时淘汰了 \"" << *evicted << "\"\n";

    // key=2 已被淘汰
    assert(cache.find(2) == nullptr);
    std::cout << "✅ find(2) = nullptr（已淘汰）\n";

    // erase
    auto erased = cache.erase(3);
    assert(erased.has_value() && *erased == "three");
    assert(cache.size() == 2);
    std::cout << "✅ erase(3) = \"" << *erased << "\", size = " << cache.size() << "\n";

    // 重复 key 更新
    cache.insert(1, "ONE_UPDATED");
    val = cache.find(1);
    assert(val && *val == "ONE_UPDATED");
    std::cout << "✅ 更新 key=1 为 \"" << *val << "\"\n";

    std::cout << "\n=== 透明哈希测试 ===\n";

    StringLruCache strCache(2);
    strCache.insert("hello", 100);
    strCache.insert("world", 200);

    // 用 string_view 查找 string key（零堆分配）
    std::string_view sv = "hello";
    auto* intVal = strCache.find(sv);
    assert(intVal && *intVal == 100);
    std::cout << "✅ 透明哈希 find(\"hello\") = " << *intVal << "\n";

    std::cout << "\n所有测试通过！\n";
    return 0;
}
```

**预期输出**：

```
=== 基本功能测试 ===
✅ 插入 3 个元素，size = 3
✅ find(1) = "one"
✅ find(999) = nullptr
✅ 插入 4 时淘汰了 "two"
✅ find(2) = nullptr（已淘汰）
✅ erase(3) = "three", size = 2
✅ 更新 key=1 为 "ONE_UPDATED"

=== 透明哈希测试 ===
✅ 透明哈希 find("hello") = 100

所有测试通过！
```

**核心数据结构选择**：`std::list`（双向链表）+ `std::unordered_map`（哈希表）组合。链表提供 O(1) 的头插和尾删，哈希表提供 O(1) 的查找。`splice()` 是 `std::list` 的成员函数，可以 O(1) 将节点移动到头部，不涉及分配和释放。

</details>

### 练习 5（挑战）：连接池实现

设计一个简化版的协程式连接池：

```cpp
// 要求：
// 1. 支持 minSize / maxSize 配置
// 2. acquire() 返回 Awaitable<shared_ptr<Connection>>
// 3. 池满时协程挂起等待（用 steady_timer 实现）
// 4. release() 归还连接并唤醒等待者
// 5. 支持超时（acquireTimeout）
// 6. 归还时自动回滚残留事务

// 思考：
// - 为什么不能用 condition_variable？
// - LIFO 比 FIFO 好在哪里？
// - 如何处理连接断开（ping 检活）？
```

<details>
<summary>参考答案</summary>

```cpp
// exercise5_connection_pool.cpp
// 这是一个简化版连接池的完整实现，使用模拟连接代替真实 MySQL。
// 编译：g++ -std=c++20 exercise5_connection_pool.cpp -lboost_system -lpthread -o exercise5

#include <boost/asio.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

namespace asio = boost::asio;
using Awaitable = asio::awaitable<void>;

// ============ 模拟连接接口 ============

class Connection
{
public:
    explicit Connection(int id) : m_id(id) {}
    virtual ~Connection() = default;

    int id() const { return m_id; }

    virtual bool inTransaction() const { return m_inTx; }

    virtual asio::awaitable<void> rollback()
    {
        std::cout << "  [Conn#" << m_id << "] ROLLBACK\n";
        m_inTx = false;
        co_return;
    }

    virtual asio::awaitable<void> beginTransaction()
    {
        std::cout << "  [Conn#" << m_id << "] BEGIN\n";
        m_inTx = true;
        co_return;
    }

    virtual asio::awaitable<bool> ping()
    {
        co_return m_alive;
    }

    void setAlive(bool alive) { m_alive = alive; }

    // 模拟业务：设置一个标记表示在事务中
    void simulateLeftoverTransaction() { m_inTx = true; }

private:
    int m_id;
    bool m_alive = true;
    bool m_inTx = false;
};

// ============ 连接工厂 ============

using ConnectionFactory = std::function<
    asio::awaitable<std::shared_ptr<Connection>>(asio::io_context&)>;

// ============ 简化版协程连接池 ============

class SimplePool : public std::enable_shared_from_this<SimplePool>
{
public:
    struct Config
    {
        size_t minSize = 2;
        size_t maxSize = 4;
        std::chrono::seconds acquireTimeout {3};
    };

    SimplePool(asio::io_context& ioCtx, Config config, ConnectionFactory factory)
        : m_ioCtx(ioCtx), m_config(std::move(config)), m_factory(std::move(factory))
    {
    }

    // 初始化：预创建 minSize 个连接
    asio::awaitable<void> init()
    {
        for (size_t i = 0; i < m_config.minSize; ++i)
        {
            auto conn = co_await m_factory(m_ioCtx);
            std::lock_guard lock(m_mutex);
            m_idle.push_back(std::move(conn));
        }
        std::cout << "[Pool] 初始化完成，预创建 " << m_config.minSize << " 个连接\n";
    }

    // 获取连接
    asio::awaitable<std::shared_ptr<Connection>> acquire()
    {
        std::unique_lock lock(m_mutex);

        // 1. 有空闲连接 → 直接返回（LIFO）
        while (!m_idle.empty())
        {
            auto conn = std::move(m_idle.back());
            m_idle.pop_back();
            ++m_activeCount;
            lock.unlock();

            // ping 检活
            bool alive = co_await conn->ping();
            if (alive)
            {
                std::cout << "[Pool] acquire → Conn#" << conn->id()
                          << "（空闲复用）\n";
                co_return conn;
            }

            // 连接已死，重试
            lock.lock();
            --m_activeCount;
        }

        // 2. 未达上限 → 创建新连接
        if (m_activeCount + m_idle.size() < m_config.maxSize)
        {
            ++m_activeCount;
            lock.unlock();
            auto conn = co_await m_factory(m_ioCtx);
            std::cout << "[Pool] acquire → Conn#" << conn->id()
                      << "（新建）\n";
            co_return conn;
        }

        // 3. 池满 → 协程挂起等待
        std::cout << "[Pool] 池满，等待归还...\n";
        auto timer = std::make_shared<asio::steady_timer>(
            m_ioCtx, m_config.acquireTimeout);
        auto result = std::make_shared<std::shared_ptr<Connection>>();
        m_waiters.push_back({timer, result});
        lock.unlock();

        boost::system::error_code ec;
        co_await timer->async_wait(
            asio::redirect_error(asio::use_awaitable, ec));

        if (*result)
        {
            std::lock_guard countLock(m_mutex);
            ++m_activeCount;
            std::cout << "[Pool] acquire → Conn#" << (*result)->id()
                      << "（等待后获取）\n";
            co_return std::move(*result);
        }

        throw std::runtime_error("acquire timeout");
    }

    // 归还连接
    void release(std::shared_ptr<Connection> conn)
    {
        if (!conn) return;

        // 检测残留事务，异步回滚
        if (conn->inTransaction())
        {
            auto self = shared_from_this();
            asio::co_spawn(m_ioCtx,
                [self, conn]() mutable -> asio::awaitable<void>
                {
                    try
                    {
                        co_await conn->rollback();
                    }
                    catch (...)
                    {
                        std::lock_guard lock(self->m_mutex);
                        --self->m_activeCount;
                        co_return;
                    }
                    self->returnToIdle(std::move(conn));
                },
                asio::detached);
            return;
        }

        returnToIdle(std::move(conn));
    }

    size_t activeCount() const { std::lock_guard l(m_mutex); return m_activeCount; }
    size_t idleCount() const { std::lock_guard l(m_mutex); return m_idle.size(); }

private:
    void returnToIdle(std::shared_ptr<Connection> conn)
    {
        std::lock_guard lock(m_mutex);
        --m_activeCount;

        // 有等待者 → 直接转交
        if (!m_waiters.empty())
        {
            auto waiter = std::move(m_waiters.front());
            m_waiters.pop_front();
            *(waiter.result) = std::move(conn);
            waiter.timer->cancel();
            return;
        }

        // 无等待者 → 归入空闲栈
        std::cout << "[Pool] release Conn#" << conn->id() << " 回空闲池\n";
        m_idle.push_back(std::move(conn));
    }

    asio::io_context& m_ioCtx;
    Config m_config;
    ConnectionFactory m_factory;

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Connection>> m_idle;  // LIFO 栈
    size_t m_activeCount = 0;

    struct Waiter
    {
        std::shared_ptr<asio::steady_timer> timer;
        std::shared_ptr<std::shared_ptr<Connection>> result;
    };
    std::deque<Waiter> m_waiters;
};

// ============ 测试 ============

asio::awaitable<void> run()
{
    auto executor = co_await asio::this_coro::executor;
    auto& ioCtx = static_cast<asio::io_context&>(executor.context());

    int nextId = 1;
    auto factory = [&nextId](asio::io_context&) -> asio::awaitable<std::shared_ptr<Connection>>
    {
        co_return std::make_shared<Connection>(nextId++);
    };

    auto pool = std::make_shared<SimplePool>(
        ioCtx,
        SimplePool::Config{.minSize = 2, .maxSize = 3, .acquireTimeout{2}},
        factory);

    co_await pool->init();

    // 测试 1：正常获取和归还
    std::cout << "\n--- 测试 1: 正常获取和归还 ---\n";
    auto c1 = co_await pool->acquire();
    auto c2 = co_await pool->acquire();
    std::cout << "活跃: " << pool->activeCount()
              << ", 空闲: " << pool->idleCount() << "\n";

    pool->release(c1);
    pool->release(c2);
    // 短暂让出执行权，让回滚协程（如果有）执行完
    asio::steady_timer pause(ioCtx, std::chrono::milliseconds(10));
    co_await pause.async_wait(asio::use_awaitable);
    std::cout << "归还后 → 活跃: " << pool->activeCount()
              << ", 空闲: " << pool->idleCount() << "\n";

    // 测试 2：LIFO 验证
    std::cout << "\n--- 测试 2: LIFO 验证 ---\n";
    auto a = co_await pool->acquire();  // 应该拿到最后归还的
    std::cout << "LIFO: 获取 Conn#" << a->id() << "\n";
    pool->release(a);
    co_await pause.async_wait(asio::use_awaitable);

    // 测试 3：池满等待
    std::cout << "\n--- 测试 3: 池满 + 归还唤醒 ---\n";
    auto x1 = co_await pool->acquire();
    auto x2 = co_await pool->acquire();
    auto x3 = co_await pool->acquire();
    std::cout << "已获取 3 个连接（满）\n";

    // 1 秒后异步归还一个
    asio::steady_timer delayRelease(ioCtx, std::chrono::seconds(1));
    asio::co_spawn(ioCtx,
        [&delayRelease, pool, x1]() mutable -> asio::awaitable<void>
        {
            co_await delayRelease.async_wait(asio::use_awaitable);
            std::cout << "[异步] 归还 Conn#" << x1->id() << "\n";
            pool->release(x1);
        },
        asio::detached);

    auto x4 = co_await pool->acquire();  // 应该在 1 秒后被唤醒
    std::cout << "等待后获取: Conn#" << x4->id() << "\n";
    pool->release(x2);
    pool->release(x3);
    pool->release(x4);
    co_await pause.async_wait(asio::use_awaitable);

    // 测试 4：残留事务自动回滚
    std::cout << "\n--- 测试 4: 残留事务自动回滚 ---\n";
    auto tc = co_await pool->acquire();
    co_await tc->beginTransaction();
    std::cout << "模拟忘记 commit，直接 release\n";
    pool->release(tc);
    // 等待异步回滚完成
    asio::steady_timer wait(ioCtx, std::chrono::milliseconds(50));
    co_await wait.async_wait(asio::use_awaitable);

    std::cout << "\n✅ 所有测试完成\n";
}

int main()
{
    asio::io_context ioCtx;
    asio::co_spawn(ioCtx, run(), [](std::exception_ptr ep)
    {
        if (ep)
        {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) { std::cerr << "错误: " << e.what() << "\n"; }
        }
    });
    ioCtx.run();
    return 0;
}
```

**思考题解答**：

1. **为什么不能用 condition_variable？**
   `condition_variable::wait()` 会阻塞当前线程。在 `1 Thread : 1 io_context` 模型中，线程被阻塞意味着该 io_context 上的所有协程都无法推进——包括负责归还连接的那个协程。结果是死锁。`steady_timer` + `co_await` 只挂起当前协程，线程继续运行其他协程。

2. **LIFO 比 FIFO 好在哪里？**
   LIFO 让最近归还的连接优先被复用，TCP 状态更热（keepalive 内）、MySQL 端的线程缓存命中率更高。不活跃的连接沉底，被空闲回收循环自然清理。FIFO 会均匀使用所有连接，导致更多连接处于活跃状态，消耗更多 MySQL 服务器资源。

3. **如何处理连接断开？**
   三层防护：(a) `acquire()` 时 ping 检活，死连接直接丢弃；(b) 后台 `healthCheckLoop` 定期 ping 空闲连接，剔除死连接并补充到 minSize；(c) `MysqlConnection::query()` 执行失败后自动重试（PreparedStatement 失效场景）。

</details>

---

## 6. 总结与拓展阅读

### 核心 API 速查表

| API                            | 说明                   | 返回类型                   |
| ------------------------------ | ---------------------- | -------------------------- |
| `any_connection(executor)`     | 创建类型擦除连接       | —                          |
| `async_connect(params)`        | 异步连接服务器         | `awaitable<void>`          |
| `async_execute(query, res)`    | 执行查询               | `awaitable<void>`          |
| `async_prepare_statement(sql)` | 预编译 SQL             | `awaitable<statement>`     |
| `stmt.bind(args...)`           | 绑定参数               | bound statement            |
| `async_close_statement(stmt)`  | 关闭 PreparedStatement | `awaitable<void>`          |
| `async_close()`                | 关闭连接               | `awaitable<void>`          |
| `async_ping()`                 | 检测连接存活           | `awaitable<void>`          |
| `results.rows()`               | 获取行集合             | `rows_view`                |
| `results.meta()`               | 获取列元信息           | `metadata_collection_view` |
| `results.affected_rows()`      | 影响行数               | `uint64_t`                 |
| `results.last_insert_id()`     | 最后插入 ID            | `uint64_t`                 |
| `field_view.as_int64()`        | 取整数值               | `int64_t`                  |
| `field_view.as_string()`       | 取字符串值             | `string_view`              |
| `field_view.is_null()`         | 是否为 NULL            | `bool`                     |
| `with_params(fmt, args...)`    | 客户端安全格式化       | formattable query          |

### 查询方式对比

| 方式              | 安全性 | 网络往返 | 可复用 | 适用场景             |
| ----------------- | ------ | -------- | ------ | -------------------- |
| 文本查询          | ❌ 低   | 1 次     | —      | DDL、SET、静态 SQL   |
| 客户端格式化      | ✅ 中   | 1 次     | —      | 简单动态查询         |
| PreparedStatement | ✅✅ 高  | 2 次*    | ✅      | 带用户输入的业务查询 |

\* 配合 StmtCache 后首次 2 次，后续 1 次。

### 拓展阅读

- [Boost.MySQL 官方文档](https://www.boost.org/doc/libs/release/libs/mysql/doc/html/index.html)
- [Boost.MySQL GitHub](https://github.com/boostorg/mysql)
- [MySQL 客户端/服务器协议](https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_basics.html)
- [C++ 协程与数据库：设计模式](https://www.boost.org/doc/libs/release/libs/mysql/doc/html/mysql/tutorial_with_params.html)
- [Boost.Describe 反射库](https://www.boost.org/doc/libs/release/libs/describe/doc/html/describe.html)（`static_results` 的基础）

### 课程回顾

本系列 5 门课程的知识依赖关系：

```
课程 1: Boost.System    ← error_code 是所有 I/O 操作的返回值
    │
    ▼
课程 2: Boost.Asio      ← 核心 I/O 引擎，协程基础
    │
    ├───────────────┐
    ▼               ▼
课程 3: Beast      课程 5: MySQL    ← 都构建在 Asio 异步模型之上
    │
    ▼
课程 4: Boost.JSON ← 与 HTTP / 数据库结果配合
```

回顾各课程核心要点：
- **课程 1 — Boost.System**：`error_code` + `error_category` 体系，I/O 错误处理的基石
- **课程 2 — Boost.Asio**：`io_context` + C++20 协程，异步编程的核心引擎
- **课程 3 — Boost.Beast**：HTTP/WebSocket 协议层，在 Asio 之上构建应用协议
- **课程 4 — Boost.JSON**：值类型操作、PMR 高性能分配、反射自动序列化
- **课程 5 — Boost.MySQL**：协程式数据库访问、连接池、PreparedStatement 缓存

> 有兴趣可查看 Hical 框架源码地址：[github.com/Hical61/Hical](https://github.com/Hical61/Hical)
