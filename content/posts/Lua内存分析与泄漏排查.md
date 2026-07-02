+++
title = 'Lua 内存泄漏排查实战：从 GC 原理到线上定位'
date = '2022-09-18'
draft = false
tags = ["Lua", "内存泄漏", "GC", "垃圾回收", "游戏服务器", "性能优化"]
categories = ["Lua"]
description = "从 Lua GC 工作原理解析出发，深入分析三种典型内存泄漏场景，并提供完整的线上定位方案和 GC 参数调优策略。"
+++

# Lua 内存分析与泄漏排查

> 适合读者：需要排查线上 Lua 内存泄漏、分析 GC 压力的开发者
> 本文以 Lua 5.3/5.4 为基础，结合游戏服务器实际场景

---

## 第一章：Lua 内存管理概览

### 1.1 Lua 的内存模型

Lua 的内存管理采用**自动垃圾回收（GC）**，开发者不需要手动分配/释放内存。所有对象（Table、Function、String、Userdata）都由 GC 统一管理。

```
Lua 进程内存分布：
┌─────────────────────────────────────┐
│ C 层面分配（lua_newstate）           │ ← Lua State 本身
├─────────────────────────────────────┤
│ Lua 栈（lua_stack）                  │ ← 每个 lua_State 的栈空间
├─────────────────────────────────────┤
│ GC 对象（可回收）                     │ ← Table, Function, String, Userdata
│   ├── Table                          │
│   ├── Function (闭包)                │
│   ├── String (短字符串/长字符串)      │
│   ├── Userdata                       │
│   └── Thread (协程)                  │
├─────────────────────────────────────┤
│ GC 元数据                            │ ← GC 链表的辅助结构
└─────────────────────────────────────┘
```

### 1.2 Lua GC 的工作方式

Lua 5.3/5.4 使用**三色标记-清除（Tri-color Mark-and-Sweep）** 算法，分步执行：

```
阶段一：标记（Mark）
  ┌──────────────────────────────────┐
  │ 从根集合开始遍历所有可达对象       │
  │ 根集合：                          │
  │   - 注册表（registry）            │
  │   - Lua 栈                        │
  │   - 全局变量（_G）                │
  │   - 正在执行的协程               │
  │  ├─ 白色（White）：不可达，待清理  │
  │  ├─ 灰色（Gray）：可达，子节点待扫  │
  │  └─ 黑色（Black）：可达，已完成扫  │
  └──────────────────────────────────┘

阶段二：清除（Sweep）
  ┌──────────────────────────────────┐
  │ 遍历所有 GC 对象链表              │
  │ - 白色对象：释放内存              │
  │ - 黑色对象：重置为白色（下一轮）   │
  └──────────────────────────────────┘
```

**为什么 GC 会导致卡顿？**

Lua 的 GC 是**增量式**的（Lua 5.1 之后），不是一次全量 STW：

```
Lua 5.0 及之前：STW（Stop The World）
  [程序运行] → [GC 全量标记+清除] → [程序运行]
                ↑ 这里会卡住所有业务！

Lua 5.1+（增量式）：
  程序[...GC步进...]程序[...GC步进...]程序[...GC步进...]程序
  每次步进只做一小部分 GC 工作，在程序执行间隙穿插

Lua 5.4（紧急GC）：
  新增 emergency GC，在内存分配失败时自动触发
  防止 OOM 崩溃
```

### 1.3 三个核心概念

```lua
-- 1. 可达性（Reachability）
-- 从根集合出发能访问到的对象 = 存活对象
-- 访问不到的对象 = 可回收

local obj = {name = "张三"}  -- obj 从根可达（在栈上）
obj = nil                     -- 现在这个 Table 不可达了，下次 GC 回收

-- 2. 循环引用（Circular Reference）
-- Lua GC 能否处理循环引用？
local a = {}
local b = {}
a.ref = b      -- a 引用 b
b.ref = a      -- b 引用 a（循环引用）

a = nil
b = nil
-- 虽然 a 和 b 互相引用，但它们都从根不可达
-- Lua GC 的标记-清除可以正确处理循环引用！
-- 不需要弱引用来打破循环（那是另外的场景）

-- 3. GC 根（Roots）
-- 以下几种对象是 GC 的"根"，永远不会被回收：
--   - 注册表（registry）：C 层面持有
--   - 全局环境（_G 表）
--   - Lua 栈上的值（正在执行的函数中的局部变量）
--   - 主线程（主协程）
```

### 1.4 查看当前内存状态

```lua
-- collectgarbage 是 Lua 的标准 GC 控制接口

-- 获取当前 Lua 内存占用（单位：KB）
local mem_kb = collectgarbage("count")
print(string.format("Lua 内存占用: %.2f MB", mem_kb / 1024))

-- 手动触发一次完整的 GC
collectgarbage("collect")

-- 获取 GC 后的内存（测真实用量）
collectgarbage("collect")
local real_mem = collectgarbage("count")
print(string.format("GC 后真实占用: %.2f MB", real_mem / 1024))

-- 查看 GC 步进参数
local pause = collectgarbage("setpause")  -- 实际是设置并返回旧值
-- 先获取当前参数（Lua 5.3 无直接 get 接口，可以用一个小技巧）
local old_pause = 200  -- 默认值
local old_stepmul = 200  -- 默认值
```

**在服务器中定期上报内存**：

```lua
-- 每 5 分钟上报一次 Lua 内存

local function ReportLuaMemory()
    collectgarbage("collect")  -- 先做一次 GC 再报，避免临时垃圾影响判断
    local mem = collectgarbage("count") / 1024  -- MB
    
    -- 写入 C++ 侧的监控系统
    CppReportMetric("lua_memory_mb", mem)
    
    -- 如果内存超过阈值，告警
    if mem > 500 then  -- 超过 500MB
        LogWarn("[LuaMem] Lua 内存过高: %.2f MB", mem)
    end
end

-- 注册定时器
ServerTimer.Repeat(300, ReportLuaMemory)  -- 每 5 分钟
```


## 第二章：内存泄漏——三个典型场景

### 2.1 什么是 Lua 内存泄漏？

**Lua 内存泄漏 = 对象从根可达（不会被 GC），但已经不会再被业务使用**。

和 C++ 不同，Lua 不会有"彻底没法访问的内存"。Lua 的"泄漏"是**逻辑泄漏**——你认为它应该被回收了，但实际上它还被人引用着。

### 2.2 场景一：全局表无限增长

```lua
-- 最常见的泄漏原因：用 Table 缓存了所有东西，但从来不清理

-- ❌ 泄漏版
local PlayerCache = {}  -- 全局表，永远不会被 GC

function OnPlayerLogin(playerId)
    -- 缓存玩家信息
    PlayerCache[playerId] = {
        loginTime = os.time(),
        data = LoadPlayerData(playerId)
    }
end

function OnPlayerLogout(playerId)
    -- 忘记清理！
    -- PlayerCache[playerId] = nil  -- 这行缺失！
end

-- 随着时间推移，PlayerCache 越来越大
-- 即使玩家再也不会登录，缓存依然占用内存

-- ✅ 修复：登出时清理
function OnPlayerLogout(playerId)
    PlayerCache[playerId] = nil  -- 释放引用，GC 可回收
end
```

**真实项目中的变种**：

```lua
-- 变种 1：Event 注册不注销
local EventHandlers = {}

function SubscribeEvent(eventId, handler)
    if not EventHandlers[eventId] then
        EventHandlers[eventId] = {}
    end
    table.insert(EventHandlers[eventId], handler)
    -- 如果 handler 捕获了大量外部变量（闭包），内存会被牢牢抓住
    -- 而且 eventId 永远不会从 EventHandlers 中删除！
end

-- 变种 2：玩家离线表的过期缓存
local PlayerCache = {}

function GetPlayerData(playerId)
    if not PlayerCache[playerId] then
        PlayerCache[playerId] = LoadFromDB(playerId)
    end
    return PlayerCache[playerId]
end
-- 这个缓存永远不会过期！离线玩家的数据一直占用内存

-- ✅ 改进：加过期时间或 LRU
local PlayerCache = {}
local CacheTimeout = {}  -- 记录每个缓存的过期时间

function GetPlayerData(playerId)
    local data = PlayerCache[playerId]
    if data and CacheTimeout[playerId] > os.time() then
        return data
    end
    -- 过期或不存在，重新加载
    PlayerCache[playerId] = LoadFromDB(playerId)
    CacheTimeout[playerId] = os.time() + 300  -- 5 分钟过期
    return PlayerCache[playerId]
end

-- 定期清理过期缓存
function CleanExpiredCache()
    local now = os.time()
    for playerId, expireTime in pairs(CacheTimeout) do
        if expireTime <= now then
            PlayerCache[playerId] = nil
            CacheTimeout[playerId] = nil
        end
    end
end

ServerTimer.Repeat(60, CleanExpiredCache)  -- 每分钟清理一次
```

### 2.3 场景二：闭包捕获——意料之外的引用

```lua
-- 闭包泄漏是 Lua 中最隐蔽的泄漏，因为你看不到"引用"

-- ❌ 泄漏版
function CreateLeak()
    -- 一个大 Table
    local bigData = {}
    for i = 1, 10000 do
        bigData[i] = "some_data_" .. i
    end
    
    -- 一个很小的回调函数
    local callback = function()
        -- 这里只用了 bigData 的一小部分
        print(bigData[1])  -- 只取第一个元素
    end
    
    -- 但闭包捕获了整个 bigData 的引用！
    -- 即使 bigData 出了作用域，
    -- 只要 callback 还被其他地方引用，bigData 就无法 GC
    
    return callback
end

-- 外部持有回调
local handler = CreateLeak()  -- bigData 无法被 GC
-- 即使 handler 只用了 bigData 的一个字段，
-- 但 Lua 的闭包捕获的是整个外部局部变量（不是变量的部分字段）

-- ✅ 修复：只暴露需要的数据
function CreateNoLeak()
    local first = "some_data_1"  -- 只捕获这个
    
    local callback = function()
        print(first)
    end
    
    return callback
end
```

**更隐蔽的例子**：

```lua
-- 服务器中常见：定时器闭包

-- ❌ 泄漏
function StartRepeatingTask(player)
    local pid = player.id
    local data = LoadHugeData(pid)  -- 大块数据
    
    -- 定时器回调捕获了 data
    -- 即使 player 下线了，只要定时器没取消，data 就在
    local timer = ServerTimer.Repeat(1, function()
        ProcessTask(data)  -- 捕获了 data 引用
    end)
end

-- ✅ 修复：定时器中不用闭包，用 ID 重新获取
function StartRepeatingTask(player)
    local pid = player.id
    
    local timer = ServerTimer.Repeat(1, function()
        local data = LoadHugeData(pid)  -- 每次重新加载
        -- 虽然会增加 DB 查询，但不泄漏
        ProcessTask(data)
    end)
end

-- ✅ 更好的修复：定时器带生命周期管理
function StartRepeatingTaskWithLifecycle(player)
    local timerId = "task_" .. player.id
    
    -- 在玩家离线时自动取消
    player.onLogout = function()
        ServerTimer.Cancel(timerId)
    end
    
    ServerTimer.Repeat(timerId, 1, function()
        -- ...
    end)
end
```

### 2.4 场景三：协程泄漏

```lua
-- Lua 协程会被 GC 回收吗？
-- 答案：只有挂起且不可达的协程才会被回收

-- ❌ 泄漏：永不结束的协程
function InfiniteCoroutine()
    while true do
        -- 没有 coroutine.yield()！
        -- 或者 yield 后再也没有 resume 它
    end
end

local co = coroutine.create(InfiniteCoroutine)
coroutine.resume(co)
-- co 现在处于 running 状态？还是 suspended？

-- 如果协程因为等待某个事件而被挂起：
co = coroutine.create(function()
    coroutine.yield()  -- 挂起等待
    -- 再也没有 resume 它
end)
coroutine.resume(co)

-- 此时 co 是 suspended 状态
-- 但如果你不再引用它：
co = nil
-- 这个协程对象就会在下次 GC 中被回收
-- 所以协程本身不会泄漏

-- 真正的问题：协程里捕获的变量！
-- 一个挂起的协程，它的整个栈帧都存活
-- 包括所有局部变量、upvalue

local co = coroutine.create(function()
    local hugeData = LoadHugeData()  -- 100MB
    -- 只要协程还挂在这里
    coroutine.yield()  -- hugeData 就不会被 GC
    -- 但如果协程再也不会被 resume，hugeData 就白占了
end)

-- 监控协程数量
local co_count = 0
local co_tracker = {}

function SafeCoroutine(func)
    local co = coroutine.create(func)
    co_count = co_count + 1
    co_tracker[co] = os.time()
    
    -- 用 metatable 自动追踪协程的状态
    -- 但 coroutine 没有 metatable 可以用
    
    return co
end

function CoroutineCleanup(co)
    co_tracker[co] = nil
    co_count = co_count - 1
end

-- 输出协程数量，如果长期增长就是泄漏
function ReportCoroutineCount()
    if co_count > 100 then  -- 超过 100 个协程是异常
        LogWarn("[LuaMem] 协程数量异常: %d", co_count)
    end
end
```


## 第三章：内存泄漏排查工具

### 3.1 基础工具一：增量法找泄漏

```lua
-- 原理：GC 后记录内存基线，跑一遍功能后再次 GC 对比

local function CheckLeak(label)
    collectgarbage("collect")  -- 强制 GC
    collectgarbage("collect")  -- 两次确保稳定
    local mem = collectgarbage("count")
    print(string.format("[%s] Lua 内存: %.2f KB", label, mem))
    return mem
end

-- 使用
local base = CheckLeak("基线")

-- 执行一次完整的业务操作
SimulateOnePlayerLogin()
SimulateOnePlayerBattle()
SimulateOnePlayerLogout()

local after = CheckLeak("操作后")

local diff = after - base
if diff > 10 then  -- 如果泄漏超过 10KB
    print("⚠ 疑似泄漏:", diff, "KB")
else
    print("✅ 正常")
end
```

### 3.2 基础工具二：遍历 GC 对象

Lua 5.2+ 提供了 `collectgarbage("count")` 获取内存总量，但没有标准 API 枚举所有 GC 对象。不过可以通过 debug 库配合 registry 来近似实现：

```lua
-- enum_objects.lua —— 枚举 Lua 中的所有对象类型

function CountGCResources()
    collectgarbage("collect")
    
    local stats = {
        table_count = 0,
        function_count = 0,
        string_count = 0,
        thread_count = 0,
        userdata_count = 0,
        total_mem_kb = collectgarbage("count"),
    }
    
    -- 遍历注册表（registry）可以访问到大多数"根"对象
    -- 但这不是完整的 GC 对象枚举
    -- 真实枚举需要 C 扩展
    
    return stats
end

-- 更实用的方法：枚举全局表 _G
function EnumerateGlobals()
    local results = {}
    for k, v in pairs(_G) do
        results[k] = {
            type = type(v),
            size = estimate_size(v)
        }
    end
    return results
end

-- 估算对象大小（粗略）
function estimate_size(obj, seen, depth, counted_strings)
	seen = seen or {}  -- 记录已访问过的 table
	counted_strings = counted_strings or {}  -- 全局字符串去重
	depth = depth or 0
	if depth > 128 then return 0 end -- 限制深度
	
    local obj_type = type(obj)
    if obj_type == "number" then return 0 end -- 数字不计，因为 Lua 常驻/共享
    if obj_type == "boolean" then return 0 end
    if obj_type == "string" then
		-- 字符串只算一次
        if counted_strings[obj] then return 0 end
        counted_strings[obj] = true
		return #obj + 56 -- 字符串本身 + 头部开销
	end  
	
    if obj_type == "table" then
		if seen[obj] then return 0 end  -- 已访问过，不再重复计算
        seen[obj] = true
		
        -- 递归估算 Table 大小
        local size = 80  -- Table 头部
        for k, v in pairs(obj) do
            size = size + estimate_size(k, seen, depth + 1, counted_strings) + estimate_size(v, seen, depth + 1, counted_strings)
        end
        return size
    end
    if obj_type == "function" then return 144 end  -- 闭包大小约
    if obj_type == "thread" then return 12288 end  -- 协程默认栈 ~12KB
    return 32
end
```

### 3.3 进阶：C 扩展 GC 枚举工具

实现一个完整的 GC 对象枚举需要 C 扩展：

```c
// lua_memdebug.c —— 枚举 Lua GC 对象
// 编译：gcc -shared -fPIC -o lua_memdebug.so lua_memdebug.c -llua

#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"  // 需要 Lua 内部头文件

// 遍历所有 GC 对象（需要访问 Lua 内部数据结构）
static int l_dump_gc_objects(lua_State *L) {
    global_State *g = G(L);
    
    int count_table = 0;
    int count_func = 0;
    int count_string = 0;
    int count_thread = 0;
    int total_size = 0;
    
    // 从 G(L)->allgc 链表开始遍历所有 GC 对象
    GCObject *obj = g->allgc;
    while (obj) {
        switch (obj->tt) {
            case LUA_TTABLE:
                count_table++;
                total_size += sizeof(Table);
                break;
            case LUA_TFUNCTION:
                count_func++;
                break;
            case LUA_TSTRING:
                count_string++;
                break;
            case LUA_TTHREAD:
                count_thread++;
                break;
        }
        obj = obj->next;
    }
    
    lua_pushnumber(L, count_table);
    lua_pushnumber(L, count_func);
    lua_pushnumber(L, count_string);
    lua_pushnumber(L, count_thread);
    return 4;
}

static const luaL_Reg memdebug_funcs[] = {
    {"dump_gc_objects", l_dump_gc_objects},
    {NULL, NULL}
};

int luaopen_memdebug(lua_State *L) {
    luaL_newlib(L, memdebug_funcs);
    return 1;
}
```

```lua
-- Lua 侧使用
local memdebug = require("memdebug")

local tables, funcs, strings, threads = memdebug.dump_gc_objects()
print(string.format(
    "Table: %d, Function: %d, String: %d, Thread: %d",
    tables, funcs, strings, threads
))
```

**不过这个方案依赖于 Lua 内部结构，和版本绑死**。更通用的方案是 Lua 5.4 的 `lua_getallocf` 和钩子机制。

### 3.4 终极方案：分析注册表（Registry）

所有 Lua 的"根"引用最终都会汇集到注册表。检查注册表可以发现一些常见的泄漏：

```lua
-- 注册表遍历器
function DumpRegistry()
    local registry = debug.getregistry()
    
    for k, v in pairs(registry) do
        local key_type = type(k)
        local val_type = type(v)
        
        -- 跳过 Lua 内部保留的键（以 _ 或 __ 开头）
        if type(k) == "string" and (k:sub(1,1) == "_" or k:sub(1,2) == "__") then
            -- 跳过内部键
        else
            -- 用户注册的键，值得关注
            print(string.format("Registry[%s] = %s", tostring(k), val_type))
        end
    end
end
```

**实际项目中，最有效的泄漏检测方式是**：

```
1. 定时输出 GC 对象数量统计（用 C 扩展）
2. 关注"持续增长"的对象数量（Table 数量、String 总大小）
3. 对新功能做前后对比（跑功能前后各 GC 一次，对比差异）
4. 线上设置报警阈值
```


## 第四章：字符串内存优化

### 4.1 Lua 字符串的内存模型

Lua 的字符串有一个**全局字符串表**（global string table），所有相同的字符串共享一份内存：

```lua
-- Lua 字符串是"内联的"（interned）
local s1 = "hello"
local s2 = "hello"

-- s1 和 s2 指向同一个字符串对象！
-- 不会重复分配内存

-- 测试
print(s1 == s2)  -- true（比较的是指针，不是内容）
```

```
字符串"hello"的内存：
┌──────────────────────┐
│  GC 头（16字节）      │ ← 所有 GC 对象都有
│  长度 = 5             │
│  哈希值 = 0xABCD      │
│  字符数据：hello\0     │ ← 6 字节（含结束符）
│  ...padding...        │ ← 对齐到 8 字节
└──────────────────────┘
总大小 ≈ 32 字节（固定的头部开销 + 字符串内容）
```

**关键优化点：字符串的总内存 = 头部开销（约 24~40 字节） + 字符串长度**。

短字符串的头部开销占比极大：

```lua
-- "a" 这个字符串：32 字节头部 + 2 字节（"a\0"）= 34 字节
-- 但实际有效数据只有 1 个字符！
-- 头部开销占了 94%！
```

### 4.2 字符串拼接陷阱

```lua
-- ❌ 慢且耗内存：反复拼接
local s = ""
for i = 1, 10000 do
    s = s .. tostring(i) .. ","  -- 每次拼接都创建一个新字符串！
end
-- 循环 10000 次，创建了 10000 个中间字符串对象
-- 内存峰值：约 (1+2+...+10000) * ~40 = ~2GB 的临时分配

-- ✅ 快且省内存：table.concat
local parts = {}
for i = 1, 10000 do
    parts[i] = tostring(i)
end
local s = table.concat(parts, ",")
-- 只创建了 10000 个 tostring 的字符串 + 1 次 final concat
-- 没有中间字符串！
```

**`table.concat` 为什么快？**

```
逐次拼接：
"1" → "1,2" → "1,2,3" → "1,2,3,4" ...
每次都要分配新内存、拷贝旧数据、释放旧串

table.concat 的原理：
先在 C 层计算所有字符串的总长度
一次性分配正好大小的内存
把所有字符串依次拷贝进去
只需要 2 次分配（计算长度的一次临时 + 最终字符串）
```

### 4.3 字符串作为键的影响

```lua
-- 字符串作为 Table 键时，引用的是同一个字符串对象
-- 不会额外占用字符串内存
local t = {}
t["hp"] = 100
t["mp"] = 200
t["atk"] = 50
-- "hp"/"mp"/"atk" 各有一个字符串对象
-- 即使有 10000 个玩家，每个玩家的属性表都用 "hp" 做键
-- "hp" 这个字符串在内存中只有一份！

-- 但注意动态字符串
local function SetAttr(player, attrName, value)
    player[attrName] = value  -- attrName 如果是动态拼出来的
end

-- 每次 SetAttr(t, "attr_" .. id, val) 都会创建一个新字符串
-- 而且这个字符串会被 Table 作为键保留
-- 如果键是动态创建的，而且数量无限增长……内存泄漏！
```

### 4.4 字符串优化实践

```lua
-- 实践 1：缓存频繁拼接的字符串
-- ❌ 差：每次调用都要拼字符串
local function AttrName(attrId)
    return "attr_" .. attrId  -- 每次都拼接新字符串
end

-- ✅ 好：缓存一份
local ATTR_NAMES = {}
for i = 1, 100 do
    ATTR_NAMES[i] = "attr_" .. i
end
local function AttrName(attrId)
    return ATTR_NAMES[attrId]
end

-- 实践 2：用整数 ID 代替字符串键
-- ❌ 差：字符串键
player["hp"] = 100
player["mp"] = 200

-- ✅ 好：整数常量键
local const = {
    HP = 1,
    MP = 2,
    ATK = 3,
}
player[const.HP] = 100
player[const.MP] = 200
-- 整数作为键不需要额外分配，查询也比字符串快

-- 实践 3：大量日志/调试字符串——线上关闭
-- ❌ 差：调试信息一直拼字符串
LogDebug(string.format("玩家 %d 坐标 (%d, %d)", pid, x, y))

-- ✅ 好：用条件编译或等级控制
if LOG_LEVEL <= LOG_DEBUG then
    LogDebug(string.format("玩家 %d 坐标 (%d, %d)", pid, x, y))
end
```


## 第五章：弱引用表——受控的缓存

### 5.1 弱引用的使用场景

弱引用表是 Lua 用来**避免泄漏的缓存**。当 key 或 value 没有其他地方引用时，GC 会自动从弱引用表中移除。

```lua
-- __mode 的取值：
-- "k"：key 是弱引用
-- "v"：value 是弱引用
-- "kv"：key 和 value 都是弱引用

-- 场景：缓存玩家的大块数据，但不阻止 GC
-- 如果玩家对象被其他逻辑释放了，缓存自动失效
local playerCache = setmetatable({}, {__mode = "k"})

function GetPlayerData(player)
    if not playerCache[player] then
        playerCache[player] = LoadPlayerDB(player.id)
    end
    return playerCache[player]
end

-- 当 player 被其他地方释放，且不再有其他引用
-- GC 会从 playerCache 中自动移除这条记录
-- 不需要手动清理！
```

### 5.2 弱引用表的 GC 行为

```lua
-- 弱引用表的清除发生在 GC 的"清除"阶段
-- 不是立即的，是下次 GC 运行时才生效

-- 示例：
local cache = setmetatable({}, {__mode = "v"})

do
    local bigData = {1, 2, 3, 4, 5}
    cache["key"] = bigData  -- value 是弱引用
end
-- bigData 超出作用域，无其他引用

print(#cache)  -- 1，还在！（还没 GC）

collectgarbage("collect")  -- 强制 GC

print(#cache)  -- 0，GC 后清除
```

**性能注意事项**：

```lua
-- 弱引用表不是免费的！
-- GC 在每次运行时需要额外遍历弱引用表
-- 如果弱引用表很大，GC 时间会明显增加

-- ❌ 不要把所有大表都设成弱引用
-- ✅ 只在明确需要"自动清理缓存"的场景使用

-- 建议：
-- 对象数量 < 1000：直接用弱引用表，简单安全
-- 对象数量 > 10000：考虑手动管理，避免增大 GC 压力
-- 频繁访问的场景：用强引用表 + 定时清理，避免 __mode 查找开销
```


## 第六章：定位线上泄漏——完整案例

### 6.1 问题发现

```
监控告警：服务器 Lua 内存在 2 小时内从 200MB 增长到 800MB
排查方向：内存泄漏
```

### 6.2 第一阶段：确认是泄漏还是 GC 来不及

```lua
-- 检查 GC 是否正常执行
-- 先强制 GC 一次，看内存是否下降

collectgarbage("collect")
local mem1 = collectgarbage("count")

-- 等待 5 秒（让业务代码产生临时对象）
ServerWait(5)

collectgarbage("collect")
local mem2 = collectgarbage("count")

if mem2 > mem1 * 1.1 then
    print("⚠ 强制 GC 后内存仍然增长，可能真的泄漏了")
else
    print("GC 能回收，可能是 GC 参数问题，调整步进参数即可")
end
```

### 6.3 第二阶段：定位泄漏点

```lua
-- 使用增量法定位泄漏模块

-- 第一步：记录模块加载前的基线
collectgarbage("collect")
local base_mem = collectgarbage("count")

-- 第二步：逐步执行怀疑的模块的操作
-- 执行 100 次玩家登录登出
for i = 1, 100 do
    SimulatePlayerLogin(i)
    SimulatePlayerLogout(i)
end

-- 第三步：检查泄漏
collectgarbage("collect")
local after_mem = collectgarbage("count")
print("泄漏:", after_mem - base_mem, "KB")

-- 如果泄漏 > 0，说明玩家登录登出逻辑中有泄漏
```

```lua
-- 更细粒度的排查：定位具体的函数

-- 包装法：对怀疑的函数做隔离测试
function MeasureFuncMemory(func, iterations, ...)
    -- GC 稳定基线
    collectgarbage("collect")
    collectgarbage("collect")
    local mem_before = collectgarbage("count")
    
    -- 运行函数多次
    for i = 1, iterations do
        func(...)
    end
    
    -- 再次 GC
    collectgarbage("collect")
    collectgarbage("collect")
    local mem_after = collectgarbage("count")
    
    local leak_per_call = (mem_after - mem_before) / iterations
    print(string.format("函数 %s: 每次调用泄漏 %.3f KB (跑 %d 次)",
        debug.getinfo(func, "n").name or "unknown",
        leak_per_call,
        iterations))
    
    return leak_per_call
end

-- 使用
MeasureFuncMemory(PlayerLogin, 1000, testPlayer)
MeasureFuncMemory(PlayerLogout, 1000, testPlayer)
MeasureFuncMemory(BattleStart, 1000, testRoom)
```

### 6.4 第三阶段：找出泄漏源的具体类型

```lua
-- 利用前面说的 C 扩展，或 debug 库做类型统计

-- 简易版：用 collectgarbage("count") 不断对比

-- 1. 统计 Table 的数量级（通过遍历 _G 和其他全局表）
local function CountGlobalTables()
    local count = 0
    local function countTables(t)
        count = count + 1
        for k, v in pairs(t) do
            if type(v) == "table" and count < 1000 then
                countTables(v)
            end
        end
    end
    countTables(_G)
    return count
end

-- 2. 观察特定表的大小变化
-- 如果怀疑是某个全局缓存出了问题
local function CheckGlobalTableSizes()
    local suspects = {
        "PlayerCache",
        "MonsterCache", 
        "EventHandler",
        "TimerPool",
    }
    for _, name in ipairs(suspects) do
        local tbl = _G[name]
        if tbl then
            local size = 0
            for _ in pairs(tbl) do
                size = size + 1
            end
            print(string.format("_G.%s = %d 条", name, size))
            
            -- 如果某种表的大小持续异常增长，就是泄漏点
        end
    end
end
```

### 6.5 实际排查的经验法则

```
排查顺序（从容易到困难）：

1. 检查全局 Table 大小 ✓         ← 这个最快，一行代码的事
2. 检查玩家登入登出的缓存清理    ← 最常见的原因
3. 检查定时器/回调注销         ← 第二常见
4. 检查 Event 注册/注销         ← 第三常见
5. 检查协程是否被挂起不回收      ← 比较少见
6. 检查闭包捕获的大变量         ← 最隐蔽，最难查
7. 用 C 扩展遍历 GC 对象链表     ← 终极手段
```


## 第七章：GC 参数调优

### 7.1 GC 参数的含义

```lua
-- collectgarbage("setpause", pause)
-- collectgarbage("setstepmul", stepmul)

-- pause：GC 触发阈值（默认 200）
-- 当 GC 估算的内存增长比率超过 pause/100 时触发 GC
-- 200 = 内存翻倍时触发 GC
-- 100 = 内存增长 100% 时触发（默认值翻倍时触发）
-- 数值越小，GC 越频繁

-- stepmul：GC 步进倍率（默认 200）
-- GC 每次步进会做多少工作
-- 200 = 按内存分配量的 200% 做 GC 工作
-- 数值越大，单次 GC 步进的处理量越大，但暂停时间也更长
```

### 7.2 不同场景的 GC 策略

```lua
-- 场景 1：内存充足，追求低延迟
-- 减少 GC 频率，接受偶尔的短暂停
collectgarbage("setpause", 300)   -- 内存增长 300% 才触发
collectgarbage("setstepmul", 150) -- 每次步进少做点

-- 场景 2：内存紧张，需要及时回收
collectgarbage("setpause", 100)   -- 内存翻倍就触发
collectgarbage("setstepmul", 400) -- 每次多回收点

-- 场景 3：副本/战场中（短暂的频繁分配）
-- 进入副本前手动 GC
function OnEnterInstance(instanceId)
    collectgarbage("collect")  -- 清理进入前的垃圾
    collectgarbage("setpause", 150)  -- 收紧策略
end

-- 出副本后恢复
function OnLeaveInstance(instanceId)
    collectgarbage("collect")  -- 清理副本中产生的垃圾
    collectgarbage("setpause", 200)  -- 恢复默认
end
```

### 7.3 GC 参数调优的验证方法

```lua
-- 调整前后对比 GC 暂停对帧率的影响

local frame_times = {}
local GC_STATS = {total_pause = 0, pause_count = 0}

function OnFrameEnd(frameCostMs)
    table.insert(frame_times, frameCostMs)
    
    -- 记录超过阈值的帧
    if frameCostMs > 50 then
        local memBefore = collectgarbage("count")
        collectgarbage("collect")
        local memAfter = collectgarbage("count")
        
        GC_STATS.total_pause = GC_STATS.total_pause + frameCostMs
        GC_STATS.pause_count = GC_STATS.pause_count + 1
        
        -- 记录这次卡顿是不是 GC 引起的
        local freedMem = memBefore - memAfter
        if freedMem > 1024 then  -- 释放了超过 1MB
            LogInfo("[GC] 卡顿 %.1fms, 释放 %.1fKB, 原因: GC",
                frameCostMs, freedMem)
        end
    end
    
    -- 每隔 1000 帧输出一次统计
    if #frame_times >= 1000 then
        local sum = 0
        for _, t in ipairs(frame_times) do
            sum = sum + t
        end
        print(string.format("平均帧耗时: %.3fms, GC 暂停次数: %d, GC 总耗时: %.1fms",
            sum / #frame_times,
            GC_STATS.pause_count,
            GC_STATS.total_pause))
        frame_times = {}
        GC_STATS = {total_pause = 0, pause_count = 0}
    end
end
```


## 第八章：总结

### 8.1 核心要点

```
Lua 内存管理的特点：
├── 自动 GC，不需要手动释放
├── 没有"野指针"问题（和 C++ 不同）
├── 循环引用能被 GC 正确处理
└── 但"逻辑泄漏"非常常见——你以为能回收，但实际上还有人引用

最常见的内存泄漏原因（按频率排序）：
1. 全局缓存不清理（玩家数据、事件回调、Timer）
2. 闭包捕获了大变量
3. 协程挂起不回收
4. 动态字符串作为 Table 键（无限增长）
```

### 8.2 检查清单

```lua
-- [ ] 所有全局 Table 是否有清理机制？
-- [ ] 缓存是否设置了过期时间？
-- [ ] 玩家离线时是否清理了所有引用？
-- [ ] Event/Timer 注册后是否及时注销？
-- [ ] 闭包中是否不小心捕获了大变量？
-- [ ] 协程是否正常结束并被回收？
-- [ ] 字符串拼接是否用了 table.concat？
-- [ ] 动态字符串键是否可控？
-- [ ] 弱引用表是否用得合理？
-- [ ] 线上是否有周期性内存告警？
```

### 8.3 一句口诀

> **缓存要有过期时间、闭包不要捕获大变量、登出要清理、字符串拼接用 concat**

### 8.4 参考资料

- [Lua 5.4 参考手册 — 垃圾回收](https://www.lua.org/manual/5.4/manual.html#2.5)
- [Lua 5.4 参考手册 — collectgarbage](https://www.lua.org/manual/5.4/manual.html#pdf-collectgarbage)
- [Programming in Lua 4th Ed. — Chapter 17 (Garbage Collection)](https://www.lua.org/pil/17.html)
