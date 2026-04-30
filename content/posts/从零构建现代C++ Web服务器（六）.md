+++
title = '从零构建现代C++ Web服务器（六）：数据库中间件与协程连接池'
date = '2026-04-30'
draft = false
tags = ["C++20", "数据库", "连接池", "中间件", "Boost.MySQL", "协程", "Hical"]
categories = ["从零构建现代C++ Web服务器"]
description = "系列第六篇：基于 Boost.MySQL 的协程化数据库层，涵盖后端抽象接口、LRU PreparedStatement 缓存、steady_timer 协程信号量连接池、洋葱模型 DB 中间件与装饰器查询日志。"
+++

# 从零构建现代C++ Web服务器（六）：数据库中间件与协程连接池

> **系列导航**：[第一篇：设计理念]({{< relref "从零构建现代C++ Web服务器（一）" >}}) | [第二篇：协程与内存池]({{< relref "从零构建现代C++ Web服务器（二）" >}}) | [第三篇：路由、中间件与SSL]({{< relref "从零构建现代C++ Web服务器（三）" >}}) | [第四篇：实战与性能]({{< relref "从零构建现代C++ Web服务器（四）" >}}) | [第五篇：Cookie、Session与文件服务]({{< relref "从零构建现代C++ Web服务器（五）" >}}) | [第六篇：数据库中间件](#)（本文）

## 前置知识

- 阅读过本系列前五篇（特别是第二篇的协程基础和第三篇的中间件洋葱模型）
- 了解 SQL 基础和 MySQL 数据库操作
- 了解连接池的基本概念

---

## 目录

- [1. Web 框架为什么需要数据库层](#1-web-框架为什么需要数据库层)
- [2. 架构总览：六层洋葱](#2-架构总览六层洋葱)
- [3. 后端抽象：DbConnection 接口](#3-后端抽象dbconnection-接口)
- [4. MySQL 实现：any_connection 封装](#4-mysql-实现any_connection-封装)
- [5. LRU PreparedStatement 缓存](#5-lru-preparedstatement-缓存)
- [6. 协程连接池：用 steady_timer 做信号量](#6-协程连接池用-steady_timer-做信号量)
- [7. DB 中间件：请求级连接生命周期](#7-db-中间件请求级连接生命周期)
- [8. 查询日志：装饰器模式的妙用](#8-查询日志装饰器模式的妙用)
- [9. 综合实战：用户管理 API + 数据库](#9-综合实战用户管理-api--数据库)
- [10. 总结](#10-总结)

---

## 1. Web 框架为什么需要数据库层

前五篇构建了 hical 的完整 HTTP 骨架——协程驱动的异步 I/O、PMR 内存池、路由、中间件、SSL、Cookie/Session、静态文件。但现实中的 Web 服务几乎都绑定数据库：用户注册要写库、商品查询要读库、交易扣款要事务。

如果把数据库操作留给业务代码自行处理，会出现几个典型问题：

| 问题                     | 后果                                              |
| ------------------------ | ------------------------------------------------- |
| 每个请求都新建连接       | MySQL 握手 + 认证 ≈ 1-3ms，高并发下成为瓶颈       |
| 业务代码管理连接生命周期 | 忘记关闭 → 连接泄漏，异常时忘记回滚 → 数据不一致  |
| 手动拼接 SQL             | SQL 注入漏洞（游戏服务器的经济系统被注入 = 灾难） |
| 同步 MySQL 客户端        | `mysql_query()` 阻塞 io_context 线程 → 吞吐暴跌   |

hical v2.3.0 补齐了这最后一块拼图：

| 模块                 | 解决的问题                 | 核心文件                 |
| -------------------- | -------------------------- | ------------------------ |
| **DbConnection**     | 后端抽象，异步协程化接口   | `DbConnection.h`         |
| **MysqlConnection**  | Boost.MySQL 具体实现       | `MysqlConnection.h/cpp`  |
| **StmtCache**        | PreparedStatement LRU 缓存 | `StmtCache.h/cpp`        |
| **DbConnectionPool** | 协程式连接池               | `DbConnectionPool.h/cpp` |
| **DbMiddleware**     | 请求级连接获取/归还/事务   | `DbMiddleware.h`         |
| **DbQueryLog**       | 查询耗时记录 + 慢查询告警  | `DbQueryLog.h/cpp`       |

---

## 2. 架构总览：六层洋葱

数据库层在 hical 整体架构中的位置：

```
                         ┌────────────────────────────────┐
                         │         HTTP 请求到达            │
                         └───────────────┬────────────────┘
                                         │
                         ┌───────────────▼────────────────┐
                         │    TcpServer (Accept + 协程)     │
                         └───────────────┬────────────────┘
                                         │
                         ┌───────────────▼────────────────┐
                         │   MiddlewarePipeline（洋葱模型）  │
                         │  ┌──────────────────────────┐  │
                         │  │  日志 / CORS / Session    │  │
                         │  │  ★ DbMiddleware  (本篇)   │  │ acquire → 注入连接
                         │  │  ★ QueryLogMiddleware     │  │ 装饰器包装
                         │  └──────────────────────────┘  │
                         └───────────────┬────────────────┘
                                         │
                         ┌───────────────▼────────────────┐
                         │      Router（路由分发）          │
                         └───────────────┬────────────────┘
                                         │
              ┌──────────────────────────┼──────────────────────────┐
              │                          │                          │
   ┌──────────▼──────────┐   ┌──────────▼──────────┐   ┌──────────▼──────────┐
   │  ★ getDbConnection()│   │  JSON / 普通响应     │   │  静态文件 / 上传     │
   │  conn->query(...)   │   │                      │   │                      │
   └──────────┬──────────┘   └──────────────────────┘   └──────────────────────┘
              │
   ┌──────────▼──────────┐
   │  ★ DbConnectionPool  │  LIFO 空闲栈 + steady_timer 信号量
   │  acquire() / release()│
   └──────────┬──────────┘
              │
   ┌──────────▼──────────┐
   │  ★ MysqlConnection   │  boost::mysql::any_connection
   │  + StmtCache (LRU)   │  PreparedStatement 缓存
   └──────────┬──────────┘
              │
   ┌──────────▼──────────┐
   │     MySQL 服务器      │
   └──────────────────────┘
```

> ★ 标记的即为本篇讲解的模块。

---

## 3. 后端抽象：DbConnection 接口

### 3.1 设计原则

hical 不直接暴露 `boost::mysql::any_connection` 给业务代码，而是定义一层抽象接口 `DbConnection`。原因有三：

1. **后端可替换**——今天用 MySQL，明天加 PostgreSQL，业务代码零改动
2. **可测试性**——单元测试可以传入 MockDbConnection，不依赖真实数据库
3. **装饰器友好**——QueryLog 中间件用装饰器包装连接，只需实现同一接口

### 3.2 接口定义

```cpp
// src/db/DbConnection.h

class DbConnection
{
public:
    virtual ~DbConnection() = default;

    // ============ 查询/执行 ============

    // 无参数化重载标记 deprecated，仅限静态 SQL（DDL/SET）
    [[deprecated("use query(sql, params) to prevent SQL injection")]]
    virtual Awaitable<DbResult> query(std::string_view sql) = 0;

    // 参数化查询（防 SQL 注入）
    virtual Awaitable<DbResult> query(
        std::string_view sql,
        std::span<const std::string> params) = 0;

    // execute 同理（省略 deprecated 重载）
    virtual Awaitable<DbResult> execute(
        std::string_view sql,
        std::span<const std::string> params) = 0;

    // ============ 事务控制 ============
    virtual Awaitable<void> beginTransaction() = 0;
    virtual Awaitable<void> commit() = 0;
    virtual Awaitable<void> rollback() = 0;
    virtual bool inTransaction() const = 0;

    // ============ 连接状态 ============
    virtual bool isAlive() const = 0;       // 本地判断，不发网络包
    virtual Awaitable<bool> ping() = 0;     // 发包验证
    virtual std::string_view backend() const = 0;  // "mysql", "pgsql"...
    virtual std::chrono::steady_clock::time_point lastActiveTime() const = 0;
    virtual std::chrono::steady_clock::time_point lastPingTime() const = 0;
    virtual void touch() = 0;               // 更新活跃时间
};
```

**关键设计决策：为什么用 `[[deprecated]]`？**

无参数化的 `query(sql)` 重载合法用途很窄（DDL、SET NAMES 等静态 SQL），但存在就一定有人拿来拼接用户输入。标记 `[[deprecated]]` 后：

- 业务代码调用时编译器会发出警告
- 框架内部合法调用可以用 `#pragma` 局部抑制
- API 仍然存在（不 break），但强烈引导开发者使用参数化版本

### 3.3 结果集：DbResult

```cpp
struct DbResult
{
    std::vector<std::string> columns;                // 列名
    std::vector<std::vector<std::string>> rows;      // 行数据（全部转为 string）
    uint64_t affectedRows = 0;                       // INSERT/UPDATE/DELETE
    uint64_t insertId = 0;                           // 自增 ID

    bool empty() const { return rows.empty(); }
    size_t size() const { return rows.size(); }
    const std::vector<std::string>& operator[](size_t index) const;
    size_t columnIndex(std::string_view name) const; // 按列名查找索引
};
```

**为什么所有值存为 string？**

在 Web 框架中，查询结果最终几乎都要序列化为 JSON 返回给客户端——到头来还是 string。用统一的 `vector<string>` 省去了泛型 variant 的复杂度，也让 `DbResult` 不依赖任何数据库后端的类型系统。

---

## 4. MySQL 实现：any_connection 封装

### 4.1 为什么选 Boost.MySQL

| 方案                | 异步模型           | 协程支持              | 依赖                   |
| ------------------- | ------------------ | --------------------- | ---------------------- |
| libmysqlclient      | 同步阻塞           | ❌                     | MySQL 官方 C 库        |
| mysql-connector-c++ | 同步 + 自行线程池  | ❌                     | MySQL 官方             |
| **Boost.MySQL**     | **原生 Asio 异步** | **co_await 一等公民** | **仅 Boost + OpenSSL** |

hical 本身已经重度依赖 Boost.Asio，选 Boost.MySQL 是零额外依赖的最优解。更关键的是，`boost::mysql::any_connection` 直接支持 `co_await`——和 hical 的协程化架构完美契合。

### 4.2 类结构

```
MysqlConnection : public DbConnection
├── m_conn       : boost::mysql::any_connection   ← 底层连接
├── m_stmtCache  : StmtCache                      ← LRU 缓存
├── m_alive      : bool                           ← 本地存活状态
├── m_inTransaction : bool                        ← 事务状态
├── m_lastActive : steady_clock::time_point       ← 最近活跃时间
└── m_lastPing   : steady_clock::time_point       ← 最近 ping 时间
```

### 4.3 工厂模式：解耦连接池与后端

连接池不应该知道 `MysqlConnection` 的存在——它只需要一个"给我造一个 `DbConnection`"的工厂函数：

```cpp
// 类型别名：工厂函数签名
using DbConnectionFactory = std::function<
    Awaitable<std::shared_ptr<DbConnection>>(
        boost::asio::io_context&,
        const DbConfig&)>;

// MysqlConnection 提供自己的工厂
DbConnectionFactory MysqlConnection::makeFactory()
{
    return [](boost::asio::io_context& ioCtx,
              const DbConfig& config) -> Awaitable<std::shared_ptr<DbConnection>>
    {
        co_return co_await MysqlConnection::create(ioCtx, config);
    };
}
```

未来增加 PostgreSQL 支持？只需要写一个 `PgsqlConnection::makeFactory()`，传入同一个连接池即可。

### 4.4 连接创建流程

```cpp
Awaitable<std::shared_ptr<MysqlConnection>> MysqlConnection::create(
    boost::asio::io_context& ioCtx,
    const DbConfig& config)
{
    auto conn = std::shared_ptr<MysqlConnection>(
        new MysqlConnection(ioCtx, config.stmtCacheSize));

    // 1. 构建连接参数
    boost::mysql::connect_params params;
    params.server_address.emplace_host_and_port(config.host, config.port);
    params.username = config.user;
    params.password = config.password;
    params.database = config.database;

    // 2. 异步连接
    co_await conn->m_conn.async_connect(params, boost::asio::use_awaitable);
    conn->m_alive = true;

    // 3. 设置元数据模式（获取列名等完整信息）
    conn->m_conn.set_meta_mode(boost::mysql::metadata_mode::full);

    // 4. 设置字符集（通过 SET NAMES）
    if (!config.charset.empty())
    {
        validateCharset(config.charset);  // 白名单校验，防注入
        boost::mysql::results r;
        co_await conn->m_conn.async_execute(
            "SET NAMES '" + config.charset + "'", r,
            boost::asio::use_awaitable);
    }

    co_return conn;
}
```

> **安全细节**：`validateCharset()` 对 charset 做白名单校验（仅字母、数字、下划线），因为 `SET NAMES '...'` 是拼接执行的——如果不校验，攻击者可以通过 config 注入任意 SQL。

### 4.5 参数化查询：PreparedStatement + 自动重试

参数化查询是整个数据库层最核心的路径：

```
query(sql, params) 执行流程：

1. getOrPrepare(sql)
   ├─ StmtCache.find(sql)
   │  └─ 命中 → 返回缓存的 statement（O(1)，零网络开销）
   └─ 未命中
      └─ async_prepare_statement(sql)   ← 一次网络往返
      └─ StmtCache.insert(sql, stmt)
         └─ 缓存满 → 淘汰 LRU → async_close_statement(evicted)

2. 构建参数
   └─ span<const string> → vector<field_view>

3. 执行
   └─ async_execute(stmt.bind(fields...), results)

4. 失败重试
   └─ 服务器重启导致 statement 失效？
      → erase 缓存 → 重新 prepare → 重试执行
      → 重试成功后新 statement 放回缓存

5. 结果转换
   └─ boost::mysql::results → DbResult（统一 string 化）
```

**为什么需要重试机制？**

MySQL 服务器重启后，所有 PreparedStatement 都会失效。客户端持有的 `statement` 对象发起执行会收到错误。对于生产环境的游戏服务器来说，DBA 做维护重启是常规操作——框架必须透明处理这种情况，而不是让业务代码崩溃。

```cpp
Awaitable<DbResult> MysqlConnection::query(
    std::string_view sql,
    std::span<const std::string> params)
{
    auto stmt = co_await getOrPrepare(sql);

    // 构建参数列表
    std::vector<boost::mysql::field_view> fields;
    fields.reserve(params.size());
    for (const auto& p : params)
    {
        fields.emplace_back(p);
    }

    boost::mysql::results boostResults;
    bool needRetry = false;
    try
    {
        co_await m_conn.async_execute(
            stmt.bind(fields.begin(), fields.end()),
            boostResults, boost::asio::use_awaitable);
    }
    catch (...)
    {
        // statement 可能已失效，标记重试
        m_stmtCache.erase(sql);
        needRetry = true;
    }

    if (needRetry)
    {
        // 在 catch 外重新 prepare（允许 co_await）
        auto freshStmt = co_await m_conn.async_prepare_statement(
            std::string(sql), boost::asio::use_awaitable);

        co_await m_conn.async_execute(
            freshStmt.bind(fields.begin(), fields.end()),
            boostResults, boost::asio::use_awaitable);

        // 重试成功，放回缓存
        auto evicted = m_stmtCache.insert(std::string(sql), std::move(freshStmt));
        if (evicted)
        {
            try { co_await m_conn.async_close_statement(*evicted, boost::asio::use_awaitable); }
            catch (...) {}
        }
    }

    touch();
    co_return convertResults(boostResults);
}
```

### 4.6 结果转换：field_view → string

`boost::mysql::results` 的每个字段是 `field_view`——一个变体类型，可能是 `int64`、`string_view`、`date`、`null` 等。`convertResults()` 将它们统一转为 `std::string`：

```cpp
DbResult MysqlConnection::convertResults(const boost::mysql::results& boostResults)
{
    DbResult result;
    result.affectedRows = boostResults.affected_rows();
    result.insertId = boostResults.last_insert_id();

    // 提取列名
    for (const auto& col : boostResults.meta())
    {
        result.columns.emplace_back(col.column_name());
    }

    // 逐行逐列转换
    for (auto row : boostResults.rows())
    {
        std::vector<std::string> dbRow;
        for (size_t i = 0; i < row.size(); ++i)
        {
            const auto& field = row.at(i);

            if (field.is_null())        dbRow.emplace_back();
            else if (field.is_int64())  dbRow.push_back(std::to_string(field.as_int64()));
            else if (field.is_uint64()) dbRow.push_back(std::to_string(field.as_uint64()));
            else if (field.is_double()) dbRow.push_back(std::to_string(field.as_double()));
            else if (field.is_string()) dbRow.emplace_back(field.as_string());
            else if (field.is_date())
            {
                auto d = field.as_date();
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
                              d.year(), d.month(), d.day());
                dbRow.emplace_back(buf);
            }
            else if (field.is_datetime())
            {
                auto dt = field.as_datetime();
                char buf[32];
                std::snprintf(buf, sizeof(buf),
                              "%04u-%02u-%02u %02u:%02u:%02u",
                              dt.year(), dt.month(), dt.day(),
                              dt.hour(), dt.minute(), dt.second());
                dbRow.emplace_back(buf);
            }
            // ... time, blob 等类似处理
        }
        result.rows.push_back(std::move(dbRow));
    }
    return result;
}
```

---

## 5. LRU PreparedStatement 缓存

### 5.1 为什么需要 Statement 缓存

每次 `async_prepare_statement()` 都是一次到 MySQL 服务器的网络往返（通常 0.1-0.5ms）。Web 框架中同一 SQL 模板（如 `SELECT * FROM users WHERE id = ?`）会被成千上万个请求重复使用——每次都 prepare 是巨大的浪费。

```
无缓存（每次 prepare + execute）：

请求1: prepare("SELECT...WHERE id=?")  ← 0.3ms 网络往返
       execute(stmt, 42)                ← 0.2ms
请求2: prepare("SELECT...WHERE id=?")  ← 0.3ms 重复！
       execute(stmt, 43)                ← 0.2ms
请求3: prepare("SELECT...WHERE id=?")  ← 0.3ms 重复！
       execute(stmt, 44)                ← 0.2ms

有缓存：

请求1: prepare("SELECT...WHERE id=?")  ← 0.3ms（首次）
       cache.insert(sql, stmt)
       execute(stmt, 42)                ← 0.2ms
请求2: cache.find(sql)                 ← ~0ns（内存查找）
       execute(stmt, 43)                ← 0.2ms
请求3: cache.find(sql)                 ← ~0ns
       execute(stmt, 44)                ← 0.2ms
```

缓存使后续请求的查询延迟从 0.5ms 降至 0.2ms——**省掉 60% 的延迟**。

### 5.2 数据结构：哈希表 + 双向链表

经典 LRU 缓存用两个数据结构协同实现 O(1) 的 find / insert / evict：

```
StmtCache 内部结构：

┌─────────────────────────────────────────────────────────────┐
│  m_lruList : std::list<pair<string, statement>>              │
│                                                              │
│  ┌─────────────┐     ┌──────────┐     ┌──────────┐          │
│  │ MRU         │ ←─→ │          │ ←─→ │ LRU      │          │
│  │ "SELECT.."  │     │ "INSERT" │     │ "UPDATE" │  ← 淘汰  │
│  │ stmt#15     │     │ stmt#8   │     │ stmt#3   │          │
│  └─────────────┘     └──────────┘     └──────────┘          │
│                                                              │
│  m_map : unordered_map<string, list::iterator>               │
│  ┌──────────────────┬─────────────┐                          │
│  │ "SELECT..."      │ → iter_MRU  │   O(1) 查找              │
│  │ "INSERT..."      │ → iter_mid  │                          │
│  │ "UPDATE..."      │ → iter_LRU  │                          │
│  └──────────────────┴─────────────┘                          │
└─────────────────────────────────────────────────────────────┘
```

### 5.3 教学代码：从零实现 LRU 缓存

```cpp
// 简化版教学代码：LRU 缓存核心逻辑
template <typename Key, typename Value>
class LruCache
{
public:
    explicit LruCache(size_t maxSize) : m_maxSize(maxSize) {}

    Value* find(const Key& key)
    {
        auto it = m_map.find(key);
        if (it == m_map.end()) return nullptr;

        // 命中 → 提升到 MRU（链表头部）
        m_list.splice(m_list.begin(), m_list, it->second);
        return &(it->second->second);
    }

    std::optional<Value> insert(const Key& key, Value value)
    {
        std::optional<Value> evicted;

        // 已存在 → 更新并提升
        auto it = m_map.find(key);
        if (it != m_map.end())
        {
            it->second->second = std::move(value);
            m_list.splice(m_list.begin(), m_list, it->second);
            return evicted;  // nullopt
        }

        // 满了 → 淘汰 LRU（链表尾部）
        if (m_list.size() >= m_maxSize)
        {
            evicted = std::move(m_list.back().second);
            m_map.erase(m_list.back().first);
            m_list.pop_back();
        }

        // 插入新条目到 MRU 位置
        m_list.emplace_front(key, std::move(value));
        m_map[key] = m_list.begin();
        return evicted;
    }

private:
    size_t m_maxSize;
    std::list<std::pair<Key, Value>> m_list;        // front=MRU, back=LRU
    std::unordered_map<Key, typename std::list<
        std::pair<Key, Value>>::iterator> m_map;     // O(1) 查找
};
```

### 5.4 hical 实现的额外细节

hical 的 `StmtCache` 在教学版基础上增加了两点：

**1. 透明哈希（Transparent Hashing）**——用 `string_view` 查找 `string` key，避免 `find()` 时分配堆内存：

```cpp
struct StringHash
{
    using is_transparent = void;  // ← 关键：启用异质查找
    size_t operator()(std::string_view sv) const
    { return std::hash<std::string_view>{}(sv); }
    size_t operator()(const std::string& s) const
    { return std::hash<std::string_view>{}(s); }
};
```

**2. 缓存禁用模式**——`maxSize = 0` 时 `find()` 直接返回 nullptr，`insert()` 直接返回传入的 statement（由调用方关闭）。这允许运行时完全关闭缓存，用于调试或特殊场景。

---

## 6. 协程连接池：用 steady_timer 做信号量

### 6.1 连接池解决什么问题

```
无连接池：
  请求1 → connect() 1.5ms → query() 0.2ms → close()
  请求2 → connect() 1.5ms → query() 0.2ms → close()
  请求3 → connect() 1.5ms → query() 0.2ms → close()
  每个请求: 1.7ms

有连接池：
  启动时 → connect() × 2（预创建）
  请求1 → acquire() ~0ms → query() 0.2ms → release()
  请求2 → acquire() ~0ms → query() 0.2ms → release()
  请求3 → acquire() ~0ms → query() 0.2ms → release()
  每个请求: 0.2ms（省掉 88% 延迟）
```

### 6.2 为什么不能用 condition_variable

传统多线程连接池用 `condition_variable` 阻塞等待空闲连接：

```cpp
// ❌ 传统方式——阻塞 io_context 线程！
std::unique_lock lock(m_mutex);
m_cv.wait(lock, [&]{ return !m_idle.empty(); });
auto conn = m_idle.back(); m_idle.pop_back();
```

问题：hical 用 `1 Thread : 1 io_context` 模型（第一篇讲过）。每个线程只跑一个事件循环。如果 `cv.wait()` 阻塞了线程，这个 io_context 上的**所有协程**都会卡住——不只是等待连接的那个。

### 6.3 解决方案：steady_timer 作为协程信号量

思路：用一个永远不会自然到期的 `steady_timer`（超时设为很远的将来），让等待者 `co_await timer.async_wait()`。当连接归还时，`timer.cancel()` 唤醒等待者：

```
acquire() 池满时：

  创建 timer（超时 = acquireTimeout）
  │
  co_await timer.async_wait()  ← 协程挂起，线程继续处理其他协程
  │
  .... 时间流逝 ....
  │
  有人调用 release()
  │
  release() 发现有等待者 → *(waiter.result) = conn → timer.cancel()
  │
  async_wait 返回（error_code = operation_aborted）
  │
  检查 result 是否有连接 → 有！直接返回
```

核心代码：

```cpp
// acquire() 中池满的处理
auto timer = std::make_shared<boost::asio::steady_timer>(
    m_ioCtx, m_config.acquireTimeout);
auto result = std::make_shared<std::shared_ptr<DbConnection>>();
m_waiters.push_back({timer, result});
lock.unlock();  // 释放锁，让 release() 可以进入

boost::system::error_code ec;
co_await timer->async_wait(
    boost::asio::redirect_error(boost::asio::use_awaitable, ec));

if (*result)
{
    // release() 已经把连接放到 result 里了
    co_return std::move(*result);
}

// result 为空 → 超时
throw std::runtime_error("DbConnectionPool: acquire timeout");
```

```cpp
// release() 中有等待者的处理
if (!m_waiters.empty())
{
    auto waiter = std::move(m_waiters.front());
    m_waiters.pop_front();
    *(waiter.result) = std::move(conn);  // 将连接放入结果槽
    waiter.timer->cancel();               // 唤醒协程
    return;
}
```

### 6.4 LIFO 还是 FIFO？

连接池用 LIFO（后进先出，栈）而非 FIFO（先进先出，队列）复用空闲连接：

```
LIFO 优势：

  时间轴: ←─────────────────────────────────────→
  conn1: [使用] [空闲.................][被回收]
  conn2: [使用]  [空闲..] [使用] [空闲] [使用]   ← 频繁复用
  conn3: [使用]   [空闲...............][被回收]

  最近归还的 conn2 最先被取用 → TCP 状态热、MySQL 线程缓存热
  不活跃的 conn1/conn3 沉底 → 被 idleCheckLoop 自然回收
```

- 减少总活跃连接数（集中在少数连接上）→ MySQL 端线程资源消耗更少
- 更热的 TCP 连接 → 避免 keepalive 超时被服务端主动断开

### 6.5 后台循环

连接池启动两个后台协程：

```
┌─────────────────────────────────────┐
│  idleCheckLoop                       │
│  间隔: idleCheckInterval (60s)       │
│  职责: 回收超时空闲连接              │
│  规则: lastActive > idleTimeout(300s) │
│  约束: 保留 ≥ minConnections 个      │
└─────────────────────────────────────┘

┌─────────────────────────────────────┐
│  healthCheckLoop                     │
│  间隔: healthCheckInterval (30s)     │
│  职责: ping 所有空闲连接，剔除死连接  │
│  流程:                               │
│   1. 将空闲连接移出（防 acquire 冲突）│
│   2. 逐个 ping                       │
│   3. 存活的放回空闲池                │
│   4. 补充新连接到 minConnections     │
└─────────────────────────────────────┘
```

**healthCheckLoop 的精妙之处**：在 ping 期间将连接从空闲池**移出**，防止 `acquire()` 在另一个协程中取到正在被 ping 的连接。ping 完成后再放回——这是一个微妙但关键的并发安全设计。

### 6.6 配置参数速查表

```cpp
struct DbConfig
{
    // 连接参数
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user, password, database;
    std::string charset = "utf8mb4";

    // 连接池参数
    size_t minConnections = 2;             // 预创建 + 保底数量
    size_t maxConnections = 16;            // 含活跃 + 空闲
    std::chrono::seconds idleTimeout{300}; // 空闲连接回收阈值
    std::chrono::seconds acquireTimeout{5};// 获取连接超时
    std::chrono::seconds queryTimeout{30}; // 查询超时

    // 后台循环
    std::chrono::seconds idleCheckInterval{60};     // 0=禁用
    std::chrono::seconds healthCheckInterval{30};   // 0=禁用
    std::chrono::seconds pingGracePeriod{15};       // acquire 跳过 ping 的宽限期

    // 缓存
    size_t stmtCacheSize = 64;            // 0=禁用
};
```

**调优建议**：

| 参数                | 游戏服务器推荐值 | Web API 推荐值 | 说明                         |
| ------------------- | ---------------- | -------------- | ---------------------------- |
| minConnections      | 4-8              | 2-4            | 游戏玩家在线期间 DB 压力稳定 |
| maxConnections      | 32-64            | 16-32          | 不超过 MySQL max_connections |
| idleTimeout         | 600s             | 300s           | 游戏连接更长寿               |
| stmtCacheSize       | 128              | 64             | 游戏 SQL 种类更多            |
| healthCheckInterval | 30s              | 60s            | 游戏对掉线更敏感             |

---

## 7. DB 中间件：请求级连接生命周期

### 7.1 问题：谁来管连接？

最幼稚的做法是在每个路由处理器里手动获取和归还连接：

```cpp
// ❌ 每个路由都要重复这段逻辑
server.get("/api/user/{id}", [&pool](HttpRequest& req) -> Awaitable<HttpResponse>
{
    auto conn = co_await pool->acquire();
    try
    {
        co_await conn->beginTransaction();
        auto result = co_await conn->query("...", params);
        co_await conn->commit();
        pool->release(conn);
        co_return HttpResponse::json({...});
    }
    catch (...)
    {
        co_await conn->rollback();
        pool->release(conn);
        throw;
    }
});
```

问题很明显：获取→事务→异常回滚→归还，每个路由都写一遍。

### 7.2 解决方案：洋葱模型中间件

hical 的 `makeDbMiddleware()` 把连接生命周期管理从业务代码中抽离：

```cpp
inline MiddlewareHandler makeDbMiddleware(
    std::shared_ptr<DbConnectionPool> pool,
    DbMiddlewareOptions opts = {})
{
    return [pool, opts](HttpRequest& req, MiddlewareNext next)
        -> Awaitable<HttpResponse>
    {
        // ① 前置：获取连接，注入请求属性
        auto conn = co_await pool->acquire();
        req.setAttribute(DbConnectionPool::hConnKey, conn);

        if (opts.injectPool)
            req.setAttribute(DbConnectionPool::hPoolKey, pool);

        // ② 可选：自动开启事务
        if (opts.autoTransaction)
            co_await conn->beginTransaction();

        std::exception_ptr eptr;
        HttpResponse res;
        try
        {
            // ③ 执行后续中间件和路由
            res = co_await next(req);

            // ④ 正常完成：提交事务
            if (opts.autoTransaction && conn->inTransaction())
                co_await conn->commit();
        }
        catch (...)
        {
            eptr = std::current_exception();
        }

        // ⑤ 异常时：回滚事务
        if (eptr && conn->inTransaction())
        {
            try { co_await conn->rollback(); }
            catch (...) {}
        }

        // ⑥ 归还连接（无论成功还是异常）
        pool->release(std::move(conn));

        if (eptr) std::rethrow_exception(eptr);
        co_return res;
    };
}
```

**洋葱模型可视化**：

```
请求进入
│
├─ ① acquire() 获取连接
├─ ② BEGIN TRANSACTION（若 autoTransaction）
│   │
│   ├─ [其他中间件]
│   │   │
│   │   └─ [路由处理器]
│   │      auto conn = getDbConnection(req);  ← 直接取
│   │      conn->query("...", params);         ← 直接用
│   │
│   ├─ ④ COMMIT（正常）
│   └─ ⑤ ROLLBACK（异常）
│
└─ ⑥ release() 归还连接
```

业务代码变得极其简洁：

```cpp
server.get("/api/user/{id}", [](HttpRequest& req) -> Awaitable<HttpResponse>
{
    auto conn = getDbConnection(req);  // 一行获取连接
    std::array<std::string, 1> params = {req.param("id")};
    auto result = co_await conn->query(
        "SELECT name, email FROM users WHERE id = ?", params);

    if (result.empty())
        co_return HttpResponse::notFound();

    co_return HttpResponse::json({
        {"name", result[0][0]},
        {"email", result[0][1]}
    });
});
```

### 7.3 release() 中的事务安全

如果业务代码在事务中间抛了异常，连接被归还时可能还残留着一个未提交的事务。`DbConnectionPool::release()` 会检测并自动回滚：

```cpp
void DbConnectionPool::release(std::shared_ptr<DbConnection> conn)
{
    // 残留事务 → 异步回滚后再归池
    if (conn->inTransaction())
    {
        auto self = shared_from_this();
        boost::asio::co_spawn(m_ioCtx,
            [self, conn]() mutable -> Awaitable<void>
            {
                try
                {
                    co_await conn->rollback();
                }
                catch (...)
                {
                    // 回滚失败 → 连接不可复用，直接丢弃
                    return;
                }
                // 回滚成功 → 放回空闲池或转交等待者
                self->returnToPool(conn);
            },
            boost::asio::detached);
        return;
    }

    returnToPool(conn);
}
```

这是**双重保护**：中间件层做了第一次回滚，连接池做最后兜底。即使中间件的回滚因为网络问题失败了，连接池还会再尝试一次。

---

## 8. 查询日志：装饰器模式的妙用

### 8.1 需求

生产环境需要知道：

- 每个请求执行了哪些 SQL、各花了多长时间
- 有没有慢查询（超过阈值的查询实时告警）
- 每个请求的总 SQL 数量（N+1 查询问题检测）

### 8.2 装饰器模式

关键洞察：业务代码用 `getDbConnection(req)` 拿到的是 `shared_ptr<DbConnection>`。如果我们在中间件里**悄悄替换**为一个装饰器，业务代码完全无感：

```
                    原始                                    替换后
            ┌──────────────────┐                  ┌──────────────────────┐
req 属性 →  │ MysqlConnection  │     req 属性 →   │ LoggingDbConnection  │
            └──────────────────┘                  │ ┌──────────────────┐ │
                                                  │ │ MysqlConnection  │ │ ← 真实连接
                                                  │ └──────────────────┘ │
                                                  │ + 计时 + 日志收集    │
                                                  └──────────────────────┘
```

`LoggingDbConnection` 继承 `DbConnection`，包装真实连接，拦截 `query()` / `execute()`：

```cpp
class LoggingDbConnection : public DbConnection
{
public:
    LoggingDbConnection(
        std::shared_ptr<DbConnection> real,
        std::shared_ptr<std::vector<QueryLogEntry>> log,
        std::chrono::microseconds slowThreshold,
        SlowQueryCallback slowCb)
        : m_real(std::move(real))
        , m_log(std::move(log))
        , m_slowThreshold(slowThreshold)
        , m_slowCb(std::move(slowCb)) {}

    Awaitable<DbResult> query(
        std::string_view sql,
        std::span<const std::string> params) override
    {
        auto start = std::chrono::steady_clock::now();

        auto result = co_await m_real->query(sql, params);  // 转发

        auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);

        // 记录日志条目
        QueryLogEntry entry{
            .sql = std::string(sql),
            .duration = elapsed,
            .rowCount = result.rows.size(),
            .affectedRows = result.affectedRows,
            .isParameterized = true
        };

        // 慢查询实时回调
        if (m_slowThreshold.count() > 0 && elapsed >= m_slowThreshold && m_slowCb)
            m_slowCb(entry);

        m_log->push_back(std::move(entry));
        co_return result;
    }

    // beginTransaction / commit / rollback / ping 等直接转发
    Awaitable<void> beginTransaction() override { return m_real->beginTransaction(); }
    // ...
};
```

### 8.3 中间件完整流程

```
请求进入
│
├─ [DbMiddleware] acquire → 注入 MysqlConnection
│
├─ [QueryLogMiddleware]
│   ├─ 前置：取出 MysqlConnection，创建 LoggingDbConnection，替换请求属性
│   │
│   ├─ [路由处理器]
│   │   getDbConnection(req)  → LoggingDbConnection（无感）
│   │   conn->query(...)      → 自动计时 + 记录
│   │
│   └─ 后置：恢复原始 MysqlConnection，触发 onRequestComplete 回调
│
├─ [DbMiddleware] commit/rollback + release
│
└─ 响应返回
```

**为什么要在后置阶段恢复原始连接？** 因为 `DbMiddleware` 的 `release()` 需要操作的是真实的 `MysqlConnection`，而不是装饰器。

### 8.4 使用示例

```cpp
// 注册中间件（顺序重要！DbMiddleware 必须在 QueryLog 之前）
server.use(makeDbMiddleware(pool, {.autoTransaction = true}));
server.use(makeQueryLogMiddleware({
    .slowQueryThreshold = std::chrono::milliseconds(100),
    .onSlowQuery = [](const QueryLogEntry& entry)
    {
        LOG_WARN("慢查询 ({}μs): {}", entry.duration.count(), entry.sql);
    },
    .onRequestComplete = [](const HttpRequest& req,
                            const std::vector<QueryLogEntry>& entries)
    {
        LOG_INFO("{} {} — {} 条查询", httpMethodToString(req.method()),
                 req.path(), entries.size());
        for (const auto& e : entries)
        {
            LOG_DEBUG("  {}μs | {} rows | {}",
                      e.duration.count(), e.rowCount, e.sql);
        }
    }
}));
```

---

## 9. 综合实战：用户管理 API + 数据库

把所有模块串起来，构建一个完整的用户管理 API：

```cpp
#include "core/HttpServer.h"
#include "db/DbMiddleware.h"
#include "db/DbQueryLog.h"
#include "db/MysqlConnection.h"
#include <boost/json.hpp>

using namespace hical;
using namespace hical::db;
namespace json = boost::json;

int main()
{
    HttpServer server(8080);
    boost::asio::io_context& ioCtx = server.ioContext();

    // ============ 数据库初始化 ============

    DbConfig dbConfig;
    dbConfig.host = "127.0.0.1";
    dbConfig.user = "root";
    dbConfig.password = "secret";
    dbConfig.database = "myapp";
    dbConfig.minConnections = 4;
    dbConfig.maxConnections = 32;

    auto pool = std::make_shared<DbConnectionPool>(
        ioCtx, dbConfig, MysqlConnection::makeFactory());

    // 协程中初始化连接池
    boost::asio::co_spawn(ioCtx,
        [&]() -> Awaitable<void>
        {
            co_await pool->init();
        }, boost::asio::detached);

    // ============ 中间件注册 ============

    // 日志中间件
    server.use([](HttpRequest& req, MiddlewareNext next)
        -> Awaitable<HttpResponse>
    {
        auto start = std::chrono::steady_clock::now();
        auto res = co_await next(req);
        auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << httpMethodToString(req.method()) << " "
                  << req.path() << " → "
                  << static_cast<int>(res.statusCode())
                  << " (" << elapsed << "μs)\n";
        co_return res;
    });

    // DB 中间件（自动事务）
    server.use(makeDbMiddleware(pool, {.autoTransaction = true}));

    // 查询日志中间件
    server.use(makeQueryLogMiddleware({
        .slowQueryThreshold = std::chrono::milliseconds(100),
        .onSlowQuery = [](const QueryLogEntry& entry)
        {
            std::cerr << "[SLOW] " << entry.duration.count()
                      << "μs: " << entry.sql << "\n";
        }
    }));

    // ============ 路由 ============

    // GET /api/users — 用户列表
    server.router().get("/api/users",
        [](HttpRequest& req) -> Awaitable<HttpResponse>
    {
        auto conn = getDbConnection(req);
        auto result = co_await conn->query(
            "SELECT id, name, email FROM users ORDER BY id", {});

        json::array users;
        for (size_t i = 0; i < result.size(); ++i)
        {
            users.push_back({
                {"id", result[i][0]},
                {"name", result[i][1]},
                {"email", result[i][2]}
            });
        }
        co_return HttpResponse::json(json::value(std::move(users)));
    });

    // GET /api/users/{id} — 查询单个用户
    server.router().get("/api/users/{id}",
        [](HttpRequest& req) -> Awaitable<HttpResponse>
    {
        auto conn = getDbConnection(req);
        std::array<std::string, 1> params = {req.param("id")};
        auto result = co_await conn->query(
            "SELECT id, name, email, created_at FROM users WHERE id = ?",
            params);

        if (result.empty())
        {
            co_return HttpResponse::notFound("User not found");
        }

        co_return HttpResponse::json({
            {"id", result[0][0]},
            {"name", result[0][1]},
            {"email", result[0][2]},
            {"created_at", result[0][3]}
        });
    });

    // POST /api/users — 创建用户
    server.router().post("/api/users",
        [](HttpRequest& req) -> Awaitable<HttpResponse>
    {
        auto body = req.jsonBody();
        auto name = json::value_to<std::string>(body.at("name"));
        auto email = json::value_to<std::string>(body.at("email"));

        auto conn = getDbConnection(req);
        std::array<std::string, 2> params = {name, email};
        auto result = co_await conn->execute(
            "INSERT INTO users (name, email) VALUES (?, ?)", params);

        co_return HttpResponse::json({
            {"id", std::to_string(result.insertId)},
            {"name", name},
            {"email", email}
        }, 201);
    });

    // PUT /api/users/{id} — 更新用户
    server.router().put("/api/users/{id}",
        [](HttpRequest& req) -> Awaitable<HttpResponse>
    {
        auto body = req.jsonBody();
        auto name = json::value_to<std::string>(body.at("name"));
        auto email = json::value_to<std::string>(body.at("email"));

        auto conn = getDbConnection(req);
        std::array<std::string, 3> params = {name, email, req.param("id")};
        auto result = co_await conn->execute(
            "UPDATE users SET name = ?, email = ? WHERE id = ?", params);

        if (result.affectedRows == 0)
        {
            co_return HttpResponse::notFound("User not found");
        }
        co_return HttpResponse::ok("Updated");
    });

    // DELETE /api/users/{id} — 删除用户
    server.router().del("/api/users/{id}",
        [](HttpRequest& req) -> Awaitable<HttpResponse>
    {
        auto conn = getDbConnection(req);
        std::array<std::string, 1> params = {req.param("id")};
        auto result = co_await conn->execute(
            "DELETE FROM users WHERE id = ?", params);

        if (result.affectedRows == 0)
        {
            co_return HttpResponse::notFound("User not found");
        }
        co_return HttpResponse::ok("Deleted");
    });

    server.start();
    return 0;
}
```

**关键观察**：

1. 业务代码**不出现** `acquire`、`release`、`beginTransaction`、`commit`、`rollback`——全部由中间件自动处理
2. 所有查询都是**参数化**的——编译器会对无参数化重载发出 deprecated 警告
3. `getDbConnection(req)` 一行获取连接——如果忘了注册 DbMiddleware 会立即 throw 明确错误
4. 慢查询自动告警——不需要在每个路由里手动埋点

---

## 10. 总结

本篇实现了 hical 数据库层的全部组件：

| 组件                 | 设计决策                           | 关键技术                                  |
| -------------------- | ---------------------------------- | ----------------------------------------- |
| **DbConnection**     | 后端无关的抽象接口                 | `[[deprecated]]` 引导参数化，工厂模式解耦 |
| **MysqlConnection**  | Boost.MySQL any_connection 封装    | 协程化异步，失败自动重试                  |
| **StmtCache**        | Per-connection LRU 缓存            | 哈希表 + 双向链表 O(1)，透明哈希          |
| **DbConnectionPool** | steady_timer 协程信号量，LIFO 复用 | 不阻塞 io_context，后台健康检查           |
| **DbMiddleware**     | 洋葱模型，请求级连接生命周期       | 自动事务，异常回滚，属性注入              |
| **DbQueryLog**       | 装饰器模式，业务无感知             | 慢查询实时告警，请求完成回调              |

### 核心要点

1. **协程连接池不能用 condition_variable**——必须用 `steady_timer` 作为协程信号量，挂起协程而不阻塞 io_context 线程
2. **LIFO 比 FIFO 更好**——最近归还的连接 TCP 状态热、MySQL 线程缓存热，不活跃连接自然沉底被回收
3. **PreparedStatement 缓存省掉 60% 延迟**——首次 prepare + execute 两次往返，后续只需 execute 一次
4. **装饰器模式是中间件的最佳拍档**——在不修改业务代码的前提下，透明地注入横切关注点
5. **安全性是框架责任**——`[[deprecated]]` 引导参数化查询，`validateCharset()` 白名单校验，release() 兜底回滚

### 知识图谱

```
从零构建现代 C++ Web 服务器
│
├── 第一篇：设计理念与架构总览
│   ├── 两层架构：core（抽象）+ asio（实现）
│   ├── C++20 Concepts 后端抽象
│   └── 线程模型：1 Thread : 1 io_context
│
├── 第二篇：协程异步与 PMR 内存池
│   ├── Awaitable<T> 协程基石
│   ├── PMR 三层内存架构
│   └── PmrBuffer 零拷贝缓冲区
│
├── 第三篇：路由、中间件与 SSL
│   ├── 双策略路由（哈希 O(1) + 参数线性）
│   ├── 洋葱模型中间件管道
│   └── 模板化 SSL（编译期零开销）
│
├── 第四篇：实战案例与性能调优
│   ├── RESTful API / WebSocket 完整案例
│   ├── C++26 反射宏系统
│   └── 性能调优与安全加固
│
├── 第五篇：Cookie、Session、静态文件与文件上传
│   ├── Cookie 惰性解析与 RFC 6265 编码
│   ├── Session 中间件与懒 GC
│   └── 静态文件 ETag 缓存与 Multipart DoS 防护
│
└── 第六篇：数据库中间件与协程连接池（本文）
    ├── DbConnection 后端抽象 + MysqlConnection 实现
    ├── StmtCache LRU PreparedStatement 缓存
    ├── DbConnectionPool（steady_timer 信号量 + LIFO）
    ├── DbMiddleware 洋葱模型（自动事务 + 异常回滚）
    └── DbQueryLog 装饰器（慢查询告警）
```

### 核心设计决策补充表

| #   | 决策             | 选择                          | 核心理由                         |
| --- | ---------------- | ----------------------------- | -------------------------------- |
| 18  | DB 客户端        | Boost.MySQL any_connection    | 原生 Asio 协程，零额外依赖       |
| 19  | 后端抽象         | DbConnection 纯虚接口 + 工厂  | 可替换、可 Mock、装饰器友好      |
| 20  | SQL 安全         | `[[deprecated]]` + 参数化强制 | 编译器引导，不靠人肉 review      |
| 21  | Statement 缓存   | Per-connection LRU            | 无线程竞争，热缓存省 60% 延迟    |
| 22  | 连接池等待机制   | steady_timer 协程信号量       | 不阻塞 io_context，协程友好      |
| 23  | 空闲连接复用策略 | LIFO                          | TCP/MySQL 缓存热，减少总活跃连接 |
| 24  | 请求级连接管理   | 洋葱模型中间件 + 属性注入     | 业务代码零样板，异常自动回滚     |
| 25  | 查询日志         | 装饰器模式                    | 业务无感知，透明注入             |

---

> **hical** — 基于 C++26 的现代高性能 Web 框架 | [GitHub](https://github.com/Hical61/Hical.git)

---

> **上一篇**：[从零构建现代C++ Web服务器（五）：Cookie、Session、静态文件与文件上传]({{< relref "从零构建现代C++ Web服务器（五）" >}})
