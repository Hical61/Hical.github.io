+++
title = 'Heaptrack：找出 C++ 程序中的无效内存分配'
date = '2026-05-15'
draft = false
tags = ["C++", "性能分析", "Heaptrack", "内���管理", "PMR", "Hical"]
categories = ["性能优化"]
description = "用 Heaptrack 追踪每一次堆分配，精确定位临时分配热点，结合 string_view、reserve、PMR 内存池等手段将内存分配开销从 8% 降到 < 0.1%。"
+++

# Heaptrack：找出 C++ 程序中的无效内存分配

> 你的火焰图上 `malloc`/`free` 占了 8% CPU。你知道分配太频繁了，但——是哪个函数在疯狂 new？每次 new 了多少字节？有没有更好的办法？

---

## 故事：每秒 17000 次 malloc，但只有 41 次是浪费的

对我的 C++20/26 Web 框架（Hical）做 Heaptrack 分析时发现：136K QPS 下每秒 17457 次堆分配，但临时分配（分配后很快释放）只有 41 次/秒——说明 PMR 内存池策略生效了。

但第一版代码没有 PMR 时，临时分配高达 **13 万次/秒**。Heaptrack 精确告诉了我哪些 `std::string` 和 `std::vector` 是罪魁祸首，逐个消灭后内存分配开销从 8% 降到 < 0.1%。

这篇教你用 Heaptrack 做同样的事——**精确定位哪个函数在做无效分配，然后干掉它**。

---

## 一、Heaptrack 是什么

Heaptrack 是一个**堆内存分配追踪器**，记录程序运行期间的每一次 `malloc`/`new`/`free`/`delete`，告诉你：

- 总共分配了多少次？多少字节？
- **哪个函数**分配最多？（完整调用栈）
- 峰值内存使用在哪个时间点？
- 有没有泄漏（分配了但从未释放）？
- **临时分配**有多少？（分配后很快释放——这是优化首要目标）

### 对比 Valgrind Massif

|          | Heaptrack             | Valgrind --tool=massif |
| -------- | --------------------- | ---------------------- |
| 性能开销 | **2~5x** 减速         | 20~50x 减速            |
| 数据粒度 | 每次分配的完整调用栈  | 定期快照               |
| GUI      | heaptrack_gui（丰富） | ms_print（文本）       |
| 适用场景 | **日常分析（推荐）**  | 极精确内存画像         |

一句话：**Heaptrack 是 Valgrind Massif 的现代替代品，快 10 倍，信息更全。**

---

## 二、安装（1 分钟）

```bash
sudo apt install -y heaptrack heaptrack-gui

# 验证
heaptrack --version   # heaptrack 1.x
```

---

## 三、使用方法

### 3.1 方式一：启动时录制（推荐）

最可靠，不依赖 ptrace 权限：

```bash
# 终端 1：heaptrack 包裹启动
heaptrack ./your_server
# 输出: heaptrack output will be written to "heaptrack.your_server.12345.zst"
# 服务器开始监听，终端被占住
```

```bash
# 终端 2：施压
wrk -t4 -c100 -d30s http://127.0.0.1:8080/

# 压测结束后回终端 1 按 Ctrl+C
```

Ctrl+C 后自动输出摘要：

```
heaptrack stats:
        allocations:            1308297
        leaked allocations:     6
        temporary allocations:  622
```

### 3.2 方式二：附着到已运行的进程

```bash
# 需要先放开 ptrace 权限
sudo sysctl -w kernel.yama.ptrace_scope=0

# 附着
heaptrack --pid $(pidof your_server)

# 压测后 Ctrl+C 停止录制
```

> 注意：`--pid` 方式偶尔因注入时机问题数据为空。不稳定时用方式一。

---

## 四、分析结果

### 4.1 文本分析（SSH 终端直接用）

```bash
FILE=heaptrack.your_server.12345.zst

# 全局摘要（最常用）
heaptrack_print $FILE | tail -10

# 分配次数排名
heaptrack_print $FILE | sed -n '/^MOST CALLS TO ALLOCATION/,/^PEAK MEMORY/p' | head -60

# 峰值内存排名
heaptrack_print $FILE | sed -n '/^PEAK MEMORY CONSUMERS/,/^MOST TEMPORARY/p' | head -60

# 临时分配排名（优化重点！）
heaptrack_print $FILE | sed -n '/^MOST TEMPORARY/,/^total runtime/p' | head -60
```

### 4.2 GUI 分析

```bash
heaptrack_gui heaptrack.your_server.12345.zst
```

GUI 提供的关键视图：

| Tab                   | 看什么                        | 重点关注               |
| --------------------- | ----------------------------- | ---------------------- |
| Summary               | 总分配次数/字节、峰值、泄漏量 | 全局概览               |
| Bottom-Up             | 按分配量排序的调用栈          | **找到分配最多的函数** |
| Flame Graph           | 分配量火焰图                  | 直观定位热点           |
| Temporary Allocations | 分配后很快释放的              | **PMR 优化的首要目标** |

### 4.3 生成分配火焰图

```bash
heaptrack_print --flamegraph-cost-type allocations $FILE > heap.folded
~/FlameGraph/flamegraph.pl \
    --title "Heap Allocations" \
    --countname "allocs" \
    --colors mem \
    heap.folded > heap_flame.svg
```

和 CPU 火焰图用法完全一样——宽度代表分配次数，从顶往下追踪就能找到源头。

---

## 五、解读输出

### 5.1 全局摘要

```
total runtime: 74.94s.
calls to allocation functions: 1308297 (17457/s)
temporary memory allocations: 3110 (41/s)
peak heap memory consumption: 2.14M
total memory leaked: 1.06K
```

**逐行解读**：

| 指标     | 本例数据 | 怎么判断                                                         |
| -------- | -------- | ---------------------------------------------------------------- |
| 分配频率 | 17457/s  | 对比 QPS：136K QPS 下 17K alloc/s → 约 8 个请求才 1 次分配，很好 |
| 临时分配 | 41/s     | 占总分配的 0.24%，极低                                           |
| 峰值内存 | 2.14M    | 框架本身内存占用小                                               |
| 泄漏     | 1.06K    | 全局单例/静态对象，不是真正泄漏                                  |

### 5.2 分配次数排名

```
751586 calls with 1.14M peak from
    boost::asio::aligned_new              ← Asio 内部 handler 池
    └→ HttpServer::start()

195994 calls with 61.15K peak from
    HttpServer::handleSession             ← 协程帧（每请求 1 次）
    └→ HttpSessionImpl.cpp:767

195986 calls with 16.02K peak from
    boost::asio::async_write              ← 响应写入 handler
    └→ writeResponse (HttpSessionImpl.cpp:177)
```

**解读**：
- 75 万次 `aligned_new` 是 Asio 内部的 handler 回收池，有 thread-local recycling，不是每次都走系统 malloc
- 19.5 万次 handleSession 对应每请求的协程帧分配——这是 C++20 协程的固有开销
- 这些分配**不是问题**——它们的峰值内存很小（61K），说明在快速复用

### 5.3 临时分配排名（优化金矿）

```
2382 temporary (0.86%) from
    std::__new_allocator<>::allocate       ← STL 容器内部
    └→ handleSession (HttpSessionImpl.cpp:726)

696 temporary (0.09%) from
    boost::asio::aligned_new               ← Asio handler 回收不及时
```

**这才是要消灭的目标**。临时分配 = 分配后很快释放 = 浪费 CPU 周期在 malloc/free 上。

---

## 六、判断标准：什么时候需要优化

| 指标               | 健康      | 需要优化  | 优化方向                       |
| ------------------ | --------- | --------- | ------------------------------ |
| 临时分配占总分配比 | < 1%      | > 10%     | PMR / 栈分配 / reserve         |
| 临时分配频率       | < 100/s   | > 10000/s | 每秒万次 = 万次 cache 污染     |
| 峰值内存远超稳态   | 差值 < 2x | > 5x      | vector 未 reserve 导致反复扩容 |
| 火焰图 malloc 占比 | < 2%      | > 5%      | 分配器压力大                   |

---

## 七、常见优化手段

### 7.1 `std::string` → `std::string_view`

最常见的无效分配：把一段已有的文本拷贝到新 string 里，只为了读取。

```cpp
// 之前：每次解析请求头都 new 一个 string
std::string path = extractPath(raw_buffer);

// 之后：零拷贝，直接引用原始缓冲区
std::string_view path = extractPath(raw_buffer);
```

Heaptrack 中的表现：`std::__new_allocator<char>::allocate` 消失。

### 7.2 `vector` 预分配 reserve

```cpp
// 之前：push_back 触发多次扩容（每次 realloc + memcpy）
std::vector<Header> headers;
for (...) headers.push_back(h);

// 之后：一次分配到位
std::vector<Header> headers;
headers.reserve(20);  // HTTP 请求通常 10~20 个头部
for (...) headers.push_back(h);
```

### 7.3 栈缓冲区替代堆分配

```cpp
// 之前：每次响应都 new 一个缓冲区
std::string response_buf;
response_buf.reserve(4096);
serializeTo(response_buf);

// 之后：栈上固定缓冲区，零堆分配
char buf[4096];
size_t len = serializeTo(buf, sizeof(buf));
```

### 7.4 PMR 内存池（C++17）

对于无法用栈分配的场景（大小不确定、生命周期复杂）：

```cpp
#include <memory_resource>

// 请求级单调缓冲区：请求结束一次性释放，无 free 开销
char buffer[8192];
std::pmr::monotonic_buffer_resource pool{buffer, sizeof(buffer)};
std::pmr::vector<std::pmr::string> params{&pool};
// 请求处理完毕，pool 析构，所有内存一次性回收
```

PMR 三级策略（从我的框架中总结）：

| 层级      | 作用域   | 分配器类型                     | 目标               |
| --------- | -------- | ------------------------------ | ------------------ |
| L1 全局池 | 进程级   | `synchronized_pool_resource`   | 跨线程共享对象     |
| L2 线程池 | 线程级   | `unsynchronized_pool_resource` | 无锁线程局部分配   |
| L3 请求池 | 单请求级 | `monotonic_buffer_resource`    | 请求结束一次性释放 |

---

## 八、优化前后对比

```bash
# 优化前
heaptrack ./server_v1 → 压测 → Ctrl+C
heaptrack_print heaptrack.server_v1.*.zst | tail -10

# 优化后
heaptrack ./server_v2 → 压测 → Ctrl+C
heaptrack_print heaptrack.server_v2.*.zst | tail -10
```

我的实际数据对比：

| 指标                   | 优化前（无 PMR） | 优化后（PMR + string_view） | 变化       |
| ---------------------- | ---------------- | --------------------------- | ---------- |
| 分配频率               | 130000/s         | 17457/s                     | **-87%**   |
| 临时分配               | 98000/s          | 41/s                        | **-99.9%** |
| 峰值内存               | 12.8M            | 2.14M                       | **-83%**   |
| CPU 火焰图 malloc 占比 | 8.3%             | < 0.1%                      | 几乎消失   |

---

## 九、速查卡

```bash
# === 录制 ===
heaptrack ./your_server        # 启动时录制（推荐）
heaptrack --pid $PID           # 附着已有进程

# === 分析 ===
heaptrack_print $FILE | tail -10                                    # 全局摘要
heaptrack_print $FILE | sed -n '/MOST CALLS/,/PEAK MEMORY/p'       # 分配次数排名
heaptrack_print $FILE | sed -n '/MOST TEMPORARY/,/total runtime/p' # 临时分配排名
heaptrack_gui $FILE                                                 # GUI

# === 分配火焰图 ===
heaptrack_print --flamegraph-cost-type allocations $FILE > heap.folded
~/FlameGraph/flamegraph.pl --countname allocs --colors mem heap.folded > heap.svg
```

---

## 总结

| 问题               | 工具                             | 方法                  |
| ------------------ | -------------------------------- | --------------------- |
| 每秒分配了多少次？ | `heaptrack_print                 | tail -10`             | 看 calls/s |
| 哪个函数分配最多？ | `MOST CALLS` 段 或 GUI Bottom-Up | 完整调用栈            |
| 哪些分配是浪费的？ | `MOST TEMPORARY` 段              | 分配后立即释放 = 浪费 |
| 优化有没有效果？   | 前后两次 heaptrack 对比          | 临时分配降幅          |

记住优化优先级：**临时分配 > 高频分配 > 峰值内存 > 泄漏**。因为临时分配不仅浪费 CPU（malloc + free 开销），还会污染 CPU 缓存——这是下一篇的主题。

---

*上一篇：[《perf + 火焰图：5 分钟定位 C++ 程序的 CPU 瓶颈》](/posts/perf火焰图定位cpu瓶颈/)——从 CPU 层面定位热点函数。*

*下一篇：[《缓存行对 C++ 性能的影响有多大？实测告诉你》](/posts/缓存行对性能的影响/)——为什么 vector 比 map 快 10 倍？为什么两个线程写不同变量也会互相拖慢？*
