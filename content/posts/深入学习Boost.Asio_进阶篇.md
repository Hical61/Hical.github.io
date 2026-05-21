+++
title = '深入学习 Boost.Asio（二）：TCP 编程与多线程模型'
date = '2026-05-20'
draft = false
tags = ["Boost", "Boost.Asio", "C++", "TCP", "多线程", "strand", "协程"]
categories = ["C++深入学习"]
description = "从同步 TCP 到协程异步，一步步构建 Echo Server；深入理解 strand 序列化、多 io_context 线程模型、async_write 与 async_write_some 的区别。"
+++

> **系列导航**：[入门篇]({{< relref "posts/深入学习Boost.Asio_入门篇.md" >}}) | **进阶篇** | [实战篇]({{< relref "posts/深入学习Boost.Asio_实战篇.md" >}})

## 前置知识

阅读本篇前，请确保已理解 [入门篇]({{< relref "posts/深入学习Boost.Asio_入门篇.md" >}}) 中的以下概念：
- io_context 的作用和 run() 执行流程
- 异步操作的生命周期（发起 → 完成 → handler 执行）
- post/dispatch 的区别

---

## 1. TCP 编程：三步演进

我们通过构建一个 Echo Server（收到什么就回什么），从最简单的同步版本逐步演进到生产级协程版本。

### 1.1 第一步：同步阻塞版

```cpp
// echo_server_sync.cpp
// 编译: g++ -std=c++20 echo_server_sync.cpp -lboost_system -lpthread -o echo
// 测试: 另开终端 nc localhost 9999，输入文字会回显

#include <boost/asio.hpp>
#include <iostream>

using boost::asio::ip::tcp;

int main()
{
    boost::asio::io_context ioCtx;

    // 创建 acceptor：监听 TCP 连接
    // 参数：io_context, 绑定地址(IPv4, 端口9999)
    tcp::acceptor acceptor(ioCtx, tcp::endpoint(tcp::v4(), 9999));
    std::cout << "同步 Echo Server 监听端口 9999\n";

    while (true)
    {
        // accept() 阻塞，直到有客户端连接
        tcp::socket socket(ioCtx);
        acceptor.accept(socket);

        std::cout << "客户端连接: "
                  << socket.remote_endpoint().address().to_string()
                  << ":" << socket.remote_endpoint().port() << "\n";

        // 处理这个连接（阻塞：处理期间无法接受新连接！）
        boost::system::error_code ec;
        char buf[1024];
        while (true)
        {
            // read_some：读取可用的数据（可能只有一部分）
            size_t n = socket.read_some(boost::asio::buffer(buf), ec);
            if (ec == boost::asio::error::eof)
            {
                std::cout << "客户端断开\n";
                break;
            }
            if (ec) throw boost::system::system_error(ec);

            // 将收到的数据原样写回
            boost::asio::write(socket, boost::asio::buffer(buf, n));
        }
    }

    return 0;
}
```

**问题**：同一时刻只能服务一个客户端。当客户端 A 连接后，客户端 B 必须等 A 断开才能被接受。

### 1.2 第二步：异步回调版（支持并发）

```cpp
// echo_server_async.cpp
// 编译: g++ -std=c++20 echo_server_async.cpp -lboost_system -lpthread -o echo

#include <boost/asio.hpp>
#include <iostream>
#include <memory>

using boost::asio::ip::tcp;

// 用 shared_ptr 管理 Session 的生命周期
// 原因：异步回调可能在 Session 对象被销毁后才执行
// enable_shared_from_this 允许在成员函数中安全获取 shared_ptr
class Session : public std::enable_shared_from_this<Session>
{
public:
    explicit Session(tcp::socket socket)
        : socket_(std::move(socket))  // 接管 socket 的所有权
    {
    }

    void start()
    {
        doRead();
    }

private:
    void doRead()
    {
        // shared_from_this()：获取指向自身的 shared_ptr
        // 将它捕获到 lambda 中，保证回调执行时 Session 对象仍然存活
        auto self = shared_from_this();

        socket_.async_read_some(
            boost::asio::buffer(data_, sizeof(data_)),
            // 回调：数据读取完成（或出错）时被调用
            [this, self](boost::system::error_code ec, size_t length) {
                if (!ec)
                {
                    doWrite(length);
                }
                // 如果 ec（如 EOF），Session 的引用计数降为 0 后自动销毁
            });
    }

    void doWrite(size_t length)
    {
        auto self = shared_from_this();

        // async_write：保证将 length 字节全部写出
        // （与 async_write_some 不同，后者可能只写一部分）
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(data_, length),
            [this, self](boost::system::error_code ec, size_t) {
                if (!ec)
                {
                    doRead();  // 写完后继续读
                }
            });
    }

    tcp::socket socket_;
    char data_[1024];
};

// 负责接受新连接
class Server
{
public:
    Server(boost::asio::io_context& ioCtx, unsigned short port)
        : acceptor_(ioCtx, tcp::endpoint(tcp::v4(), port))
    {
        doAccept();
    }

private:
    void doAccept()
    {
        // async_accept：异步等待新连接
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec)
                {
                    std::cout << "新连接: "
                              << socket.remote_endpoint() << "\n";
                    // 创建 Session 并启动读取循环
                    std::make_shared<Session>(std::move(socket))->start();
                }
                // 无论成功与否，继续等待下一个连接
                doAccept();
            });
    }

    tcp::acceptor acceptor_;
};

int main()
{
    boost::asio::io_context ioCtx;
    Server server(ioCtx, 9999);
    std::cout << "异步 Echo Server 监听端口 9999\n";
    ioCtx.run();
    return 0;
}
```

**关键设计要点**：
1. `enable_shared_from_this` 保证异步回调执行时对象仍存活
2. `async_read_some` → `async_write` → `async_read_some` 形成异步调用链
3. 所有连接的 I/O 在**同一个线程**中交替执行——靠事件驱动，不靠多线程

### 1.3 第三步：协程版（现代推荐写法）

```cpp
// echo_server_coroutine.cpp
// 编译: g++ -std=c++20 -fcoroutines echo_server_coroutine.cpp \
//        -lboost_system -lpthread -o echo

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;

// 处理单个连接：像写同步代码一样，但实际是异步的！
awaitable<void> echo(tcp::socket socket)
{
    try
    {
        char data[1024];
        for (;;)
        {
            // co_await：挂起协程，等待数据到来
            // 数据到来后，协程从这里恢复执行
            std::size_t n = co_await socket.async_read_some(
                boost::asio::buffer(data), use_awaitable);

            // co_await：挂起协程，等待数据发送完毕
            co_await async_write(socket,
                boost::asio::buffer(data, n), use_awaitable);
        }
    }
    catch (std::exception& e)
    {
        // EOF 或其他错误都走异常路径
        // 协程结束 → 栈上的 socket 自动析构 → 连接关闭
        std::cout << "连接结束: " << e.what() << "\n";
    }
}

// 接受连接的协程
awaitable<void> listener(tcp::acceptor acceptor)
{
    for (;;)
    {
        // co_await：挂起，等待新连接
        tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
        std::cout << "新连接: " << socket.remote_endpoint() << "\n";

        // co_spawn：启动独立协程处理这个连接
        // detached：不关心返回值，协程独立运行
        boost::asio::co_spawn(
            acceptor.get_executor(),
            echo(std::move(socket)),
            boost::asio::detached);
    }
}

int main()
{
    boost::asio::io_context ioCtx;

    tcp::acceptor acceptor(ioCtx, tcp::endpoint(tcp::v4(), 9999));
    std::cout << "协程 Echo Server 监听端口 9999\n";

    boost::asio::co_spawn(ioCtx, listener(std::move(acceptor)),
                          boost::asio::detached);
    ioCtx.run();
    return 0;
}
```

### 1.4 三种方式对比

| 对比项       | 同步版             | 回调版              | 协程版           |
| ------------ | ------------------ | ------------------- | ---------------- |
| 并发能力     | 无（一次一个连接） | 支持（事件驱动）    | 支持（事件驱动） |
| 代码行数     | ~30 行             | ~70 行              | ~40 行           |
| 生命周期管理 | 自动（栈变量）     | 手动 shared_ptr     | 自动（协程栈）   |
| 错误处理     | error_code / 异常  | error_code 参数判断 | try/catch        |
| 可读性       | 最好（但不能并发） | 差（回调嵌套）      | 好（线性流程）   |
| 性能         | —                  | 基准                | 接近基准         |
| 推荐程度     | 仅学习用           | 维护旧代码          | **新项目首选**   |

---

## 2. async_write vs async_write_some

这是新手常见的坑：

```cpp
// ❌ 错误：async_write_some 可能只发送了一部分数据
char bigData[65536];
socket.async_write_some(boost::asio::buffer(bigData),
    [](boost::system::error_code ec, size_t bytesWritten) {
        // bytesWritten 可能是 1024, 4096, 或其他任意值
        // 剩余的数据需要你自己循环发送！
    });

// ✅ 正确：async_write 保证发送全部数据（或出错）
boost::asio::async_write(socket, boost::asio::buffer(bigData),
    [](boost::system::error_code ec, size_t bytesWritten) {
        // 如果 !ec，则 bytesWritten == 65536
        // async_write 内部循环调用了 async_write_some
    });
```

**类比理解**：
- `async_write_some` 相当于 `send()` 系统调用——发多少算多少
- `async_write` 相当于一个循环 `send()` 直到全部发完

同理，`async_read` vs `async_read_some`：

```cpp
// async_read_some：有多少读多少
char buf[1024];
auto n = co_await socket.async_read_some(
    boost::asio::buffer(buf), use_awaitable);
// n 可能是 1 ~ 1024 之间的任意值

// async_read：精确读取 100 字节
char exact[100];
co_await boost::asio::async_read(
    socket, boost::asio::buffer(exact), use_awaitable);
// 要么读满 100 字节，要么抛异常（如 EOF）
```

---

## 3. 多线程模型

单线程 io_context 在大多数场景足够。但当连接数极多或 handler 中有少量计算时，需要多线程。

### 3.1 模型 A：单 io_context + 多线程 run()

```
┌──────────────────────────────────┐
│          io_context              │
│  ┌────────────────────────────┐  │
│  │       任务队列              │  │
│  └────────────────────────────┘  │
│                                  │
│  Thread 1: run()                 │
│  Thread 2: run()     ← 竞争取任务│
│  Thread 3: run()                 │
└──────────────────────────────────┘
```

```cpp
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    boost::asio::io_context ioCtx;
    auto guard = boost::asio::make_work_guard(ioCtx);

    // 投递任务
    for (int i = 0; i < 10; ++i)
    {
        boost::asio::post(ioCtx, [i]() {
            std::cout << "任务 " << i << " 在线程 "
                      << std::this_thread::get_id() << "\n";
        });
    }

    // 多个线程同时运行同一个 io_context
    // 任务会被不同线程抢占执行
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back([&ioCtx]() { ioCtx.run(); });
    }

    // 所有任务完成后，释放 guard 让线程退出
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    guard.reset();
    for (auto& t : threads) t.join();
    return 0;
}
```

**优点**：简单，负载自动均衡
**缺点**：同一个连接的多个 handler 可能在不同线程并发执行 → 需要 strand 保护

### 3.2 模型 B：多 io_context，每线程一个

```
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  io_context 1   │  │  io_context 2   │  │  io_context 3   │
│  Thread 1: run()│  │  Thread 2: run()│  │  Thread 3: run()│
│  [连接A, 连接D] │  │  [连接B, 连接E] │  │  [连接C, 连接F] │
└─────────────────┘  └─────────────────┘  └─────────────────┘
         ↑                    ↑                    ↑
         └────────────────────┴────────────────────┘
                     round-robin 分发新连接
```

```cpp
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <memory>

class IoContextPool
{
public:
    explicit IoContextPool(size_t poolSize)
        : nextIndex_(0)
    {
        for (size_t i = 0; i < poolSize; ++i)
        {
            // 创建独立的 io_context
            contexts_.emplace_back(std::make_unique<boost::asio::io_context>());

            // work_guard 防止 io_context 在空闲时退出
            guards_.emplace_back(
                boost::asio::make_work_guard(*contexts_.back()));
        }
    }

    // 启动所有线程
    void run()
    {
        for (auto& ctx : contexts_)
        {
            threads_.emplace_back([&ctx]() { ctx->run(); });
        }
    }

    // 停止所有线程
    void stop()
    {
        guards_.clear();  // 释放所有 work_guard
        for (auto& ctx : contexts_) ctx->stop();
        for (auto& t : threads_) t.join();
    }

    // round-robin 选择下一个 io_context
    boost::asio::io_context& getNext()
    {
        auto& ctx = *contexts_[nextIndex_];
        nextIndex_ = (nextIndex_ + 1) % contexts_.size();
        return ctx;
    }

private:
    std::vector<std::unique_ptr<boost::asio::io_context>> contexts_;
    std::vector<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> guards_;
    std::vector<std::thread> threads_;
    size_t nextIndex_;
};
```

**优点**：连接绑定到固定线程，无需 strand/锁
**缺点**：负载可能不均衡（某个线程的连接恰好都很忙）

### 3.3 两种模式对比

| 对比       | 单 io_context + strand  | 多 io_context        |
| ---------- | ----------------------- | -------------------- |
| 复杂度     | 需要正确使用 strand     | 架构稍复杂           |
| 锁/同步    | strand 序列化           | 无需（线程隔离）     |
| 负载均衡   | 自动（共享队列）        | 需要 round-robin     |
| 跨连接通信 | 直接（同一 io_context） | 需要 post 到目标线程 |
| 适用场景   | 连接之间有大量交互      | 连接相对独立         |

---

## 4. strand 详解

当多个线程调用 `io_context::run()` 时，handler 可能在任意线程中执行。如果两个 handler 访问同一个对象，就会有数据竞争。

`strand` 是 Asio 提供的**无锁序列化**机制——保证提交给它的任务**按顺序逐一执行**，即使来自不同线程：

### 4.1 基础用法

```cpp
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    boost::asio::io_context ioCtx;

    // 创建一个 strand
    auto strand = boost::asio::make_strand(ioCtx);

    int counter = 0;  // 共享变量

    // 通过 strand 提交 1000 个递增任务
    for (int i = 0; i < 1000; ++i)
    {
        // 通过 strand 提交 → 保证串行执行 → 无需加锁
        boost::asio::post(strand, [&counter]() {
            ++counter;
        });
    }

    // 4 个线程并发运行 io_context
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back([&ioCtx]() { ioCtx.run(); });
    }
    for (auto& t : threads) t.join();

    // counter 一定是 1000（strand 保证了串行）
    std::cout << "counter = " << counter << "\n";  // 1000
    return 0;
}
```

### 4.2 绑定 strand 到异步操作

在实际服务器中，每个连接绑定一个 strand：

```cpp
class Connection : public std::enable_shared_from_this<Connection>
{
public:
    Connection(boost::asio::io_context& ioCtx, tcp::socket socket)
        : strand_(boost::asio::make_strand(ioCtx))  // 每连接独立 strand
        , socket_(std::move(socket))
    {
    }

    // 可以从任意线程安全调用
    void send(const std::string& msg)
    {
        // 将发送操作投递到 strand 中
        boost::asio::post(strand_,
            [self = shared_from_this(), msg]() {
                self->doSend(msg);
            });
    }

private:
    void doRead()
    {
        // bind_executor：将 handler 绑定到 strand
        // 效果：读操作完成时，handler 一定在 strand 中执行
        socket_.async_read_some(
            boost::asio::buffer(readBuf_),
            boost::asio::bind_executor(strand_,
                [self = shared_from_this()](
                    boost::system::error_code ec, size_t n) {
                    if (!ec) {
                        self->processData(n);
                        self->doRead();
                    }
                }));
    }

    void doSend(const std::string& msg)
    {
        // 此函数一定在 strand 中执行
        // 可以安全访问 writeQueue_ 而无需加锁
        writeQueue_.push_back(msg);
        if (writeQueue_.size() == 1)
        {
            doWrite();
        }
    }

    void doWrite() { /* async_write 回调也绑定到 strand */ }
    void processData(size_t n) { /* ... */ }

    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    tcp::socket socket_;
    char readBuf_[4096];
    std::vector<std::string> writeQueue_;
};
```

### 4.3 strand 与协程的关系

在同一个协程内部，代码天然是串行的——`co_await` 挂起后恢复时，保证在同一个执行上下文中。所以协程内部通常**不需要显式 strand**。

但如果一个对象被多个协程共享，仍然需要同步：

```cpp
// 两个协程共享同一个 socket 的场景（如读写分离）
awaitable<void> reader(tcp::socket& socket)
{
    char buf[1024];
    for (;;)
    {
        auto n = co_await socket.async_read_some(
            boost::asio::buffer(buf), use_awaitable);
        // 处理读取的数据...
    }
}

awaitable<void> writer(tcp::socket& socket,
                       std::deque<std::string>& queue)
{
    // 这里 socket 被两个协程共享！
    // 如果在单线程 io_context 上则安全（同一时刻只有一个协程执行）
    // 如果在多线程 io_context 上则需要 strand
}
```

**经验法则**：如果使用多 io_context（每连接绑定到一个），就不需要 strand——这是最简单的模型。

---

## 5. 常见陷阱

### 5.1 陷阱：悬垂引用

```cpp
// ❌ 错误：buf 在 async_write 完成前可能已经被销毁
void send(tcp::socket& socket)
{
    std::string buf = "Hello";
    // async_write 立即返回，但内部持有 buf 的指针
    boost::asio::async_write(socket, boost::asio::buffer(buf),
        [](boost::system::error_code, size_t) {});
    // 函数退出 → buf 析构 → async_write 使用悬垂指针！
}

// ✅ 正确：用 shared_ptr 延长 buffer 的生命周期
void send(tcp::socket& socket)
{
    auto buf = std::make_shared<std::string>("Hello");
    boost::asio::async_write(socket, boost::asio::buffer(*buf),
        [buf](boost::system::error_code, size_t) {
            // buf 在回调中被捕获，生命周期足够长
        });
}

// ✅ 协程版：天然安全（栈变量在 co_await 期间仍然存活）
awaitable<void> send(tcp::socket& socket)
{
    std::string buf = "Hello";
    // co_await 挂起协程但不销毁栈帧，buf 安全
    co_await boost::asio::async_write(socket,
        boost::asio::buffer(buf), use_awaitable);
}
```

### 5.2 陷阱：忘记驱动 io_context

```cpp
// ❌ 错误：发起了异步操作但没调用 run()
void badExample()
{
    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    // ... connect ...
    socket.async_read_some(buffer, handler);
    // 忘记 ioCtx.run()！handler 永远不会执行
}
```

### 5.3 陷阱：错误的线程操作 socket

```cpp
// ❌ 错误：从非 io_context 线程直接操作 socket
void bad(tcp::socket& socket)
{
    std::thread t([&socket]() {
        char buf[1024];
        // 这个线程没有运行 io_context::run()
        // 直接操作 socket 是未定义行为！
        socket.async_read_some(boost::asio::buffer(buf),
            [](auto, auto) {});
    });
    t.join();
}

// ✅ 正确：通过 post 投递到 io_context 线程
void correct(boost::asio::io_context& ioCtx, tcp::socket& socket)
{
    std::thread t([&ioCtx, &socket]() {
        boost::asio::post(ioCtx, [&socket]() {
            // 这个 lambda 在 io_context::run() 线程中执行
            char buf[1024];
            socket.async_read_some(boost::asio::buffer(buf),
                [](auto, auto) {});
        });
    });
    t.join();
}
```

---

## 本篇小结

| 概念                            | 要点                               |
| ------------------------------- | ---------------------------------- |
| 同步 → 异步 → 协程              | 三步演进，协程是现代首选           |
| enable_shared_from_this         | 回调版保活对象的标准做法           |
| async_write vs async_write_some | 前者保证全部写完，后者只写一部分   |
| 单 io_context 多线程            | 简单但需要 strand                  |
| 多 io_context                   | 线程隔离，无需同步                 |
| strand                          | 无锁序列化，保证 handler 不并发    |
| 悬垂引用                        | 异步编程最常见的 bug，协程自然规避 |

下一篇 [实战篇]({{< relref "posts/深入学习Boost.Asio_实战篇.md" >}}) 将进入协程进阶技巧、完整聊天室项目和性能调优。
