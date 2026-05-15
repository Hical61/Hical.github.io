+++
title = 'VM 编译运行 Hical Benchmark 流程：不走 Docker 的本地压测方案'
date = '2026-05-08'
draft = false
tags = ["Benchmark", "CMake", "wrk", "性能测试", "Hical", "Linux"]
categories = ["性能优化"]
description = "在 Ubuntu VM 上直接编译 Hical 源码并运行 bench_server + wrk 压测的完整流程，适用于代码改动后的快速 A/B 性能对比。"
+++

# VM 直接编译运行 Hical Benchmark 流程

> 不走 Docker，直接在 VM 上编译项目源码 + bench_server，用 wrk 压测。
> 适用于需要验证本地代码改动的场景（如性能优化后的 A/B 对比）。

## 前置条件

- VM 里已有 Hical 项目源码：`~/projects/Hical/`
- 已安装编译依赖（GCC 14+、CMake 3.20+、Boost 1.82+、OpenSSL）
- 挂接点 `/mnt/hical_host/` 对应宿主机 `d:/hical/Hical/`

## 一、同步宿主机代码改动到 VM

如果在宿主机上修改了源码，需要先拷贝到 VM 项目目录：

```bash
# 示例：拷贝 3 个改动文件
cp /mnt/hical_host/src/core/HttpServer.h \
   /mnt/hical_host/src/core/HttpServer.cpp \
   /mnt/hical_host/src/core/HttpSessionImpl.cpp \
   ~/projects/Hical/src/core/
```

或者整体同步 src 目录：

```bash
rsync -av /mnt/hical_host/src/ ~/projects/Hical/src/
```

## 二、编译

```bash
cd ~/projects/Hical

# 清理旧构建（可选，首次或 CMake 配置变更时执行）
rm -rf build

# 配置：Release + 编译 bench_server
cmake -B build -DCMAKE_BUILD_TYPE=Release -DHICAL_BUILD_BENCH=ON

# 编译（-j 自动取 CPU 核数）
cmake --build build -j$(nproc)
```

编译产物：`build/bench_server`（直接链接本地 `hical_core` 库，代码改动即生效）。

## 三、安装 wrk（仅首次）

```bash
sudo apt-get install -y wrk

# 验证
wrk --version
```

如果 apt 没有 wrk 包，手动编译：

```bash
sudo apt-get install -y build-essential libssl-dev git
git clone https://github.com/wg/wrk.git /tmp/wrk
cd /tmp/wrk && make -j$(nproc) && sudo cp wrk /usr/local/bin/
```

## 四、运行 bench_server

```bash
# 后台运行（监听 8080 端口，4 线程）
./build/bench_server &

# 验证服务正常
curl http://127.0.0.1:8080/
# 应返回: Hello, World!
```

### bench_server 端点一览

| 端点             | 方法 | 说明                   |
| ---------------- | ---- | ---------------------- |
| `/`              | GET  | Hello World            |
| `/api/status`    | GET  | JSON 响应              |
| `/api/echo`      | POST | JSON 反序列化 + 序列化 |
| `/users/{id}`    | GET  | 路径参数               |
| `/middleware/0`  | GET  | 无中间件基线           |
| `/middleware/3`  | GET  | 3 层异步中间件         |
| `/middleware/10` | GET  | 10 层异步中间件        |

## 五、压测

### 基础测试（Hello World）

```bash
# 与 Docker benchmark 参数一致：4 线程、100 并发、30 秒
wrk -t4 -c100 -d30s http://127.0.0.1:8080/

# 多跑 3 轮取平均，消除波动
wrk -t4 -c100 -d30s http://127.0.0.1:8080/
wrk -t4 -c100 -d30s http://127.0.0.1:8080/
```

### JSON 测试

```bash
wrk -t4 -c100 -d30s http://127.0.0.1:8080/api/status
```

### POST JSON Echo 测试

```bash
# 先创建 lua 脚本
cat > /tmp/post_echo.lua << 'EOF'
wrk.method = "POST"
wrk.body   = '{"name":"Hical","age":30,"email":"hical@example.com"}'
wrk.headers["Content-Type"] = "application/json"
EOF

wrk -t4 -c100 -d30s -s /tmp/post_echo.lua http://127.0.0.1:8080/api/echo
```

### 中间件链测试

```bash
wrk -t4 -c100 -d30s http://127.0.0.1:8080/middleware/0
wrk -t4 -c100 -d30s http://127.0.0.1:8080/middleware/3
wrk -t4 -c100 -d30s http://127.0.0.1:8080/middleware/10
```

### 高并发测试

```bash
wrk -t4 -c1000 -d30s http://127.0.0.1:8080/
wrk -t4 -c10000 -d30s http://127.0.0.1:8080/
```

## 六、Profiling（可选）

### 火焰图

#### 1. 安装依赖（仅首次）

```bash
# perf 工具（需要与当前内核版本匹配）
sudo apt-get install -y linux-tools-$(uname -r) linux-tools-generic

# 验证 perf 可用
perf --version

# FlameGraph 脚本集
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
```

#### 2. 确认内核参数（允许非 root perf）

```bash
# 查看当前值（默认通常为 4，限制较严）
cat /proc/sys/kernel/perf_event_paranoid

# 设为 -1 允许所有用户使用 perf（临时，重启失效）
sudo sysctl -w kernel.perf_event_paranoid=-1

# 允许读取内核符号（生成火焰图需要）
sudo sysctl -w kernel.kptr_restrict=0
```

#### 3. 编译带调试符号的 Release 版本

```bash
cd ~/projects/Hical

# RelWithDebInfo：保留 -O2 优化 + 带 -g 调试符号（火焰图能看到函数名）
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHICAL_BUILD_BENCH=ON
cmake --build build -j$(nproc)
```

> **注意**：纯 Release（`-O2 -DNDEBUG`）也能生成火焰图，但函数名可能被内联优化掉。
> `RelWithDebInfo` 是 perf profiling 的推荐模式。

#### 4. 启动 bench_server

```bash
./build/bench_server &
# $! 是 shell 内置变量，表示上一个后台进程的 PID。所以 SERVER_PID=$! 就是把刚才 ./build/bench_server & 启动的进程 PID 保存到变量 SERVER_PID 里，后面 perf record -p $SERVER_PID 就能直接引用，不用手动查 PID。
SERVER_PID=$!  

# 验证
curl -s http://127.0.0.1:8080/ && echo " OK"
```

#### 5. 开始录制 + 同时施压

需要两个终端（或用 `&` 后台化其中一个）：

```bash
# 终端 1：启动 perf 录制（采样 999 Hz，持续 30 秒，仅录制 bench_server 进程）
sudo perf record -g -F 999 -p $SERVER_PID -- sleep 30 &
PERF_PID=$!

# 终端 1（紧接着）：启动 wrk 压测（同样 30 秒，确保录制期间有负载）
wrk -t4 -c100 -d30s http://127.0.0.1:8080/

# 等待 perf record 结束
wait $PERF_PID
```

**参数说明**：
- `-g`：记录调用栈（生成火焰图必须）
- `-F 999`：每秒采样 999 次（避免与系统时钟 1000Hz 产生谐振锁步）
- `-p $SERVER_PID`：仅采样目标进程（不采样 wrk 和其他进程）
- `-- sleep 30`：录制 30 秒后自动停止

录制完成后生成 `perf.data` 文件（通常 50-200MB）。

#### 6. 生成火焰图 SVG

```bash
# 导出符号化的调用栈文本
sudo perf script > perf_out.txt

# 折叠调用栈 → 生成火焰图 SVG
~/FlameGraph/stackcollapse-perf.pl perf_out.txt | ~/FlameGraph/flamegraph.pl > flame.svg

# 查看文件大小确认生成成功
ls -lh flame.svg
```

#### 7. 查看火焰图

```bash
# 方案 A：拷贝到宿主机共享目录，在浏览器打开
cp flame.svg /mnt/hical_host/docker/flame.svg

# 方案 B：VM 有桌面环境时直接打开
firefox flame.svg &
# 或
xdg-open flame.svg
```

在浏览器中打开 SVG，可以：
- 点击任意函数块 **放大** 查看子调用
- 搜索框输入函数名 **高亮** 匹配项
- 宽度 = CPU 占比（越宽 = 越热）

#### 8. 火焰图分析要点

```
典型 Hical benchmark 火焰图热点分布：

├── 内核态 (50-60%)
│   ├── tcp_sendmsg / __ip_queue_xmit    — socket 发送（不可优化）
│   ├── epoll_ctl                         — 事件注册（SO_REUSEPORT 已优化）
│   └── schedule / wake_up_process        — 线程调度
│
├── Boost.Asio 调度 (20-30%)
│   ├── io_context::run_one              — 事件循环
│   ├── epoll_reactor::run               — reactor 分发
│   └── scheduler::do_run_one            — 协程恢复
│
└── Hical 用户态 (<5%)
    ├── phr_parse_request                — HTTP 解析
    ├── serializeHeadTo                  — 响应序列化
    ├── Router::dispatch                 — 路由查找
    └── handleSession                    — 会话循环
```

**关注点**：
- Hical 框架代码占比应 < 5%（否则有代码级瓶颈）
- `epoll_ctl` > 10% → 考虑减少 timer 操作（已用 atomic 时间戳优化）
- `wake_one_thread_and_unlock` > 10% → 跨线程调度过多（已用 SO_REUSEPORT 优化）
- `malloc/free` 出现在热路径 → PMR 池或预分配优化

#### 9. 清理

```bash
# 删除大文件
rm -f perf.data perf_out.txt

# 停止 bench_server
pkill bench_server
```

#### 10. 高级：差异火焰图（A/B 对比）

对比优化前后两次 profiling 的差异：

```bash
# 假设已有两次录制：perf_baseline.data 和 perf_optimized.data
sudo perf script -i perf_baseline.data > baseline.txt
sudo perf script -i perf_optimized.data > optimized.txt

~/FlameGraph/stackcollapse-perf.pl baseline.txt > baseline.folded
~/FlameGraph/stackcollapse-perf.pl optimized.txt > optimized.folded

# 生成差异火焰图（红色=回归，蓝色=优化）
~/FlameGraph/difffolded.pl baseline.folded optimized.folded \
    | ~/FlameGraph/flamegraph.pl > diff_flame.svg
```

### strace 统计系统调用频率

用于分析 `epoll_ctl`、`epoll_wait`、`sendmsg` 等系统调用的频率和耗时占比，
判断内核态瓶颈是否在事件注册（epoll_ctl）还是网络 I/O（sendmsg）。

#### 1. 安装 strace（仅首次）

```bash
sudo apt-get install -y strace

# 验证
strace --version
```

#### 2. 启动 bench_server 并确认 PID

```bash
./build/bench_server &
SERVER_PID=$!
echo "bench_server PID: $SERVER_PID"
```

#### 3. 开始 strace + 同时施压

需要两个终端，或者用后台化：

```bash
# 终端 1：strace 附着到 bench_server 所有线程，统计模式（-c 汇总，不打印每次调用）
# Ctrl+C 手动停止，停止后会打印统计表
sudo strace -c -f -p $SERVER_PID &
STRACE_PID=$!

# 终端 1（紧接着）：启动 wrk 压测 10 秒（strace 有开销，时间不宜太长）
wrk -t4 -c100 -d10s http://127.0.0.1:8080/

# 压测结束后停止 strace（发送 SIGINT）
sudo kill -INT $STRACE_PID
```

**参数说明**：
- `-c`：统计模式，输出每种系统调用的次数/耗时/错误占比
- `-f`：跟踪所有子线程（bench_server 是多线程的）
- `-p $SERVER_PID`：附着到已运行的进程

> **注意**：strace 有显著性能开销（~10-50x 减速），统计结果中的绝对 QPS 无意义，
> 只看各系统调用的**相对比例**。

#### 4. 解读输出

strace 停止后输出类似：

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 45.23    1.234567           2    617234           sendmsg
 22.11    0.603456           1    603456           epoll_wait
 15.67    0.427890           1    427890           recvmsg
  8.45    0.230678           1    230678           epoll_ctl
  5.12    0.139876           0    139876           clock_gettime
  ...
------ ----------- ----------- --------- --------- ----------------
100.00    2.730000                2019134           total
```

**关注指标**：

| 指标                           | 健康值               | 异常信号                             |
| ------------------------------ | -------------------- | ------------------------------------ |
| `epoll_ctl` 占比               | < 10%                | > 15% 说明 timer 频繁注册/取消       |
| `epoll_ctl` 调用次数 vs 请求数 | 接近 0（理想）或 1:1 | 2:1 或更高说明每请求做了多次事件修改 |
| `sendmsg` 占比                 | 最高（正常）         | 发送是主要工作                       |
| `futex` 出现                   | 不应出现             | 有锁竞争                             |
| `clock_gettime`                | 占比低               | > 10% 说明时间戳获取过于频繁         |

#### 5. 仅统计 epoll_ctl（精简模式）

如果只关心 epoll_ctl 是否被优化掉：

```bash
# 仅追踪 epoll_ctl，10 秒后自动停止
timeout 10 sudo strace -c -f -e epoll_ctl -p $SERVER_PID 2>&1 | tail -10
```

同时在另一个终端跑压测：

```bash
wrk -t4 -c100 -d10s http://127.0.0.1:8080/
```

#### 6. 打印每次 epoll_ctl 调用详情（调试用）

```bash
# 去掉 -c，打印每次调用的参数（仅短时间使用，输出量巨大）
sudo strace -f -e epoll_ctl -p $SERVER_PID 2>&1 | head -50
```

输出示例：

```
[pid 26283] epoll_ctl(3, EPOLL_CTL_ADD, 12, {events=EPOLLIN|EPOLLOUT, ...}) = 0
[pid 26283] epoll_ctl(3, EPOLL_CTL_MOD, 12, {events=EPOLLIN, ...}) = 0
```

- `EPOLL_CTL_ADD`：新连接注册
- `EPOLL_CTL_MOD`：事件修改（timer expires_after 或 读写切换）
- `EPOLL_CTL_DEL`：连接关闭

**优化目标**：keep-alive 请求期间不应出现 `EPOLL_CTL_MOD`（atomic 时间戳 timer 已消除 per-request 的 timer 注册/取消）。

#### 7. 清理

```bash
pkill bench_server
```

## 七、停止

```bash
# 前台运行时：Ctrl+C
# 后台运行时：
kill %1
# 或
pkill bench_server

# 查看是否杀掉 bench_server 进程
pidof bench_server
```



## 八、快速 A/B 对比模板

```bash
# ── 基线测试 ──
cd ~/projects/Hical
git stash  # 保存改动
cmake --build build -j$(nproc)
./build/bench_server &
wrk -t4 -c100 -d30s http://127.0.0.1:8080/  # 记录 QPS
pkill bench_server

# ── 优化版测试 ──
git stash pop  # 恢复改动
cmake --build build -j$(nproc)
./build/bench_server &
wrk -t4 -c100 -d30s http://127.0.0.1:8080/  # 对比 QPS
pkill bench_server
```
