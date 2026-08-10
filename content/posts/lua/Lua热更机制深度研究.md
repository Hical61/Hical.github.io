+++
title = 'Lua 热更机制深度研究：从底层原理到线上框架'
date = '2022-09-20'
draft = false
tags = ["Lua", "热更新", "Hotfix", "游戏服务器", "闭包", "Upvalue"]
categories = ["Lua"]
description = "深入 Lua 热更底层原理，从 require 缓存机制、闭包 upvalue 管理到完整的代理方案和服务器热更框架设计，全面掌握热更技术。"
+++

# Lua 热更机制深度研究

> 适合读者：需要处理 Lua 热更新逻辑的服务端/客户端开发者
> 本文以 **Lua 5.3/5.4** 为基础，结合游戏服务器实际场景

---

## 第一章：为什么要热更？

### 1.1 需求场景

MMORPG 服务器是 7×24 小时运行的，不能随便重启。但业务逻辑天天都要改：

```
周一：修复一个 BOSS 技能 bug
周二：调整副本掉落概率
周三：加一个限时活动
周四：修复玩家卡地图的 bug
周五：优化新手引导文本
...
```

如果每次改一行 Lua 就要重启服务器，所有在线玩家都要掉线重连，体验极差。

**热更（Hot Reload / Hotfix）** 就是：**在不重启进程的前提下，动态替换正在运行的代码**。

### 1.2 热更的技术挑战

听起来很美好，但有几个棘手的问题：

```
┌─────────────────────────────────────────────┐
│  Lua 热更要解决的 4 个核心问题               │
│                                              │
│  1. 代码替换：怎么用新代码替换旧代码？        │
│  2. 状态保持：旧函数持有的数据怎么办？         │
│  3. 持有引用：旧函数还被其他地方引用怎么办？   │
│  4. 安全性：热更到一半崩溃了怎么办？          │
└─────────────────────────────────────────────┘
```


## 第二章：Lua 热更的底层原理

### 2.1 重新理解 Lua 的"加载文件"过程

先看一个最基本的问题：**`require("xxx")` 到底做了什么？**

```c
// C 层面的简化流程
void luaL_require(lua_State *L, const char *modname) {
    // 1. 检查 package.loaded[modname] 是否存在
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    lua_getfield(L, -1, modname);
    
    if (!lua_isnil(L, -1)) {
        // 2. 如果已加载，直接返回缓存的结果
        return;  // 不会重新执行文件！
    }
    
    // 3. 如果没加载，才真正执行文件
    lua_pop(L, 1);  // 弹出 nil
    lua_pushvalue(L, ...);
    lua_call(L, ...);
    
    // 4. 把结果缓存到 package.loaded[modname]
    lua_setfield(L, -2, modname);
}
```

**关键点**：`require` 在第二次调用同一个模块时，**不会重新执行文件内容**，而是直接返回 `package.loaded` 里的缓存结果。

这就是热更的第一道坎：你不能简单地在游戏运行时再 `require` 一次来更新代码——它根本不会执行。

### 2.2 跳过缓存——强制重新加载

最简单的热更思路就是绕过这个缓存：

```lua
-- 最原始的热更方式：暴力绕过缓存
local function force_load(modname)
    package.loaded[modname] = nil  -- 清除缓存
    return require(modname)        -- 重新加载
end

-- 使用
force_load("system.skill")  -- 重新加载技能系统
```

**但是这样够了吗？远远不够。**

### 2.3 Chunk（函数原型）和闭包

Lua 中每个 `.lua` 文件被加载后，实际上变成了一个 **函数（chunk）**。这个函数里定义了其他函数，比如：

```lua
-- skill.lua
-- 这个文件加载后，整个文件内容变成一个大函数
-- 我们把它叫做 "chunk function"

-- 函数定义：实际上是在 chunk function 执行时创建闭包
local function calc_damage(a, b)  -- 创建闭包 #1
    return a * 2 - b
end

local function use_skill(skill_id)  -- 创建闭包 #2
    local cd = get_cd(skill_id)
    if cd > 0 then
        return false, "冷却中"
    end
    -- ... 施放技能逻辑
    return true
end

-- 导出
return {
    calc_damage = calc_damage,
    use_skill = use_skill,
}
```

执行 `require("skill")` 后：

```
package.loaded["skill"] = {
    calc_damage = <闭包#1 (L:3-5)>,
    use_skill   = <闭包#2 (L:7-13)>,
}
```

**热更的核心就是：创建一个新的闭包#1' 和 闭包#2'，替换掉旧的。**

但问题来了：如果某个地方已经在使用旧的闭包#1，比如已经把这个函数传给了一个回调，那新的闭包#1' 替换的是 `package.loaded["skill"]` 里的引用，**对方手里的旧引用还在**。

```lua
-- 战斗系统里这么用：
local skill_mod = require("skill")

-- 注册回调：这里拿到的是旧闭包！
fight_system.on_damage = function()
    local dmg = skill_mod.calc_damage(100, 50)
    -- ↑ 这个 skill_mod 引用了 package.loaded["skill"]
    --   而 calc_damage 字段指向旧的闭包
end

-- 热更后：
package.loaded["skill"].calc_damage 指向了新闭包
-- 但 fight_system.on_damage 里的 skill_mod 变量
-- 仍然指向旧的 package.loaded["skill"] 表
-- 所以调用的还是旧函数！
```


## 第三章：热更的方案设计

### 3.1 方案一：重新获取引用（最基础）

```lua
-- 最简单的做法：所有使用方都通过 package.loaded 获取模块
-- 不要缓存模块引用到局部变量里

-- ❌ 错误：缓存了模块引用
local skill_mod = require("skill")  -- 一旦缓存，热更无效

local function use_skill(skill_id)
    return skill_mod.use_skill(skill_id)  -- 永远调用旧函数
end

-- ✅ 正确：每次都通过 package.loaded 获取
local function get_skill_mod()
    return package.loaded["skill"]
end

local function use_skill(skill_id)
    return get_skill_mod().use_skill(skill_id)  -- 热更后拿到新函数
end
```

**缺点**：
- 每次调用都多一次 Table 查找
- 需要所有开发者遵守这个约定，很容易漏掉
- 无法处理已经传递出去的函数引用（回调函数）

### 3.2 方案二：用 Upvalue 实现函数转发（推荐）

```lua
-- skill_mod.lua
-- 思路：不直接导出函数，而是导出一个"可替换的实现"

-- 用一个表保存实现函数
local _impl = {}

-- 热更入口：替换实现
function _impl.calc_damage(a, b)
    return a * 2 - b
end

function _impl.use_skill(skill_id)
    -- 旧的实现
    return true
end

-- 导出的"壳函数"——不变，内部调用可替换的 _impl
local M = {}

-- 每个函数都是壳，转发到 _impl
M.calc_damage = function(...)
    return _impl.calc_damage(...)
end

M.use_skill = function(...)
    return _impl.use_skill(...)
end

-- 热更函数（对外暴露）
M.hotfix = function(new_impl)
    -- 只替换实现，不替换壳
    for name, func in pairs(new_impl) do
        if type(func) == "function" then
            _impl[name] = func  -- 替换实现
            print("[Hotfix] 替换函数:", name)
        end
    end
end

return M
```

外部使用时：

```lua
-- 其他模块使用
local skill = require("skill_mod")

-- 这里拿到的 calc_damage 是"壳函数"
-- 壳函数永远不变，但内部调用的 _impl 可以被替换
local dmg = skill.calc_damage(100, 50)

-- 热更时：
local new_code = require("hotfix_data.skill_mod")  -- 从热更数据模块加载新代码
skill.hotfix(new_code)
```

**底层的闭包结构**：

```
热更前：
    M.calc_damage = <壳闭包 0x1000>
        壳闭包的 upvalue[0] = _impl = { calc_damage = <旧实现 0x2000> }

热更后：
    _impl.calc_damage = <新实现 0x3000>
    
    壳闭包 0x1000 的 upvalue[0] 仍然是 _impl
    但 _impl.calc_damage 已经指向 0x3000 了
    所以 M.calc_damage(...) 调用的是新实现！
```

**优点**：
- 所有外部引用（壳函数）不会变，一直有效
- 只需要替换 upvalue 表中的函数字段，引用关系不变

### 3.3 方案三：利用元表进行代理（更通用）

```lua
-- hotfix_proxy.lua —— 通用的热更代理方案

-- 热更代理工具
local HotfixProxy = {}

function HotfixProxy.new(modname, initial_impl)
    -- 创建一个代理表，用 __index 转发方法调用
    local proxy = {}
    
    -- 保存当前实现
    local current_impl = initial_impl or {}
    
    -- 代理表：读取任何字段都从 current_impl 获取
    setmetatable(proxy, {
        __index = function(t, key)
            return current_impl[key]  -- 从当前实现读取
        end,
        __newindex = function(t, key, value)
            -- 通常不允许直接写，以防绕过热更
            error("不能直接修改代理表，请用 hotfix()")
        end
    })
    
    -- 注册到 package.loaded
    package.loaded[modname] = proxy
    
    -- 热更方法
    function proxy.hotfix(new_impl)
        for k, v in pairs(new_impl) do
            if type(v) == "function" then
                current_impl[k] = v  -- 替换实现
                print("[Hotfix Proxy] 替换", modname, ":", k)
            end
        end
    end
    
    -- 获取当前实现的原始引用（供调试用）
    function proxy.get_impl()
        return current_impl
    end
    
    return proxy
end

return HotfixProxy
```

使用方式：

```lua
-- skill.lua —— 被热更的模块
local proxy = require("hotfix_proxy")

-- 初始实现
local skill_impl = {
    calc_damage = function(self, a, b)
        return a * 2 + b
    end,

    use_skill = function(self, skill_id, player)
        print("使用技能:", skill_id)
        return true
    end
}

-- 创建代理对象
return proxy.new("skill", skill_impl)
```

```lua
-- 外部使用和其他模块完全一样
local skill = require("skill")
skill.calc_damage(100, 50)

-- 热更时（从热更配置或控制台触发）：
-- 加载新实现
local new_impl = {
    calc_damage = function(self, a, b)
        return a * 3 - b  -- 公式改了
    end
}
skill.hotfix(new_impl)  -- 一行代码完成热更！
```

**原理图**：

```
                  ┌──────────────────┐
    其他模块 ──→  │ package.loaded   │
                  │   ["skill"]      │
                  │   = 代理表        │
                  └────────┬─────────┘
                           │ __index
                           ▼
                  ┌──────────────────┐
                  │ current_impl     │
                  │   calc_damage    │ ← 热更时替换这里的值
                  │   use_skill      │
                  └──────────────────┘
```

### 3.4 方案四：服务器全量热更框架

在实际项目中，热更不是某一个模块的事，而是一个**完整的流程**：

```lua
-- hotfix_manager.lua — 游戏服务器热更管理器

local HotfixManager = {}

-- 热更流程状态
local STATE = {
    IDLE = 0,           -- 空闲
    LOADING = 1,         -- 正在加载新代码
    VERIFYING = 2,       -- 验证中
    COMMITTING = 3,      -- 提交中
    ROLLING_BACK = 4,    -- 回滚中
}

local state = STATE.IDLE

-- 保存旧实现的备份，用于回滚
local backups = {}
-- 保存热更日志
local hotfix_log = {}

-- 核心热更函数
function HotfixManager.hotfix(module_name, new_code)
    -- 1. 检查状态
    if state ~= STATE.IDLE then
        error("正在进行热更操作，请等待完成")
    end
    
    state = STATE.LOADING
    
    -- 2. 备份旧实现
    local old_impl = {}
    local mod = package.loaded[module_name]
    if mod and mod.get_impl then
        -- 对于使用代理方案的模块，备份 current_impl
        for k, v in pairs(mod.get_impl()) do
            if type(v) == "function" then
                old_impl[k] = v
            end
        end
    end
    
    -- 3. 验证新代码（语法检查）
    state = STATE.VERIFYING
    local ok, err = load(new_code, "hotfix_" .. module_name, "t")
    if not ok then
        state = STATE.IDLE
        error("热更代码语法错误: " .. tostring(err))
    end
    
    -- 4. 在沙箱环境中执行新代码，获取新实现
    local new_impl = nil
    local success, err = pcall(function()
        -- 使用安全环境执行热更代码
        -- 
        -- 这里需要设置沙箱环境，让热更代码只能访问有限的全局函数
        -- 在 Lua 5.1 中用 setfenv，在 Lua 5.3/5.4 中通过 _ENV upvalue 实现
        --
        -- 原理：每个 Lua 代码块（chunk）都有一个叫 _ENV 的隐式 upvalue
        -- 它指向全局环境表。正常情况下 _ENV = _G（全局表）
        -- 我们用一个受限的 env 表替换 _ENV，热更代码就只能访问 env 里的函数了
        --
        -- 实现方式：用 load() 的第三个参数设置自定义环境（Lua 5.3+ 支持）
        local env = {
            print = print,
            -- 只暴露必要的全局函数
            ipairs = ipairs,
            pairs = pairs,
            tonumber = tonumber,
            tostring = tostring,
            type = type,
            math = math,
            table = table,
            string = string,
            -- ... 按需添加其他安全函数
        }
        -- Lua 5.3+ 方式：load 的第四个参数就是环境表
        -- 等价于把热更代码的 _ENV upvalue 指向 env
        local loaded_func = load(new_code, "hotfix_" .. module_name, "t", env)
        if not loaded_func then
            error("热更代码加载失败")
        end
        new_impl = loaded_func()  -- 执行热更代码，获取返回值
    end)
    
    if not success or not new_impl then
        state = STATE.IDLE
        error("热更代码执行失败: " .. tostring(err))
    end
    
    -- 5. 提交热更
    state = STATE.COMMITTING
    
    if mod and mod.hotfix then
        -- 支持热更的模块
        local backup = {module = module_name, impl = old_impl}
        table.insert(backups, backup)
        mod.hotfix(new_impl)
    else
        -- 不支持热更的模块，使用强制替换
        -- 这可能导致旧引用失效，需要谨慎使用
        package.loaded[module_name] = nil
        local new_mod = require(module_name)
        -- 如果有全局引用也需要更新...
    end
    
    -- 6. 记录日志
    local log_entry = {
        time = os.time(),
        module = module_name,
        functions = {}
    }
    for name, _ in pairs(new_impl) do
        if type(new_impl[name]) == "function" then
            table.insert(log_entry.functions, name)
        end
    end
    table.insert(hotfix_log, log_entry)
    
    state = STATE.IDLE
    print(string.format("[HotfixManager] 热更成功: %s (%d 个函数)",
        module_name, #log_entry.functions))
    
    return true
end

-- 回滚上一次热更
function HotfixManager.rollback()
    if #backups == 0 then
        error("没有可回滚的热更记录")
    end
    
    local backup = table.remove(backups)
    local mod = package.loaded[backup.module]
    
    if mod and mod.hotfix then
        print("[HotfixManager] 回滚:", backup.module)
        mod.hotfix(backup.impl)  -- 用旧实现覆盖新实现
    else
        error("模块不支持回滚: " .. backup.module)
    end
end

-- 查看热更历史
function HotfixManager.get_history()
    return hotfix_log
end

return HotfixManager
```


## 第四章：热更实战中的陷阱与应对

### 4.1 陷阱一：函数闭包捕获的 Upvalue

```lua
-- 这是热更里最容易忽视的问题！

-- 场景：一个模块维护着状态数据
-- counter.lua
local count = 0  -- ← upvalue！被函数捕获
```

---

**补充：什么是 Upvalue？**

先看一个更简单的例子：

```lua
function make_counter()
    local count = 0     -- ← 这是局部变量
    return function()   -- ← 这是闭包（匿名函数）
        count = count + 1  -- ← 这里的 count 不是参数，不是局部变量，那是什么？
        return count
    end
end

local c1 = make_counter()
print(c1())  -- 1
print(c1())  -- 2
```

执行到 `count = count + 1` 时，`count` 既不是参数也不是局部变量——它是外层函数 `make_counter` 的局部变量。

通常情况下，函数返回后它的局部变量就被销毁了。但这里内层函数**捕获**了外层函数的局部变量，让它在函数返回后依然存活。

这个被捕获的变量就叫 **Upvalue**。

Lua 的底层实现中，每个闭包（函数对象）内部有一个 `upvalue` 数组：

```
闭包对象（内存中）：
┌─────────────────┐
│ 函数原型指针     │  ← 指向字节码指令序列
│ upvalue[0]       │──→ "count" 的引用（指向堆上分配的一块内存）
│ upvalue[1]       │──→ _ENV（全局环境）
│ ...              │
└─────────────────┘
```

回到热更的问题：

```lua
-- 热更前：
-- counter.lua
local count = 0

local function increment()
    count = count + 1  -- 闭包 increment 的 upvalue[0] = count
    return count
end
```

热更后，新代码的 `local count = 0` 创建了**一个新的局部变量**，新的 `increment` 闭包的 upvalue[0] 指向这个**新变量**。但旧闭包还握着旧 `count` 的引用——这就是 Upvalue 丢失。

```lua
-- 热更 counter.lua，把 increment 改成加 2

local function increment()
    count = count + 1
    return count
end

local function get_count()
    return count
end

return {
    increment = increment,
    get_count = get_count
}
```

问题出在哪？执行热更后：

```lua
-- 热更 counter.lua，把 increment 改成加 2
-- 新代码：
-- local count = 0  -- ← 这条语句在热更时会被执行！
-- 
-- function increment()
--     count = count + 2
--     return count
-- end

-- 热更后：
-- 新 increment 函数的 upvalue 是新的 count 变量（从新代码的 local count = 0 而来）
-- 旧 increment 函数的 upvalue 是旧的 count 变量
-- 两个 count 互不相干！
```

这就是 **Upvalue 丢失** 问题。热更后的新闭包绑定了新创建的局部变量，和原来的状态完全割裂。

**解决方法一：不要使用模块级 upvalue 存状态**

```lua
-- ✅ 正确：状态存入 Table，函数通过 self 访问
local M = {}

M.count = 0  -- 存在 Table 里，不是 upvalue

function M.increment()
    M.count = M.count + 1
    return M.count
end

function M.get_count()
    return M.count
end

return M
```

**解决方法二：热更时传递旧状态**

```lua
-- 把旧状态的引用传给新实现
function M.hotfix(new_functions, old_state)
    for name, func in pairs(new_functions) do
        _impl[name] = func
    end
    -- 如果新实现需要旧状态，通过 old_state 获取
    return _impl  -- 返回当前实现，让新代码能访问旧数据
end
```

### 4.2 陷阱二：GC 与旧闭包

```lua
-- 热更后，旧函数没有被立即释放的问题
-- 场景：大量对象持有旧函数引用

-- 热更前，创建了 10000 个怪物，每个怪物持有技能回调
for i = 1, 10000 do
    local monster = create_monster()
    monster.on_skill = skill_mod.calc_damage  -- 每个怪物都持有一个旧闭包引用
end

-- 热更后：
skill_mod.calc_damage 被替换为新函数
-- 但是 10000 个怪物还持有旧闭包引用
-- 这些旧闭包无法被 GC 回收，直到怪物被销毁
-- 如果怪物一直存在，旧代码就一直驻留在内存！
```

**解决方案**：如果是转发方案，壳函数不变，不产生新闭包，不会有这个问题。这是推荐用转发/代理方案的重要原因之一。

### 4.3 陷阱三：热更后函数的 identity 变了

```lua
-- 某些代码依赖"同一个函数"的比较
-- 比如注册、反注册模式：

local handlers = {}

function register_handler(name, handler)
    handlers[name] = handler
end

function unregister_handler(name, handler)
    if handlers[name] == handler then  -- 比较函数引用是否相同！
        handlers[name] = nil
    end
end

-- 注册
register_handler("use_skill", skill.use_skill)

-- 热更后，skill.use_skill 指向了新闭包
-- 但 handler 表里存的是旧闭包

-- 反注册
unregister_handler("use_skill", skill.use_skill)  -- 不相等！反注册失败！
```

**解决方案**：
- 使用代理方案，函数引用不变
- 或者反注册时用名字而不是函数引用

### 4.4 陷阱四：热更状态管理——半热更状态

```lua
-- 热更过程中如果出现错误，模块可能处于"半热更"状态
-- 部分函数是新实现，部分还是旧实现

-- 错误的写法：
function M.hotfix(new_impl)
    for name, func in pairs(new_impl) do
        -- 逐个替换。如果第三个函数执行出错，前两个已经替换成功了！
        _impl[name] = func  
    end
end

-- 正确的写法：全部验证通过再一次性替换
function M.hotfix(new_impl)
    -- 先验证所有函数
    for name, func in pairs(new_impl) do
        assert(type(func) == "function", 
            string.format("热更 %s 不是函数", name))
    end
    
    -- 再一次性替换
    for name, func in pairs(new_impl) do
        _impl[name] = func
    end
    
    -- 只有全部替换成功才算完成
end
```


## 第五章：实战——定时热更与监控

### 5.1 热更目录轮询

线上服务器通过检测文件变化来自动热更：

```lua
-- hotfix_loader.lua —— 自动检测热更文件并应用

local HotfixManager = require("hotfix_manager")

-- 热更文件目录
local HOTFIX_DIR = "../Data/Lua/Hotfix/"

-- 记录已应用的热更文件
local applied = {}

-- 扫描并应用新的热更文件
function check_and_apply()
    -- 遍历热更目录
    local files = io.popen(string.format("dir %s*.lua /b", HOTFIX_DIR))
    if not files then return end
    
    for filename in files:lines() do
        if not applied[filename] then
            -- 读取热更文件内容
            local f = io.open(HOTFIX_DIR .. filename, "r")
            if f then
                local content = f:read("*all")
                f:close()
                
                -- 解析文件名获取模块名
                local modname = filename:gsub("%.lua$", "")
                
                -- 应用热更
                local ok, err = pcall(HotfixManager.hotfix, modname, content)
                if ok then
                    applied[filename] = true
                    print("[AutoHotfix] 已应用:", filename)
                else
                    print("[AutoHotfix] 失败:", filename, err)
                end
            end
        end
    end
end

-- 定时轮询（在服务器主循环中调用）
-- 比如每 5 秒执行一次
return {
    check = check_and_apply
}
```

### 5.2 Lua 5.4 的新特性对热更的影响

Lua 5.4 引入了几个新特性，对热更有影响：

```lua
-- 1. const 变量（不可重绑定）
-- 被声明为 <const> 的变量，热更时无法通过重新赋值替换
local function calc_damage(a, b)  <const>
    -- 这个函数引用不可变更
end
-- 但热更替换的是 _impl 表里的字段，不是 const 变量本身
-- 所以用代理方案不受影响

-- 2. to-be-closed 变量
-- 声明了 <close> 的变量在离开作用域时会触发 __close 元方法
-- 热更时需要确保旧闭包正确关闭，新闭包能接管状态
local f <close> = io.open("config.txt")
-- 热更期间要小心这类资源的生命周期

-- 3. 属性访问器（__index + __newindex 的改进）
-- 代理方案正好利用了元表，Lua 5.4 的优化让代理方案性能更好
```

### 5.3 热更风险等级评估

不是所有的热更都能做。在实际项目中建议分级：

```
风险等级  适用范围                    注意事项
──────────────────────────────────────────────────────────
★☆☆☆☆    纯数值调整                   直接热更，无风险
          如：掉落概率、技能系数

★★☆☆☆    纯逻辑修复                   做好验证，基本安全
          如：条件判断修正、日志优化

★★★☆☆    流程修改                     需要备份，准备好回滚
          如：技能流程调整、任务链修改

★★★★☆    状态数据结构变更              风险很大，需谨慎评估
          如：新增状态字段、修改旧数据结构

★★★★★    底层通信协议、网络包处理        不建议热更，需重启
          如：消息格式变更、协议号变化
```


## 第六章：总结

### 6.1 最佳实践总结

```
热更方案选择：
├── 小型工具模块 → 直接替换 package.loaded
├── 业务逻辑模块 → 代理方案（推荐）
├── 数值配置模块 → 重载 + 热更通知
└── 底层框架模块 → 不热更，走重启流程

热更安全检查清单：
[ ] 热更代码是否做了语法检查？
[ ] 模块状态是否在 Table 中而不是在 upvalue 中？
[ ] 是否备份了旧实现，支持回滚？
[ ] 是否避免了"半热更"状态？
[ ] 旧函数的引用持有者是否被妥善处理？
[ ] 是否记录了热更日志（谁、什么时候、改了啥）？
[ ] 热更失败时是否能优雅降级？
```

### 6.2 一句口诀

> **用代理、保状态、先验证、再替换、能回滚**

### 6.3 参考资料

- [Lua 5.4 参考手册 — 关于 require 的部分](https://www.lua.org/manual/5.4/manual.html#pdf-require)
- [Lua 源码中 liolib.c — 文件加载的实现](https://www.lua.org/source/5.4/liolib.c.html)
- [Programming in Lua 4th Ed. — Chapter 8 (Compilation, Execution, and Errors)](https://www.lua.org/pil/8.html)
