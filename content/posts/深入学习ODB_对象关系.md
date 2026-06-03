+++
title = '深入学习 ODB（三）：对象关系——一对一、一对多与多对多'
date = '2025-06-07'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "关系映射", "外键", "智能指针"]
categories = ["ODB学习"]
description = "全面讲解 ODB 中对象关系的建模：以玩家档案（一对一）、背包系统（一对多）、公会成员（多对多）为例，理解 lazy_ptr、inverse、容器映射的设计原理。"
+++

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | **对象关系**（本文） | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})

---

## 引言：从单表到关系网络

前两篇我们学会了单个 Player 对象的持久化。但真实的游戏系统远不止一张表：

```
Player ──── 1:1 ────▶ PlayerProfile  （玩家档案：头像、签名、战斗统计）
   │
   ├─── 1:N ────▶ InventoryItem[]   （背包：拥有多个道具）
   │
   ├─── 1:N ────▶ Equipment[]       （装备栏：穿戴的装备）
   │
   └─── M:N ────▶ Guild[]           （公会：多个玩家加入多个公会）
```

在关系数据库中，这些关系通过**外键**表达。而在 C++ 中，我们习惯用**指针和容器**表达。ODB 的工作就是在这两种世界观之间架起桥梁。

本篇将逐步讲解 ODB 如何用智能指针和 STL 容器映射所有类型的对象关系。

---

## 1. 关系映射基础概念

### 1.1 数据库外键 vs C++ 指针

| 数据库世界                   | C++ 世界                     | ODB 桥接方式              |
| ---------------------------- | ---------------------------- | ------------------------- |
| 外键列（`player_id BIGINT`） | `std::shared_ptr<Player>`    | ODB 自动处理 load/persist |
| JOIN 查询                    | 解引用指针 `ptr->name()`     | 延迟或急切加载            |
| 联接表（junction table）     | `std::vector<shared_ptr<T>>` | ODB 自动维护联接表        |

### 1.2 ODB 的指针类型选择

ODB 支持多种指针类型来表达对象引用：

| 指针类型                  | 加载时机             | 内存管理   | 适用场景               |
| ------------------------- | -------------------- | ---------- | ---------------------- |
| `std::shared_ptr<T>`      | **急加载**（eager）  | 共享所有权 | 总是需要关联数据       |
| `odb::lazy_shared_ptr<T>` | **延迟加载**（lazy） | 共享所有权 | 按需加载，避免无用查询 |
| `odb::lazy_weak_ptr<T>`   | 延迟加载             | 弱引用     | 避免循环引用           |
| `std::unique_ptr<T>`      | 急加载               | 独占所有权 | 一对一强拥有关系       |

**急加载 vs 延迟加载**：

```cpp
// 急加载：load Player 时，同时发 SQL 加载 Profile
std::shared_ptr<PlayerProfile> profile_;
// → 一条 SELECT player ... 紧跟一条 SELECT player_profile ...

// 延迟加载：load Player 时，只存 profile 的 ID
odb::lazy_shared_ptr<PlayerProfile> profile_;
// → 只执行 SELECT player ...
// → 直到你调用 profile_.load() 时才发第二条 SQL
```

> **游戏服务器建议**：大多数场景用 `lazy_shared_ptr`。玩家上线时不需要立即加载所有关联数据——战斗统计、成就列表等可以等玩家打开对应面板时再加载。

---

## 2. 一对一关系：Player <-> PlayerProfile

### 2.1 场景分析

游戏中，玩家基础数据（等级、金币）需要频繁读写，而扩展档案（签名、头像URL、历史战绩）访问较少。将它们拆分为两张表：

- `player` 表：核心数据，每次登录必加载
- `player_profile` 表：扩展档案，打开"个人资料"面板时才加载

### 2.2 单向一对一

最简单的形式——Player 持有 Profile 的引用，Profile 不知道属于谁：

```cpp
// player_profile.hxx
#ifndef PLAYER_PROFILE_HXX
#define PLAYER_PROFILE_HXX

#include <string>
#include <cstdint>
#include <odb/core.hxx>

#pragma db object table("player_profile")
class PlayerProfile
{
public:
    PlayerProfile() = default;

    PlayerProfile(const std::string& avatar, const std::string& signature)
        : avatar_(avatar), signature_(signature) {}

    uint64_t id() const { return id_; }
    const std::string& avatar() const { return avatar_; }
    const std::string& signature() const { return signature_; }
    int totalKills() const { return totalKills_; }
    int totalDeaths() const { return totalDeaths_; }

    void setSignature(const std::string& s) { signature_ = s; }
    void addKill() { ++totalKills_; }
    void addDeath() { ++totalDeaths_; }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(256)")
    std::string avatar_;  // 头像 URL

    #pragma db type("VARCHAR(128)")
    std::string signature_;  // 个性签名

    #pragma db column("total_kills")
    int totalKills_{0};

    #pragma db column("total_deaths")
    int totalDeaths_{0};
};

#endif
```

```cpp
// player.hxx（增加 profile 关联）
#include <memory>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>
#include "player_profile.hxx"

#pragma db object table("player")
class Player
{
public:
    Player() = default;
    Player(const std::string& name, int level)
        : name_(name), level_(level) {}

    // 获取 Profile（延迟加载版本）
    odb::lazy_shared_ptr<PlayerProfile>& profile() { return profile_; }

    // ... 其他 getter/setter 省略 ...

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    int level_{1};
    uint64_t exp_{0};
    uint64_t gold_{0};

    // 一对一关联：延迟加载
    // 数据库中表现为 player 表的 profile_id 外键列
    #pragma db not_null
    odb::lazy_shared_ptr<PlayerProfile> profile_;
};
```

**生成的 SQL**：

```sql
CREATE TABLE `player` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`       VARCHAR(32) NOT NULL,
  `level`      INT NULL,
  `exp`        BIGINT UNSIGNED NULL,
  `gold`       BIGINT UNSIGNED NULL,
  `profile`    BIGINT UNSIGNED NOT NULL,  -- 外键指向 player_profile.id
  PRIMARY KEY (`id`),
  UNIQUE INDEX `player_name_i` (`name`),
  CONSTRAINT `player_profile_fk`
    FOREIGN KEY (`profile`) REFERENCES `player_profile` (`id`)
) ENGINE=InnoDB;
```

### 2.3 使用一对一关系

```cpp
void createPlayerWithProfile(odb::database& db)
{
    odb::transaction t(db.begin());

    // 1. 先创建 Profile（因为 Player 引用它，必须先存在）
    auto profile = std::make_shared<PlayerProfile>(
        "https://cdn.game.com/avatar/default.png",
        "这个人很懒，什么都没留下"
    );
    db.persist(*profile);  // profile 获得自增 ID

    // 2. 创建 Player，关联 Profile
    Player player("剑圣无名", 1);
    player.profile() = profile;  // 建立关联
    db.persist(player);

    t.commit();
}

void loadPlayerProfile(odb::database& db, uint64_t playerId)
{
    odb::transaction t(db.begin());

    auto player = db.load<Player>(playerId);
    // 此时 profile_ 只有 ID，没有完整数据

    // 调用 load() 触发延迟加载（发第二条 SQL）
    auto profile = player->profile().load();
    std::cout << "签名: " << profile->signature() << std::endl;
    std::cout << "K/D: " << profile->totalKills()
              << "/" << profile->totalDeaths() << std::endl;

    t.commit();
}
```

### 2.4 双向一对一（inverse）

如果 Profile 也需要反向引用 Player（比如通过 Profile 查到所属玩家）：

```cpp
// player_profile.hxx（双向版本）
#pragma db object table("player_profile")
class PlayerProfile
{
public:
    // ...

    odb::lazy_weak_ptr<Player>& owner() { return owner_; }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    // inverse 标记：这一侧不创建外键列
    // 由 Player::profile_ 那边维护外键
    #pragma db inverse(profile_)
    odb::lazy_weak_ptr<Player> owner_;

    // ... 其他字段 ...
};
```

**关键点**：`#pragma db inverse(profile_)` 告诉 ODB：
- "这个关系的外键在 Player 那边（`Player::profile_`）"
- "不要在 player_profile 表创建额外的外键列"
- "加载时通过反向查询填充"

```
┌─────────────────┐        ┌──────────────────────┐
│     player      │        │   player_profile     │
├─────────────────┤        ├──────────────────────┤
│ id (PK)         │        │ id (PK)              │
│ name            │        │ avatar               │
│ profile (FK) ───┼───────▶│ signature            │
│ ...             │        │ total_kills          │
└─────────────────┘        └──────────────────────┘
                     外键只在一侧
         Player.profile_ 是"拥有方"
         PlayerProfile.owner_ 是"inverse 方"（不产生列）
```

---

## 3. 一对多关系：Player -> InventoryItem

### 3.1 场景分析

玩家的背包系统是最典型的一对多关系：一个玩家拥有多个道具。

### 3.2 使用指针容器（推荐方式）

```cpp
// inventory_item.hxx
#ifndef INVENTORY_ITEM_HXX
#define INVENTORY_ITEM_HXX

#include <string>
#include <cstdint>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>

// 前向声明，避免循环 #include
class Player;

#pragma db object table("inventory")
class InventoryItem
{
public:
    InventoryItem() = default;

    InventoryItem(uint64_t templateId, int count, int slot)
        : templateId_(templateId), count_(count), slot_(slot) {}

    uint64_t id() const { return id_; }
    uint64_t templateId() const { return templateId_; }
    int count() const { return count_; }
    int slot() const { return slot_; }

    void setCount(int c) { count_ = c; }
    void addCount(int n) { count_ += n; }

    // 获取所属玩家
    odb::lazy_shared_ptr<Player>& owner() { return owner_; }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    // 多方持有外键，指向 Player
    #pragma db not_null
    odb::lazy_shared_ptr<Player> owner_;

    #pragma db column("template_id") not_null
    uint64_t templateId_{0};  // 道具模板 ID

    #pragma db not_null
    int count_{1};  // 堆叠数量

    #pragma db not_null
    int slot_{0};  // 背包格子位置
};

#endif
```

```cpp
// player.hxx（增加背包关联）
#include <vector>
#include <memory>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>

class InventoryItem;  // 前向声明

#pragma db object table("player")
class Player
{
public:
    Player() = default;
    Player(const std::string& name, int level)
        : name_(name), level_(level) {}

    // 获取背包道具列表
    const std::vector<odb::lazy_shared_ptr<InventoryItem>>& inventory() const
    {
        return inventory_;
    }

    // ... 其他方法 ...

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    int level_{1};
    uint64_t exp_{0};
    uint64_t gold_{0};

    // 一对多：Player 拥有多个 InventoryItem
    // inverse(owner_) 表示由 InventoryItem::owner_ 维护外键
    #pragma db inverse(owner_) value_not_null
    std::vector<odb::lazy_shared_ptr<InventoryItem>> inventory_;
};
```

**生成的 SQL**：

```sql
-- inventory 表持有外键
CREATE TABLE `inventory` (
  `id`          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `owner`       BIGINT UNSIGNED NOT NULL,  -- 外键 → player.id
  `template_id` BIGINT UNSIGNED NOT NULL,
  `count`       INT NOT NULL,
  `slot`        INT NOT NULL,
  PRIMARY KEY (`id`),
  INDEX `inventory_owner_i` (`owner`),
  CONSTRAINT `inventory_owner_fk`
    FOREIGN KEY (`owner`) REFERENCES `player` (`id`)
) ENGINE=InnoDB;
```

### 3.3 使用一对多关系

```cpp
// 给玩家添加道具
void addItemToInventory(odb::database& db, uint64_t playerId,
                        uint64_t templateId, int count)
{
    odb::session s;  // 开启 session，防止重复加载
    odb::transaction t(db.begin());

    // 加载玩家
    auto player = db.load<Player>(playerId);

    // 创建道具对象
    InventoryItem item(templateId, count, findEmptySlot(player));
    item.owner() = player;  // 设置外键关联

    // 持久化道具（自动在 inventory 表插入一行）
    db.persist(item);

    t.commit();
}

// 遍历玩家背包
void listInventory(odb::database& db, uint64_t playerId)
{
    odb::session s;
    odb::transaction t(db.begin());

    auto player = db.load<Player>(playerId);

    // inventory() 是延迟加载的容器
    // 首次访问时触发: SELECT * FROM inventory WHERE owner=?
    for (auto& itemPtr : player->inventory())
    {
        auto item = itemPtr.load();  // 加载每个道具
        std::cout << "格子[" << item->slot() << "] "
                  << "道具#" << item->templateId()
                  << " x" << item->count() << std::endl;
    }

    t.commit();
}

// 删除道具（使用后消耗）
void consumeItem(odb::database& db, uint64_t itemId, int consumeCount)
{
    odb::transaction t(db.begin());

    auto item = db.load<InventoryItem>(itemId);

    if (item->count() <= consumeCount)
    {
        // 全部消耗，删除整行
        db.erase(*item);
    }
    else
    {
        // 部分消耗，减少数量
        item->addCount(-consumeCount);
        db.update(*item);
    }

    t.commit();
}
```

### 3.4 一对多的"拥有方"与"被拥有方"

```
┌─────────────────────────────────────────────────────────┐
│                                                         │
│   Player（一方）              InventoryItem（多方）       │
│   ┌──────────────┐           ┌──────────────────┐      │
│   │ id           │           │ id               │      │
│   │ name         │           │ owner (FK) ──────┼──┐   │
│   │ inventory_   │◀── inverse ──│ template_id    │  │   │
│   │  (vector)    │           │ count            │  │   │
│   └──────────────┘           └──────────────────┘  │   │
│          ▲                                         │   │
│          └─────────────────────────────────────────┘   │
│                                                         │
│   规则：外键永远在"多方"（InventoryItem.owner）          │
│         "一方"用 inverse 标记，不产生额外列              │
└─────────────────────────────────────────────────────────┘
```

---

## 4. 多对多关系：Player <-> Guild

### 4.1 场景分析

游戏中玩家和公会是多对多关系：
- 一个玩家可以加入多个公会（主公会 + 副本联盟等）
- 一个公会包含多名玩家

多对多在数据库中需要**联接表**（junction table）来实现。

### 4.2 定义多对多关系

```cpp
// guild.hxx
#ifndef GUILD_HXX
#define GUILD_HXX

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>

class Player;  // 前向声明

#pragma db object table("guild")
class Guild
{
public:
    Guild() = default;

    Guild(const std::string& name, uint64_t leaderId)
        : name_(name), leaderId_(leaderId) {}

    uint64_t id() const { return id_; }
    const std::string& name() const { return name_; }
    uint64_t leaderId() const { return leaderId_; }
    int memberCount() const { return static_cast<int>(members_.size()); }

    // 获取成员列表
    std::vector<odb::lazy_shared_ptr<Player>>& members()
    {
        return members_;
    }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    #pragma db column("leader_id") not_null
    uint64_t leaderId_{0};

    #pragma db type("INT") not_null
    int maxMembers_{50};  // 最大成员数

    // 多对多关系：公会包含多个玩家
    // ODB 自动创建联接表 guild_members
    #pragma db value_not_null unordered
    std::vector<odb::lazy_shared_ptr<Player>> members_;
};

#endif
```

```cpp
// player.hxx（增加公会关联）
#pragma db object table("player")
class Player
{
public:
    // ...

    // 获取加入的公会列表
    std::vector<odb::lazy_shared_ptr<Guild>>& guilds()
    {
        return guilds_;
    }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    // ... 其他字段 ...

    // 多对多的反向引用
    // inverse(members_) 指向 Guild::members_，不创建额外联接表
    #pragma db inverse(members_) value_not_null
    std::vector<odb::lazy_shared_ptr<Guild>> guilds_;
};
```

### 4.3 生成的联接表

ODB 自动为 `Guild::members_` 生成联接表：

```sql
-- 联接表（由 ODB 自动生成和维护）
CREATE TABLE `guild_members` (
  `object_id`  BIGINT UNSIGNED NOT NULL,  -- 指向 guild.id
  `value`      BIGINT UNSIGNED NOT NULL,  -- 指向 player.id
  INDEX `guild_members_object_id_i` (`object_id`),
  INDEX `guild_members_value_i` (`value`),
  CONSTRAINT `guild_members_object_id_fk`
    FOREIGN KEY (`object_id`) REFERENCES `guild` (`id`) ON DELETE CASCADE,
  CONSTRAINT `guild_members_value_fk`
    FOREIGN KEY (`value`) REFERENCES `player` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB;
```

你也可以自定义联接表名：

```cpp
// 自定义联接表名
#pragma db value_not_null unordered table("guild_player_map")
std::vector<odb::lazy_shared_ptr<Player>> members_;
```

### 4.4 使用多对多关系

```cpp
// 创建公会
uint64_t createGuild(odb::database& db, uint64_t leaderId,
                     const std::string& guildName)
{
    odb::session s;
    odb::transaction t(db.begin());

    // 加载会长
    auto leader = db.load<Player>(leaderId);

    // 创建公会
    Guild guild(guildName, leaderId);
    guild.members().push_back(leader);  // 会长自动加入

    uint64_t guildId = db.persist(guild);
    // persist(guild) 同时会向联接表插入一行

    t.commit();
    return guildId;
}

// 加入公会
void joinGuild(odb::database& db, uint64_t playerId, uint64_t guildId)
{
    odb::session s;
    odb::transaction t(db.begin());

    auto guild = db.load<Guild>(guildId);
    auto player = db.load<Player>(playerId);

    // 检查人数上限
    if (guild->memberCount() >= guild->maxMembers())
        throw std::runtime_error("公会已满");

    // 添加成员
    guild->members().push_back(player);

    // update 公会对象，ODB 自动更新联接表
    db.update(*guild);

    t.commit();
}

// 退出公会
void leaveGuild(odb::database& db, uint64_t playerId, uint64_t guildId)
{
    odb::session s;
    odb::transaction t(db.begin());

    auto guild = db.load<Guild>(guildId);

    // 从成员列表中移除
    auto& members = guild->members();
    members.erase(
        std::remove_if(members.begin(), members.end(),
            [playerId](const odb::lazy_shared_ptr<Player>& p) {
                return p.object_id<Player>() == playerId;
                // object_id() 无需 load 即可获取 ID
            }),
        members.end()
    );

    db.update(*guild);  // 更新联接表（删除对应行）

    t.commit();
}

// 查看公会成员
void listGuildMembers(odb::database& db, uint64_t guildId)
{
    odb::session s;
    odb::transaction t(db.begin());

    auto guild = db.load<Guild>(guildId);

    std::cout << "=== " << guild->name() << " ===" << std::endl;
    for (auto& memberPtr : guild->members())
    {
        auto member = memberPtr.load();
        std::cout << "  " << member->name()
                  << " Lv." << member->level() << std::endl;
    }

    t.commit();
}
```

### 4.5 多对多关系图解

```
┌──────────┐      ┌─────────────────┐      ┌──────────┐
│  player  │      │  guild_members  │      │  guild   │
├──────────┤      ├─────────────────┤      ├──────────┤
│ id = 1   │◀─────│ value = 1       │      │ id = 10  │
│ 剑圣无名  │      │ object_id = 10 ─┼─────▶│ 逍遥派   │
├──────────┤      ├─────────────────┤      ├──────────┤
│ id = 2   │◀─────│ value = 2       │      │ id = 11  │
│ 花间游    │      │ object_id = 10 ─┼─────▶│ 逍遥派   │
│          │◀─────│ value = 2       │      │          │
│          │      │ object_id = 11 ─┼─────▶│ 烽火连天  │
└──────────┘      └─────────────────┘      └──────────┘

玩家2"花间游"同时属于"逍遥派"和"烽火连天"两个公会
```

---

## 5. 容器映射进阶

### 5.1 值类型容器

除了对象指针容器，ODB 还能映射**值类型容器**——容器里存的不是另一个对象的引用，而是简单值：

```cpp
#pragma db object table("player")
class Player
{
    // ...

    // 值类型容器：玩家拥有的成就 ID 列表
    // 映射为独立表 player_achievements (player_id, value)
    #pragma db table("player_achievements") value_column("achievement_id")
    std::vector<uint64_t> achievements_;

    // 值类型容器：玩家标签（如 "PVP高手", "氪金大佬"）
    #pragma db table("player_tags") value_type("VARCHAR(32)")
    std::set<std::string> tags_;

    // 有序容器会自动添加 index 列维护顺序
    // player_achievements 表结构：
    //   player_id BIGINT (FK)
    //   index     BIGINT (排序用)
    //   achievement_id BIGINT (值)
};
```

**生成的 SQL**：

```sql
CREATE TABLE `player_achievements` (
  `object_id`      BIGINT UNSIGNED NOT NULL,  -- 所属 player 的 ID
  `index`          BIGINT UNSIGNED NOT NULL,  -- vector 中的位置
  `achievement_id` BIGINT UNSIGNED NOT NULL,
  CONSTRAINT `player_achievements_object_id_fk`
    FOREIGN KEY (`object_id`) REFERENCES `player` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE `player_tags` (
  `object_id` BIGINT UNSIGNED NOT NULL,
  `value`     VARCHAR(32) NOT NULL,
  CONSTRAINT `player_tags_object_id_fk`
    FOREIGN KEY (`object_id`) REFERENCES `player` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB;
```

### 5.2 map 容器映射

`std::map` 映射为键值对表，适合游戏中的"属性-值"配置：

```cpp
#pragma db object table("player")
class Player
{
    // ...

    // map 容器：玩家自定义设置
    // 映射为 player_settings (player_id, key, value)
    #pragma db table("player_settings") \
             key_type("VARCHAR(32)") value_type("VARCHAR(256)")
    std::map<std::string, std::string> settings_;

    // map 容器：技能等级 (技能ID -> 等级)
    #pragma db table("player_skills") \
             key_column("skill_id") value_column("skill_level")
    std::map<uint64_t, int> skills_;
};
```

```sql
CREATE TABLE `player_skills` (
  `object_id`   BIGINT UNSIGNED NOT NULL,
  `skill_id`    BIGINT UNSIGNED NOT NULL,
  `skill_level` INT NULL,
  CONSTRAINT `player_skills_object_id_fk`
    FOREIGN KEY (`object_id`) REFERENCES `player` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB;
```

### 5.3 ordered vs unordered

```cpp
// ordered（默认）：维护插入顺序，额外存储 index 列
std::vector<odb::lazy_shared_ptr<InventoryItem>> inventory_;
// 表中有 index 列：0, 1, 2, 3 ...
// 适合：背包（格子顺序重要）

// unordered：不维护顺序，无 index 列
#pragma db unordered
std::vector<odb::lazy_shared_ptr<Player>> members_;
// 适合：公会成员列表（顺序无所谓）
```

| 选项            | 额外存储 | 插入/删除性能            | 适用场景             |
| --------------- | -------- | ------------------------ | -------------------- |
| ordered（默认） | index 列 | 中间插入需更新后续 index | 背包格子、技能快捷栏 |
| unordered       | 无       | O(1) 追加                | 公会成员、好友列表   |

### 5.4 容器更新策略

ODB 对容器的 `update()` 是**全量替换**——先清空容器表相关行，再重新插入：

```cpp
// update(guild) 时对联接表的操作：
// 1. DELETE FROM guild_members WHERE object_id = ?
// 2. INSERT INTO guild_members (object_id, value) VALUES (?, ?)
// 3. INSERT INTO guild_members (object_id, value) VALUES (?, ?)
// ... 逐条插入当前 members_ 中所有元素
```

> **性能提示**：如果容器很大（比如千人公会），每次 update 都全量替换会很慢。后面第七篇"性能优化"会讲如何避免这个问题。在成员变动不频繁的场景下，这个开销可以接受。

---

## 6. 综合示例：完整的关系网络

将所有关系组合在一起：

```cpp
// 完整的 Player 定义（包含所有关系）
#pragma db object table("player")
class Player
{
public:
    Player() = default;
    Player(const std::string& name, int level)
        : name_(name), level_(level) {}

    uint64_t id() const { return id_; }
    const std::string& name() const { return name_; }
    int level() const { return level_; }
    uint64_t gold() const { return gold_; }

    // 关系访问器
    odb::lazy_shared_ptr<PlayerProfile>& profile() { return profile_; }
    std::vector<odb::lazy_shared_ptr<InventoryItem>>& inventory()
    {
        return inventory_;
    }
    std::vector<odb::lazy_shared_ptr<Guild>>& guilds()
    {
        return guilds_;
    }

    // 业务方法
    void setLevel(int lv) { level_ = lv; }
    void addGold(uint64_t g) { gold_ += g; }
    void deductGold(uint64_t g) { gold_ -= g; }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    int level_{1};
    uint64_t exp_{0};
    uint64_t gold_{0};

    // ===== 关系映射 =====

    // 一对一：玩家档案
    #pragma db not_null
    odb::lazy_shared_ptr<PlayerProfile> profile_;

    // 一对多（反向）：背包道具
    #pragma db inverse(owner_) value_not_null
    std::vector<odb::lazy_shared_ptr<InventoryItem>> inventory_;

    // 多对多（反向）：加入的公会
    #pragma db inverse(members_) value_not_null
    std::vector<odb::lazy_shared_ptr<Guild>> guilds_;

    // 值类型容器：技能等级
    #pragma db table("player_skills") \
             key_column("skill_id") value_column("skill_level")
    std::map<uint64_t, int> skills_;
};
```

**玩家上线时的加载流程**：

```cpp
void onPlayerLogin(odb::database& db, uint64_t playerId)
{
    odb::session s;
    odb::transaction t(db.begin());

    // 加载核心数据（1 条 SQL）
    auto player = db.load<Player>(playerId);
    std::cout << player->name() << " 上线了！" << std::endl;

    // 延迟加载：此时 profile_、inventory_、guilds_ 都只有 ID
    // 没有额外 SQL 被执行

    // 客户端请求打开背包面板时，才加载道具
    // → SELECT * FROM inventory WHERE owner=?
    for (auto& itemPtr : player->inventory())
    {
        auto item = itemPtr.load();
        sendToClient(player->id(), buildItemPacket(item));
    }

    // 客户端请求查看个人资料时，才加载档案
    // → SELECT * FROM player_profile WHERE id=?
    auto profile = player->profile().load();
    sendToClient(player->id(), buildProfilePacket(profile));

    t.commit();
}
```

---

## 设计要点

### 为何用 lazy_shared_ptr 而非裸指针？

```cpp
// 假设用裸指针
Player* owner;  // ❌ 谁负责释放？加载时机？线程安全？

// ODB 的选择
odb::lazy_shared_ptr<Player> owner_;
```

`lazy_shared_ptr` 封装了三个关键能力：

1. **所有权语义** — `shared_ptr` 共享所有权，对象不会被意外释放
2. **延迟加载** — 持有目标 ID，首次 `.load()` 时才发 SQL 查询
3. **缓存集成** — 配合 `odb::session`，同一 ID 不会重复加载

这对游戏服务器至关重要：玩家对象可能被多个系统引用（战斗、社交、交易），`shared_ptr` 的引用计数保证任何一方持有时对象都不会被销毁。

### inverse 标记解决了什么问题？

考虑双向关系**没有** inverse 时会发生什么：

```cpp
// ❌ 如果两边都不标 inverse
class Player {
    std::vector<shared_ptr<InventoryItem>> inventory_;  // 联接表 A
};
class InventoryItem {
    shared_ptr<Player> owner_;  // 外键列 B
};
// 结果：数据库中有两份冗余信息！
//   1. inventory 表的 owner 外键列
//   2. 一张额外的联接表 player_inventory
// 插入道具时要写两处，删除时也要清两处 → 数据不一致风险
```

`inverse` 告诉 ODB：**这个关系的"真相来源"只有一处**。

```cpp
// ✅ inverse 标记后
class Player {
    #pragma db inverse(owner_)  // 我只是反向引用
    std::vector<shared_ptr<InventoryItem>> inventory_;
};
class InventoryItem {
    shared_ptr<Player> owner_;  // 真相在这里（外键列）
};
// 结果：数据库只有一个外键 inventory.owner
//   Player.inventory_ 通过反向查询填充（SELECT * FROM inventory WHERE owner=?）
```

### 容器为何需要额外表？

关系数据库遵循**第一范式（1NF）**：每个字段只能存储一个原子值。

```
❌ 违反 1NF（不可能在 MySQL 中实现）：
player 表
| id | name | inventory        |
| 1  | 剑圣 | [道具A, 道具B, 道具C] |  ← 一个字段存多值

✅ 符合 1NF：
player 表                    inventory 表
| id | name |                 | id | owner | item |
| 1  | 剑圣 |                 | 1  | 1     | 道具A |
                              | 2  | 1     | 道具B |
                              | 3  | 1     | 道具C |
```

ODB 自动将 `std::vector<T>` 映射为额外表，是关系数据库范式的必然要求。这不是 ODB 的设计选择，而是关系模型的基本约束。

---

## 小结

本篇我们掌握了 ODB 的三种对象关系：

| 关系类型 | 游戏示例           | 外键位置      | 关键 pragma                  |
| -------- | ------------------ | ------------- | ---------------------------- |
| 一对一   | Player ↔ Profile   | 一方持有 FK   | `inverse` 标记反向           |
| 一对多   | Player → Inventory | "多方"持有 FK | `inverse` + `value_not_null` |
| 多对多   | Player ↔ Guild     | 联接表        | `unordered` + `table()`      |

核心要记住的规则：
1. **外键永远在"多方"或联接表**，不在"一方"
2. **inverse 标记消除冗余**——关系的真相来源只有一处
3. **延迟加载是默认最佳选择**——按需加载，避免无用查询
4. **session 在遍历关系时必须开启**——防止同一对象被重复实例化

**下一篇**我们将学习 ODB 的查询系统——如何类型安全地查询排行榜 Top100、按品质筛选装备、模糊搜索玩家昵称？告别手写 SQL 的最后一块拼图。

---

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | **对象关系**（本文） | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})
