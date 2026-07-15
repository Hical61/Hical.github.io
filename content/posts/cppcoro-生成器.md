+++
title = '从 generator 到 async_generator：协程生成器的三层进化'
date = '2024-11-22'
draft = false
tags = ["C++", "cppcoro", "协程", "C++20", "源码分析", "generator", "async_generator"]
categories = ["C++"]
description = "拆开 cppcoro 给你看 · 第 4 篇。从 generator 到 recursive_generator 再到 async_generator 的三层进化，看每种生成器解决什么瓶颈。"
+++

# 从 generator 到 async_generator：协程生成器的三层进化

> 本专栏文章：拆开 cppcoro 给你看 · 第 4 篇

前几篇一直在讲"等一个结果返回"的协程模式——`task<T>`、同步原语、`when_all`。但协程还有另一面：**用协程产生一系列值**，而不是一次返回一个结果。

cppcoro 为此提供了三种生成器，每一种都是为解决前一种的瓶颈而生的。本文从一个具体痛点出发：**遍历一棵二叉树的所有节点**，看着普通的 `generator` 怎么在递归场景下性能退化成 O(N²)，然后 `recursive_generator` 怎么用 O(1) 的 `pull()` 解决，最后 `async_generator` 怎么让生成器支持 `co_await`。

```
generator<T>       → O(1) 遍历平面序列，但递归时 operator++() 退化成 O(depth)
recursive_generator<T> → pull() 直接驱动叶子，递归场景 O(1)
async_generator<T> → 支持 co_await，值可以异步产生
```

---

## 1. `generator<T>` —— 同步惰性序列

### 1.1 最简单的使用场景

```cpp
generator<int> fibonacci() {
    int a = 0, b = 1;
    for (int i = 0; i < 10; ++i) {
        co_yield b;
        int t = a;
        a = b;
        b += t;
    }
}

int main() {
    for (int n : fibonacci()) {
        std::cout << n << " ";  // 1 1 2 3 5 8 13 21 34 55
    }
}
```

### 1.2 和 `task<T>` 的设计对比

| 特性              | `task<T>`                  | `generator<T>`         |
| ----------------- | -------------------------- | ---------------------- |
| 用途              | 产生一个最终结果           | 产生一系列中间值       |
| 关键字            | `co_return`                | `co_yield`             |
| 可以用 co_await？ | ✅                          | ❌                      |
| final_suspend     | FinalAwaiter（转移控制权） | `suspend_always`       |
| 谁决定何时结束    | 协程自己 (co_return)       | 调用者 (不再调用 ++it) |

### 1.3 为什么 final_suspend 是 suspend_always？

与 `task<T>` 的 FinalAwaiter（把控制权转回等待者）不同，generator 的 `final_suspend` 就是纯纯的 `suspend_always`。

原因：generator 不是由另一个协程驱动的（没有 continuation 链），它是由外部 `for` 循环驱动的。协程结束时，调用者通过 `it == end()` 或 `coroutine.done()` 发现序列结束，然后 `generator` 的析构函数调用 `handle.destroy()`。不需要把控制权转给谁——调用者本来就是同步的，自己会检查。

### 1.4 `operator++()` 就是 `handle.resume()`

```cpp
generator_iterator& operator++() {
    m_coroutine.resume();  // 恢复协程，执行到下一个 co_yield
    if (m_coroutine.done()) {
        m_coroutine.promise().rethrow_if_exception();
    }
    return *this;
}
```

每次 `++it` 就是一次 `resume()`。一个 `for` 循环迭代 = 一次 `resume()` + 一次读取。

### 1.5 yield_value 存指针不存值

```cpp
std::suspend_always yield_value(T& value) noexcept {
    m_value = std::addressof(value);  // 存指针！
    return {};
}
```

为什么存指针而不是拷贝？因为值还在协程帧中（在 `co_yield value` 的挂起点之后），消费者通过 `operator*()` 读取时值仍然有效。存指针省了一次拷贝。

### 1.6 不能使用 co_await

```cpp
template<typename U>
std::suspend_never await_transform(U&& value) = delete;
```

generator 协程体内不能有 `co_await`。这不是功能限制，而是**语义约束**——generator 是同步的，调用者期望 `++it` 立即返回。如果需要等异步 I/O，应该用 `async_generator`。

---

## 2. `recursive_generator<T>` —— 递归场景的 O(1) 迭代

### 2.1 问题：普通 generator 递归遍历二叉树的性能灾难

```cpp
generator<int> traverse(Node* node) {
    if (!node) co_return;
    co_yield node->value;                    // ① 产出当前节点
    for (int v : traverse(node->left))       // ② 递归左子树
        co_yield v;
    for (int v : traverse(node->right))      // ③ 递归右子树
        co_yield v;
}
```

每层递归是一个独立的协程。当迭代器执行 `++it` 时：

```
++it → resume(最外层)
     → resume → 发现左子树还没完成
     → resume(左子树)
     → resume → 发现左-左子树还没完成
     → ... (一直递到叶子层)
     → resume(叶子) → co_yield → 产出值
     → 逐层返回...

每次 operator++() 的 resume/suspend 次数 = O(depth)
```

对平衡二叉树 depth ≈ log₂(N)，还不算太糟。但对链表式结构（退化成 depth = N），**每次 `++it` 要 resume N 次**，总复杂度 O(N²)。

### 2.2 解决方案：`pull()` 直接驱动叶子

`recursive_generator` 的核心思想：**所有嵌套的协程组成一棵树，`pull()` 直接 resume 最深的那个（叶子）**。

```cpp
struct promise_type {
    promise_type* m_root;       // 指向根 promise（永远不变）
    promise_type* m_parentOrLeaf; // 根: 指向叶子 / 子: 指向父

    void pull() noexcept {
        m_parentOrLeaf->resume();  // 直接 resume 叶子协程！O(1)！

        // 叶子完成后，沿着 m_parentOrLeaf 链回退
        while (m_parentOrLeaf != this && m_parentOrLeaf->is_complete()) {
            m_parentOrLeaf = m_parentOrLeaf->m_parentOrLeaf;
            m_parentOrLeaf->resume();
        }
    }
};
```

> 💡 `m_parentOrLeaf` 的双重身份：对于根 promise，指向当前能产生值的叶子协程；对于子 promise，指向它的父协程（完成后沿着这条链回退）。

### 2.3 嵌套 yield 如何建立协程树

当 `co_yield recursive_generator` 时：

```cpp
auto yield_value(recursive_generator&& generator) noexcept {
    if (generator.m_promise != nullptr) {
        // 1. 把子 promise 插入树中
        m_root->m_parentOrLeaf = generator.m_promise;     // 根现在指向新叶子
        generator.m_promise->m_root = m_root;             // 子记录根
        generator.m_promise->m_parentOrLeaf = this;       // 子记录父

        // 2. 启动子协程
        generator.m_promise->resume();

        // 3. 如果子没有立即完成 → 挂起当前协程
        if (!generator.m_promise->is_complete()) {
            return awaitable{generator.m_promise};
        }

        // 4. 子同步完成 → 回退
        m_root->m_parentOrLeaf = this;
    }
    return awaitable{nullptr};
}
```

### 2.4 性能对比

```
遍历深度 N 的左倾树（链表式）:

generator:              recursive_generator:
  ++it1: N 次 resume      ++it1: 1 次 resume
  ++it2: N-1 次 resume    ++it2: 1 次 resume
  ...                     ...
  总计: O(N²)             总计: O(N)
```

用每个协程帧多存两个指针（`m_root` / `m_parentOrLeaf`）的空间换来了时间。

---

## 3. `async_generator<T>` —— 异步序列生成

### 3.1 问题场景

```cpp
// 每秒产生一个 tick
async_generator<int> ticker(int count, static_thread_pool& tp) {
    for (int i = 0; i < count; ++i) {
        co_await tp.schedule_after(1s);  // ← 可以用 co_await！
        co_yield i;
    }
}

task<> consumer(static_thread_pool& tp) {
    auto seq = ticker(10, tp);
    for co_await (int i : seq) {   // ← for co_await，不是 for(:)
        std::cout << "Tick " << i << std::endl;
    }
}
```

`for co_await` 是专门为 `async_generator` 设计的语法——每次迭代都需要 `co_await` 等待下一个值。

### 3.2 两种实现路径

cppcoro 的 `async_generator` 有**两套完全不同的实现**：

```cpp
#if CPPCORO_COMPILER_SUPPORTS_SYMMETRIC_TRANSFER
// 路径 A：对称转移版（~200 行）—— 极简优雅
#else
// 路径 B：原子状态机版（~600 行）—— 复杂但兼容
#endif
```

> 💡 有对称转移的时候，200 行搞定；没有的时候，600 行状态机。这 400 行的差距，就是编译器技术决定框架代码量的真实案例。

### 3.3 路径 A：对称转移版

#### 生产者↔消费者的对称转移链

```
消费者协程                       生产者协程
──────────                       ──────────
co_await gen.begin():
  await_suspend:
    m_promise->m_consumer = 消费者句柄
    return 生产者句柄 ────对称转移──→ 生产者开始执行
                                    co_await some_async()
                                    co_yield value1:
                                      m_currentValue = &value
                                      return m_consumer ──对称转移──→
  消费者被唤醒 ←──对称转移────
  *it → value1
  co_await ++it:
    await_suspend:
      记住消费者
      return 生产者句柄 ────对称转移──→ 生产者继续
                                    co_yield value2:
                                      return m_consumer ──对称转移──→
  消费者被唤醒 ←──对称转移────
  ...直到生产者 co_return:
    final_suspend → return m_consumer ──对称转移──→ 消费者发现结束
```

每次在消费者和生产者之间切换都是对称转移——**零栈帧增长**。

#### 极致简洁的核心代码

```cpp
// 生产者→消费者（co_yield 时）
class async_generator_yield_operation {
    std::coroutine_handle<> m_consumer;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<>) noexcept {
        return m_consumer;  // 对称转移到消费者！
    }
};

// 消费者→生产者（++it 时）
class async_generator_advance_operation {
    async_generator_promise_base* m_promise;
    std::coroutine_handle<> m_producerCoroutine;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> consumerCoroutine) noexcept {
        m_promise->m_consumerCoroutine = consumerCoroutine;
        return m_producerCoroutine;  // 对称转移到生产者！
    }
};
```

三行核心逻辑。对称转移让这一切成为可能。

### 3.4 路径 B：原子状态机版

老编译器不支持对称转移，只能用 `atomic<state>` + 手动 `resume()` 在消费者和生产者之间切换。五态状态机：

```
      消费者调用 ++it / begin()
            │
            ▼
    ┌──────────────────────┐
    │ value_not_ready_     │ 消费者活跃，等值
    │ consumer_active      │
    └──────┬───────────────┘
           │ [消费者挂起]
           ▼
    ┌──────────────────────┐
    │ value_not_ready_     │ 消费者已挂起
    │ consumer_suspended   │←───────────────┐
    └──────┬───────────────┘                 │
           │ yield_value()                  │
           ▼                                │
    ┌──────────────────────┐               │
    │ value_ready_         │ 值就绪，生产者活跃│
    │ producer_active      │               │
    └──────┬───────────────┘               │
           │ [生产者挂起]                    │
           ▼                                │
    ┌──────────────────────┐               │
    │ value_ready_         │ 值就绪，生产者挂起│
    │ producer_suspended   │───────────────┘
    └──────────────────────┘   消费者取走值后 ++it
```

每个状态转换都有一个 CAS 守卫——只有一个线程能成功改变状态，另一个看到后调整行为。

### 3.5 取消安全

```cpp
~async_generator() {
    if (m_coroutine) {
        if (m_coroutine.promise().request_cancellation()) {
            m_coroutine.destroy();  // 生产者在挂起点 → 安全销毁
        }
        // 否则：生产者在活跃执行中 → 等它自己到下个挂起点自毁
    }
}
```

`request_cancellation()` 检查生产者是否在挂起点：
- 是 → 同步销毁（安全，没有活跃栈帧）
- 否 → 让协程自己走到下一个挂起点

> ⚠️ 取消不会强杀正在执行的协程。如果生产者在一个永远不会完成的 `co_await` 上挂着，它永远不会到达下一个挂起点。这是 async_generator 的一个已知局限。

---

## 4. 三种生成器对比

| 特性         | `generator`      | `recursive_generator`       | `async_generator`      |
| ------------ | ---------------- | --------------------------- | ---------------------- |
| `co_await`   | ❌                | ❌                           | ✅                      |
| `co_yield`   | ✅                | ✅                           | ✅                      |
| 嵌套 yield   | ❌                | ✅                           | ❌                      |
| 迭代方式     | `for(:)`         | `for(:)`                    | `for co_await(:)`      |
| operator++() | `O(1)` resume    | `O(1)` pull（直接驱动叶子） | 对称转移或 CAS 状态机  |
| 复杂度       | 260 行           | 345 行                      | 1089 行                |
| 适用场景     | 同步、非递归数据 | 同步、递归数据              | 需要 `co_await` 的场景 |

---

## 回顾一下你学会了什么

1. **generator**：`co_yield` 存指针，`suspend_always` 两端挂起，`for(:)` 驱动
2. **recursive_generator**：协程树 + `pull()` 直接驱动叶子 → O(1) 迭代
3. **`m_parentOrLeaf`** 的双重身份——设计精妙之处
4. **async_generator 路径 A**：对称转移版，生产者↔消费者来回跳，200 行
5. **async_generator 路径 B**：五态 `atomic<state>` + CAS，600+ 行
6. **取消**：`request_cancellation()` 不强制杀协程，只等它自己到下一个挂起点

---

## 动手练习

### 练习 1：写一个 generator 的 `fmap`

```cpp
// 实现 generator::fmap(f, gen)：对 gen 的每个值应用 f
// 提示：fmap 本身是一个 generator 协程
template<typename F, typename T>
generator</* f(T) 的返回类型 */> fmap(F f, generator<T> source) {
    // ...
}
```

### 练习 2：用 async_generator 实现一个分页加载器

```cpp
// 模拟"分页加载"的数据源
async_generator<int> paginated_loader(int total, int pageSize) {
    // 每次 co_await 模拟加载一页，co_yield 每个元素
}
```

---

## 常见坑

### 坑 1：generator 析构前没消费完

```cpp
{
    auto gen = fibonacci();
    auto it = gen.begin();
    std::cout << *it;       // 只读了第一个
    ++it;                   // 第二个
    // gen 析构 → handle.destroy() → 协程停在 co_yield 处被强杀
    // 局部变量的析构函数会正常执行（编译器保证）
}
```

### 坑 2：co_yield 返回值的地址在下次 resume 后失效

```cpp
generator<const int&> bad_generator() {
    int x = 0;
    while (true) {
        co_yield x;  // 存了 x 的地址
        x++;         // 修改了 x ← 消费者手里的地址还指向旧位置 → UB
    }
}
```

`yield_value` 存的是值的地址。`co_yield` 后协程挂起，消费者持有一个指向协程帧内变量的指针。如果恢复后修改了那个变量，消费者的指针就指向了错误的值。

### 坑 3：async_generator 循环中间退出 → 可能等不到取消

```cpp
auto gen = ticker(999, tp);
for co_await (int i : gen) {
    if (i > 10) break;  // 中途退出
    // gen 析构 → 等生产者到下个 co_yield 自毁
    // 如果生产者卡在一个不会完成的 co_await 上 → 永远等不到
}
```

---

> 下一篇：[协程调度器到底在调度什么？从 inline_scheduler 到工作窃取线程池](05-调度器与IO.md)
