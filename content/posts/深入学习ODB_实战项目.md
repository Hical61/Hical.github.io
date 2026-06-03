+++
title = '深入学习 ODB（八）：实战——构建完整的游戏存档系统'
date = '2025-07-12'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "实战", "游戏服务器", "存档系统"]
categories = ["ODB学习"]
description = "综合运用 ODB 全部特性，从零构建一个游戏存档系统：玩家/背包/装备/公会/交易日志的完整数据建模、Repository 层封装、业务场景实现与 Schema 迁移全流程。"
+++

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | **实战项目**（本文）

---

## 引言：从零到完整

经过前七篇的学习，我们掌握了 ODB 的所有核心能力。本篇将综合运用它们，构建一个**完整可运行的游戏存档系统**——这不是简化的教学示例，而是接近生产级别的架构。

我们要实现以下游戏系统：

```
┌─────────────────────────────────────────────────────────┐
│                    游戏存档系统                            │
├─────────────┬──────────────┬────────────┬───────────────┤
│  玩家系统    │   背包系统    │  公会系统   │   交易系统     │
│  Player     │  Inventory   │  Guild     │  TradeLog    │
│  Profile    │  Equipment   │  Members   │  AuditLog    │
└─────────────┴──────────────┴────────────┴───────────────┘
```

---

## 1. 需求分析与数据建模

### 1.1 E-R 关系图

```
                    ┌──────────────┐
                    │ PlayerProfile│
                    │ (1:1)        │
                    └──────┬───────┘
                           │
┌──────────┐        ┌──────┴───────┐        ┌──────────────┐
│ Equipment│◀── 1:N ┤    Player    ├ M:N ──▶│    Guild     │
│ (装备栏)  │        │              │        │              │
└──────────┘        └──────┬───────┘        └──────────────┘
                           │
                    ┌──────┴───────┐
                    │InventoryItem │
                    │ (1:N 背包)   │
                    └──────────────┘

                    ┌──────────────┐
                    │  TradeLog    │ ← 独立审计表
                    └──────────────┘
```

### 1.2 实体清单

| 实体            | 说明                                      | 关键关系                                    |
| --------------- | ----------------------------------------- | ------------------------------------------- |
| `BaseEntity`    | 抽象基类，提供 id / createdAt / updatedAt | 所有实体继承                                |
| `Player`        | 玩家核心数据                              | 一对一 Profile，一对多背包/装备，多对多公会 |
| `PlayerProfile` | 玩家扩展档案                              | 反向引用 Player                             |
| `InventoryItem` | 背包道具                                  | 多对一 Player                               |
| `Equipment`     | 装备栏（穿戴中的装备）                    | 多对一 Player                               |
| `Guild`         | 公会                                      | 多对多 Player                               |
| `TradeLog`      | 交易日志                                  | 独立表，通过 playerId 关联                  |

---

## 2. ODB 对象定义

### 2.1 抽象基类

```cpp
// db/objects/base_entity.hxx
#ifndef BASE_ENTITY_HXX
#define BASE_ENTITY_HXX

#include <string>
#include <cstdint>
#include <odb/core.hxx>

// 所有实体的公共字段
#pragma db object abstract
class BaseEntity
{
public:
    uint64_t id() const { return id_; }
    const std::string& createdAt() const { return createdAt_; }
    const std::string& updatedAt() const { return updatedAt_; }

    void setCreatedAt(const std::string& t) { createdAt_ = t; }
    void setUpdatedAt(const std::string& t) { updatedAt_ = t; }

protected:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db type("DATETIME") column("created_at") not_null
    std::string createdAt_;

    #pragma db type("DATETIME") column("updated_at") not_null
    std::string updatedAt_;
};

#endif
```

### 2.2 玩家与档案

```cpp
// db/objects/player_profile.hxx
#ifndef PLAYER_PROFILE_HXX
#define PLAYER_PROFILE_HXX

#include <string>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>
#include "base_entity.hxx"

class Player;

#pragma db object table("player_profile")
class PlayerProfile : public BaseEntity
{
public:
    PlayerProfile() = default;

    PlayerProfile(const std::string& avatar, const std::string& signature)
        : avatar_(avatar), signature_(signature) {}

    const std::string& avatar() const { return avatar_; }
    const std::string& signature() const { return signature_; }
    int totalKills() const { return totalKills_; }
    int totalDeaths() const { return totalDeaths_; }
    int totalPlayTime() const { return totalPlayTime_; }

    void setAvatar(const std::string& a) { avatar_ = a; }
    void setSignature(const std::string& s) { signature_ = s; }
    void addKill() { ++totalKills_; }
    void addDeath() { ++totalDeaths_; }
    void addPlayTime(int seconds) { totalPlayTime_ += seconds; }

    odb::lazy_weak_ptr<Player>& owner() { return owner_; }

private:
    friend class odb::access;

    #pragma db inverse(profile_)
    odb::lazy_weak_ptr<Player> owner_;

    #pragma db type("VARCHAR(256)")
    std::string avatar_;

    #pragma db type("VARCHAR(128)")
    std::string signature_;

    #pragma db column("total_kills") not_null
    int totalKills_{0};

    #pragma db column("total_deaths") not_null
    int totalDeaths_{0};

    #pragma db column("total_play_time") not_null
    int totalPlayTime_{0};  // 累计在线秒数
};

#endif
```

```cpp
// db/objects/player.hxx
#ifndef PLAYER_HXX
#define PLAYER_HXX

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>
#include <odb/section.hxx>
#include "base_entity.hxx"

class PlayerProfile;
class InventoryItem;
class Equipment;
class Guild;

// 玩家状态枚举
enum class PlayerStatus : int
{
    Active   = 0,  // 正常
    Banned   = 1,  // 封禁
    Deleted  = 2   // 已删除（软删除）
};

#pragma db model version(1, 1)

#pragma db object table("player")
class Player : public BaseEntity
{
public:
    Player() = default;

    Player(const std::string& name, int level = 1)
        : name_(name), level_(level) {}

    // --- 核心字段访问 ---
    const std::string& name() const { return name_; }
    int level() const { return level_; }
    uint64_t exp() const { return exp_; }
    uint64_t gold() const { return gold_; }
    PlayerStatus status() const { return status_; }

    void setName(const std::string& n) { name_ = n; }
    void setLevel(int lv) { level_ = lv; }
    void addExp(uint64_t e) { exp_ += e; }
    void addGold(uint64_t g) { gold_ += g; }
    void deductGold(uint64_t g) { gold_ -= g; }
    void setStatus(PlayerStatus s) { status_ = s; }

    // --- 关系访问 ---
    odb::lazy_shared_ptr<PlayerProfile>& profile() { return profile_; }

    std::vector<odb::lazy_shared_ptr<InventoryItem>>& inventory()
    {
        return inventory_;
    }

    std::vector<odb::lazy_shared_ptr<Equipment>>& equipments()
    {
        return equipments_;
    }

    std::vector<odb::lazy_shared_ptr<Guild>>& guilds()
    {
        return guilds_;
    }

    // --- 扩展段访问 ---
    odb::section& skillSection() { return skillSection_; }
    const std::map<uint64_t, int>& skills() const { return skills_; }
    void setSkillLevel(uint64_t skillId, int level) { skills_[skillId] = level; }

private:
    friend class odb::access;

    // ===== 核心字段（默认加载）=====

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    #pragma db not_null index
    int level_{1};

    uint64_t exp_{0};
    uint64_t gold_{0};

    #pragma db column("vip_level") not_null
    int vipLevel_{0};

    #pragma db not_null
    PlayerStatus status_{PlayerStatus::Active};

    // ===== 关系映射 =====

    // 一对一：玩家档案
    #pragma db not_null
    odb::lazy_shared_ptr<PlayerProfile> profile_;

    // 一对多：背包道具
    #pragma db inverse(owner_) value_not_null
    std::vector<odb::lazy_shared_ptr<InventoryItem>> inventory_;

    // 一对多：装备栏
    #pragma db inverse(wearer_) value_not_null
    std::vector<odb::lazy_shared_ptr<Equipment>> equipments_;

    // 多对多：加入的公会
    #pragma db inverse(members_) value_not_null
    std::vector<odb::lazy_shared_ptr<Guild>> guilds_;

    // ===== 扩展段（按需加载）=====

    #pragma db load(lazy) update(change) section(skillSection_) \
             table("player_skills") key_column("skill_id") value_column("level")
    std::map<uint64_t, int> skills_;

    #pragma db transient
    odb::section skillSection_;
};

#endif
```

### 2.3 背包与装备

```cpp
// db/objects/inventory_item.hxx
#ifndef INVENTORY_ITEM_HXX
#define INVENTORY_ITEM_HXX

#include <cstdint>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>
#include "base_entity.hxx"

class Player;

// 道具品质
enum class ItemQuality : int
{
    White = 1, Green = 2, Blue = 3, Purple = 4, Orange = 5
};

#pragma db object table("inventory")
class InventoryItem : public BaseEntity
{
public:
    InventoryItem() = default;

    InventoryItem(uint64_t templateId, int count, int slot,
                  ItemQuality quality)
        : templateId_(templateId), count_(count),
          slot_(slot), quality_(quality) {}

    uint64_t templateId() const { return templateId_; }
    int count() const { return count_; }
    int slot() const { return slot_; }
    ItemQuality quality() const { return quality_; }

    void setCount(int c) { count_ = c; }
    void addCount(int n) { count_ += n; }
    void setSlot(int s) { slot_ = s; }

    odb::lazy_shared_ptr<Player>& owner() { return owner_; }

private:
    friend class odb::access;

    #pragma db not_null
    odb::lazy_shared_ptr<Player> owner_;

    #pragma db column("template_id") not_null index
    uint64_t templateId_{0};

    #pragma db not_null
    int count_{1};

    #pragma db not_null
    int slot_{0};

    #pragma db not_null
    ItemQuality quality_{ItemQuality::White};
};

#endif
```

```cpp
// db/objects/equipment.hxx
#ifndef EQUIPMENT_HXX
#define EQUIPMENT_HXX

#include <cstdint>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>
#include "base_entity.hxx"

class Player;

// 装备部位
enum class EquipSlot : int
{
    Head = 0, Chest = 1, Legs = 2, Feet = 3,
    MainHand = 4, OffHand = 5, Ring = 6, Necklace = 7
};

#pragma db object table("equipment")
class Equipment : public BaseEntity
{
public:
    Equipment() = default;

    Equipment(uint64_t templateId, EquipSlot slot,
              int attack, int defense, int enhanceLevel)
        : templateId_(templateId), slot_(slot),
          attack_(attack), defense_(defense),
          enhanceLevel_(enhanceLevel) {}

    uint64_t templateId() const { return templateId_; }
    EquipSlot slot() const { return slot_; }
    int attack() const { return attack_; }
    int defense() const { return defense_; }
    int enhanceLevel() const { return enhanceLevel_; }
    int maxEnhance() const { return maxEnhance_; }

    void setEnhanceLevel(int lv) { enhanceLevel_ = lv; }
    void addAttack(int a) { attack_ += a; }
    void addDefense(int d) { defense_ += d; }

    odb::lazy_shared_ptr<Player>& wearer() { return wearer_; }

private:
    friend class odb::access;

    #pragma db not_null
    odb::lazy_shared_ptr<Player> wearer_;

    #pragma db column("template_id") not_null
    uint64_t templateId_{0};

    #pragma db not_null
    EquipSlot slot_{EquipSlot::MainHand};

    int attack_{0};
    int defense_{0};

    #pragma db column("enhance_level") not_null
    int enhanceLevel_{0};

    #pragma db column("max_enhance") not_null
    int maxEnhance_{15};
};

#endif
```

### 2.4 公会

```cpp
// db/objects/guild.hxx
#ifndef GUILD_HXX
#define GUILD_HXX

#include <string>
#include <vector>
#include <cstdint>
#include <odb/core.hxx>
#include <odb/lazy-ptr.hxx>
#include "base_entity.hxx"

class Player;

#pragma db object table("guild")
class Guild : public BaseEntity
{
public:
    Guild() = default;

    Guild(const std::string& name, uint64_t leaderId)
        : name_(name), leaderId_(leaderId) {}

    const std::string& name() const { return name_; }
    uint64_t leaderId() const { return leaderId_; }
    int level() const { return level_; }
    const std::string& announcement() const { return announcement_; }

    void setLevel(int lv) { level_ = lv; }
    void setLeaderId(uint64_t id) { leaderId_ = id; }
    void setAnnouncement(const std::string& a) { announcement_ = a; }

    std::vector<odb::lazy_shared_ptr<Player>>& members()
    {
        return members_;
    }

private:
    friend class odb::access;

    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    #pragma db column("leader_id") not_null
    uint64_t leaderId_{0};

    #pragma db not_null
    int level_{1};

    #pragma db column("max_members") not_null
    int maxMembers_{50};

    #pragma db type("VARCHAR(256)")
    std::string announcement_;

    #pragma db value_not_null unordered table("guild_members")
    std::vector<odb::lazy_shared_ptr<Player>> members_;
};

#endif
```

### 2.5 交易日志

```cpp
// db/objects/trade_log.hxx
#ifndef TRADE_LOG_HXX
#define TRADE_LOG_HXX

#include <string>
#include <cstdint>
#include <odb/core.hxx>
#include "base_entity.hxx"

enum class TradeAction : int
{
    Buy       = 1,   // 商店购买
    Sell      = 2,   // 商店出售
    Transfer  = 3,   // 玩家间转账
    Enhance   = 4,   // 装备强化消耗
    Reward    = 5    // 系统奖励
};

#pragma db object table("trade_log")
class TradeLog : public BaseEntity
{
public:
    TradeLog() = default;

    TradeLog(uint64_t playerId, TradeAction action,
             uint64_t itemId, int quantity, uint64_t goldChange,
             const std::string& detail)
        : playerId_(playerId), action_(action),
          itemId_(itemId), quantity_(quantity),
          goldChange_(goldChange), detail_(detail) {}

    uint64_t playerId() const { return playerId_; }
    TradeAction action() const { return action_; }
    uint64_t itemId() const { return itemId_; }
    int quantity() const { return quantity_; }
    uint64_t goldChange() const { return goldChange_; }
    const std::string& detail() const { return detail_; }

private:
    friend class odb::access;

    #pragma db column("player_id") not_null index
    uint64_t playerId_{0};

    #pragma db not_null
    TradeAction action_{TradeAction::Buy};

    #pragma db column("item_id")
    uint64_t itemId_{0};

    int quantity_{0};

    #pragma db column("gold_change")
    uint64_t goldChange_{0};

    #pragma db type("VARCHAR(256)")
    std::string detail_;
};

#endif
```

### 2.6 View 定义

```cpp
// db/views/game_views.hxx
#ifndef GAME_VIEWS_HXX
#define GAME_VIEWS_HXX

#include <string>
#include <cstdint>
#include <odb/core.hxx>
#include "db/objects/player.hxx"
#include "db/objects/guild.hxx"
#include "db/objects/trade_log.hxx"

// 排行榜条目
#pragma db view object(Player)
struct LeaderboardEntry
{
    #pragma db column(Player::id_)
    uint64_t id;

    #pragma db column(Player::name_)
    std::string name;

    #pragma db column(Player::level_)
    int level;

    #pragma db column(Player::exp_)
    uint64_t exp;
};

// 全服统计
#pragma db view object(Player)
struct ServerStats
{
    #pragma db column("count(" + Player::id_ + ")")
    uint64_t totalPlayers;

    #pragma db column("avg(" + Player::level_ + ")")
    double avgLevel;

    #pragma db column("max(" + Player::level_ + ")")
    int maxLevel;

    #pragma db column("sum(" + Player::gold_ + ")")
    uint64_t totalGold;
};

// 公会战力排名
#pragma db view object(Guild) object(Player: Guild::members_)
struct GuildPowerRank
{
    #pragma db column(Guild::id_)
    uint64_t guildId;

    #pragma db column(Guild::name_)
    std::string guildName;

    #pragma db column("count(" + Player::id_ + ")")
    int memberCount;

    #pragma db column("sum(" + Player::level_ + ")")
    int totalPower;
};

// 每日交易汇总
#pragma db view query(                                              \
    "SELECT DATE(created_at) AS trade_date, "                       \
    "       action AS trade_action, "                               \
    "       COUNT(*) AS trade_count, "                              \
    "       SUM(gold_change) AS total_gold "                        \
    "FROM trade_log "                                               \
    "GROUP BY DATE(created_at), action "                            \
    "ORDER BY trade_date DESC, action ASC")
struct DailyTradeReport
{
    #pragma db column("trade_date") type("DATE")
    std::string tradeDate;

    #pragma db column("trade_action")
    int tradeAction;

    #pragma db column("trade_count")
    uint64_t tradeCount;

    #pragma db column("total_gold")
    uint64_t totalGold;
};

#endif
```

---

## 3. Repository 层实现

Repository 层封装所有数据库操作，让业务层不直接依赖 ODB 类型。

### 3.1 通用 Repository 基类

```cpp
// db/repository/base_repo.hxx
#ifndef BASE_REPO_HXX
#define BASE_REPO_HXX

#include <memory>
#include <vector>
#include <cstdint>
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/query.hxx>
#include <odb/result.hxx>

// 分页结果
template<typename T>
struct Page
{
    std::vector<T> items;
    int page;
    int pageSize;
    // 注意：totalCount 需要额外查询，按需使用
};

// 通用 Repository 模板
template<typename T>
class BaseRepository
{
public:
    explicit BaseRepository(odb::database& db) : db_(db) {}

    // 按主键查找（返回 nullptr 表示不存在）
    std::unique_ptr<T> findById(uint64_t id)
    {
        odb::transaction t(db_.begin());
        auto result = db_.find<T>(id);
        t.commit();
        return result;
    }

    // 按主键加载（不存在则抛异常）
    std::unique_ptr<T> loadById(uint64_t id)
    {
        odb::transaction t(db_.begin());
        auto result = db_.load<T>(id);
        t.commit();
        return result;
    }

    // 保存新对象
    uint64_t save(T& entity)
    {
        odb::transaction t(db_.begin());
        uint64_t id = db_.persist(entity);
        t.commit();
        return id;
    }

    // 更新已有对象
    void update(T& entity)
    {
        odb::transaction t(db_.begin());
        db_.update(entity);
        t.commit();
    }

    // 按主键删除
    void remove(uint64_t id)
    {
        odb::transaction t(db_.begin());
        db_.erase<T>(id);
        t.commit();
    }

    // 批量保存
    void saveBatch(std::vector<T>& entities, size_t batchSize = 500)
    {
        for (size_t i = 0; i < entities.size(); i += batchSize)
        {
            odb::transaction t(db_.begin());

            size_t end = std::min(i + batchSize, entities.size());
            for (size_t j = i; j < end; ++j)
            {
                db_.persist(entities[j]);
            }

            t.commit();
        }
    }

    // 批量更新
    void updateBatch(std::vector<T*>& entities, size_t batchSize = 500)
    {
        for (size_t i = 0; i < entities.size(); i += batchSize)
        {
            odb::transaction t(db_.begin());

            size_t end = std::min(i + batchSize, entities.size());
            for (size_t j = i; j < end; ++j)
            {
                db_.update(*entities[j]);
            }

            t.commit();
        }
    }

protected:
    odb::database& db_;
};

#endif
```

### 3.2 玩家 Repository

```cpp
// db/repository/player_repo.hxx
#ifndef PLAYER_REPO_HXX
#define PLAYER_REPO_HXX

#include "base_repo.hxx"
#include "db/objects/player.hxx"
#include "db/objects/player_profile.hxx"
#include "db/views/game_views.hxx"

class PlayerRepository : public BaseRepository<Player>
{
public:
    using BaseRepository::BaseRepository;

    // 按昵称查找
    std::unique_ptr<Player> findByName(const std::string& name)
    {
        typedef odb::query<Player> Q;

        odb::transaction t(db_.begin());
        auto player = db_.query_one<Player>(Q::name == name);
        t.commit();
        return player;
    }

    // 创建新玩家（含 Profile 初始化）
    uint64_t createPlayer(const std::string& name)
    {
        odb::transaction t(db_.begin());

        // 创建档案
        auto profile = std::make_shared<PlayerProfile>(
            "default_avatar.png", "");
        profile->setCreatedAt(now());
        profile->setUpdatedAt(now());
        db_.persist(*profile);

        // 创建玩家
        Player player(name, 1);
        player.addGold(10000);  // 新手礼金
        player.profile() = profile;
        player.setCreatedAt(now());
        player.setUpdatedAt(now());
        uint64_t id = db_.persist(player);

        t.commit();
        return id;
    }

    // 搜索玩家（支持多条件动态筛选）
    std::vector<Player> search(const std::string& keyword,
                               int minLevel, int maxLevel,
                               int limit)
    {
        typedef odb::query<Player> Q;

        odb::transaction t(db_.begin());

        Q q(Q::status == static_cast<int>(PlayerStatus::Active));

        if (!keyword.empty())
            q = q && Q::name.like("%" + keyword + "%");
        if (minLevel > 0)
            q = q && Q::level >= minLevel;
        if (maxLevel > 0)
            q = q && Q::level <= maxLevel;

        q = q + "ORDER BY" + Q::level + "DESC"
              + "LIMIT" + Q::_val(limit);

        auto r = db_.query<Player>(q);

        std::vector<Player> results;
        for (const Player& p : r)
            results.push_back(p);

        t.commit();
        return results;
    }

    // 等级排行榜
    std::vector<LeaderboardEntry> getLeaderboard(int topN)
    {
        typedef odb::query<LeaderboardEntry> Q;

        odb::transaction t(db_.begin());

        auto r = db_.query<LeaderboardEntry>(
            "ORDER BY" + Q::level + "DESC," + Q::exp + "DESC"
            + "LIMIT" + Q::_val(topN));

        std::vector<LeaderboardEntry> entries;
        for (const LeaderboardEntry& e : r)
            entries.push_back(e);

        t.commit();
        return entries;
    }

    // 全服统计
    ServerStats getServerStats()
    {
        odb::transaction t(db_.begin());
        auto stats = db_.query_value<ServerStats>();
        t.commit();
        return stats;
    }

    // 批量存档（高性能）
    void saveAll(std::vector<Player*>& players)
    {
        const size_t batchSize = 500;
        for (size_t i = 0; i < players.size(); i += batchSize)
        {
            odb::transaction t(db_.begin());

            size_t end = std::min(i + batchSize, players.size());
            for (size_t j = i; j < end; ++j)
            {
                players[j]->setUpdatedAt(now());
                db_.update(*players[j]);
            }

            t.commit();
        }
    }

private:
    // 当前时间字符串
    static std::string now()
    {
        time_t t = time(nullptr);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
        return buf;
    }
};

#endif
```

### 3.3 背包 Repository

```cpp
// db/repository/inventory_repo.hxx
#ifndef INVENTORY_REPO_HXX
#define INVENTORY_REPO_HXX

#include "base_repo.hxx"
#include "db/objects/inventory_item.hxx"

class InventoryRepository : public BaseRepository<InventoryItem>
{
public:
    using BaseRepository::BaseRepository;

    // 查询玩家的所有道具
    std::vector<InventoryItem> findByPlayer(uint64_t playerId)
    {
        typedef odb::query<InventoryItem> Q;

        odb::transaction t(db_.begin());

        auto r = db_.query<InventoryItem>(
            Q::owner == playerId
            + "ORDER BY" + Q::slot + "ASC");

        std::vector<InventoryItem> items;
        for (const InventoryItem& item : r)
            items.push_back(item);

        t.commit();
        return items;
    }

    // 查找玩家某个模板ID的道具（用于堆叠判断）
    std::unique_ptr<InventoryItem> findByPlayerAndTemplate(
        uint64_t playerId, uint64_t templateId)
    {
        typedef odb::query<InventoryItem> Q;

        odb::transaction t(db_.begin());

        auto item = db_.query_one<InventoryItem>(
            Q::owner == playerId &&
            Q::templateId == templateId);

        t.commit();
        return item;
    }

    // 按品质筛选
    std::vector<InventoryItem> findByQuality(
        uint64_t playerId, ItemQuality minQuality)
    {
        typedef odb::query<InventoryItem> Q;

        odb::transaction t(db_.begin());

        auto r = db_.query<InventoryItem>(
            Q::owner == playerId &&
            Q::quality >= static_cast<int>(minQuality));

        std::vector<InventoryItem> items;
        for (const InventoryItem& item : r)
            items.push_back(item);

        t.commit();
        return items;
    }

    // 统计玩家道具数量
    int countByPlayer(uint64_t playerId)
    {
        typedef odb::query<InventoryItem> Q;

        odb::transaction t(db_.begin());

        auto r = db_.query<InventoryItem>(Q::owner == playerId);

        int count = 0;
        for (auto it = r.begin(); it != r.end(); ++it)
            ++count;

        t.commit();
        return count;
    }
};

#endif
```

### 3.4 公会 Repository

```cpp
// db/repository/guild_repo.hxx
#ifndef GUILD_REPO_HXX
#define GUILD_REPO_HXX

#include "base_repo.hxx"
#include "db/objects/guild.hxx"
#include "db/objects/player.hxx"
#include "db/views/game_views.hxx"

class GuildRepository : public BaseRepository<Guild>
{
public:
    using BaseRepository::BaseRepository;

    // 按名称查找
    std::unique_ptr<Guild> findByName(const std::string& name)
    {
        typedef odb::query<Guild> Q;

        odb::transaction t(db_.begin());
        auto guild = db_.query_one<Guild>(Q::name == name);
        t.commit();
        return guild;
    }

    // 公会战力排名
    std::vector<GuildPowerRank> getPowerRanking(int topN)
    {
        typedef odb::query<GuildPowerRank> Q;

        odb::transaction t(db_.begin());

        auto r = db_.query<GuildPowerRank>(
            "GROUP BY" + Q::guildId + "," + Q::guildName
            + "ORDER BY" + Q::totalPower + "DESC"
            + "LIMIT" + Q::_val(topN));

        std::vector<GuildPowerRank> ranks;
        for (const GuildPowerRank& g : r)
            ranks.push_back(g);

        t.commit();
        return ranks;
    }
};

#endif
```

---

## 4. 业务场景实现

### 4.1 玩家登录加载

```cpp
// service/login_service.cxx

struct PlayerSession
{
    std::unique_ptr<Player> player;
    std::vector<InventoryItem> inventory;
    std::vector<Equipment> equipments;
};

PlayerSession onPlayerLogin(odb::database& db, uint64_t playerId)
{
    PlayerSession session;

    // 核心数据：一条 SQL
    {
        odb::transaction t(db.begin());
        session.player = db.load<Player>(playerId);
        t.commit();
    }

    // 背包数据：一条 SQL（View 方式避免 N+1）
    {
        typedef odb::query<InventoryItem> Q;
        odb::transaction t(db.begin());
        auto r = db.query<InventoryItem>(
            Q::owner == playerId + "ORDER BY" + Q::slot + "ASC");
        for (const InventoryItem& item : r)
            session.inventory.push_back(item);
        t.commit();
    }

    // 装备数据：一条 SQL
    {
        typedef odb::query<Equipment> Q;
        odb::transaction t(db.begin());
        auto r = db.query<Equipment>(Q::wearer == playerId);
        for (const Equipment& eq : r)
            session.equipments.push_back(eq);
        t.commit();
    }

    // 总计 3 条 SQL，无 N+1 问题
    // Profile 和 skills 延迟到玩家打开面板时再加载
    return session;
}
```

### 4.2 道具购买（事务原子性）

```cpp
// service/shop_service.cxx

enum class BuyResult
{
    Success, InsufficientGold, BagFull, ItemNotFound, Error
};

BuyResult buyItem(odb::database& db, uint64_t playerId,
                  uint64_t templateId, int quantity, uint64_t unitPrice)
{
    try
    {
        odb::session s;
        odb::transaction t(db.begin());

        // 1. 加载玩家
        auto player = db.load<Player>(playerId);

        // 2. 校验金币
        uint64_t totalCost = unitPrice * quantity;
        if (player->gold() < totalCost)
            return BuyResult::InsufficientGold;

        // 3. 扣金币
        player->deductGold(totalCost);
        db.update(*player);

        // 4. 检查是否已有该道具（可堆叠）
        typedef odb::query<InventoryItem> Q;
        auto existing = db.query_one<InventoryItem>(
            Q::owner == playerId && Q::templateId == templateId);

        if (existing)
        {
            // 已有，增加数量
            existing->addCount(quantity);
            db.update(*existing);
        }
        else
        {
            // 没有，创建新道具
            InventoryItem item(templateId, quantity, findEmptySlot(db, playerId),
                               ItemQuality::White);
            item.owner() = player;
            db.persist(item);
        }

        // 5. 写交易日志
        TradeLog log(playerId, TradeAction::Buy,
                     templateId, quantity, totalCost,
                     "购买道具");
        db.persist(log);

        // 6. 原子提交
        t.commit();
        return BuyResult::Success;
    }
    catch (const odb::exception& e)
    {
        // 自动回滚：金币不会被扣，道具不会被加
        LOG_ERROR("购买道具异常: {}", e.what());
        return BuyResult::Error;
    }
}

int findEmptySlot(odb::database& db, uint64_t playerId)
{
    typedef odb::query<InventoryItem> Q;
    auto items = db.query<InventoryItem>(
        Q::owner == playerId + "ORDER BY" + Q::slot + "ASC");

    int nextSlot = 0;
    for (const InventoryItem& item : items)
    {
        if (item.slot() != nextSlot)
            return nextSlot;  // 找到空位
        ++nextSlot;
    }
    return nextSlot;
}
```

### 4.3 装备强化

```cpp
// service/enhance_service.cxx

enum class EnhanceResult
{
    Success, MaxLevel, InsufficientGold, Failed, Error
};

// 强化费用表
static const uint64_t enhanceCostTable[] = {
    100, 200, 400, 800, 1600, 3200, 6400, 12800,
    25600, 51200, 100000, 200000, 400000, 800000, 1600000
};

// 强化成功率表（万分比）
static const int enhanceRateTable[] = {
    10000, 9500, 9000, 8000, 7000, 6000, 5000,
    4000, 3000, 2000, 1500, 1000, 500, 300, 100
};

EnhanceResult enhanceEquipment(odb::database& db, uint64_t playerId,
                               uint64_t equipmentId)
{
    try
    {
        odb::session s;
        odb::transaction t(db.begin());

        auto player = db.load<Player>(playerId);
        auto equip = db.load<Equipment>(equipmentId);

        // 验证装备属于该玩家
        if (equip->wearer().object_id<Player>() != playerId)
            return EnhanceResult::Error;

        int currentLevel = equip->enhanceLevel();

        // 已满级
        if (currentLevel >= equip->maxEnhance())
            return EnhanceResult::MaxLevel;

        // 检查金币
        uint64_t cost = enhanceCostTable[currentLevel];
        if (player->gold() < cost)
            return EnhanceResult::InsufficientGold;

        // 扣金币
        player->deductGold(cost);
        db.update(*player);

        // 判断是否成功（伪随机）
        int rate = enhanceRateTable[currentLevel];
        bool success = (rand() % 10000) < rate;

        if (success)
        {
            equip->setEnhanceLevel(currentLevel + 1);
            equip->addAttack(10 + currentLevel * 5);
            equip->addDefense(5 + currentLevel * 3);
            db.update(*equip);
        }
        // 失败不降级，只扣金币

        // 记录日志
        TradeLog log(playerId, TradeAction::Enhance,
                     equip->templateId(), 1, cost,
                     success ? "强化成功" : "强化失败");
        db.persist(log);

        t.commit();
        return success ? EnhanceResult::Success : EnhanceResult::Failed;
    }
    catch (const odb::exception& e)
    {
        LOG_ERROR("装备强化异常: {}", e.what());
        return EnhanceResult::Error;
    }
}
```

### 4.4 公会创建与管理

```cpp
// service/guild_service.cxx

uint64_t createGuild(odb::database& db, uint64_t leaderId,
                     const std::string& guildName)
{
    odb::session s;
    odb::transaction t(db.begin());

    // 检查名称是否已存在
    typedef odb::query<Guild> Q;
    auto existing = db.query_one<Guild>(Q::name == guildName);
    if (existing)
        throw std::runtime_error("公会名已存在");

    // 检查创建费用
    auto leader = db.load<Player>(leaderId);
    const uint64_t createCost = 50000;
    if (leader->gold() < createCost)
        throw std::runtime_error("金币不足");

    // 扣费
    leader->deductGold(createCost);
    db.update(*leader);

    // 创建公会
    Guild guild(guildName, leaderId);
    guild.members().push_back(leader);  // 会长自动加入
    uint64_t guildId = db.persist(guild);

    // 日志
    TradeLog log(leaderId, TradeAction::Buy, 0, 1, createCost,
                 "创建公会: " + guildName);
    db.persist(log);

    t.commit();
    return guildId;
}

void joinGuild(odb::database& db, uint64_t playerId, uint64_t guildId)
{
    odb::session s;
    odb::transaction t(db.begin());

    auto guild = db.load<Guild>(guildId);
    auto player = db.load<Player>(playerId);

    // 检查人数上限
    if (static_cast<int>(guild->members().size()) >= guild->maxMembers())
        throw std::runtime_error("公会已满");

    guild->members().push_back(player);
    db.update(*guild);

    t.commit();
}

void dissolveGuild(odb::database& db, uint64_t guildId, uint64_t operatorId)
{
    odb::session s;
    odb::transaction t(db.begin());

    auto guild = db.load<Guild>(guildId);

    // 只有会长可以解散
    if (guild->leaderId() != operatorId)
        throw std::runtime_error("权限不足");

    // 清空成员关系
    guild->members().clear();
    db.update(*guild);

    // 删除公会
    db.erase(*guild);

    t.commit();
}
```

### 4.5 定时全服存档

```cpp
// service/save_service.cxx

// 高性能全服存档
void worldSave(odb::database& db,
               std::vector<Player*>& onlinePlayers,
               std::vector<InventoryItem*>& dirtyItems,
               std::vector<Equipment*>& dirtyEquipments)
{
    auto startTime = std::chrono::steady_clock::now();

    const size_t batchSize = 500;

    // 阶段 1：存档玩家核心数据
    for (size_t i = 0; i < onlinePlayers.size(); i += batchSize)
    {
        odb::transaction t(db.begin());
        size_t end = std::min(i + batchSize, onlinePlayers.size());
        for (size_t j = i; j < end; ++j)
            db.update(*onlinePlayers[j]);
        t.commit();
    }

    // 阶段 2：存档变更过的道具
    for (size_t i = 0; i < dirtyItems.size(); i += batchSize)
    {
        odb::transaction t(db.begin());
        size_t end = std::min(i + batchSize, dirtyItems.size());
        for (size_t j = i; j < end; ++j)
            db.update(*dirtyItems[j]);
        t.commit();
    }

    // 阶段 3：存档变更过的装备
    for (size_t i = 0; i < dirtyEquipments.size(); i += batchSize)
    {
        odb::transaction t(db.begin());
        size_t end = std::min(i + batchSize, dirtyEquipments.size());
        for (size_t j = i; j < end; ++j)
            db.update(*dirtyEquipments[j]);
        t.commit();
    }

    auto elapsed = std::chrono::steady_clock::now() - startTime;
    LOG_INFO("全服存档完成: {} 玩家, {} 道具, {} 装备, 耗时 {}ms",
             onlinePlayers.size(), dirtyItems.size(),
             dirtyEquipments.size(),
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 elapsed).count());
}
```

---

## 5. Schema 迁移演示

### 5.1 v1 → v2：添加 VIP 等级字段

```cpp
// 修改 player.hxx 中的版本号
#pragma db model version(2, 1)

// Player 类中添加字段（v1 已有 vipLevel_，假设这是新增需求）
// #pragma db column("login_count") not_null default(0)
// int loginCount_{0};
```

```bash
# 重新运行 ODB 编译器
odb --database mysql --generate-schema embedded --generate-query \
    --changelog-dir changelog --std c++17 \
    src/db/objects/player.hxx

# ODB 自动检测变更，在 changelog 中记录差异
# 程序下次启动时自动执行:
# ALTER TABLE player ADD COLUMN login_count INT NOT NULL DEFAULT 0;
```

### 5.2 v2 → v3：添加成就系统表

```cpp
#pragma db model version(3, 1)

// 新增成就对象（新文件 achievement.hxx）
#pragma db object table("achievement")
class Achievement : public BaseEntity
{
    // ...
};

// ODB 检测到新对象，自动生成 CREATE TABLE achievement
```

### 5.3 启动时自动迁移

```cpp
// main.cxx — 服务器启动入口
int main(int argc, char* argv[])
{
    // 1. 创建数据库连接
    auto factory = std::make_unique<odb::mysql::connection_pool_factory>(5, 20);
    std::unique_ptr<odb::database> db(
        new odb::mysql::database(
            "game_user", "password", "game_world_1",
            "127.0.0.1", 3306, "", "utf8mb4",
            0, std::move(factory)));

    // 2. Schema 迁移（自动检测版本差异并执行 DDL）
    {
        odb::transaction t(db->begin());
        odb::schema_catalog::create_schema(*db);
        t.commit();
        LOG_INFO("Schema 初始化/迁移完成");
    }

    // 3. 初始化 Repository 层
    PlayerRepository playerRepo(*db);
    InventoryRepository inventoryRepo(*db);
    GuildRepository guildRepo(*db);

    // 4. 启动游戏逻辑
    LOG_INFO("游戏服务器启动");

    // 打印全服统计
    auto stats = playerRepo.getServerStats();
    LOG_INFO("全服玩家: {}, 平均等级: {:.1f}, 金币总量: {}",
             stats.totalPlayers, stats.avgLevel, stats.totalGold);

    // ... 启动网络监听、定时存档等 ...

    return 0;
}
```

---

## 设计要点

### Repository 模式在 C++ 中的实践

Repository 模式来自领域驱动设计（DDD），其核心思想是：**业务层只关注领域对象，不关心数据怎么存取**。

```
❌ 没有 Repository：ODB 类型渗透到业务层
  void processLogin(odb::database& db, ...) {
      odb::transaction t(db.begin());
      typedef odb::query<Player> Q;
      auto r = db.query<Player>(Q::name == name);
      ...
  }
  // 业务代码里到处是 odb::transaction、odb::query

✅ 有 Repository：业务层只调用 Repository 方法
  void processLogin(PlayerRepository& repo, ...) {
      auto player = repo.findByName(name);
      ...
  }
  // 业务代码不知道 ODB 的存在
  // 测试时可以用 MockPlayerRepository 替换
```

但在 C++ 游戏服务器中，不要过度抽象：
- Repository 是薄封装，**不要在里面放业务逻辑**
- 事务边界应该由**调用方**控制（Service 层），而非 Repository 内部
- 简单的 CRUD 直接用 BaseRepository 模板，不需要为每个实体都写子类

### 抽象基类 BaseEntity 的取舍

**好处**：
- `id`、`createdAt`、`updatedAt` 只定义一次
- 可以写通用的审计逻辑（自动填充时间戳）
- Repository 模板可以基于 BaseEntity 约束类型

**代价**：
- 具体表继承模式下，每个子类表都有这些列（这是期望的行为）
- 如果某个实体确实不需要 updatedAt（如只写不更新的日志表），要么忍受冗余列，要么不继承

游戏服务器中推荐使用 BaseEntity——几乎所有实体都需要 ID 和时间戳，统一管理比灵活性更重要。

### 何时该用原生 SQL

ODB 覆盖了大部分场景，但以下情况应该果断使用原生 SQL：

| 场景                        | 原因                                                               |
| --------------------------- | ------------------------------------------------------------------ |
| 批量 UPDATE（如全服加金币） | `UPDATE player SET gold = gold + 1000` 比逐个 load+update 快几十倍 |
| 复杂统计报表                | 多层嵌套子查询、窗口函数，超出 ODB View 能力                       |
| 合服数据迁移                | INSERT INTO ... SELECT FROM 跨库操作                               |
| 临时修数据（GM 工具）       | 一次性操作，不值得定义 View                                        |

原生 SQL 不是"退化"，而是**正确的工具选择**。ODB 处理日常 CRUD 和对象关系，原生 SQL 处理批量操作和复杂查询，两者互补。

---

## 小结

本篇我们综合运用了系列全部知识，构建了一个完整的游戏存档系统：

| 模块     | 使用的 ODB 特性                                        |
| -------- | ------------------------------------------------------ |
| 数据建模 | abstract 基类、pragma 注解、枚举映射                   |
| 关系映射 | 一对一（Profile）、一对多（背包/装备）、多对多（公会） |
| 查询     | 类型安全查询、动态条件、View 聚合                      |
| 事务     | RAII 事务保证道具购买/装备强化原子性                   |
| 性能     | 分批存档、section 延迟加载、避免 N+1                   |
| 迁移     | changelog 自动检测、启动时 migrate                     |
| 架构     | Repository 模式封装、分层设计                          |

回顾整个系列：

| 篇章                                                   | 核心收获                                   |
| ------------------------------------------------------ | ------------------------------------------ |
| [第一篇]({{< relref "深入学习ODB_编译器与注解.md" >}}) | 编译期代码生成、pragma 注解、第一个 Player |
| [第二篇]({{< relref "深入学习ODB_连接与事务.md" >}})   | 连接池、RAII 事务、CRUD、session 缓存      |
| [第三篇]({{< relref "深入学习ODB_对象关系.md" >}})     | 一对一/一对多/多对多、lazy_ptr、inverse    |
| [第四篇]({{< relref "深入学习ODB_类型安全查询.md" >}}) | 操作符重载查询、动态条件、预备语句         |
| [第五篇]({{< relref "深入学习ODB_继承与视图.md" >}})   | 三种继承策略、View 投影与聚合              |
| [第六篇]({{< relref "深入学习ODB_迁移与多库.md" >}})   | changelog 迁移、多数据库切换               |
| [第七篇]({{< relref "深入学习ODB_性能与实践.md" >}})   | 批量操作、N+1 规避、section、连接池调优    |
| **第八篇**（本文）                                     | 综合实战：完整游戏存档系统                 |

ODB 是 C++ 世界中独特的存在——它证明了编译期代码生成在 ORM 领域的可行性与高效性。虽然它不如 Java Hibernate 那样功能全面，但在 C++ 游戏服务器这个特定领域，它的零运行时开销、类型安全和非侵入性设计恰好是最优选择。

---

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | **实战项目**（本文）
