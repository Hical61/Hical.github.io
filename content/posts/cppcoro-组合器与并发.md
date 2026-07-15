+++
title = '协程世界的并发编排：从需求反推 when_all 的实现'
date = '2024-11-18'
draft = false
tags = ["C++", "cppcoro", "协程", "C++20", "源码分析", "并发", "when_all"]
categories = ["C++"]
description = "拆开 cppcoro 给你看 · 第 3 篇。从需求出发反推 when_all 实现：顺序等待太慢 → 手动管理太繁琐 → 注册竞态太难搞，逐个填坑。"
+++

# 协程世界的并发编排：从需求反推 when_all 的实现

> 本专栏文章：拆开 cppcoro 给你看 · 第 3 篇

这篇文章我想换个写法。前两篇是"先给答案再解释"，这篇反过来——**从需求出发，一步步推到最终实现**。

需求很简单：三个查询并发执行，全部完成后取结果。但从这个需求到最终的 `when_all`，踩了三个坑：顺序等待太慢 → 手动管理太繁琐 → 注册竞态太难搞。我们逐个填。

---

## 1. 场景引入：三个并发查询

```cpp
task<User>    load_user(int id);
task<Order>   load_orders(int userId);
task<Address> load_address(int userId);
```

### 尝试 1：顺序等待（慢）

```cpp
task<void> handle(int userId) {
    auto user    = co_await load_user(userId);     // ~200ms
    auto orders  = co_await load_orders(user.id);  // ~300ms
    auto address = co_await load_address(user.id); // ~100ms
    // 总耗时: ~600ms——三个没有依赖的操作却串行执行了
}
```

### 尝试 2：手动管理 counter（繁琐）

每次需要并发时都要手动写计数器逻辑——大量重复代码，容易出错。

我们需要一个**泛化的并发执行工具**。这就是 `when_all` 的由来。

---

## 2. 核心问题：怎么知道"所有任务都完成了"？

答案就是上一篇文章提到的 `when_all_counter`：

```cpp
class when_all_counter {
    std::atomic<std::size_t> m_count;
    std::coroutine_handle<> m_awaitingCoroutine;
};
```

让我们从头推导它的设计。

### Step 1：计数器初始化

```cpp
when_all_counter counter(3);  // 3 个任务
```

### Step 2：每个任务完成时递减

```cpp
void notify_awaitable_completed() noexcept {
    if (m_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // 我是最后一个完成的 → 唤醒等待者
        m_awaitingCoroutine.resume();
    }
}
```

### Step 3：等待者注册自己

```cpp
bool try_await(std::coroutine_handle<> awaitingCoroutine) noexcept {
    m_awaitingCoroutine = awaitingCoroutine;
    return m_count.fetch_sub(1, std::memory_order_acq_rel) > 1;
    // >1 → 还有任务没完成 → 挂起
    // ==1 → 所有任务在注册前就完成了 → 不挂起
}
```

### 为什么 m_count 初始 = 任务数 + 1？

> 💡 这是本文第一个关键洞察——额外的那 1 票代表"注册尚未完成"。

```cpp
when_all_counter(std::size_t count) noexcept
    : m_count(count + 1)  // ← 多了一个！
{}
```

**没有额外那 1 票会怎样？** 如果所有子任务在 `try_await` 被调用之前就全部完成了：
- count 初始 = 3，3 个子任务完成 → count 减到 0
- 但没人 resume（m_awaitingCoroutine 还是 null）
- 然后 `try_await` 被调用，count 已经是 0 → 永远等不到最后一个 notify

**有额外那 1 票**：
- count 初始 = 4，3 个子任务完成 → count 减到 1（>0，不触发 resume）
- `try_await`：count 从 1 减到 0 → `fetch_sub(1) > 1` 为 false → 不挂起！

完美解决注册阶段和子任务完成之间的竞态。

---

## 3. `when_all_task` —— 包装每个 awaitable

直接 `co_await` 一个 `task<T>` 会取值并消耗 task。但在 `when_all` 中，我们需要启动 task、等它完成但不取值、最后分别取结果。`when_all_task` 实现这个分离。

### 大图：整个调用链

```
when_all_ready(taskA, taskB, taskC)
  │
  ├─ make_when_all_task(taskA) → when_all_task<ResultA>
  ├─ make_when_all_task(taskB) → when_all_task<ResultB>
  └─ make_when_all_task(taskC) → when_all_task<ResultC>
       │
       ▼
  when_all_ready_awaitable<tuple<when_all_task<A>, when_all_task<B>, when_all_task<C>>>
       │
       ├─ m_counter(count=3+1=4)
       └─ m_tasks (tuple 存着三个 when_all_task)
```

### `when_all_task_promise` 的关键设计

```cpp
template<typename RESULT>
class when_all_task_promise {
    when_all_counter* m_counter;        // 指向共享计数器
    std::exception_ptr m_exception;     // 存异常（不立即传播）
    std::add_pointer_t<RESULT> m_result; // 存结果的指针
};
```

非 void 结果的 `make_when_all_task` 用 `co_yield co_await` 而不是 `co_return co_await`：

```cpp
when_all_task<RESULT> make_when_all_task(AWAITABLE awaitable) {
    co_yield co_await static_cast<AWAITABLE&&>(awaitable);
}
```

为什么是 `co_yield`？`co_yield` 把结果交给 `yield_value(T)`，然后 `yield_value` 返回 `final_suspend()`——更直接的控制流，减少了一次代码生成路径。

```cpp
auto yield_value(RESULT&& result) noexcept {
    m_result = std::addressof(result);  // 存结果地址
    return final_suspend();             // 直接进入 final_suspend
}

auto final_suspend() noexcept {
    class completion_notifier {
        bool await_ready() const noexcept { return false; }  // 总是挂起
        void await_suspend(coroutine_handle_t coro) const noexcept {
            coro.promise().m_counter->notify_awaitable_completed();
            // ↑ 递减计数器——可能是最后一个完成的
        }
        void await_resume() const noexcept {}
    };
    return completion_notifier{};
}
```

---

## 4. `when_all_ready` —— 并发编排层

### 完整执行流程

```cpp
auto [t1, t2, t3] = co_await when_all_ready(taskA(), taskB(), taskC());
```

展开后的时序：

```
时间 →
等待者协程                    when_all_ready_awaitable     子任务们
──────────                   ──────────────────────        ───────
co_await when_all_ready_awaitable:
  await_ready() → false
  await_suspend:
    try_await():
      start_tasks():
        t1.start(counter) ──────────→ t1协程启动
        t2.start(counter) ──────────→ t2协程启动
        t3.start(counter) ──────────→ t3协程启动
      counter.try_await(等待者句柄):
        count = 3+1 = 4
        fetch_sub(1) = 4 → >1 → 挂起
      等待者挂起 ←───┐
                     │
                     │              t1 完成 → notify → count=2
                     │              t3 完成 → notify → count=1 → resume!
  等待者被唤醒 ←──────┘
  await_resume() → 返回 tuple{when_all_task1, when_all_task2, when_all_task3}

// 然后分别调用 t1.result(), t2.result(), t3.result()
```

### `start_tasks` 的实现

```cpp
template<std::size_t... INDICES>
void start_tasks(std::integer_sequence<std::size_t, INDICES...>) noexcept {
    (void)std::initializer_list<int>{
        (std::get<INDICES>(m_tasks).start(m_counter), 0)...
    };
}
```

用 `initializer_list` 保证从左到右的求值顺序。逗号表达式 `(expr, 0)` 丢弃返回值。

---

## 5. `when_all` —— 结果提取层

`when_all` 本质上就是**对 `when_all_ready` 的结果应用 `fmap`**：

```cpp
template<typename... AWAITABLES>
auto when_all(AWAITABLES&&... awaitables) {
    return fmap(
        [](auto&& taskTuple) {
            return std::apply([](auto&&... tasks) {
                return std::make_tuple(
                    static_cast<decltype(tasks)>(tasks).non_void_result()...);
            }, static_cast<decltype(taskTuple)>(taskTuple));
        },
        when_all_ready(std::forward<AWAITABLES>(awaitables)...)
    );
}
```

> 💡 `when_all = fmap(提取结果, when_all_ready(...))`——这就是核心等式。

### `when_all_ready` vs `when_all` 的选择

```cpp
// 场景 1：需要分别处理成功和失败 → when_all_ready
auto [t1, t2, t3] = co_await when_all_ready(taskA(), taskB(), taskC());
try {
    auto& r1 = t1.result();  // 可能抛异常
} catch (const std::exception& e) {
    log("taskA failed: {}", e.what());  // 知道具体是哪个失败了
}
auto& r2 = t2.result();  // taskA 的异常不影响访问 taskB 的结果

// 场景 2：任何失败都应该终止 → when_all（更简洁）
auto [r1, r2, r3] = co_await when_all(taskA(), taskB(), taskC());
// 任何一个子任务异常 → 这里就抛异常
```

---

## 6. `fmap` —— 协程世界的 `std::transform`

```cpp
// fmap 做什么：
task<B> result = fmap(a_to_b, get_an_a());
// 等价于：
auto a = co_await get_an_a();
B b = a_to_b(a);
co_return b;
```

### 零开销的秘诀：fmap_awaiter 透传

```cpp
template<typename FUNC, typename AWAITABLE>
class fmap_awaiter {
    FUNC&& m_func;
    awaiter_t m_awaiter;  // 原始 awaitable 的 awaiter

    // await_ready → 原封不动透传
    decltype(auto) await_ready() {
        return m_awaiter.await_ready();
    }

    // await_suspend → 原封不动透传（包括对称转移！）
    template<typename PROMISE>
    decltype(auto) await_suspend(coroutine_handle<PROMISE> coro) {
        return m_awaiter.await_suspend(std::move(coro));
    }

    // await_resume → 唯一不同的地方：对结果应用 func
    decltype(auto) await_resume() {
        return std::invoke(
            std::forward<FUNC>(m_func),
            std::forward<awaiter_t>(m_awaiter).await_resume());
    }
};
```

`await_ready` 和 `await_suspend` 完全透传。如果原始 awaiter 的 `await_suspend` 返回 `coroutine_handle<>`（对称转移），`fmap_awaiter` 也会返回 `coroutine_handle<>`——**对称转移原封不动地保留**。`fmap` 不引入额外的挂起点。

### 管道语法

```cpp
// 函数式
task<B> b = fmap(a_to_b, get_an_a());

// 管道式
task<B> b = get_an_a() | fmap(a_to_b);

// 链式
auto result = get_user(id)
    | fmap(extract_name)
    | fmap(to_uppercase);
```

管道语法通过 `operator|` 重载 + ADL 实现，非常轻量。

---

## 7. `shared_task<T>` —— 多消费者共享结果

### 问题场景

```cpp
shared_task<Config> load_config();  // 只加载一次

task<> consumerA() {
    auto cfg = co_await sharedConfig;  // const Config&
}
task<> consumerB() {
    auto cfg = co_await sharedConfig;  // 同一个结果，不重新加载
}
```

如果用 `task<T>`：move-only，第一个人 await 后 task 空了，第二个人 await 得到 `broken_promise`。

### 与 `task<T>` 的关键差异

| 特性                | `task<T>`     | `shared_task<T>`        |
| ------------------- | ------------- | ----------------------- |
| 拷贝                | move-only     | 可拷贝（引用计数）      |
| `co_await` 返回类型 | `T&` 或 `T&&` | 始终 `const T&`         |
| 并发等待            | 不支持        | 支持                    |
| 额外开销            | 几乎为零      | 多一个原子 `m_refCount` |

### 两阶段启动：避免递归 resume 栈溢出

cppcoro 的 `shared_task` 启动分两步，避免了微妙的死锁：

```cpp
bool try_await(shared_task_waiter* waiter, coroutine_handle<> coroutine) {
    // 第1步：如果是第一次 await → 先启动协程
    void* oldWaiters = m_waiters.load(acquire);
    if (oldWaiters == notStartedValue &&
        m_waiters.compare_exchange_strong(oldWaiters, startedNoWaitersValue, relaxed))
    {
        coroutine.resume();  // 启动！
        oldWaiters = m_waiters.load(acquire);
    }

    // 第2步：再注册等待者
    do {
        if (oldWaiters == valueReadyValue) {
            return false;  // 协程已经完成了 → 不挂起
        }
        waiter->m_next = static_cast<shared_task_waiter*>(oldWaiters);
    } while (!m_waiters.compare_exchange_weak(
        oldWaiters, static_cast<void*>(waiter), release, acquire));

    return true;
}
```

**如果第1步和第2步反过来会怎样？** 先注册再启动 → 同步完成的协程在 `resume()` 内就完成了 → `final_suspend` 检查到等待者 → `resume()` 内部递归地 resume 等待者 → 潜在的栈溢出。

> 💡 两阶段启动——先 resume 再注册——避免了这种递归 resume。如果同步完成了，`oldWaiters == valueReadyValue` → 不挂起，等本函数返回后自然继续执行。

### 引用计数

```cpp
shared_task_promise_base() noexcept
    : m_refCount(2)  // 初始就是 2！
{}
// 1 票给协程自身（执行完成时释放）
// 1 票给第一个 shared_task 对象
```

---

## 回顾一下你学会了什么

1. **`when_all_counter` 初始值 +1**：额外一票代表"注册尚未完成"，解决竞态
2. **`when_all_task` 用 `co_yield co_await`** 而非 `co_return co_await`
3. **`when_all_ready`** = 并发启动 + 统一计数器，不自动提取结果
4. **`when_all` = `fmap(提取结果, when_all_ready(...))`**
5. **`fmap_awaiter` 完全透传** `await_ready`/`await_suspend`，包括对称转移——零开销
6. **`void_value`** 是 void 的占位类型，让 `tuple<...>` 能包含 void
7. **管道语法 `x | fmap(f)`** 通过 ADL + `operator|` 实现
8. **`shared_task` 四态**：not_started / started_no_waiters / 等待者链表 / value_ready
9. **两阶段启动**：先 resume 再注册，防止递归 resume 栈溢出
10. **refCount=2**：1 票给协程自身，1 票给首个 shared_task 对象

---

## 动手练习

### 练习 1：手写 when_all_counter

```cpp
#include <coroutine>
#include <atomic>
#include <iostream>

struct WhenAllCounter {
    std::atomic<size_t> count;
    std::coroutine_handle<> waiter;

    WhenAllCounter(size_t n) : count(n + 1) {}

    bool try_await(std::coroutine_handle<> h) {
        waiter = h;
        return count.fetch_sub(1) > 1;
    }

    void notify_one_done() {
        if (count.fetch_sub(1) == 1) waiter.resume();
    }
};

int main() {
    WhenAllCounter c(3);
    c.notify_one_done(); std::cout << c.count.load() << " ";
    c.notify_one_done(); std::cout << c.count.load() << " ";
    c.notify_one_done(); std::cout << c.count.load() << " ";
    // 输出: 3 2 1
}
```

### 练习 2：手写 fmap_awaiter

实现 fmap：让 `co_await fmap(f, task)` 等价于 `f(co_await task)`。提示：把原始 awaiter 的 `await_ready`/`await_suspend` 透传，只在 `await_resume` 中多调用一次 `f`。

---

## 常见坑

### 坑 1：when_all 内的异常静默丢失

```cpp
// 如果 taskB 抛异常，taskA 和 taskC 的结果丢失
auto [a, b, c] = co_await when_all(taskA(), taskB(), taskC());
```

需要分别处理时用 `when_all_ready`。

### 坑 2：when_all_ready 后忘记检查异常

```cpp
auto [t1, t2, t3] = co_await when_all_ready(taskA(), taskB(), taskC());
auto& r1 = t1.result();  // ← 可能抛异常！如果不 catch，t2 t3 白取了
```

---

> 下一篇：[从 generator 到 async_generator：协程生成器的三层进化](04-生成器.md)
