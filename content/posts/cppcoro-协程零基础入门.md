+++
title = '从 0 到 1 理解 C++ 协程：30 行代码搞定协程帧、co_await 和 promise_type'
date = '2024-11-08'
draft = false
tags = ["C++", "cppcoro", "协程", "C++20", "源码分析", "coroutine"]
categories = ["C++"]
description = "拆开 cppcoro 给你看 · 第 0 篇。30 行代码手写 Generator，彻底搞懂协程帧、co_await 展开步骤和 promise_type 的 7 个接口，零基础也能直接上手。"
+++

# 从 0 到 1 理解 C++ 协程：30 行代码搞定协程帧、co_await 和 promise_type

> 本专栏文章：拆开 cppcoro 给你看 · 第 0 篇

如果你写过 C++ 异步代码，一定踩过回调地狱的坑——3 层嵌套是起步，5 层不稀奇。C++20 的协程本该解决这个问题，但市面上的入门材料不是太浅（"协程就是可以暂停的函数"——然后呢？），就是太深（上来就讲对称转移和 IOCP），中间缺了一环。

这篇文章补上这一环。读完之后你不需要看任何其他"协程入门"材料，可以直接开始拆 cppcoro 的 `task<T>` 源码。

---

## 1. 协程到底解决了什么问题？

### 1.1 先看一段让你血压升高的代码

假设你要从数据库查用户、再查订单、最后发网络请求——三个操作都是异步的。传统回调写法长这样：

```cpp
// 回调地狱（callback hell）
void handle_request(int userId) {
    db.query_user(userId, [](User user) {       // 回调1
        db.query_orders(user.id, [](Orders orders) { // 回调2
            http.send(orders, [](Response rsp) {     // 回调3
                // 三层嵌套...实际项目里可能是五六层
                // 错误处理散落各处，上下文信息全部丢失
                finalize(rsp);
            });
        });
    });
}
```

你发现没有——**代码逻辑明明是线性的**（查用户 → 查订单 → 发请求），**但写出来是嵌套的**。三层还算友好，加到五六层的时候，你根本不想看自己的代码。

### 1.2 协程的解法

```cpp
task<> handle_request(int userId) {
    User user   = co_await db.query_user(userId);    // 等1
    Orders ords = co_await db.query_orders(user.id); // 等2
    Response r  = co_await http.send(ords);           // 等3
    finalize(r);
}
```

三个异步操作写成了三行顺序代码。`co_await` 的意思是：暂停这个函数，等异步操作完成后再从这里继续。

**编译器帮你把这三行代码变成状态机，但你写的时候完全不用关心。**

> 💡 **核心认知**：协程要解决的不是"怎么更快的执行"，而是"怎么写起来更自然"。它把异步代码伪装成了同步的样子。

---

## 2. C++ 协程的基本概念

### 2.1 协程是什么？

协程 = **一个可以暂停和恢复的函数**。

普通函数只能"一口气跑完"：

```
调用 → [执行...执行...执行] → 返回
        ↑ 中间停不下来
```

协程是这样的：

```
调用 → [执行] → 暂停 → (做别的事) → 恢复 → [执行] → 暂停 → 恢复 → [执行] → 结束
             ↑ 可以中途暂停                          ↑ 从暂停点继续
```

### 2.2 编译器怎么知道这是协程？

很简单：**函数体中只要出现了 `co_await`、`co_yield` 或 `co_return` 三个关键字中的任意一个**，编译器就自动把这个函数识别为协程。

```cpp
int normal_func() {          // 普通函数
    return 42;
}

task<int> coro_func() {      // 协程！因为有 co_return
    co_return 42;
}
```

返回值类型看都不用看——只要函数体里出现了三个 `co_*` 关键字之一，就是协程。

### 2.3 co_await / co_yield / co_return 的分工

```cpp
// co_return  — 协程结束，返回最终结果（只能用一次）
task<int> f1() {
    int x = 10;
    co_return x * 2;   // 结束协程，返回 20
}

// co_yield  — 产出一个中间值，但协程不结束（可以用多次）
generator<int> f2() {
    co_yield 1;   // 产出 1，暂停
    co_yield 2;   // 产出 2，暂停
    co_yield 3;   // 产出 3，暂停，然后结束
}

// co_await  — 等待一个异步操作完成，可能暂停
task<int> f3() {
    int x = co_await some_async_op();  // 等 some_async_op 完成，拿到结果继续
    co_return x;
}
```

三个关键字的语义一目了然。接下来进入本文最重要的部分——协程帧。

---

## 3. 协程帧：协程的"家"

### 3.1 普通函数为什么不能暂停

```cpp
int add(int a, int b) { return a + b; }

int main() {
    int x = add(1, 2);  // 调用过程：
    // 1. 栈上压入参数 a=1, b=2
    // 2. 压入返回地址
    // 3. 跳转到 add 函数体
    // 4. add 执行完，栈帧弹出、被复用
    // 5. 返回到 main
}
```

普通函数所有数据都在**线程调用栈**上。函数返回后栈帧就没了（内存还在，但会被下一个函数调用覆盖掉）。所以普通函数不能"暂停"——暂停意味着函数没返回但控制权回到了外层，回来看的时候栈帧已经被写废了。

### 3.2 协程的家：堆上的协程帧

协程需要暂停，意味着**它的局部变量、参数、执行状态必须在暂停期间存活**。所以协程帧不能放栈上，必须放**堆**上。

协程帧 = 编译器在堆上分配的一块内存，里面装了这些：

```
┌──────────────────────────────────────────┐
│              协程帧 (堆内存)               │
├──────────────────────────────────────────┤
│  promise_type 对象                       │  ← 协程的"控制面板"
│  (里面存了返回值、异常指针等)              │
├──────────────────────────────────────────┤
│  函数参数                               │  ← 值拷贝/移动进来的
│  (int userId 变成帧内的一个 int)          │
├──────────────────────────────────────────┤
│  局部变量                               │  ← 跨暂停点存活的变量
│  (User user; Order order; 等)            │
├──────────────────────────────────────────┤
│  当前执行到哪了（暂停点编号/状态机状态）    │  ← 恢复时知道从哪继续
└──────────────────────────────────────────┘
```

一个协程帧通常只有几十到几百字节。编译器在协程函数被调用时一次性分配（`operator new`），在协程结束时释放（`operator delete`）。

### 3.3 coroutine_handle：指向协程帧的"遥控器"

`coroutine_handle<P>` = 指向协程帧的轻量指针（就是一个 `void*`，8 字节）。

```cpp
coroutine_handle<my_promise> h;

h.resume();     // 恢复执行挂起的协程
h.done();       // 协程是否已结束？
h.destroy();    // 销毁协程帧（释放堆内存）
h.promise();    // 获取帧内 promise 对象的引用
h.address();    // 获取帧的原始地址
coroutine_handle<P>::from_promise(p);   // 从 promise 反向获取 handle
coroutine_handle<P>::from_address(ptr); // 从原始地址构造 handle
```

---

## 4. 手写最简协程——30 行理解全部机制

下面是我认为整篇文章最重要的部分。不依赖任何库，用纯 C++20 标准写一个最小协程。**你把它手抄一遍、编译运行一次，比读十篇文章都管用。**

### 4.1 需求：一个可暂停的计数器

我们希望这样用：

```cpp
auto gen = counter();   // 创建协程（不执行！）
gen.next();             // 执行到第一个 co_yield，返回 1
gen.next();             // 继续执行，返回 2
gen.next();             // 继续执行，返回 3，协程结束
```

### 4.2 完整实现（30 行）

```cpp
#include <iostream>
#include <coroutine>

// ===== 第1步：定义协程的返回值类型 =====
struct Generator 
{
    // 必须内嵌 promise_type，编译器通过它找到协程的"控制面板"
    struct promise_type 
    {
        int current_value;  // 存储 co_yield 产出的值

        // 协程一创建就挂起（不立即执行——惰性启动）
        std::suspend_always initial_suspend() { return {}; }

        // 协程结束后也挂起（让调用者去 destroy）
        std::suspend_always final_suspend() noexcept { return {}; }

        // 从 promise 构造返回给调用者的 Generator 对象
        Generator get_return_object() 
        {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // 处理 co_yield：存值，然后暂停协程
        std::suspend_always yield_value(int value) 
        {
            current_value = value;
            return {};
        }

        void return_void() {}  // 协程没有 co_return 值时的默认结束
        void unhandled_exception() { std::terminate(); }
    };

    // Generator 持有协程句柄（"遥控器"）
    std::coroutine_handle<promise_type> handle;

    Generator(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Generator() { if (handle) handle.destroy(); }

    // 用户接口：恢复协程直到下一个 co_yield 或结束
    bool next() 
    {
        if (!handle || handle.done()) return false;
        handle.resume();                 // 恢复执行！
        return !handle.done();
    }

    int value() { return handle.promise().current_value; }
};

// ===== 第2步：写协程函数 =====
Generator counter() {
    co_yield 1;   // 暂停，产出 1
    co_yield 2;   // 暂停，产出 2
    co_yield 3;   // 暂停，产出 3
    // 隐式 co_return
}

// ===== 第3步：使用协程 =====
int main() {
    auto gen = counter();
    while (gen.next()) 
    {
        std::cout << gen.value() << std::endl;
    }
    // 输出：1 2 3
}
```

### 4.3 逐行解释执行过程

| 步骤                    | 发生了什么                                                                                                                                                                   |
| ----------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `auto gen = counter();` | ①编译器在堆上分配协程帧 ②构造 promise_type ③调用 `get_return_object()` 得到 Generator 对象 ④执行 `initial_suspend()` → 返回 `suspend_always` → **协程挂起，body 还没执行！** |
| `gen.next()` 第1次      | 调用 `handle.resume()` → 协程体开始执行 → `co_yield 1` → promise 的 `yield_value(1)` 被调用 → `current_value=1` → 返回 `suspend_always` → 协程挂起 → `next()` 返回 true      |
| `gen.value()` 第1次     | 返回 `handle.promise().current_value` = 1                                                                                                                                    |
| `gen.next()` 第2次      | `handle.resume()` → 从上次暂停点继续 → `co_yield 2` → `current_value=2` → 协程挂起 → 返回 true                                                                               |
| `gen.value()` 第2次     | 返回 2                                                                                                                                                                       |
| `gen.next()` 第3次      | `handle.resume()` → `co_yield 3` → `current_value=3` → 协程挂起 → 返回 true                                                                                                  |
| `gen.value()` 第3次     | 返回 3                                                                                                                                                                       |
| `gen.next()` 第4次      | `handle.resume()` → 协程体到达结尾 → `return_void()` → `final_suspend()` → 返回 `suspend_always` → 协程挂起（但 `done() == true`） → `next()` 返回 false                     |
| `~Generator()`          | `handle.destroy()` → 释放堆上的协程帧                                                                                                                                        |

### 4.4 关键洞察

`initial_suspend()` 返回 `suspend_always` ——协程一开始就挂起。这叫做"**惰性启动**"（lazy start）。cppcoro 的 `task<T>` 正是这样设计的：调用协程函数只是创建了协程帧，实际执行要等到第一次 `co_await`。

`final_suspend()` 返回 `suspend_always` ——协程结束后仍然挂起。调用者可以用 `handle.done()` 检查它是否结束，然后手动 `destroy()`。这跟 `task<T>` 的 `FinalAwaiter` 是同一个思路——给调用者一个机会在销毁前取走结果。

---

## 5. co_await 的内部机制

### 5.1 编译器怎么处理 co_await

当你写 `co_await expr` 时，编译器把它展开成类似这样的代码：

```cpp
// 你写的：
int x = co_await some_awaitable();

// 编译器实际生成的（伪代码）：
auto&& awaiter = get_awaiter(some_awaitable());  // ①获取 awaiter 对象
if (awaiter.await_ready()) // ②已经就绪？
{
    // 就绪 → 不挂起，直接拿结果
} 
else 
{
    <保存当前协程的寄存器/局部变量到协程帧>        // ③保存现场
    <标记暂停点，以便恢复时从这继续>
    auto result = awaiter.await_suspend(当前协程句柄); // ④通知"我要挂了"
    // await_suspend 可能 resume 当前协程的句柄（由其他线程/事件循环调用）
    // 也可能返回另一个协程的句柄让编译器跳转（对称转移）
    <从协程帧恢复寄存器/局部变量>                  // ⑤恢复现场
}
auto result = awaiter.await_resume();             // ⑥获取最终结果
```

关键步骤分解：
- **① get_awaiter(x)**：获取 awaiter 对象。三种方式（按优先级）：`x.operator co_await()` > `operator co_await(x)` > x 本身就是 awaiter
- **② await_ready()**：快速检查——返回 true 就跳过等待，同步完成
- **④ await_suspend(handle)**：核心步骤。传入当前协程的句柄。返回 void / bool / coroutine_handle 三种可能
- **⑥ await_resume()**：拿到 `co_await` 表达式的最终返回值

### 5.2 await_suspend 的三种返回类型

| 返回类型             | 含义         | 挂起行为                                                     |
| -------------------- | ------------ | ------------------------------------------------------------ |
| `void`               | 无条件挂起   | 协程挂起，由 await_suspend 内部负责在合适时机 resume(handle) |
| `bool`               | 条件挂起     | true = 挂起，false = 不要挂起（同步完成了）                  |
| `coroutine_handle<>` | **对称转移** | 直接跳转到目标协程，不经过调度器，零额外栈帧                 |

对称转移是整个协程性能的杀手锏——它是一种"不增加栈帧的函数调用"，编译器直接生成一条 jmp 指令。cppcoro 中大量代码的条件编译就是在处理"编译器是否支持对称转移"的差异。有这个特性时几十行代码搞定，没有时要写几百行的原子状态机。

### 5.3 一个最简 awaiter 示例

```cpp
// 一个可被 co_await 的"延迟一帧"操作
struct DelayOneFrame 
{
    bool await_ready() { return false; }         // 永远不认为已就绪
    void await_suspend(std::coroutine_handle<> h) 
    {
        // 把协程句柄丢进队列，下个事件循环再恢复它
        global_queue.push(h);
    }
    void await_resume() {}                       // 不返回值
};

// 使用
task<> my_coro() 
{
    std::cout << "A";
    co_await DelayOneFrame{};  // 暂停，下个循环继续
    std::cout << "B";
    // 输出: A (一帧后) B
}
```

把协程句柄放进队列、由事件循环取出并 resume——这就是**所有协程 I/O 框架的基础模型**。

---

## 6. promise_type 的 7 个接口完整说明

当你写 `task<int> my_coro() { co_return 42; }` 时，编译器要求 `task<int>` 必须内嵌 `task<int>::promise_type`。这个类型需要/可以有以下成员：

| 接口                    | 何时被调用              | 必须/可选              | 典型实现                                                                |
| ----------------------- | ----------------------- | ---------------------- | ----------------------------------------------------------------------- |
| `get_return_object()`   | 协程帧构造后            | **必须**               | 返回给调用者的 `task<T>` 对象                                           |
| `initial_suspend()`     | 协程体执行前            | **必须**               | 返回 `suspend_always` (惰性) 或 `suspend_never` (立即执行)              |
| `final_suspend()`       | 协程体执行完毕后        | **必须**               | 返回 `suspend_always` 让调用者手动清理，或返回自定义 awaiter 转移控制权 |
| `return_value(v)`       | `co_return expr;`       | 非 void 协程**必须**   | 把返回值存入 promise                                                    |
| `return_void()`         | `co_return;` 或函数结尾 | void 协程**必须**      | 标记完成（通常为空）                                                    |
| `yield_value(v)`        | `co_yield expr;`        | 用 co_yield 时**必须** | 存中间值，返回 suspend 策略                                             |
| `unhandled_exception()` | 协程体抛出未捕获异常    | **必须**               | 存 `std::current_exception()`                                           |

### 6.1 构造函数和 operator new 的特殊规则

promise_type 的构造函数参数决定了协程函数的参数如何传递：

```cpp
task<int> my_coro(int a, std::string b);  // 协程函数签名

// 编译器生成的等价代码：
// new_coro_frame(a, b)
//   → 如果 promise_type 有匹配的构造函数 → 用它构造
//   → 否则用默认构造函数，参数单独存入帧
```

编译器也可能将 `operator new` 的调用替换为帧内分配（C++23 的静态协程帧优化），但这对于理解 cppcoro 不重要。

---

## 7. 内存模型总结

```
调用 my_coro(arg1, arg2) 时:

  堆
  ┌─────────────────────────────────────────┐
  │             协程帧 (~200字节)             │
  │                                          │
  │  ┌─────────────────────────────────┐    │
  │  │ promise_type                     │    │
  │  │ ├─ 返回值/异常 (union)            │    │
  │  │ ├─ result_type 标记              │    │
  │  │ ├─ m_continuation (等待者句柄)    │    │
  │  │ └─ [非对称转移] m_state (原子锁)  │    │
  │  └─────────────────────────────────┘    │
  │                                          │
  │  ┌─────────────────────────────────┐    │
  │  │ 函数参数（拷贝/移动进来的）        │    │
  │  │ ├─ arg1                         │    │
  │  │ └─ arg2                         │    │
  │  └─────────────────────────────────┘    │
  │                                          │
  │  ┌─────────────────────────────────┐    │
  │  │ 局部变量（跨暂停点存活的）         │    │
  │  │ ├─ 变量1                         │    │
  │  │ └─ 变量2                         │    │
  │  └─────────────────────────────────┘    │
  │                                          │
  │  ┌─────────────────────────────────┐    │
  │  │ 暂停点编号（恢复时从哪继续）       │    │
  │  └─────────────────────────────────┘    │
  └─────────────────────────────────────────┘
         ↑
         │  .address()
  ┌──────┴──────┐
  │ coroutine_  │  调用者栈上（8 字节）
  │  handle<P>  │
  └─────────────┘

协程帧的地址就是 coroutine_handle 的值（void*）。
promise() 在帧内的偏移是固定的（编译器决定的）。
```

---

## 8. 试试回答这 7 个问题

读完本篇后，你应该能答上：

- [ ] 协程帧为什么必须在堆上？
- [ ] `initial_suspend` 和 `final_suspend` 分别什么时候被调用？
- [ ] `co_await` 的编译器展开步骤（get_awaiter → await_ready → await_suspend → await_resume）
- [ ] `await_suspend` 三种返回类型的含义分别是什么？
- [ ] `promise_type` 的 `get_return_object()` 返回了什么？
- [ ] `coroutine_handle` 提供了哪些操作？
- [ ] 协程帧里面存了哪些东西？

**如果能答上 5 个，就可以开始读 cppcoro 的 `task<T>` 源码了。下一篇文章我们直接拆开看。**

---

## 9. 动手练习

最有效的方式是自己写一遍。三个练习，难度递增：

**练习 1（10 分钟）**：把第 4 节的 `Generator` 手抄一遍，在本地编译运行。C++20 标准，任何支持协程的编译器都行（GCC 10+/Clang 14+/MSVC 2022+）。

**练习 2（20 分钟）**：给 Generator 加上异常处理——协程体里抛异常时，`next()` 应该重新抛出来。

**练习 3（30 分钟）**：手写一个最小化的 `task<T>`——支持 `co_return` 和 `co_await`（但不能 await 其他 task），用 `sync_wait` 驱动。

---

> 下一篇：[cppcoro 的 task\<T\> 到底做了什么？从 25 行到 90 行，5 版迭代拆开看](01-协程基础机制-task-lifetime.md)
