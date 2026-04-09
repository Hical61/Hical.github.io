+++
date = '2026-04-08'
draft = false
title = '深入学习 C++20 协程（Coroutines）'
categories = ["C++"]
tags = ["C++", "C++20", "协程", "Coroutines", "异步编程", "学习笔记"]
description = "全面解析 C++20 协程：从设计动机、编译器变换模型、三大关键字（co_await/co_yield/co_return）、Promise 与 Awaiter 协议，到自定义协程类型、游戏服务器实战场景，一篇掌握现代 C++ 无栈协程机制。"
+++


# 深入学习 C++20 协程（Coroutines）

> 头文件：`<coroutine>`
> 命名空间：`std`
> 编译器要求：GCC 11+ / Clang 14+ / MSVC 19.28+（均需 `-std=c++20` 或以上）
> 注意：GCC 10 / Clang 8~13 可通过 `-fcoroutines` 和 `<experimental/coroutine>` 使用实验性支持

---

## 一、为什么需要协程？

### 1.1 异步编程的传统痛点

游戏服务器中充斥着异步操作——数据库查询、网络 I/O、定时器回调。传统方案各有各的痛：

**方案 A：回调地狱（Callback Hell）**

```cpp
void HandleLogin(Connection* conn, const LoginPacket& pkt) {
    // 第1步：查询数据库验证账号
    dbManager->QueryAsync("SELECT * FROM accounts WHERE name=?", pkt.name,
        [conn, pkt](const DBResult& result) {
            if (!result.ok) { conn->SendError("DB错误"); return; }
            // 第2步：查询角色列表
            dbManager->QueryAsync("SELECT * FROM characters WHERE account_id=?", result.accountId,
                [conn](const DBResult& charResult) {
                    if (!charResult.ok) { conn->SendError("DB错误"); return; }
                    // 第3步：加载角色数据
                    dbManager->QueryAsync("SELECT * FROM inventory WHERE char_id=?", charResult.charId,
                        [conn, charResult](const DBResult& invResult) {
                            // 第4步：终于可以发送登录成功了...
                            conn->SendLoginSuccess(charResult, invResult);
                        });
                });
        });
}
```

**方案 B：状态机（State Machine）**

```cpp
class LoginHandler {
    enum State { INIT, WAITING_ACCOUNT, WAITING_CHARS, WAITING_INVENTORY, DONE };
    State state_ = INIT;
    // 每个阶段的中间数据都要存为成员变量
    int accountId_;
    int charId_;
    DBResult charResult_;

public:
    void OnDBResult(const DBResult& result) {
        switch (state_) {
            case WAITING_ACCOUNT:
                accountId_ = result.accountId;
                QueryCharacters(accountId_);
                state_ = WAITING_CHARS;
                break;
            case WAITING_CHARS:
                charId_ = result.charId;
                charResult_ = result;
                QueryInventory(charId_);
                state_ = WAITING_INVENTORY;
                break;
            case WAITING_INVENTORY:
                SendLoginSuccess(charResult_, result);
                state_ = DONE;
                break;
        }
    }
};
```

**核心问题：**

| 痛点                 | 回调方案                  | 状态机方案                   |
| -------------------- | ------------------------- | ---------------------------- |
| **代码可读性**       | 嵌套深、逻辑碎片化        | 流程分散在 switch 各分支     |
| **错误处理**         | 每层回调都要写错误处理    | 需要在每个状态处理异常       |
| **局部变量生命周期** | 需要 capture 或提升为成员 | 所有中间状态都要存为成员变量 |
| **调试难度**         | 调用栈看不到完整流程      | 状态转换逻辑难以追踪         |
| **组合性**           | 回调难以组合和复用        | 状态机难以嵌套               |

### 1.2 协程的解法：用同步的写法做异步的事

C++20 协程让你写出**看起来同步、实际异步**的代码：

```cpp
Task<void> HandleLogin(Connection* conn, const LoginPacket& pkt) {
    // 第1步：查询账号——挂起，等 DB 返回后恢复
    auto account = co_await dbManager->QueryAsync("SELECT * FROM accounts WHERE name=?", pkt.name);
    if (!account.ok) { conn->SendError("DB错误"); co_return; }

    // 第2步：查询角色列表——再次挂起
    auto chars = co_await dbManager->QueryAsync("SELECT * FROM characters WHERE account_id=?", account.id);
    if (!chars.ok) { conn->SendError("DB错误"); co_return; }

    // 第3步：加载背包——再次挂起
    auto inventory = co_await dbManager->QueryAsync("SELECT * FROM inventory WHERE char_id=?", chars.charId);

    // 第4步：全部完成，发送响应
    conn->SendLoginSuccess(chars, inventory);
}
```

**一句话总结：协程把"异步等待"从回调/状态机的控制流反转，恢复为线性的顺序代码，编译器帮你管理挂起/恢复的状态保存。**

---

## 二、C++20 协程核心概念

### 2.1 什么是无栈协程

C++20 的协程是**无栈协程（Stackless Coroutine）**——协程的状态（局部变量、挂起点）保存在**堆上的协程帧**中，而非像有栈协程那样拥有独立的调用栈。

**有栈 vs 无栈对比：**

```
有栈协程（如 Boost.Context、ucontext）:
┌─────────────────┐    ┌─────────────────┐
│  协程 A 的完整栈  │    │  协程 B 的完整栈  │
│  (通常 64KB~1MB) │    │  (通常 64KB~1MB) │
│  ┌─────────────┐│    │  ┌─────────────┐│
│  │ 栈帧 3      ││    │  │ 栈帧 2      ││
│  │ 栈帧 2      ││    │  │ 栈帧 1      ││
│  │ 栈帧 1      ││    │  │             ││
│  └─────────────┘│    │  └─────────────┘│
└─────────────────┘    └─────────────────┘
切换方式：保存/恢复整个 CPU 寄存器 + 栈指针

无栈协程（C++20）:
┌────────────────┐    ┌────────────────┐
│ 协程帧 A (堆上) │    │ 协程帧 B (堆上) │
│ - 局部变量      │    │ - 局部变量      │
│ - 挂起点索引    │    │ - 挂起点索引    │
│ - promise 对象  │    │ - promise 对象  │
│ (按需分配大小)  │    │ (按需分配大小)  │
└────────────────┘    └────────────────┘
切换方式：普通函数调用/返回（通过 coroutine_handle::resume()）
```

**无栈协程的优势：**
- 协程帧大小按需分配（几十到几百字节），而非固定几百 KB 的栈
- 数万个协程的内存开销可控
- 编译器可以优化掉协程帧分配（HALO 优化）
- 不需要平台相关的汇编/上下文切换代码

### 2.2 三大关键字

C++20 引入了三个关键字，**函数体内出现任意一个，该函数就成为协程：**

| 关键字      | 语义                       | 典型用途           |
| ----------- | -------------------------- | ------------------ |
| `co_await`  | 挂起协程，等待异步操作完成 | 异步 I/O、定时等待 |
| `co_yield`  | 挂起协程并产出一个值       | 生成器、数据流     |
| `co_return` | 结束协程并（可选）返回值   | 协程正常结束       |

```cpp
// co_await: 异步等待
Task<int> AsyncAdd(int a, int b) {
    co_await SomeAsyncWork();
    co_return a + b;
}

// co_yield: 生成器
Generator<int> Range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;  // 产出一个值后挂起
    }
}

// co_return: 返回结果
Task<std::string> GetName() {
    co_return "Hello, Coroutine!";
}
```

### 2.3 协程的核心组件

C++20 协程由**四个核心角色**协作：

```
        调用者（Caller）
           │
           │ 调用协程函数
           ▼
    ┌──────────────────┐
    │  协程返回类型      │ ← 如 Task<T>、Generator<T>
    │  (Return Object)  │    调用者通过它与协程交互
    └────────┬─────────┘
             │ 内部关联
    ┌────────▼─────────┐
    │  promise_type     │ ← 协程的"控制面板"
    │  (Promise 对象)   │    控制协程的生命周期和值传递
    └────────┬─────────┘
             │ 通过 coroutine_handle 关联
    ┌────────▼─────────┐
    │  协程帧            │ ← 编译器生成的状态存储
    │  (Coroutine Frame)│    包含局部变量、挂起点、promise
    └──────────────────┘

    ┌──────────────────┐
    │  Awaiter 对象      │ ← co_await 的操作数
    │  (Awaitable)      │    决定是否挂起、如何恢复
    └──────────────────┘
```

---

## 三、编译器变换模型

理解协程最关键的一步是理解**编译器对协程函数做了什么变换**。

### 3.1 原始协程代码

```cpp
Task<int> Compute(int x) {
    int a = co_await AsyncGetValue();
    int b = co_await AsyncGetValue();
    co_return a + b + x;
}
```

### 3.2 编译器变换后的伪代码

```cpp
Task<int> Compute(int x) {
    // ① 分配协程帧（堆上）
    auto* frame = new __coroutine_frame_Compute;
    frame->x = x;  // 拷贝参数到帧中

    // ② 在帧内构造 promise 对象
    auto& promise = frame->promise;

    // ③ 获取返回对象（返回给调用者）
    Task<int> returnObject = promise.get_return_object();

    // ④ 初始挂起点
    co_await promise.initial_suspend();  // 通常返回 suspend_always 或 suspend_never

    try {
        // ⑤ 原始函数体（每个 co_await 是一个挂起/恢复点）
        // --- 挂起点 1 ---
        int a = co_await AsyncGetValue();
        // --- 挂起点 2 ---
        int b = co_await AsyncGetValue();

        // ⑥ co_return → 调用 promise.return_value(a + b + x)
        promise.return_value(a + b + x);
    } catch (...) {
        // ⑦ 异常处理
        promise.unhandled_exception();
    }

    // ⑧ 最终挂起点
    co_await promise.final_suspend();

    // ⑨ 如果没在 final_suspend 挂起，销毁协程帧
    // delete frame;

    return returnObject;  // 实际上在 ③ 之后就已经返回了
}
```

### 3.3 挂起/恢复的本质

每个 `co_await` 点，编译器做的事情：

```
co_await expr;

展开为：
┌─────────────────────────────────────────────────────┐
│ 1. 获取 Awaiter 对象                                  │
│    auto&& awaiter = get_awaiter(expr);               │
│                                                     │
│ 2. 检查是否需要挂起                                    │
│    if (!awaiter.await_ready()) {                     │
│        // 3. 保存当前状态到协程帧                      │
│        //    （局部变量、挂起点索引）                   │
│        // 4. 调用 await_suspend                       │
│        awaiter.await_suspend(coroutine_handle);      │
│        // 5. 控制权返回给调用者/恢复者                  │
│        return;  // ← 这就是"挂起"                     │
│    }                                                 │
│ 6. 从 await_resume 获取结果                            │
│    auto result = awaiter.await_resume();             │
└─────────────────────────────────────────────────────┘
```

**恢复时**（某处调用了 `handle.resume()`），执行流直接跳到上次挂起点之后，从 `await_resume()` 获取结果，继续执行。

---

## 四、Promise Type — 协程的控制面板

### 4.1 Promise 协议全貌

`promise_type` 是协程最核心的定制点。编译器通过协程的返回类型找到 `promise_type`：

```cpp
// 编译器查找规则：
// 协程返回类型为 Task<int> → 查找 Task<int>::promise_type
// 也可以通过 std::coroutine_traits 特化来指定
```

```cpp
struct MyPromise {
    // ═══ 必须实现 ═══

    // 构造返回对象（调用者拿到的东西）
    ReturnType get_return_object();

    // 初始挂起：suspend_always = 惰性启动，suspend_never = 立即执行
    std::suspend_always initial_suspend() noexcept;

    // 最终挂起：suspend_always = 调用者负责销毁，suspend_never = 自动销毁
    std::suspend_always final_suspend() noexcept;

    // 未捕获异常的处理
    void unhandled_exception();

    // 二选一：
    void return_void();             // 用于 co_return; 或协程体结束
    void return_value(T value);     // 用于 co_return expr;

    // ═══ 可选实现 ═══

    // 自定义 co_yield 行为
    auto yield_value(T value);

    // 自定义 co_await 转换
    auto await_transform(T expr);

    // 自定义协程帧分配
    static void* operator new(size_t size);
    static void operator delete(void* ptr, size_t size);

    // 优化：如果返回 true，跳过堆分配（提示编译器可以 HALO 优化）
    static auto get_return_object_on_allocation_failure();
};
```

### 4.2 initial_suspend 的选择

```cpp
// 惰性启动（Lazy）—— 创建后不立即执行，等调用者 resume
std::suspend_always initial_suspend() noexcept { return {}; }

// 立即启动（Eager）—— 创建后立即执行到第一个 co_await
std::suspend_never initial_suspend() noexcept { return {}; }
```

**游戏服务器中的选择：**
- **Task（异步任务）**：通常用 `suspend_always`（惰性），由调度器决定何时启动
- **Generator（生成器）**：通常用 `suspend_always`（惰性），调用者 pull 数据时才执行
- **Fire-and-forget**：用 `suspend_never`（立即），启动后自行运行

### 4.3 final_suspend 的选择

```cpp
// 挂起在最终点 —— 协程帧不会自动销毁，调用者需要手动 destroy()
std::suspend_always final_suspend() noexcept { return {}; }

// 不挂起 —— 协程结束后自动销毁帧，但调用者不能再访问 handle
std::suspend_never final_suspend() noexcept { return {}; }
```

**关键规则：`final_suspend()` 必须是 `noexcept`，因为此时异常已经没地方去了。**

---

## 五、Awaiter — co_await 的底层机制

### 5.1 Awaiter 三件套

任何类型只要实现以下三个方法，就可以被 `co_await`：

```cpp
struct MyAwaiter {
    // 快速路径：结果已经就绪？不需要挂起
    bool await_ready() const noexcept;

    // 挂起时调用：安排恢复工作（注册回调、投递到其他线程等）
    // 返回值有三种形式（见下文）
    void/bool/coroutine_handle<> await_suspend(std::coroutine_handle<> h);

    // 恢复后调用：返回 co_await 表达式的结果
    T await_resume();
};
```

### 5.2 await_suspend 的三种返回类型

`await_suspend` 的返回类型决定了挂起后的行为：

```cpp
// 形式 1：void —— 无条件挂起
void await_suspend(std::coroutine_handle<> h) {
    // 安排某个时机调用 h.resume()
    scheduler->enqueue(h);
}

// 形式 2：bool —— 条件挂起
bool await_suspend(std::coroutine_handle<> h) {
    // 返回 true  → 挂起（控制权回到调用者）
    // 返回 false → 不挂起（立即恢复执行，等价于 await_ready 返回 true）
    return !result_ready_.load();
}

// 形式 3：coroutine_handle<> —— 对称转移（Symmetric Transfer）
std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
    // 返回的 handle 会被立即 resume —— 不经过调用栈
    // 这避免了递归 resume 导致的栈溢出
    h.destroy();
    return continuation_;  // 直接跳到等待者，不回到调用者
}
```

**对称转移（Symmetric Transfer）** 是 C++20 协程的一个关键特性：

```
普通恢复（resume 嵌套）:              对称转移:
Caller                              Caller
  └→ A.resume()                       └→ A.resume()
       └→ B.resume()                       A 返回 B 的 handle
            └→ C.resume()                  └→ B.resume()
                 └→ ...                        B 返回 C 的 handle
            栈深度持续增长！                    └→ C.resume()
                                               栈深度恒定 = 1
```

### 5.3 标准库提供的两个 Awaiter

```cpp
// 永远不挂起——co_await 直接跳过
struct suspend_never {
    bool await_ready() const noexcept { return true; }   // 已就绪
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

// 永远挂起——co_await 一定会挂起
struct suspend_always {
    bool await_ready() const noexcept { return false; }  // 未就绪
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};
```

---

## 六、从零实现协程类型

### 6.1 实现 Generator — 惰性序列生成器

```cpp
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>

template <typename T>
class Generator {
public:
    struct promise_type {
        std::optional<T> currentValue;
        std::exception_ptr exception;

        Generator get_return_object() {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // 惰性启动：创建后暂停，等调用者 pull
        std::suspend_always initial_suspend() noexcept { return {}; }

        // 结束时挂起，让调用者有机会读取最后状态
        std::suspend_always final_suspend() noexcept { return {}; }

        // co_yield value → 存储值并挂起
        std::suspend_always yield_value(T value) {
            currentValue = std::move(value);
            return {};
        }

        void return_void() {}

        void unhandled_exception() {
            exception = std::current_exception();
        }
    };

private:
    std::coroutine_handle<promise_type> handle_;

public:
    explicit Generator(std::coroutine_handle<promise_type> h) : handle_(h) {}
    ~Generator() { if (handle_) handle_.destroy(); }

    // 禁止拷贝
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    // 支持移动
    Generator(Generator&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Generator& operator=(Generator&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    // 迭代器接口——支持 range-for
    struct Iterator {
        std::coroutine_handle<promise_type> handle;

        Iterator& operator++() {
            handle.resume();
            if (handle.done()) {
                // 协程结束，检查是否有未处理异常
                if (handle.promise().exception) {
                    std::rethrow_exception(handle.promise().exception);
                }
            }
            return *this;
        }

        const T& operator*() const { return *handle.promise().currentValue; }
        bool operator==(std::default_sentinel_t) const { return handle.done(); }
    };

    Iterator begin() {
        handle_.resume();  // 推进到第一个 co_yield
        return Iterator{handle_};
    }

    std::default_sentinel_t end() { return {}; }
};
```

**使用示例：**

```cpp
// 斐波那契数列生成器——无限序列，惰性求值
Generator<uint64_t> Fibonacci() {
    uint64_t a = 0, b = 1;
    while (true) {
        co_yield a;
        auto next = a + b;
        a = b;
        b = next;
    }
}

// 使用
auto fib = Fibonacci();
int count = 0;
for (auto val : fib) {
    printf("%llu ", val);  // 0 1 1 2 3 5 8 13 21 34
    if (++count >= 10) break;
}
```

### 6.2 实现 Task — 异步任务

```cpp
#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

template <typename T>
class Task {
public:
    struct promise_type {
        std::optional<T> result;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;  // 等待此 Task 的协程

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // 惰性启动
        std::suspend_always initial_suspend() noexcept { return {}; }

        // 最终挂起时，通过对称转移恢复等待者
        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() const noexcept { return false; }
                // 对称转移：直接跳到等待此 Task 的协程
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    auto continuation = h.promise().continuation;
                    return continuation ? continuation : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }

        void return_value(T value) {
            result = std::move(value);
        }

        void unhandled_exception() {
            exception = std::current_exception();
        }
    };

    // co_await Task<T> 时使用的 Awaiter
    auto operator co_await() const noexcept {
        struct TaskAwaiter {
            std::coroutine_handle<promise_type> handle;

            bool await_ready() const noexcept {
                return handle.done();
            }

            // 当前协程挂起，记录为 continuation，然后启动目标 Task
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                handle.promise().continuation = caller;
                return handle;  // 对称转移：立即开始执行此 Task
            }

            T await_resume() {
                if (handle.promise().exception) {
                    std::rethrow_exception(handle.promise().exception);
                }
                return std::move(*handle.promise().result);
            }
        };
        return TaskAwaiter{handle_};
    }

    // 同步等待（在非协程上下文中使用）
    T SyncWait() {
        handle_.resume();
        // 注意：这只适用于不涉及真正异步 I/O 的场景
        // 真正的异步场景需要事件循环驱动
        if (handle_.promise().exception) {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().result);
    }

private:
    std::coroutine_handle<promise_type> handle_;

public:
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
};

// void 特化
template <>
class Task<void> {
public:
    struct promise_type {
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    auto continuation = h.promise().continuation;
                    return continuation ? continuation : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }

        void return_void() {}

        void unhandled_exception() {
            exception = std::current_exception();
        }
    };

    auto operator co_await() const noexcept {
        struct TaskAwaiter {
            std::coroutine_handle<promise_type> handle;
            bool await_ready() const noexcept { return handle.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                handle.promise().continuation = caller;
                return handle;
            }
            void await_resume() {
                if (handle.promise().exception) {
                    std::rethrow_exception(handle.promise().exception);
                }
            }
        };
        return TaskAwaiter{handle_};
    }

private:
    std::coroutine_handle<promise_type> handle_;

public:
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
};
```

**使用示例：**

```cpp
Task<int> ComputeAsync(int x) {
    co_return x * 2;
}

Task<int> CombineAsync() {
    int a = co_await ComputeAsync(10);  // a = 20
    int b = co_await ComputeAsync(21);  // b = 42
    co_return a + b;                     // 62
}
```

### 6.3 实现自定义 Awaiter — 异步定时器

```cpp
#include <chrono>
#include <coroutine>

// 假设存在一个定时器管理器
class TimerManager {
public:
    using Callback = std::function<void()>;
    void AddTimer(int delayMs, Callback cb);
    static TimerManager& Instance();
};

// co_await SleepFor(100ms) —— 挂起协程，100ms 后恢复
struct SleepFor {
    std::chrono::milliseconds duration;

    bool await_ready() const noexcept {
        return duration <= std::chrono::milliseconds::zero();  // 0ms 不需要挂起
    }

    void await_suspend(std::coroutine_handle<> h) const {
        // 注册定时器，到期后恢复协程
        TimerManager::Instance().AddTimer(
            static_cast<int>(duration.count()),
            [h]() mutable { h.resume(); }
        );
    }

    void await_resume() const noexcept {}
};

// 使用
Task<void> DelayedGreeting() {
    printf("开始等待...\n");
    co_await SleepFor{std::chrono::milliseconds{1000}};
    printf("1秒后：Hello!\n");
    co_await SleepFor{std::chrono::milliseconds{2000}};
    printf("又过了2秒：World!\n");
}
```

---

## 七、await_transform — 协程级别的 co_await 拦截

`promise_type` 可以定义 `await_transform` 来拦截和转换所有 `co_await` 表达式：

```cpp
struct ScheduledPromise {
    // 拦截所有 co_await 表达式
    template <typename T>
    auto await_transform(T&& awaitable) {
        // 可以在这里做日志、权限检查、调度等
        return std::forward<T>(awaitable);  // 直接透传
    }

    // 特殊处理：禁止在此协程中 co_await 某些类型
    auto await_transform(std::suspend_always) = delete;  // 编译错误！

    // 特殊处理：自动注入调度逻辑
    auto await_transform(SwitchToThread target) {
        struct ThreadSwitchAwaiter {
            SwitchToThread target;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) const {
                target.threadPool->enqueue([h] { h.resume(); });
            }
            void await_resume() const noexcept {}
        };
        return ThreadSwitchAwaiter{target};
    }
};
```

**典型用途：**
- 在游戏服务器中，确保协程恢复时回到正确的线程/EventLoop
- 自动为所有异步操作添加超时检测
- 禁止协程等待不安全的类型

---

## 八、游戏服务器实战场景

### 8.1 场景一：异步数据库查询

```cpp
// 将回调式 DB 接口包装为可 co_await 的
template <typename ResultType>
class DBQueryAwaiter {
    std::string sql_;
    DBManager* db_;
    std::optional<ResultType> result_;
    std::exception_ptr exception_;
    std::coroutine_handle<> handle_;

public:
    DBQueryAwaiter(DBManager* db, std::string sql)
        : db_(db), sql_(std::move(sql)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        db_->QueryAsync(sql_, [this](const DBResult& res) {
            if (res.ok) {
                result_ = res.As<ResultType>();
            } else {
                exception_ = std::make_exception_ptr(
                    std::runtime_error("DB query failed: " + res.error));
            }
            // 恢复协程——注意要回到正确的线程
            handle_.resume();
        });
    }

    ResultType await_resume() {
        if (exception_) std::rethrow_exception(exception_);
        return std::move(*result_);
    }
};

// 封装为便利函数
template <typename T>
DBQueryAwaiter<T> QueryDB(DBManager* db, std::string sql) {
    return DBQueryAwaiter<T>{db, std::move(sql)};
}

// 业务代码——读起来就像同步的
Task<void> HandlePlayerLogin(Connection* conn, int accountId) {
    // 每一步都是异步的，但代码是线性的
    auto account = co_await QueryDB<AccountInfo>(db,
        "SELECT * FROM accounts WHERE id=" + std::to_string(accountId));

    if (account.banned) {
        conn->SendError("账号已封禁");
        co_return;
    }

    auto characters = co_await QueryDB<std::vector<CharInfo>>(db,
        "SELECT * FROM characters WHERE account_id=" + std::to_string(accountId));

    conn->SendCharacterList(characters);
}
```

### 8.2 场景二：协程化的游戏 AI 行为树

传统 AI 行为树需要手写复杂的状态机，协程可以大幅简化：

```cpp
// NPC 巡逻行为——看起来像同步脚本
Task<void> PatrolBehavior(NPC* npc) {
    while (npc->IsAlive()) {
        // 巡逻路径上的每个点
        for (const auto& waypoint : npc->GetPatrolPath()) {
            // 移动到目标点——可能需要多帧
            co_await MoveTo(npc, waypoint);

            // 到达后等待一会儿
            co_await SleepFor{std::chrono::seconds{2}};

            // 检查是否发现敌人
            auto* enemy = npc->DetectEnemy(500.0f);  // 500 范围内搜索
            if (enemy) {
                // 切换到战斗行为
                co_await ChaseBehavior(npc, enemy);
                break;  // 战斗结束后重新开始巡逻
            }
        }
    }
}

Task<void> ChaseBehavior(NPC* npc, Entity* target) {
    while (target->IsAlive() && npc->IsAlive()) {
        float distance = npc->DistanceTo(target);

        if (distance > npc->GetAttackRange()) {
            // 追击——移动一帧的距离
            co_await MoveToward(npc, target->GetPosition(), npc->GetMoveSpeed());
        } else {
            // 在攻击范围内——执行攻击
            co_await PerformAttack(npc, target);
            // 攻击冷却
            co_await SleepFor{std::chrono::milliseconds{npc->GetAttackCooldown()}};
        }

        // 超出追击距离，放弃
        if (distance > 2000.0f) {
            npc->Shout("逃跑了吗...");
            co_return;
        }
    }
}

// MoveTo 的实现——每帧推进一步，到达后恢复协程
struct MoveTo {
    NPC* npc;
    Vec3 target;

    bool await_ready() const noexcept {
        return npc->GetPosition().DistanceTo(target) < 1.0f;  // 已到达
    }

    void await_suspend(std::coroutine_handle<> h) {
        // 注册到移动系统，每帧 tick 推进位置，到达后 resume
        npc->GetMoveSystem()->StartMove(npc, target, [h] {
            h.resume();
        });
    }

    void await_resume() noexcept {}
};
```

### 8.3 场景三：协程化的任务/剧情系统

```cpp
// 一个新手引导任务链
Task<void> BeginnerQuest(Player* player) {
    // 第1步：显示对话
    co_await ShowDialog(player, "NPC_村长", "欢迎来到远征世界！请先去消灭5只野猪。");

    // 第2步：等待玩家完成击杀目标
    co_await WaitForKill(player, MOB_WILD_BOAR, 5);

    // 第3步：给予奖励
    player->AddItem(ITEM_IRON_SWORD, 1);
    player->AddExp(100);

    co_await ShowDialog(player, "NPC_村长", "干得好！这把铁剑送给你。");

    // 第4步：引导去下一个 NPC
    co_await ShowDialog(player, "NPC_村长", "去找铁匠学习锻造吧。");
    co_await WaitForTalkTo(player, "NPC_铁匠");

    co_await ShowDialog(player, "NPC_铁匠", "来，我教你打造第一件装备。");

    // 任务完成
    player->CompleteQuest(QUEST_BEGINNER);
}

// WaitForKill 的实现
struct WaitForKill {
    Player* player;
    int mobId;
    int count;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        int killed = 0;
        // 监听玩家击杀事件
        player->OnKill.Subscribe([=, &killed](int killedMobId) mutable {
            if (killedMobId == mobId) {
                ++killed;
                player->SendQuestProgress(killed, count);
                if (killed >= count) {
                    h.resume();  // 击杀够了，恢复协程
                    return false; // 取消订阅
                }
            }
            return true;  // 继续监听
        });
    }

    void await_resume() noexcept {}
};
```

### 8.4 场景四：并发等待多个异步操作

```cpp
// WhenAll —— 等待多个 Task 全部完成
template <typename... Tasks>
Task<std::tuple<typename Tasks::value_type...>> WhenAll(Tasks... tasks);

// 使用示例：并行加载玩家数据
Task<void> LoadPlayerData(Player* player) {
    // 三个 DB 查询并发执行
    auto [bag, skills, quests] = co_await WhenAll(
        QueryDB<BagData>(db, "SELECT * FROM bags WHERE player_id=" + playerId),
        QueryDB<SkillData>(db, "SELECT * FROM skills WHERE player_id=" + playerId),
        QueryDB<QuestData>(db, "SELECT * FROM quests WHERE player_id=" + playerId)
    );

    player->InitBag(bag);
    player->InitSkills(skills);
    player->InitQuests(quests);
}
```

---

## 九、协程帧分配与 HALO 优化

### 9.1 协程帧的堆分配

默认情况下，每次创建协程都会在堆上分配协程帧：

```cpp
// 编译器生成的帧分配
auto* frame = ::operator new(frame_size);
// frame_size = sizeof(promise) + sizeof(所有局部变量) + 对齐填充 + 编译器簿记
```

可以在 `promise_type` 中自定义分配：

```cpp
struct promise_type {
    // 自定义帧分配——例如从内存池分配
    static void* operator new(size_t size) {
        return CoroutinePool::Allocate(size);
    }

    static void operator delete(void* ptr, size_t size) {
        CoroutinePool::Deallocate(ptr, size);
    }

    // 带参数的 operator new——可以拿到协程函数的参数
    // 如果协程参数中有 allocator/resource，可以用它来分配
    static void* operator new(size_t size, Player* player, auto&&...) {
        return player->GetArena()->allocate(size, alignof(std::max_align_t));
    }
};
```

### 9.2 HALO 优化（Heap Allocation eLision Optimization）

编译器可以在某些条件下完全消除协程帧的堆分配，将帧嵌入调用者的栈帧中：

```
HALO 优化条件（非标准，因实现而异）:
1. 协程的生命周期完全被调用者包含
2. 编译器能在编译期确定协程帧大小
3. 协程没有逃逸到调用者之外（handle 没被保存到全局/堆上）
```

```cpp
// 容易被 HALO 优化的模式
Task<int> Inner() { co_return 42; }

Task<int> Outer() {
    int x = co_await Inner();  // Inner 的帧可能被消除
    co_return x;
}

// 不容易被优化的模式
Task<int> EscapingCoroutine() {
    auto task = Inner();
    globalStorage.push_back(std::move(task));  // handle 逃逸
    co_return 0;
}
```

---

## 十、使用注意事项与陷阱

### 10.1 悬垂引用——协程的头号陷阱

```cpp
// 致命错误：引用参数在协程挂起后可能已失效
Task<void> ProcessData(const std::vector<int>& data) {
    co_await SomeAsyncWork();
    // 恢复时 data 可能已经被销毁！
    for (auto v : data) { /* 未定义行为 */ }
}

// 调用处
void Caller() {
    std::vector<int> temp = {1, 2, 3};
    auto task = ProcessData(temp);
    // temp 在这里销毁，但协程还持有它的引用！
}
```

**修复方案：协程参数用值传递或确保引用对象的生命周期覆盖协程全程**

```cpp
// 正确：值传递
Task<void> ProcessData(std::vector<int> data) {
    co_await SomeAsyncWork();
    for (auto v : data) { /* 安全：data 在协程帧中 */ }
}

// 正确：用 shared_ptr 延长生命周期
Task<void> ProcessData(std::shared_ptr<std::vector<int>> data) {
    co_await SomeAsyncWork();
    for (auto v : *data) { /* 安全 */ }
}
```

### 10.2 线程安全——恢复线程不确定

```cpp
Task<void> UnsafeExample() {
    // 此时在 Thread-1
    printf("Before: thread %d\n", GetCurrentThreadId());

    co_await SomeAsyncIO();  // IO 完成回调可能在 IO 线程上

    // 此时可能在 Thread-2！
    printf("After: thread %d\n", GetCurrentThreadId());

    // 如果这里访问了 Thread-1 专属的数据结构——数据竞争！
    threadLocalData.modify();  // 危险！
}
```

**修复方案：在 Awaiter 中确保恢复到正确的线程**

```cpp
struct ResumeOnEventLoop {
    EventLoop* targetLoop;

    void await_suspend(std::coroutine_handle<> h) {
        asyncIO->Start([h, loop = targetLoop]() {
            // IO 线程上完成，投递回目标 EventLoop
            loop->Post([h]() { h.resume(); });
        });
    }
};
```

### 10.3 协程生命周期管理

```cpp
// 错误：fire-and-forget 但没人持有 Task → 协程帧泄漏
void StartWork() {
    SomeCoroutine();  // 返回的 Task 被丢弃！
    // 如果 Task 析构时 destroy 了 handle，协程还没执行完就被销毁了
    // 如果 Task 析构时不 destroy，协程帧泄漏
}
```

**正确做法：**

```cpp
// 方案 A：调用者持有 Task 直到完成
Task<void> Caller() {
    auto task = SomeCoroutine();
    co_await task;  // 等待完成
}

// 方案 B：使用专门的 fire-and-forget 类型
struct FireAndForget {
    struct promise_type {
        FireAndForget get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
        // initial 和 final 都不挂起 → 协程自行运行和清理
    };
};

FireAndForget StartBackgroundWork() {
    co_await SomeAsyncWork();
    // 完成后协程帧自动销毁
}
```

### 10.4 异常安全

```cpp
Task<void> LeakyCoroutine() {
    auto* raw = new BigObject();
    co_await SomeAsyncWork();   // 如果这里抛异常...
    delete raw;                  // 这一行永远不会执行 → 内存泄漏
}

// 修复：使用 RAII
Task<void> SafeCoroutine() {
    auto obj = std::make_unique<BigObject>();
    co_await SomeAsyncWork();   // 异常时 obj 自动析构
    obj->DoSomething();
}
```

---

## 十一、协程与传统方案对比总结

| 维度           | 回调             | 状态机             | 有栈协程（Boost）     | C++20 无栈协程        |
| -------------- | ---------------- | ------------------ | --------------------- | --------------------- |
| **代码可读性** | 差（嵌套地狱）   | 中（分散但可控）   | 好（同步风格）        | 好（同步风格）        |
| **内存开销**   | 低               | 中（状态成员变量） | 高（每协程几百KB栈）  | 低（帧按需分配）      |
| **上下文切换** | 无               | 无                 | 重（保存/恢复寄存器） | 轻（普通函数调用）    |
| **可扩展性**   | 高（无阻塞）     | 高                 | 中（栈内存受限）      | 高                    |
| **调试体验**   | 差               | 中                 | 中                    | 中（工具链在改善）    |
| **标准库支持** | 无               | 无                 | 第三方                | C++20 标准            |
| **编译器优化** | 有限             | 一般               | 无法优化              | 可 HALO 消除堆分配    |
| **学习成本**   | 低               | 中                 | 中                    | 高（Promise/Awaiter） |
| **最佳场景**   | 简单的一次性异步 | 明确的状态转换     | 大量阻塞式遗留代码    | 复杂异步流程编排      |

---

## 十二、思考题

1. **生命周期陷阱**：以下代码有什么问题？如何修复？

```cpp
Task<std::string_view> GetName(const Player& player) {
    co_await LoadPlayerData(player.id);
    co_return player.GetName();  // 返回 string_view
}

Task<void> PrintName() {
    auto name = co_await GetName(Player{123});
    printf("Name: %s\n", name.data());  // 安全吗？
}
```

2. **调度选型**：游戏服务器中有两种常见的协程调度模型：
   - (a) 单线程 EventLoop + 协程（所有协程在同一线程恢复）
   - (b) 线程池 + 协程（协程可能在任意线程恢复）

   各自的优缺点是什么？游戏逻辑线程应该选哪种？

3. **性能权衡**：对比以下两种实现游戏 AI 巡逻逻辑的方式，在 10000 个 NPC 同时巡逻的场景下，内存和 CPU 开销各有什么差异？

   - (a) 每个 NPC 一个协程（`PatrolBehavior` 协程）
   - (b) 统一的 tick 函数 + 状态枚举

---

## 十三、思考题参考答案

### 题 1：GetName 返回 string_view 的生命周期问题

**答：有两个问题——引用参数悬垂和 `string_view` 悬垂。**

**问题一：`const Player& player` 引用悬垂**

```cpp
Task<void> PrintName() {
    auto name = co_await GetName(Player{123});
    //                          ^^^^^^^^^^^
    // Player{123} 是临时对象，在 co_await 表达式完成后就销毁了
    // 但 GetName 的协程帧中持有的是引用——指向已销毁的临时对象
}
```

`Player{123}` 是一个临时对象。协程函数的参数是引用类型时，编译器**只在协程帧中存储引用（指针）**，不会拷贝对象本身。当协程在 `co_await LoadPlayerData(...)` 处挂起时，临时对象的生命周期已经结束，之后恢复时 `player` 引用指向的是已析构的内存。

**问题二：`string_view` 悬垂**

即使修复了参数问题（改为值传递 Player），`co_return player.GetName()` 返回的 `string_view` 指向 `Player` 内部的字符串。当 `GetName` 协程结束后，其帧被销毁（包括帧中的 Player 拷贝），`string_view` 就悬垂了。

```cpp
auto name = co_await GetName(Player{123});
// 此时 GetName 的协程帧已经销毁
// name (string_view) 指向的字符串已经不存在了
printf("Name: %s\n", name.data());  // 未定义行为！
```

**修复方案：**

```cpp
// 修复1：参数值传递 + 返回 string 而非 string_view
Task<std::string> GetName(Player player) {  // 值传递，拷贝到协程帧
    co_await LoadPlayerData(player.id);
    co_return std::string(player.GetName());  // 返回拥有所有权的 string
}

// 修复2：如果不想拷贝 Player，使用 shared_ptr
Task<std::string> GetName(std::shared_ptr<Player> player) {
    co_await LoadPlayerData(player->id);
    co_return std::string(player->GetName());
}
```

**核心原则：**
- 协程参数尽量**值传递**，避免引用或指针指向可能在挂起期间销毁的对象
- 协程的 `co_return` 不要返回指向协程帧内局部数据的引用/view，因为帧会被销毁
- 如果必须用引用，确保引用对象的生命周期严格长于协程的整个执行周期

---

### 题 2：协程调度模型选型

**答：游戏逻辑线程选 (a) 单线程 EventLoop + 协程。**

**方案 (a) 单线程 EventLoop + 协程：**

```
EventLoop (单线程)
┌─────────────────────────────────────┐
│  事件队列: [协程恢复, IO完成, 定时器] │
│                                     │
│  while (running) {                  │
│      event = queue.pop();           │
│      event.execute();  // 可能 resume 某个协程  │
│  }                                  │
│                                     │
│  所有协程都在同一线程恢复             │
│  → 无需加锁                          │
│  → 可以安全访问所有游戏数据           │
└─────────────────────────────────────┘
```

| 优点                                 | 缺点                              |
| ------------------------------------ | --------------------------------- |
| 无锁，无数据竞争                     | 单线程无法利用多核                |
| 游戏对象可以自由交互                 | 长时间计算会阻塞整个 Loop         |
| 调试简单，执行顺序确定               | 吞吐量受限于单核性能              |
| 恢复线程确定，不用担心上下文切换问题 | 需要将 CPU 密集任务卸载到工作线程 |

**方案 (b) 线程池 + 协程：**

```
线程池
┌──────────┐ ┌──────────┐ ┌──────────┐
│ Thread-1 │ │ Thread-2 │ │ Thread-3 │
│ 协程 A   │ │ 协程 B   │ │ 协程 C   │
│ 协程 D   │ │          │ │ 协程 E   │
└──────────┘ └──────────┘ └──────────┘
                ↕ 协程可能在不同线程恢复
```

| 优点               | 缺点                             |
| ------------------ | -------------------------------- |
| 充分利用多核       | 共享数据必须加锁                 |
| 吞吐量高           | 协程恢复线程不确定，容易引发竞态 |
| 适合 IO 密集型服务 | 游戏对象交互需要额外同步机制     |
|                    | 调试困难，执行顺序不确定         |

**为什么游戏逻辑线程选 (a)？**

游戏逻辑（战斗计算、技能释放、背包操作等）本质上是**大量对象间的密集交互**——一个技能释放可能涉及施法者、目标、周围所有玩家、buff 系统、伤害系统等多个子系统。如果这些在多线程中执行，加锁的复杂度和性能开销会非常大。

**推荐架构：**

```
游戏逻辑：单线程 EventLoop + 协程（方案 a）
IO / DB：线程池（方案 b）或独立 IO 线程

GameLoop (Thread-1)          IO 线程池
┌────────────────────┐      ┌──────────┐
│ 协程: HandleLogin  │─────→│ DB 查询  │
│   co_await DB查询   │      │          │
│   (挂起)           │      │ 完成后:  │
│                    │←─────│ Post回   │
│   (恢复,继续逻辑)   │      │ GameLoop │
└────────────────────┘      └──────────┘
```

DB 查询在 IO 线程池执行，完成后将恢复操作投递回游戏逻辑线程的 EventLoop，确保协程始终在同一线程恢复。

---

### 题 3：协程 vs 状态枚举在 10000 NPC 巡逻场景下的对比

**答：各有优劣，具体数据如下。**

**方案 (a)：每个 NPC 一个协程**

```cpp
Task<void> PatrolBehavior(NPC* npc) {
    while (npc->IsAlive()) {
        for (const auto& wp : npc->GetPatrolPath()) {
            co_await MoveTo(npc, wp);
            co_await SleepFor{2s};
            if (auto* e = npc->DetectEnemy(500.0f)) {
                co_await ChaseBehavior(npc, e);
                break;
            }
        }
    }
}
```

内存开销分析：
- 每个协程帧：约 100~300 字节（局部变量 + promise + 编译器簿记）
- 10000 个 NPC：约 1~3 MB
- 额外开销：每个协程帧一次堆分配（可通过自定义 allocator 缓解）

CPU 开销分析：
- 每帧只有"需要动作"的 NPC 被 resume，等待中的协程开销为零
- resume 一个协程 ≈ 一次间接函数调用 + 恢复局部变量
- 如果大部分 NPC 在 `SleepFor` 或 `MoveTo` 中等待，CPU 开销极低

**方案 (b)：统一 tick + 状态枚举**

```cpp
struct NPCPatrolState {
    enum State { MOVING, WAITING, CHASING } state;
    int waypointIndex;
    float waitTimer;
    // 更多中间状态...
};

void TickAllNPCs(std::vector<NPC>& npcs, float dt) {
    for (auto& npc : npcs) {
        switch (npc.patrolState.state) {
            case MOVING:
                MoveStep(npc, dt);
                if (Arrived(npc)) npc.patrolState.state = WAITING;
                break;
            case WAITING:
                npc.patrolState.waitTimer -= dt;
                if (npc.patrolState.waitTimer <= 0) {
                    npc.patrolState.waypointIndex++;
                    npc.patrolState.state = MOVING;
                }
                break;
            // ...
        }
    }
}
```

内存开销分析：
- 每个 NPC 的 PatrolState：约 16~32 字节（枚举 + 几个字段）
- 10000 个 NPC：约 160~320 KB
- 无堆分配开销

CPU 开销分析：
- 每帧遍历所有 10000 个 NPC，即使大部分什么都不做
- 数据布局紧凑，缓存友好（连续内存遍历）
- switch/case 分支预测友好

**对比总结：**

| 维度                    | 协程方案                   | 状态枚举方案         |
| ----------------------- | -------------------------- | -------------------- |
| **内存 (10000 NPC)**    | 1~3 MB                     | 160~320 KB           |
| **CPU（大部分空闲时）** | 几乎零（只 resume 活跃的） | 遍历全部，但每次极快 |
| **CPU（全部活跃时）**   | 类似                       | 可能更快（缓存友好） |
| **代码复杂度**          | 低（线性逻辑）             | 高（状态机维护复杂） |
| **行为扩展性**          | 高（加 co_await 即可）     | 低（状态爆炸）       |
| **堆分配**              | 每个协程一次               | 无                   |

**实际建议：**

- 如果 AI 行为简单（巡逻、追击、返回），**状态枚举更轻量**
- 如果 AI 行为复杂（多阶段 Boss 战、带条件分支的行为树），**协程大幅降低代码复杂度**
- 折中方案：简单 NPC 用状态枚举，复杂 Boss/剧情 NPC 用协程
- 可以对协程帧使用自定义内存池来减少堆分配开销

---

## 参考资料

- [cppreference: Coroutines (C++20)](https://en.cppreference.com/w/cpp/language/coroutines)
- [cppreference: coroutine_handle](https://en.cppreference.com/w/cpp/coroutine/coroutine_handle)
- [CppCon 2022: Andreas Fertig — C++20 Coroutines — Complete Guide](https://www.youtube.com/watch?v=j9FUMpUWOZc)
- [Lewis Baker — Asymmetric Transfer (系列博客，协程深度解析)](https://lewissbaker.github.io/)
- [cppcoro: 实际可用的协程库（Lewis Baker）](https://github.com/lewissbaker/cppcoro)
- [C++20 — The Complete Guide (Nicolai Josuttis), Part III: Coroutines](https://leanpub.com/cpp20)
