+++
title = '深入学习 io_uring（一）：从原理到第一个异步程序'
date = '2025-10-01'
draft = false
tags = ["Linux", "io_uring", "C++", "异步IO", "高性能", "系统编程"]
categories = ["io_uring学习"]
description = "从 Linux I/O 演进出发，深入理解 io_uring 的共享环形缓冲区架构、零系统调用提交机制，配合 liburing 完成第一个异步文件读写程序。"
+++

> **系列导航**：**入门篇** | [进阶篇]({{< relref "posts/深入学习io_uring_进阶篇.md" >}}) | [实战篇]({{< relref "posts/深入学习io_uring_实战篇.md" >}})

## 引言：epoll 之后，还能更快吗？

假设你用 epoll 写了一个高并发 TCP 服务器，性能已经不错——C10K 问题解决了。但当你把连接数推到 **C1M（百万级）** 时：

```
epoll 的瓶颈：

  用户态                       内核态
   │                            │
   │ epoll_wait()               │
   ├───────系统调用─────────────→│  ← 每次至少一次上下文切换
   │                            │
   │ 返回就绪的 fd 列表          │
   │←───────────────────────────┤
   │                            │
   │ recv(fd_1, buf, ...)       │
   ├───────系统调用─────────────→│  ← 每个 fd 又一次系统调用！
   │                            │
   │ send(fd_1, resp, ...)      │
   ├───────系统调用─────────────→│  ← 再一次！
   │                            │
   │ recv(fd_2, buf, ...)       │
   ├───────系统调用─────────────→│  ← N 个连接 = 2N+ 次系统调用
```

**问题**：epoll 只解决了"哪些 fd 就绪"的问题，每次 I/O 操作仍然需要**独立的系统调用**。百万连接下，系统调用的开销成为主要瓶颈——上下文切换、数据拷贝、内核锁竞争。

**io_uring 的解决思路**：用户态和内核态共享两个环形缓冲区，**批量提交**、**批量收割**，甚至可以**零系统调用**完成 I/O：

```
io_uring 的工作方式：

  用户态（共享内存）                    内核态
   │                                    │
   │ 向 SQ 写入多个请求                  │
   │ [read fd_1] [send fd_2] [recv fd_3]│
   │────────────共享内存──────────────→  │  ← 无需系统调用！
   │                                    │
   │                                    │ 内核逐个执行
   │                                    │ read → send → recv
   │                                    │
   │ 从 CQ 读取多个结果                  │
   │ [fd_1: 4096B] [fd_2: OK] [fd_3: ...]│
   │←────────────共享内存───────────────│  ← 无需系统调用！
```

io_uring 是 Linux 5.1（2019 年）引入的革命性异步 I/O 框架，由 Jens Axboe 开发。本篇将带你从原理到实践，彻底理解它的设计。

---

## 1. Linux I/O 演进之路

### 1.1 四代 I/O 多路复用

```
第一代：select (1983)
  ├─ fd 数量限制（FD_SETSIZE = 1024）
  ├─ 每次调用拷贝整个 fd_set
  └─ O(n) 扫描所有 fd

第二代：poll (1997)
  ├─ 无 fd 数量限制（使用 pollfd 数组）
  ├─ 仍然每次拷贝整个数组
  └─ 仍然 O(n) 扫描

第三代：epoll (2002)
  ├─ 内核维护 fd 集合，增删改为增量操作
  ├─ 只返回就绪的 fd → O(活跃连接数)
  └─ ✅ 解决了 C10K，但每次 I/O 仍需独立系统调用

第四代：io_uring (2019)
  ├─ 共享内存环形缓冲区，避免数据拷贝
  ├─ 批量提交 + 批量收割
  ├─ 支持 SQPOLL 模式：零系统调用
  └─ ✅ 统一的异步接口：文件/网络/定时器全覆盖
```

### 1.2 为什么不是 Linux AIO？

Linux 有一个更早的异步 I/O 接口——`io_submit` / `io_getevents`（POSIX AIO / Linux Native AIO）。它的问题：

| 特性         | Linux AIO        | io_uring                   |
| ------------ | ---------------- | -------------------------- |
| 网络支持     | ❌ 仅支持文件     | **✅ 文件 + 网络 + 定时器** |
| 缓冲 I/O     | ❌ 仅 O_DIRECT    | **✅ 缓冲和直接都支持**     |
| 真正异步     | ❌ 经常退化为同步 | **✅ 内核线程池保证异步**   |
| 系统调用开销 | 每次提交一次调用 | **批量提交，可零调用**     |
| API 复杂度   | 简单但功能有限   | **功能丰富但设计优雅**     |

> io_uring 是 Linux 上**第一个真正意义上的通用异步 I/O 框架**——它能异步完成几乎所有系统调用（read、write、accept、connect、send、recv、openat、close、fsync……）。

---

## 2. io_uring 核心架构

### 2.1 双环形缓冲区

io_uring 的核心是两个**共享内存**的环形队列：

```
用户态进程                              内核
┌─────────────────────────────────────────────────────────┐
│                  共享内存区域（mmap）                      │
│                                                          │
│  ┌──────────────────────────────┐                        │
│  │  SQ (Submission Queue)       │  提交队列：用户写，内核读 │
│  │  ┌───┬───┬───┬───┬───┬───┐  │                        │
│  │  │ 0 │ 1 │ 2 │ 3 │ 4 │ 5 │  │  ← 每个槽位是一个索引  │
│  │  └───┴───┴───┴───┴───┴───┘  │    指向 SQE 数组中的项  │
│  │  head ──────→   ←────── tail │                        │
│  │  (内核消费)        (用户生产) │                        │
│  └──────────────────────────────┘                        │
│                                                          │
│  ┌──────────────────────────────┐                        │
│  │  SQE Array (提交队列项数组)   │                        │
│  │  ┌─────┬─────┬─────┬─────┐  │                        │
│  │  │SQE 0│SQE 1│SQE 2│SQE 3│  │  ← 每个 SQE 描述一个  │
│  │  │read │write│recv │send │  │    I/O 操作的全部参数  │
│  │  └─────┴─────┴─────┴─────┘  │                        │
│  └──────────────────────────────┘                        │
│                                                          │
│  ┌──────────────────────────────┐                        │
│  │  CQ (Completion Queue)       │  完成队列：内核写，用户读 │
│  │  ┌─────┬─────┬─────┬─────┐  │                        │
│  │  │CQE 0│CQE 1│CQE 2│CQE 3│  │  ← 每个 CQE 包含      │
│  │  │res=N│res=0│res=N│res=E│  │    操作结果和 user_data │
│  │  └─────┴─────┴─────┴─────┘  │                        │
│  │  head ──────→   ←────── tail │                        │
│  │  (用户消费)        (内核生产) │                        │
│  └──────────────────────────────┘                        │
└─────────────────────────────────────────────────────────┘
```

**关键设计**：
- SQ 和 CQ 都在 `mmap()` 映射的共享内存中，用户态和内核态**直接读写同一块内存**
- 使用 **memory barrier**（内存屏障）而非锁来同步，无竞争开销
- SQ 和 CQ 的大小都是 2 的幂，用位掩码实现高效的环形索引

### 2.2 SQE（Submission Queue Entry）结构

每个 SQE 描述一个待执行的 I/O 操作：

```c
// 简化的 io_uring_sqe 结构（实际约 64 字节）
struct io_uring_sqe {
    __u8   opcode;      // 操作类型：IORING_OP_READ / WRITE / RECV / SEND / ACCEPT ...
    __u8   flags;       // 标志位：IOSQE_FIXED_FILE | IOSQE_IO_LINK | ...
    __u16  ioprio;      // I/O 优先级
    __s32  fd;          // 目标文件描述符

    union {
        __u64 off;      // 文件偏移量（用于 read/write）
        __u64 addr2;    // 第二地址（用于某些操作）
    };

    union {
        __u64 addr;     // 缓冲区地址
        __u64 splice_off_in;
    };

    __u32  len;         // 缓冲区长度
    __u64  user_data;   // ← 关键！用户自定义标识，原样出现在 CQE 中
};
```

### 2.3 CQE（Completion Queue Entry）结构

每个 CQE 报告一个已完成的操作结果：

```c
// io_uring_cqe 结构（仅 16 字节，极简）
struct io_uring_cqe {
    __u64 user_data;   // 从对应 SQE 原样复制过来，用于识别"是哪个操作完成了"
    __s32 res;         // 操作结果：成功时等同于对应系统调用的返回值
                       //          失败时为负的 errno（如 -EAGAIN, -ECONNRESET）
    __u32 flags;       // 附加标志（如 IORING_CQE_F_MORE 表示 multishot 还有后续）
};
```

> **`user_data` 是 io_uring 异步模型的灵魂**——你在提交 SQE 时设置它（通常是连接对象的指针或请求 ID），在收割 CQE 时用它识别"这个完成对应的是哪个请求"。这和 Windows IOCP 的 `OVERLAPPED*` 机制异曲同工。

### 2.4 提交与收割的完整流程

```
提交一个异步读操作的步骤：

  ① 获取 SQE              用户态
     sqe = io_uring_get_sqe(&ring)
     ↓
  ② 填充 SQE
     io_uring_prep_read(sqe, fd, buf, len, offset)
     sqe->user_data = my_request_id
     ↓
  ③ 提交到内核
     io_uring_submit(&ring)          →  内核态
     ↓                                    ↓
     (可选：如果开了 SQPOLL，             内核从 SQ 取出 SQE
      这一步都省了)                       执行 read 操作
                                          ↓
                                     写入 CQE 到 CQ：
                                       cqe->user_data = my_request_id
                                       cqe->res = bytes_read (或 -errno)
     ↓                                    ↓
  ④ 等待完成
     io_uring_wait_cqe(&ring, &cqe)  ←
     ↓
  ⑤ 处理结果
     if (cqe->res >= 0) { /* 成功 */ }
     else { /* 错误：-cqe->res 即 errno */ }
     ↓
  ⑥ 标记已消费
     io_uring_cqe_seen(&ring, cqe)
```

---

## 3. 系统调用接口

io_uring 只需要**三个系统调用**（对比 epoll 的 `epoll_create` + `epoll_ctl` + `epoll_wait`）：

### 3.1 io_uring_setup

```c
// 创建一个 io_uring 实例
// entries: SQ 大小（会被向上取整为 2 的幂）
// params: 配置参数（输入/输出）
int io_uring_setup(unsigned entries, struct io_uring_params *params);
```

这个调用做了三件事：
1. 分配 SQ ring、CQ ring、SQE array 的内核内存
2. 返回一个文件描述符（ring fd），用于后续 `mmap()` 和 `io_uring_enter()`
3. 通过 `params` 返回各个 ring 的偏移量，用于 `mmap()` 映射

```c
// io_uring_params 的关键字段
struct io_uring_params {
    __u32 sq_entries;    // [输出] SQ 实际大小
    __u32 cq_entries;    // [输出] CQ 实际大小（默认 = 2 * sq_entries）
    __u32 flags;         // [输入] 配置标志
                         //   IORING_SETUP_SQPOLL  → 内核轮询模式
                         //   IORING_SETUP_IOPOLL  → 忙轮询 I/O 完成
                         //   IORING_SETUP_COOP_TASKRUN → 协作式任务运行
    __u32 sq_thread_cpu; // [输入] SQPOLL 线程绑定的 CPU
    __u32 sq_thread_idle;// [输入] SQPOLL 线程空闲超时（毫秒）

    struct io_sqring_offsets sq_off;  // [输出] SQ ring 的 mmap 偏移量
    struct io_cqring_offsets cq_off;  // [输出] CQ ring 的 mmap 偏移量
};
```

### 3.2 io_uring_enter

```c
// 提交 SQE 并/或等待 CQE
int io_uring_enter(unsigned int fd,        // ring fd
                   unsigned int to_submit, // 要提交的 SQE 数量
                   unsigned int min_complete, // 至少等待多少个 CQE
                   unsigned int flags);    // IORING_ENTER_GETEVENTS 等
```

**这个调用极为灵活**：
- `to_submit > 0, min_complete == 0`：只提交，不等待（非阻塞）
- `to_submit == 0, min_complete > 0`：只等待，不提交
- `to_submit > 0, min_complete > 0`：同时提交并等待（最常用）
- 在 SQPOLL 模式下，连这个调用都不需要——内核线程自动消费 SQ

### 3.3 io_uring_register

```c
// 预注册资源，避免每次 I/O 时内核重复映射
int io_uring_register(unsigned int fd,
                      unsigned int opcode,   // 注册类型
                      void *arg,
                      unsigned int nr_args);
```

常用注册操作：

| opcode                      | 作用                                      |
| --------------------------- | ----------------------------------------- |
| `IORING_REGISTER_BUFFERS`   | 预注册用户缓冲区，避免每次 I/O pin 页     |
| `IORING_REGISTER_FILES`     | 预注册文件描述符，跳过 fd 查表            |
| `IORING_REGISTER_RING_FD`   | 注册 ring fd 自身，减少引用计数开销       |
| `IORING_REGISTER_PBUF_RING` | 注册 provided buffer ring（内核选缓冲区） |

> 预注册是 io_uring 性能优化的关键手段。后续进阶篇会深入讲解。

---

## 4. liburing：优雅的封装库

直接操作 `io_uring_setup` + `mmap` 非常繁琐。`liburing` 是 Jens Axboe（io_uring 作者）维护的官方封装库，提供简洁的 C API：

### 4.1 核心 API 速览

```c
#include <liburing.h>

// === 初始化与销毁 ===
int  io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags);
void io_uring_queue_exit(struct io_uring *ring);

// === 获取 SQE ===
struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring);
//   返回 NULL 表示 SQ 已满，需要先 submit

// === 填充 SQE（prep 系列函数） ===
void io_uring_prep_read(struct io_uring_sqe *sqe,
                        int fd, void *buf, unsigned nbytes, __u64 offset);
void io_uring_prep_write(struct io_uring_sqe *sqe,
                         int fd, const void *buf, unsigned nbytes, __u64 offset);
void io_uring_prep_recv(struct io_uring_sqe *sqe,
                        int fd, void *buf, unsigned len, int flags);
void io_uring_prep_send(struct io_uring_sqe *sqe,
                        int fd, const void *buf, unsigned len, int flags);
void io_uring_prep_accept(struct io_uring_sqe *sqe,
                          int fd, struct sockaddr *addr,
                          socklen_t *addrlen, int flags);

// === 设置 user_data ===
void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data);
void io_uring_sqe_set_data64(struct io_uring_sqe *sqe, __u64 data);

// === 提交 ===
int io_uring_submit(struct io_uring *ring);
//   返回成功提交的 SQE 数量

// === 等待完成 ===
int io_uring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr);
//   阻塞等待至少一个 CQE
int io_uring_peek_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr);
//   非阻塞查看，无 CQE 则返回 -EAGAIN
int io_uring_wait_cqe_nr(struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
                         unsigned wait_nr);
//   等待至少 wait_nr 个 CQE

// === 读取 CQE 数据 ===
void *io_uring_cqe_get_data(const struct io_uring_cqe *cqe);
__u64 io_uring_cqe_get_data64(const struct io_uring_cqe *cqe);

// === 标记 CQE 已消费 ===
void io_uring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe);
```

### 4.2 使用模式：三步曲

几乎所有 io_uring 程序都遵循相同的模式：

```
io_uring 程序的基本骨架：

  ┌─ 初始化 ──────────────────────┐
  │  io_uring_queue_init()         │
  └────────────────────────────────┘
           │
           ▼
  ┌─ 事件循环 ────────────────────┐
  │  while (running) {             │
  │    ① 获取 SQE                  │
  │       sqe = io_uring_get_sqe() │
  │    ② 填充请求                  │
  │       io_uring_prep_xxx(sqe)   │
  │    ③ 提交                      │
  │       io_uring_submit()        │
  │    ④ 等待完成                  │
  │       io_uring_wait_cqe()      │
  │    ⑤ 处理结果                  │
  │       cqe->res / user_data     │
  │    ⑥ 释放 CQE                 │
  │       io_uring_cqe_seen()      │
  │  }                             │
  └────────────────────────────────┘
           │
           ▼
  ┌─ 清理 ────────────────────────┐
  │  io_uring_queue_exit()         │
  └────────────────────────────────┘
```

---

## 5. 第一个完整示例：异步文件读写

### 5.1 异步读取文件

```c
// async_read.c — 使用 io_uring 异步读取文件
// 编译：gcc -std=c11 -O2 async_read.c -luring -o async_read

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <文件路径>\n", argv[0]);
        return 1;
    }

    // 打开目标文件
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // ① 初始化 io_uring，SQ 大小为 4（向上取整为 2 的幂）
    struct io_uring ring;
    int ret = io_uring_queue_init(4, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init 失败: %s\n", strerror(-ret));
        close(fd);
        return 1;
    }

    // 分配读缓冲区
    char *buf = malloc(BUFFER_SIZE);
    memset(buf, 0, BUFFER_SIZE);

    // ② 获取一个 SQE
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "SQ 已满，无法获取 SQE\n");
        goto cleanup;
    }

    // ③ 填充 SQE：异步读取，从文件偏移 0 开始，读取 BUFFER_SIZE 字节
    io_uring_prep_read(sqe, fd, buf, BUFFER_SIZE, 0);
    // 设置 user_data，这里简单用整数 42 标识
    io_uring_sqe_set_data64(sqe, 42);

    // ④ 提交到内核
    ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit 失败: %s\n", strerror(-ret));
        goto cleanup;
    }
    printf("已提交 %d 个请求，等待完成...\n", ret);

    // ⑤ 等待完成
    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe 失败: %s\n", strerror(-ret));
        goto cleanup;
    }

    // ⑥ 处理结果
    __u64 userData = io_uring_cqe_get_data64(cqe);
    if (cqe->res < 0) {
        // 失败：res 是负的 errno
        fprintf(stderr, "读取失败 (request %llu): %s\n",
                userData, strerror(-cqe->res));
    } else {
        // 成功：res 是读取的字节数
        printf("读取成功 (request %llu): %d 字节\n", userData, cqe->res);
        printf("--- 文件内容 ---\n");
        // 只打印实际读到的字节
        fwrite(buf, 1, cqe->res, stdout);
        printf("\n--- 结束 ---\n");
    }

    // ⑦ 标记 CQE 已消费（推进 CQ head）
    io_uring_cqe_seen(&ring, cqe);

cleanup:
    free(buf);
    io_uring_queue_exit(&ring);
    close(fd);
    return 0;
}
// 运行：./async_read /etc/hostname
// 输出：
// 已提交 1 个请求，等待完成...
// 读取成功 (request 42): 12 字节
// --- 文件内容 ---
// my-server
// --- 结束 ---
```

### 5.2 批量提交：同时读取多个文件

io_uring 的杀手特性是**批量提交**——一次 `io_uring_submit()` 可以提交多个请求：

```c
// batch_read.c — 批量异步读取多个文件
// 编译：gcc -std=c11 -O2 batch_read.c -luring -o batch_read

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 1024
#define MAX_FILES   8

// 用于追踪每个请求的上下文
struct ReadRequest {
    const char *filename;
    int fd;
    char buf[BUFFER_SIZE];
};

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <文件1> [文件2] [文件3] ...\n", argv[0]);
        return 1;
    }

    int fileCount = argc - 1;
    if (fileCount > MAX_FILES) fileCount = MAX_FILES;

    // 初始化 io_uring，SQ 大小足够容纳所有请求
    struct io_uring ring;
    int ret = io_uring_queue_init(MAX_FILES, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret));
        return 1;
    }

    // 为每个文件创建请求上下文
    struct ReadRequest requests[MAX_FILES];

    for (int i = 0; i < fileCount; i++) {
        requests[i].filename = argv[i + 1];
        requests[i].fd = open(argv[i + 1], O_RDONLY);
        if (requests[i].fd < 0) {
            perror(argv[i + 1]);
            continue;
        }
        memset(requests[i].buf, 0, BUFFER_SIZE);

        // 获取 SQE 并填充
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_read(sqe, requests[i].fd,
                           requests[i].buf, BUFFER_SIZE, 0);
        // user_data 指向请求上下文——收割时用它识别是哪个文件的结果
        io_uring_sqe_set_data(sqe, &requests[i]);
    }

    // 一次性提交所有请求（内核批量处理）
    ret = io_uring_submit(&ring);
    printf("批量提交 %d 个读请求\n\n", ret);

    // 逐个收割完成结果
    for (int i = 0; i < fileCount; i++) {
        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) break;

        // 通过 user_data 还原请求上下文
        struct ReadRequest *req = io_uring_cqe_get_data(cqe);

        if (cqe->res < 0) {
            printf("[%s] 读取失败: %s\n", req->filename, strerror(-cqe->res));
        } else {
            printf("[%s] 读取 %d 字节: %.50s%s\n",
                   req->filename, cqe->res, req->buf,
                   cqe->res > 50 ? "..." : "");
        }

        io_uring_cqe_seen(&ring, cqe);
    }

    // 清理
    for (int i = 0; i < fileCount; i++) {
        if (requests[i].fd >= 0) close(requests[i].fd);
    }
    io_uring_queue_exit(&ring);
    return 0;
}
// 运行：./batch_read /etc/hostname /etc/os-release /etc/hosts
// 输出：
// 批量提交 3 个读请求
//
// [/etc/hostname] 读取 12 字节: my-server
// [/etc/os-release] 读取 400 字节: NAME="Ubuntu"
// VERSION="22.04.3 LTS (Jammy Jellyf...
// [/etc/hosts] 读取 221 字节: 127.0.0.1 localhost
// ...
```

> **对比 epoll**：如果用 epoll + read，你需要 `open` 后逐个调用 `read()` 系统调用，N 个文件 = N 次系统调用。io_uring 只需**一次** `io_uring_submit()`。

### 5.3 异步写入文件

```c
// async_write.c — 异步写入文件
// 编译：gcc -std=c11 -O2 async_write.c -luring -o async_write

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main()
{
    // 创建/截断输出文件
    int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }

    struct io_uring ring;
    io_uring_queue_init(4, &ring, 0);

    // 准备要写入的数据
    const char *messages[] = {
        "Hello from io_uring!\n",
        "这是第二行，异步写入的。\n",
        "Third line: batch write demo.\n",
    };

    // 批量提交三个写请求（注意：同一文件的写入需要指定不同偏移量）
    __u64 offset = 0;
    for (int i = 0; i < 3; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        size_t len = strlen(messages[i]);

        // prep_write 的最后一个参数是文件偏移量
        io_uring_prep_write(sqe, fd, messages[i], len, offset);
        io_uring_sqe_set_data64(sqe, i);

        offset += len;  // 下一个写请求从上一个结束位置开始
    }

    int submitted = io_uring_submit(&ring);
    printf("提交 %d 个写请求\n", submitted);

    // 收割所有完成事件
    for (int i = 0; i < submitted; i++) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);

        __u64 reqId = io_uring_cqe_get_data64(cqe);
        if (cqe->res < 0) {
            fprintf(stderr, "写入 %llu 失败: %s\n", reqId, strerror(-cqe->res));
        } else {
            printf("写入 %llu 完成: %d 字节\n", reqId, cqe->res);
        }
        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);
    close(fd);

    printf("\n写入完成，验证内容：\n");
    // 用系统命令验证
    // cat output.txt
    return 0;
}
// 输出：
// 提交 3 个写请求
// 写入 0 完成: 21 字节
// 写入 1 完成: 37 字节
// 写入 2 完成: 30 字节
```

> **注意**：对同一文件的多个写入如果偏移量重叠且没有用 `IOSQE_IO_LINK` 串联，内核可能乱序执行——导致数据交错。进阶篇会详细讲 Linked SQE 的用法。

---

## 6. io_uring vs epoll：性能对比视角

### 6.1 系统调用次数对比

假设处理 1000 个就绪连接，每个连接做一次 recv + 一次 send：

| 指标             | epoll                 | io_uring                      |
| ---------------- | --------------------- | ----------------------------- |
| 等待事件         | 1 次 `epoll_wait`     | 0 次（SQPOLL）或 1 次 `enter` |
| 提交 I/O         | 2000 次 `recv`/`send` | **1 次** `io_uring_submit`    |
| 收割结果         | —（同步返回）         | 1 次 `io_uring_wait_cqe`      |
| **合计系统调用** | **2001 次**           | **1~2 次**                    |

### 6.2 内存拷贝对比

```
epoll 的数据路径：
  内核缓冲区 → copy_to_user → 用户缓冲区        ← recv() 每次都拷贝
  用户缓冲区 → copy_from_user → 内核缓冲区      ← send() 每次都拷贝

io_uring + 固定缓冲区：
  用户缓冲区（已通过 io_uring_register 注册，页表已锁定）
  内核直接读写该缓冲区                           ← 省去了 pin/unpin 页的开销
```

### 6.3 何时该用 io_uring？

| 场景                         | 推荐         |
| ---------------------------- | ------------ |
| 连接数 < 1000，逻辑简单      | epoll 足够   |
| 高并发网络服务（C100K+）     | **io_uring** |
| 高吞吐文件 I/O（数据库引擎） | **io_uring** |
| 需要混合文件 + 网络 I/O      | **io_uring** |
| 需要兼容旧内核（< 5.1）      | epoll        |
| 跨平台（macOS/BSD）          | epoll/kqueue |

---

## 7. 内核版本与特性对照表

io_uring 的特性随内核版本不断扩展：

| 内核版本 | 新增特性                                                  |
| -------- | --------------------------------------------------------- |
| 5.1      | io_uring 首次引入，支持 read/write/fsync                  |
| 5.4      | `IORING_OP_TIMEOUT`、`IORING_OP_ACCEPT`                   |
| 5.5      | `IORING_OP_RECV` / `SEND`、SQ 轮询（SQPOLL）              |
| 5.6      | `io_uring_register` 预注册缓冲区                          |
| 5.7      | Linked SQE（链式操作）                                    |
| 5.10     | `IORING_OP_SHUTDOWN`、固定文件表更新                      |
| 5.13     | `IORING_OP_RENAMEAT` / `MKDIRAT` 等文件系统操作           |
| 5.19     | Multishot accept / recv、provided buffers ring            |
| 6.0      | `IORING_SETUP_COOP_TASKRUN`、`IORING_SETUP_SINGLE_ISSUER` |
| 6.1      | `io_uring_cmd`（设备自定义命令）                          |
| **6.7+** | **Zero-copy send**、增强的 provided buffers               |

> **建议**：生产环境至少使用 **5.19+** 内核以获得 multishot 和 provided buffers 支持——这是网络编程场景的关键优化。

---

## 本篇小结

| 概念          | 要点                                                              |
| ------------- | ----------------------------------------------------------------- |
| io_uring 定位 | Linux 第四代 I/O 框架，统一文件/网络异步操作                      |
| 双环形缓冲区  | SQ（用户→内核）+ CQ（内核→用户），共享内存 mmap，无锁设计         |
| SQE           | 描述一个 I/O 请求：opcode + fd + buffer + user_data               |
| CQE           | 描述一个完成结果：user_data + res（成功为字节数，失败为负 errno） |
| user_data     | 异步模型的灵魂，用于关联请求和完成                                |
| 批量提交      | 多个 SQE 一次 submit，大幅减少系统调用                            |
| liburing      | 官方 C 封装库，提供 prep/submit/wait 系列函数                     |
| 三大系统调用  | `io_uring_setup` + `io_uring_enter` + `io_uring_register`         |

下一篇 [进阶篇]({{< relref "posts/深入学习io_uring_进阶篇.md" >}}) 将深入高级特性（SQPOLL、Fixed Buffers、Linked SQE）和 TCP 网络编程。
