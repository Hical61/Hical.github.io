+++
title = '深入学习 ODB（五）：继承策略与 View 的强大抽象'
date = '2025-06-21'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "继承映射", "视图", "View"]
categories = ["ODB学习"]
description = "深入 ODB 三种继承映射策略（单表、类表、具体表），以道具继承体系为例完整对比；掌握 View 进行跨对象查询、聚合统计，构建排行榜与战力报表。"
+++

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | **继承与视图**（本文） | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})

---

## 引言：道具系统的继承难题

游戏中的道具系统天然适合用继承建模：

```
Item（道具基类）
├── Weapon（武器）    — 攻击力、攻速、暴击率
├── Armor（防具）     — 防御力、抗性、耐久度
└── Consumable（消耗品）— 恢复量、冷却时间、持续时间
```

在 C++ 中，这是一个干净的继承体系。但映射到关系数据库时，问题来了——**关系数据库没有继承的概念**。一张表怎么存不同子类的字段？ODB 提供了三种策略来解决这个问题，各有取舍。

此外，游戏运营经常需要统计数据——"全服多少人在线"、"公会战力排名"、"每日交易额"。加载完整对象再聚合太慢了，ODB 的 **View**（视图）让你直接获取投影和聚合结果，不产生完整对象。

---

## 1. 继承映射三策略

### 1.1 共同的基类定义

三种策略使用相同的 C++ 继承体系，只是 pragma 配置不同：

```cpp
// item_base.hxx — 道具基类（所有策略共用）
#include <string>
#include <cstdint>
#include <odb/core.hxx>

// 道具品质枚举
enum class ItemQuality : int
{
    White  = 1,  // 白色 - 普通
    Green  = 2,  // 绿色 - 优秀
    Blue   = 3,  // 蓝色 - 精良
    Purple = 4,  // 紫色 - 史诗
    Orange = 5   // 橙色 - 传说
};
```

### 1.2 策略一：单表继承（Table Per Hierarchy）

**所有子类共用一张表**，通过鉴别器列（discriminator）区分类型：

```cpp
// 单表继承：所有道具存在同一张 item 表
#pragma db object polymorphic table("item")
class Item
{
public:
    Item() = default;
    Item(const std::string& name, ItemQuality quality)
        : name_(name), quality_(quality) {}

    virtual ~Item() = default;  // 多态基类必须有虚析构

    uint64_t id() const { return id_; }
    const std::string& name() const { return name_; }
    ItemQuality quality() const { return quality_; }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(64)") not_null
    std::string name_;

    #pragma db not_null
    ItemQuality quality_{ItemQuality::White};

    // ODB 自动添加鉴别器列 typeid，用于区分子类
};

// 武器
#pragma db object table("item")
class Weapon : public Item
{
public:
    Weapon() = default;
    Weapon(const std::string& name, ItemQuality q,
           int attack, float attackSpeed)
        : Item(name, q), attack_(attack), attackSpeed_(attackSpeed) {}

    int attack() const { return attack_; }
    float attackSpeed() const { return attackSpeed_; }

private:
    friend class odb::access;

    int attack_{0};

    #pragma db column("attack_speed")
    float attackSpeed_{1.0f};

    #pragma db column("crit_rate") type("FLOAT")
    float critRate_{0.0f};
};

// 防具
#pragma db object table("item")
class Armor : public Item
{
public:
    Armor() = default;
    Armor(const std::string& name, ItemQuality q,
          int defense, int durability)
        : Item(name, q), defense_(defense), durability_(durability) {}

    int defense() const { return defense_; }
    int durability() const { return durability_; }

private:
    friend class odb::access;

    int defense_{0};
    int durability_{100};
    int resistance_{0};
};

// 消耗品
#pragma db object table("item")
class Consumable : public Item
{
public:
    Consumable() = default;
    Consumable(const std::string& name, ItemQuality q,
               int healAmount, float cooldown)
        : Item(name, q), healAmount_(healAmount), cooldown_(cooldown) {}

    int healAmount() const { return healAmount_; }

private:
    friend class odb::access;

    #pragma db column("heal_amount")
    int healAmount_{0};

    float cooldown_{0.0f};     // 冷却时间（秒）
    float duration_{0.0f};     // 持续时间（秒）
};
```

**生成的 SQL**：

```sql
-- 所有子类共用一张表
CREATE TABLE `item` (
  `typeid`       VARCHAR(256) NOT NULL,  -- 鉴别器：存储实际类型名
  `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`         VARCHAR(64) NOT NULL,
  `quality`      INT NOT NULL,
  -- Weapon 的字段
  `attack`       INT NULL,
  `attack_speed` FLOAT NULL,
  `crit_rate`    FLOAT NULL,
  -- Armor 的字段
  `defense`      INT NULL,
  `durability`   INT NULL,
  `resistance`   INT NULL,
  -- Consumable 的字段
  `heal_amount`  INT NULL,
  `cooldown`     FLOAT NULL,
  `duration`     FLOAT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB;
```

**单表继承的特征**：

| 优势                  | 劣势                                      |
| --------------------- | ----------------------------------------- |
| 查询最快——无需 JOIN   | 大量 NULL 列（武器行的防御字段全是 NULL） |
| 插入/更新只操作一张表 | 子类字段无法加 NOT NULL 约束              |
| 多态查询天然支持      | 表会越来越宽，子类越多列越多              |

### 1.3 策略二：类表继承（Table Per Class，ODB 默认）

**每个类一张表**，子类表通过 JOIN 关联基类表：

```cpp
// 类表继承：每个类独立建表（ODB 默认策略）
#pragma db object polymorphic
class Item
{
public:
    // ... 同上
private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(64)") not_null
    std::string name_;

    #pragma db not_null
    ItemQuality quality_{ItemQuality::White};
};

#pragma db object
class Weapon : public Item
{
    // 子类不需要额外的 #pragma db id
    // 共享基类的 id，通过 JOIN 关联
private:
    friend class odb::access;

    int attack_{0};
    float attackSpeed_{1.0f};
    float critRate_{0.0f};
};

#pragma db object
class Armor : public Item
{
private:
    friend class odb::access;

    int defense_{0};
    int durability_{100};
    int resistance_{0};
};

#pragma db object
class Consumable : public Item
{
private:
    friend class odb::access;

    int healAmount_{0};
    float cooldown_{0.0f};
    float duration_{0.0f};
};
```

**生成的 SQL**：

```sql
-- 基类表：只有公共字段
CREATE TABLE `item` (
  `typeid`  VARCHAR(256) NOT NULL,
  `id`      BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`    VARCHAR(64) NOT NULL,
  `quality` INT NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB;

-- 子类表：只有自己的字段 + 关联 ID
CREATE TABLE `weapon` (
  `id`           BIGINT UNSIGNED NOT NULL,
  `attack`       INT NULL,
  `attack_speed` FLOAT NULL,
  `crit_rate`    FLOAT NULL,
  PRIMARY KEY (`id`),
  CONSTRAINT `weapon_id_fk`
    FOREIGN KEY (`id`) REFERENCES `item` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE `armor` (
  `id`         BIGINT UNSIGNED NOT NULL,
  `defense`    INT NULL,
  `durability` INT NULL,
  `resistance` INT NULL,
  PRIMARY KEY (`id`),
  CONSTRAINT `armor_id_fk`
    FOREIGN KEY (`id`) REFERENCES `item` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE `consumable` (
  `id`          BIGINT UNSIGNED NOT NULL,
  `heal_amount` INT NULL,
  `cooldown`    FLOAT NULL,
  `duration`    FLOAT NULL,
  PRIMARY KEY (`id`),
  CONSTRAINT `consumable_id_fk`
    FOREIGN KEY (`id`) REFERENCES `item` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB;
```

**类表继承的特征**：

| 优势                     | 劣势                          |
| ------------------------ | ----------------------------- |
| 无 NULL 浪费，表结构清晰 | 加载子类需要 JOIN（2 表）     |
| 子类字段可以加 NOT NULL  | 插入/更新需操作 2 张表        |
| 方便后续加子类（只加表） | 多态查询需要 UNION 或多次查询 |

### 1.4 策略三：具体表继承（Table Per Concrete Class）

**每个具体（非抽象）类独立一张完整表**，包含所有继承的字段：

```cpp
// 具体表继承：基类标记为 abstract
#pragma db object abstract
class Item
{
public:
    // ...
protected:
    // 注意：abstract 类没有自己的表，id 定义在这里但存储在子类表
    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(64)") not_null
    std::string name_;

    #pragma db not_null
    ItemQuality quality_{ItemQuality::White};
};

#pragma db object table("weapon")
class Weapon : public Item
{
    // Weapon 表包含 id + name + quality + attack + ...
    // 完整独立，不依赖其他表
private:
    friend class odb::access;

    int attack_{0};
    float attackSpeed_{1.0f};
    float critRate_{0.0f};
};

#pragma db object table("armor")
class Armor : public Item
{
private:
    friend class odb::access;

    int defense_{0};
    int durability_{100};
    int resistance_{0};
};

#pragma db object table("consumable")
class Consumable : public Item
{
private:
    friend class odb::access;

    int healAmount_{0};
    float cooldown_{0.0f};
    float duration_{0.0f};
};
```

**生成的 SQL**：

```sql
-- 每张表都是完整的，包含基类字段
CREATE TABLE `weapon` (
  `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`         VARCHAR(64) NOT NULL,
  `quality`      INT NOT NULL,
  `attack`       INT NULL,
  `attack_speed` FLOAT NULL,
  `crit_rate`    FLOAT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB;

CREATE TABLE `armor` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`       VARCHAR(64) NOT NULL,
  `quality`    INT NOT NULL,
  `defense`    INT NULL,
  `durability` INT NULL,
  `resistance` INT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB;

-- consumable 表同理...
```

**具体表继承的特征**：

| 优势               | 劣势                         |
| ------------------ | ---------------------------- |
| 单表查询，性能最好 | 基类字段重复存储             |
| 每张表完全独立     | **无法通过基类进行多态查询** |
| 不需要 JOIN        | 基类改字段，所有子类表都要改 |

### 1.5 三策略对比总结

| 维度        | 单表继承           | 类表继承（默认）     | 具体表继承           |
| ----------- | ------------------ | -------------------- | -------------------- |
| 表数量      | 1 张               | N+1 张               | N 张                 |
| NULL 浪费   | 严重               | 无                   | 无                   |
| 加载性能    | 最快（单表）       | 中等（JOIN）         | 最快（单表）         |
| 多态查询    | ✅ 天然支持         | ✅ 支持（JOIN/UNION） | ❌ 不支持             |
| Schema 演进 | 加字段影响所有     | 只改对应表           | 基类改动波及全部     |
| 适用场景    | 子类少、字段差异小 | **通用选择**         | 子类独立、无多态需求 |

> **游戏服务器建议**：
> - 道具系统（需要"查所有道具"的多态查询）→ **类表继承**或**单表继承**
> - 日志系统（战斗日志、交易日志、登录日志完全独立查询）→ **具体表继承**
> - 不确定时选**类表继承**（ODB 默认），它的灵活性最好

---

## 2. 多态加载

类表继承和单表继承支持**多态加载**——用基类指针加载，ODB 自动返回正确的子类：

```cpp
void demonstratePolymorphicLoad(odb::database& db)
{
    odb::transaction t(db.begin());

    // 创建不同类型的道具
    Weapon sword("霜之哀伤", ItemQuality::Orange, 350, 1.2f);
    Armor plate("圣光之铠", ItemQuality::Purple, 200, 500);
    Consumable potion("生命药水", ItemQuality::Green, 500, 30.0f);

    uint64_t swordId = db.persist(sword);
    uint64_t plateId = db.persist(plate);
    uint64_t potionId = db.persist(potion);

    // 多态加载：用 Item 基类加载，ODB 根据 typeid 列返回正确子类
    std::unique_ptr<Item> item1(db.load<Item>(swordId));
    std::unique_ptr<Item> item2(db.load<Item>(plateId));
    std::unique_ptr<Item> item3(db.load<Item>(potionId));

    // item1 的实际类型是 Weapon
    if (auto* w = dynamic_cast<Weapon*>(item1.get()))
    {
        std::cout << w->name() << " 攻击力: " << w->attack() << std::endl;
    }
    // item2 的实际类型是 Armor
    if (auto* a = dynamic_cast<Armor*>(item2.get()))
    {
        std::cout << a->name() << " 防御力: " << a->defense() << std::endl;
    }

    // 多态查询：查询所有传说品质的道具（不管什么子类）
    typedef odb::query<Item> Q;
    auto r = db.query<Item>(Q::quality == static_cast<int>(ItemQuality::Orange));

    for (auto& item : r)
    {
        std::cout << "[" << typeid(item).name() << "] "
                  << item.name() << std::endl;
    }
    // 输出: [Weapon] 霜之哀伤
    // ODB 内部执行了 JOIN，自动填充子类字段

    t.commit();
}
```

**类表继承多态加载的 SQL**：

```sql
-- load<Item>(swordId) 时：
-- 1. 先查基类表获取 typeid
SELECT `typeid`, `id`, `name`, `quality` FROM `item` WHERE `id` = ?
-- 结果: typeid = 'Weapon'

-- 2. 根据 typeid 查对应子类表
SELECT `id`, `attack`, `attack_speed`, `crit_rate` FROM `weapon` WHERE `id` = ?

-- 两次查询组合出完整的 Weapon 对象
```

---

## 3. abstract 基类：复用字段但不建表

有时你需要一个基类来复用公共字段，但这个基类本身不是独立的实体：

```cpp
// 所有游戏实体的公共字段
#pragma db object abstract
class BaseEntity
{
public:
    uint64_t id() const { return id_; }
    const std::string& createdAt() const { return createdAt_; }
    const std::string& updatedAt() const { return updatedAt_; }

protected:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("DATETIME") column("created_at") not_null
    std::string createdAt_;

    #pragma db type("DATETIME") column("updated_at") not_null
    std::string updatedAt_;
};

// Player 继承 BaseEntity，获得 id/createdAt/updatedAt
#pragma db object table("player")
class Player : public BaseEntity
{
    // Player 表包含 id, created_at, updated_at, name, level ...
    // BaseEntity 不建表
private:
    friend class odb::access;

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    int level_{1};
    // ...
};

// Guild 也继承 BaseEntity
#pragma db object table("guild")
class Guild : public BaseEntity
{
private:
    friend class odb::access;

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    uint64_t leaderId_{0};
    // ...
};
```

**abstract vs polymorphic**：

| 特性     | `#pragma db abstract` | `#pragma db polymorphic` |
| -------- | --------------------- | ------------------------ |
| 基类建表 | ❌ 不建                | ✅ 建表                   |
| 多态加载 | ❌ 不支持 `load<Base>` | ✅ 支持                   |
| 用途     | 纯字段复用（DRY）     | 真正的继承关系映射       |

---

## 4. View（视图）基础

### 4.1 什么是 View？

View 是 ODB 中的**只读投影**——它不对应持久化对象，而是一次查询的结果结构。可以理解为"定制化的 SELECT 结果"。

为什么需要 View？

```cpp
// ❌ 为了统计在线人数，加载所有 Player 对象？
auto all = db.query<Player>(Q::online == true);
int count = 0;
for (auto& p : all) ++count;
// 加载了完整的 Player 对象（name, level, gold, ...），只为了数个数
// 浪费内存，浪费网络带宽

// ✅ 用 View 只取需要的数据
// SELECT COUNT(*) FROM player WHERE online = 1
// 一个数字，零对象实例化
```

### 4.2 单对象 View：选择性投影

```cpp
// 排行榜条目：只需要 name, level, exp（不需要 gold, createdAt 等）
#pragma db view object(Player)
struct LeaderboardEntry
{
    // 从 Player 投影这三个字段
    #pragma db column(Player::name_)
    std::string name;

    #pragma db column(Player::level_)
    int level;

    #pragma db column(Player::exp_)
    uint64_t exp;
};
```

```cpp
// 使用 View 查询排行榜
void showLeaderboard(odb::database& db)
{
    odb::transaction t(db.begin());

    typedef odb::query<LeaderboardEntry> Q;

    auto r = db.query<LeaderboardEntry>(
        "ORDER BY" + Q::level + "DESC," + Q::exp + "DESC"
        + "LIMIT 10"
    );

    int rank = 1;
    for (const LeaderboardEntry& e : r)
    {
        std::cout << "#" << rank++ << " "
                  << e.name << " Lv." << e.level
                  << " (Exp:" << e.exp << ")" << std::endl;
    }

    t.commit();
}
// 生成的 SQL:
// SELECT `player`.`name`, `player`.`level`, `player`.`exp`
// FROM `player`
// ORDER BY `player`.`level` DESC, `player`.`exp` DESC LIMIT 10
//
// 对比 db.query<Player>()：
// SELECT `id`,`name`,`level`,`exp`,`gold`,`created_at` FROM `player` ...
// View 只传输 3 个字段 vs 完整对象 6+ 个字段
```

### 4.3 聚合 View：COUNT / SUM / AVG

```cpp
// 统计 View：全服玩家概览
#pragma db view object(Player)
struct PlayerStats
{
    // COUNT(*)：总玩家数
    #pragma db column("count(" + Player::id_ + ")")
    uint64_t totalPlayers;

    // AVG：平均等级
    #pragma db column("avg(" + Player::level_ + ")")
    double avgLevel;

    // MAX：最高等级
    #pragma db column("max(" + Player::level_ + ")")
    int maxLevel;

    // SUM：全服金币总量（经济监控）
    #pragma db column("sum(" + Player::gold_ + ")")
    uint64_t totalGold;
};
```

```cpp
void showServerStats(odb::database& db)
{
    odb::transaction t(db.begin());

    // query_value：聚合结果只有一行
    PlayerStats stats = db.query_value<PlayerStats>();

    std::cout << "=== 全服统计 ===" << std::endl;
    std::cout << "总玩家: " << stats.totalPlayers << std::endl;
    std::cout << "平均等级: " << stats.avgLevel << std::endl;
    std::cout << "最高等级: " << stats.maxLevel << std::endl;
    std::cout << "金币总量: " << stats.totalGold << std::endl;

    t.commit();
}
// 生成的 SQL:
// SELECT count(`player`.`id`), avg(`player`.`level`),
//        max(`player`.`level`), sum(`player`.`gold`)
// FROM `player`
// → 一行结果，零对象实例化
```

### 4.4 带条件的聚合

```cpp
// 统计各等级段的玩家分布
#pragma db view object(Player)
struct LevelDistribution
{
    #pragma db column(Player::level_)
    int level;

    #pragma db column("count(" + Player::id_ + ")")
    uint64_t playerCount;
};

void showLevelDistribution(odb::database& db)
{
    odb::transaction t(db.begin());

    typedef odb::query<LevelDistribution> Q;

    // GROUP BY level，只显示有玩家的等级
    auto r = db.query<LevelDistribution>(
        "GROUP BY" + Q::level
        + "HAVING" + Q::playerCount + "> 0"
        + "ORDER BY" + Q::level + "DESC"
    );

    for (const LevelDistribution& d : r)
    {
        std::cout << "Lv." << d.level << ": "
                  << d.playerCount << " 人" << std::endl;
    }

    t.commit();
}
// 生成的 SQL:
// SELECT `player`.`level`, count(`player`.`id`)
// FROM `player`
// GROUP BY `player`.`level` HAVING count(`player`.`id`) > 0
// ORDER BY `player`.`level` DESC
```

---

## 5. View 高级用法

### 5.1 多对象 JOIN View

View 可以关联多个对象，实现跨表查询：

```cpp
// 公会成员详情 View：JOIN guild + player
#pragma db view object(Guild) object(Player: Guild::members_)
struct GuildMemberDetail
{
    #pragma db column(Guild::name_)
    std::string guildName;

    #pragma db column(Player::name_)
    std::string playerName;

    #pragma db column(Player::level_)
    int playerLevel;
};
```

```cpp
// 查看某公会的成员详情
void showGuildDetail(odb::database& db, const std::string& guildName)
{
    odb::transaction t(db.begin());

    typedef odb::query<GuildMemberDetail> Q;

    auto r = db.query<GuildMemberDetail>(
        Q::Guild::name == guildName
        + "ORDER BY" + Q::Player::level + "DESC"
    );

    std::cout << "=== 公会: " << guildName << " ===" << std::endl;
    for (const GuildMemberDetail& m : r)
    {
        std::cout << "  " << m.playerName
                  << " Lv." << m.playerLevel << std::endl;
    }

    t.commit();
}
// 生成的 SQL:
// SELECT `guild`.`name`, `player`.`name`, `player`.`level`
// FROM `guild`
// INNER JOIN `guild_members` ON `guild_members`.`object_id` = `guild`.`id`
// INNER JOIN `player` ON `guild_members`.`value` = `player`.`id`
// WHERE `guild`.`name` = ?
// ORDER BY `player`.`level` DESC
```

### 5.2 公会战力排名聚合

```cpp
// 公会战力 = 所有成员等级之和（简化模型）
#pragma db view object(Guild) object(Player: Guild::members_)
struct GuildPowerRank
{
    #pragma db column(Guild::id_)
    uint64_t guildId;

    #pragma db column(Guild::name_)
    std::string guildName;

    // 成员数量
    #pragma db column("count(" + Player::id_ + ")")
    int memberCount;

    // 战力 = 等级总和
    #pragma db column("sum(" + Player::level_ + ")")
    int totalPower;

    // 平均等级
    #pragma db column("avg(" + Player::level_ + ")")
    double avgLevel;
};
```

```cpp
void showGuildPowerRanking(odb::database& db)
{
    odb::transaction t(db.begin());

    typedef odb::query<GuildPowerRank> Q;

    auto r = db.query<GuildPowerRank>(
        "GROUP BY" + Q::guildId + "," + Q::guildName
        + "ORDER BY" + Q::totalPower + "DESC"
        + "LIMIT 10"
    );

    int rank = 1;
    for (const GuildPowerRank& g : r)
    {
        std::cout << "#" << rank++ << " "
                  << g.guildName
                  << " | 成员:" << g.memberCount
                  << " | 战力:" << g.totalPower
                  << " | 平均Lv:" << g.avgLevel << std::endl;
    }

    t.commit();
}
// 生成的 SQL:
// SELECT `guild`.`id`, `guild`.`name`,
//        count(`player`.`id`), sum(`player`.`level`), avg(`player`.`level`)
// FROM `guild`
// INNER JOIN `guild_members` ON ...
// INNER JOIN `player` ON ...
// GROUP BY `guild`.`id`, `guild`.`name`
// ORDER BY sum(`player`.`level`) DESC LIMIT 10
```

### 5.3 原生 SQL View

当 ODB 的 View 语法不够用时，可以直接写 SQL：

```cpp
// 原生 SQL View：最近 7 天的每日交易统计
#pragma db view query(                                                 \
    "SELECT DATE(created_at) AS trade_date, "                          \
    "       COUNT(*) AS trade_count, "                                 \
    "       SUM(amount) AS total_amount "                              \
    "FROM trade_log "                                                  \
    "WHERE created_at >= DATE_SUB(NOW(), INTERVAL 7 DAY) "            \
    "GROUP BY DATE(created_at) "                                       \
    "ORDER BY trade_date DESC")
struct DailyTradeReport
{
    #pragma db column("trade_date") type("DATE")
    std::string tradeDate;

    #pragma db column("trade_count")
    uint64_t tradeCount;

    #pragma db column("total_amount")
    uint64_t totalAmount;
};
```

```cpp
void showWeeklyTradeReport(odb::database& db)
{
    odb::transaction t(db.begin());

    auto r = db.query<DailyTradeReport>();

    std::cout << "=== 近 7 天交易统计 ===" << std::endl;
    for (const DailyTradeReport& d : r)
    {
        std::cout << d.tradeDate << " | "
                  << d.tradeCount << " 笔 | "
                  << "总额: " << d.totalAmount << " 金币" << std::endl;
    }

    t.commit();
}
```

---

## 设计要点

### 三种继承策略的取舍

选择策略时要考虑**实际的查询模式**：

```
问自己：需要"查所有道具"这种跨子类的多态查询吗？

  是 → 单表或类表继承
       └─ 子类字段差异大吗？
           是 → 类表继承（避免 NULL 泛滥）
           否 → 单表继承（一张表搞定）

  否 → 具体表继承（每个子类独立，性能最好）
```

**游戏实例**：
- 背包里可能同时有武器、防具、消耗品，玩家打开背包要看所有道具 → **类表或单表**
- 战斗日志、交易日志、登录日志各有专属查询页面，从不混查 → **具体表**

### View 为什么是只读的？

View 对应的 SQL 可能是聚合查询（COUNT/SUM）或多表 JOIN。这些结果不是具体的一行数据，无法直接"更新回去"：

```sql
-- 这个聚合结果你怎么 UPDATE？
SELECT guild_name, avg(level) FROM ... GROUP BY guild_name
-- "更新平均等级"没有意义
```

把 View 设计为只读，职责清晰：
- **object** = 可读可写的持久化实体
- **view** = 只读的查询投影

这避免了"部分更新"的歧义——如果 View 只投影了 3 个字段，update 时其他字段怎么办？设为 NULL？保留原值？没有合理的默认行为。

### ODB View vs 数据库 VIEW

| 对比        | ODB View              | MySQL CREATE VIEW  |
| ----------- | --------------------- | ------------------ |
| 定义位置    | C++ 代码中（#pragma） | 数据库中（DDL）    |
| 类型安全    | ✅ 编译期检查          | ❌ 运行时字符串     |
| 可移植性    | ✅ 跨数据库            | ❌ 绑定特定数据库   |
| 灵活性      | 可带 C++ 参数         | 静态定义           |
| Schema 管理 | 不占数据库资源        | 需要维护 VIEW 对象 |

ODB View 是**应用层概念**，编译后变成普通 SELECT 语句。它比数据库 VIEW 更灵活——可以通过 C++ 变量动态参数化，不占用数据库 Schema 空间。

---

## 小结

本篇我们掌握了两个高级特性：

**继承映射**：
| 策略         | pragma 关键字            | 适用场景             |
| ------------ | ------------------------ | -------------------- |
| 单表         | `polymorphic` + 共享表名 | 子类少、字段差异小   |
| 类表（默认） | `polymorphic`            | 通用选择             |
| 具体表       | `abstract` 基类          | 子类独立、无多态需求 |

**View**：
| 用法        | 场景                      |
| ----------- | ------------------------- |
| 单对象投影  | 排行榜（只取 name/level） |
| 聚合查询    | 全服统计（COUNT/SUM/AVG） |
| 多对象 JOIN | 公会成员详情              |
| 原生 SQL    | 复杂报表                  |

**下一篇**我们将学习 Schema 迁移与多数据库支持——游戏更新时给 Player 加新字段怎么不停服？开发用 SQLite、生产用 MySQL 怎么切换？ODB 的 changelog 机制让 Schema 演进可追溯、可回滚。

---

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | **继承与视图**（本文） | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})
