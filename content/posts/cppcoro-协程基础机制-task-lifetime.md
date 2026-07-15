+++
title = 'cppcoro 的 task\<T\> 到底做了什么？从 25 行到 90 行，5 版迭代拆开看'
date = '2024-11-10'
draft = false
tags = ["C++", "cppcoro", "协程", "C++20", "源码分析", "task"]
categories = ["C++"]
description = "拆开 cppcoro 给你看 · 第 1 篇。分 5 版迭代拆解 task<T> 核心逻辑，每版只加一个功能点，从 25 行到 90 行完整理解协程生命周期。"
+++

# cppcoro 的 task\<T\> 到底做了什么？从 25 行到 90 行，5 版迭代拆开看

> 本专栏文章：拆开 cppcoro 给你看 · 第 1 篇

上一篇我们手写了 30 行的 Generator，理解了协程帧、co_await 的展开步骤和 promise_type 的 7 个接口。今天来真的——把 cppcoro 的 `task<T>` 拆开看。

cppcoro 的 `task.hpp` 一共 482 行，但你不需要直接从第 1 行读到第 482 行。其中差不多 300 行是关于对称转移/非对称转移的条件编译、MSVC 的 workaround、异常处理细节。真正核心的东西不到 100 行。

**本文的学习策略是分 5 版迭代——每个版本只加一个功能点，每版都是可编译运行的完整代码。**

| 版本 | 功能                                        | 行数 |
| ---- | ------------------------------------------- | ---- |
| v1   | 只能 `co_return`，不能 `co_await`           | ~25  |
| v2   | 加上 `co_await` 支持（用 `sync_wait` 驱动） | ~40  |
| v3   | 加上对称转移（`final_awaitable`）           | ~55  |
| v4   | 加上异常处理                                | ~70  |
| v5   | 完整版——对应 cppcoro 源码                   | ~90  |

---

## v1：只能 co_return 的 task

v1 的目标很简单：理解一个协程怎么把结果从协程帧传回调用者。

```cpp
#include <coroutine>
#include <iostream>
#include <cassert>

template<typename T>
struct Task {
    struct promise_type {
        T result_value;  // 存 co_return 的值

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(T v) { result_value = v; }
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Task() { if (handle) handle.destroy(); }

    T get_result() {
        handle.resume();  // resume 协程让其停在 final_suspend
        return handle.promise().result_value;
    }
};

// 使用
Task<int> compute() {
    co_return 42;
}

int main() {
    auto t = compute();
    std::cout << t.get_result() << std::endl;  // 42
}
```

执行时间线：

```
auto t = compute() 时：
  ① 堆上分配协程帧
  ② 构造 promise_type
  ③ 调用 get_return_object() → Task
  ④ 调用 initial_suspend() → 挂起
  ⑤ Task 返回给 main()
  注意：协程体还没执行！

t.get_result() 时：
  ① handle.resume()
  ② 协程体开始执行
  ③ co_return 42 → promise.return_value(42) → result_value = 42
  ④ final_suspend() → suspend_always → 协程挂起
  ⑤ get_result() 返回 result_value
```

v1 的问题很明显——这个 Task 只能 `co_return`，不能 `co_await` 任何东西。当你写 `co_await other_task()` 时，编译器找不到 awaiter。

---

## v2：支持 co_await 其他 task

v2 要让一个 task 可以 `co_await` 另一个 task，实现协程间的串联。关键是在 Task 上添加 `operator co_await()`。

```cpp
template<typename T>
struct Task {
    // ... v1 的 promise_type 不变 ...

    std::coroutine_handle<promise_type> handle;
    // ... 构造/析构不变 ...

    // ──── 新增：operator co_await() ────
    auto operator co_await() {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;

            bool await_ready() {
                return h.done();  // 已结束 → 直接拿结果
            }
            void await_suspend(std::coroutine_handle<> waiting) {
                h.resume();                  // 启动被等待的 task
                if (h.done()) {
                    waiting.resume();        // 同步完成了 → 唤醒等待者
                }
                // 否则等待者保持挂起...但谁来唤醒它？
            }
            T await_resume() {
                return h.promise().result_value;
            }
        };
        return Awaiter{handle};
    }
};
```

你发现没有——这里有个致命问题：如果被等待的 task 没有同步完成，等待者就永远醒不过来了。因为被等待的 task 没有办法知道"谁在等我"。

这就是 continuation 的由来。v3 要解决的就是这个问题。

---

## v3：continuation 链和对称转移

v3 是本文最重要的一个版本。它引入了两个关键设计：

1. **continuation 链**：被等待的 task 完成后自动恢复等待者
2. **对称转移**：每次切换不是函数调用，是 tail-call jump

```cpp
#include <coroutine>
#include <iostream>
#include <atomic>

template<typename T>
struct Task {
    struct promise_type {
        T result_value;
        std::coroutine_handle<> m_continuation;  // ← 新增：存等待者

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }

        // ← 关键改变：final_suspend 不再是 suspend_always
        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }  // 永远挂起
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> self) noexcept {
                    return self.promise().m_continuation;  // 直接跳转到等待者！
                }
                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }

        void return_value(T v) { result_value = v; }
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    auto operator co_await() {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;

            bool await_ready() { return h.done(); }

            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> waiting) {
                // 1. 告诉被等待的 task："完成后跳到我这里"
                h.promise().m_continuation = waiting;
                // 2. 返回被等待 task 的句柄 → 对称转移到它！
                return h;
            }

            T await_resume() {
                return h.promise().result_value;
            }
        };
        return Awaiter{handle};
    }
};
```

### 对称转移的完整执行流（本文最重要的一张图）

> 💡 如果只能从本文带走一个知识点，请带走这个时序图。

```
假设:
  taskA = computeA();  // co_await taskB; co_return 1;
  taskB = computeB();  // co_return 2;

时间线:
──────────────────────────────────────────────────────────────
[驱动者]           [taskA 协程]                [taskB 协程]
───────           ───────────                ──────────
taskA.resume()
                  A 开始执行
                  co_await taskB:
                    await_suspend:
                      taskB.promise()
                        .m_continuation = A的句柄
                      return taskB.handle ← 对称转移！
                                              B 开始执行
                                              co_return 2
                                              B 结束
                                              final_suspend:
                                                return m_continuation
                                                  (即 A的句柄)
                                              ← 对称转移！
                  A 从 co_await 后继续
                  拿到 taskB 的结果 = 2
                  co_return 1
                  A 结束
                  final_suspend:
                    返回驱动者的 continuation
                                              ← A 已完成
[得到结果 1]
──────────────────────────────────────────────────────────────
```

**关键理解**：每次"跳转"都不是函数调用，而是对称转移——编译器把它编译成一条 jmp 指令，不增加调用栈深度。如果你有 100 个串联的 `co_await`，栈也只有一层。

### 如果没有对称转移呢？

cppcoro 兼容老编译器的方式长这样：

```cpp
// 非对称转移的 await_suspend
bool await_suspend(std::coroutine_handle<> waiting) {
    h.promise().m_continuation = waiting;
    h.resume();                              // 普通函数调用 resume！
    return !h.done();                        // 没完成就挂起
}

// 非对称转移的 final_suspend
void await_suspend(std::coroutine_handle<promise_type> self) noexcept {
    // 用原子变量解决"完成"和"挂起"的竞态
    if (self.promise().m_state.exchange(true, std::memory_order_acq_rel)) {
        self.promise().m_continuation.resume();   // 普通 resume
    }
}
```

这就是 cppcoro 中 `CPPCORO_COMPILER_SUPPORTS_SYMMETRIC_TRANSFER` 条件编译的原因——有对称转移时代码极简（直接 return handle），没有时要用原子变量 + 手动 resume。**一个编译器特性决定了几百行代码的存在与否。**

---

## v4：加上异常处理

v4 让协程抛出的异常能传播到 `co_await` 的调用者。

```cpp
struct promise_type {
    T result_value;
    std::exception_ptr m_exception;  // ← 新增
    std::coroutine_handle<> m_continuation;

    // ...

    void unhandled_exception() {
        m_exception = std::current_exception();  // 捕获异常，不抛
    }

    // await_resume 中检查异常（在 Awaiter 中）
    T await_resume() {
        if (h.promise().m_exception) {
            std::rethrow_exception(h.promise().m_exception);
        }
        return h.promise().result_value;
    }
};
```

这样 taskB 抛异常 → taskA 的 `co_await taskB` 抛异常 → 继续传播到调用者。异常在协程链中的传播路径是畅通的。

---

## v5：cppcoro 源码精读

现在你已经自己实现了 4 版，可以直接对照 cppcoro 源码理解它的额外设计。

拿出 `task.hpp`（482 行），结构是这样的：

```
行 1-22    ── include 和注释
行 23-272  ── detail 命名空间
  29-114    ── task_promise_base      (你 v3 的 promise，不含 T)
  116-235   ── task_promise<T>        (你 v4 的 promise，含 T)
  206-235   ── task_promise<void> 特化
  237-271   ── task_promise<T&> 特化
行 274-451 ── task<T> 类
  293-336   ── awaitable_base         (你 v3 的 Awaiter)
  392-430   ── operator co_await()    (const & 和 const && 两个版本)
  432-445   ── when_ready()           (只同步不取值)
行 453-479 ── 行外定义
```

下面逐段看 cppcoro 比你多做了什么。

### 5.1 task_promise_base：为什么拆出基类

```cpp
class task_promise_base {
    struct final_awaitable { ... };
    coroutine_handle<> m_continuation;
};
```

`m_continuation` 和 `final_awaitable` 的逻辑是所有 T 类型共享的。`task_promise<T>`、`task_promise<void>`、`task_promise<T&>` 只需要关心"怎么存结果"，不需要关心"怎么把控制权转给等待者"。

### 5.2 为什么用 union 存结果和异常

```cpp
union {
    T m_value;
    std::exception_ptr m_exception;  // 不同时存活 → union 省内存
};
enum class result_type { empty, value, exception };
```

协程帧在堆上，每一个字节都是钱。T 和 exception_ptr 不会同时有效（要么返回值，要么抛异常）。union 比分别存两个字段省约 50% 空间。而且用的是 placement new 而不是赋值——因为 union 成员初始状态是未构造的，直接赋值是 UB：

```cpp
::new (static_cast<void*>(std::addressof(m_value))) T(std::forward<VALUE>(value));
```

### 5.3 task_promise\<void\> 和 task_promise\<T&\> 特化

```cpp
// task_promise<void>:
// - 不需要 union（只有 exception_ptr）
// - return_void() 而不是 return_value()

// task_promise<T&>:
// - 存 T* 指针（引用不能放 union 里）
// - return_value(T& value) → m_value = &value
```

模板特化自动处理了这些差异，用户写 `task<void>` 或 `task<int&>` 时不用做任何额外工作。

### 5.4 `operator co_await() const &` vs `operator co_await() const &&`

这是 cppcoro 中一个精妙设计：

```cpp
// 左值版本 → 返回 T&
auto operator co_await() const & noexcept {
    // ... await_resume 返回 T& ——左值引用
}

// 右值版本 → 返回 T&& （支持移动语义）
auto operator co_await() const && noexcept {
    // ... await_resume 返回 T&& ——可移动
}
```

使用场景：

```cpp
task<string> get_string();

// 右值版本：task 是临时的 → 结果可以移动走
string s = co_await get_string();  // 调用 const && 版本，移动语义

// 左值版本：task 是持久的 → 结果不能移动（后面可能还有人 await）
task<string> t = get_string();
string& ref = co_await t;          // 调用 const & 版本，返回引用
```

### 5.5 MSVC workaround：基本类型的右值引用问题

```cpp
// HACK: MSVC 对基本类型（int, double, 指针）返回 T&& 时
// 会产生一次不必要的拷贝。
using rvalue_type = std::conditional_t<
    std::is_arithmetic_v<T> || std::is_pointer_v<T>,
    T,
    T&&>;
```

这是实际项目才有的细节——标准行为有 bug，编译器实现不一致，框架作者不得不打补丁。这种"血泪补丁"在读源码时很容易被忽略，但它恰恰是一个工业级库的标志。

### 5.6 `when_ready()`：只同步不取值

```cpp
auto when_ready() const noexcept {
    struct awaitable : awaitable_base {
        void await_resume() const noexcept {}  // 空的！不取值！不抛异常！
    };
    return awaitable{m_coroutine};
}
```

|                        | `co_await task` | `co_await task.when_ready()` |
| ---------------------- | --------------- | ---------------------------- |
| 等待任务完成           | ✅               | ✅                            |
| 获取返回值             | ✅               | ❌                            |
| 抛异常（如果任务失败） | ✅               | ❌                            |

`when_ready()` 的典型场景是 `shared_task` 的多消费者——多个协程在等同一个任务完成，但不需要每个都取值。

### 5.7 `make_task`：类型擦除

```cpp
template<typename AWAITABLE>
auto make_task(AWAITABLE awaitable)
    -> task<detail::remove_rvalue_reference_t<
        typename awaitable_traits<AWAITABLE>::await_result_t>>
{
    co_return co_await static_cast<AWAITABLE&&>(awaitable);
}
```

把任何可 await 的东西包装成统一的 `task<T>`。一个函数、两行代码，完成了类型擦除。

---

## 回顾一下你学会了什么

1. 协程帧在堆上，promise 在帧内偏移固定，coroutine_handle 是指向帧的 8 字节指针
2. `initial_suspend = suspend_always` → 惰性启动，调用函数不等于开始执行
3. `final_suspend = FinalAwaiter` → `await_ready()` 永远 false，总是挂起，把控制权交还给等待者
4. 对称转移：`await_suspend` 返回 `coroutine_handle<>`，编译成 tail-call jump
5. Continuation 链：每个 task 完成时自动恢复它的等待者（通过 final_suspend）
6. `task_promise` 用 union 存 T 和 exception_ptr——不同时存活，省 50% 空间
7. placement new 初始化 union 成员（不能直接赋值，因为初始状态未构造）
8. `operator co_await() const &` vs `const &&`：左值返回 T&，右值返回 T&&
9. `when_ready()`：只同步不取值，不传播异常
10. `make_task`：两行代码的类型擦除

---

## 动手练习

### 练习 1：手写最简 task（对应 v3）

把下面的代码复制到本地运行，输出应该是 "inner returned: 42" 和 "outer result: 84"：

```cpp
#include <coroutine>
#include <iostream>

template<typename T>
struct Task {
    struct promise_type {
        T result_value;
        std::coroutine_handle<> m_continuation;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> self) noexcept {
                    return self.promise().m_continuation;
                }
                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }

        void return_value(T v) { result_value = v; }
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;
    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& other) : handle(other.handle) { other.handle = nullptr; }
    ~Task() { if (handle) handle.destroy(); }

    auto operator co_await() {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;
            bool await_ready() { return h.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> waiting) {
                h.promise().m_continuation = waiting;
                return h;
            }
            T await_resume() { return h.promise().result_value; }
        };
        return Awaiter{handle};
    }

    T get() {
        auto& p = handle.promise();
        p.m_continuation = std::noop_coroutine();
        handle.resume();
        return p.result_value;
    }
};

Task<int> inner() {
    co_return 42;
}

Task<int> outer() {
    int val = co_await inner();
    std::cout << "inner returned: " << val << std::endl;
    co_return val * 2;
}

int main() {
    auto t = outer();
    std::cout << "outer result: " << t.get() << std::endl;
}
```

### 练习 2：加上异常处理

让 `inner()` 抛出异常，在 `outer()` 中 `co_await inner()` 时捕获。

> 提示：promise 中加 `std::exception_ptr m_exception`，`unhandled_exception()` 中存异常，`await_resume()` 中重新抛出。

---

## 常见坑

### 坑 1：co_await 后使用悬挂引用

```cpp
task<int&> bad(Task<int> t) {
    auto& val = co_await t;  // t 的协程帧在 co_await 后...
    co_return val;            // ...可能已经悬空了！
}
```

确保被 await 的 task 的协程帧生命周期长于 await 的结果引用。

### 坑 2：m_continuation 未初始化

```cpp
std::coroutine_handle<> m_continuation;  // 未初始化！→ UB
// FinalAwaiter 返回一个随机地址 → 崩溃
```

cppcoro 的 task_promise_base 构造函数会初始化 m_continuation，所以实际不会出问题。但自己写的时候要注意。

### 坑 3：final_suspend 不挂起

```cpp
// 错误
std::suspend_never final_suspend() { return {}; }
// → 协程不挂起 → 协程帧立即销毁
// → 等待者的 co_await 还在访问 promise.result_value → UAF！
```

必须让 final_suspend 挂起，给调用者机会在 destroy 之前取走结果。这就是为什么 cppcoro 的 `FinalAwaiter::await_ready()` 永远返回 `false`。

---

> 下一篇：[用一个场景把 cppcoro 的 7 个协程同步原语全部串起来](02-协程同步原语.md)
