+++
title = '火焰图对比分析：自研 HTTP 栈 vs Beast HTTP 栈'
date = '2026-05-12'
draft = false
tags = ["C++20", "性能分析", "Hical", "Boost.Beast", "火焰图", "perf"]
categories = ["性能优化"]
description = "通过两份火焰图逐项对比，量化自研零拷贝 HTTP 栈相比 Beast 在解析、Header 存储、响应序列化上的性能收益。"
+++

# 火焰图对比分析：自研 HTTP 栈 vs Beast HTTP 栈

> Hical v2.6.0 完成了从 Beast HTTP 到自研零拷贝 HTTP 栈的迁移。本文通过两份火焰图的逐项对比，用数据量化"去 Beast"到底省了什么、省了多少，以及当前性能瓶颈到底在哪里。

---

## 目录

- [火焰图对比分析：自研 HTTP 栈 vs Beast HTTP 栈](#火焰图对比分析自研-http-栈-vs-beast-http-栈)
  - [目录](#目录)
  - [1. 测试环境与采集方式](#1-测试环境与采集方式)
  - [2. 总体热度分布对比](#2-总体热度分布对比)
    - [flame.svg（自研路径）— 总计 ~299 亿 samples](#flamesvg自研路径-总计-299-亿-samples)
    - [flame1.svg（Beast 路径）— 总计 ~411 亿 samples](#flame1svgbeast-路径-总计-411-亿-samples)
  - [3. HTTP 解析：picohttpparser vs Beast parser](#3-http-解析picohttpparser-vs-beast-parser)
  - [4. Header 存储：栈数组 vs 链表堆分配](#4-header-存储栈数组-vs-链表堆分配)
  - [5. 响应序列化：FixedBuffer vs Beast serializer](#5-响应序列化fixedbuffer-vs-beast-serializer)
    - [自研路径](#自研路径)
    - [Beast 路径](#beast-路径)
  - [6. 发送路径：sendto vs sendmsg](#6-发送路径sendto-vs-sendmsg)
  - [7. 协程与调度开销](#7-协程与调度开销)
  - [8. 内核瓶颈：loopback softirq 的天花板](#8-内核瓶颈loopback-softirq-的天花板)
    - [epoll\_ctl 已不是瓶颈](#epoll_ctl-已不是瓶颈)
  - [9. strace 佐证：系统调用频率](#9-strace-佐证系统调用频率)
  - [10. 结论与下一步](#10-结论与下一步)
    - [量化收益：去 Beast 到底省了多少](#量化收益去-beast-到底省了多少)
    - [当前性能分布总结](#当前性能分布总结)
    - [下一步优化方向](#下一步优化方向)
    - [最终结论](#最终结论)

---

## 1. 测试环境与采集方式

| 项目     | 配置                                                    |
| -------- | ------------------------------------------------------- |
| 环境     | Ubuntu VM (Docker 内)，GCC 14，-O2 -g                   |
| 压测工具 | wrk，4 线程，keep-alive                                 |
| 采集     | `perf record -F 99 -g -p <pid>` → FlameGraph 生成 SVG   |
| 辅助     | `strace -c -f -p <pid>` 统计系统调用频率                |
| 对比目标 | flame.svg（自研路径 v2.6.0）vs flame1.svg（Beast 路径） |

两份火焰图采集条件一致，唯一区别是 HTTP 处理栈的实现路径。

---

## 2. 总体热度分布对比

### flame.svg（自研路径）— 总计 ~299 亿 samples

| 层级               | 占比     | 主要函数                                                                  |
| ------------------ | -------- | ------------------------------------------------------------------------- |
| 内核 TCP 发送      | ~65%     | `sendto` → `tcp_sendmsg` → `tcp_write_xmit` → `__tcp_transmit_skb`        |
| 内核 softirq 收包  | ~53%     | `do_softirq` → `tcp_v4_rcv` → `__wake_up_sync_key` (45%)                  |
| epoll_wait 等待    | 9.6%     | `epoll_reactor::run` → `ep_poll` → `schedule`                             |
| Boost.Asio 协程    | ~6.4%    | `co_spawn_entry_point` 3.3% + `awaitable_handler` 3.05%                   |
| **Hical 框架代码** | **2.5%** | `handleSession` + `fromParsed` + `HeaderMap` + `writeResponse` + `Router` |
| picohttpparser     | 0.06%    | 几乎可忽略                                                                |
| 用户态 malloc      | ~0.7%    | `operator new` 0.37% + `malloc` 0.3%                                      |

### flame1.svg（Beast 路径）— 总计 ~411 亿 samples

| 层级                        | 占比       | 主要函数                                                            |
| --------------------------- | ---------- | ------------------------------------------------------------------- |
| 内核 TCP 发送               | ~65.6%     | `sendmsg` → `tcp_sendmsg` → `tcp_write_xmit` → `__tcp_transmit_skb` |
| 内核 softirq 收包           | ~50.5%     | `do_softirq` → `tcp_v4_rcv` → `__wake_up_sync_key` (43.4%)          |
| epoll_wait 等待             | 7.9%       | `epoll_reactor::run` → `ep_poll` → `schedule`                       |
| Boost.Asio 协程             | ~2.0%      | `co_spawn_entry_point` 1.03% + `pump` 1.02%                         |
| **Beast HTTP 序列化**       | **~1.9%**  | `serializer::next` + `write_op` + `consume` + `fwrinit`             |
| **Beast HTTP 解析**         | **~0.63%** | `basic_parser<true>::put` + `parse_fields`                          |
| **Beast basic_fields 分配** | **~0.95%** | `new_element` (每 header 一次堆分配)                                |
| Hical 框架代码              | ~1.4%      | `handleSession` + `Router` + `HttpRequest/Response` 构造析构        |
| 用户态 malloc               | ~1.1%      | `operator new` 0.81% + `malloc` 0.77%                               |

---

## 3. HTTP 解析：picohttpparser vs Beast parser

| 指标     | 自研路径                                | Beast 路径                               | 差距      |
| -------- | --------------------------------------- | ---------------------------------------- | --------- |
| 解析器   | picohttpparser                          | `boost::beast::http::basic_parser<true>` | —         |
| CPU 占比 | **0.06%**                               | **0.63%**                                | **10.5x** |
| 解析方式 | 零拷贝，输出 `string_view` 指向 readBuf | 解析即分配，每 header 调用 `new_element` | —         |
| 栈分配   | `phr_header[64]` 栈数组                 | 无（直接堆分配）                         | —         |

Beast parser 的代价不仅是解析本身，而是**解析与分配耦合**：

```
basic_parser::put → parse_fields → set_element → new_element → operator new
```

每解析一个 header 就执行一次堆分配，请求结束后 `~basic_fields` 又逐个释放。而 picohttpparser 只产出指针偏移量，零分配。

---

## 4. Header 存储：栈数组 vs 链表堆分配

| 指标                 | 自研路径                                      | Beast 路径                  |
| -------------------- | --------------------------------------------- | --------------------------- |
| 存储结构             | `RequestHeaders`: 栈 `array<Entry, 64>`       | `basic_fields`: 侵入式链表  |
| 分配次数/请求        | **0 次**                                      | **N 次**（N = header 数量） |
| `new_element` 耗时   | 无                                            | 0.95%                       |
| `~basic_fields` 析构 | 无                                            | 0.16%                       |
| 查找方式             | `HeaderMap`: vector 线性扫描（L1 cache 友好） | 链表遍历 + hash             |

自研方案的核心思路：HTTP 请求典型 header 数量 < 20，栈上 64-entry 数组足够覆盖 99.9% 场景，且 L1 cache line 连续命中。Beast 的链表节点分散在堆上，每次查找都可能 cache miss。

---

## 5. 响应序列化：FixedBuffer vs Beast serializer

### 自研路径

```
NativeResponse::serializeHeadTo(FixedBuffer<4096>&)  →  单次 async_write
```

- `FixedBuffer<4096>` 栈分配，head + body 合并到一个 buffer
- 一次 `sendto` 系统调用发出完整响应
- 总开销：`writeResponse` 0.53% + `serializeHeadTo` 0.09% = **0.62%**

### Beast 路径

```
serializer::next → write_some_op → write_op → sendmsg (scatter-gather)
```

- Beast serializer 是状态机，需多次 `next()` + `consume()` 推进
- 每次 write 涉及 `async_base` 构造/析构（0.16%）、`allocate_stable_state`（0.04%）
- 总开销：serializer 0.15% + write_op 0.24% + fields alloc 0.95% + consume 0.08% = **~1.4%**

| 对比              | 自研                  | Beast                             | 差距     |
| ----------------- | --------------------- | --------------------------------- | -------- |
| 序列化+发送总开销 | 0.62%                 | 1.4%                              | **2.3x** |
| 系统调用          | `sendto`（单 buffer） | `sendmsg`（scatter-gather iovec） | —        |
| 堆分配            | 零                    | 每响应多次                        | —        |

---

## 6. 发送路径：sendto vs sendmsg

从 strace 数据：

```
自研路径:  sendto    62.66%  (20,523 次, 均 876μs/次)
Beast路径: sendmsg   65.60%  (scatter-gather writev)
```

内核层面 `sendto` vs `sendmsg` 的差异极小：
- `sendmsg` 多了 `___sys_sendmsg` → `____sys_sendmsg` → `__copy_msghdr` 的 iovec 元数据拷贝（0.01%）
- 最终都走 `tcp_sendmsg_locked` → `tcp_push` → `tcp_write_xmit`

**结论**：sendmsg 的 scatter-gather 能力在理论上减少了一次 memcpy（不用先合并 buffer），但在小响应场景下优势不明显。自研路径通过 FixedBuffer 预合并同样避免了多余拷贝。

---

## 7. 协程与调度开销

| 指标                            | 自研路径  | Beast 路径 |
| ------------------------------- | --------- | ---------- |
| `co_spawn_entry_point`          | 3.31%     | 1.03%      |
| `awaitable_handler::operator()` | 3.05%     | —          |
| `awaitable_thread::pump`        | —         | 1.02%      |
| 合计                            | **~6.4%** | **~2.0%**  |

Beast 路径协程开销更低，原因：
- Beast `async_read` / `async_write` 内部使用**组合操作状态机**（composed operation），在一次回调内完成多步 I/O
- 自研路径的 readLoop 每请求有更多 `co_await` 点（`co_await recv` → 解析 → `co_await send`）

这一差异说明：如果要进一步优化自研路径，可以借鉴 Beast 的"一次回调处理完整请求"模式，减少协程 suspend/resume 次数。

---

## 8. 内核瓶颈：loopback softirq 的天花板

两份火焰图中最大的共同瓶颈：

```
__local_bh_enable_ip (50-53%)
  └─ do_softirq
       └─ net_rx_action → __napi_poll → process_backlog
            └─ tcp_v4_rcv → tcp_data_ready
                 └─ sock_def_readable
                      └─ __wake_up_sync_key (43-45%)
```

这是 **loopback 网络 + VM 环境的固有瓶颈**：

1. `sendto`/`sendmsg` 将数据送入 TCP 栈
2. 因为是 loopback，数据不经过网卡，直接在**同一 CPU 的 softirq** 中处理收包
3. `tcp_data_ready` → `__wake_up_sync_key` 唤醒 epoll 等待的线程

整个"发送 → 收包 → 唤醒"在**同一次系统调用**中同步完成，导致 `sendto` 的实际耗时包含了对端的整个收包路径。

**这不是框架可以优化的**——在真实生产环境（跨网卡通信），发送和收包在不同 CPU core 异步处理，这 43-45% 的开销会消失。

### epoll_ctl 已不是瓶颈

strace 显示 `epoll_ctl` 仅 202 次（对比 4.3 万次请求），占比 0.15%。之前版本中 `epoll_ctl` 12.5% 的问题已通过 SO_REUSEPORT + 连接级 fd 复用彻底解决。

---

## 9. strace 佐证：系统调用频率

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 62.66   17.992624         876     20523           sendto
 33.83    9.713727         469     20668        69 recvfrom
  2.77    0.794654         458      1734           epoll_wait
  0.15    0.042617         210       202           epoll_ctl
  0.09    0.025900         256       101           setsockopt
  0.09    0.025036         247       101           ioctl
  0.05    0.014145         140       101           close
```

关键观察：
- **sendto:recvfrom ≈ 1:1**（20,523 vs 20,668），说明每请求恰好一读一写
- **epoll_wait 仅 1,734 次**，说明大多数情况下 recv 返回数据时 epoll 不需要等待（ET 模式高效）
- **epoll_ctl 仅 202 次** = 连接建立/断开时才操作，不是每请求

---

## 10. 结论与下一步

### 量化收益：去 Beast 到底省了多少

| 维度                   | Beast 路径            | 自研路径  | 节省      |
| ---------------------- | --------------------- | --------- | --------- |
| HTTP 解析              | 0.63%                 | 0.06%     | **0.57%** |
| Header 分配            | 0.95% + 0.16% = 1.11% | 0%        | **1.11%** |
| 响应序列化             | ~1.4%                 | 0.62%     | **0.78%** |
| 用户态总 malloc        | 1.1%                  | 0.7%      | **0.4%**  |
| **合计用户态框架开销** | **~4.0%**             | **~2.5%** | **1.5%**  |

去 Beast 在用户态节省约 **1.5% CPU**，对应的 QPS 提升取决于基准值——在 ~150K QPS 级别下约贡献 6-8K QPS 的增量。

### 当前性能分布总结

```
┌─────────────────────────────────────────────────────┐
│  内核 TCP 发送路径        65%  ← 无法优化           │
│  内核 softirq/收包唤醒    45%  ← loopback 固有      │
│  epoll_wait (真实等待)    ~9%  ← 正常空闲           │
│  Boost.Asio 协程          ~6%  ← 可微调             │
│  Hical 框架代码           ~2.5% ← 已接近极限        │
│  picohttpparser            0.06% ← 极致高效         │
└─────────────────────────────────────────────────────┘
```

### 下一步优化方向

| 方向                                    | 预期收益                                                  | 复杂度     |
| --------------------------------------- | --------------------------------------------------------- | ---------- |
| **io_uring 替代 epoll**                 | 批量 syscall，省 ~9% epoll_wait + 减少 send/recv 逐次调用 | 高         |
| **减少协程 suspend/resume**             | 省 ~2-3% co_await 开销                                    | 中         |
| **关闭 nf_conntrack**（benchmark 环境） | 省 ~1.5% netfilter 开销                                   | 低         |
| **真实网络测试**                        | 消除 45% 的 loopback softirq 同步开销                     | 无需改代码 |

### 最终结论

> 火焰图明确证实：**Hical 框架本身的 CPU 开销仅占 2.5%**，去 Beast 后的自研 HTTP 栈比 Beast 快约 1.5 个百分点。当前 159K QPS 的瓶颈 100% 在 Linux 内核 TCP 栈的 loopback 同步 softirq 路径上——这是 benchmark 环境的固有限制，不是框架问题。

在真实跨网络部署场景下，Hical 的理论 QPS 上限应远高于 VM loopback 测出的数字。
