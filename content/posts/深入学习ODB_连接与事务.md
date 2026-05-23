+++
title = '深入学习 ODB（二）：连接管理与事务的艺术'
date = '2025-05-31'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "事务", "连接池", "RAII"]
categories = ["ODB学习"]
description = "深入 ODB 的数据库工厂、连接池机制、事务 RAII 语义，以游戏服务器的金币交易、道具发放为例，理解 persist/load/update/erase 的事务保证。"
+++

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | **连接与事务**（本文） | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})

---

## 引言：一次失败的道具购买

某天，游戏服务器日志里出现了一条玩家投诉：

> "我花了 500 金币买药水，金币扣了但背包里没有药水！"

排查代码发现问题出在这里：

```cpp
// 错误示范：非原子操作
void buyItem(Player& player, uint64_t itemId, uint64_t price)
{
    player.deductGold(price);
    updatePlayerGold(conn, player);  // SQL: UPDATE player SET gold=? WHERE id=?

    // ← 假如这里服务器崩溃了...

    Item item(itemId, player.id());
    insertItem(conn, item);          // SQL: INSERT INTO inventory ...
}
```

金币扣除成功提交了，但道具插入还没执行服务器就崩了。**数据不一致**。

这个问题的根本解法是 **事务（Transaction）**：要么全部成功，要么全部回滚。ODB 用 RAII 语义让事务管理变得优雅且安全——即使代码抛异常，也不会产生"半完成"状态。

---

## 1. database 工厂与连接池

### 1.1 创建数据库连接

ODB 中，`odb::database` 是所有操作的入口。对于 MySQL 后端：

```cpp
#include <odb/mysql/database.hxx>
#include <memory>

// 最简方式：直接传参数
std::unique_ptr<odb::database> db(
    new odb::mysql::database(
        "game_user",      // 用户名
        "s3cret_pass",    // 密码
        "game_world_1",   // 数据库名
        "127.0.0.1",      // 主机地址
        3306,             // 端口
        "",               // UNIX socket（为空则用 TCP）
        "utf8mb4"         // 字符集
    ));
```

### 1.2 连接池机制

游戏服务器是高并发场景——数千玩家同时在线，每次操作都新建/销毁连接代价太高。ODB 内置了连接池：

```cpp
#include <odb/mysql/database.hxx>
#include <odb/mysql/connection-factory.hxx>

// 创建连接池工厂：最小 5 连接，最大 20 连接
std::unique_ptr<odb::mysql::connection_pool_factory> factory(
    new odb::mysql::connection_pool_factory(
        5,    // min_connections：池中保持的最小空闲连接数
        20    // max_connections：池中最大连接数（0 = 无限制）
    ));

// 将工厂传给 database 构造函数
std::unique_ptr<odb::database> db(
    new odb::mysql::database(
        "game_user",
        "s3cret_pass",
        "game_world_1",
        "127.0.0.1",
        3306,
        "",
        "utf8mb4",
        0,                          // flags
        std::move(factory)          // 连接池工厂
    ));
```

**连接池工作流程**：

```
┌─────────────────────────────────────────────────┐
│               连接池 (pool)                       │
│                                                  │
│  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐      │
│  │conn1│ │conn2│ │conn3│ │conn4│ │conn5│ ...   │
│  └──┬──┘ └──┬──┘ └──┬──┘ └─────┘ └─────┘      │
│     │        │        │      idle    idle        │
│     ▼        ▼        ▼                          │
│  Thread1  Thread2  Thread3                       │
│  (玩家存档) (道具交易) (排行榜)                    │
└─────────────────────────────────────────────────┘

请求连接 → 池中有空闲？→ 是：取出使用
                         → 否且未达上限：新建连接
                         → 否且已达上限：等待归还
归还连接 → 放回池中，标记为空闲
```

### 1.3 不同连接工厂的选择

ODB 提供三种连接工厂：

| 工厂类型 | 行为 | 适用场景 |
|---------|------|---------|
| `connection_pool_factory` | 维护连接池，复用连接 | **游戏服务器首选**，高并发 |
| `new_connection_factory` | 每次请求新建连接 | 低频工具脚本 |
| 默认（不传工厂） | 等同于无限连接池 | 简单测试 |

> **游戏服务器建议**：连接池大小 = CPU 核数 × 2 + 磁盘数。对于 8 核服务器，16~20 个连接通常足够。过多连接反而因 MySQL 内部锁争用降低性能。

---

## 2. 事务（Transaction）深入

### 2.1 RAII 事务对象

ODB 的事务是 RAII 风格的——通过构造和析构控制事务生命周期：

```cpp
#include <odb/transaction.hxx>

void savePlayerData(odb::database& db, Player& player)
{
    // 构造 transaction 对象 = BEGIN TRANSACTION
    odb::transaction t(db.begin());

    // 在事务内执行操作
    db.update(player);

    // 显式提交 = COMMIT
    t.commit();

    // 如果在 commit() 之前抛出异常：
    // transaction 析构函数会自动执行 ROLLBACK
}
```

**关键语义**：

```
transaction t(db.begin());   →  MySQL: START TRANSACTION
      │
      │  db.persist(obj);    →  MySQL: INSERT ...
      │  db.update(obj);     →  MySQL: UPDATE ...
      │
      ├─ t.commit();         →  MySQL: COMMIT        ← 正常路径
      │
      └─ ~transaction()      →  MySQL: ROLLBACK      ← 异常路径（未 commit）
```

### 2.2 异常安全保证

这是 RAII 事务的核心价值。看一个实际的游戏场景：

```cpp
// 道具购买：扣金币 + 添加道具，必须原子执行
void buyItem(odb::database& db, uint64_t playerId,
             uint64_t itemTemplateId, uint64_t price)
{
    odb::transaction t(db.begin());  // BEGIN

    // 1. 加载玩家
    std::unique_ptr<Player> player(db.load<Player>(playerId));

    // 2. 检查金币是否足够
    if (player->gold() < price)
    {
        throw std::runtime_error("金币不足");
        // ↑ 抛异常 → ~transaction() 自动 ROLLBACK
        //   数据库状态不变，安全！
    }

    // 3. 扣除金币
    player->deductGold(price);
    db.update(*player);

    // 4. 创建道具
    InventoryItem item(playerId, itemTemplateId, 1);
    db.persist(item);

    // 5. 记录交易日志
    TradeLog log(playerId, "buy_item", itemTemplateId, price);
    db.persist(log);

    // 6. 全部成功，提交
    t.commit();  // COMMIT

    // 如果步骤 4 或 5 出错（比如道具模板 ID 无效触发外键约束），
    // 异常向上传播 → t 的析构函数执行 ROLLBACK
    // → 金币不会被扣除！数据始终一致。
}
```

### 2.3 嵌套事务与 Savepoint

ODB 不支持真正的嵌套事务（MySQL 不支持），但可以通过 savepoint 实现部分回滚：

```cpp
void complexOperation(odb::database& db, Player& player)
{
    odb::transaction t(db.begin());

    // 基础操作：一定要执行
    player.addExp(100);
    db.update(player);

    // 尝试发送邮件奖励（可能失败，但不影响经验值）
    try
    {
        // 手动执行 savepoint（ODB 不直接封装，用原生 SQL）
        db.execute("SAVEPOINT sp_mail");

        Mail mail(player.id(), "系统", "升级奖励", "恭喜升级！");
        db.persist(mail);

        MailAttachment att(mail.id(), ITEM_EXP_POTION, 5);
        db.persist(att);

        db.execute("RELEASE SAVEPOINT sp_mail");
    }
    catch (const odb::exception&)
    {
        // 邮件发送失败，回滚到 savepoint，经验值保留
        db.execute("ROLLBACK TO SAVEPOINT sp_mail");
    }

    t.commit();  // 经验值一定会提交
}
```

### 2.4 事务隔离级别

游戏服务器常见的并发场景需要关注隔离级别：

```cpp
// 设置当前连接的隔离级别（在 begin 之前）
db.execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED");

odb::transaction t(db.begin());
// ... 操作 ...
t.commit();
```

| 隔离级别 | 游戏场景 | 说明 |
|---------|---------|------|
| READ COMMITTED | 排行榜查询 | 允许不可重复读，性能好 |
| REPEATABLE READ | 道具交易（MySQL 默认） | 同一事务内多次读取一致 |
| SERIALIZABLE | 拍卖行竞价 | 最严格，性能最差 |

---

## 3. CRUD 四大操作详解

### 3.1 persist()：创建新对象

```cpp
// 场景：新玩家注册
void registerPlayer(odb::database& db, const std::string& name)
{
    odb::transaction t(db.begin());

    Player player(name, 1);  // 昵称，初始等级 1
    player.addGold(10000);   // 新手礼包：1万金币

    // persist() 插入新行，返回自增 ID
    uint64_t id = db.persist(player);
    // 执行后 player.id_ 被自动赋值

    std::cout << "注册成功，玩家ID: " << id << std::endl;

    t.commit();
}
// 生成的 SQL:
// INSERT INTO `player` (`name`,`level`,`exp`,`gold`,`created_at`)
// VALUES (?,?,?,?,?)
```

**注意事项**：
- 对已存在的对象调用 `persist()` 会抛 `odb::object_already_persistent` 异常
- 如果主键是 `id auto`，persist 后 ODB 自动回填自增值
- 如果主键是手动赋值的，你需要在 persist 前设好 ID

### 3.2 load()：按主键读取

```cpp
// 场景：玩家登录，从数据库加载角色数据
std::unique_ptr<Player> loadPlayer(odb::database& db, uint64_t playerId)
{
    odb::transaction t(db.begin());

    // load() 按主键查询，找不到抛 object_not_persistent 异常
    std::unique_ptr<Player> player(db.load<Player>(playerId));

    t.commit();
    return player;
}
// 生成的 SQL:
// SELECT `id`,`name`,`level`,`exp`,`gold`,`created_at`
// FROM `player` WHERE `id`=?
```

**load vs find 的区别**：

```cpp
// load()：找不到直接抛异常
std::unique_ptr<Player> p(db.load<Player>(playerId));
// 如果 playerId 不存在 → 抛 odb::object_not_persistent

// find()：找不到返回 nullptr（不抛异常）
std::unique_ptr<Player> p(db.find<Player>(playerId));
if (p == nullptr)
{
    // 玩家不存在，正常处理
    std::cout << "角色不存在" << std::endl;
}
```

**选择建议**：
- `load()`：你确定对象存在（比如通过 session 缓存查过），找不到说明有 bug
- `find()`：不确定是否存在（比如玩家输入的 ID），需要优雅处理缺失情况

### 3.3 update()：更新已有对象

```cpp
// 场景：玩家击杀怪物，获得经验和金币
void onMonsterKilled(odb::database& db, uint64_t playerId,
                     uint64_t expReward, uint64_t goldReward)
{
    odb::transaction t(db.begin());

    std::unique_ptr<Player> player(db.load<Player>(playerId));

    player->addExp(expReward);
    player->addGold(goldReward);

    // 检查是否升级
    while (player->exp() >= expTable[player->level()])
    {
        player->addExp(-expTable[player->level()]);
        player->setLevel(player->level() + 1);
    }

    // update() 将整个对象的当前状态同步到数据库
    db.update(*player);

    t.commit();
}
// 生成的 SQL:
// UPDATE `player`
// SET `name`=?, `level`=?, `exp`=?, `gold`=?, `created_at`=?
// WHERE `id`=?
```

**重要细节**：`update()` 默认更新**所有列**（不仅仅是变化的列）。这是设计权衡——追踪"脏字段"需要额外运行时开销，ODB 选择简单直接的全量更新。对于游戏服务器，这通常不是问题，因为玩家数据本身字段不多。

### 3.4 erase()：删除对象

```cpp
// 场景：删除角色（或标记为已删除）
void deletePlayer(odb::database& db, uint64_t playerId)
{
    odb::transaction t(db.begin());

    // 方式 1：按主键直接删除（不需要先 load）
    db.erase<Player>(playerId);

    // 方式 2：删除已加载的对象
    // std::unique_ptr<Player> p(db.load<Player>(playerId));
    // db.erase(*p);

    t.commit();
}
// 生成的 SQL:
// DELETE FROM `player` WHERE `id`=?
```

> **游戏服务器提示**：实际项目中很少物理删除玩家数据。更常见的做法是"软删除"——加一个 `deleted_at` 字段：
> ```cpp
> #pragma db type("DATETIME") null
> odb::nullable<std::string> deletedAt_;
> ```

### 3.5 reload()：刷新对象

```cpp
// 场景：长时间在线的玩家，需要刷新可能被 GM 修改的数据
void refreshPlayer(odb::database& db, Player& player)
{
    odb::transaction t(db.begin());

    // reload() 重新从数据库加载，覆盖内存中的值
    db.reload(player);

    t.commit();
}
// 等价于 load()，但是更新已有对象而非返回新对象
```

---

## 4. session 与对象缓存

### 4.1 身份映射（Identity Map）

考虑这个场景：

```cpp
odb::transaction t(db.begin());

// 两次加载同一个玩家
std::unique_ptr<Player> p1(db.load<Player>(1001));
std::unique_ptr<Player> p2(db.load<Player>(1001));

// 没有 session 时：p1 和 p2 是两个独立对象！
// 修改 p1 不影响 p2，最后 update 哪个就覆盖另一个的修改
p1->addGold(100);
p2->addGold(200);
db.update(*p1);  // gold + 100
db.update(*p2);  // 覆盖！变成 gold + 200，p1 的 +100 丢失了
```

`odb::session` 通过身份映射解决这个问题：

```cpp
#include <odb/session.hxx>

// 在作用域内开启 session
odb::session s;

odb::transaction t(db.begin());

std::unique_ptr<Player> p1(db.load<Player>(1001));
std::unique_ptr<Player> p2(db.load<Player>(1001));

// 有 session 时：p1 和 p2 指向同一个对象！
// 第二次 load 直接从缓存返回，不发 SQL
assert(p1.get() == p2.get());  // true

t.commit();
```

### 4.2 session 的作用域

```cpp
// session 是基于作用域的
{
    odb::session s;  // session 开始

    // 此作用域内所有 load/find 都走缓存
    odb::transaction t(db.begin());
    auto p = db.load<Player>(1001);  // 发 SQL
    auto p2 = db.load<Player>(1001); // 命中缓存，不发 SQL
    t.commit();

}  // session 析构，缓存清空

// 此时再 load 会重新发 SQL
```

### 4.3 何时使用 session？

| 场景 | 是否需要 session | 原因 |
|------|-----------------|------|
| 简单的单对象 CRUD | 不需要 | 没有重复加载，session 是多余开销 |
| 对象关系遍历 | **需要** | 多对多关系会多次 load 同一对象 |
| 一个请求内的复杂业务逻辑 | **需要** | 避免同一实体多份拷贝导致数据覆盖 |
| 跨请求的长连接 | **不建议** | 缓存会越来越大，数据变陈旧 |

> **游戏服务器模式**：通常每个玩家请求（或每帧 tick）开启一个 session，请求结束即销毁。不要让 session 跨越太长时间。

---

## 5. 错误处理模式

### 5.1 ODB 异常层次

```
odb::exception（基类）
├── odb::database_exception      — 数据库返回的错误
│   └── odb::mysql::database_exception  — MySQL 特有错误（含错误码）
├── odb::object_not_persistent   — load() 找不到对象
├── odb::object_already_persistent — persist() 主键冲突
├── odb::result_not_cached       — 结果集未缓存就访问
├── odb::connection_lost         — 连接丢失
└── odb::timeout                 — 操作超时
```

### 5.2 实战：游戏服务器的错误处理

```cpp
#include <odb/exceptions.hxx>
#include <odb/mysql/exceptions.hxx>

void createGuild(odb::database& db, uint64_t leaderId,
                 const std::string& guildName)
{
    try
    {
        odb::transaction t(db.begin());

        Guild guild(guildName, leaderId);
        db.persist(guild);

        t.commit();
    }
    catch (const odb::object_already_persistent&)
    {
        // 主键冲突（极少发生，除非手动指定 ID）
        // 处理：返回错误码给客户端
        sendError(leaderId, ERR_GUILD_CREATE_FAILED);
    }
    catch (const odb::mysql::database_exception& e)
    {
        // MySQL 特有错误，可以获取错误码
        if (e.error() == 1062)  // ER_DUP_ENTRY
        {
            // 唯一约束冲突：公会名已存在
            sendError(leaderId, ERR_GUILD_NAME_TAKEN);
        }
        else
        {
            // 其他数据库错误，记录日志
            LOG_ERROR("创建公会失败: {} (MySQL errno={})",
                      e.what(), e.error());
            sendError(leaderId, ERR_INTERNAL);
        }
    }
    catch (const odb::connection_lost&)
    {
        // 连接断开，触发重连逻辑
        LOG_WARN("数据库连接丢失，尝试重连...");
        reconnectDatabase();
        sendError(leaderId, ERR_TRY_AGAIN);
    }
    // 注意：无需手动 rollback
    // transaction 在异常路径析构时自动 rollback
}
```

### 5.3 连接断开的重试策略

```cpp
// 带重试的事务执行器
template<typename Func>
void executeWithRetry(odb::database& db, Func&& func, int maxRetries = 3)
{
    for (int attempt = 1; attempt <= maxRetries; ++attempt)
    {
        try
        {
            odb::transaction t(db.begin());
            func(db, t);
            t.commit();
            return;  // 成功，退出
        }
        catch (const odb::connection_lost&)
        {
            LOG_WARN("数据库连接丢失，第 {}/{} 次重试", attempt, maxRetries);
            if (attempt == maxRetries)
                throw;  // 重试耗尽，向上抛出

            std::this_thread::sleep_for(
                std::chrono::milliseconds(100 * attempt));  // 退避
        }
    }
}

// 使用
executeWithRetry(db, [&](odb::database& db, odb::transaction& t) {
    auto player = db.load<Player>(playerId);
    player->addGold(reward);
    db.update(*player);
});
```

---

## 6. 完整实战：道具购买流程

将本篇所有知识点串联起来，实现一个完整的道具购买逻辑：

```cpp
// item_shop.hxx — 商店系统
#include <odb/core.hxx>
#include "player.hxx"

// 商品模板（策划配置表，只读）
#pragma db object readonly table("item_template")
class ItemTemplate
{
public:
    ItemTemplate() = default;

    uint64_t id() const { return id_; }
    const std::string& name() const { return name_; }
    uint64_t price() const { return price_; }
    int maxStack() const { return maxStack_; }

private:
    friend class odb::access;

    #pragma db id
    uint64_t id_{0};

    #pragma db type("VARCHAR(64)") not_null
    std::string name_;

    uint64_t price_{0};

    #pragma db column("max_stack")
    int maxStack_{1};
};

// 背包道具（玩家持有）
#pragma db object table("inventory")
class InventoryItem
{
public:
    InventoryItem() = default;

    InventoryItem(uint64_t playerId, uint64_t templateId, int count)
        : playerId_(playerId), templateId_(templateId), count_(count) {}

    uint64_t id() const { return id_; }
    uint64_t playerId() const { return playerId_; }
    uint64_t templateId() const { return templateId_; }
    int count() const { return count_; }

    void addCount(int n) { count_ += n; }

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db column("player_id") not_null index
    uint64_t playerId_{0};

    #pragma db column("template_id") not_null
    uint64_t templateId_{0};

    #pragma db not_null
    int count_{0};
};

// 交易日志（审计用）
#pragma db object table("trade_log")
class TradeLog
{
public:
    TradeLog() = default;

    TradeLog(uint64_t playerId, const std::string& action,
             uint64_t itemId, uint64_t amount)
        : playerId_(playerId), action_(action),
          itemId_(itemId), amount_(amount) {}

private:
    friend class odb::access;

    #pragma db id auto
    uint64_t id_{0};

    #pragma db column("player_id") not_null index
    uint64_t playerId_{0};

    #pragma db type("VARCHAR(32)") not_null
    std::string action_;

    #pragma db column("item_id")
    uint64_t itemId_{0};

    uint64_t amount_{0};

    #pragma db type("TIMESTAMP") column("created_at") not_null
    std::string createdAt_;
};
```

```cpp
// shop_service.cxx — 购买逻辑

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/session.hxx>

#include "player.hxx"
#include "player-odb.hxx"
#include "item_shop.hxx"
#include "item_shop-odb.hxx"

enum class BuyResult
{
    Success,
    InsufficientGold,
    ItemNotFound,
    BagFull,
    DatabaseError
};

BuyResult buyItem(odb::database& db, uint64_t playerId,
                  uint64_t itemTemplateId, int quantity)
{
    try
    {
        // 开启 session 防止同一对象多次加载
        odb::session s;
        odb::transaction t(db.begin());

        // 1. 查询商品模板（只读，不进事务写集）
        std::unique_ptr<ItemTemplate> tmpl(
            db.find<ItemTemplate>(itemTemplateId));
        if (!tmpl)
            return BuyResult::ItemNotFound;

        // 2. 加载玩家数据
        std::unique_ptr<Player> player(db.load<Player>(playerId));

        // 3. 计算总价并校验
        uint64_t totalPrice = tmpl->price() * quantity;
        if (player->gold() < totalPrice)
            return BuyResult::InsufficientGold;

        // 4. 扣金币
        player->deductGold(totalPrice);
        db.update(*player);

        // 5. 添加道具到背包
        InventoryItem item(playerId, itemTemplateId, quantity);
        db.persist(item);

        // 6. 写交易日志
        TradeLog log(playerId, "buy", itemTemplateId, totalPrice);
        db.persist(log);

        // 7. 提交事务——以上步骤全部原子执行
        t.commit();

        return BuyResult::Success;
    }
    catch (const odb::connection_lost&)
    {
        // 连接丢失，事务自动回滚，客户端可重试
        return BuyResult::DatabaseError;
    }
    catch (const odb::exception& e)
    {
        LOG_ERROR("购买道具异常: {}", e.what());
        // transaction 析构自动 rollback
        return BuyResult::DatabaseError;
    }
    // 正常退出或异常退出，数据都是一致的
}
```

**这段代码的事务保证**：

| 步骤 | 失败时的表现 |
|------|-------------|
| 步骤 1-3 失败 | 直接 return，事务析构 rollback，什么都没改 |
| 步骤 4 后崩溃 | 事务未 commit，金币扣除自动回滚 |
| 步骤 5 后崩溃 | 事务未 commit，金币和道具都回滚 |
| 步骤 7 commit 成功 | 金币扣除、道具添加、日志记录同时生效 |

这就是 RAII 事务的力量——你不需要在每个分支写 rollback，只要不 commit 就自动安全。

---

## 设计要点

### 事务为何采用 RAII 而非 begin/end 显式调用？

对比两种 API 风格：

```cpp
// 风格 A：显式 begin/end（Java JDBC 风格）
db.beginTransaction();
try {
    db.update(player);
    db.persist(item);
    db.commitTransaction();  // 容易忘记！
} catch (...) {
    db.rollbackTransaction();  // 每个 catch 都要写！
    throw;
}
// 问题：如果 catch 之前有 early return，事务泄露

// 风格 B：RAII（ODB 的选择）
{
    odb::transaction t(db.begin());
    db.update(player);
    db.persist(item);
    t.commit();
}
// 无论正常退出、异常、还是 early return，析构函数都保证 rollback
```

RAII 的优势：

1. **异常安全** — C++ 的异常可以从任何地方抛出，RAII 确保不遗漏 rollback
2. **编译器帮你检查** — 如果忘记 `commit()`，事务自动 rollback（安全侧倾）
3. **作用域即边界** — 事务的生命周期一目了然
4. **嵌套友好** — 可以用 `{}` 划定精确的事务范围

> **C++ 哲学**：资源获取即初始化。锁是 `lock_guard`，文件是 `fstream`，事务就是 `transaction`。同一套心智模型，零额外学习成本。

### 连接池为什么是工厂模式？

ODB 不是直接传一个 `pool_size` 参数，而是传一个 `connection_factory` 对象。为什么？

```cpp
// 你可以实现自定义工厂
class MyGameConnectionFactory : public odb::mysql::connection_factory
{
public:
    // 比如：记录连接创建/销毁的监控指标
    // 比如：在连接创建时设置 session 变量
    // 比如：实现读写分离（读请求走从库，写请求走主库）
};
```

工厂模式的好处：
1. **策略可替换** — 测试时用单连接、生产用池化连接，一行代码切换
2. **可扩展** — 需要连接监控？继承工厂重写即可
3. **关注点分离** — database 类不需要知道连接怎么管理

### session 身份映射解决了什么问题？

核心问题：**同一数据库行对应多个 C++ 对象时的一致性**。

```
没有 session：
  load(1001) → Player 对象 A (gold=500)
  load(1001) → Player 对象 B (gold=500)  // 独立拷贝
  A.addGold(100)  → A.gold = 600
  B.addGold(200)  → B.gold = 700
  update(A)  → 数据库 gold=600
  update(B)  → 数据库 gold=700  // A 的修改被覆盖！

有 session：
  load(1001) → Player 对象 A (gold=500)
  load(1001) → 返回 A 本身（缓存命中，不发 SQL）
  // 只有一个对象，不存在覆盖问题
```

这对关系映射尤其重要——当你遍历 Guild -> Members -> Player 时，同一个 Player 可能被多条路径引用。没有 session，就会出现数据撕裂。

---

## 小结

本篇我们掌握了：

1. **连接池** — 游戏服务器必备，`connection_pool_factory` 管理连接复用
2. **RAII 事务** — 构造即 BEGIN，commit 即 COMMIT，析构即 ROLLBACK，异常安全
3. **CRUD 四件套** — persist / load(find) / update / erase，每个操作的语义和生成的 SQL
4. **session 缓存** — 身份映射避免同一行产生多个对象导致数据覆盖
5. **错误处理** — 异常层次、MySQL 错误码、连接丢失重试

**下一篇**我们将进入对象关系的世界——玩家有背包（一对多）、有详细档案（一对一）、可以加入公会（多对多）。ODB 如何用 C++ 智能指针和容器优雅地映射这些关系？

---

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | **连接与事务**（本文） | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | [类型安全查询]({{< relref "深入学习ODB_类型安全查询.md" >}}) | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})
