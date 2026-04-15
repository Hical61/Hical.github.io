+++
title = '综合项目A：性能压测与分析'
date = '2026-04-15'
draft = false
tags = ["C++", "性能测试", "QPS", "PMR", "基准测试", "Hical", "学习笔记"]
categories = ["Hical框架"]
description = "学会使用框架自带的 benchmark 工具进行压测，理解 QPS、延迟分位数等性能指标，分析 PMR 内存池对性能的实际影响。"
+++

# 综合项目A：性能压测与分析 - 学习笔记

> 学会使用框架自带的 benchmark 工具进行压测，理解 QPS、延迟分位数等性能指标，分析 PMR 内存池对性能的实际影响。

---

## 一、框架提供的压测工具一览

| 工具                 | 文件                          | 目标              | 指标                      |
| -------------------- | ----------------------------- | ----------------- | ------------------------- |
| **http_benchmark**   | `examples/http_benchmark.cpp` | HTTP 服务器端到端 | QPS、P50/P90/P95/P99 延迟 |
| **benchmark**        | `examples/benchmark.cpp`      | TCP Echo 服务器   | QPS、平均延迟             |
| **pmr_benchmark**    | `examples/pmr_benchmark.cpp`  | 内存分配策略      | 分配/释放耗时(ms)         |
| **pmr_poc**          | `examples/pmr_poc.cpp`        | PMR 功能验证      | 四种场景对比              |
| **test_router_perf** | `tests/test_router_perf.cpp`  | 路由查找          | 每次分发延迟(ns)          |

---

## 二、http_benchmark — HTTP 压测工具

### 2.1 架构设计

**源码位置**：`examples/http_benchmark.cpp`

```
┌───────────────────────────────────────────────┐
│              io_context (多线程共享)            │
│                                               │
│  ┌─────────┐ ┌─────────┐     ┌─────────┐     │
│  │Client 0 │ │Client 1 │ ... │Client N │     │
│  │(协程)   │ │(协程)   │     │(协程)   │     │
│  └────┬────┘ └────┬────┘     └────┬────┘     │
│       │           │               │           │
│       └───────────┼───────────────┘           │
│                   │                           │
│            Thread Pool                        │
│     Thread0  Thread1 ... ThreadM              │
└───────────────────────────────────────────────┘
```

每个 `HttpBenchClient` 是一个独立协程，在自己的 TCP 连接上发送指定数量的 HTTP 请求。

### 2.2 单个客户端协程流程

```cpp
boost::asio::awaitable<void> run() {
    // 1. DNS 解析
    auto endpoints = co_await resolver.async_resolve(host, port, use_awaitable);

    // 2. 建立连接
    beast::tcp_stream stream(io_);
    co_await stream.async_connect(endpoints, use_awaitable);

    // 3. 请求循环（Keep-Alive 复用连接）
    for (int i = 0; i < numRequests_; ++i) {
        http::request<http::string_body> req(method_, target_, 11);
        req.set(http::field::connection, "keep-alive");

        auto start = high_resolution_clock::now();

        co_await http::async_write(stream, req, use_awaitable);   // 发送
        co_await http::async_read(stream, buffer, res, use_awaitable); // 接收

        auto end = high_resolution_clock::now();
        double latencyMs = duration<double, std::milli>(end - start).count();

        latencies_.push_back(latencyMs);    // 记录延迟
        completed_.fetch_add(1);            // 计数
        buffer.consume(buffer.size());
    }

    // 4. 优雅关闭
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}
```

**关键设计**：
- **Keep-Alive**：每个客户端复用一个 TCP 连接发送多个请求，避免连接建立开销
- **协程并发**：N 个客户端协程在 io_context 上并发执行，协作式调度
- **逐请求计时**：`start` 到 `end` 精确测量每个请求的端到端延迟

### 2.3 延迟分位数统计

```cpp
void printHistogram(std::vector<double>& latencies) {
    std::sort(latencies.begin(), latencies.end());

    auto percentile = [&](double p) -> double {
        auto idx = static_cast<size_t>(latencies.size() * p);
        return latencies[idx];
    };

    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    // 输出：平均、P50、P90、P95、P99、最小、最大
}
```

**分位数的含义**：

| 指标     | 含义                   | 关注理由                 |
| -------- | ---------------------- | ------------------------ |
| **P50**  | 50% 的请求延迟低于此值 | 典型用户体验             |
| **P90**  | 90% 的请求延迟低于此值 | 大部分用户体验           |
| **P95**  | 95% 的请求延迟低于此值 | 性能 SLA 常用指标        |
| **P99**  | 99% 的请求延迟低于此值 | 长尾延迟（GC、锁竞争等） |
| **最大** | 最慢的一次请求         | 极端情况                 |

**为什么 P99 比平均值更重要？**

假设 100 个请求中 99 个耗时 1ms、1 个耗时 100ms：
- 平均延迟 = (99 × 1 + 100) / 100 = **1.99ms** ← 看起来很好
- P99 = **100ms** ← 暴露了真实的尾部延迟问题

P99 能捕获到被平均值掩盖的性能毛刺。

### 2.4 使用方式

```bash
# 编译
cmake --build build

# 先启动服务器
./build/examples/http_server 8080

# 运行压测
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
#                                host    port 并发 请求/连接 路径    方法

# POST 带 body
./build/examples/http_benchmark localhost 8080 50 1000 /api/echo POST '{"hello":"world"}'
```

**参数说明**：

| 参数         | 含义                    | 建议值      |
| ------------ | ----------------------- | ----------- |
| 并发数       | 同时连接的客户端数      | 10~200      |
| 每连接请求数 | 每个连接发多少个请求    | 100~10000   |
| 总请求数     | = 并发数 × 每连接请求数 | 1000~100000 |

### 2.5 输出解读示例

```
========== hical HTTP 基准测试 ==========
目标: localhost:8080/api/status
方法: GET
并发连接: 50
每连接请求: 1000
总请求数: 50000
=========================================

========== 测试结果 ==========
  总请求数:    50000
  成功请求:    50000
  失败请求:    0
  总耗时:      2345.67 ms
  QPS:         21316 req/s

  延迟分布:
    平均:  0.046 ms
    P50:   0.035 ms       ← 半数请求在 35 微秒内完成
    P90:   0.072 ms
    P95:   0.098 ms
    P99:   0.215 ms       ← 1% 的请求超过 215 微秒
    最小:  0.012 ms
    最大:  1.834 ms       ← 最慢的请求接近 2 毫秒
==============================
```

---

## 三、benchmark — TCP Echo 压测工具

### 3.1 设计对比

**源码位置**：`examples/benchmark.cpp`

| 维度     | http_benchmark  | benchmark   |
| -------- | --------------- | ----------- |
| 协议     | HTTP/1.1        | 裸 TCP      |
| 架构     | 协程式          | 回调式      |
| 延迟统计 | P50/P90/P95/P99 | 仅平均延迟  |
| 目标     | HttpServer      | echo_server |

### 3.2 回调式 vs 协程式的差异

**benchmark.cpp（回调式）**：

```cpp
void send_request() {
    boost::asio::async_write(socket_, buffer(msg),
        [this, msg](error_code ec, size_t) {
            if (!ec) {
                receive_response(msg.length());  // 回调嵌套
            }
        });
}

void receive_response(size_t expected_len) {
    boost::asio::async_read(socket_, buffer(buffer_, expected_len),
        [this](error_code ec, size_t) {
            if (!ec) {
                completed_++;
                send_request();  // 递归回调
            }
        });
}
```

**http_benchmark.cpp（协程式）**：

```cpp
for (int i = 0; i < numRequests_; ++i) {
    co_await http::async_write(stream, req, use_awaitable);
    co_await http::async_read(stream, buffer, res, use_awaitable);
    completed_++;
}
```

协程版本代码量减少约 60%，且更容易在请求间插入计时逻辑。

### 3.3 使用方式

```bash
# 先启动 echo_server
./build/examples/echo_server 8888

# 运行 TCP 压测
./build/examples/benchmark localhost 8888 100 1000
#                           host    port 并发 请求/连接
```

---

## 四、pmr_benchmark — 内存分配策略对比

### 4.1 测试维度

**源码位置**：`examples/pmr_benchmark.cpp`

共 3 组测试：

**测试1：单线程分配/释放性能**

对比 5 种分配器在不同块大小下的耗时：

| 分配器                | 64B  | 256B | 4KB  | 特点         |
| --------------------- | ---- | ---- | ---- | ------------ |
| `new/delete`          | 基准 | 基准 | 基准 | 全局堆锁     |
| `synchronized_pool`   | 快   | 快   | 快   | 线程安全池   |
| `unsynchronized_pool` | 很快 | 很快 | 很快 | 无锁池       |
| `monotonic`           | 极快 | 极快 | 极快 | 只分配不释放 |
| `hical threadLocal`   | 很快 | 很快 | 很快 | Hical 三级池 |

**预期排序**：`monotonic` > `unsync_pool` ≈ `hical threadLocal` > `sync_pool` > `new/delete`

**测试2：多线程并发分配**

```cpp
// N 个线程同时分配/释放
void benchMultiThread(int threadsCount, int iterationsPerThread, size_t allocSize) {
    // new/delete: 全局堆锁竞争严重
    // hical threadLocal: 每线程独享 unsynchronized_pool，无锁
}
```

多线程场景下 `hical threadLocal` 的优势最明显——各线程的分配完全独立，零锁竞争。

**测试3：PmrBuffer append 性能**

```cpp
// std::string vs PmrBuffer(默认) vs PmrBuffer(hical pool)
void benchPmrBuffer(int iterations, size_t appendSize) {
    // 对比追加数据的耗时
}
```

### 4.2 使用方式

```bash
./build/examples/pmr_benchmark
```

### 4.3 输出解读示例

```
========== hical pmr 内存池基准测试 ==========
硬件并发线程数: 8

=== 单线程分配/释放 (1000000 次, 256 字节) ===
  new/delete:       125.3 ms
  pmr sync_pool:     42.1 ms     ← 比 new/delete 快 3x
  pmr unsync_pool:   18.7 ms     ← 比 sync_pool 快 2x（无锁）
  pmr monotonic:      3.2 ms     ← 极致快（只移指针）
  hical threadLocal:  19.1 ms    ← 接近 unsync_pool

=== 多线程并发分配 (8 线程, 500000 次/线程, 256 字节) ===
  new/delete:       487.2 ms     ← 锁竞争严重
  hical threadLocal:  52.3 ms    ← 无锁，几乎线性扩展
    全局池分配次数: 128           ← 线程本地池向全局池的请求次数很少
    峰值分配: 524288 bytes

=== PmrBuffer append 性能 (1000000 次, 128 字节) ===
  std::string:       89.4 ms
  PmrBuffer(默认):   91.2 ms     ← 默认分配器与 string 相当
  PmrBuffer(pool):   45.6 ms     ← 使用 pool 快 2x
```

### 4.4 关键分析点

**为什么 monotonic 最快？**

```
new/delete:  malloc → 搜索空闲链表 → 返回指针 → free → 合并空闲块
unsync_pool: 在池缓存中查找 → 命中返回 / 未命中向上游申请
monotonic:   指针 += size → 返回      （就这一步！）
```

monotonic 的 allocate 就是指针前移，deallocate 是空操作。代价是不能单独释放。

**为什么 hical threadLocal 在多线程下接近线性扩展？**

```
Thread 0: → unsync_pool_0（无锁） → 偶尔向 global_sync_pool 要大块
Thread 1: → unsync_pool_1（无锁） → 偶尔向 global_sync_pool 要大块
...
Thread 7: → unsync_pool_7（无锁） → 偶尔向 global_sync_pool 要大块
```

99% 的分配在线程本地完成（无锁），只有池需要补充时才访问全局池（有锁但极少发生）。

---

## 五、test_router_perf — 路由查找性能

### 5.1 测试场景

**源码位置**：`tests/test_router_perf.cpp`（第6课已详细分析）

| 场景          | 路由数 | 查找目标        | 性能要求 |
| ------------- | ------ | --------------- | -------- |
| 静态-首条命中 | 100    | route0          | < 10us   |
| 静态-末条命中 | 100    | route99         | < 10us   |
| 静态-未命中   | 100    | nonexistent     | < 10us   |
| 参数路由命中  | 20     | resource10/{id} | < 50us   |
| 大量路由      | 1000   | route500        | < 10us   |

### 5.2 关键验证点

**哈希表 O(1) 验证**：

如果静态路由查找是真正的 O(1)，那么：
- 100 条路由查 route0 ≈ 查 route99 ≈ 查 route500（1000 条）
- 三者耗时应该在同一数量级

**参数路由 O(N) 验证**：

- 20 条参数路由查第 10 条 → 需要线性扫描前 10 条 → 比静态路由慢
- 但绝对值仍在 50us 以内（20 条路由不是瓶颈）

### 5.3 运行方式

```bash
# 通过 ctest 运行
ctest --test-dir build -R test_router_perf --output-on-failure

# 或直接运行
./build/tests/test_router_perf
```

---

## 六、pmr_poc — PMR 功能验证

### 6.1 四种场景

**源码位置**：`examples/pmr_poc.cpp`（第4课已详细分析）

| 场景       | 对比                                        | 验证点                      |
| ---------- | ------------------------------------------- | --------------------------- |
| 复用缓冲区 | `std::vector` vs `pmr::vector(threadLocal)` | 线程本地池的复用加速        |
| 批量分配   | `std::allocator` vs `pmr::monotonic`        | 单调池的整体释放优势        |
| PmrBuffer  | 功能测试                                    | append/read/findCRLF 正确性 |
| 多线程并发 | 4 线程 × 50000 次                           | 无崩溃、统计数据合理        |

---

## 七、压测实践步骤

### 步骤1：编译所有工具

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release    # Release 模式！
cmake --build build -j$(nproc)
```

**必须用 Release 模式**，Debug 模式下的性能数据没有参考价值（关闭了优化，开启了 sanitizer）。

### 步骤2：运行 PMR 内存基准

```bash
./build/examples/pmr_benchmark

# 记录：
# - 单线程各分配器的绝对耗时
# - 多线程 new/delete vs hical threadLocal 的加速比
# - PmrBuffer(pool) vs PmrBuffer(默认) 的加速比
```

### 步骤3：运行路由性能测试

```bash
./build/tests/test_router_perf

# 记录：
# - 静态路由首条/末条/1000条的 ns/op
# - 参数路由的 ns/op
# - 验证哈希表 O(1) 特性（各场景耗时接近）
```

### 步骤4：启动 HTTP 服务器

```bash
./build/examples/http_server 8080
```

### 步骤5：运行 HTTP 压测

```bash
# 场景1：小并发，验证基本性能
./build/examples/http_benchmark localhost 8080 10 1000 / GET

# 场景2：中等并发
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET

# 场景3：高并发
./build/examples/http_benchmark localhost 8080 200 500 /api/status GET

# 场景4：POST 带 body
./build/examples/http_benchmark localhost 8080 50 1000 /api/echo POST '{"data":"test"}'

# 场景5：路径参数
./build/examples/http_benchmark localhost 8080 50 1000 /users/42 GET
```

### 步骤6：整理数据

建议用表格记录：

| 场景            | 并发 | 总请求 | QPS | P50 | P99 | 错误数 |
| --------------- | ---- | ------ | --- | --- | --- | ------ |
| GET /           | 10   | 10000  | ?   | ?   | ?   | 0      |
| GET /api/status | 50   | 50000  | ?   | ?   | ?   | 0      |
| GET /api/status | 200  | 100000 | ?   | ?   | ?   | 0      |
| POST /api/echo  | 50   | 50000  | ?   | ?   | ?   | 0      |
| GET /users/42   | 50   | 50000  | ?   | ?   | ?   | 0      |

---

## 八、性能分析方法论

### 8.1 QPS 与延迟的关系

```
低负载时：
    QPS ↑ → 延迟几乎不变（系统空闲资源充足）

中等负载：
    QPS ↑ → P99 开始上升（队列等待时间增加）

高负载（饱和）：
    QPS 不再增长 → 延迟急剧上升（CPU/内存/fd 达到瓶颈）
```

找到**拐点**（QPS 不再增长而延迟陡升）就是系统的极限吞吐。

### 8.2 瓶颈定位

| 现象             | 可能瓶颈                          | 排查方法                       |
| ---------------- | --------------------------------- | ------------------------------ |
| QPS 低，CPU 空闲 | IO 等待、锁竞争                   | `top` 看 CPU%，`perf` 看锁等待 |
| QPS 低，CPU 满   | 计算密集（JSON 序列化、路由匹配） | `perf record` 热点分析         |
| P99 远高于 P50   | GC（无）、锁竞争、线程切换        | 对比单线程 vs 多线程 P99       |
| 错误率高         | fd 耗尽、内存不足                 | `ulimit -n` 检查 fd 上限       |

### 8.3 静态路由 vs 参数路由的性能差距

从 test_router_perf 的数据可以观察：

```
静态路由（哈希表）：~100-500 ns/op      ← O(1)
参数路由（线性扫描）：~1000-5000 ns/op   ← O(N)，N=路由数

差距：参数路由约慢 5-10x
```

但在实际 HTTP 请求中，路由查找耗时占总请求耗时的 **< 1%**（网络 IO 是大头），所以这个差距在端到端 QPS 上几乎不可见。

### 8.4 PMR 对 HTTP 性能的影响

对比方式：修改 `HttpServer::handleSession` 中的分配器（PMR pool vs 默认 new/delete），分别压测相同场景。

预期结果：
- **低并发**：差距不明显（CPU 不是瓶颈）
- **高并发**：PMR 的 QPS 更高、P99 更低（减少了 malloc 锁竞争和碎片）

---

## 九、关键问题思考与回答

**Q1: 为什么压测必须用 Release 模式？**

> Debug 模式下编译器不做优化（-O0），且可能开启 AddressSanitizer/UBSanitizer 等检测工具，性能可能比 Release 慢 10-100 倍。压测数据只有在 Release 模式下才有参考价值。

**Q2: QPS 和 P99 哪个更重要？**

> 取决于场景：
> - **吞吐优先**（批量任务、日志收集）：关注 QPS
> - **延迟敏感**（游戏服务器、交易系统）：关注 P99/P999
>
> 游戏服务器通常更关注 P99——一个请求卡 100ms 对玩家来说就是明显的"卡顿"。

**Q3: 如何解读 "全局池分配次数: 128" ？**

> 这是 pmr_benchmark 多线程测试输出的 TrackedResource 统计。含义：
> - 8 个线程共执行了 400 万次分配（8 × 500000）
> - 其中只有 128 次穿透到全局同步池
> - 命中率 = (4000000 - 128) / 4000000 ≈ **99.997%**
>
> 这说明线程本地池的缓存极其有效，几乎所有分配都在无锁路径完成。

**Q4: 压测中的"错误请求"通常是什么原因？**

> 1. **fd 耗尽**：每个 TCP 连接消耗一个 fd，超过 `ulimit -n` 限制会 accept 失败
> 2. **连接队列满**：`listen()` 的 backlog 满了，新连接被拒绝
> 3. **超时**：服务器处理太慢，客户端等待超时
> 4. **内存不足**：高并发下每个连接的缓冲区累计消耗大量内存
>
> 排查：先看错误类型（connection refused / timeout / broken pipe），再定位根因。

---

## 十、与游戏服务器性能调优的对比

| 压测概念     | 游戏服务器等价物                      |
| ------------ | ------------------------------------- |
| QPS          | CCU（在线人数）下的消息处理速率       |
| P99 延迟     | 玩家操作响应时间（< 50ms 为优秀）     |
| 并发连接数   | 同时在线玩家数                        |
| Keep-Alive   | 游戏客户端长连接                      |
| body_limit   | 消息包最大长度                        |
| fd 上限      | 最大连接数瓶颈（`ulimit -n 65535`）   |
| PMR 内存池   | 对象池 / 消息缓冲池（减少 GC 和碎片） |
| monotonic 池 | 帧级分配器（每帧结束整体释放）        |

---

*下一步：综合项目 B — 动手扩展新功能（Rate Limiter / 静态文件服务 / JSON RPC），将前面所学融会贯通。*
