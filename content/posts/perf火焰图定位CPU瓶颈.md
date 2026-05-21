+++
title = 'perf + 火焰图：5 分钟定位 C++ 程序的 CPU 瓶颈'
date = '2026-05-15'
draft = false
tags = ["C++", "性能分析", "perf", "火焰图", "Linux", "Hical"]
categories = ["性能优化"]
description = "从 perf stat 判断瓶颈类型到 perf record 生成火焰图，再到 perf annotate 逐行定位热点——完整分享 Hical 框架从 27K 到 136K QPS 的 CPU 性能分析流程。"
+++

# perf + 火焰图：5 分钟定位 C++ 程序的 CPU 瓶颈

> 你的服务器 CPU 跑满了，QPS 却上不去。top 告诉你"忙"，但不告诉你忙在哪。怎么办？

---

## 故事：从 27K 到 136K QPS

我开发了一个 C++20/26 Web 框架（Hical），第一次压测只有 27K QPS，而同场景下 Drogon 和 Cinatra 都在 160K+。CPU 使用率 100%，top 没用，gdb 打断点太慢。

最终靠 `perf record` + 火焰图，**5 分钟定位到瓶颈不在我的框架代码（仅占 2% CPU），而在 Boost.Asio 的调度层**——跨线程 `epoll_ctl` 和 per-request timer 合计吃了 27% CPU。

优化后 QPS 从 27K → 136K。

这篇文章把我整套分析流程分享出来。不需要你用过 Hical，**任何 C++ 服务器程序都适用**。

---

## 一、工具安装（2 分钟搞定）

```bash
# perf（必须匹配内核版本）
sudo apt install -y linux-tools-$(uname -r) linux-tools-generic

# FlameGraph 脚本（Brendan Gregg 出品）
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git ~/FlameGraph

# 放开 perf 权限（否则只能看到自己的进程）
sudo sysctl -w kernel.perf_event_paranoid=-1
sudo sysctl -w kernel.kptr_restrict=0
```

验证：

```bash
perf --version   # 应输出 perf version 6.x
```

> 如果要永久生效，写入 `/etc/sysctl.d/99-perf.conf`。

---

## 二、perf stat：先判断瓶颈类型

火焰图之前，先用 `perf stat` 花 30 秒判断程序是 **CPU-bound** 还是 **Memory-bound**。方向错了，后面全白做。

### 2.1 基本用法

```bash
# 启动你的服务器
./your_server &
SERVER_PID=$!

# 采集 30 秒（同时在另一个终端施压）
perf stat -e cycles,instructions,cache-references,cache-misses,\
branches,branch-misses,task-clock,context-switches \
    -p $SERVER_PID -- sleep 30
```

### 2.2 输出解读

```
 Performance counter stats for process id '12345':

       30,127.45 msec  task-clock                #    7.896 CPUs utilized
          15,234       context-switches           #  505.6 /sec
  98,765,432,100       cycles                     #    3.28 GHz
  78,912,345,600       instructions               #    0.80  insn per cycle
     345,678,000       branch-misses              #    2.80% of all branches
     234,567,000       cache-misses               #    4.13% of all cache refs
```

### 2.3 核心指标：IPC

```
IPC = instructions / cycles
```

这是**最重要的单一指标**——现代 CPU 一个周期能发射 4~6 条指令，如果 IPC 远低于理论值，说明 CPU 大量时间在"等待"。

| IPC 范围  | 含义                  | 优化方向                       |
| --------- | --------------------- | ------------------------------ |
| > 2.0     | CPU 执行效率极高      | 想提速只能减指令数（算法优化） |
| 1.0 ~ 2.0 | 正常                  | 大多数优化过的程序             |
| 0.5 ~ 1.0 | 有停顿                | 可能是缓存或分支预测问题       |
| < 0.5     | **严重 Memory-bound** | 优化数据布局，减少 cache miss  |

### 2.4 快速判断流程

```
                   IPC 值
                  /      \
             > 1.0        < 0.5
          CPU-bound    Memory-bound
          优化算法     优化数据布局
               \          /
             看 cache-misses
            /              \
        < 5%            > 10%
       正常          数据局部性差
                    重排数据结构
```

**补充判断**：
- `context-switches > 10000/sec` → 锁竞争严重
- `task-clock # < 1.0 CPUs` → I/O-bound 或锁等待
- `branch-misses > 5%` → 分支预测频繁失败，考虑重排条件判断

> **结论**：`perf stat` 不告诉你"哪个函数慢"，但告诉你"应该往哪个方向优化"。确认方向后，上火焰图。

---

## 三、perf record + 火焰图：定位具体函数

### 3.1 编译要求

火焰图需要调试符号才能显示函数名。推荐 `RelWithDebInfo`（保留优化 + 有完整符号）：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

| 构建类型           | 编译选项       | 火焰图效果     | 性能代表性                |
| ------------------ | -------------- | -------------- | ------------------------- |
| Debug              | `-O0 -g`       | 所有函数可见   | 差（与 Release 差距巨大） |
| **RelWithDebInfo** | **`-O2 -g`**   | **大部分可见** | **好（推荐）**            |
| Release            | `-O2 -DNDEBUG` | 很多被内联     | 最好（但看不清细节）      |

### 3.2 录制（30 秒）

```bash
# 启动服务
./your_server &
SERVER_PID=$!

# 开始录制：999Hz 采样，记录调用栈
sudo perf record -g -F 999 -p $SERVER_PID -o perf.data -- sleep 30 &

# 同时施压
wrk -t4 -c100 -d30s http://127.0.0.1:8080/

# 等待录制结束
wait
```

**参数含义**：

| 参数          | 含义             | 为什么                     |
| ------------- | ---------------- | -------------------------- |
| `-g`          | 记录完整调用栈   | 火焰图必须                 |
| `-F 999`      | 每秒采样 999 次  | 避免与 1000Hz 内核时钟谐振 |
| `-p $PID`     | 仅采样目标进程   | 排除 wrk 和系统噪声        |
| `-- sleep 30` | 录制 30 秒后停止 | 与压测时长匹配             |

### 3.3 生成火焰图（3 条命令）

```bash
# 导出调用栈文本
sudo perf script -i perf.data > perf_out.txt

# 折叠调用栈
~/FlameGraph/stackcollapse-perf.pl perf_out.txt > perf.folded

# 生成 SVG
~/FlameGraph/flamegraph.pl --title "My Server Profile" perf.folded > flame.svg
```

用浏览器打开 `flame.svg`——这是一个**可交互**的 SVG。

### 3.4 火焰图读图指南

```
┌────────────────── 火焰图布局 ──────────────────────┐
│                                                      │
│  顶部 → 叶子函数（真正消耗 CPU 的地方）← 重点看   │
│          │                                           │
│          │  越宽 = 占 CPU 时间越多                   │
│          │                                           │
│  底部 → 入口函数（main）                            │
│                                                      │
│  X 轴：宽度 = 采样占比（不是时间顺序！）            │
│  Y 轴：调用栈深度                                    │
│  颜色：随机的，不代表任何含义                        │
└──────────────────────────────────────────────────────┘
```

**最关键的一点**：底部的块宽不代表它自身慢——它宽是因为它调用了很多子函数。**真正消耗 CPU 的是顶部那些最宽的叶子函数**。

### 3.5 交互操作

| 操作                | 效果                                             |
| ------------------- | ------------------------------------------------ |
| 鼠标悬停            | 底部显示函数名和占比（如 `handleSession 12.3%`） |
| 点击函数块          | 以该函数为 100% 放大                             |
| 左上角 "Reset Zoom" | 回到全局视图                                     |
| 右上角 "Search"     | 搜索函数名（支持正则）                           |

### 3.6 搜索定位热点（最实用的技巧）

按 `Search` 键打开搜索，输入关键词——匹配的块变紫红色，右下角显示占比：

| 搜索什么                     | 目的              | 健康值 | 需关注 |
| ---------------------------- | ----------------- | ------ | ------ |
| 你的项目名                   | 框架/应用自身开销 | < 5%   | > 10%  |
| `malloc\|free\|operator new` | 内存分配压力      | < 2%   | > 5%   |
| `memcpy\|memmove`            | 数据拷贝开销      | < 3%   | > 5%   |
| `mutex\|futex`               | 锁竞争            | < 1%   | > 5%   |
| `epoll_wait`                 | I/O 等待          | 5~30%  | > 50%  |

**实例**：我搜索 `hical` 发现框架代码只占 2%；搜索 `epoll_ctl` 发现占 12.5%——**瓶颈不在我的代码，而在 Asio 的 timer 调度**。

### 3.7 perf annotate：逐行热点

火焰图定位到函数级别后，用 `perf annotate` 看函数内部哪一行最热：

```bash
sudo perf annotate -i perf.data -s your_hot_function --stdio
```

输出（左边数字是该行采样占比）：

```
       │     for (;;) {
 12.34 │         ch = *buf;
       │         if (ch == '\r') {
  8.76 │             ++buf;
       │         }
 23.45 │         *token_start++ = ch;    ← 热点行
       │     }
```

---

## 四、差异火焰图：A/B 对比优化效果

优化前后各录一份，生成差异火焰图：

```bash
# 优化前
sudo perf script -i perf_before.data > before.txt
~/FlameGraph/stackcollapse-perf.pl before.txt > before.folded

# 优化后
sudo perf script -i perf_after.data > after.txt
~/FlameGraph/stackcollapse-perf.pl after.txt > after.folded

# 生成差异图
~/FlameGraph/difffolded.pl before.folded after.folded \
    | ~/FlameGraph/flamegraph.pl --title "Before vs After" > diff.svg
```

颜色含义：
- **红色** → 该函数占比**增加**了（回归）
- **蓝色** → 该函数占比**减少**了（优化成功）

---

## 五、实战案例：Hical 框架的分析结果

用上面的方法分析我的 C++20/26 Web 框架，得到以下结论。

> **注意**：以下火焰图数据采集于优化**之后**。早期版本中 per-request timer 导致每次请求都触发 `epoll_ctl` 注册/注销，占约 12% CPU；改用连接级 atomic 时间戳替代后，该热点已完全消除，不再出现在火焰图中。

```
bench_server (100%)
├── 内核态 TCP/IP 协议栈 (~88%)
│   ├── sendto → tcp_sendmsg → ip_queue_xmit (66%)
│   └── recvfrom → tcp_recvmsg (6.5%)
├── Boost.Asio 事件循环 (~10%)
│   ├── epoll_reactor::run (10%)
│   └── scheduler::do_run_one (0.4%)
└── Hical 用户态 (~2%)
    ├── handleSession (1.27%)
    ├── HeaderMap::set (0.60%)
    ├── Router::resolveRoute (0.23%)
    └── phr_parse_request (0.02%)  ← picohttpparser 极快
```

**关键发现**：
1. 框架代码仅占 2% CPU — 效率已经很高
2. 88% 时间在内核态 — Hello World 场景下正常（真实业务场景会变化）
3. 没有 `malloc`/`free` 出现在热点 — PMR 内存池生效
4. `epoll_ctl` 已从热点中消失 — 早期版本中 per-request timer 导致其占约 12% CPU，改用连接级 atomic 时间戳后彻底消除，这是 27K → 136K QPS 的主要优化点

---

## 六、速查卡

```bash
# === perf stat（判断瓶颈类型）===
perf stat -e cycles,instructions,cache-misses,context-switches \
    -p $PID -- sleep 30

# === perf record + 火焰图（定位热点函数）===
sudo perf record -g -F 999 -p $PID -o perf.data -- sleep 30
sudo perf script -i perf.data > out.txt
~/FlameGraph/stackcollapse-perf.pl out.txt > out.folded
~/FlameGraph/flamegraph.pl out.folded > flame.svg

# === perf annotate（逐行热点）===
sudo perf annotate -i perf.data -s <函数名> --stdio

# === 差异火焰图 ===
~/FlameGraph/difffolded.pl before.folded after.folded \
    | ~/FlameGraph/flamegraph.pl > diff.svg
```

---

## 总结

| 步骤 | 工具                     | 回答的问题                    | 耗时   |
| ---- | ------------------------ | ----------------------------- | ------ |
| 1    | `perf stat`              | CPU-bound 还是 Memory-bound？ | 30 秒  |
| 2    | `perf record` + 火焰图   | 哪个函数最热？                | 2 分钟 |
| 3    | 搜索 malloc/memcpy/futex | 有没有分配/拷贝/锁竞争？      | 10 秒  |
| 4    | `perf annotate`          | 热点函数的哪一行？            | 1 分钟 |
| 5    | 差异火焰图               | 优化有没有效果？              | 2 分钟 |

**全流程 5 分钟**，从"CPU 满载不知道为什么"到"精确定位到第 X 行代码"。

---

*下一篇：[《Heaptrack：找出 C++ 程序中的无效内存分配》](/posts/heaptrack找出无效内存分配/)——当火焰图告诉你 malloc 占比过高时，怎么找到那些无效的分配并消灭它们。*
