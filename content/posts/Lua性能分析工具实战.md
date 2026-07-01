+++
title = 'Lua 性能分析工具实战：从零搭建你的 Profiling 工具箱'
date = '2022-09-15'
draft = false
tags = ["Lua", "性能分析", "Profiling", "采样", "插桩", "游戏服务器"]
categories = ["Lua"]
description = "手把手教你搭建 Lua 性能分析工具链：从 os.clock 计时、debug.sethook 采样到 C 扩展 Profiler，覆盖开发期到线上的全场景排查方案。"
+++

# Lua 性能分析工具实战

> 适合读者：想在项目中定位 Lua 性能瓶颈、用数据说话的开发者
> 本文以 Lua 5.3/5.4 为基础，结合游戏服务器实际场景

---

## 第一章：为什么要做性能分析？

### 1.1 一个常见的困境

你写了一堆"Lua 性能优化技巧"文档，告诉你用 local、少创建 Table、重用对象……但是：

```
问题 1：线上服务器慢，但到底是 C++ 慢还是 Lua 慢？
问题 2：几个队友都说自己的代码"没问题"，如何客观地找到最慢的？
问题 3：优化后怎么"证明"确实变快了？
问题 4：上线前怎么防止新代码引入性能回退？
```

**答案**：不要靠猜，用工具说话。

### 1.2 性能分析金字塔

```
         ┌──────────┐
         │ 线上监控  │  ← 最慢但最真实
         ├──────────┤
         │ 压测场景  │  ← 模拟真实负载
         ├──────────┤
         │ 单元分析  │  ← 分析单个函数/模块
         ├──────────┤
         │ CPU Prof  │  ← 采样/插桩分析
         ├──────────┤
         │ 代码审查  │  ← 最便宜但最不准
         └──────────┘
```

本文从下往上，带你把每一层都跑通。


## 第二章：Lua 自带的计时——最简单的 Profiling

### 2.1 `os.clock` 基本计时

```lua
-- 最简单的基准测试工具：os.clock
-- 返回程序启动以来的 CPU 时间（秒）

local start = os.clock()

-- 待测代码
for i = 1, 1000000 do
    local x = math.sin(i)
end

local elapsed = os.clock() - start
print(string.format("耗时: %.3f 秒", elapsed))
```

**适用场景**：快速比较两种写法的性能差异。

```lua
-- 对比两种写法的性能
local ITERATIONS = 1000000

-- 写法 A：反复查 _ENV
local start = os.clock()
for i = 1, ITERATIONS do
    math.sin(i)
end
print("全局访问:", os.clock() - start)

-- 写法 B：local 缓存
local start = os.clock()
local sin = math.sin
for i = 1, ITERATIONS do
    sin(i)
end
print("local缓存:", os.clock() - start)

-- 输出示例：
-- 全局访问: 0.052 秒
-- local缓存: 0.031 秒
-- 差异：local 缓存快了约 40%
```

### 2.2 封装一个简单的计时器

```lua
-- benchmark.lua —— 轻量性能测试工具

local Benchmark = {}

function Benchmark.new(name)
    return setmetatable({
        name = name,
        start_time = nil,
        total_time = 0,
        call_count = 0,
        min_time = math.huge,
        max_time = 0
    }, {__index = Benchmark})
end

function Benchmark:start()
    self.start_time = os.clock()
end

function Benchmark:stop()
    local elapsed = os.clock() - self.start_time
    self.total_time = self.total_time + elapsed
    self.call_count = self.call_count + 1
    if elapsed < self.min_time then self.min_time = elapsed end
    if elapsed > self.max_time then self.max_time = elapsed end
end

function Benchmark:report()
    local avg = self.total_time / self.call_count
    print(string.format(
        "[%s] 调用 %d 次 | 总 %.3f秒 | 平均 %.6f秒 | 最小 %.6f | 最大 %.6f",
        self.name, self.call_count, self.total_time, avg,
        self.min_time, self.max_time
    ))
end

return Benchmark

-- 使用示例
-- local bench = Benchmark.new("战斗循环")
-- for i = 1, 100 do
--     bench:start()
--     DoBattle()
--     bench:stop()
-- end
-- bench:report()
```

### 2.3 服务器中的宏观性能监控

```lua
-- perf_monitor.lua —— 线上 Lua 性能监控器

local PerfMonitor = {}
PerfMonitor.__index = PerfMonitor

-- 每个监控项的模型
function PerfMonitor.new(name)
    return setmetatable({
        name = name,
        total_cost = 0,      -- 总耗时（毫秒）
        call_count = 0,      -- 调用次数
        max_cost = 0,        -- 单次最大耗时
        warning_threshold = 50,  -- 单次超过 50ms 报警
    }, PerfMonitor)
end

function PerfMonitor:record(cost_ms)
    self.total_cost = self.total_cost + cost_ms
    self.call_count = self.call_count + 1
    if cost_ms > self.max_cost then
        self.max_cost = cost_ms
    end
    if cost_ms > self.warning_threshold then
        LogWarn("[性能] %s 单次耗时 %.2fms，超过阈值 %dms",
                self.name, cost_ms, self.warning_threshold)
    end
end

function PerfMonitor:reset()
    self.total_cost = 0
    self.call_count = 0
    self.max_cost = 0
end

function PerfMonitor:report()
    if self.call_count == 0 then
        print(self.name, "无数据")
        return
    end
    local avg = self.total_cost / self.call_count
    print(string.format("[性能报告] %s: 调用%d次, 平均%.3fms, 最大%.2fms, 总%.2fms",
          self.name, self.call_count, avg, self.max_cost, self.total_cost))
end

-- 全局监控表
local monitors = {}

function StartMonitor(name)
    local m = PerfMonitor.new(name)
    monitors[name] = m
    return m
end

function GetMonitor(name)
    return monitors[name]
end

function ReportAll()
    for _, m in pairs(monitors) do
        m:report()
    end
end
```

```lua
-- 实际使用：配合钩子函数自动检测
-- 在服务器的 Lua tick 循环中

local UPDATE_MONITOR = StartMonitor("Tick_Update")
local BATTLE_MONITOR = StartMonitor("Tick_Battle")
local AI_MONITOR = StartMonitor("Tick_AI")

function OnServerTick(delta_ms)
    -- 更新系统
    local t0 = os.clock()
    UpdateSystem()
    UPDATE_MONITOR:record((os.clock() - t0) * 1000)
    
    -- 战斗系统
    local t1 = os.clock()
    BattleSystem()
    BATTLE_MONITOR:record((os.clock() - t1) * 1000)
    
    -- AI 系统
    local t2 = os.clock()
    AISystem()
    AI_MONITOR:record((os.clock() - t2) * 1000)
end

-- 定时输出报告（比如每 5 分钟）
function OnReportTimer()
    ReportAll()
    -- 重置，准备下一轮
    for _, m in pairs(monitors) do
        m:reset()
    end
end
```

**`os.clock` 的局限性**：
- 粒度不够细（秒级精度，Windows 上约 1ms）
- 只能测量整个函数，不能定位到函数内部的哪一行慢
- 不适合微秒级的热点分析
- 侵入式（要改代码）


## 第三章：Lua 调试库——更精细的 Profiling

### 3.1 `debug.sethook` 钩子函数

Lua 的 `debug` 库提供了一套钩子机制（Hook），可以在每执行一定数量的指令后触发回调。这是实现 Lua  Profiling 的核心工具：

```lua
-- debug.sethook 的基本用法
--    hook_callback: 每次触发时调用的函数
--    mask: "c"=函数调用、"r"=函数返回、"l"=每行
--    count: 每执行 N 条指令触发一次

-- 示例：统计每条 Lua 指令的执行次数
local counts = {}

function hook()
    -- debug.getinfo 获取当前调用栈信息
    local info = debug.getinfo(2, "nS")  -- 2=调用者
    if info and info.name then
        counts[info.name] = (counts[info.name] or 0) + 1
    end
end

-- 每执行 100 条指令触发一次 hook
debug.sethook(hook, "", 100)

-- 运行待测代码
DoSomething()

debug.sethook()  -- 关闭 hook

-- 输出结果
for name, count in pairs(counts) do
    print(name, count)
end
```

### 3.2 用 Hook 实现采样 Profiler

```lua
-- sample_profiler.lua —— 采样式 Profiler

local SampleProfiler = {}

function SampleProfiler.new()
    local self = {
        samples = {},      -- 采样样本
        sample_count = 0,  -- 总采样次数
        started = false,
        interval = 10,     -- 每 10 条指令采样一次
    }
    return setmetatable(self, {__index = SampleProfiler})
end

function SampleProfiler:start()
    if self.started then return end
    self.started = true
    self.samples = {}
    self.sample_count = 0
    
    -- 采样回调
    self.hook = function()
        local info = debug.getinfo(2, "nS")  -- 当前执行的函数
        if info then
            -- 记录函数名和所在行
            local key = string.format("%s:%d",
                info.source or "unknown",
                info.currentline or 0)
            self.samples[key] = (self.samples[key] or 0) + 1
            self.sample_count = self.sample_count + 1
        end
    end
    
    debug.sethook(self.hook, "", self.interval)
end

function SampleProfiler:stop()
    if not self.started then return end
    debug.sethook()  -- 关闭 hook
    self.started = false
end

function SampleProfiler:report(top_n)
    if self.sample_count == 0 then
        print("无采样数据")
        return
    end
    
    top_n = top_n or 20  -- 默认显示 Top20
    
    -- 排序
    local sorted = {}
    for key, count in pairs(self.samples) do
        table.insert(sorted, {key = key, count = count})
    end
    table.sort(sorted, function(a, b) return a.count > b.count end)
    
    print(string.format("\n===== Lua 采样 Profiling 报告 ====="))
    print(string.format("总采样次数: %d", self.sample_count))
    print(string.format("不同热点: %d 个", #sorted))
    print(string.format("%-40s %10s %8s", "热点位置", "次数", "占比"))
    print(string.rep("-", 62))
    
    for i = 1, math.min(top_n, #sorted) do
        local ratio = sorted[i].count / self.sample_count * 100
        print(string.format("%-40s %10d %7.2f%%",
            sorted[i].key, sorted[i].count, ratio))
    end
end

return SampleProfiler
```

```lua
-- 使用示例
local profiler = SampleProfiler.new()

profiler:start()

-- 运行要分析的代码
local sum = 0
for i = 1, 1000000 do
    sum = sum + math.sin(i) * math.cos(i)
end

profiler:stop()
profiler:report(10)

-- 输出示例：
-- ===== Lua 采样 Profiling 报告 =====
-- 总采样次数: 1225
-- 不同热点: 3 个
-- 热点位置                                    次数     占比
-- @test.lua:6                               1225   100.00%
```

**采样 Profiler 的优点**：
- 对代码影响小（每 N 条指令采一次，不是每一条）
- 不需要修改被分析代码
- 可以热插拔（任意时间 start/stop）

**缺点**：
- 统计性结果，不是精确的
- 采样间隔不够密可能会遗漏短时间但频繁执行的热点


## 第四章：函数级计时——插桩式 Profiling

### 4.1 自动包装函数

```lua
-- instrument.lua —— 插桩式性能分析工具

local Instrument = {}

-- 保存被装饰的函数
local instrumented = {}

function Instrument.wrap(func, name)
    local wrapped = function(...)
        local start = os.clock()
        local results = table.pack(pcall(func, ...))
        local elapsed = os.clock() - start
        
        -- 记录耗时
        if not instrumented[name] then
            instrumented[name] = {total = 0, count = 0, max = 0}
        end
        local record = instrumented[name]
        record.total = record.total + elapsed
        record.count = record.count + 1
        if elapsed > record.max then
            record.max = elapsed
        end
        
        if not results[1] then
            error(results[2])  -- 重新抛出 pcall 的错误
        end
        
        return table.unpack(results, 2, results.n)
    end
    
    return wrapped
end

function Instrument.report()
    print("\n===== 插桩 Profiling 报告 =====")
    local sorted = {}
    for name, record in pairs(instrumented) do
        table.insert(sorted, {
            name = name,
            total = record.total,
            count = record.count,
            avg = record.total / record.count,
            max = record.max
        })
    end
    table.sort(sorted, function(a, b) return a.total > b.total end)
    
    print(string.format("%-30s %8s %10s %10s %10s",
        "函数名", "调用次数", "总耗时(ms)", "平均(ms)", "最大(ms)"))
    print(string.rep("-", 72))
    for _, v in ipairs(sorted) do
        print(string.format("%-30s %8d %10.3f %10.6f %10.3f",
            v.name, v.count, v.total*1000, v.avg*1000, v.max*1000))
    end
end

function Instrument.reset()
    instrumented = {}
end

return Instrument
```

```lua
-- 使用方式：不需要改原函数
local instrument = require("instrument")

-- 原函数
function CalculateDamage(atk, def)
    -- 复杂计算
    local ratio = math.max(0, atk - def) / (atk + def)
    return atk * ratio
end

-- 插桩包装
CalculateDamage = instrument.wrap(CalculateDamage, "CalculateDamage")

-- 或者对模块中的所有函数批量包装
local SkillSystem = require("skill_system")
for name, func in pairs(SkillSystem) do
    if type(func) == "function" then
        SkillSystem[name] = instrument.wrap(func, "Skill." .. name)
    end
end

-- 正常使用
for i = 1, 1000 do
    local dmg = CalculateDamage(100, 50)
end

-- 输出报告
instrument.report()
```

### 4.2 非侵入式 Hook——注册表方法

更优雅的方式：不修改原函数引用，而是在 C 层面或 Lua 的 metatable 层面做 Hook：

```lua
-- hook_system.lua —— 非侵入式函数监控

local HookSystem = {}

-- 被 hook 的函数表
local hooks = {}

function HookSystem.hook_module(modname, filter)
    local mod = package.loaded[modname]
    if not mod then
        error("模块未加载: " .. modname)
    end
    
    local hooked = {}
    for name, func in pairs(mod) do
        if type(func) == "function" then
            -- 如果传了 filter，只 hook 匹配的函数
            if filter and not filter(name) then
                hooked[name] = func
            else
                hooked[name] = HookSystem.hook_func(func, modname .. "." .. name)
            end
        else
            hooked[name] = func
        end
    end
    
    package.loaded[modname] = hooked
end

function HookSystem.hook_func(func, label)
    local wrapper = function(...)
        local start = os.clock()
        local ok, result1, result2, result3 = pcall(func, ...)
        local elapsed = os.clock() - start
        
        if elapsed > 0.05 then  -- 超过 50ms 才记录
            LogWarn("[Hook] %s 耗时 %.3fms", label, elapsed * 1000)
        end
        
        if not ok then
            error(result1)
        end
        return result1, result2, result3
    end
    
    hooks[label] = wrapper
    return wrapper
end

function HookSystem.report()
    -- 输出所有 hook 统计
end

return HookSystem
```

```lua
-- 在服务器启动时 hook 所有战斗系统模块
local hook = require("hook_system")

hook.hook_module("battle.skill", function(name)
    return name:match("^[A-Z]")  -- 只 hook 大写开头的函数
end)

hook.hook_module("battle.buff")
hook.hook_module("battle.ai")
```

### 4.3 线上安全 Profiling 注意事项

```lua
-- 线上使用 hook 时必须注意性能影响！

-- ❌ 危险：线上全量 hook
-- 每次函数调用都触发 os.clock + pcall，性能下降可能 > 50%

-- ✅ 安全：线上只对可疑函数 hook
-- 比如发现某个模块变慢了，单独 hook 它

-- ✅ 安全：采样式 hook（配合 debug.sethook）
-- 每 N 条指令触发一次，影响小

-- ✅ 安全：使用阈值过滤
-- 只记录超过 50ms 以上的调用，忽略大多数正常调用

-- ✅ 安全：控制 hook 时长
-- 打开 hook → 收集 30 秒数据 → 自动关闭 → 输出报告
-- 不要让 hook 永远开着
```

```lua
-- 安全的线上 Profiling 流程
local profiler = require("sample_profiler")
local p = profiler.new()

-- 1. 线上开 hook（控制采样频率低一些）
p.interval = 1000  -- 每 1000 条指令采一次（影响极小）
p:start()

-- 2. 打日志
LogInfo("[Perf] 开启采样 Profiling，持续 30 秒...")

-- 3. 30 秒后自动关闭
ServerTimer.After(30, function()
    p:stop()
    p:report(30)  -- 输出 Top30 热点
    
    -- 如果怀疑某个热点函数，再单独插桩它
end)
```


## 第五章：Bytecode Profiling——深入 VM 层面

### 5.1 统计每字节码指令的执行频次

利用 `debug.sethook` 的 count 模式，可以统计哪种字节码指令最常被执行：

```lua
-- bytecode_stats.lua —— 字节码指令统计

local BytecodeStats = {}

function BytecodeStats.run(func, ...)
    local opcodes = {}
    local total = 0
    
    -- 这需要配合 C 扩展或在 Lua 5.4 中利用 debug 库
    -- 这里展示原理：利用 sethook 的 count 模式近似统计
    
    local hook = function()
        local info = debug.getinfo(2, "nS")
        if info and info.currentline then
            -- 实际上这里无法直接拿到字节码指令
            -- 真实实现需要 C 扩展 lib 来遍历 Proto 的字节码
            -- 这里只展示思路
        end
    end
    
    debug.sethook(hook, "", 1)  -- 每条指令触发
    func(...)
    debug.sethook()
end

return BytecodeStats
```

**实际项目中这个功能通常用 C 扩展实现**。比如 LuaJIT 提供了 `jit.util.funcinfo` 和 `jit.util.funcbc` 来查看字节码。

### 5.2 LuaJIT 的 Profiling 工具

如果你的项目用 LuaJIT，它自带一个强大的 Profiler：

```bash
# 命令行使用 LuaJIT Profiler
luajit -jv mycode.lua       # 显示 JIT 编译状态
luajit -jdump=ix mycode.lua  # dump 字节码和机器码
luajit -jp=m mycode.lua     # 按模块统计性能
```

```lua
-- 代码中控制 LuaJIT Profiler
jit.p = require("jit.profile")

-- 开启采样 Profiler（和 debug.sethook 原理不同，LuaJIT 的 Profiler 是基于信号的）
-- 每 100 微秒采样一次当前执行的函数
jit.p.start("100", function(thread, samples)
    -- 这个回调不能分配内存、不能创建 Table
    -- 只能做最简单的计数（用数组）
end)

-- ... 运行代码 ...

jit.p.stop()

-- 输出结果需要配合 jit.vmstate 和 dump 模块
-- 实际用法参考：http://luajit.org/running.html
```

### 5.3 字节码执行计数（C 扩展方式）

```c
// lua_profiler.c —— C 扩展字节码计数器
// 这只是一个展示原理的伪代码，项目中要实际实现需要：
// 1. 从 lua_State 获取当前执行的 Proto
// 2. 遍历 Proto 的字节码数组
// 3. 修改 VM dispatch 循环，插入计数

// 简化版本：不是每条指令计数，而是函数调用入口计数
static int l_count_func_calls(lua_State *L) {
    // 使用 sethook 的 "c" 模式——每次函数调用时触发
    lua_sethook(L, count_hook, LUA_MASKCALL, 0);
    return 0;
}

static void count_hook(lua_State *L, lua_Debug *ar) {
    lua_getinfo(L, "nS", ar);
    // 记录函数调用
    // ...
}
```


## 第六章：用 C++ 侧计时工具分析 Lua 函数调用

### 6.1 在绑定层植入计时

```cpp
// 在 C++ 绑定层自动为每个 Lua 函数调用加上计时

// 方案：修改 LuaBridge 的包装函数，在调用前后计时

// 简化的实现
int InstrumentedInvokeWrapper(lua_State *L) {
    auto func = /* 从 upvalue 获取 C++ 函数指针 */;
    
    // 获取当前函数名（从 Lua 调用栈）
    lua_Debug ar;
    lua_getstack(L, 1, &ar);
    lua_getinfo(L, "n", &ar);
    const char *funcName = ar.name ? ar.name : "unknown";
    
    // 计时开始
    auto start = std::chrono::high_resolution_clock::now();
    
    // 调用真正的函数
    int result = func(L);
    
    // 计时结束
    auto end = std::chrono::high_resolution_clock::now();
    auto cost_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    // 记录慢调用（超过阈值）
    constexpr int64_t WARN_THRESHOLD_US = 10000; // 10ms
    if (cost_us > WARN_THRESHOLD_US) {
        LogWarn("[LuaProfiler] %s 耗时 %lldus", funcName, cost_us);
    }
    
    return result;
}
```

### 6.2 利用项目已有的日志系统

很多项目已经有了 C++ 级别的日志系统，可以通过 Lua 的 `print` 或自定义日志函数来实现跨语言追踪：

```cpp
// Lua 侧注册的自定义日志函数
static int Lua_LogPerformance(lua_State *L) {
    const char *module = lua_tostring(L, 1);
    double cost_ms = lua_tonumber(L, 2);
    
    // 写入 C++ 侧的性能日志系统
    g_PerfLogger.Record(module, cost_ms);
    
    return 0;
}

// 注册到 Lua
lua_pushcfunction(L, Lua_LogPerformance);
lua_setglobal(L, "LogPerf");
```

```lua
-- Lua 侧的使用
local function ProfileFunction(name, func, ...)
    local start = os.clock()
    local results = table.pack(pcall(func, ...))
    local cost = (os.clock() - start) * 1000
    
    LogPerf(name, cost)  -- 交给 C++ 日志系统
    
    if not results[1] then
        error(results[2])
    end
    return table.unpack(results, 2, results.n)
end
```


## 第七章：实战——找出线上性能瓶颈

### 7.1 发现问题的典型流程

```
1. 服务器出现卡顿 / 帧率下降
       ↓
2. 查看性能日志（如果有宏观监控，先看哪个模块耗时最高）
       ↓
3. 如果怀疑是 Lua 层问题：
   a. 打开采样 Profiler，确定热点函数
   b. 对热点函数单独插桩计时
   c. 读代码确认问题原因
       ↓
4. 优化代码
       ↓
5. 重新 Profiling 确认优化效果
       ↓
6. 关闭所有 Profiling 工具，上线
```

### 7.2 案例：战斗卡顿排查

```lua
-- 问题：某副本战斗出现明显的卡顿

-- 第一步：宏观监控
-- 发现 Tick_Battle 平均耗时从 3ms 上升到 25ms

-- 第二步：采样 Profiling
-- 开启采样 10 秒
local profiler = require("sample_profiler")
local p = profiler.new()
p:start()

-- 让玩家打一次战斗
SimulateOneBattle()

p:stop()
p:report(10)

-- 输出：
-- ===== Lua 采样 Profiling 报告 =====
-- 总采样次数: 4220
-- @battle/skill.lua:154        38.2%    ← 技能伤害计算
-- @battle/buff.lua:203         22.5%    ← Buff 效果处理
-- @battle/ai.lua:89            15.3%    ← AI 行为树
-- @entity/player.lua:67         8.1%

-- 第三步：对嫌疑函数插桩
local skill = require("battle.skill")
local orig = skill.CalcDamage
skill.CalcDamage = function(...)
    local start = os.clock()
    local result = orig(...)
    local cost = (os.clock() - start) * 1000
    if cost > 1 then  -- 超过 1ms 的记录
        LogInfo("CalcDamage 耗时 %.3fms", cost)
    end
    return result
end

-- 再次战斗，发现 CalcDamage 中有一个表插入操作耗时异常
-- 检查代码发现：
-- function CalcDamage(atk, def)
--     local temp = {}
--     for i = 1, #atk.buffs do      ← 每次创建 Table
--         table.insert(temp, ...)    ← 这里频繁 rehash
--     end
-- end

-- 第四步：优化——复用 Table
-- 第五步：再次 Profiling 验证
-- 验证三次确认优化生效后，关闭所有 Hook
```

### 7.3 案例：GC 导致的卡顿排查

```lua
-- 问题：服务器每隔几十秒出现一次峰值卡顿

-- 排查方向：怀疑是 Lua GC 导致的
-- 因为 Lua GC 是标记-清除，会 Stop The World！

-- 第一步：确认 GC 行为
print("GC 当前参数:", collectgarbage("count"))  -- 当前内存(KB)
print("GC 步进模式:", collectgarbage("isrunning"))

-- 第二步：查看 GC 频率
-- 在每帧的末尾记录 GC 状态
local last_gc_count = 0
local last_gc_time = os.clock()

function OnFrameEnd()
    local current_gc = collectgarbage("count")
    local current_time = os.clock()
    
    if current_gc < last_gc_count * 0.8 then
        -- GC 回收了超过 20% 的内存，说明刚刚发生了 GC
        local gc_cost = current_time - last_gc_time
        LogInfo("[GC] 回收了 %.1fKB (%.2f%%), 距离上次 %.3f秒",
            last_gc_count - current_gc,
            (last_gc_count - current_gc) / last_gc_count * 100,
            gc_cost)
    end
    
    last_gc_count = current_gc
    last_gc_time = current_time
end

-- 第三步：如果确认是 GC 频繁卡顿
-- 调整 GC 参数：降低频率但增加单个 GC 步进的量
-- collectgarbage("setpause", 200)   -- GC 触发阈值（默认 200）
-- collectgarbage("setstepmul", 500) -- GC 步进速度（默认 200）

-- 第四步：减少 GC 压力（根源优化）
-- 减少临时 Table 创建
-- 使用对象池复用
-- 减少字符串拼接
```

### 7.4 性能分析工具的选择策略

```
你的目标                推荐工具                    精度    侵入性
───────────────────────────────────────────────────────────────
快速比较两种写法         os.clock / 计时函数          低     中
定位慢函数               debug.sethook 采样          中     低
分析前 N 个热点          采样 Profiler               中     低
详细分析单函数           插桩包装                    高     高
全局性能监控             线上监控计数器              低     极高（一直跑）
调优 GC                  collectgarbage + os.clock   中     低
确认优化效果             优化前后对比                 低     中
分析 JIT 效果            LuaJIT -jv/-jdump          高     无
```


## 第八章：总结

### 8.1 一句话记住

> **先采样定位热点，再插桩确认根因，用数据驱动优化，优化后重新验证。**

### 8.2 原则

1. **不要优化你不测量的东西**——性能问题大部分是少数热点导致的（二八原则）
2. **线上 profiling 要轻量**——采样控制频率、设置阈值、定时间关闭
3. **对比要有基线**——优化前后在相同条件下对比，排除偶然因素
4. **C++ 层的问题不要怪 Lua**——先确认是真的 Lua 慢，还是 C++ 逻辑本身慢

### 8.3 最终检查清单

```lua
-- [ ] 开发期跑过采样 Profiler？Top5 慢函数是否合理？
-- [ ] 热点函数是否有更高效的写法？（local 缓存、复用 Table、避免 rehash）
-- [ ] 跨语言调用次数是否过多？（考虑批量接口）
-- [ ] 是否有不必要的临时 Table/字符串创建？
-- [ ] 线上监控是否有性能告警？
-- [ ] GC 频率是否正常？是否调整过参数？
-- [ ] 优化后是否做了对比验证？
-- [ ] 新代码上线前是否跑过性能回归测试？
```
