+++
title = '深入学习 ODB（四）：ODB Query Language——类型安全的查询之道'
date = '2025-06-14'
draft = false
tags = ["C++", "ODB", "ORM", "MySQL", "查询", "类型安全", "预备语句"]
categories = ["ODB学习"]
description = "掌握 ODB 的类型安全查询系统：以等级排行榜、道具筛选、玩家搜索为例，从简单条件到复合查询、排序分页、原生 SQL 混用，理解编译期查询校验的设计。"
+++

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | **类型安全查询**（本文） | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})

---

## 引言：手写 SQL 查询的隐患

游戏服务器中，查询无处不在——排行榜、道具筛选、好友搜索、日志审计。手写 SQL 最大的风险是**编译器看不见的错误**：

```cpp
// ❌ 手写 SQL：字段名拼错，编译不报错，运行时才崩
auto result = mysql_query(conn,
    "SELECT * FROM player WHERE levle > 50 ORDER BY levle DESC");
//                                  ^^^^                ^^^^
//                               拼成了 levle，MySQL 直接报错
//                               但编译器对此一无所知

// ❌ 类型错误：用字符串比较数字
auto result = mysql_query(conn,
    "SELECT * FROM player WHERE level > '50'");
//                                      ^^^^
//                              隐式类型转换，可能有意外行为
```

ODB 的查询系统从根本上解决了这个问题——**字段名是 C++ 符号，类型是编译期检查的**。拼错字段名？编译不通过。类型不匹配？编译不通过。

---

## 1. 查询基础

### 1.1 query 类型与列引用

ODB 为每个持久化对象生成一个 `query` 类型，其中包含所有列的引用：

```cpp
// ODB 编译器为 Player 自动生成（在 player-odb.hxx 中）：
// odb::query<Player>::id      → `player`.`id`
// odb::query<Player>::name    → `player`.`name`
// odb::query<Player>::level   → `player`.`level`
// odb::query<Player>::exp     → `player`.`exp`
// odb::query<Player>::gold    → `player`.`gold`
```

这些列引用是**编译期符号**，拼错会直接编译报错：

```cpp
#include "player-odb.hxx"

typedef odb::query<Player> query;

// ✅ 正确：level 是 Player 的成员
auto q = query::level > 50;

// ❌ 编译错误：levle 不存在
// auto q = query::levle > 50;
// error: 'levle' is not a member of 'odb::query<Player>'
```

### 1.2 基本查询与结果迭代

```cpp
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include "player.hxx"
#include "player-odb.hxx"

typedef odb::query<Player> query;
typedef odb::result<Player> result;

// 查询所有等级大于 50 的玩家
void findHighLevelPlayers(odb::database& db)
{
    odb::transaction t(db.begin());

    // db.query<T>(条件) 返回 result<T>
    result r(db.query<Player>(query::level > 50));

    // 方式 1：范围 for 迭代
    for (const Player& p : r)
    {
        std::cout << p.name() << " Lv." << p.level() << std::endl;
    }

    // 方式 2：迭代器
    // for (auto it = r.begin(); it != r.end(); ++it)
    // {
    //     std::cout << it->name() << std::endl;
    // }

    t.commit();
}
// 生成的 SQL:
// SELECT `id`,`name`,`level`,`exp`,`gold`,`created_at`
// FROM `player` WHERE `level` > ?
// 参数绑定: [50]
```

### 1.3 查询所有记录

```cpp
// 不传条件 = 查询全部
result r(db.query<Player>());
// SQL: SELECT ... FROM `player`

// 传 true 也行（语义更清晰）
result r(db.query<Player>(query::id != 0)); // WHERE id != 0（即全部）
```

---

## 2. 操作符与条件组合

### 2.1 比较操作符

ODB 重载了标准 C++ 操作符来构建查询条件：

```cpp
typedef odb::query<Player> Q;

// 等于
Q::name == "剑圣无名"          // WHERE name = '剑圣无名'

// 不等于
Q::level != 1                  // WHERE level != 1

// 大于/小于
Q::level > 50                  // WHERE level > 50
Q::gold >= 10000               // WHERE gold >= 10000

// NULL 检查（用 is_null / is_not_null）
Q::deletedAt.is_null()         // WHERE deleted_at IS NULL
Q::deletedAt.is_not_null()     // WHERE deleted_at IS NOT NULL
```

### 2.2 逻辑组合（AND / OR / NOT）

```cpp
typedef odb::query<Player> Q;

// AND：等级 > 50 且 金币 > 10000
Q::level > 50 && Q::gold > 10000
// WHERE `level` > ? AND `gold` > ?

// OR：名字是"剑圣无名"或者等级 >= 100
Q::name == "剑圣无名" || Q::level >= 100
// WHERE `name` = ? OR `level` >= ?

// NOT：不是 1 级小号
!(Q::level == 1)
// WHERE NOT (`level` = ?)

// 复合条件：活跃的高级玩家
(Q::level > 50 && Q::gold > 10000) || Q::name == "GM管理员"
// WHERE (`level` > ? AND `gold` > ?) OR `name` = ?
```

### 2.3 字符串操作

```cpp
typedef odb::query<Player> Q;

// LIKE 模糊查询：搜索名字包含"剑"的玩家
Q::name.like("%剑%")
// WHERE `name` LIKE '%剑%'

// LIKE 带转义：搜索包含下划线的名字
Q::name.like("%\\_%")
// WHERE `name` LIKE '%\_%'
```

### 2.4 IN 查询

```cpp
typedef odb::query<Player> Q;

// IN 列表：查找特定 ID 的玩家
std::vector<uint64_t> ids = {1001, 1002, 1005, 1010};
Q::id.in_range(ids.begin(), ids.end())
// WHERE `id` IN (?, ?, ?, ?)
// 参数绑定: [1001, 1002, 1005, 1010]
```

---

## 3. 游戏场景实战查询

### 3.1 等级排行榜 Top 100

```cpp
typedef odb::query<Player> Q;
typedef odb::result<Player> R;

std::vector<Player> getLeaderboard(odb::database& db, int topN)
{
    odb::transaction t(db.begin());

    // ORDER BY level DESC, exp DESC + LIMIT
    R r(db.query<Player>(
        "ORDER BY" + Q::level + "DESC," + Q::exp + "DESC"
        + "LIMIT" + Q::_val(topN)
    ));

    std::vector<Player> leaderboard;
    for (const Player& p : r)
    {
        leaderboard.push_back(p);
    }

    t.commit();
    return leaderboard;
}
// 生成的 SQL:
// SELECT ... FROM `player`
// ORDER BY `level` DESC, `exp` DESC LIMIT 100
```

**解读**：
- `"ORDER BY" + Q::level` 将字符串片段和列引用拼接
- `Q::_val(topN)` 将 C++ 变量作为参数绑定（不是字符串拼接！防注入）
- 排行榜按等级降序，同等级按经验降序

### 3.2 按品质筛选道具

```cpp
typedef odb::query<InventoryItem> Q;
typedef odb::result<InventoryItem> R;

// 查找某玩家背包中所有史诗品质以上的装备
std::vector<InventoryItem> findEpicItems(
    odb::database& db, uint64_t playerId)
{
    odb::transaction t(db.begin());

    // 假设 quality 字段：1=白,2=绿,3=蓝,4=紫(史诗),5=橙(传说)
    R r(db.query<InventoryItem>(
        Q::owner == playerId &&
        Q::quality >= 4
    ));

    std::vector<InventoryItem> items;
    for (const InventoryItem& item : r)
    {
        items.push_back(item);
    }

    t.commit();
    return items;
}
// 生成的 SQL:
// SELECT ... FROM `inventory`
// WHERE `owner` = ? AND `quality` >= ?
// 参数绑定: [playerId, 4]
```

### 3.3 模糊搜索玩家昵称

```cpp
typedef odb::query<Player> Q;
typedef odb::result<Player> R;

// 搜索昵称包含关键词的玩家（最多返回 20 个）
std::vector<Player> searchPlayerByName(
    odb::database& db, const std::string& keyword)
{
    odb::transaction t(db.begin());

    std::string pattern = "%" + keyword + "%";

    R r(db.query<Player>(
        Q::name.like(pattern)
        + "ORDER BY" + Q::level + "DESC"
        + "LIMIT" + Q::_val(20)
    ));

    std::vector<Player> results;
    for (const Player& p : r)
    {
        results.push_back(p);
    }

    t.commit();
    return results;
}
// 生成的 SQL:
// SELECT ... FROM `player`
// WHERE `name` LIKE ? ORDER BY `level` DESC LIMIT 20
// 参数绑定: ["%剑%", 20]
```

### 3.4 分页查询

```cpp
struct PageResult
{
    std::vector<Player> items;
    int totalCount;
};

PageResult getPlayerPage(odb::database& db, int page, int pageSize)
{
    odb::transaction t(db.begin());

    PageResult result;

    // 查询总数（使用 View 更高效，这里先用 query 演示）
    int offset = (page - 1) * pageSize;

    // 分页数据
    typedef odb::query<Player> Q;
    auto r = db.query<Player>(
        "ORDER BY" + Q::id + "ASC"
        + "LIMIT" + Q::_val(pageSize)
        + "OFFSET" + Q::_val(offset)
    );

    for (const Player& p : r)
    {
        result.items.push_back(p);
    }

    t.commit();
    return result;
}
// 生成的 SQL:
// SELECT ... FROM `player` ORDER BY `id` ASC LIMIT ? OFFSET ?
// 参数绑定: [20, 40]  （第3页，每页20条 → offset=40）
```

### 3.5 多条件复合查询：交易日志审计

```cpp
typedef odb::query<TradeLog> Q;
typedef odb::result<TradeLog> R;

// 查询某玩家在指定时间范围内的高额交易
R queryLargeTransactions(odb::database& db, uint64_t playerId,
                         const std::string& startTime,
                         const std::string& endTime,
                         uint64_t minAmount)
{
    odb::transaction t(db.begin());

    R r(db.query<TradeLog>(
        Q::playerId == playerId &&
        Q::createdAt >= startTime &&
        Q::createdAt <= endTime &&
        Q::amount >= minAmount
        + "ORDER BY" + Q::amount + "DESC"
    ));

    for (const TradeLog& log : r)
    {
        std::cout << "[" << log.createdAt() << "] "
                  << log.action() << " 金额:" << log.amount()
                  << std::endl;
    }

    t.commit();
    return r;
}
// 生成的 SQL:
// SELECT ... FROM `trade_log`
// WHERE `player_id` = ? AND `created_at` >= ? AND `created_at` <= ?
//   AND `amount` >= ?
// ORDER BY `amount` DESC
```

---

## 4. 动态查询构建

实际游戏中，查询条件往往是动态的——玩家可以选择性地筛选。

### 4.1 条件拼接

```cpp
typedef odb::query<Player> Q;

// 动态查询构建器：GM 后台玩家搜索
odb::result<Player> searchPlayers(
    odb::database& db,
    const std::string& name,       // 可选：昵称关键词
    int minLevel,                   // 可选：最低等级（0=不限）
    int maxLevel,                   // 可选：最高等级（0=不限）
    uint64_t minGold)               // 可选：最低金币（0=不限）
{
    odb::transaction t(db.begin());

    // 从 true 开始，逐步追加 AND 条件
    Q q(true);  // WHERE true（等价于无条件）

    if (!name.empty())
        q = q && Q::name.like("%" + name + "%");

    if (minLevel > 0)
        q = q && Q::level >= minLevel;

    if (maxLevel > 0)
        q = q && Q::level <= maxLevel;

    if (minGold > 0)
        q = q && Q::gold >= minGold;

    // 追加排序
    q = q + "ORDER BY" + Q::level + "DESC LIMIT 100";

    auto r = db.query<Player>(q);

    t.commit();
    return r;
}
```

### 4.2 使用示例

```cpp
// GM 搜索：等级 50-100 的土豪玩家
auto r1 = searchPlayers(db, "", 50, 100, 100000);
// SQL: WHERE level >= 50 AND level <= 100 AND gold >= 100000

// GM 搜索：名字含"剑"的玩家
auto r2 = searchPlayers(db, "剑", 0, 0, 0);
// SQL: WHERE name LIKE '%剑%'

// GM 搜索：无筛选条件（返回前 100 名）
auto r3 = searchPlayers(db, "", 0, 0, 0);
// SQL: （无 WHERE 子句）ORDER BY level DESC LIMIT 100
```

---

## 5. 原生 SQL 与混合模式

### 5.1 何时需要原生 SQL

ODB Query Language 覆盖了大部分常见查询，但某些场景仍需要原生 SQL：

| 场景           | 原因                                  |
| -------------- | ------------------------------------- |
| 复杂多表 JOIN  | ODB 查询以单对象为主                  |
| MySQL 特有函数 | `UNIX_TIMESTAMP()`、`MATCH...AGAINST` |
| 子查询         | ODB 不直接支持                        |
| 存储过程调用   | 超出 ORM 范畴                         |
| 极端性能优化   | 需要完全控制 SQL                      |

### 5.2 原生查询语法

```cpp
typedef odb::query<Player> Q;

// 方式 1：纯原生 SQL 条件
auto r = db.query<Player>(
    Q("level > 50 AND gold > 10000 ORDER BY level DESC")
);
// SQL: SELECT ... FROM player WHERE level > 50 AND gold > 10000 ...

// 方式 2：原生 SQL + 参数绑定（防注入）
int minLevel = 50;
auto r = db.query<Player>(
    Q("level > " + Q::_val(minLevel) + " AND name != " + Q::_val("GM"))
);
// SQL: SELECT ... FROM player WHERE level > ? AND name != ?
// 参数绑定: [50, "GM"]

// 方式 3：混合模式（ODB 列引用 + 原生片段）
auto r = db.query<Player>(
    Q::level > 50 &&
    Q("UNIX_TIMESTAMP(created_at) > UNIX_TIMESTAMP(NOW()) - 86400")
);
// 查找：等级>50 且 24小时内注册的玩家
```

### 5.3 原生 SQL 执行（非对象查询）

```cpp
// 直接执行 SQL（不返回对象，用于 DDL/DML）
db.execute("CREATE INDEX idx_player_level ON player(level)");

db.execute("UPDATE player SET gold = gold * 2 WHERE level >= 50");
// 全服 50 级以上玩家金币翻倍（运营活动）
```

---

## 6. 查询性能考量

### 6.1 查看生成的 SQL

调试时需要知道 ODB 实际生成了什么 SQL：

```cpp
// 方法 1：开启 MySQL 通用查询日志
// 在 my.cnf 中:
// [mysqld]
// general_log = 1
// general_log_file = /var/log/mysql/query.log

// 方法 2：ODB 的 tracer 机制
#include <odb/tracer.hxx>
#include <odb/mysql/tracer.hxx>

class SqlTracer : public odb::tracer
{
public:
    void execute(odb::connection&, const char* statement) override
    {
        std::cout << "[SQL] " << statement << std::endl;
    }
};

// 注册 tracer
SqlTracer tracer;
db.tracer(tracer);

// 之后所有操作都会打印 SQL
db.load<Player>(1001);
// 输出: [SQL] SELECT `id`,`name`,`level`... FROM `player` WHERE `id`=?
```

### 6.2 预备语句自动缓存

ODB 默认使用预备语句（prepared statements），并自动缓存：

```
首次执行 query(Q::level > 50):
  1. PREPARE stmt FROM 'SELECT ... WHERE level > ?'    ← MySQL 编译 SQL
  2. EXECUTE stmt USING 50                              ← 绑定参数执行

第二次执行 query(Q::level > 80):
  1. （跳过 PREPARE，复用缓存的 stmt）
  2. EXECUTE stmt USING 80                              ← 只需绑定新参数
```

**好处**：
1. **防 SQL 注入** — 参数绑定，不存在字符串拼接
2. **性能复用** — MySQL 只编译一次 SQL，后续执行跳过解析和优化
3. **零额外配置** — ODB 自动管理，开发者无感

### 6.3 索引配合

查询性能离不开索引。结合 ODB pragma 配置索引：

```cpp
#pragma db object table("player")
class Player
{
    #pragma db id auto
    uint64_t id_{0};

    // 唯一索引：昵称唯一
    #pragma db type("VARCHAR(32)") not_null unique
    std::string name_;

    // 普通索引：排行榜按等级查询
    #pragma db not_null index
    int level_{1};

    // 复合索引：按等级+经验查排行榜
    #pragma db index("idx_level_exp") member(level_) member(exp_)

    uint64_t exp_{0};
    uint64_t gold_{0};
};
```

**索引选择建议**：

| 查询场景     | 建议索引                 | pragma                   |
| ------------ | ------------------------ | ------------------------ |
| 按昵称搜索   | name 唯一索引            | `unique`                 |
| 等级排行榜   | (level, exp) 复合索引    | `index("idx_level_exp")` |
| 背包查询     | inventory.owner 外键索引 | ODB 自动创建             |
| 交易日志审计 | (player_id, created_at)  | 复合索引                 |

---

## 7. query_one 与 query_value

### 7.1 查询单个对象

```cpp
typedef odb::query<Player> Q;

// query_one：最多返回一个对象（返回 unique_ptr）
// 0 条结果 → nullptr，1 条 → 对象指针，>1 条 → 异常
std::unique_ptr<Player> findByName(odb::database& db,
                                    const std::string& name)
{
    odb::transaction t(db.begin());

    auto player = db.query_one<Player>(Q::name == name);
    // 如果没找到：player == nullptr
    // 如果找到一个：player 指向该对象
    // 如果找到多个：抛 odb::result_not_one 异常

    t.commit();
    return player;
}
```

### 7.2 query_value

```cpp
// query_value：确定只有一个结果（直接返回对象，非指针）
// 0 条或 >1 条 → 都抛异常
Player getPlayerByName(odb::database& db, const std::string& name)
{
    odb::transaction t(db.begin());

    Player player = db.query_value<Player>(Q::name == name);
    // 没找到或找到多个 → 抛异常

    t.commit();
    return player;
}
```

| 方法            | 返回类型        | 0 条结果 | 1 条结果 | >1 条结果 |
| --------------- | --------------- | -------- | -------- | --------- |
| `query()`       | `result<T>`     | 空集合   | 1 元素   | N 元素    |
| `query_one()`   | `unique_ptr<T>` | nullptr  | 对象指针 | 异常      |
| `query_value()` | `T`             | 异常     | 对象     | 异常      |

---

## 设计要点

### 查询为何用操作符重载而非字符串 DSL？

对比三种查询 API 设计：

```cpp
// 方案 A：纯字符串（手写 SQL 的 ORM 包装）
db.query<Player>("level > 50 AND name = '剑圣'");
// ❌ 无编译期检查，level 拼错不报错
// ❌ 字符串拼接有注入风险

// 方案 B：字符串 DSL（类似 Django ORM）
db.query<Player>().filter("level__gt", 50).filter("name", "剑圣");
// ❌ 字段名仍是字符串
// ❌ 操作符是约定的后缀（__gt, __lte），不直观

// 方案 C：操作符重载（ODB 的选择）
db.query<Player>(Q::level > 50 && Q::name == "剑圣");
// ✅ Q::level 是编译期符号，拼错直接报错
// ✅ > 和 == 是 C++ 原生操作符，直观
// ✅ 参数自动走预备语句绑定，防注入
```

ODB 选择操作符重载的核心理由：

1. **编译期类型安全** — `Q::level > "abc"` 会编译报错（int 不能和 string 比较）
2. **IDE 友好** — 输入 `Q::` 后自动补全所有可用列名
3. **C++ 原生表达** — 不需要学新语法，`&&`/`||`/`>`/`==` 都是 C++ 程序员的肌肉记忆
4. **重构安全** — 重命名成员变量时，所有引用该列的查询编译都会失败，不会遗漏

### 预备语句自动化的设计

ODB 的预备语句缓存是**透明的**——开发者不需要手动 prepare/execute：

```
开发者视角:
  db.query<Player>(Q::level > 50);   // 只看到一行代码

ODB 内部:
  1. 检查语句缓存 → 未命中
  2. PREPARE stmt FROM 'SELECT ... WHERE level > ?'
  3. BIND 参数 [50]
  4. EXECUTE stmt
  5. 将 stmt 存入缓存
  
  下次 db.query<Player>(Q::level > 80):
  1. 检查语句缓存 → 命中（同结构查询）
  2. BIND 参数 [80]
  3. EXECUTE stmt
```

这个设计解决了两个问题：
- **防 SQL 注入**：参数永远是绑定的，不会拼进 SQL 字符串
- **性能**：MySQL 只解析一次 SQL，后续执行跳过 parse → optimize 阶段

### 与 LINQ（C#）、QueryDSL（Java）的对比

| 特性     | ODB Query          | C# LINQ      | Java QueryDSL     |
| -------- | ------------------ | ------------ | ----------------- |
| 查询构建 | 操作符重载         | 语言内置语法 | 流式 API          |
| 类型安全 | ✅ 编译期           | ✅ 编译期     | ✅ 编译期          |
| 代码生成 | 外部编译器         | 编译器内置   | 注解处理器（APT） |
| 延迟执行 | ❌ 立即执行         | ✅ IQueryable | ✅                 |
| 子查询   | 有限（需原生 SQL） | ✅ 完整支持   | ✅ 完整支持        |
| 跨数据库 | ✅                  | ✅            | ✅                 |

ODB 的查询能力在子查询和复杂 JOIN 上不如 LINQ/QueryDSL 完整，但对于游戏服务器的典型查询场景（单表条件查询、排序分页、按外键关联查询）已经足够。真正需要复杂查询时，混合原生 SQL 即可。

---

## 小结

本篇我们掌握了 ODB 的查询系统：

1. **列引用** — `query::level` 是编译期符号，拼错即报错
2. **操作符重载** — `>`/`<`/`==`/`&&`/`||` 构建查询条件，直观且类型安全
3. **字符串操作** — `like()` 模糊查询、`in_range()` 范围查询
4. **动态构建** — 从 `true` 开始逐步追加条件，应对不确定的筛选
5. **原生 SQL** — 复杂场景的逃生舱，可混合使用
6. **性能** — 预备语句自动缓存，索引配合 pragma 配置

**下一篇**我们将学习继承映射与 View——道具系统中武器、防具、消耗品共享 Item 基类，ODB 提供三种继承策略来映射这个体系。同时，View 让你无需加载完整对象就能获取聚合统计，是排行榜和报表的利器。

---

> **系列导航**：[编译器与注解]({{< relref "深入学习ODB_编译器与注解.md" >}}) | [连接与事务]({{< relref "深入学习ODB_连接与事务.md" >}}) | [对象关系]({{< relref "深入学习ODB_对象关系.md" >}}) | **类型安全查询**（本文） | [继承与视图]({{< relref "深入学习ODB_继承与视图.md" >}}) | [迁移与多库]({{< relref "深入学习ODB_迁移与多库.md" >}}) | [性能与实践]({{< relref "深入学习ODB_性能与实践.md" >}}) | [实战项目]({{< relref "深入学习ODB_实战项目.md" >}})
