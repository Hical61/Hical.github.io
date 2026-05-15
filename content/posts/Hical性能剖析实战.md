+++
title = 'Hical 性能剖析实战：perf + 火焰图定位 QPS 瓶颈'
date = '2026-05-09'
draft = false
tags = ["C++20", "性能剖析", "Hical", "perf", "火焰图", "Boost.Asio"]
categories = ["性能优化"]
description = "用 perf record + 火焰图精确定位 Hical 27K QPS 瓶颈：框架代码仅占 2% CPU，真正瓶颈在 Boost.Asio 调度层的 epoll_ctl 和跨线程唤醒。"
+++

# Hical 性能剖析实战：perf + 火焰图定位 QPS 瓶颈

> 在 [C++ 框架性能实测](C++Web框架性能实测.md)中，Hical 的 Hello World QPS（~27K）远低于 Cinatra（165K）和 Drogon（161K）。静态链接 + strip 验证后确认瓶颈不在链接方式。本文记录用 `perf record` + 火焰图精确定位 CPU 热点的全过程。

---

## 目录

- [1. 背景与动机](#1-背景与动机)
- [2. Profiling 环境搭建](#2-profiling-环境搭建)
- [3. 数据采集](#3-数据采集)
- [4. 火焰图分析](#4-火焰图分析)
- [5. 优化方向](#5-优化方向)
- [6. 复现指南](#6-复现指南)

---

## 1. 背景与动机

### 1.1 已排除的因素

在本次 profiling 之前，已经通过对照实验排除了以下因素：

| 假设                      | 验证方式                      | 结论                         |
| ------------------------- | ----------------------------- | ---------------------------- |
| 动态链接 Boost 有性能损耗 | 改为 Boost 静态链接，重跑压测 | QPS 无显著变化（27K → 27K）  |
| strip 影响性能            | strip vs 不 strip 对比        | 无影响（符号表不参与运行时） |
| 二进制体积（icache 压力） | 7.8M(strip) vs 9.3M(不strip)  | QPS 在噪声范围内，非瓶颈     |

**排除结论**：性能瓶颈在框架运行时架构，需要 profiling 定位具体热点函数。

### 1.2 关键性能差距

Hello World 场景（`GET /`，纯文本 "Hello, World!"，无 JSON 无中间件）：

| 框架      | QPS        | Avg Latency | 异步模型         |
| --------- | ---------- | ----------- | ---------------- |
| Cinatra   | 165,058    | 1.64ms      | C++20 协程       |
| Drogon    | 161,244    | 1.66ms      | Trantor 事件循环 |
| Crow      | 103,341    | 1.60ms      | Standalone Asio  |
| **Hical** | **27,493** | **4.86ms**  | Boost.Asio 协程  |
| Oat++     | 19,629     | 5.66ms      | 同步线程池       |

Hical 和 Cinatra 都使用 C++20 协程，但 QPS 相差 6 倍——问题不在协程本身，而在 Boost.Asio + Beast 的集成方式。

---

## 2. Profiling 环境搭建

### 2.1 Profiling 专用 Dockerfile

生产 Dockerfile 中 `strip` 去除了符号表，perf 无法解析函数名。需要创建一个保留符号的版本 `Dockerfile.profiling`：

```dockerfile
# 关键差异：-g 保留调试符号，不 strip
RUN cmake --preset conan-release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -g -DNDEBUG" \
    && cmake --build --preset conan-release -j$(nproc)
    # 注意：不执行 strip

# 运行阶段安装 perf 工具
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    linux-tools-generic linux-tools-common
```

| 对比项     | 生产 Dockerfile | Profiling Dockerfile  |
| ---------- | --------------- | --------------------- |
| 编译选项   | `-O2`           | `-O2 -g`（保留符号）  |
| strip      | 是              | 否                    |
| 二进制大小 | 7.8M            | 12M                   |
| perf 工具  | 无              | `linux-tools-generic` |

### 2.2 容器权限配置

perf 需要访问内核性能计数器，Docker 默认不允许。在 `docker-compose.yml` 中为 profiling 服务添加：

```yaml
hical-profiling:
  cap_add:
    - SYS_ADMIN       # 访问 perf_event_open
    - SYS_PTRACE       # 跟踪进程
  security_opt:
    - seccomp=unconfined  # 放宽系统调用限制
```

缺少这些权限会导致 `perf record` 报 `permission denied` 或只能采集用户态样本。

---

## 3. 数据采集

### 3.1 采集流程

```bash
# 后台启动 perf 录制（99Hz 采样，30 秒）
docker compose --profile profiling exec -d hical-profiling \
  perf record -g -F 99 -p 1 -o /profiling/perf.data -- sleep 30

# 同步用 wrk 压测（与 perf 同时运行 30 秒）
docker compose --profile profiling exec wrk \
  wrk -t4 -c100 -d30s http://hical-profiling:8080/

# 生成调用栈文本
docker compose --profile profiling exec hical-profiling \
  perf script -i /profiling/perf.data > profiling-output/perf.script

# 生成火焰图 SVG
stackcollapse-perf.pl profiling-output/perf.script | \
  flamegraph.pl --title "Hical Hello World" > profiling-output/flamegraph.svg
```

### 3.2 采集参数说明

| 参数     | 值          | 说明                                         |
| -------- | ----------- | -------------------------------------------- |
| `-F 99`  | 99Hz        | 每秒 99 次采样，足够精确且不影响性能         |
| `-g`     | —           | 记录完整调用栈（DWARF unwinding）            |
| `-p 1`   | PID 1       | 容器内 server 进程（Docker 中 PID 通常为 1） |
| wrk 参数 | 4t/100c/30s | 与 benchmark 保持一致                        |

---

## 4. 火焰图分析

### 4.1 热点函数 Top 10

| 排名 | 函数                                                | CPU 占比  | 类别                |
| ---- | --------------------------------------------------- | --------- | ------------------- |
| 1    | `sendmsg`（内核）                                   | **53.8%** | 内核态 socket 发送  |
| 2    | `epoll_ctl`                                         | **12.5%** | epoll 事件注册/修改 |
| 3    | `scheduler::wake_one_thread_and_unlock`             | **9.0%**  | Asio 调度器线程唤醒 |
| 4    | `pthread_cond_signal` + `post_immediate_completion` | **5.5%**  | 线程间条件变量通知  |
| 5    | `epoll_wait`                                        | 1.8%      | epoll 等待事件      |
| 6    | `reactive_socket_service_base::do_start_op`         | 1.2%      | 注册异步 I/O 操作   |
| 7    | `handleSession`                                     | 0.85%     | Hical 会话处理      |
| 8    | `malloc` / `cfree` / `operator new`                 | ~0.7%     | 堆内存分配          |
| 9    | `Router::dispatch`                                  | 0.24%     | Hical 路由分发      |
| 10   | `HttpResponse::ok`                                  | 0.10%     | Hical 响应构建      |

### 4.2 关键发现

**发现一：Hical 框架自身代码不是瓶颈**

`Router::dispatch`（0.24%）、`HttpResponse::ok`（0.10%）、`handleSession`（0.85%）——框架用户态逻辑总计不到 2%。优化框架代码对 QPS 几乎无影响。

**发现二：超过 80% 的 CPU 时间在 Boost.Asio 调度层 + 内核 I/O**

```
sendmsg          53.8%  ─── 内核态 socket 发送（不可优化）
epoll_ctl        12.5%  ─── 可优化：减少 epoll 修改频率
thread wakeup    14.5%  ─── 可优化：减少跨线程 post
epoll_wait        1.8%  ─── 正常等待，不可优化
Hical 框架代码    ~2%   ─── 不是瓶颈
其他             ~15%   ─── 分散在各种小函数中
```

**发现三：53.8% 的 sendmsg 是正常开销**

在 Hello World 场景中，处理逻辑极简（返回 13 字节字符串），CPU 时间自然被 I/O 主导。这个比例在所有高性能 HTTP 框架中都类似。差异在于 **其他 46.2% 的分配方式**——Drogon/Cinatra 在这 46.2% 中的 epoll_ctl 和线程唤醒开销更小。

**发现四：epoll_ctl 12.5% 是主要可优化项**

Boost.Asio 默认使用 level-triggered 模式，每次读/写操作前后都要调 `epoll_ctl` 修改事件集。Drogon 的 Trantor 库使用更高效的 epoll 策略，减少了这部分开销。

**发现五：线程唤醒 14.5% 说明存在跨线程调度**

`wake_one_thread_and_unlock` + `pthread_cond_signal` 表明协程完成后通过条件变量唤醒其他 IO 线程。理想情况下，一个请求应该在同一个 IO 线程内完成全部处理，无需跨线程通知。

---

## 5. 优化方向

根据火焰图数据，按预期收益排序：

### 5.1 减少 epoll_ctl 调用（预期收益：12.5% CPU）

**现状**：Boost.Asio 的 `epoll_reactor` 在每次 async 操作时调用 `epoll_ctl(EPOLL_CTL_MOD)`。

**优化方向**：
- 探索 `EPOLLONESHOT` + edge-triggered 模式
- 或在连接生命周期内只注册一次 `EPOLLIN | EPOLLOUT`，不频繁修改
- 参考 Drogon/Trantor 的 epoll 策略

### 5.2 减少跨线程 post（预期收益：14.5% CPU）

**现状**：`post_immediate_completion` 和 `wake_one_thread_and_unlock` 表明请求处理涉及跨线程调度。

**优化方向**：
- 确保请求在 accept 所在的 IO 线程内完成（thread affinity）
- 减少 `io_context::post()` 调用，改用 `dispatch()`（如果已在正确线程则直接执行）
- EventLoopPool 的 round-robin 策略可能导致 handler 在非连接所属线程执行

### 5.3 PMR 内存池优化（预期收益：<1% CPU，但影响延迟稳定性）

**现状**：`monotonic_buffer_resource::_M_new_buffer` 和 `~monotonic_buffer_resource` 出现在采样中，说明请求级 PMR 池有分配/回收开销。

**优化方向**：
- 预分配更大的初始 buffer 减少 `_M_new_buffer` 调用
- 考虑 request 级 buffer 复用（池化 monotonic_buffer_resource 对象）

### 5.4 长期架构方向

| 方案                                    | 投入 | 收益             | 风险            |
| --------------------------------------- | ---- | ---------------- | --------------- |
| 自定义 epoll reactor 替换 Asio 默认实现 | 极大 | 可能 +50% QPS    | 偏离 Boost 生态 |
| 实现 io_uring 后端（Linux 5.6+）        | 大   | 减少系统调用开销 | 平台限制        |
| 单线程 event loop 模式（类 Drogon）     | 中   | 消除线程唤醒开销 | 改变并发模型    |

---

> **数据说明**：本文数据采集于 2026-05-09，Linux VM (Hyper-V)，Docker 容器（4CPU/512MB），Hical v2.5.1，perf 6.8.12，wrk 4t/100c/30s。

> **利益声明**：本文作者是 Hical 框架的开发者。profiling 数据如实呈现，包括对 Hical 不利的结论。

---

> 系列文章：[设计理念](01-design-philosophy.md) · [协程与内存](02-coroutine-and-memory.md) · [路由中间件](03-router-middleware-ssl.md) · [框架横评](07-framework-comparison.md) · [C++ vs Go vs Rust](11-cpp-vs-go-rust-web.md) · [框架性能实测](21-cpp-framework-benchmark.md) · **性能剖析实战**
