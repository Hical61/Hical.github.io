+++
title = '深入学习 ODB（六）：Schema 演进与多数据库适配'
date = '2025-06-28'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "SQLite", "Schema迁移", "changelog"]
categories = ["ODB学习"]
description = "掌握 ODB 的 schema 版本管理与 changelog 机制，以游戏版本更新为例实现数据库无感升级；理解多数据库后端的抽象层设计，实现开发用 SQLite、生产用 MySQL 的无缝切换。"
+++

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | **迁移与多库**（本文） | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})

---

## 引言：游戏版本更新的数据库噩梦

游戏上线后，每次版本更新几乎都涉及数据库变更：

- v1.1：给 Player 加 `vip_level` 字段
- v1.2：新增"成就系统"表
- v1.3：把 `gold` 从 INT 改为 BIGINT（金币膨胀了）
- v2.0：重构道具表，拆分装备和消耗品

传统做法是手写 ALTER TABLE 脚本：

```sql
-- v1.1_add_vip.sql
ALTER TABLE player ADD COLUMN vip_level INT NOT NULL DEFAULT 0;

-- v1.2_achievement.sql
CREATE TABLE achievement (...);

-- v1.3_gold_bigint.sql
ALTER TABLE player MODIFY COLUMN gold BIGINT UNSIGNED DEFAULT 0;
```

这套流程的问题：

| 问题 | 后果 |
|------|------|
| SQL 脚本与 C++ 对象定义脱节 | 改了代码忘了改 SQL，运行时崩溃 |
| 手动维护执行顺序 | v1.2 的脚本依赖 v1.1，漏执行就完蛋 |
| 无法自动校验 | 不知道线上 Schema 是否与代码匹配 |
| 多数据库不兼容 | MySQL 的 MODIFY COLUMN 在 SQLite 里不能用 |

ODB 的 **changelog 机制** 从根本上解决这些问题——Schema 变更从 C++ 对象定义**自动推导**，版本化管理，跨数据库兼容。

---

## 1. Schema 生成基础

### 1.1 两种 Schema 生成模式

ODB 支持两种方式管理数据库 Schema：

```
模式 A：独立 SQL 文件
  odb --generate-schema sql player.hxx
  → 生成 player.sql，手动执行建表

模式 B：嵌入式 Schema（推荐）
  odb --generate-schema embedded player.hxx
  → Schema 嵌入 C++ 代码，程序启动时自动建表
```

### 1.2 嵌入式 Schema

嵌入式模式让程序首次启动时自动创建表结构：

```cpp
#include <odb/schema-catalog.hxx>

void initializeDatabase(odb::database& db)
{
    // 在事务内创建所有表
    odb::transaction t(db.begin());

    // 如果表不存在则创建（幂等操作）
    odb::schema_catalog::create_schema(db);

    t.commit();
}
```

ODB 编译器生成的嵌入式 Schema 代码大致如下：

```cpp
// player-odb.cxx（自动生成，简化示意）
static const char create_statement[] =
    "CREATE TABLE IF NOT EXISTS `player` ("
    "  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  `name` VARCHAR(32) NOT NULL,"
    "  `level` INT NOT NULL,"
    "  PRIMARY KEY (`id`)"
    ") ENGINE=InnoDB";
```

### 1.3 游戏服务器的初始化流程

```cpp
int main()
{
    auto db = createDatabase();  // 创建连接

    // 启动时自动检查并创建 Schema
    {
        odb::transaction t(db->begin());

        // create_schema 会检查 schema_version 表
        // 如果是全新数据库 → 创建所有表
        // 如果版本已是最新 → 什么都不做
        odb::schema_catalog::create_schema(*db);

        t.commit();
    }

    // 正常启动游戏逻辑
    startGameServer(*db);
    return 0;
}
```

---

## 2. 版本模型与 Changelog

### 2.1 声明 Schema 版本

通过 pragma 声明当前数据模型的版本号：

```cpp
// 版本声明（通常放在一个公共头文件中）
// 参数：(当前版本, 最早兼容版本)
#pragma db model version(3, 1)
// 含义：当前 Schema 是 v3，支持从 v1 开始的迁移
```

### 2.2 ODB Changelog 工作流

Changelog 是一个 XML 文件，记录每个版本之间的 Schema 差异：

```
┌─────────────┐      ┌─────────────┐      ┌─────────────┐
│  v1 对象定义  │      │  v2 对象定义  │      │  v3 对象定义  │
│  Player:     │      │  Player:     │      │  Player:     │
│    id        │      │    id        │      │    id        │
│    name      │  ──▶ │    name      │  ──▶ │    name      │
│    level     │      │    level     │      │    level     │
│              │      │    vipLevel  │ 新增  │    vipLevel  │
│              │      │              │      │    title     │ 新增
└─────────────┘      └─────────────┘      └─────────────┘
      │                     │                     │
      ▼                     ▼                     ▼
  odb 编译器 ─────▶ changelog.xml ─────▶ 迁移代码
                    (记录 v1→v2→v3 的差异)
```

### 2.3 实际操作流程

**步骤 1：初始版本（v1）**

```cpp
// player.hxx — 版本 1
#pragma db model version(1, 1)

#pragma db object table("player")
class Player
{
    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    int level_{1};
    uint64_t gold_{0};

    friend class odb::access;
};
```

```bash
# 首次生成：创建 changelog 文件
odb --database mysql \
    --generate-schema embedded \
    --generate-query \
    --changelog-dir changelog \
    --std c++17 \
    player.hxx
# 生成文件：
#   changelog/player.xml    — changelog（初始版本快照）
#   player-odb.hxx/ixx/cxx  — 映射代码 + 嵌入式 Schema
```

**步骤 2：版本更新（v1 → v2），添加 VIP 等级**

```cpp
// player.hxx — 版本 2
#pragma db model version(2, 1)  // 当前 v2，兼容从 v1 迁移

#pragma db object table("player")
class Player
{
    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    int level_{1};
    uint64_t gold_{0};

    // v2 新增：VIP 等级
    #pragma db column("vip_level") not_null default(0)
    int vipLevel_{0};

    friend class odb::access;
};
```

```bash
# 重新运行 odb 编译器（自动对比 changelog，生成迁移代码）
odb --database mysql \
    --generate-schema embedded \
    --generate-query \
    --changelog-dir changelog \
    --std c++17 \
    player.hxx
# ODB 编译器会：
# 1. 读取 changelog/player.xml 中的 v1 快照
# 2. 对比当前 C++ 定义（v2）
# 3. 自动检测到：新增了 vip_level 列
# 4. 在 changelog 中记录 v1→v2 的差异
# 5. 在生成代码中嵌入 ALTER TABLE 迁移语句
```

**步骤 3：程序启动时自动迁移**

```cpp
void initializeDatabase(odb::database& db)
{
    odb::transaction t(db.begin());

    // schema_catalog 检查数据库中的 schema_version 表
    // 发现线上是 v1，代码是 v2
    // 自动执行: ALTER TABLE player ADD COLUMN vip_level INT NOT NULL DEFAULT 0
    // 然后更新 schema_version 为 v2
    odb::schema_catalog::create_schema(db);

    t.commit();
}
// 全自动，无需手写 SQL，无需运维介入
```

### 2.4 Changelog XML 结构

```xml
<!-- changelog/player.xml（简化示意） -->
<changelog>
  <!-- v1 初始快照 -->
  <changeset version="1">
    <table name="player">
      <column name="id" type="BIGINT UNSIGNED" not-null="true"/>
      <column name="name" type="VARCHAR(32)" not-null="true"/>
      <column name="level" type="INT" null="true"/>
      <column name="gold" type="BIGINT UNSIGNED" null="true"/>
    </table>
  </changeset>

  <!-- v1 → v2 差异 -->
  <changeset version="2">
    <alter-table name="player">
      <add-column name="vip_level" type="INT" not-null="true" default="0"/>
    </alter-table>
  </changeset>
</changelog>
```

---

## 3. 常见迁移场景

### 3.1 添加新列（带默认值）

这是最安全、最常见的迁移操作：

```cpp
// v2: 给 Player 加 title 字段
#pragma db model version(3, 1)

#pragma db object table("player")
class Player
{
    // ... 已有字段 ...

    // 新增称号字段，默认值为空字符串
    #pragma db type("VARCHAR(64)") not_null default("''")
    std::string title_;
};
```

生成的迁移 SQL：

```sql
ALTER TABLE `player` ADD COLUMN `title` VARCHAR(64) NOT NULL DEFAULT '';
```

> **注意**：`default("''")`——ODB 的 default pragma 值会直接嵌入 SQL，字符串需要带 SQL 的单引号。

### 3.2 添加新表

```cpp
// v3: 新增成就系统
#pragma db model version(3, 1)

#pragma db object table("achievement")
class Achievement
{
    #pragma db id auto
    uint64_t id_{0};

    #pragma db column("player_id") not_null index
    uint64_t playerId_{0};

    #pragma db column("achievement_type") not_null
    int achievementType_{0};

    #pragma db type("DATETIME") column("unlocked_at") not_null
    std::string unlockedAt_;

    friend class odb::access;
};
```

ODB 检测到新的 `#pragma db object`，自动生成 `CREATE TABLE achievement`。

### 3.3 添加索引

```cpp
// v4: 给 player 表的 level 加索引（排行榜查询优化）
#pragma db model version(4, 1)

#pragma db object table("player")
class Player
{
    // 原来没有 index，现在加上
    #pragma db not_null index
    int level_{1};
    // ...
};
```

生成的迁移 SQL：

```sql
CREATE INDEX `player_level_i` ON `player` (`level`);
```

### 3.4 删除列

```cpp
// v5: 移除废弃的 legacyField 字段
// 直接从 C++ 类中删除该成员即可
// ODB 检测到 changelog 中存在但当前代码不存在的列
// 自动生成 ALTER TABLE ... DROP COLUMN
```

> **警告**：删除列是不可逆的。生产环境建议先将列标记为 `#pragma db transient`（不再读写但表结构不变），观察一段时间确认无影响后再真正删除。

### 3.5 修改列类型

```cpp
// v6: 金币从 INT 升级为 BIGINT（数值膨胀）
// 只需修改 C++ 类型：
uint64_t gold_{0};  // 原来可能是 int gold_{0};
// ODB 检测到类型变化，生成:
// ALTER TABLE `player` MODIFY COLUMN `gold` BIGINT UNSIGNED;
```

### 3.6 数据迁移（需要自定义逻辑）

有些迁移不仅是改结构，还需要迁移数据：

```cpp
// 场景：拆分 name 为 firstName + lastName
// ODB 不能自动推导数据转换逻辑，需要手动处理

void migrateV6ToV7(odb::database& db)
{
    // 1. 先让 ODB 执行结构变更（添加新列）
    odb::schema_catalog::migrate(db);

    // 2. 手动执行数据迁移
    odb::transaction t(db.begin());

    db.execute(
        "UPDATE player SET "
        "  first_name = SUBSTRING_INDEX(name, ' ', 1), "
        "  last_name = SUBSTRING_INDEX(name, ' ', -1) "
        "WHERE first_name IS NULL"
    );

    t.commit();
}
```

---

## 4. 迁移管理最佳实践

### 4.1 版本号管理策略

```cpp
// 推荐：版本号与游戏版本同步
// 游戏 v1.0 → Schema v1
// 游戏 v1.1 → Schema v2
// 游戏 v2.0 → Schema v10（大版本可以跳号）

#pragma db model version(current, base)
// current: 当前最新版本
// base:    支持的最老版本（更老的数据库必须先升级到 base）
```

### 4.2 游戏服务器的安全迁移流程

```
┌─────────────────────────────────────────────────────┐
│              停服更新流程                              │
├─────────────────────────────────────────────────────┤
│                                                     │
│  1. 关闭游戏服务器                                    │
│  2. 备份数据库 (mysqldump)                            │
│  3. 启动新版本服务器                                  │
│     → schema_catalog::create_schema() 自动迁移       │
│  4. 验证数据完整性                                    │
│  5. 开服                                            │
│                                                     │
│  如果步骤 3 失败：                                    │
│  → 恢复备份 + 回退到旧版本服务器                       │
└─────────────────────────────────────────────────────┘
```

```cpp
// 更完善的启动迁移代码
void safeSchemaUpgrade(odb::database& db)
{
    odb::transaction t(db.begin());

    // 获取当前数据库版本
    odb::schema_catalog::schema_version sv =
        odb::schema_catalog::current_version(db);

    odb::schema_catalog::schema_version target =
        odb::schema_catalog::base_version();

    if (sv == 0)
    {
        // 全新数据库，创建所有表
        LOG_INFO("全新数据库，创建 Schema...");
        odb::schema_catalog::create_schema(db);
    }
    else if (sv < target)
    {
        // 需要迁移
        LOG_INFO("Schema 从 v{} 迁移到 v{}", sv, target);
        odb::schema_catalog::migrate(db);
    }
    else if (sv == target)
    {
        LOG_INFO("Schema 已是最新版本 v{}", sv);
    }
    else
    {
        // 代码版本比数据库旧？不应该发生
        LOG_ERROR("数据库版本 v{} 高于代码版本 v{}，拒绝启动", sv, target);
        throw std::runtime_error("Schema 版本不匹配");
    }

    t.commit();
}
```

### 4.3 开服/合服的 Schema 管理

游戏运营常见操作：

```
新开服务器：
  1. 创建空数据库 game_server_N
  2. 启动服务器 → schema_catalog::create_schema() 建全部表
  3. 自动完成，无需手动执行 SQL

合服（合并两个服务器数据）：
  1. 确认两个服都是同一 Schema 版本
  2. 导出源服数据 (mysqldump --no-create-info)
  3. 导入目标服（处理 ID 冲突和昵称冲突）
  4. 这一步超出 ODB 范围，需要自定义脚本
```

---

## 5. 多数据库支持

### 5.1 ODB 的数据库后端抽象

ODB 通过编译期选择数据库后端：

```
┌──────────────────────────────────────────────────────┐
│                   你的代码                             │
│  db->persist(player)  /  db->load<Player>(id)        │
├──────────────────────────────────────────────────────┤
│                 ODB 抽象层                             │
│  odb::database / odb::transaction / odb::query        │
├──────┬──────────┬──────────┬──────────┬──────────────┤
│MySQL │ SQLite   │ PostgreSQL│ Oracle  │ SQL Server   │
│libodb│ libodb   │ libodb    │ libodb  │ libodb       │
│-mysql│ -sqlite  │ -pgsql    │ -oracle │ -mssql       │
└──────┴──────────┴──────────┴──────────┴──────────────┘
```

### 5.2 编译期多数据库

用 `--multi-database` 选项生成支持多数据库的代码：

```bash
# 生成同时支持 MySQL 和 SQLite 的代码
odb --multi-database dynamic \
    --database mysql \
    --database sqlite \
    --generate-schema embedded \
    --generate-query \
    --std c++17 \
    player.hxx

# 生成的文件：
#   player-odb.hxx/ixx/cxx     — 通用代码
#   player-odb-mysql.hxx/cxx   — MySQL 特定代码
#   player-odb-sqlite.hxx/cxx  — SQLite 特定代码
```

### 5.3 运行时数据库切换

```cpp
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>
#include <odb/sqlite/database.hxx>

enum class DbBackend { MySQL, SQLite };

// 工厂函数：根据配置创建对应数据库
std::unique_ptr<odb::database> createDatabase(DbBackend backend)
{
    switch (backend)
    {
        case DbBackend::MySQL:
            return std::unique_ptr<odb::database>(
                new odb::mysql::database(
                    "game_user", "password", "game_db",
                    "127.0.0.1", 3306));

        case DbBackend::SQLite:
            return std::unique_ptr<odb::database>(
                new odb::sqlite::database(
                    "game_dev.db",
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
    }
    throw std::runtime_error("不支持的数据库后端");
}

// 业务代码完全不感知后端差异
void savePlayer(odb::database& db, const Player& player)
{
    odb::transaction t(db.begin());
    db.persist(player);
    t.commit();
    // 同一份代码，MySQL 执行 MySQL SQL，SQLite 执行 SQLite SQL
}
```

### 5.4 数据库特有 pragma

当不同数据库需要不同的类型映射时：

```cpp
#pragma db object table("player")
class Player
{
    #pragma db id auto
    uint64_t id_{0};

    // 通用类型（所有数据库都支持）
    #pragma db type("VARCHAR(32)") not_null
    std::string name_;

    // MySQL 特有类型
    #pragma db mysql:type("BIGINT UNSIGNED")
    uint64_t gold_{0};

    // SQLite 特有类型（SQLite 没有 BIGINT，用 INTEGER）
    #pragma db sqlite:type("INTEGER")
    uint64_t gold_{0};

    // 也可以同时指定
    #pragma db mysql:type("DATETIME") sqlite:type("TEXT")
    std::string createdAt_;

    friend class odb::access;
};
```

### 5.5 实战：开发用 SQLite、生产用 MySQL

这是多数据库最实用的模式——开发和测试用轻量的 SQLite，生产用 MySQL：

```cpp
// config.hxx — 配置管理
struct DatabaseConfig
{
    DbBackend backend;
    std::string host;
    int port;
    std::string user;
    std::string password;
    std::string dbName;
    std::string sqlitePath;

    // 从配置文件加载
    static DatabaseConfig load(const std::string& configFile);
};

// main.cxx
int main(int argc, char* argv[])
{
    auto config = DatabaseConfig::load("server.conf");

    // 根据配置选择后端
    auto db = createDatabase(config);

    // Schema 初始化（对 MySQL 和 SQLite 都有效）
    {
        odb::transaction t(db->begin());
        odb::schema_catalog::create_schema(*db);
        t.commit();
    }

    // 后续所有代码完全相同
    startGameServer(*db);
    return 0;
}
```

```ini
# server.conf — 开发环境
[database]
backend = sqlite
sqlite_path = ./game_dev.db

# server.conf — 生产环境
[database]
backend = mysql
host = 10.0.1.100
port = 3306
user = game_user
password = s3cret_pass
db_name = game_world_1
```

### 5.6 单元测试：SQLite 内存数据库

SQLite 支持内存数据库，非常适合单元测试——创建快、运行快、用完即弃：

```cpp
#include <odb/sqlite/database.hxx>

class GameTest : public ::testing::Test
{
protected:
    std::unique_ptr<odb::database> db_;

    void SetUp() override
    {
        // 内存数据库：每个测试用例独立，互不影响
        db_.reset(new odb::sqlite::database(
            ":memory:",                           // 内存模式
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));

        // 创建表结构
        odb::transaction t(db_->begin());
        odb::schema_catalog::create_schema(*db_);
        t.commit();
    }

    void TearDown() override
    {
        db_.reset();  // 析构时内存数据库自动销毁
    }
};

// 测试用例
TEST_F(GameTest, PlayerPersist)
{
    odb::transaction t(db_->begin());

    Player player("测试玩家", 10);
    uint64_t id = db_->persist(player);
    EXPECT_GT(id, 0u);

    auto loaded = db_->load<Player>(id);
    EXPECT_EQ(loaded->name(), "测试玩家");
    EXPECT_EQ(loaded->level(), 10);

    t.commit();
}

TEST_F(GameTest, BuyItemTransaction)
{
    odb::transaction t(db_->begin());

    Player player("买家", 50);
    player.addGold(10000);
    db_->persist(player);

    // 测试道具购买的事务性...

    t.commit();
}
// 每个测试独立的内存数据库，无需清理，毫秒级完成
```

---

## 6. 迁移脚本导出与 CI/CD 集成

### 6.1 导出 SQL 迁移脚本

除了嵌入式迁移，也可以导出独立的 SQL 文件供 DBA 审核：

```bash
# 生成 SQL 格式的迁移脚本
odb --database mysql \
    --generate-schema sql \
    --changelog-dir changelog \
    --schema-format sql \
    --std c++17 \
    player.hxx

# 生成: player.sql（包含完整建表 + 各版本 ALTER）
```

### 6.2 CI/CD 中的 Schema 验证

```yaml
# .github/workflows/schema-check.yml
name: Schema Validation
on: [push, pull_request]

jobs:
  schema-check:
    runs-on: ubuntu-latest
    services:
      mysql:
        image: mysql:8.0
        env:
          MYSQL_ROOT_PASSWORD: test
          MYSQL_DATABASE: game_test
        ports:
          - 3306:3306

    steps:
      - uses: actions/checkout@v4

      - name: Install ODB
        run: |
          # 安装 ODB 编译器和库
          sudo apt-get install -y libodb-dev libodb-mysql-dev

      - name: Generate ODB code
        run: |
          odb --database mysql --generate-schema embedded \
              --generate-query --changelog-dir changelog \
              --std c++17 src/db/*.hxx

      - name: Build and test schema migration
        run: |
          cmake -B build && cmake --build build
          # 运行 Schema 迁移测试
          ./build/schema_test --db-host 127.0.0.1 --db-name game_test
```

---

## 设计要点

### Changelog 为何用 XML 而非 SQL diff？

对比两种迁移记录格式：

```
方案 A：直接存 SQL diff（类似 Flyway）
  V2__add_vip_level.sql:
    ALTER TABLE player ADD COLUMN vip_level INT NOT NULL DEFAULT 0;
  问题：
    - 这条 SQL 只适用于 MySQL
    - 换 PostgreSQL 需要另一套脚本
    - SQL 是"怎么做"，没有记录"改了什么"

方案 B：XML 语义差异（ODB 的选择）
  <alter-table name="player">
    <add-column name="vip_level" type="INT" not-null="true" default="0"/>
  </alter-table>
  优势：
    - 跨数据库：ODB 根据目标数据库生成对应 SQL
    - 语义明确："添加了一列" vs "执行了一条 ALTER"
    - 可逆推导：知道加了什么列，就知道怎么回滚
```

XML changelog 是**声明式**的——描述"Schema 应该变成什么样"，而非"执行什么命令"。ODB 根据目标数据库将其翻译为正确的 DDL。

### 多数据库抽象的编译期 vs 运行时

ODB 的多数据库支持分两种模式：

```
编译期选择（--database mysql）：
  - 只生成 MySQL 代码
  - 二进制体积小
  - 适合：确定只用一种数据库的项目

运行时选择（--multi-database dynamic）：
  - 生成所有指定数据库的代码
  - 运行时通过 database 对象类型决定
  - 二进制体积大（包含多套 SQL）
  - 适合：需要多环境切换的项目
```

游戏服务器推荐：**编译期选 MySQL**（生产环境），但 CMake 中配置两个 target：

```cmake
# 生产 target：MySQL
add_executable(game_server_mysql ...)
target_link_libraries(game_server_mysql PRIVATE odb-mysql)

# 测试 target：SQLite
add_executable(game_test_sqlite ...)
target_link_libraries(game_test_sqlite PRIVATE odb-sqlite)
```

### 与 Flyway / Alembic 等迁移工具的对比

| 对比维度 | ODB Changelog | Flyway (Java) | Alembic (Python) |
|---------|--------------|---------------|------------------|
| 迁移来源 | C++ 对象定义自动推导 | 手写 SQL 脚本 | Python 代码 |
| 跨数据库 | ✅ 同一 changelog | ❌ 每个库一套 SQL | ✅ SQLAlchemy 抽象 |
| 版本跟踪 | schema_version 表 | flyway_schema_history | alembic_version |
| 自动检测变更 | ✅ | ❌ 手动编写 | ✅ autogenerate |
| 数据迁移 | 需手动 SQL | 支持 | 支持 |
| 回滚支持 | 有限 | undo 脚本 | downgrade 函数 |

ODB 的最大优势是**变更来源是 C++ 代码**——修改类定义就自动产生迁移，不需要"改了代码还要再写一遍 ALTER TABLE"。代价是对复杂数据迁移（不仅改结构，还要转换数据）的支持较弱，需要自己写 SQL。

---

## 小结

本篇我们掌握了两个工程化关键能力：

**Schema 迁移**：
| 要素 | 做法 |
|------|------|
| 版本声明 | `#pragma db model version(N, base)` |
| 变更记录 | `--changelog-dir` 生成 XML changelog |
| 自动迁移 | `schema_catalog::create_schema()` / `migrate()` |
| 常见操作 | 加列、加表、加索引、改类型 |

**多数据库**：
| 场景 | 方案 |
|------|------|
| 开发环境 | SQLite 文件数据库（轻量、无需安装） |
| 单元测试 | SQLite `:memory:`（毫秒级创建销毁） |
| 生产环境 | MySQL（连接池、高并发） |
| 切换方式 | 配置文件选择后端，业务代码不变 |

**下一篇**我们将深入性能优化——万人同服的定时存档如何不卡帧？排行榜批量刷新怎么做？N+1 查询陷阱如何规避？ODB 的预备语句缓存、批量操作、section 分段加载将逐一揭晓。

---

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | **迁移与多库**（本文） | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})
