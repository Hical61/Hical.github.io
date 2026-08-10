+++
title = 'Lua 性能进阶：VM 原理、Table 布局优化与 JIT 编译'
date = '2022-09-10'
draft = false
tags = ["Lua", "性能优化", "LuaJIT", "VM", "Table", "JIT", "游戏服务器"]
categories = ["Lua"]
description = "深入 Lua VM 执行循环和 Table 内部布局，结合 LuaJIT Trace Compiler 原理，带你写出真正高性能的 Lua 代码。"
+++

# Lua 性能进阶：Lua VM 原理、Table 布局优化、JIT 编译优化

> 适合读者：有一定 Lua 使用经验，想深入理解底层原理并写出高性能 Lua 代码的开发者
> 本文以 **Lua 5.3** 为主（项目中的 Lua 5.4 大同小异），JIT 部分以 **LuaJIT** 为例


## 第一章：Lua VM 概览

### 1.1 Lua 是一门什么语言？

Lua 是一门**动态类型、寄存器式虚拟机**的语言。这个定义里最关键的是"寄存器式虚拟机"——它决定了 Lua 的执行模型。

对比一下两种虚拟机模型：

| 模型           | 代表        | 特点                                      |
| -------------- | ----------- | ----------------------------------------- |
| 栈式虚拟机     | JVM, Python | 大部分指令操作栈顶元素                    |
| 寄存器式虚拟机 | Lua, Dalvik | 指令直接操作虚拟寄存器（类似 CPU 寄存器） |

寄存器式的好处是什么？**一条指令能干更多事**。比如 `a = b + c`，Lua 编译器会生成一条 `ADD R(a) R(b) R(c)` 指令，直接完成；而栈式可能需要 `push b; push c; add; pop a` 四条指令。

### 1.2 Lua 程序是怎么跑起来的？

```
源代码 → 词法分析 → 语法分析 → 字节码生成 → VM 执行
```

我们写一段 Lua 代码，看看它编译成什么样子：

```lua
-- test.lua
function add(a, b)
    return a + b
end

local result = add(3, 5)
print(result)
```

用 `luac -l test.lua` 看它的字节码：

```
main <test.lua:0,0> (5 instructions)
0+ params, 3 slots, 2 upvalues, 1 local, 3 constants, 0 functions

    1   [1] CLOSURE         0       0       ; 将闭包放入 R(0)
    2   [1] SETTABUP        0 0     0       ; 设置 upvalue _ENV["add"] = R(0)
    3   [4] GETTABUP        0 0     0       ; R(0) = _ENV["add"]
    4   [4] LOADI           1       3       ; R(1) = 3
    5   [4] LOADI           2       5       ; R(2) = 5
    6   [4] CALL            0 3     2       ; R(0..2) = R(0)(R(1), R(2))
    7   [5] GETTABUP        1 0     1       ; R(1) = _ENV["print"]
    8   [5] MOVE            2       0       ; R(2) = R(0)
    9   [5] CALL            1 2     1       ; R(1) = R(1)(R(2))
   10   [5] RETURN          0       1       ; return

function <test.lua:1,3> (2 instructions)
2 params, 3 slots, 0 upvalues, 2 locals, 0 constants, 0 functions

    1   [2] ADD             2       0 1     ; R(2) = R(0) + R(1)
    2   [3] RETURN          2       2       ; return R(2), 2 个返回值
```

看到没？`a + b` 就一条 `ADD` 指令。R(0) 是第一个参数 a，R(1) 是第二个参数 b，结果放入 R(2)，然后 RETURN。

**关键点**：Lua VM 的指令编码格式是 **iABC**（三个操作数），每条指令 4 字节。比如 `ADD R(2) R(0) R(1)`，A=2, B=0, C=1。这种紧凑编码让 Lua 的指令序列非常小，执行效率很高。


### 1.3 VM 执行循环

Lua VM 的核心是个巨大的 `switch` 循环，伪代码如下：

```c
// Lua 源码 lvm.c 中 dispatch 循环的简化版
void luaV_execute(lua_State *L) {
    Instruction i;  // 当前指令（4字节）
    
    while (1) {  // 主循环
        i = *pc++;  // 取指令，PC 自增
        
        switch (GET_OPCODE(i)) {  // 根据操作码分发
            case OP_MOVE: {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                setobjs2s(L, ra + a, rb + b);  // R(A) = R(B)
                break;
            }
            case OP_ADD: {
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                setfltvalue(ra + a, fltvalue(rb + b) + fltvalue(rc + c));
                break;  // R(A) = R(B) + R(C)
            }
            case OP_CALL: {
                // 函数调用——最复杂的指令之一
                // ...
                break;
            }
            // ... 几十种指令
        }
    }
}
```

这段代码里出现了一些 C 宏，刚看可能一脸懵，下面逐一解释：

---

#### `GET_OPCODE(i)` 是什么？

**作用**：从一条 4 字节指令中提取"操作码"（Opcode），告诉 VM **这条指令要干什么**。

一条 Lua 指令的二进制布局（32 位）：

```
┌─────────┬──────────┬──────────┬──────────┐
│ Opcode  │   A      │   B      │   C      │
│ (6 bit) │ (8 bit)  │ (9 bit)  │ (9 bit)  │
└─────────┴──────────┴──────────┴──────────┘
```

`GET_OPCODE(i)` 取出前 6 位，结果是一个数字（比如 0x01 表示 `MOVE`，0x0B 表示 `ADD`），然后 `switch` 根据这个数字跳到对应的 `case` 去执行。

```c
// 实际源码（lopcodes.h）中的实现
#define GET_OPCODE(i)   cast(OpCode, (i) >> 26)
// 把 32 位整数右移 26 位，剩下的 6 位就是操作码
```

后面的 `GETARG_A` / `GETARG_B` / `GETARG_C` 同理，分别从指令的不同 bit 位段取出操作数 A / B / C，告诉指令**操作哪个槽（寄存器）**。

---

#### `setobjs2s` 是什么？

**作用**：把某个值**从一个槽复制到另一个槽**。

拆开看名字：
- `set` = 设置
- `obj` = TValue（Lua 的通用值类型，见下文）
- `2` = to（简写）
- `s` = stack（栈）

合起来就是：**在栈上（虚拟寄存器区）把一个 TValue 复制到另一个位置**。

```c
// 对应到 Lua 代码就是：
R(A) = R(B)  // 把 R(b) 处的值复制到 R(a)
```

在循环代码里，`ra` 是当前函数**栈帧基地址指针**，所以 `ra + a` 就是"第 a 个槽的位置"，`rb + b` 同理。`setobjs2s(L, ra + a, rb + b)` 就是从槽 b 复制到槽 a。

为什么不用 `=` 直接赋值？因为 TValue 是一个结构体，包含值和类型标记两部分，复制时需要同时拷贝两者，所以 Lua 封装了一个宏来做这件事。

---

#### `setfltvalue` 和 `fltvalue` 是什么？

这两个是一对：**写浮点数** 和 **读浮点数**。

要理解它们，先要知道 **TValue**（Tagged Value）是什么——它是 Lua 里所有值的**统一表示**：

```c
// Lua 源码 lobject.h 中的类型定义（简化）
typedef struct TValue {
    Value value_;       // 实际值（联合体，可能是 number/pointer/...）
    lu_byte tt_;        // 类型标记（当前存的是 number、string、table...）
} TValue;

typedef union Value {
    struct GCObject *gc;    // 需要 GC 的对象（table, function, string...）
    void *p;                // 裸指针
    lua_Number n;           // 双精度浮点数（Lua 里的 number）
    // ...
} Value;
```

看到 `TValue` 有两个字段：
- `value_` 是个联合体，可以解释成 `double`，也可以解释成 `GC 对象指针`，取决于你**按哪种类型去读**
- `tt_` 是个标记，告诉你当前到底存的是哪种类型

`fltvalue(x)` 就是从 TValue 中**读浮点数**（取 .value_.n 字段），而 `setfltvalue(x, v)` 是往 TValue 里**写入一个浮点数**（设置 .value_.n 字段）。

```c
// fltvalue(x) — 取出浮点数
// 就像 Lua 代码里的 local v = t[i]，取到的值是个 number

// setfltvalue(x, v) — 写入浮点数
// 就像 Lua 代码里的 t[i] = 3.14
```

---

#### 回到指令一看就懂了

```c
case OP_ADD: {
    int a = GETARG_A(i);  // 解析操作数：目标槽索引（比如 2）
    int b = GETARG_B(i);  // 解析操作数：第一个加数槽索引（比如 0）
    int c = GETARG_C(i);  // 解析操作数：第二个加数槽索引（比如 1）
    
    // 1. fltvalue(rb + b) → 到 R0 槽读值（比如 100.0）
    // 2. fltvalue(rc + c) → 到 R1 槽读值（比如 50.0）
    // 3. 100.0 + 50.0 = 150.0
    // 4. setfltvalue(ra + a, ...) → 把 150.0 写入 R2 槽
    setfltvalue(ra + a, 
        fltvalue(rb + b) + fltvalue(rc + c));
    break;
}
```

对应 Lua 代码 `local c = a + b`，编译成 `ADD R2 R0 R1`，VM 执行时就是：

> **到 slot 0 拿个值，到 slot 1 拿个值，加起来，放到 slot 2**

整个过程就是几条 C 语句，没有函数调用，所以非常快。因为前面在解析字节码时已经知道了类型（`OP_ADD` 的操作数一定是 number），所以这里不再检查类型，直接用 `fltvalue` 去取 .value_.n 字段，省掉了所有运行时判断。

---

**理解这个循环对性能优化的意义**：
- 循环体越少的 `switch` 分支，执行越快
- 这意味着：**代码越简单、指令越少 → 执行越快**
- 而 Lua 编译器优化很弱（几乎没有），代码怎么写基本决定了指令怎么生成


## 第二章：Table 内部实现——Lua 唯一的"数据结构"

### 2.1 为什么 Table 这么重要？

在 Lua 里，Table 是唯一的、也是最核心的数据结构。**数组、字典、对象、模块、类**——背后全是 Table。优化 Lua 性能，本质上就是在优化 Table 的使用。

### 2.2 Table 的内存布局

Lua 的 Table 由**两部分**组成：

```
Table 对象
├── 数组部分 (array part)  —— 存连续整数键 1..n
│   └── 一块连续内存，索引直接算偏移
│
└── 哈希部分 (hash part)   —— 存其他所有键
    └── 开放地址法的哈希表，键 -> 探测查找
```

结构体的简化版本：

```c
// lobject.h 中 Table 的定义（简化）
typedef struct Table {
    CommonHeader;           // GC 头（所有 GC 对象都有，包含 GC 标记位、类型等）
    lu_byte flags;          // 一些元方法的缓存标记
    lu_byte lsizenode;      // 哈希部分的 2^n 大小（即实际大小 = 2^lsizenode）
    unsigned int alimit;    // 数组部分的逻辑大小上限（见下方说明）
    TValue *array;          // 数组部分指针
    Node *node;             // 哈希部分指针
    Node *lastfree;         // 哈希自由链表的游标
    struct Table *metatable; // 元表
    GCObject *gclist;       // GC 链表
} Table;
```

这里有三个字段需要展开解释：

---

**`alimit` — 数组的"逻辑大小"和"实际分配大小"**

`alimit` 存的是**逻辑大小**，不是实际分配了多少内存。比如：

```lua
local t = {}
t[1] = "a"
t[2] = "b"
t[3] = "c"
```

此时 Lua 可能给数组部分实际分配了 4 个槽位（`实际分配 = 4`），但 `alimit = 3`（你只用到 3 个）。
- **实际分配大小**：Lua 内部管理的内存（按 2 的幂分配）
- **逻辑大小（alimit）**：数组部分的"最后一个非 nil 元素的索引"

`alimit` 的一个重要用途是 `#t` 取长度——它直接从 `alimit` 字段读，所以 `#t` 对数组部分是 O(1) 的。但如果有 nil 空洞，`#t` 就要二分查找了，这个是第 3.4 节的内容。

---

**`lastfree` — 哈希自由链表的游标**

Lua 的哈希部分用**开放地址法**（见第 2.5 节），冲突时往后探测空位。`lastfree` 就是用来加速"找空位"这个操作的游标：

```
哈希表结构：
┌─────┬─────┬─────┬─────┐
│  0  │  1  │  2  │  3  │  ← 哈希槽位
└─────┴─────┴─────┴─────┘
   ↑                    ↑
  node              lastfree ← 从尾部往前搜索空位
```

当要插入一个新键值对，但计算出的哈希位置被占了，Lua 会从 `lastfree` 位置（尾部）**向前搜索**空闲槽位。把 `lastfree` 往前走的好处是：**新元素尽量往后放，老元素尽量靠近它们的哈希位置**，减少冲突链长度。

---

**`CommonHeader` — GC 对象公共头**

所有需要 GC 回收的 Lua 对象（Table、Function、String、Userdata 等）都包含这个头部，它等价于：

```c
#define CommonHeader struct GCObject *next; lu_byte tt; lu_byte marked
// next   = GC 链表指针（把所有 GC 对象串起来）
// tt     = 类型标记（这是 Table 还是 Function 还是 String...）
// marked = GC 标记位（用于标记-清除算法的白色/灰色/黑色标记）
```

所以 Table 结构体最前面 3 个字段实际上是 `next`、`tt`、`marked`，所有 GC 对象都一样，Lua GC 就通过 `next` 链表遍历所有对象。

---

关键理解：**每个 Table 内部有两块独立的内存区域**，用整数键还是用字符串键，走的是完全不同的路径。

### 2.3 数组部分的"50% 规则"

Lua 怎么决定把键放到数组部分还是哈希部分？规则如下：

> 如果一个整数键 `i` 满足：`1 <= i <= 当前数组大小` 且 **数组部分填充率 > 50%**，则放入数组部分。
> 否则放入哈希部分。

这个 **50% 规则** 是 Lua Table 性能陷阱的根源之一。

看看具体的例子：

```lua
-- 场景 A：连续整数键，从 1 开始 → 全部进入数组部分 ✓
local t = {}
for i = 1, 100 do
    t[i] = i * 2  -- t[1]..t[100] 都在数组部分
end

-- 场景 B：从 0 开始 → 全进哈希部分 ✗
local t = {}
for i = 0, 99 do
    t[i] = i * 2  -- t[0] 不满足 1 <= i，进哈希部分
end

-- 场景 C：先插字符串键，再插整数键 → 整数键也可能进哈希 ✗
local t = {}
t["name"] = "张三"
t["age"] = 25
t[1] = "第一个"  -- 此时 Table 已分配哈希部分但可能没数组部分
t[2] = "第二个"  -- 也进哈希
```

**实践原则**：
- 能用数组的场景，**从 1 开始连续递增**地赋值
- 不要混用"数组用法"和"字典用法"——要么全整数键，要么全字符串键
- 避免从 0 开始索引

### 2.4 Rehash —— Table 的动态扩容

当 Table 装不下新元素时，Lua 会触发 **rehash**。这是 Table 操作中最昂贵的操作，没有之一。

#### 2.4.1 为什么要 rehash？

用一个简单的场景来理解：

```lua
-- 初始时 Table 是空的
local t = {}
-- Lua 内部：array = NULL, hash = NULL, alimit = 0

-- 插入第一个元素
t[1] = "a"
-- Lua：数组部分分配 1 个槽位
-- 内部状态：array = [a], hash = NULL, alimit = 1

-- 插入第二个元素
t[2] = "b"
-- Lua：数组部分可能要扩容到 2
-- 因为当前大小=1，装不下第 2 个
-- 内部状态：array = [a, b], alimit = 2

-- 插入第三个元素
t[3] = "c"
-- 数组部分又要扩容，2→4
-- 内部状态：array = [a, b, c, nil], alimit = 3
```

**问题**：数组/哈希表的大小是固定的一段连续内存。当容量不够时，需要：
1. 申请一块更大的内存
2. 把旧数据全部搬过去
3. 释放旧内存

这个过程就是 `rehash`。和 C++ 的 `std::vector` 扩容是同一个道理。

#### 2.4.2 Rehash 的完整流程

```c
// ltable.c 中 luaH_resize 的伪代码——每一步干什么
void luaH_resize(lua_State *L, Table *t, 
                 unsigned int new_array_size, 
                 unsigned int new_hash_size) {
    
    // 第一步：分配新内存（此时新旧并存）
    TValue *new_array = malloc(new_array_size * sizeof(TValue));
    Node *new_hash = malloc(new_hash_size * sizeof(Node));
    
    // 第二步：初始化新内存（所有槽位置为 nil）
    for (int i = 0; i < new_array_size; i++)
        new_array[i] = nil;
    for (int i = 0; i < new_hash_size; i++)
        new_hash[i] = nil;
    
    // 第三步：遍历旧数组部分，一个一个搬到新位置
    for (int i = 0; i < old_array_size; i++) {
        if (old_array[i] != nil) {
            // 根据 50% 规则决定放数组还是哈希
            if (应该放入新数组 & 新数组范围内) {
                new_array[i] = old_array[i];  // 直接搬
            } else {
                // 需要放入哈希部分——重新计算哈希位置
                int pos = hash(key) & (new_hash_size - 1);
                // 如果冲突，线性探测找空位（详情见2.5 节）
                while (new_hash[pos] != nil) 
                    pos = (pos + 1) & (new_hash_size - 1);
                new_hash[pos] = old_array[i];
            }
        }
    }
    
    // 第四步：遍历旧哈希部分，一个个重新插入
    for (int i = 0; i < old_hash_size; i++) {
        if (old_hash[i] != nil) {
            // 同样要重新计算：放数组还是放哈希？
            // 规则和第三步完全一样
            // ...
        }
    }
    
    // 第五步：替换指针，释放旧内存
    t->array = new_array;      
    t->node = new_hash;
    t->alimit = new_array_size;
    t->lsizenode = log2(new_hash_size);
    free(old_array);
    free(old_hash);
}
```

**关键理解**：rehash 不是简单地"数组翻倍"，而是**把所有键值对拿出来重新分配**——检查每个键应该去数组部分还是哈希部分，放不下的重新计算哈希位置。所以它和内存大小成正比。

#### 2.4.3 什么时候触发 rehash？

不仅是"装满了才触发"，实际上 Lua Table 在以下情况都可能触发 rehash：

```lua
local t = {}

-- 情况 1：数组部分满了，要插入新的整数键
for i = 1, 4 do
    t[i] = i  -- 第 4 次插入时可能触发 rehash（当前数组大小=4→8）
end

-- 情况 2：哈希部分满了，要插入新的非整数键
-- 哈希部分的装载因子达到 100% 左右触发
t["name"] = "张三"  -- 如果哈希部分已满，触发 rehash
t["age"] = 25

-- 情况 3：从数组部分移入哈希部分（破坏数组连续性时）
t[100] = "新元素"  -- 当前数组部分只有 4 个元素（1..4）
-- 插入 t[100] 后，Lua 检查 50% 规则：
-- 数组部分要扩展到 100？
-- 但 5/100 = 5% < 50%，不满足数组条件
-- 所以 t[100] 进哈希部分
-- 但这次操作可能会触发一次 rehash 来重新分配
```

#### 2.4.4 rehash 的真实成本——步进式图解

```lua
-- 让我们逐步看一个 Table 从空到插入 7 个元素的完整过程
local t = {}

-- 初始状态：array = NULL, hash = NULL
-- 什么都没分配

t[1] = "a"
-- 第一次插入 → 分配 1 个数组槽
-- 状态：array[4]? 实际上 Lua 最小数组 = 4

t[2] = "b"
-- alimit = 2，还在范围内，不需要 rehash

t[3] = "c"
-- alimit = 3，不需要 rehash

t[4] = "d"
-- alimit = 4 === 当前数组大小，下一次插入需要 rehash

t[5] = "e"
-- ⚠ 插入第 5 个 → rehash 触发！
-- 旧状态：array[1..4] 满了
-- rehash 后：array 扩展到 8（2 倍）
-- 状态：array[1..5] = a,b,c,d,e, nil,nil,nil
-- 这次 rehash 搬了 4 个元素

-- 继续插入...
t[6] = "f"
t[7] = "g"
t[8] = "h"
-- alimit = 8 == 数组大小，下一次又要触发

t[9] = "i"
-- ⚠ rehash 触发！array 8→16
-- 搬了 8 个元素
```

整个过程的内存变化：

```
插入次数     数组大小    是否 rehash     搬了多少元素
───────────────────────────────────────────────
t[1]          4         初始分配        0
t[2]          4         否              0
t[3]          4         否              0
t[4]          4         否              0
t[5]          8         ⚠ rehash       4
t[6..8]       8         否              0
t[9]         16         ⚠ rehash       8
t[10..16]    16         否              0
t[17]        32         ⚠ rehash       16
```

#### 2.4.5 100000 个元素的成本分析

```lua
-- 重新看最开始的例子
local t = {}
for i = 1, 100000 do
    t[i] = i
end
```

这个循环触发了多少次 rehash？

```
数组扩容序列：4 → 8 → 16 → 32 → 64 → 128 → 256 → 512 → 1024 → 2048 → 4096 → 8192 → 16384 → 32768 → 65536 → 131072

从 4 到 131072（>=100000），一共扩容了 15 次
每次搬移的元素数量：4+8+16+32+64+128+256+512+1024+2048+4096+8192+16384+32768+65536 = 131068

总搬移量 ≈ 130,000 个单元
单位搬移成本：创建 TValue + 类型赋值
```

虽然看起来搬了 13 万次，但每次搬移实际上只是**内存拷贝 + 哈希计算**，C 层面很快。在 100000 规模下，这 15 次 rehash 总共耗时大概 **1~3 毫秒**——相比 Lua 循环本身的执行时间，占比不大。

**那什么时候 rehash 真正成为问题？**

```lua
-- 问题场景：频繁创建大 Table 的函数
function create_big_table()
    local t = {}
    for i = 1, 100000 do
        t[i] = i * 2
    end
    return t
end

-- 每秒被调用 10 次！
for frame = 1, 600 do  -- 60 秒 × 10fps = 600 帧
    local t = create_big_table()  -- 每帧创建 + 15 次 rehash
    process(t)
end
-- 总耗时：600 帧 × 15 次 rehash × ~0.2ms = 1800ms
-- 其中 1.8 秒花在 rehash 上！

-- 优化：复用同一个 Table
local t = {}
for frame = 1, 600 do
    for i = 1, 100000 do
        t[i] = i * 2  -- 重用 Table，不再触发 rehash
    end
    process(t)
end
-- 只需要第一次循环时的 15 次 rehash
-- 后续帧：0 次 rehash
```

#### 2.4.6 rehash 和 GC 的关系

rehash 不仅仅是"搬数据"，它还给 GC 带来额外压力：

```c
// rehash 中创建的新数组和哈希表
TValue *new_array = malloc(...);   // 新内存 ← GC 要追踪
Node *new_hash = malloc(...);      // 新内存 ← GC 要追踪

// rehash 完成后旧内存被释放
free(old_array);  // 旧内存 ← GC 标记清除
free(old_hash);
```

每次 rehash 会产生：
1. **新的 GC 对象**（新的 array 和 hash 内存块）
2. **旧的 GC 对象变成垃圾**（下次 GC 循环要清理）
3. **GC 压力增加**：对象越多，GC 的标记-清除阶段越慢

这就是为什么频繁创建大 Table 不仅慢在 rehash，还慢在 GC。

#### 2.4.7 预分配的正确做法

前面提到的 `t[i] = nil` 预分配在 Lua 5.3+ 中不一定有效，因为对 nil 的赋值不会真的分配空间。正确的预分配做法：

```lua
-- 方法一：用 dummy 值填充（最可靠）
local function prealloc_array(n)
    local t = {}
    for i = 1, n do
        t[i] = 0  -- 先填 0 占位，后续再覆盖
    end
    return t
end

-- 使用
local arr = prealloc_array(100000)
for i = 1, 100000 do
    arr[i] = compute(i)  -- 覆盖，零 rehash
end

-- 方法二：构造时直接初始化（如果值已知）
local arr = {}
for i = 1, 100000 do
    arr[i] = i  -- 边初始化边扩，反正只有 15 次 rehash
end

-- 方法三：多次复用同一个 Table（最佳实践）
local cache = {}  -- 只创建一次

function process_items(items)
    local n = #items
    
    -- 确保容量足够（如果 cache 太小，只补到够用，不缩）
    -- 利用 Table 只扩不缩的特性
    for i = 1, n do
        cache[i] = process(items[i])
    end
    -- 清理尾部多余的元素（上轮留下的）
    for i = n + 1, #cache do
        cache[i] = nil
    end
    return cache
end
```

#### 2.4.8 rehash 的性能底线

```lua
-- 记住几条经验法则：

-- 规则 1：对纯数组 Table，扩容 15 次 vs 预分配
-- 差异大约只有几毫秒。如果你的函数不是 hot path，不需要纠结。

-- 规则 2：rehash 真正吃性能的地方是哈希部分
-- 因为数组部分就是"建立新数组，memcpy 旧数据"
-- 而哈希部分要对每个元素重新计算哈希位！有 O(n) 的哈希开销

-- 规则 3：大 Table 的 rehash 对 GC 的影响可能比 rehash 本身更大
-- 频繁创建大 Table → 大量 GC 对象 → GC 变慢

-- 规则 4：复用 Table 是性价比最高的优化
-- 不是省 rehash，是省 GC
```

### 2.5 哈希冲突和查找性能——开放地址法详解

#### 2.5.1 什么是开放地址法？

Lua 的哈希部分使用**开放地址法（Open Addressing）**，和常见的"拉链法（链表法）"不同。

两种方法对比：

```
拉链法（如 Java HashMap）：
┌─────┐
│ [0] │──→ [key1] → [key2] → ...  ← 每个槽位挂一个链表
├─────┤
│ [1] │──→ null                    ← 空槽位不占链表
├─────┤
│ [2] │──→ [key3] → ...
└─────┘

开放地址法（Lua Table）：
┌─────┬─────┬─────┬─────┐
│ [0] │ [1] │ [2] │ [3] │          ← 所有元素存在数组里，链表为 0
│ k1  │ k3  │ k2  │     │          ← 冲突了就往后放
└─────┴─────┴─────┴─────┘
```

开放地址法的特点是：**没有链表，所有元素都存储在哈希数组本身中**。冲突时不挂链表，而是找下一个空位放。

#### 2.5.2 哈希表的基本结构

Lua 的哈希部分底层是一个 `Node` 数组：

```c
// lobject.h 中的定义（简化）
typedef struct Node {
    TValue i_val;    // 值（和 TValue 一样，可以存任何类型）
    TKey i_key;      // 键（TKey 比 TValue 多一个 hash 缓存字段）
} Node;

// 哈希表：Node nodes[2^lsizenode]
// 比如 lsizenode = 3，则 nodes[0..7] 一共 8 个槽位
```

每个 `Node` 就是一对**键值对**。整个哈希表是一个连续数组。

#### 2.5.3 插入一个键值对时的完整流程

```lua
local t = {}
t["hello"] = "world"
```

在 C 层面发生了什么：

第一步：**计算哈希值**

```c
// lstring.c 中字符串哈希的简化
unsigned int luaS_hash(const char *str, size_t len) {
    // Lua 使用改良的 Jenkins 哈希算法
    unsigned int h = (unsigned int)len;
    for (size_t i = 0; i < len; i++) {
        h ^= ((h << 5) + (h >> 2) + (unsigned char)str[i]);
    }
    return h;
}
```

以 `"hello"` 为例，假设算出来的哈希值是 `0xABCD1234`。

第二步：**主位置（Main Position）定位**

```c
// 假设当前哈希表大小是 4（2^2，lsizenode = 2）
int mainpos = hash("hello") & (size - 1);
// 等价于：0xABCD1234 & 3 = 2（取最后 2 位）
// 所以主位置是 index = 2
```

第三步：**检查主位置是否为空**

```c
Node *main = &nodes[2];  // 取 index=2 的 Node

if (main->i_key.tt_ == LUA_TNIL) {
    // 主位置为空，直接占用
    main->i_key = "hello";
    main->i_val = "world";
    return;  // 插入完成！
}
```

第四步：**主位置已被占 → 冲突处理**

```c
// 主位置 2 已经被其他 key（比如 "abc"）占了
// 开始"线性探测"：

// 检查位置 3
if (nodes[3].i_key.tt_ == LUA_TNIL) {
    // 空位，放入
    nodes[3].i_key = "hello";
    nodes[3].i_val = "world";
    return;
}

// 位置 3 也被占了？检查位置 0（绕回开头）
if (nodes[0].i_key.tt_ == LUA_TNIL) {
    nodes[0].i_key = "hello";
    nodes[0].i_val = "world";
    return;
}

// 位置 0 也被占了？检查位置 1
// 如果全部检查完都没空位 → 触发 rehash，扩容后再试
```

整个探测路径的可视化：

```
哈希表（大小 = 4）：
┌─────┬─────┬─────┬─────┐
│ [0] │ [1] │ [2] │ [3] │
└─────┴─────┴─────┴─────┘
                ↑
               "hello" 的主位置 2

占用情况：
┌─────┬─────┬─────┬─────┐
│     │  k3 │  k1 │     │  ← k1 占了主位置 2
└─────┴─────┴─────┴─────┘

"hello" 插入过程：
┌─────┬─────┬─────┬─────┐
│     │  k3 │  k1 │     │
└─────┴─────┴─────┴─────┘
                ↑ 冲突！检查 2 已被 k1 占

┌─────┬─────┬─────┬─────┐
│     │  k3 │  k1 │ ←？ │  → 位置 3 为空，放入 "hello"
└─────┴─────┴─────┴─────┘

最终：
┌─────┬─────┬─────┬─────┐
│     │  k3 │  k1 │hel lo│
└─────┴─────┴─────┴─────┘
```

#### 2.5.4 查找时的完整流程

```lua
local v = t["hello"]  -- 查找 key = "hello"
```

```c
// ltable.c 中 luaH_get 的简化流程

// 1. 计算哈希值，定位主位置
unsigned int hash = luaS_hash("hello");
int pos = hash & (size - 1);  // 主位置 = 2

// 2. 检查主位置
Node *n = &nodes[pos];
if (n->i_key == "hello") {
    return &n->i_val;  // 找到了！一步到位
}

// 3. 主位置不是要找的 key → 线性探测
for (int i = 1; i < size; i++) {
    int probe = (pos + i) & (size - 1);  // 下一个探测位置
    n = &nodes[probe];
    
    if (n->i_key.tt_ == LUA_TNIL) {
        return NULL;  // 遇到空位 → key 不存在，结束查找
    }
    if (n->i_key == "hello") {
        return &n->i_val;  // 找到了！
    }
}

// 4. 整个表找遍了没有 → 返回 nil
return NULL;
```

查找时的关键优化：**遇到 nil 就停**。因为开放地址法插入时，冲突元素是顺次往后放的，所以一旦在探测链上遇到空位，就说明后面不可能有我们要找的 key（如果后面还有，它应该填在这个空位之前）。这个性质保证了查找只需要线性探测到第一个空位为止。

#### 2.5.5 为什么 Lua 不直接用拉链法？

Lua 选择开放地址法的原因：

| 因素         | 开放地址法                   | 拉链法                                  |
| ------------ | ---------------------------- | --------------------------------------- |
| 内存消耗     | 一个连续数组                 | 数组 + 链表节点（每个节点额外指针开销） |
| CPU 缓存友好 | ✅ 连续内存，遍历时缓存命率高 | ❌ 链表节点分散，缓存不友好              |
| 删除操作     | ❌ 需要标记而非直接删除       | ✅ 直接删除链表节点                      |
| 装载因子上限 | 约 50%~70%（否则性能骤降）   | 可达 70%~80%                            |

对于 Lua 这样追求极致性能的脚本语言，**CPU 缓存命中率**比内存占用更重要。连续内存的扫描虽然探测次数可能多一点，但每次都在连续地址上访问，缓存行（Cache Line）一次可以加载多个 Node。

#### 2.5.6 装载因子对性能的影响

装载因子 = `已存储的节点数 / 哈希表总大小`

```lua
-- 用一个简单的实验来看装载因子的影响
-- 假设我们有 1000 个数据，分别用不同大小的哈希表

-- 情况 A：装载因子 50%（哈希表大小 2000）
-- 每 2 个位置里只有 1 个有数据
-- 探测的平均次数 ≈ 1.5 次
-- 性能：接近 O(1)

-- 情况 B：装载因子 80%（哈希表大小 1250）
-- 每 5 个位置里 4 个有数据
-- 探测的平均次数 ≈ 4~5 次
-- 性能：明显下降

-- 情况 C：装载因子 95%（哈希表大小 ~1053）
-- 几乎满了
-- 探测链可能很长，极端情况接近 O(n)！
-- 这就是为什么 Lua 在 rehash 时保持约 50% 空闲
```

```
装载因子和探测次数的关系（近似）：
 50% → 平均 1.5 次探测
 60% → 平均 2.0 次探测
 70% → 平均 3.0 次探测
 80% → 平均 5.0 次探测
 90% → 平均 10+ 次探测（性能骤降）
```

#### 2.5.7 哈希碰撞的实际影响

```lua
-- 正常情况：哈希分布均匀，查找快
local t = {}
for i = 1, 1000 do
    t["key_" .. i] = i  -- 不同的哈希值均匀分布在表中
end
-- 每次查找平均 1~2 次探测

-- 极端情况：刻意构造哈希碰撞
-- Lua 字符串哈希对不同长度的字符串分布较好
-- 但数字（整数）作为哈希键时：
local t = {}
t[1] = "a"
t[2] = "b"
-- 整数键会先检查是否要进数组部分（见 2.3 的 50% 规则）
-- 如果进了哈希部分，哈希冲突情况取决于整数的分布
```

**实际影响总结**：
- 对于字符串键：Lua 的哈希算法分布良好，碰撞很少，不用担心
- 对于整数键：Lua 先走数组部分（见 2.3 节），只有不符合规则的才进哈希
- **真正的性能瓶颈**不在于哈希碰撞本身，而在于 rehash（重新分配内存 + 全量迁移）和后续章节要讲的各种 Table 使用习惯

### 2.6 弱引用表（__mode）对 GC 的影响

```lua
-- 弱引用表：key 或 value 是弱引用，不影响 GC 回收
local weak = setmetatable({}, {__mode = "v"})  -- value 弱引用

-- 不加弱引用的话，即使其他地方不再引用，table 里的引用会阻止 GC
local normal = {}
do
    local obj = {data = "large"}
    normal[1] = obj  -- obj 被 table 持有，不会 GC
end
-- 这里 obj 虽然超出作用域，但因为 normal[1] 还引用着它，内存不释放

-- 用弱引用 table 就不会阻止 GC
do
    local obj = {data = "large"}
    weak[1] = obj  -- 弱引用，obj 超出作用域后可被回收
end
```

**性能要点**：弱引用表在 GC 阶段需要额外遍历清理，不是免费的。只在需要缓存且不想阻止回收的场景使用。


## 第三章：Table 布局优化实战

### 3.1 预分配 vs 动态增长

```lua
-- 写法一：动态增长（差）✗
-- 每次都 append，会触发多次 rehash
local function collect_items(items)
    local t = {}
    for i = 1, #items do
        table.insert(t, process(items[i]))  -- 可能每次都得扩容
    end
    return t
end

-- 写法二：预分配（好）✓
-- 减少 rehash 次数
local function collect_items(items)
    local n = #items
    local t = {}
    -- 预先写入 n 个 nil，强制数组扩展到 n
    for i = 1, n do
        t[i] = nil  -- Lua 5.4 中这个写法有效
    end
    -- 或者更直接的写法：预先填一个值
    -- for i = 1, n do t[i] = false end
    for i = 1, n do
        t[i] = process(items[i])  -- 不会再触发 rehash
    end
    return t
end

-- 写法三：提前知道大小就用 table.create（Lua 5.3+ 无此 API）
-- 可以用一个简单的辅助函数
local function new_table(narray, nhash)
    local t = {}
    if narray and narray > 0 then
        for i = 1, narray do
            t[i] = false  -- 预占位
        end
    end
    return t, narray  -- 返回数组大小，让调用方知道接着写
end
```

### 3.2 避免"中间空洞"破坏数组部分

```lua
-- 错误示范：中间跳号 → 数组部分被破坏 ✗
local t = {}
t[1] = "A"
t[2] = "B"
t[4] = "D"  -- 跳过了 3！看看会怎样？
-- 此时数组部分是第 1~2 个元素，哈希部分存 (4 -> "D")
-- 但如果再插入 t[3] = "C"：
t[3] = "C"  -- 数组部分扩充到 4（填充率 4/4 = 100% > 50%）
-- t[1]..t[4] 全部进数组部分了

-- 但如果先写 t[4] 再写 t[2]：
local t = {}
t[4] = "D"  -- 数组大小 4，只有 1/4 = 25% < 50%，全进哈希！
t[2] = "B"  -- 也进哈希
```

**最佳实践**：如果要用数组，就**从 1 开始连续地填**。如果做不到连续，干脆用字符串键当字典用。

### 3.3 local 引用——减少 Table 查找的最有效手段

```lua
-- 这是最有效的优化技巧，没有之一

-- 慢版本：反复从全局表查找 ✗
function slow_version()
    for i = 1, 100000 do
        math.sin(i)  -- 每次都要查 _ENV["math"]，再查 math["sin"]
    end
end
```

这里注释里出现了 `_ENV`。它是什么？

**`_ENV` 是 Lua 中的"全局环境"**。Lua 5.2+ 中，每个代码块（chunk）都有一个叫 `_ENV` 的 upvalue，指向一个 Table。当你写 `math.sin(i)` 时，Lua 实际上编译成：

```
GETTABUP  R(0) _ENV "math"     -- R(0) = _ENV["math"]，即全局的 math 表
GETTABLE R(1)  R(0) "sin"      -- R(1) = R(0)["sin"]，即 math.sin 函数
```

所以每调用一次 `math.sin(i)`，就是**一次 Upvalue 访问 + 两次 Table 查找**。在循环里跑十万次，就是二十万次 Table 查找。

```lua
-- 快版本：局部化引用 ✓
function fast_version()
    local sin = math.sin  -- 只查一次：_ENV["math"] → math["sin"] → 存到 local 变量
    for i = 1, 100000 do
        sin(i)  -- 直接调用 local 变量，零 Table 查找
    end
end

-- 测试结果（Lua 5.3）：
-- slow_version: 约 15ms
-- fast_version: 约 8ms
-- 快了近一倍！
```

**更极端的例子：大量 Table 访问**

```lua
-- 复杂的数据结构访问 ✗
function update_player_slow(player)
    player.hp = player.hp - player.damage
    if player.hp <= 0 then
        player.state = "dead"
        player.death_time = os.time()
    end
    player.mp = player.mp + player.mp_regen
    if player.mp > player.max_mp then
        player.mp = player.max_mp
    end
end

-- 局部化优化 ✓
function update_player_fast(player)
    local hp = player.hp
    local damage = player.damage
    local mp = player.mp
    local mp_regen = player.mp_regen
    local max_mp = player.max_mp
    
    hp = hp - damage
    if hp <= 0 then
        player.state = "dead"
        player.death_time = os.time()
        player.hp = 0
    else
        player.hp = hp
    end
    
    mp = mp + mp_regen
    if mp > max_mp then
        mp = max_mp
    end
    player.mp = mp
end
```

### 3.4 避免用 `table.insert` 和 `#` 取长度

```lua
-- table.insert 式写法 ✗
local t = {}
for i = 1, 1000 do
    table.insert(t, i)  -- 函数调用开销 + 边界检查 + 可能的 rehash
end

-- 直接赋值 ✓
local t = {}
for i = 1, 1000 do
    t[i] = i  -- 直接赋值，少了一次函数调用
end
```

`#t` 取长度也不是 O(1) 的——准确说，**对纯数组是 O(1)，对有空洞或哈希表则不是**：

```lua
-- 数组部分的 #t 是 O(1)
local arr = {1, 2, 3, 4, 5}
print(#arr)  -- 5，直接从 Table 头部的 alimit 字段读
-- 回顾第 2.2 节：alimit 是"逻辑大小上限"
-- 这里数组部分 5 个元素连续，alimit = 5，直接返回
-- 相当于：#t 就是 "intuitional array length"

-- 但如果数组部分有"空洞"（nil），Lua 需要二分查找
local arr = {1, 2, nil, 4, 5}
print(#arr)  -- ? 结果不确定！
-- Lua 的规则是：如果最后一个元素是 nil，# 会向前找第一个非 nil 的"边界"
-- 这里的结果可能是 2 或 5，取决于具体实现
-- 因为 alimit=5 但 t[5]=5 存在，但中间有 nil，二分后结果可能是 2（连续部分）或 5（最后一个非 nil）

-- 哈希表用 #t 几乎总是 O(n)
local dict = {a = 1, b = 2, c = 3}
print(#dict)  -- 0（哈希表没有数组部分，alimit = 0，# 返回 0）
```

**实践建议**：
- 需要取数组大小时，**自己维护长度变量**
- 不要依赖 `#t` 取字典的元素个数（Lua 5.4 提供了 `table.count`）

```lua
-- 自己维护长度的模式
local function create_queue()
    local queue = {}
    local size = 0  -- 自己记长度
    return {
        push = function(item)
            size = size + 1
            queue[size] = item
        end,
        pop = function()
            if size <= 0 then return nil end
            local item = queue[1]
            -- 前移（如果要保证 O(1) pop，应该用环形缓冲区）
            for i = 2, size do
                queue[i - 1] = queue[i]
            end
            queue[size] = nil
            size = size - 1
            return item
        end,
        size = function() return size end
    }
end
```

### 3.5 在 Hot Path 中避免 Table 创建

```lua
-- 不好的写法：每次调用都创建新 Table ✗
function get_skill_info(skill_id)
    return {
        id = skill_id,
        name = get_skill_name(skill_id),
        cd = get_skill_cd(skill_id),
        damage = get_skill_damage(skill_id)
    }
end

-- 频繁调用：比如每秒 60 帧的战斗循环中
for _, skill in ipairs(player.skills) do
    local info = get_skill_info(skill.id)  -- 每秒创建 60 个 Table！
    process_skill(info)
end

-- 更好的做法：复用 Table ✓
local skill_info_pool = {}

local function fill_skill_info(info, skill_id)
    info.id = skill_id
    info.name = get_skill_name(skill_id)
    info.cd = get_skill_cd(skill_id)
    info.damage = get_skill_damage(skill_id)
    return info
end

-- 预先创建好
for i = 1, 10 do
    skill_info_pool[i] = {}
end

local pool_idx = 1
for _, skill in ipairs(player.skills) do
    local info = skill_info_pool[pool_idx]
    pool_idx = pool_idx + 1
    fill_skill_info(info, skill.id)
    process_skill(info)
end
-- 用完归还（或 pool_idx 重置回 1）
```

### 3.6 选择合适的数据结构

不是所有场景都要用 Table：

| 场景           | 最佳方案                          | 原因                                |
| -------------- | --------------------------------- | ----------------------------------- |
| 固定数量的数值 | **局部变量**                      | 零 Table 访问                       |
| 小型固定集合   | **多重赋值 / 多返回值**           | 函数返回多个值不创建 Table          |
| 大型连续数组   | **Table 数组部分**                | O(1) 访问，内存紧凑                 |
| 频繁增删的队列 | **数组 + 头尾指针（环形缓冲区）** | 避免 table.insert/remove 的元素移动 |

```lua
-- 多返回值替代 Table ✗→✓
-- 差：用 Table 包装返回值
function get_position()
    return {x = 100, y = 200, z = 300}
end
local pos = get_position()
print(pos.x, pos.y, pos.z)  -- 创建了临时 Table

-- 好：多返回值
function get_position_fast()
    return 100, 200, 300  -- 零分配
end
local x, y, z = get_position_fast()
print(x, y, z)
```


## 第四章：JIT 编译优化（LuaJIT 篇）

### 4.1 什么是 JIT？

**JIT = Just-In-Time Compilation（即时编译）**。

普通的 Lua 解释器（PUC-Rio Lua）是**纯解释执行**的：一条一条读字节码，一条一条执行。

而 LuaJIT 多了一步：**把热点代码编译成机器码**，然后直接执行机器码，不需要再一条条解释。

```
纯解释执行：
  源码 → 字节码 → [读取 → 分发 → 执行] × N 条指令

JIT 执行：
  源码 → 字节码 → [解释执行，同时统计热度]
                 → 热点达到阈值 → 编译为机器码
                 → 直接执行机器码（相当于在内存里生成了 C 代码编译后的结果）
```

### 4.2 LuaJIT 的工作模式

LuaJIT 有三种执行模式：

| 模式             | 速度    | 说明                                      |
| ---------------- | ------- | ----------------------------------------- |
| **Interpreter**  | 1x      | 标准的字节码解释器，和 Lua 5.1 速度相当   |
| **JIT-compiled** | 10~100x | 热点代码编译为机器码执行                  |
| **C 函数调用**   | 一样快  | LuaJIT 的 FFI 可以直接调用 C 函数，零开销 |

### 4.3 Trace Compiler——LuaJIT 的核心

和很多 JIT（如 JVM 的 C2 编译器）以"函数"为单位编译不同，LuaJIT 以 **Trace（跟踪）** 为单位。

什么叫 Trace？就是**程序中的热点路径**。

```
for i = 1, 1000000 do
    sum = sum + arr[i]  -- 这条路径被反复执行
end
```

LuaJIT 的执行流程：

```
1. 解释执行，同时记录每条指令的执行次数（hot count）
2. 某个循环入口 hot count 达到阈值（默认 1000）
3. 开始录制（Record）：记录当前 Trace 执行的所有操作
4. 一路录到循环结束，生成 SSA IR（中间表示）
5. 优化 IR（常量折叠、死代码消除等）
6. 生成机器码（Emit）
7. 下次再走到这个入口，直接跳转执行机器码
```

---

**补充：什么是 SSA IR？**

SSA = **Static Single Assignment（静态单赋值）**，是一种中间表示的格式。

核心思想：**每个变量只赋值一次**。

看个例子，普通写法 vs SSA 形式：

```lua
-- Lua 源码
x = a + b
x = x * 2
y = x + c

-- SSA 形式（每个变量只赋值一次，重命名多次赋值）
x1 = a + b     -- 第一次赋值：x1
x2 = x1 * 2    -- 第二次赋值：用新名字 x2，明确知道 x1 === a + b
y1 = x2 + c    -- 只用 x2，不会误用旧值
```

SSA 有什么好处？

```
普通写法（非 SSA）：                     SSA 写法：
  x = a + b                               x1 = a + b
  ...   ← 万一这里有人改了 x？              ...   ← x1 不可能被改（只赋一次值）
  y = x * 2    ← 这个 x 是哪个？            y1 = x1 * 2  ← 明确就是 a+b
```

- **常量折叠**：`x1 = 2 + 3` → 直接变成 `x1 = 5`
- **死代码消除**：`x1 = a + b` 后面如果没有任何地方用 `x1` → 整行删掉
- **不变性保证**：`x1` 的值不会在 "..." 中被篡改，优化器可以放心使用

LuaJIT 把 Trace 翻译成 SSA IR 后，再做各种优化，最后生成机器码。这套流水线让生成的机器码非常高效。

### 4.4 Side Trace —— 处理分支

```lua
for i = 1, 1000000 do
    if arr[i] > 0 then
        sum = sum + arr[i]   -- 热点路径 A（95% 的情况）
    else
        other_sum = other_sum + arr[i]  -- 分支 B（5% 的情况）
    end
end
```

LuaJIT 的 Trace Compiler 是先录制**主路径 A**（Record 阶段只记录实际走过的指令）。当主 Trace 已经编译好并执行后，如果某次执行走进了分支 B：

1. 主 Trace 执行到 `if` 条件的机器码
2. 条件判断走分支 B
3. 机器码 **退出（Exits）** Trace，回到解释器
4. 解释器发现这个退出点也很热
5. 从退出点开始录**分支 B 的 Trace**（叫 Side Trace）
6. 把 Side Trace 和主 Trace **桥接（Bridging）** 起来
7. 下次直接在主 Trace 和 Side Trace 间切换，无需出 Trace

```
    ┌──────────┐
    │ 主 Trace  │  ← 循环入口（热）
    └────┬─────┘
         │
    ┌────▼──────┐
    │ arr[i]>0? │——否——→ ┌───────────┐
    └────┬──────┘        │ Side Trace │
         │ 是            └─────┬─────┘
    ┌────▼─────┐              │
    │ sum +=   │              │
    │ arr[i]   │              │
    └────┬─────┘              │
         │                    │
         └─────────┬──────────┘
                   ▼
            ┌──────────┐
            │ 循环尾    │
            │ 跳回开头  │
            └──────────┘
```

### 4.5 NYI —— 为什么你的代码没有被 JIT 编译？

NYI = **Not Yet Implemented**。不是"还没有实现"，而是"**这一条操作 LuaJIT 没法编译成机器码**"。

```lua
-- 以下操作会导致 Trace 中止（NYI），退回解释执行
-- 你的代码再热也不会被 JIT 编译！

-- 1. 使用不支持的库函数
local r = math.fmod(10, 3)  -- math.fmod 是 NYI！用 % 运算符代替

-- 2. 字符串连接太复杂
local s = "a" .. "b" .. "c"  -- 简单连接没问题
local s = table.concat({"a", "b", "c"})  -- 这个更好

-- 3. 创建新函数（闭包）
funcs = {}
for i = 1, 1000 do
    funcs[i] = function() return i end  -- 每次迭代创建闭包 → 中止 Trace！
end
-- 优化：在外面创建函数
function make_counter(i)
    return function() return i end
end
for i = 1, 1000 do
    funcs[i] = make_counter(i)  -- 虽然也创建闭包，但不阻塞 JIT
    -- 实际上这也中止... 更好的做法是重构设计
end

-- 4. 协程操作
coroutine.yield()  -- NYI

-- 5. pcall / xpcall 内部的长循环
-- pcall 内部代码很难被 JIT 编译

-- 6. FFI callback（回调 C 函数）
ffi.cdef("void cb(int);")
local cbfunc = ffi.cast("void (*)(int)", function(i) 
    -- 这里的代码不会被 JIT
end)
```

### 4.6 如何检查你的代码有没有被 JIT 编译

```bash
# 启动 LuaJIT 并开启 JIT 日志
luajit -jdump=ix mycode.lua
# -j 表示 JIT 控制
# dump 表示 dump 编译结果
# i 显示 IR（中间表示）
# x 显示机器码
# mycode.lua 是你要分析的脚本
```

或者更直观的，用 `-jv` 查看 JIT 状态：

```bash
luajit -jv mycode.lua
```

```lua
-- 代码里查看 JIT 状态
local jit = require("jit")

-- 打印当前 JIT 状态
print(jit.status())

-- 查看某个函数是否被 JIT 编译
-- 用 jit.util 模块（只有开启了 -j 才能用）
```

### 4.7 JIT 友好的代码写法

```lua
-- ✅ 好：简单的数值循环
local sum = 0
for i = 1, 1000000 do
    sum = sum + i
end

-- ✅ 好：简单的 Table 遍历
local arr = {}
for i = 1, 10000 do
    arr[i] = i
end
local sum = 0
for i = 1, #arr do
    sum = sum + arr[i]
end

-- ❌ 差：频繁创建 Table
local sum = 0
for i = 1, 10000 do
    local t = {a = i, b = i * 2}
    sum = sum + t.a + t.b
end
-- 如果可以，把 t 挪到外面复用

-- ❌ 差：使用 string 库的部分函数
local s = "hello world"
for i = 1, 10000 do
    s = string.gsub(s, "l", "x")  -- string.gsub 是 NYI，会中止 JIT
end
-- 用 LuaJIT 的 FFI 调用 C 的 str_replace 会更快

-- ❌ 差：混用不同类型的值
local t = {}
for i = 1, 10000 do
    t[i] = i  -- 全是 number
end
t[5000] = "hello"  -- 突然变成 string！→ JIT 编译器要生成 guard
-- LuaJIT 会为 number 生成一条 Trace，但遇到 string 要退出到解释器
-- 尽量保持类型稳定！
```

### 4.8 LuaJIT 的极致性能：FFI

LuaJIT 的 FFI（Foreign Function Interface）可以直接调用 C 结构体和 C 函数，**零开销**：

```lua
-- 纯 Lua 版本（慢）
local x = 0
for i = 1, 1000000 do
    x = x + math.sqrt(i)
end

-- FFI 版本（快 10 倍+）
local ffi = require("ffi")
ffi.cdef("double sqrt(double);")  -- 声明 C 函数

local x = 0
for i = 1, 1000000 do
    x = x + ffi.C.sqrt(i)  -- 直接调 C 的 sqrt，通过 JIT 内联成一条机器码
end

-- 更极致：用 C 结构体代替 Lua Table
local ffi = require("ffi")

-- 声明一个 C 结构体
ffi.cdef[[
    typedef struct {
        double x;
        double y;
        double z;
    } vec3;
]]

-- 创建 C 结构体数组（连续内存，不是 Table 数组！）
local points = ffi.new("vec3[?]", 10000)

-- 访问比 Table 快很多
for i = 0, 9999 do  -- C 数组从 0 开始
    points[i].x = points[i].x + 1.0
    -- 编译后就是几条 CPU 指令：mov [eax+offset], ...
    -- 而 Table 需要查哈希、调 GC 保护...
end
```


## 第五章：总结与实战检查清单

### 5.1 性能优化优先顺序

```
影响从大到小排列：

1. 算法优化（O(n²) → O(n log n)）：收益最大，先做
2. 减少 Table 创建（对象池 / 复用）：减少 GC 压力
3. local 缓存引用（math.sin → local sin）：几乎零成本，收益明显
4. Table 布局优化（预分配、连续索引）：对大数据集有效
5. JIT 友好（避免 NYI、保持类型稳定）：用 LuaJIT 时才有关
6. 微优化（用 for 代替 ipairs、用 + 代替 concat）：最后考虑
```

### 5.2 Lua 性能自检查清单

```lua
-- 写完代码后，对照检查：

-- [ ] 热点路径上的全局函数/变量是否 local 化了？
-- [ ] 循环内有没有创建新 Table/闭包？
-- [ ] 大的 Table 是否预分配了？
-- [ ] 整数键是否从 1 开始连续？
-- [ ] 频繁调用的 C API 是否在初始化时缓存了引用？
-- [ ] 用 LuaJIT 时：有无 NYI 函数阻塞 JIT？
-- [ ] 用 LuaJIT 时：循环内类型是否稳定？
-- [ ] 是否可以用多返回值代替临时 Table？
-- [ ] 字符串拼接用 .. 还是 table.concat？（大量拼接用 concat）
```

### 5.3 常见误区

| 误区                        | 正解                                 |
| --------------------------- | ------------------------------------ |
| "`#t` 是 O(1) 的，随便用"   | 只在纯数组且没有 nil 空洞时才是 O(1) |
| "`table.insert` 很方便就用" | 直接赋值 `t[i] = v` 更快             |
| "局部化所有变量"            | 局部化热点路径的变量，冷代码没区别   |
| "JIT 会自动优化所有代码"    | JIT 遇到 NYI 就退回到解释执行        |
| "Lua 和 C 一样快"           | JIT 编译后接近，纯解释执行慢几十倍   |

---

最后记住一句话：**先让代码正确，再让代码变快。用性能分析工具说话，不要靠猜。**
