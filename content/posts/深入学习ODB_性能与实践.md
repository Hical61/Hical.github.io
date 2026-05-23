+++
title = '深入学习 ODB（七）：性能调优与生产最佳实践'
date = '2025-07-05'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "性能优化", "批量操作", "连接池"]
categories = ["ODB学习"]
description = "ODB 性能优化全攻略：以万人同服定时存档、排行榜批量刷新、高并发交易为场景，深入预备语句缓存、批量操作、N+1 问题规避、section 分段加载与连接池调优。"
+++

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | **性能与实践**（本文） | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})

---

## 引言：万人同服的存档难题

某 MMO 游戏服务器，5000 人同时在线，每 5 分钟执行一次全服存档：

```cpp
// 朴素实现：逐个存档
void saveAllPlayers(odb::database& db,
                    const std::vector<Player*>& onlinePlayers)
{
    for (auto* player : onlinePlayers)
    {
        odb::transaction t(db.begin());
        db.update(*player);
        t.commit();
    }
}
// 5000 个玩家 × 每次 1 个事务 × 每事务 1 条 UPDATE
// = 5000 次网络往返 + 5000 次事务提交
// 耗时：约 15~30 秒（取决于网络延迟）
// 这期间游戏逻辑被阻塞，玩家感受到明显卡顿
```

这段代码有三个性能杀手：

| 问题 | 开销 |
|------|------|
| 每个 update 单独开事务 | 5000 次 COMMIT，每次刷盘 |
| 逐条执行 SQL | 5000 次网络往返 |
| 未使用预备语句缓存 | 每条 SQL 都被 MySQL 重新解析 |

本篇将系统性地解决这些问题，把 30 秒的存档压缩到 1 秒以内。

---

## 1. 预备语句与查询缓存

### 1.1 预备语句自动管理

ODB 默认使用 MySQL 预备语句（prepared statements），并自动维护语句缓存（statement cache）：

```
首次 db.update(player):
  ┌────────────────────────────────────────────────┐
  │ 1. PREPARE stmt_update_player FROM             │
  │    'UPDATE player SET name=?,level=?,exp=?,    │
  │     gold=? WHERE id=?'                         │ ← MySQL 编译 SQL
  │ 2. EXECUTE stmt_update_player USING            │
  │    ['剑圣无名', 50, 12000, 99999, 1001]        │ ← 绑定参数执行
  │ 3. 将 stmt_update_player 存入连接的语句缓存     │
  └────────────────────────────────────────────────┘

后续 db.update(anotherPlayer):
  ┌────────────────────────────────────────────────┐
  │ 1. 命中缓存，跳过 PREPARE                       │
  │ 2. EXECUTE stmt_update_player USING            │
  │    ['花间游', 80, 45000, 200000, 1002]          │ ← 只绑定新参数
  └────────────────────────────────────────────────┘
```

**性能收益**：
- MySQL 跳过 SQL 解析和查询优化阶段（节省约 30% 单次查询时间）
- 每个连接独立缓存，无锁争用
- 完全自动，开发者无感

### 1.2 语句缓存的生命周期

```
连接级别缓存：
  ┌─────────────────────────────────────┐
  │  Connection #1                       │
  │  ┌─────────────────────────────┐    │
  │  │ Statement Cache:            │    │
  │  │  persist<Player>  → stmt_1  │    │
  │  │  load<Player>     → stmt_2  │    │
  │  │  update<Player>   → stmt_3  │    │
  │  │  erase<Player>    → stmt_4  │    │
  │  │  query<Player>(Q) → stmt_5  │    │
  │  └─────────────────────────────┘    │
  └─────────────────────────────────────┘

缓存销毁时机：
  - 连接归还到池中：缓存保留（下次复用）
  - 连接断开重连：缓存重建
  - 连接池销毁：全部清理
```

### 1.3 查看缓存命中

```cpp
// 通过 MySQL 状态变量监控
// mysql> SHOW GLOBAL STATUS LIKE 'Prepared_stmt%';
// +-----------------------------+-------+
// | Variable_name               | Value |
// +-----------------------------+-------+
// | Prepared_stmt_count         | 24    |  ← 当前缓存的预备语句数
// | Com_stmt_execute            | 85000 |  ← 执行次数
// | Com_stmt_prepare            | 24    |  ← 编译次数（远小于执行次数 = 命中率高）
// +-----------------------------+-------+
// 命中率 = 1 - (24 / 85000) ≈ 99.97%
```

---

## 2. 批量操作优化

### 2.1 事务批处理：首要优化

最简单也是效果最显著的优化——**把多个操作合并到一个事务里**：

```cpp
// ❌ 5000 个事务：每个 update 一个事务
for (auto* player : onlinePlayers)
{
    odb::transaction t(db.begin());
    db.update(*player);
    t.commit();  // 每次 COMMIT 触发一次磁盘刷写
}
// 耗时：~15-30 秒

// ✅ 1 个事务：所有 update 合并
{
    odb::transaction t(db.begin());
    for (auto* player : onlinePlayers)
    {
        db.update(*player);  // 只发 EXECUTE，不 COMMIT
    }
    t.commit();  // 只刷盘一次
}
// 耗时：~1-3 秒（提升 10~15 倍）
```

**为什么快这么多？**

| 操作 | 5000 事务 | 1 事务 |
|------|-----------|--------|
| COMMIT（磁盘刷写） | 5000 次 | 1 次 |
| 网络往返（COMMIT 等待确认） | 5000 次 | 1 次 |
| redo log 刷盘 | 5000 次 | 1 次 |

InnoDB 的 COMMIT 是最昂贵的操作——它需要将 redo log 刷到磁盘以保证持久性。合并事务直接消除了这个瓶颈。

### 2.2 分批提交：平衡安全与性能

一个巨大的事务也有风险——如果在第 4999 个玩家时崩溃，所有人的数据都回滚。分批提交是折中方案：

```cpp
void batchSaveAllPlayers(odb::database& db,
                         const std::vector<Player*>& players)
{
    const size_t batchSize = 500;  // 每批 500 个

    for (size_t i = 0; i < players.size(); i += batchSize)
    {
        odb::transaction t(db.begin());

        size_t end = std::min(i + batchSize, players.size());
        for (size_t j = i; j < end; ++j)
        {
            db.update(*players[j]);
        }

        t.commit();
        // 每 500 人提交一次
        // 最坏情况只丢失最后一批未提交的数据
    }
}
// 5000 人 / 500 批 = 10 次 COMMIT（而非 5000 次）
// 耗时：~1-2 秒
```

### 2.3 persist 批量插入

新开服时需要导入大量初始数据（如 NPC、商品模板）：

```cpp
void importItemTemplates(odb::database& db,
                         const std::vector<ItemTemplate>& templates)
{
    const size_t batchSize = 1000;

    for (size_t i = 0; i < templates.size(); i += batchSize)
    {
        odb::transaction t(db.begin());

        size_t end = std::min(i + batchSize, templates.size());
        for (size_t j = i; j < end; ++j)
        {
            db.persist(templates[j]);
        }

        t.commit();
    }
}
// 10000 个模板 / 1000 批 = 10 次 COMMIT
```

### 2.4 与原生 SQL 批量操作的对比

对于极端场景（百万级插入），ODB 的逐条 persist 仍然比原生 `LOAD DATA INFILE` 慢：

| 方式 | 10 万行插入耗时 | 适用场景 |
|------|---------------|---------|
| 逐条 persist（单事务） | ~5-10 秒 | 日常业务操作 |
| 分批 persist（1000/批） | ~5-10 秒 | 日常批量操作 |
| 原生 `INSERT ... VALUES (),(),()` | ~2-3 秒 | 需要更快的批量插入 |
| `LOAD DATA INFILE` | ~0.5-1 秒 | 初始化导入、数据迁移 |

> **游戏服务器建议**：日常存档用 ODB 分批提交足够。只有开服导入初始数据、合服迁移等一次性操作才需要考虑原生 SQL。

---

## 3. 延迟加载与 N+1 问题

### 3.1 什么是 N+1 问题

N+1 问题是 ORM 最经典的性能陷阱：

```cpp
// 场景：显示公会成员列表
void showGuildMembers(odb::database& db, uint64_t guildId)
{
    odb::session s;
    odb::transaction t(db.begin());

    // 查询 1：加载公会
    auto guild = db.load<Guild>(guildId);
    // SQL: SELECT ... FROM guild WHERE id = ?       ← 1 条查询

    // 查询 N：逐个加载成员
    for (auto& memberPtr : guild->members())
    {
        auto member = memberPtr.load();  // 每次触发一条 SQL
        // SQL: SELECT ... FROM player WHERE id = ?  ← N 条查询
        std::cout << member->name() << " Lv." << member->level() << std::endl;
    }

    t.commit();
}
// 如果公会有 50 个成员：
// 1（加载公会）+ 50（逐个加载成员）= 51 条 SQL
// 这就是 "N+1 问题"
```

### 3.2 用 View 规避 N+1

View 直接在数据库端 JOIN，一条 SQL 拿到所有数据：

```cpp
// 定义 View：公会成员信息（JOIN 查询）
#pragma db view object(Guild) object(Player: Guild::members_)
struct GuildMemberInfo
{
    #pragma db column(Player::id_)
    uint64_t playerId;

    #pragma db column(Player::name_)
    std::string playerName;

    #pragma db column(Player::level_)
    int playerLevel;
};

void showGuildMembersOptimized(odb::database& db, uint64_t guildId)
{
    odb::transaction t(db.begin());

    typedef odb::query<GuildMemberInfo> Q;

    // 一条 SQL 搞定
    auto r = db.query<GuildMemberInfo>(
        Q::Guild::id == guildId
        + "ORDER BY" + Q::Player::level + "DESC"
    );

    for (const GuildMemberInfo& m : r)
    {
        std::cout << m.playerName << " Lv." << m.playerLevel << std::endl;
    }

    t.commit();
}
// SQL: SELECT player.id, player.name, player.level
//      FROM guild
//      JOIN guild_members ON ...
//      JOIN player ON ...
//      WHERE guild.id = ?
//      ORDER BY player.level DESC
// → 1 条 SQL，无论多少成员
```

**对比**：

| 方式 | SQL 条数 | 网络往返 | 适用场景 |
|------|---------|---------|---------|
| lazy_ptr 逐个 load | 1 + N | 1 + N | 按需访问个别成员 |
| eager 加载 | 1 + N（自动） | 1 + N | 总是需要所有成员 |
| View JOIN | 1 | 1 | 列表展示、只需部分字段 |

### 3.3 用查询替代关系遍历

另一种规避 N+1 的方式——直接查询"多方"，而非通过"一方"遍历：

```cpp
// ❌ N+1：通过 Player 遍历背包
auto player = db.load<Player>(playerId);
for (auto& itemPtr : player->inventory())
{
    auto item = itemPtr.load();  // 每个道具一条 SQL
}

// ✅ 1 条 SQL：直接查询该玩家的所有道具
typedef odb::query<InventoryItem> Q;
auto items = db.query<InventoryItem>(Q::owner == playerId);
for (const InventoryItem& item : items)
{
    // 已经是完整对象，无需额外 load
}
```

### 3.4 识别 N+1 问题

使用 SQL tracer 检测：

```cpp
class N1Detector : public odb::tracer
{
public:
    void execute(odb::connection&, const char* stmt) override
    {
        queryCount_++;
        if (queryCount_ > threshold_)
        {
            LOG_WARN("疑似 N+1 问题：单次业务操作执行了 {} 条 SQL",
                     queryCount_);
        }
    }

    void reset() { queryCount_ = 0; }

private:
    int queryCount_{0};
    int threshold_{10};  // 超过 10 条 SQL 告警
};

// 使用
N1Detector detector;
db.tracer(detector);

detector.reset();
showGuildMembers(db, guildId);  // 如果触发 N+1，会打印告警
```

---

## 4. Section：分段加载

### 4.1 问题：大对象的按需加载

玩家对象可能包含很多数据，但不是每次都需要全部加载：

```cpp
#pragma db object table("player")
class Player
{
    // === 核心数据（每次登录必加载）===
    uint64_t id_;
    std::string name_;
    int level_;
    uint64_t gold_;

    // === 扩展数据（打开面板时才需要）===
    std::string biography_;      // 个人传记（可能几百字）
    std::vector<uint64_t> achievements_;  // 成就列表（可能上百个）
    std::map<uint64_t, int> skills_;      // 技能树（几十个技能）
    std::string settings_;       // 用户配置（JSON 字符串，可能很长）
};
// 如果每次 load 都加载全部字段：
// SELECT id, name, level, gold, biography, settings, ... FROM player
// achievements 还需要额外查询 player_achievements 表
// 大量无用数据传输
```

### 4.2 用 section 拆分加载

ODB 的 `section` 机制允许将对象的字段分组，按需加载：

```cpp
#include <odb/section.hxx>

#pragma db object table("player")
class Player
{
public:
    Player() = default;

    // 核心字段的 getter（默认加载）
    uint64_t id() const { return id_; }
    const std::string& name() const { return name_; }
    int level() const { return level_; }
    uint64_t gold() const { return gold_; }

    // 扩展字段的 getter（需要先加载 section）
    const std::string& biography() const { return biography_; }
    const std::map<uint64_t, int>& skills() const { return skills_; }

    // 获取 section 对象（用于手动触发加载）
    odb::section& extendedSection() { return extendedSection_; }

private:
    friend class odb::access;

    // === 默认段：load() 时自动加载 ===
    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    int level_{1};
    uint64_t gold_{0};

    // === 扩展段：手动调用 load(section) 才加载 ===
    #pragma db load(lazy) update(change) section(extendedSection_)
    std::string biography_;

    #pragma db load(lazy) update(change) section(extendedSection_) \
             table("player_skills") key_column("skill_id") value_column("level")
    std::map<uint64_t, int> skills_;

    #pragma db load(lazy) update(change) section(extendedSection_) \
             table("player_achievements")
    std::vector<uint64_t> achievements_;

    // section 对象本身
    #pragma db transient
    odb::section extendedSection_;
};
```

### 4.3 使用 section

```cpp
void onPlayerLogin(odb::database& db, uint64_t playerId)
{
    odb::transaction t(db.begin());

    // 加载核心数据（id, name, level, gold）
    auto player = db.load<Player>(playerId);
    // SQL: SELECT id, name, level, gold FROM player WHERE id = ?
    // biography, skills, achievements 都没有被加载

    std::cout << player->name() << " Lv." << player->level() << std::endl;

    t.commit();
}

void onOpenProfilePanel(odb::database& db, Player& player)
{
    odb::transaction t(db.begin());

    // 玩家打开个人资料面板时，加载扩展数据
    db.load(player, player.extendedSection());
    // SQL: SELECT biography FROM player WHERE id = ?
    // SQL: SELECT skill_id, level FROM player_skills WHERE object_id = ?
    // SQL: SELECT value FROM player_achievements WHERE object_id = ?

    std::cout << "传记: " << player.biography() << std::endl;
    std::cout << "技能数: " << player.skills().size() << std::endl;

    t.commit();
}

void onPlayerSave(odb::database& db, Player& player)
{
    odb::transaction t(db.begin());

    // 更新核心数据（总是执行）
    db.update(player);

    // 扩展数据只在被修改时才更新（update(change)）
    if (player.extendedSection().loaded()
        && player.extendedSection().changed())
    {
        db.update(player, player.extendedSection());
    }

    t.commit();
}
```

### 4.4 section 的策略选项

| pragma | 含义 | 适用场景 |
|--------|------|---------|
| `load(lazy)` | 延迟加载，手动触发 | 大字段、容器字段 |
| `load(eager)` | 随主对象一起加载 | 小字段、总是需要 |
| `update(always)` | 每次 update 都同步 | 数据频繁变化 |
| `update(change)` | 只在标记为 changed 时更新 | 数据不常变化 |
| `update(manual)` | 完全手动控制 | 精细控制更新时机 |

> **游戏服务器典型分段**：
> - **默认段**：id、名字、等级、金币、位置——每帧/每次存档都需要
> - **扩展段**：传记、成就、技能——打开面板时才加载
> - **统计段**：总在线时长、击杀数——GM 后台查询时才需要

---

## 5. 连接池与并发调优

### 5.1 连接池大小

经验公式：**连接数 = CPU 核数 × 2 + 磁盘数**

```cpp
// 8 核服务器，1 块 SSD
// 推荐：8 × 2 + 1 = 17，取整 20
auto factory = std::make_unique<odb::mysql::connection_pool_factory>(
    5,    // min：最少保持 5 个空闲连接
    20    // max：最多 20 个连接
);
```

为什么不是越多越好？

```
连接数过少（如 2 个）：
  Thread1 占用 conn1 → Thread2 占用 conn2 → Thread3 等待...
  → 并发度不够，请求排队

连接数过多（如 200 个）：
  200 个连接同时发 SQL → MySQL 内部锁争用加剧
  → 上下文切换开销 → 整体吞吐量反而下降

最优区间（CPU×2 附近）：
  足够并发，不造成 MySQL 内部争用
```

### 5.2 长事务 vs 短事务

```cpp
// ❌ 长事务：持有连接时间过长
void badPattern(odb::database& db)
{
    odb::transaction t(db.begin());  // 获取连接

    auto player = db.load<Player>(playerId);

    // ... 执行大量 CPU 计算（5 秒）...
    auto result = calculateCombatResult(player);

    // ... 等待网络回调（可能几秒）...
    waitForClientConfirm();

    db.update(*player);
    t.commit();  // 连接被占用了 10+ 秒
}
// 后果：20 个连接被 20 个长事务占满，其他请求全部排队

// ✅ 短事务：快速读取/写入，中间处理不持有连接
void goodPattern(odb::database& db)
{
    Player playerCopy;

    // 事务 1：快速读取
    {
        odb::transaction t(db.begin());
        auto player = db.load<Player>(playerId);
        playerCopy = *player;  // 拷贝到内存
        t.commit();
    }
    // 连接已归还

    // CPU 计算和网络等待（不占连接）
    auto result = calculateCombatResult(playerCopy);
    waitForClientConfirm();

    // 事务 2：快速写入
    {
        odb::transaction t(db.begin());
        applyResult(db, playerId, result);
        t.commit();
    }
    // 连接已归还
}
```

### 5.3 读写分离思路

高负载游戏服务器可以配置 MySQL 主从复制，读操作走从库：

```cpp
class DatabaseManager
{
public:
    DatabaseManager()
    {
        // 主库：写操作
        master_ = createMySQLDatabase("10.0.1.100", 3306);

        // 从库：读操作（可配多个）
        slaves_.push_back(createMySQLDatabase("10.0.1.101", 3306));
        slaves_.push_back(createMySQLDatabase("10.0.1.102", 3306));
    }

    // 写操作走主库
    odb::database& master() { return *master_; }

    // 读操作走从库（轮询负载均衡）
    odb::database& slave()
    {
        size_t idx = nextSlave_.fetch_add(1) % slaves_.size();
        return *slaves_[idx];
    }

private:
    std::unique_ptr<odb::database> master_;
    std::vector<std::unique_ptr<odb::database>> slaves_;
    std::atomic<size_t> nextSlave_{0};
};

// 使用
void onLeaderboardRequest(DatabaseManager& dbm, uint64_t playerId)
{
    // 排行榜查询走从库，不影响主库写入性能
    odb::transaction t(dbm.slave().begin());
    auto leaderboard = dbm.slave().query<LeaderboardEntry>(...);
    t.commit();
}

void onPlayerSave(DatabaseManager& dbm, Player& player)
{
    // 玩家存档走主库
    odb::transaction t(dbm.master().begin());
    dbm.master().update(player);
    t.commit();
}
```

---

## 6. 生产最佳实践

### 6.1 项目结构推荐

```
game_server/
├── CMakeLists.txt
├── src/
│   ├── main.cxx
│   ├── db/                          # 数据库层
│   │   ├── objects/                 # ODB 持久化对象定义
│   │   │   ├── player.hxx
│   │   │   ├── inventory_item.hxx
│   │   │   ├── guild.hxx
│   │   │   └── trade_log.hxx
│   │   ├── views/                   # ODB View 定义
│   │   │   ├── leaderboard_view.hxx
│   │   │   ├── guild_stats_view.hxx
│   │   │   └── trade_report_view.hxx
│   │   ├── repository/              # Repository 封装层
│   │   │   ├── player_repo.hxx
│   │   │   ├── player_repo.cxx
│   │   │   ├── guild_repo.hxx
│   │   │   └── guild_repo.cxx
│   │   └── db_manager.hxx          # 连接管理
│   ├── service/                     # 业务逻辑层
│   │   ├── shop_service.cxx
│   │   └── guild_service.cxx
│   └── net/                         # 网络层
├── changelog/                       # ODB changelog 目录
│   ├── player.xml
│   ├── inventory_item.xml
│   └── guild.xml
└── build/                           # 构建目录（ODB 生成文件在这里）
```

### 6.2 CMake 集成模板

```cmake
cmake_minimum_required(VERSION 3.16)
project(game_server CXX)

set(CMAKE_CXX_STANDARD 17)

find_program(ODB_COMPILER odb REQUIRED)

# ODB 持久化对象头文件列表
set(ODB_HEADERS
    src/db/objects/player.hxx
    src/db/objects/inventory_item.hxx
    src/db/objects/guild.hxx
    src/db/objects/trade_log.hxx
    src/db/views/leaderboard_view.hxx
    src/db/views/guild_stats_view.hxx
)

# 批量生成 ODB 代码
set(ODB_GENERATED_SOURCES)
foreach(header ${ODB_HEADERS})
    get_filename_component(base ${header} NAME_WE)
    get_filename_component(dir ${header} DIRECTORY)

    set(odb_cxx ${CMAKE_CURRENT_BINARY_DIR}/${base}-odb.cxx)

    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/${base}-odb.hxx
            ${CMAKE_CURRENT_BINARY_DIR}/${base}-odb.ixx
            ${odb_cxx}
        COMMAND ${ODB_COMPILER}
            --database mysql
            --generate-schema embedded
            --generate-query
            --changelog-dir ${CMAKE_SOURCE_DIR}/changelog
            --std c++17
            --output-dir ${CMAKE_CURRENT_BINARY_DIR}
            -I ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/${header}
        DEPENDS ${CMAKE_SOURCE_DIR}/${header}
        COMMENT "ODB: ${base}"
    )
    list(APPEND ODB_GENERATED_SOURCES ${odb_cxx})
endforeach()

add_executable(game_server
    src/main.cxx
    src/db/repository/player_repo.cxx
    src/db/repository/guild_repo.cxx
    src/service/shop_service.cxx
    ${ODB_GENERATED_SOURCES}
)

target_include_directories(game_server PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(game_server PRIVATE
    odb-mysql odb mysqlclient pthread
)
```

### 6.3 日志与调试

```cpp
// 开发环境：打印所有 SQL
class DebugTracer : public odb::tracer
{
public:
    void execute(odb::connection&, const char* stmt) override
    {
        LOG_DEBUG("[SQL] {}", stmt);
    }
};

// 生产环境：只记录慢查询
class SlowQueryTracer : public odb::tracer
{
public:
    void execute(odb::connection&, const char* stmt) override
    {
        auto start = std::chrono::steady_clock::now();
        // ... 实际执行 ...
        auto elapsed = std::chrono::steady_clock::now() - start;

        if (elapsed > std::chrono::milliseconds(100))
        {
            LOG_WARN("[慢查询 {}ms] {}", 
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         elapsed).count(),
                     stmt);
        }
    }
};

// 按环境配置
#ifdef NDEBUG
    db->tracer(slowQueryTracer);   // Release：只记慢查询
#else
    db->tracer(debugTracer);       // Debug：全量记录
#endif
```

### 6.4 常见陷阱与排错

| 陷阱 | 现象 | 解决 |
|------|------|------|
| 事务外操作 | `not in transaction` 异常 | 所有 ODB 操作必须在事务内 |
| session 跨事务 | 数据不刷新 | 每个业务请求独立 session |
| 忘记 `friend class odb::access` | 编译错误 | 持久化类必须声明 |
| 没有默认构造函数 | ODB 编译器报错 | 持久化类必须有 `T() = default` |
| 容器全量替换 | 大容器 update 慢 | 用 section + `update(change)` |
| lazy_ptr 事务外 load | 连接已归还 | 确保 load 在事务作用域内 |
| 循环引用死锁 | A→B→A 互相 load | 使用 `session` + `weak_ptr` |

---

## 设计要点

### 预备语句为何默认开启？

在其他语言的 ORM 中（如 Python SQLAlchemy），预备语句往往是可选的优化。但 ODB 默认启用，原因在于 C++ 的使用模式：

1. **C++ 服务器通常是长连接** — 连接复用率极高，语句缓存命中率接近 100%
2. **C++ 对性能敏感** — 选择 C++ 的项目通常不能容忍不必要的开销
3. **安全性加成** — 预备语句天然防 SQL 注入，没有理由不用

### section 分段加载的设计思路

section 的设计借鉴了数据库的**列族（Column Family）**概念（如 HBase、Cassandra）：

```
传统做法：一个对象 = 一次查询加载全部字段
  SELECT * FROM player WHERE id = 1001
  → 加载了 20 个字段，但这次只用到 4 个

section 做法：一个对象 = 核心段（自动）+ 扩展段（按需）
  SELECT id, name, level, gold FROM player WHERE id = 1001  ← 核心段
  （用户打开面板时才执行）
  SELECT biography FROM player WHERE id = 1001               ← 扩展段
```

核心权衡：**用更多的 SQL 次数换取更少的数据传输**。在游戏服务器中，玩家上线后大部分时间只需要核心数据，扩展数据的访问频率低得多。

### ODB 在架构中的定位

ODB 应该放在架构的**数据访问层**，不应该渗透到业务逻辑层：

```
推荐架构：
  ┌─────────────┐
  │  网络层      │  接收/发送协议包
  ├─────────────┤
  │  业务逻辑层  │  游戏规则、战斗计算
  │             │  不直接调用 db.query()
  ├─────────────┤
  │  Repository │  封装 ODB 操作
  │  数据访问层  │  PlayerRepo::findByName()
  ├─────────────┤
  │  ODB        │  对象-关系映射
  ├─────────────┤
  │  MySQL      │  数据存储
  └─────────────┘
```

Repository 层的好处：
- 业务层不依赖 ODB 类型（`odb::query`、`odb::transaction`）
- 可以为测试提供 Mock Repository
- 集中管理查询逻辑，避免到处散落的 `db.query()`

---

## 小结

本篇我们系统地掌握了 ODB 的性能优化手段：

| 优化手段 | 效果 | 适用场景 |
|---------|------|---------|
| 事务批处理 | 10~15 倍提升 | 批量存档、批量导入 |
| 预备语句缓存 | 30% 单次提升 | 所有场景（自动启用） |
| View 替代 N+1 | N 倍提升 | 列表展示、关系遍历 |
| section 分段加载 | 减少无用传输 | 大对象按需加载 |
| 连接池调优 | 避免瓶颈 | 高并发场景 |
| 短事务模式 | 提高连接利用率 | 需要等待的业务 |

**下一篇**——系列最终章，我们将综合运用全部特性，从零构建一个完整的游戏存档系统：玩家数据、背包道具、装备强化、公会系统、交易日志，完整的 Repository 层 + Schema 迁移工作流。

---

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | **性能与实践**（本文） | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})
