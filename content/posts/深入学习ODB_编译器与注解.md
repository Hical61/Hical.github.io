+++
title = '深入学习 ODB（一）：编译期 ORM 的革命性思路'
date = '2025-05-25'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "编译期代码生成"]
categories = ["ODB学习"]
description = "从游戏服务器手写 SQL 的痛点出发，理解 ODB 编译器的代码生成原理、pragma 注解映射机制，搭建开发环境完成第一个 Player 对象的持久化。"
+++

> **系列导航**：**编译器与注解**（本文） | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})

---

## 引言：游戏服务器为什么需要 ORM？

假设你正在开发一款 MMO 游戏服务器，玩家下线时需要将角色数据存入 MySQL。最朴素的做法是：

```cpp
// 传统方式：手动拼接 SQL
void savePlayer(MYSQL* conn, const Player& p)
{
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "REPLACE INTO player (id, name, level, exp, gold, created_at) "
        "VALUES (%lu, '%s', %d, %lu, %lu, '%s')",
        p.id, p.name.c_str(), p.level, p.exp, p.gold,
        p.createdAt.c_str());

    mysql_query(conn, sql);
    // 没有类型检查、没有防注入、字段增减要同步改 SQL...
}
```

这段代码有几个致命问题：

| 问题 | 后果 |
|------|------|
| **类型不安全** | `snprintf` 格式符写错编译器不报错，运行时崩溃或数据错乱 |
| **SQL 注入风险** | 玩家昵称含单引号直接 crash |
| **字段同步噩梦** | Player 加一个字段，所有 INSERT/UPDATE/SELECT 都要改 |
| **无事务保证** | 扣金币和发道具分开执行，中间崩了数据就不一致 |
| **结果集手动解析** | `mysql_fetch_row` 返回字符串数组，类型转换全靠人肉 |

随着游戏系统膨胀——背包、装备、公会、邮件、交易——手写 SQL 的维护成本呈指数级增长。

**Java/C# 游戏服务器** 早就用 Hibernate、Entity Framework 等 ORM 解决了这些问题。但 C++ 的困境在于：**没有运行时反射**。你无法像 Java 那样在运行时扫描注解、动态生成 SQL。

ODB 的回答是：**既然运行时做不到，那就在编译期做。**

---

## 1. ODB 是什么？

ODB（Object-Relational Mapping for C++）是由 CodeSynthesis 开发的开源 C++ ORM 框架。它的核心思路与众不同：

```
┌─────────────────────────────────────────────────────────────┐
│                    ODB 编译流程                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  player.hxx          odb 编译器           生成的文件          │
│  ┌──────────┐       ┌──────────┐       ┌───────────────┐   │
│  │ #pragma  │──────▶│   odb    │──────▶│ player-odb.hxx│   │
│  │ db object│       │ compiler │       │ player-odb.ixx│   │
│  │ class    │       └──────────┘       │ player-odb.cxx│   │
│  │ Player   │                          │ player.sql    │   │
│  └──────────┘                          └───────────────┘   │
│                                                             │
│  你写的代码            自动处理            ODB 生成的代码      │
│  （只关注业务）        （编译期完成）      （SQL映射+序列化）   │
└─────────────────────────────────────────────────────────────┘
```

**核心特性**：

1. **编译期代码生成** — 不依赖运行时反射，零额外开销
2. **类型安全** — 编译器帮你检查类型匹配，字段改名编译就报错
3. **非侵入性** — 用 `#pragma` 注解，不改变类本身的结构
4. **多数据库** — 同一份代码可适配 MySQL、PostgreSQL、SQLite、Oracle、SQL Server

### 1.1 与其他方案的对比

| 方案 | 机制 | 运行时开销 | 类型安全 | 侵入性 |
|------|------|-----------|---------|--------|
| **手写 SQL** | 无抽象 | 最低 | ❌ | ❌ |
| **SOCI** | 运行时字符串绑定 | 低 | 部分 | 低 |
| **sqlpp11** | 模板元编程 | 低 | ✅ | 中（需定义表结构 DSL） |
| **ODB** | 编译期代码生成 | 零 | ✅ | 低（pragma 注解） |
| **Qt SQL + MOC** | MOC 代码生成 | 低 | 部分 | 高（继承 QObject） |

ODB 的独特优势：**你写的类就是普通 C++ 类**，不需要继承特定基类、不需要宏包裹，只需要加几行 `#pragma` 注释。

---

## 2. ODB 编译器：代码生成的核心引擎

ODB 编译器是一个独立的命令行工具，它解析带有 `#pragma db` 注解的 C++ 头文件，生成数据库操作代码。

### 2.1 生成文件说明

对于一个 `player.hxx` 输入文件，ODB 编译器生成：

| 生成文件 | 内容 | 用途 |
|---------|------|------|
| `player-odb.hxx` | 声明（traits、query 列名等） | 被其他翻译单元 #include |
| `player-odb.ixx` | 内联实现 | 性能关键路径的内联函数 |
| `player-odb.cxx` | 主实现（persist/load/update/erase） | 编译为 .o 链接 |
| `player.sql` | DDL 建表脚本 | 可选，用于手动建表 |

### 2.2 编译命令

```bash
# 基本用法：指定数据库后端为 mysql
odb --database mysql --generate-query --generate-schema \
    --std c++17 player.hxx

# 参数说明：
#   --database mysql     目标数据库（mysql/sqlite/pgsql/oracle/mssql）
#   --generate-query     生成查询支持代码（ODB Query Language）
#   --generate-schema    生成 SQL DDL 建表脚本
#   --std c++17          C++ 标准版本
#   player.hxx           输入头文件
```

### 2.3 整体编译流程

```bash
# 完整的构建步骤（手动演示，实际项目用 CMake 自动化）

# 1. ODB 编译器生成代码
odb --database mysql --generate-query --generate-schema \
    --std c++17 player.hxx

# 2. 编译你的源文件
g++ -std=c++17 -c player-odb.cxx -I/usr/include/odb
g++ -std=c++17 -c main.cxx -I/usr/include/odb

# 3. 链接（需要 libodb 和 libodb-mysql）
g++ -o game_server main.o player-odb.o \
    -lodb-mysql -lodb -lmysqlclient
```

---

## 3. pragma 注解系统详解

ODB 使用 `#pragma db` 前缀的编译器指令来标注映射信息。这些 pragma 对普通 C++ 编译器是透明的（被忽略），只有 ODB 编译器会解析它们。

### 3.1 核心注解一览

```cpp
// player.hxx
#include <string>
#include <cstdint>
#include <odb/core.hxx>  // odb::nullable 等基础类型

// 标记这个类是一个持久化对象
#pragma db object
class Player
{
public:
    Player() = default;  // ODB 要求默认构造函数

    // 主键，auto 表示数据库自增
    #pragma db id auto
    uint64_t id_{0};

    // 映射为 VARCHAR(32)，加唯一约束和索引
    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    // 默认映射：int -> INT，无需额外标注
    int level_{1};

    // 默认映射：uint64_t -> BIGINT UNSIGNED
    uint64_t exp_{0};

    uint64_t gold_{0};

    // 映射为 DATETIME，指定列名
    #pragma db column("created_at") type("DATETIME") not_null
    std::string createdAt_;

private:
    // ODB 需要访问私有成员，声明友元
    friend class odb::access;
};
```

### 3.2 常用 pragma 速查

| pragma | 含义 | 示例 |
|--------|------|------|
| `#pragma db object` | 标记类为持久化对象 | 类定义前 |
| `#pragma db id` | 主键 | `id auto`（自增）、`id` （手动赋值） |
| `#pragma db column("name")` | 指定列名 | 当 C++ 成员名与列名不同 |
| `#pragma db type("...")` | 指定 SQL 类型 | `type("VARCHAR(64)")` |
| `#pragma db not_null` | 非空约束 | 默认允许 NULL |
| `#pragma db unique` | 唯一约束 | 用户名不可重复 |
| `#pragma db index` | 创建索引 | 加速查询 |
| `#pragma db transient` | 不映射到数据库 | 临时计算字段 |
| `#pragma db readonly` | 只读列 | 只能 persist 时写入 |

### 3.3 transient：跳过不需要持久化的字段

游戏对象通常有大量运行时状态，这些不需要存入数据库：

```cpp
#pragma db object
class Player
{
public:
    #pragma db id auto
    uint64_t id_{0};

    std::string name_;
    int level_{1};

    // ---------- 以下字段不入库 ----------

    // 运行时位置，不需要持久化（进游戏会重新计算）
    #pragma db transient
    float posX_{0.0f};

    #pragma db transient
    float posY_{0.0f};

    // 当前连接的 socket fd，纯运行时状态
    #pragma db transient
    int socketFd_{-1};

    // 是否在线标记，由服务器内存管理
    #pragma db transient
    bool online_{false};

private:
    friend class odb::access;
};
```

### 3.4 类型映射规则

ODB 内置了 C++ 类型到 SQL 类型的默认映射：

| C++ 类型 | MySQL 默认映射 | 备注 |
|----------|---------------|------|
| `bool` | `TINYINT(1)` | |
| `int` / `int32_t` | `INT` | |
| `uint64_t` | `BIGINT UNSIGNED` | 适合 ID、经验值 |
| `float` | `FLOAT` | |
| `double` | `DOUBLE` | |
| `std::string` | `TEXT` | 可用 type 改为 VARCHAR |
| `odb::nullable<T>` | 对应类型 + NULL | 可选字段 |

当默认映射不满足需求时，用 `#pragma db type(...)` 覆盖：

```cpp
// 昵称限制 32 字符，用 VARCHAR 而非 TEXT（节省存储、可建索引）
#pragma db type("VARCHAR(32)") not_null
std::string name_;

// 用 DECIMAL 存储精确金额（避免浮点数精度问题）
#pragma db type("DECIMAL(12,2)")
double realMoney_{0.0};

// 用 TIMESTAMP 自动维护更新时间
#pragma db type("TIMESTAMP") column("updated_at")
std::string updatedAt_;
```

---

## 4. 第一个完整示例：Player 对象持久化

下面是一个完整可运行的示例，展示 ODB 的基本工作流。

### 4.1 定义持久化对象

```cpp
// player.hxx — 玩家持久化对象定义
#ifndef PLAYER_HXX
#define PLAYER_HXX

#include <string>
#include <cstdint>
#include <odb/core.hxx>

#pragma db object table("player")
class Player
{
public:
    Player() = default;

    Player(const std::string& name, int level)
        : name_(name), level_(level) {}

    // --- Getter/Setter ---
    uint64_t id() const { return id_; }
    const std::string& name() const { return name_; }
    int level() const { return level_; }
    uint64_t exp() const { return exp_; }
    uint64_t gold() const { return gold_; }

    void setLevel(int lv) { level_ = lv; }
    void addExp(uint64_t e) { exp_ += e; }
    void addGold(uint64_t g) { gold_ += g; }
    void deductGold(uint64_t g) { gold_ -= g; }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    #pragma db not_null
    int level_{1};

    uint64_t exp_{0};
    uint64_t gold_{0};

    #pragma db type("DATETIME") not_null
    std::string createdAt_;
};

#endif // PLAYER_HXX
```

### 4.2 生成的 SQL（player.sql）

运行 `odb --database mysql --generate-schema player.hxx` 后，ODB 自动生成：

```sql
/* 这段 SQL 完全由 ODB 编译器生成，你无需手写 */
CREATE TABLE `player` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`       VARCHAR(32) NOT NULL,
  `level`      INT NOT NULL,
  `exp`        BIGINT UNSIGNED NULL,
  `gold`       BIGINT UNSIGNED NULL,
  `created_at` DATETIME NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE INDEX `player_name_i` (`name`)
) ENGINE=InnoDB;
```

对照一下：
- `id_` 变成了 `BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY`
- `name_` 的 `type("VARCHAR(32)") not_null unique` 精确映射
- `level_` 的 `not_null` 生成了 `INT NOT NULL`
- `exp_` 和 `gold_` 没有标注 `not_null`，所以允许 NULL

### 4.3 主程序：CRUD 操作

```cpp
// main.cxx — 游戏服务器启动，演示基本 CRUD
#include <iostream>
#include <memory>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/mysql/database.hxx>

#include "player.hxx"
#include "player-odb.hxx"

int main()
{
    try
    {
        // 1. 创建数据库连接
        std::unique_ptr<odb::database> db(
            new odb::mysql::database(
                "game_user",      // 用户名
                "password123",    // 密码
                "game_db",        // 数据库名
                "127.0.0.1",      // 主机
                3306              // 端口
            ));

        // ========== CREATE：新玩家注册 ==========
        uint64_t playerId{0};
        {
            // 开启事务（RAII：离开作用域自动 rollback，除非显式 commit）
            odb::transaction t(db->begin());

            Player newPlayer("剑圣无名", 1);
            newPlayer.addGold(1000);  // 新手奖励 1000 金币

            // persist() 将对象插入数据库，返回后 id_ 被自动填充
            playerId = db->persist(newPlayer);

            t.commit();
            std::cout << "新玩家注册成功，ID = " << playerId << std::endl;
        }
        // 生成的 SQL: INSERT INTO `player` (`name`,`level`,`exp`,`gold`,`created_at`)
        //             VALUES (?,?,?,?,?)

        // ========== READ：加载玩家数据 ==========
        {
            odb::transaction t(db->begin());

            // load() 按主键加载对象
            std::unique_ptr<Player> p(db->load<Player>(playerId));

            std::cout << "加载玩家: " << p->name()
                      << " Lv." << p->level()
                      << " Gold:" << p->gold() << std::endl;

            t.commit();
        }
        // 生成的 SQL: SELECT `id`,`name`,`level`,`exp`,`gold`,`created_at`
        //             FROM `player` WHERE `id`=?

        // ========== UPDATE：玩家升级 ==========
        {
            odb::transaction t(db->begin());

            std::unique_ptr<Player> p(db->load<Player>(playerId));
            p->setLevel(2);
            p->addExp(1500);

            // update() 将修改后的对象同步回数据库
            db->update(*p);

            t.commit();
            std::cout << "玩家升级到 Lv." << p->level() << std::endl;
        }
        // 生成的 SQL: UPDATE `player` SET `name`=?,`level`=?,`exp`=?,`gold`=?,
        //             `created_at`=? WHERE `id`=?

        // ========== DELETE：删除角色 ==========
        {
            odb::transaction t(db->begin());

            // erase() 按主键删除
            db->erase<Player>(playerId);

            t.commit();
            std::cout << "玩家 ID=" << playerId << " 已删除" << std::endl;
        }
        // 生成的 SQL: DELETE FROM `player` WHERE `id`=?
    }
    catch (const odb::exception& e)
    {
        std::cerr << "ODB 错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
```

### 4.4 编译与运行

```bash
# 步骤 1：ODB 编译器生成映射代码
odb --database mysql --generate-query --generate-schema \
    --std c++17 player.hxx

# 步骤 2：编译所有源文件
g++ -std=c++17 -c player-odb.cxx
g++ -std=c++17 -c main.cxx

# 步骤 3：链接
g++ -o game_server main.o player-odb.o \
    -lodb-mysql -lodb -lmysqlclient

# 步骤 4：建表（首次运行）
mysql -u game_user -p game_db < player.sql

# 步骤 5：运行
./game_server
```

输出：
```
新玩家注册成功，ID = 1
加载玩家: 剑圣无名 Lv.1 Gold:1000
玩家升级到 Lv.2
玩家 ID=1 已删除
```

---

## 5. 环境搭建指南

### 5.1 安装 ODB 编译器

**Ubuntu/Debian**：

```bash
# 添加 CodeSynthesis 仓库（以 Ubuntu 22.04 为例）
sudo apt-get install gcc g++ libmysqlclient-dev

# 下载 ODB 编译器
# https://www.codesynthesis.com/products/odb/download.xhtml
wget https://www.codesynthesis.com/download/odb/2.4/odb-2.4.0-x86_64-linux-gnu.tar.bz2
tar xf odb-2.4.0-x86_64-linux-gnu.tar.bz2
sudo cp odb-2.4.0-*/bin/odb /usr/local/bin/

# 安装 ODB 运行时库
# libodb（核心）+ libodb-mysql（MySQL 后端）
sudo apt-get install libodb-dev libodb-mysql-dev
```

**Windows（MSYS2 MINGW64）**：

```bash
# ODB 编译器需要手动下载
# 从 https://www.codesynthesis.com/products/odb/download.xhtml 下载 Windows 版本

# 安装运行时库（通过 build2 或手动编译）
# 或使用 vcpkg：
vcpkg install libodb libodb-mysql
```

### 5.2 CMake 集成

在实际项目中，手动调用 odb 编译器太繁琐。以下是 CMake 自动化配置：

```cmake
cmake_minimum_required(VERSION 3.16)
project(game_server CXX)

set(CMAKE_CXX_STANDARD 17)

# 找到 ODB 编译器
find_program(ODB_COMPILER odb REQUIRED)

# 找到 ODB 运行时库
find_package(PkgConfig REQUIRED)
pkg_check_modules(ODB REQUIRED libodb)
pkg_check_modules(ODB_MYSQL REQUIRED libodb-mysql)

# 定义需要 ODB 处理的头文件
set(ODB_HEADERS
    player.hxx
)

# 自定义命令：调用 ODB 编译器生成代码
foreach(header ${ODB_HEADERS})
    get_filename_component(base ${header} NAME_WE)
    set(odb_output
        ${CMAKE_CURRENT_BINARY_DIR}/${base}-odb.hxx
        ${CMAKE_CURRENT_BINARY_DIR}/${base}-odb.ixx
        ${CMAKE_CURRENT_BINARY_DIR}/${base}-odb.cxx
    )
    add_custom_command(
        OUTPUT ${odb_output}
        COMMAND ${ODB_COMPILER}
            --database mysql
            --generate-query
            --generate-schema
            --std c++17
            --output-dir ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/${header}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${header}
        COMMENT "ODB: 生成 ${base} 的数据库映射代码"
    )
    list(APPEND ODB_GENERATED_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/${base}-odb.cxx)
endforeach()

# 构建游戏服务器
add_executable(game_server
    main.cxx
    ${ODB_GENERATED_SOURCES}
)

target_include_directories(game_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}  # 生成的 -odb.hxx 在这里
    ${ODB_INCLUDE_DIRS}
    ${ODB_MYSQL_INCLUDE_DIRS}
)

target_link_libraries(game_server PRIVATE
    ${ODB_LIBRARIES}
    ${ODB_MYSQL_LIBRARIES}
    mysqlclient
)
```

### 5.3 项目结构推荐

```
game_server/
├── CMakeLists.txt
├── src/
│   ├── main.cxx              # 入口
│   └── db/
│       ├── player.hxx        # ODB 持久化对象定义
│       ├── item.hxx
│       └── guild.hxx
├── build/                    # 构建目录（包含生成的 -odb.* 文件）
└── sql/                      # 可选：手动管理的 SQL 脚本
    └── migrations/
```

---

## 设计要点

### 为什么选择编译期代码生成？

这是 ODB 最核心的设计决策。让我们对比三种 ORM 实现路径：

| 路径 | 代表 | 原理 | 优势 | 劣势 |
|------|------|------|------|------|
| **运行时反射** | Java Hibernate | 反射获取字段信息 | 零配置 | C++ 无原生反射 |
| **模板元编程** | sqlpp11 | 编译期展开模板 | 纯头文件 | 需要手写表描述 DSL |
| **外部代码生成** | ODB / protobuf | 专用编译器预处理 | 零运行时开销 | 多一步构建 |

ODB 选择"外部代码生成"基于以下考量：

1. **零运行时开销** — 生成的代码直接是 `mysql_stmt_execute()` 调用，没有虚函数分发、没有哈希表查找字段名
2. **完整类型信息** — ODB 编译器解析完整的 C++ AST，理解继承、模板、命名空间
3. **编译期错误检测** — 字段名拼错？类型不匹配？**编译时直接报错**，而非运行时崩溃
4. **非侵入性** — 你的类就是普通 C++ 类，不需要继承 `DbObject` 基类，也不需要宏污染

> **游戏服务器视角**：游戏服务器对性能极度敏感。每一帧要处理数千个网络包，数据库操作不能有任何多余开销。编译期生成确保了 ODB 的 persist/load 操作与手写 SQL 性能完全相同——因为它本质上就是帮你生成了那些 SQL 代码。

### pragma 注解为什么用 `#pragma` 而非宏或 C++ 属性？

```cpp
// 方案 A：用宏（侵入性强）
ODB_OBJECT(Player,
    ODB_FIELD(id_, auto_increment),
    ODB_FIELD(name_, varchar(32))
)

// 方案 B：用 C++ 属性（C++11 标准不保证自定义属性可用）
class [[odb::object]] Player {
    [[odb::id, odb::auto]] uint64_t id_;
};

// 方案 C：用 #pragma（ODB 的选择）
#pragma db object
class Player {
    #pragma db id auto
    uint64_t id_;
};
```

ODB 选择 `#pragma` 的原因：

1. **标准保证被忽略** — C++ 标准规定未知的 `#pragma` 应被编译器忽略，不影响正常编译
2. **非侵入性** — 类的定义完全不变，只是"旁边多了几行注释"
3. **位置灵活** — 可以注解类、成员、甚至命名空间
4. **GCC/Clang/MSVC 全兼容** — 不依赖编译器扩展

> **对比 Qt MOC**：Qt 的 MOC 要求你继承 `QObject`、加 `Q_OBJECT` 宏。而 ODB 的类可以是任何形式——POD、继承体系、模板实例化——只要有默认构造函数就行。

### 生成代码的可审计性

ODB 生成的代码是**人可读的 C++**。你可以打开 `player-odb.cxx` 看到：

```cpp
// player-odb.cxx（简化示意）
void access::object_traits_impl<Player, id_mysql>::persist(
    database& db, object_type& obj)
{
    mysql::connection& conn(static_cast<mysql::connection&>(db.connection()));
    statements_type& sts(conn.statement_cache().find_object<object_type>());

    // 绑定参数到预备语句
    image_type& im(sts.image());
    im.id_value = obj.id_;
    im.name_value = obj.name_.c_str();
    im.level_value = obj.level_;
    // ...

    // 执行 INSERT
    sts.persist_statement().execute();

    // 回写自增 ID
    obj.id_ = sts.persist_statement().id();
}
```

这意味着：
- 出了问题可以直接 debug 生成代码
- 性能分析时可以精确定位到哪一行 SQL 执行
- 不是"黑盒"魔法

---

## 小结

本篇我们建立了对 ODB 的整体认知：

1. **定位** — C++ 唯一真正的编译期 ORM，零运行时开销
2. **工作流** — 写 pragma 注解 → odb 编译器生成代码 → 链接库 → CRUD
3. **核心注解** — `object`、`id`、`column`、`type`、`transient` 等
4. **设计哲学** — 编译期做事、非侵入性、类型安全优先

**下一篇**我们将深入连接管理与事务——这是游戏服务器数据一致性的基石。当玩家购买道具时，扣金币和加道具必须是原子操作，ODB 的 RAII 事务如何优雅地解决这个问题？

---

> **系列导航**：**编译器与注解**（本文） | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})
