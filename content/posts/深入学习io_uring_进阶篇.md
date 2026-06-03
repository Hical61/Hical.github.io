+++
title = '深入学习 io_uring（二）：高级特性与 TCP 网络编程'
date = '2025-10-02'
draft = false
tags = ["Linux", "io_uring", "C++", "网络编程", "高性能", "TCP", "SQPOLL"]
categories = ["io_uring学习"]
description = "深入 io_uring 的高级特性——SQPOLL 零系统调用、Fixed Buffers 零拷贝、Linked SQE 依赖链、Multishot 操作，并用 io_uring 实现完整的 TCP Echo Server。"
+++

> **系列导航**：[入门篇]({{< relref "posts/深入学习io_uring_入门篇.md" >}}) | **进阶篇** | [实战篇]({{< relref "posts/深入学习io_uring_实战篇.md" >}})

## 前置知识

- 已阅读入门篇，理解 SQ/CQ 双环形缓冲区和 liburing 基本 API
- 熟悉 TCP socket 编程基础（socket、bind、listen、accept）

---

## 1. SQPOLL：零系统调用提交

### 1.1 常规模式的瓶颈

入门篇中每次 `io_uring_submit()` 底层都会调用 `io_uring_enter()` 系统调用：

```
常规模式：
  用户态                        内核态
   │                             │
   │ SQE 写入共享内存              │
   │                             │
   │ io_uring_enter(to_submit=N) │
   ├────────系统调用──────────────→│  ← 仍有上下文切换
   │                             │ 读取 SQ，执行 I/O
   │              返回            │
   │←─────────────────────────────┤
```

对于超高频提交场景（如高频交易、高吞吐数据库），连这一次系统调用都嫌多。

### 1.2 SQPOLL 模式工作原理

开启 `IORING_SETUP_SQPOLL` 后，内核会创建一个**专用轮询线程**，持续监控 SQ：

```
SQPOLL 模式：
  用户态                        内核态
   │                             │
   │ SQE 写入共享内存              │   ┌─────────────────────┐
   │                             │   │  SQ 轮询线程 (kthread)│
   │（不需要任何系统调用！）        │   │  while (!idle) {     │
   │                             │   │    检查 SQ tail       │
   │                             │   │    有新 SQE → 执行    │
   │                             │   │    无新 SQE → 休眠    │
   │                             │   │  }                    │
   │                             │   └─────────────────────┘
   │                             │
   │ 直接从 CQ 读取结果            │   CQE 写入共享内存
   │←────────共享内存──────────────│
```

**零系统调用的完整路径**：用户写 SQ → 内核线程轮询发现 → 执行 I/O → 写 CQ → 用户读 CQ。全程无 `io_uring_enter()`。

### 1.3 代码示例

```c
// sqpoll_demo.c — 使用 SQPOLL 模式的异步读
// 编译：gcc -std=c11 -O2 sqpoll_demo.c -luring -o sqpoll_demo
// 注意：SQPOLL 需要 root 权限或 CAP_SYS_NICE 能力

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main()
{
    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    // 关键标志：启用 SQ 轮询线程
    params.flags = IORING_SETUP_SQPOLL;
    // 轮询线程空闲 2000ms 后休眠（节省 CPU）
    params.sq_thread_idle = 2000;

    int ret = io_uring_queue_init_params(8, &ring, &params);
    if (ret < 0) {
        fprintf(stderr, "初始化失败: %s（需要 root 权限）\n", strerror(-ret));
        return 1;
    }

    printf("SQPOLL 模式已启用，SQ 轮询线程 CPU: %u\n", params.sq_thread_cpu);

    int fd = open("/etc/hostname", O_RDONLY);
    char buf[256] = {0};

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fd, buf, sizeof(buf), 0);
    io_uring_sqe_set_data64(sqe, 1);

    // 在 SQPOLL 模式下，io_uring_submit() 实际上不调用 io_uring_enter()
    // 它只是更新 SQ tail 指针，内核轮询线程会自动发现新的 SQE
    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    io_uring_wait_cqe(&ring, &cqe);
    // 注意：wait_cqe 在 SQPOLL 模式下可能仍需系统调用
    //       （当 CQ 为空时需要唤醒轮询线程）
    //       但如果 CQ 中已有结果，则直接读取共享内存即可

    if (cqe->res >= 0) {
        printf("读取 %d 字节: %s", cqe->res, buf);
    }

    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    close(fd);
    return 0;
}
```

### 1.4 SQPOLL 的注意事项

| 注意事项         | 说明                                                         |
| ---------------- | ------------------------------------------------------------ |
| 权限要求         | 需要 `root` 或 `CAP_SYS_NICE`                                |
| CPU 占用         | 轮询线程活跃期间会占用 100% 的一个 CPU 核心                  |
| 空闲休眠         | `sq_thread_idle` 控制无新 SQE 时多久后休眠                   |
| **唤醒轮询线程** | 线程休眠后需要 `io_uring_enter(IORING_ENTER_SQ_WAKEUP)` 唤醒 |
| 适用场景         | 超低延迟、CPU 核心充足的场景                                 |

> 大多数场景下，常规模式的 `io_uring_submit()` 已经足够快（一次系统调用提交数十个请求）。**不要无脑开 SQPOLL**——它用 CPU 换延迟，只在延迟极其敏感的场景下值得。

---

## 2. Fixed Buffers：预注册消除页表操作

### 2.1 普通 I/O 的页表开销

每次 `read()` / `write()` 时，内核需要：
1. 验证用户态缓冲区地址合法
2. **Pin 物理页**（防止 I/O 期间页被换出）
3. 执行 DMA 传输
4. **Unpin 物理页**

```
普通 I/O 的内存操作：

  每次 read/write:
    ① get_user_pages() → 锁定页面 → 设置 DMA 映射
    ② 执行 I/O
    ③ put_user_pages() → 解锁页面 → 拆除 DMA 映射
    
  每次操作的开销：~1-5 μs（取决于页面数量和 IOMMU 配置）
  高频 I/O 下累积非常可观
```

### 2.2 Fixed Buffers 跳过重复映射

`io_uring_register(IORING_REGISTER_BUFFERS)` 一次性完成页面锁定和 DMA 映射，后续 I/O 直接使用：

```
Fixed Buffers 的内存操作：

  初始化时一次性完成：
    io_uring_register_buffers()
      → get_user_pages() × N 个缓冲区
      → 设置 DMA 映射
      → 缓存到 io_uring 实例

  每次 read_fixed/write_fixed:
    ① 直接使用已锁定的页面  ← 跳过 pin/unpin！
    ② 执行 I/O
    （无需 unpin，页面一直锁着）

  销毁时一次性清理：
    io_uring_unregister_buffers()
      → put_user_pages() × N
```

### 2.3 代码示例

```c
// fixed_buffers.c — 使用预注册缓冲区的高性能读写
// 编译：gcc -std=c11 -O2 fixed_buffers.c -luring -o fixed_buffers

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define BUF_COUNT 4

int main()
{
    struct io_uring ring;
    io_uring_queue_init(8, &ring, 0);

    // ① 分配缓冲区数组（必须页对齐以获得最佳性能）
    char *buffers[BUF_COUNT];
    struct iovec iovecs[BUF_COUNT];

    for (int i = 0; i < BUF_COUNT; i++) {
        // posix_memalign 保证页对齐
        posix_memalign((void **)&buffers[i], 4096, BUF_SIZE);
        memset(buffers[i], 0, BUF_SIZE);
        iovecs[i].iov_base = buffers[i];
        iovecs[i].iov_len = BUF_SIZE;
    }

    // ② 预注册缓冲区——此后使用 prep_read_fixed / prep_write_fixed
    int ret = io_uring_register_buffers(&ring, iovecs, BUF_COUNT);
    if (ret < 0) {
        fprintf(stderr, "注册缓冲区失败: %s\n", strerror(-ret));
        return 1;
    }
    printf("成功注册 %d 个固定缓冲区\n", BUF_COUNT);

    int fd = open("/etc/os-release", O_RDONLY);

    // ③ 使用 prep_read_fixed 代替 prep_read
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read_fixed(sqe,
        fd,                  // 文件描述符
        buffers[0],          // 缓冲区地址（必须是注册过的）
        BUF_SIZE,            // 读取长度
        0,                   // 文件偏移
        0);                  // buf_index：iovecs 数组的索引！
    io_uring_sqe_set_data64(sqe, 100);

    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    io_uring_wait_cqe(&ring, &cqe);

    if (cqe->res > 0) {
        printf("读取 %d 字节:\n%.*s\n", cqe->res, cqe->res, buffers[0]);
    }

    io_uring_cqe_seen(&ring, cqe);

    // ④ 清理：注销缓冲区
    io_uring_unregister_buffers(&ring);
    for (int i = 0; i < BUF_COUNT; i++) free(buffers[i]);
    io_uring_queue_exit(&ring);
    close(fd);
    return 0;
}
```

> **性能提示**：Fixed Buffers 在高频小 I/O 场景（如数据库随机读写 4KB 页）效果最显著。对于低频大块 I/O，pin/unpin 开销占比不高，收益有限。

---

## 3. Fixed Files：预注册文件描述符

### 3.1 常规 fd 的开销

每次 I/O 操作时，内核需要通过 fd 号在进程的文件描述符表中查找对应的 `struct file*`，并对其引用计数做 `atomic_inc` / `atomic_dec`。在高并发场景下，这些原子操作在多核上产生 cache bouncing。

### 3.2 Fixed Files 跳过 fd 表查找

```c
// fixed_files.c — 预注册文件描述符
// 编译：gcc -std=c11 -O2 fixed_files.c -luring -o fixed_files

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main()
{
    struct io_uring ring;
    io_uring_queue_init(8, &ring, 0);

    // 打开文件获取 fd
    int fds[2];
    fds[0] = open("/etc/hostname", O_RDONLY);
    fds[1] = open("/etc/hosts", O_RDONLY);

    // 注册文件描述符表
    int ret = io_uring_register_files(&ring, fds, 2);
    if (ret < 0) {
        fprintf(stderr, "注册文件失败: %s\n", strerror(-ret));
        return 1;
    }

    char buf[256] = {0};
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);

    // 使用注册索引（0）而非原始 fd
    io_uring_prep_read(sqe, 0, buf, sizeof(buf), 0);
    // 关键：设置 FIXED_FILE 标志，告诉内核 fd 字段是注册表索引
    sqe->flags |= IOSQE_FIXED_FILE;

    io_uring_sqe_set_data64(sqe, 1);
    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    io_uring_wait_cqe(&ring, &cqe);
    if (cqe->res > 0) {
        printf("通过 Fixed File 读取: %.*s", cqe->res, buf);
    }

    io_uring_cqe_seen(&ring, cqe);
    io_uring_unregister_files(&ring);
    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
    return 0;
}
```

---

## 4. Linked SQE：操作依赖链

### 4.1 问题：顺序执行

有时你需要保证 I/O 操作的执行顺序——比如先 read，read 完再 write。默认情况下，io_uring 可能乱序执行同一批提交的 SQE。

### 4.2 IOSQE_IO_LINK 标志

给 SQE 设置 `IOSQE_IO_LINK` 标志，将它和下一个 SQE 串联：

```
Linked SQE 执行流程：

  SQE_A [LINK] → SQE_B [LINK] → SQE_C
    │                │               │
    │ 先执行          │               │
    ├──────────→ A 成功？            │
    │            ├─ 是 → 执行 B      │
    │            │       ├─ 成功？    │
    │            │       ├─ 是 → 执行 C
    │            │       └─ 否 → C 以 -ECANCELED 完成
    │            └─ 否 → B 和 C 都以 -ECANCELED 完成
```

### 4.3 代码示例：链式 read-then-write（文件拷贝）

```c
// linked_copy.c — 用 Linked SQE 实现 read→write 文件拷贝
// 编译：gcc -std=c11 -O2 linked_copy.c -luring -o linked_copy

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <源文件> <目标文件>\n", argv[0]);
        return 1;
    }

    int srcFd = open(argv[1], O_RDONLY);
    int dstFd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (srcFd < 0 || dstFd < 0) { perror("open"); return 1; }

    struct io_uring ring;
    io_uring_queue_init(8, &ring, 0);

    char buf[BUF_SIZE];

    // SQE 1：异步读源文件
    struct io_uring_sqe *sqeRead = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqeRead, srcFd, buf, BUF_SIZE, 0);
    io_uring_sqe_set_data64(sqeRead, 1);
    // 关键：设置 LINK 标志，将此 SQE 和下一个链接
    sqeRead->flags |= IOSQE_IO_LINK;

    // SQE 2：异步写目标文件（只有 SQE 1 成功后才执行）
    struct io_uring_sqe *sqeWrite = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqeWrite, dstFd, buf, BUF_SIZE, 0);
    io_uring_sqe_set_data64(sqeWrite, 2);
    // 这个不设 LINK，它是链的最后一个

    // 一次提交两个 SQE
    int submitted = io_uring_submit(&ring);
    printf("提交 %d 个链式请求\n", submitted);

    // 收割两个 CQE（每个 SQE 无论成功失败都会产生一个 CQE）
    for (int i = 0; i < 2; i++) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);

        __u64 reqId = io_uring_cqe_get_data64(cqe);
        if (cqe->res < 0) {
            if (cqe->res == -ECANCELED) {
                printf("请求 %llu: 因前序操作失败被取消\n", reqId);
            } else {
                printf("请求 %llu: 失败 %s\n", reqId, strerror(-cqe->res));
            }
        } else {
            printf("请求 %llu: 完成 %d 字节\n", reqId, cqe->res);
        }
        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);
    close(srcFd);
    close(dstFd);
    return 0;
}
// 运行：./linked_copy /etc/hostname copy.txt
// 输出：
// 提交 2 个链式请求
// 请求 1: 完成 12 字节
// 请求 2: 完成 4096 字节
```

> **注意**：链式 SQE 的 write 操作中的 `len` 是你预先指定的，不会自动等于 read 实际读到的字节数。如果需要精确匹配，需要在收割 read 的 CQE 后再提交 write——这正是事件循环模式的用武之地。

### 4.4 IOSQE_IO_HARDLINK

`IOSQE_IO_LINK` 在前序失败时取消后续。如果你希望"无论前序成功失败，后续都执行"，使用 `IOSQE_IO_HARDLINK`：

```c
// 硬链接：read 即使失败，close 也要执行（确保资源释放）
sqeRead->flags |= IOSQE_IO_HARDLINK;
// 后续 SQE：close(fd) —— 无论 read 结果如何都执行
struct io_uring_sqe *sqeClose = io_uring_get_sqe(&ring);
io_uring_prep_close(sqeClose, fd);
```

---

## 5. Multishot 操作：一次提交，多次完成

### 5.1 传统 accept 的重复提交问题

```
传统模式下的 accept 循环：

  提交 accept SQE
       ↓
  等待 CQE（一个新连接）
       ↓
  处理新连接
       ↓
  再次提交 accept SQE    ← 每次都要重新提交！
       ↓
  等待 CQE...
  （循环往复）
```

每接受一个连接就要重新提交一次 accept SQE——在每秒数万新连接的场景下，这种重复提交也是开销。

### 5.2 Multishot Accept（内核 5.19+）

Multishot 让一个 SQE 产生**多个** CQE——提交一次 accept，内核每接受一个连接就产生一个 CQE：

```
Multishot accept：

  提交 accept SQE（设置 IORING_ACCEPT_MULTISHOT）
       ↓
  CQE 1: 新连接 fd_a        ← flags 含 IORING_CQE_F_MORE
  CQE 2: 新连接 fd_b        ← flags 含 IORING_CQE_F_MORE
  CQE 3: 新连接 fd_c        ← flags 含 IORING_CQE_F_MORE
  ...
  CQE N: 出错或被取消        ← flags 不含 IORING_CQE_F_MORE → 终止
```

### 5.3 代码示例

```c
// multishot_accept.c — Multishot Accept 示例
// 编译：gcc -std=c11 -O2 multishot_accept.c -luring -o multishot_accept
// 需要：Linux 5.19+

#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main()
{
    // 创建监听 socket
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(9000),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(listenFd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listenFd, 128);
    printf("监听端口 9000...\n");

    struct io_uring ring;
    io_uring_queue_init(32, &ring, 0);

    // 提交 Multishot Accept —— 只需提交一次！
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_multishot_accept(sqe, listenFd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, 0);  // 0 表示 accept 事件

    io_uring_submit(&ring);

    // 事件循环
    while (1) {
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) break;

        if (cqe->res < 0) {
            fprintf(stderr, "accept 错误: %s\n", strerror(-cqe->res));
            io_uring_cqe_seen(&ring, cqe);
            break;
        }

        int clientFd = cqe->res;
        printf("新连接: fd=%d", clientFd);

        // 检查是否还有后续 CQE
        if (cqe->flags & IORING_CQE_F_MORE) {
            printf("（multishot 继续）\n");
            // 不需要重新提交 accept！
        } else {
            printf("（multishot 终止，需要重新提交）\n");
            // Multishot 被终止（可能是 listen fd 出错），需要重新提交
            struct io_uring_sqe *newSqe = io_uring_get_sqe(&ring);
            io_uring_prep_multishot_accept(newSqe, listenFd, NULL, NULL, 0);
            io_uring_sqe_set_data64(newSqe, 0);
            io_uring_submit(&ring);
        }

        io_uring_cqe_seen(&ring, cqe);

        // 简单处理：关闭连接（实际应该读写数据）
        close(clientFd);
    }

    io_uring_queue_exit(&ring);
    close(listenFd);
    return 0;
}
```

### 5.4 Multishot Recv（内核 5.19+）

Multishot 也可用于 `recv`——一次提交，每次有数据到达都产生 CQE。配合 **Provided Buffers**（内核自动选取缓冲区）效果更佳：

```c
// 普通模式：你提供缓冲区
io_uring_prep_recv(sqe, fd, myBuf, bufLen, 0);
// 每次 recv 完需要重新提交

// Multishot + Provided Buffers：内核自动选缓冲区
io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
sqe->buf_group = myBufGroup;  // 指定缓冲区组
sqe->flags |= IOSQE_BUFFER_SELECT;
// 每次有数据到达，内核从缓冲区组中选一个写入，产生 CQE
// 不需要重新提交！
```

---

## 6. TCP Echo Server：从单连接到完整实现

### 6.1 第一步：单连接 Echo

先实现最简单的——处理一个连接的 recv→send 回显：

```c
// echo_single.c — 单连接 Echo Server
// 编译：gcc -std=c11 -O2 echo_single.c -luring -o echo_single

#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 1024

// 请求类型枚举
enum EventType {
    EVENT_ACCEPT = 0,
    EVENT_READ   = 1,
    EVENT_WRITE  = 2,
};

int main()
{
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(9000),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(listenFd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listenFd, 128);

    struct io_uring ring;
    io_uring_queue_init(32, &ring, 0);

    // 提交 accept 请求
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, listenFd, NULL, NULL, 0);
    // 用 user_data 编码事件类型
    io_uring_sqe_set_data64(sqe, EVENT_ACCEPT);
    io_uring_submit(&ring);

    char buf[BUF_SIZE];
    int clientFd = -1;

    printf("Echo Server 监听端口 9000...\n");

    while (1) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);

        __u64 eventType = io_uring_cqe_get_data64(cqe);

        switch (eventType) {
        case EVENT_ACCEPT: {
            clientFd = cqe->res;
            if (clientFd < 0) {
                fprintf(stderr, "accept 失败: %s\n", strerror(-clientFd));
                break;
            }
            printf("新连接: fd=%d\n", clientFd);

            // 提交 recv 请求
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_recv(sqe, clientFd, buf, BUF_SIZE, 0);
            io_uring_sqe_set_data64(sqe, EVENT_READ);
            io_uring_submit(&ring);
            break;
        }
        case EVENT_READ: {
            if (cqe->res <= 0) {
                // 连接关闭或出错
                printf("连接关闭 (fd=%d)\n", clientFd);
                close(clientFd);
                // 重新提交 accept
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_accept(sqe, listenFd, NULL, NULL, 0);
                io_uring_sqe_set_data64(sqe, EVENT_ACCEPT);
                io_uring_submit(&ring);
                break;
            }

            int bytesRead = cqe->res;
            printf("收到 %d 字节: %.*s", bytesRead, bytesRead, buf);

            // 提交 send 请求（回显数据）
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_send(sqe, clientFd, buf, bytesRead, 0);
            io_uring_sqe_set_data64(sqe, EVENT_WRITE);
            io_uring_submit(&ring);
            break;
        }
        case EVENT_WRITE: {
            if (cqe->res < 0) {
                fprintf(stderr, "send 失败: %s\n", strerror(-cqe->res));
                close(clientFd);
                break;
            }
            printf("回显 %d 字节\n", cqe->res);

            // send 完成后，继续 recv
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_recv(sqe, clientFd, buf, BUF_SIZE, 0);
            io_uring_sqe_set_data64(sqe, EVENT_READ);
            io_uring_submit(&ring);
            break;
        }
        }

        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);
    close(listenFd);
    return 0;
}
// 测试：
// 终端1: ./echo_single
// 终端2: nc localhost 9000
//         输入 "hello"
//         收到 "hello"
```

### 6.2 第二步：多连接 Echo Server

单连接版本的问题：只能处理一个连接。改进为多连接版本——用结构体追踪每个连接的状态：

```c
// echo_multi.c — 多连接 Echo Server
// 编译：gcc -std=c11 -O2 echo_multi.c -luring -o echo_multi

#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE    1024
#define MAX_CONNS   128

enum EventType { EVENT_ACCEPT, EVENT_READ, EVENT_WRITE };

// 每个连接的上下文
struct ConnCtx {
    int fd;
    enum EventType eventType;
    char buf[BUF_SIZE];
};

// 用于 accept 的特殊上下文
struct ConnCtx acceptCtx = { .fd = -1, .eventType = EVENT_ACCEPT };

// 提交 accept 请求的辅助函数
void submitAccept(struct io_uring *ring, int listenFd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, listenFd, NULL, NULL, 0);
    acceptCtx.fd = listenFd;
    io_uring_sqe_set_data(sqe, &acceptCtx);
}

// 提交 recv 请求的辅助函数
void submitRecv(struct io_uring *ring, struct ConnCtx *ctx)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, ctx->fd, ctx->buf, BUF_SIZE, 0);
    ctx->eventType = EVENT_READ;
    io_uring_sqe_set_data(sqe, ctx);
}

// 提交 send 请求的辅助函数
void submitSend(struct io_uring *ring, struct ConnCtx *ctx, int len)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_send(sqe, ctx->fd, ctx->buf, len, 0);
    ctx->eventType = EVENT_WRITE;
    io_uring_sqe_set_data(sqe, ctx);
}

int main()
{
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(9000),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(listenFd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listenFd, 128);

    struct io_uring ring;
    io_uring_queue_init(256, &ring, 0);

    // 初始提交 accept
    submitAccept(&ring, listenFd);
    io_uring_submit(&ring);

    printf("多连接 Echo Server 监听端口 9000...\n");

    while (1) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);

        struct ConnCtx *ctx = io_uring_cqe_get_data(cqe);

        switch (ctx->eventType) {
        case EVENT_ACCEPT: {
            int clientFd = cqe->res;
            if (clientFd >= 0) {
                printf("[+] 新连接 fd=%d\n", clientFd);

                // 为新连接分配上下文
                struct ConnCtx *newCtx = malloc(sizeof(struct ConnCtx));
                newCtx->fd = clientFd;

                // 提交 recv
                submitRecv(&ring, newCtx);
            }

            // 立即再次提交 accept（接受下一个连接）
            submitAccept(&ring, listenFd);
            io_uring_submit(&ring);
            break;
        }
        case EVENT_READ: {
            if (cqe->res <= 0) {
                // 连接关闭
                printf("[-] 连接关闭 fd=%d\n", ctx->fd);
                close(ctx->fd);
                free(ctx);
            } else {
                // 收到数据，回显
                submitSend(&ring, ctx, cqe->res);
                io_uring_submit(&ring);
            }
            break;
        }
        case EVENT_WRITE: {
            if (cqe->res < 0) {
                printf("[!] 发送失败 fd=%d: %s\n", ctx->fd, strerror(-cqe->res));
                close(ctx->fd);
                free(ctx);
            } else {
                // 发送完成，继续 recv
                submitRecv(&ring, ctx);
                io_uring_submit(&ring);
            }
            break;
        }
        }

        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);
    close(listenFd);
    return 0;
}
// 测试：
// 终端1: ./echo_multi
// 终端2: nc localhost 9000   ← 连接1
// 终端3: nc localhost 9000   ← 连接2
// 两个连接可以同时收发消息
```

### 6.3 对比三种 TCP 服务器模型

| 指标          | 多线程（1 线程/连接） | epoll 单线程     | **io_uring 单线程**          |
| ------------- | --------------------- | ---------------- | ---------------------------- |
| 系统调用/连接 | 2（recv + send）      | 2（recv + send） | **可批量，摊到 < 1**         |
| 线程数        | O(连接数)             | 1                | 1                            |
| 内存/连接     | ~1MB（线程栈）        | ~几十 KB         | **~几十 KB**                 |
| 内核通知方式  | 阻塞唤醒              | epoll_wait       | **CQE 共享内存**             |
| 新增连接开销  | 创建线程 ~100μs       | epoll_ctl ~1μs   | **直接 accept → 0 额外注册** |
| 代码复杂度    | 低（同步风格）        | 中（状态机）     | **中（状态机，但更统一）**   |

---

## 7. 常见陷阱

### 7.1 忘记 io_uring_cqe_seen

```c
// ❌ 忘记标记 CQE 已消费
io_uring_wait_cqe(&ring, &cqe);
process(cqe);
// 没有调用 io_uring_cqe_seen()！
// CQ head 没有推进 → CQ 最终会满 → 新的完成无法入队 → 死锁

// ✅ 正确做法
io_uring_wait_cqe(&ring, &cqe);
process(cqe);
io_uring_cqe_seen(&ring, cqe);  // 必须！
```

### 7.2 SQ 满时获取 SQE 失败

```c
// ❌ 不检查 get_sqe 返回值
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, fd, buf, len, 0);  // sqe 可能是 NULL！

// ✅ 正确做法
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
if (!sqe) {
    // SQ 已满，先 submit 消化掉积压的 SQE
    io_uring_submit(&ring);
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        // 仍然失败，说明 SQ 太小，需要增大
        fprintf(stderr, "SQ 容量不足！\n");
        return;
    }
}
```

### 7.3 缓冲区生命周期

```c
// ❌ 缓冲区在 I/O 完成前被释放
void badExample(struct io_uring *ring, int fd)
{
    char buf[1024];  // 栈上分配
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_read(sqe, fd, buf, sizeof(buf), 0);
    io_uring_submit(ring);
    // 函数返回 → buf 被销毁 → 内核往已释放的地址写数据 → 未定义行为！
}

// ✅ 正确做法：确保缓冲区存活到 CQE 被处理
struct Request {
    char buf[1024];
    int fd;
};

void goodExample(struct io_uring *ring, int fd)
{
    struct Request *req = malloc(sizeof(struct Request));
    req->fd = fd;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_read(sqe, fd, req->buf, sizeof(req->buf), 0);
    io_uring_sqe_set_data(sqe, req);  // 通过 user_data 追踪
    io_uring_submit(ring);
    // req 在 CQE 处理时 free
}
```

### 7.4 提交后修改 SQE

```c
// ❌ submit 后修改 SQE
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, fd, buf, 1024, 0);
io_uring_submit(&ring);
sqe->len = 2048;  // 无效！内核已经拷贝走了 SQE 的内容

// ✅ SQE 在 submit 时被内核消费，之后可以安全重用其槽位
```

---

## 本篇小结

| 概念             | 要点                                                                |
| ---------------- | ------------------------------------------------------------------- |
| SQPOLL           | 内核轮询线程自动消费 SQ，实现零系统调用提交；需 root，消耗 CPU      |
| Fixed Buffers    | 预注册缓冲区避免每次 I/O 的 pin/unpin 页开销                        |
| Fixed Files      | 预注册 fd 避免文件描述符表查找和引用计数原子操作                    |
| Linked SQE       | `IOSQE_IO_LINK` 保证 SQE 顺序执行；前序失败后续以 `-ECANCELED` 完成 |
| HARDLINK         | `IOSQE_IO_HARDLINK` 无条件执行后续（用于清理操作）                  |
| Multishot Accept | 一次提交持续产生 CQE，`IORING_CQE_F_MORE` 标志判断是否继续          |
| Multishot Recv   | 配合 Provided Buffers 使用，内核自动选缓冲区                        |
| TCP Echo Server  | 用 user_data 编码事件类型和连接上下文，形成状态机驱动的事件循环     |

下一篇 [实战篇]({{< relref "posts/深入学习io_uring_实战篇.md" >}}) 将用 C++ 封装 io_uring，实现协程集成和高性能服务器架构。
