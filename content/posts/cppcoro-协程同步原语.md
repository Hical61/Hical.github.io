+++
title = '用一个生产者-消费者场景，把 cppcoro 的 7 个协程同步原语全部串起来'
date = '2024-11-15'
draft = false
tags = ["C++", "cppcoro", "协程", "C++20", "源码分析", "同步", "async_mutex"]
categories = ["C++"]
description = "拆开 cppcoro 给你看 · 第 2 篇。用同一个生产者-消费者场景逐步升级，串起 async_mutex、async_latch 等 7 个协程同步原语。"
+++

# 用一个生产者-消费者场景，把 cppcoro 的 7 个协程同步原语全部串起来

> 本专栏文章：拆开 cppcoro 给你看 · 第 2 篇

上一篇文章我把 cppcoro 的 `task<T>` 拆成了 5 个版本迭代。今天要搞定协程世界里另一个核心问题：**多个协程之间怎么同步？**

市面上的做法是一张 API 列表——"这是 async_mutex，这是 async_latch，啥时候用自己看着办"。我不这么讲。本文用**同一个生产者-消费者场景，从简单到复杂逐步升级**，每级只比上一级多一个特性，一共串起 7 个同步原语：

```
场景：生产者协程产生数据，消费者协程处理数据

Level 1: 单消费者，发信号通知 → single_consumer_event
Level 2: 同上，但信号自动复位  → single_consumer_async_auto_reset_event
Level 3: 多消费者，手动复位     → async_manual_reset_event
Level 4: 多消费者，自动复位     → async_auto_reset_event
Level 5: 共享数据的互斥访问     → async_mutex
Level 6: 等待 N 个操作完成      → async_latch
Bonus:  when_all 的核心组件     → when_all_counter
```

每级都是可运行的代码。

---

## Level 1：单消费者 + 手动复位信号

### 问题

```cpp
// 生产者：生成数据后通知消费者
// 消费者：等通知到了才处理
// 约束：只有一个消费者

std::string shared_data;

task<> producer(some_event& ready) {
    shared_data = "hello from producer";
    ready.set();  // "数据准备好了"
    co_return;
}

task<> consumer(some_event& ready) {
    co_await ready;  // 等信号
    std::cout << shared_data << std::endl;
}
```

### 设计思路

需要一个三态对象：

```
not_set ──set()──→ set
   ↑                 │
   │  co_await       │ reset()
   │                 ↓
 not_set_consumer ←─ not_set
    _waiting
```

### cppcoro 实现

[single_consumer_event.hpp](https://github.com/lewissbaker/cppcoro/blob/main/include/cppcoro/single_consumer_event.hpp)（128 行）：

```cpp
class single_consumer_event {
    enum class state {
        not_set,                    // 未设置，没人在等
        not_set_consumer_waiting,   // 未设置，有人在等
        set                         // 已设置
    };
    std::atomic<state> m_state;
    std::coroutine_handle<> m_awaiter;  // 存等待者的句柄
};
```

#### `co_await` 的流程

```cpp
auto operator co_await() noexcept {
    class awaiter {
        single_consumer_event& m_event;

        bool await_ready() const noexcept {
            return m_event.is_set();  // 检查 m_state 是否是 set
        }

        bool await_suspend(std::coroutine_handle<> h) {
            // 第 1 步：把等待者句柄存到事件对象中
            m_event.m_awaiter = h;

            // 第 2 步：原子地从 not_set 改成 not_set_consumer_waiting
            state oldState = state::not_set;
            return m_event.m_state.compare_exchange_strong(
                oldState,                        // 期望是 not_set
                state::not_set_consumer_waiting, // 改成"有人在等"
                std::memory_order_release,
                std::memory_order_acquire);

            // CAS 成功 → 返回 true → 协程挂起
            // CAS 失败 → 有人在第1步和第2步之间调了 set()
            //         → 返回 false → 协程不挂起（继续执行）
        }

        void await_resume() noexcept {}
    };
    return awaiter{*this};
}
```

#### `set()` 的流程

```cpp
void set() {
    const state oldState = m_state.exchange(state::set,
        std::memory_order_acq_rel);

    if (oldState == state::not_set_consumer_waiting) {
        m_awaiter.resume();  // 只有"有人在等"时才唤醒
    }
}
```

#### 完整时序图

```
时间 →
生产者线程                    消费者协程
──────────                   ──────────
                             co_await event:
                               await_ready() → false
                               m_event.m_awaiter = 消费者句柄
                               CAS(not_set → waiting) → 成功
                               协程挂起 ← 返回 true
shared_data = "..."
event.set():
  exchange(waiting → set)
  旧状态 == waiting
  → m_awaiter.resume()  ───────→ 协程恢复
                                await_resume()
                                处理 shared_data
```

### 这个设计的脆弱性

如果在 `co_await` 的 `m_event.m_awaiter = h` 和 CAS 之间有**另一个协程**也调了 `co_await`，第二个协程的句柄会覆盖第一个的，第一个协程永远不会被唤醒。

这就是为什么它叫 `single_consumer_event`——只能有一个消费者。这是性能换安全的取舍：省掉了链表操作的开销。

---

## Level 2：单消费者 + 自动复位信号

### 问题

和 Level 1 几乎一样，但每次 `set()` 唤醒消费者后**自动恢复成 not_set**，不用手动 `reset()`。

### 设计思路

把"set"和"有等待者"两个状态编码到一个 `void*` 中：

```
nullptr → 未设置，无等待者
this    → 已设置
其他    → 未设置，指向存储 coroutine_handle 的地址
```

> 💡 一个原子指针替代了 `enum + coroutine_handle`——这个模式在 cppcoro 的多模块中反复出现。

省掉了一个成员变量——等待者的句柄直接存在原子变量里。

### cppcoro 实现

[single_consumer_async_auto_reset_event.hpp](https://github.com/lewissbaker/cppcoro/blob/main/include/cppcoro/single_consumer_async_auto_reset_event.hpp)（101 行）：

```cpp
void set() noexcept {
    void* oldValue = m_state.exchange(this, std::memory_order_release);

    if (oldValue != nullptr && oldValue != this) {
        // oldValue 是协程句柄的地址
        auto handle = *static_cast<std::coroutine_handle<>*>(oldValue);

        // 原子复位（"自动 reset"的精髓）
        (void)m_state.exchange(nullptr, std::memory_order_acquire);

        // 唤醒等待者
        handle.resume();
    }
    // 如果 oldValue == this → 已经在 set 状态 → no-op
    // 如果 oldValue == nullptr → 没人在等 → 继续保持 set
}
```

**自动复位的语义**：`set()` 会立即把状态改回 `nullptr`，除非没人在等（保持 `this` 让下个 `co_await` 能直接拿到）。

---

## Level 3：多消费者 + 手动复位

### 问题

多个协程同时等待同一个事件。`set()` 一次，**所有**等待者都被唤醒。

### 设计思路：从单个句柄到链表

Level 1 只能存一个协程句柄，要支持多消费者，需要存一个**链表**。

`m_state` 仍然用 `atomic<void*>`，但含义扩充：

```
nullptr → 未设置，无等待者
this    → 已设置
其他    → 未设置，指向等待者链表的头节点
```

每个等待者在自己的协程帧中存一个节点（`next` 指针 + `coroutine_handle`），入队时用 CAS 循环 push 到链表头。这就是"侵入式链表"——节点直接内嵌在 `operator co_await()` 返回的临时对象中，这个对象就在协程帧里。

### cppcoro 实现

[async_manual_reset_event.hpp](https://github.com/lewissbaker/cppcoro/blob/main/include/cppcoro/async_manual_reset_event.hpp)（104 行）+ .cpp（100 行）：

```cpp
class async_manual_reset_event {
    mutable std::atomic<void*> m_state;  // 三态指针
};

class async_manual_reset_event_operation {
    const async_manual_reset_event& m_event;
    async_manual_reset_event_operation* m_next;  // ← 链表指针
    std::coroutine_handle<> m_awaiter;            // ← 等待的协程
};
```

#### `set()` —— 唤醒所有等待者

```cpp
void set() noexcept {
    void* const setState = static_cast<void*>(this);
    void* oldState = m_state.exchange(setState, std::memory_order_acq_rel);

    if (oldState != setState) {
        auto* current = static_cast<async_manual_reset_event_operation*>(oldState);
        while (current != nullptr) {
            auto* next = current->m_next;          // 先存 next！
            current->m_awaiter.resume();           // resume 可能销毁 current
            current = next;
        }
    }
}
```

注意：必须先读 `current->m_next`，再 `resume()`。因为 `resume()` 可能立即销毁 `current` 指向的节点（协程结束时协程帧被清理）。

#### `await_suspend` —— CAS 循环入队

```cpp
bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
    m_awaiter = awaiter;

    const void* const setState = static_cast<const void*>(&m_event);

    void* oldState = m_event.m_state.load(std::memory_order_acquire);
    do {
        if (oldState == setState) {
            return false;  // 已被 set → 不挂起
        }
        m_next = static_cast<async_manual_reset_event_operation*>(oldState);
    } while (!m_event.m_state.compare_exchange_weak(
        oldState,
        static_cast<void*>(this),       // 新链表头 = this
        std::memory_order_release,
        std::memory_order_acquire));

    return true;  // 挂起
}
```

这是标准的**无锁栈入队**模式（Treiber Stack）。入队是 LIFO（后入队的先被唤醒），但 manual-reset 语义是"所有人醒来"，顺序不重要。

---

## Level 4：多消费者 + 自动复位

### 问题

Level 3 每次 set 唤醒所有人。但有时我们要的是：`set()` 一次**最多**唤醒一个等待者。这是 auto-reset 的语义。

### 设计思路：双计数器模式

用一个 `atomic<uint64_t>` 同时存两个 32 位计数器：

```
位 0-31:  set_count     (set() 被调用了几次)
位 32-63: waiter_count  (有几个等待者)
```

**为什么用一个 64 位原子而不是两个 32 位？** 因为需要对两个计数器的读-改-写是原子的。分成两个变量你就没法原子地判断"set_count > 0 且 waiter_count > 0"。

可被唤醒的等待者数量 = `min(set_count, waiter_count)`。

### cppcoro 实现

[async_auto_reset_event.cpp](https://github.com/lewissbaker/cppcoro/blob/main/lib/async_auto_reset_event.cpp)（286 行）——整个库中**最复杂的同步原语**：

```
场景: 3 个消费者等待，生产者调用 set() 2 次
时间 →
                                     m_state:
消费者1: co_await                     (set=0, waiter=0)
  await_suspend:
    入队消费者1
    m_state.fetch_add(waiter_inc)     (set=0, waiter=1)
    set_count==0 → 不获取"锁"

消费者2: co_await                     (set=0, waiter=1)
    入队消费者2
    m_state.fetch_add(waiter_inc)     (set=0, waiter=2)
    set_count==0 → 不获取"锁"

生产者: set()                         (set=0, waiter=2)
  fetch_add(set_inc)                  (set=1, waiter=2)
  旧 set_count==0 && 旧 waiter_count>0
  → 获得"锁"！进入 resume_waiters(1,2)

resume_waiters(1,2):
  resumable = min(1,2) = 1            ← 只唤醒 1 个！
  出队 1 个等待者
  fetch_sub(1个set + 1个waiter)       (set=0, waiter=1)
  → 唤醒消费者1

生产者: set() 第二次                   (set=0, waiter=1)
  fetch_add(set_inc)                  (set=1, waiter=1)
  → 唤醒消费者2

// 消费者3 还在等...没有更多 set() 就不会被唤醒
```

### 为什么需要引用计数

每个 `async_auto_reset_event_operation` 构造时 `m_refCount = 2`：
- **引用 1**：`await_suspend()` 持有（入队完成后释放）
- **引用 2**：`resume_waiters()` 持有（出队时）

> 💡 没有引用计数的话，`resume_waiters` 可能在一个等待者还没完成入队时就去 `resume()` 它，导致 UAF。

---

## Level 5：互斥访问共享数据

### 问题

两个协程并发地修改共享数据，需要互斥：

```cpp
async_mutex mtx;
std::vector<int> shared_data;

task<> writer(int value) {
    co_await mtx;      // 获取互斥锁（被占用就挂起）
    shared_data.push_back(value);
    mtx.unlock();      // 释放
}
```

### cppcoro 实现

[async_mutex.hpp](https://github.com/lewissbaker/cppcoro/blob/main/include/cppcoro/async_mutex.hpp)（200 行）+ .cpp（122 行）：

```cpp
class async_mutex {
    // 三种状态：
    // not_locked (1)     — 空闲
    // locked_no_waiters (0) — 被占用，没人在排队
    // 其他值              — 被占用，等待者链表头
    std::atomic<std::uintptr_t> m_state;
    async_mutex_lock_operation* m_waiters;
};
```

#### FIFO 公平性：链表反转

`await_suspend` 中入队是 LIFO（CAS push 到链表头），`unlock()` 中反转链表变成 FIFO：

```cpp
void unlock() {
    async_mutex_lock_operation* waitersHead = m_waiters;
    if (waitersHead == nullptr) {
        // ...检查 m_state 上有没有新入队的...

        // 反转链表: LIFO → FIFO
        auto* next = reinterpret_cast<async_mutex_lock_operation*>(oldState);
        do {
            auto* temp = next->m_next;
            next->m_next = waitersHead;
            waitersHead = next;
            next = temp;
        } while (next != nullptr);
    }

    // 弹出 FIFO 顺序的头部，唤醒它
    m_waiters = waitersHead->m_next;
    waitersHead->m_awaiter.resume();
}
```

**链表反转确保**：先调 `co_await mtx` 的协程先获得锁。这个模式在 cppcoro 的多个模块中重复出现——生产端用 CAS LIFO 无锁入队，消费端加锁反转变成 FIFO。

---

## Level 6：等待 N 个操作都完成

### 设计：组合已有原语

```cpp
class async_latch {
    std::atomic<std::ptrdiff_t> m_count;
    async_manual_reset_event m_event;   // 复用 Level 3 的事件！
};
```

`async_latch` 没有自己实现任何同步逻辑——它只是一个计数器 + 一个事件的组合：

```cpp
async_latch(std::ptrdiff_t initialCount) noexcept
    : m_count(initialCount)
    , m_event(initialCount <= 0)  // 如果初始就 ≤ 0，直接 set
{}

void count_down(std::ptrdiff_t n = 1) noexcept {
    if (m_count.fetch_sub(n, std::memory_order_acq_rel) <= n) {
        m_event.set();  // 最后一个到达的打开门闩
    }
}

auto operator co_await() const noexcept {
    return m_event.operator co_await();  // 委托给 event
}
```

`fetch_sub(n) <= n` 的含义：假设 `initialCount = 3`，三次 `count_down(1)` 依次返回 3、2、1。只有第三次（返回值 1 ≤ 1）触发 set。

> 💡 一个好的库不重写已有的机制，而是组合它们。async_latch 75 行，async_manual_reset_event 200 行——用组合而不是重写。

---

## Bonus：when_all_counter

```cpp
class when_all_counter {
    std::atomic<std::size_t> m_count;
    std::coroutine_handle<> m_awaitingCoroutine;
};
```

`when_all` 和 `when_all_ready` 的内部核心就是这个计数器。下一篇文章详细拆解。

---

## 回顾一下你学会了什么

1. **Level 1-2**：single_consumer 用 `atomic<void*>` 编码三态——nullptr / this / &waiter
2. **Level 3-4**：多消费者用侵入式链表——节点存在协程帧里
3. **CAS 循环入队**：读 → 写 next → CAS → 失败重试，是标准无锁模式
4. **链表反转 → FIFO**：LIFO 入队（CAS 无锁）+ FIFO 出队（加锁反转），保证公平
5. **`async_auto_reset_event` 双计数器**：一个 `atomic<uint64_t>` 存 set_count+waiter_count
6. **引用计数**防止 `await_suspend` 和 `resume_waiters` 竞态导致 UAF
7. **"锁"转移**：CAS 状态转换隐式传递"谁负责唤醒"的责任
8. **`async_latch`**：75 行，组合已有原语而不是重写

---

## 动手练习

### 练习 1：手写 single_consumer_event

```cpp
#include <coroutine>
#include <atomic>
#include <iostream>
#include <thread>

struct SingleConsumerEvent {
    enum class State { not_set, waiting, set };
    std::atomic<State> m_state{State::not_set};
    std::coroutine_handle<> m_waiter;

    void set() {
        auto old = m_state.exchange(State::set);
        if (old == State::waiting) m_waiter.resume();
    }

    auto operator co_await() {
        struct Awaiter {
            SingleConsumerEvent& e;
            bool await_ready() { return e.m_state.load() == State::set; }
            bool await_suspend(std::coroutine_handle<> h) {
                e.m_waiter = h;
                State expected = State::not_set;
                return e.m_state.compare_exchange_strong(expected, State::waiting);
            }
            void await_resume() {}
        };
        return Awaiter{*this};
    }
};
```

### 练习 2：加上 reset()

`reset()` 从 `set` CAS 回 `not_set`。修改 producer，set 之后 sleep 一会再 reset，观察消费者能否多次等待。

---

## 常见坑

### 坑 1：单消费者事件被多个协程使用

```cpp
single_consumer_event evt;
// 协程 A: co_await evt;
// 协程 B: co_await evt;   ← 同时等！只有一个能被正确唤醒！
```

### 坑 2：async_mutex 不绑定线程

```cpp
async_mutex mtx;
// 线程 1: co_await mtx;
// 线程 2: mtx.unlock();  ← 即使线程 2 没持有锁也能 unlock！
```

和 `std::mutex` 不同——cppcoro 的 `async_mutex` 不绑定线程，只关心"谁最后调了 lock_async"。

### 坑 3：async_latch count_down 过多

```cpp
async_latch latch(3);
latch.count_down(5);  // count 变 -2，但 latch 已经被 set（不可逆）
```

一旦 count ≤ 0，latch 永远保持 set 状态。

---

> 下一篇：[协程世界的并发编排：从需求反推 when_all 的实现](03-组合器与并发.md)
