+++
title = '深入学习 Boost.Asio（三）：协程进阶与实战项目'
date = '2026-05-21'
draft = false
tags = ["Boost", "Boost.Asio", "C++", "协程", "C++20", "网络编程", "性能优化"]
categories = ["Boost学习课程"]
description = "深入 C++20 协程与 Asio 的结合：超时控制、并发 awaitable、完整聊天室实战项目、性能调优要点与最佳实践总结。"
+++

> **系列导航**：[入门篇]({{< relref "posts/深入学习Boost.Asio_入门篇.md" >}}) | [进阶篇]({{< relref "posts/深入学习Boost.Asio_进阶篇.md" >}}) | **实战篇**

## 前置知识

阅读本篇前，请确保已掌握：
- [入门篇]({{< relref "posts/深入学习Boost.Asio_入门篇.md" >}})：io_context、异步操作生命周期、定时器
- [进阶篇]({{< relref "posts/深入学习Boost.Asio_进阶篇.md" >}})：协程 Echo Server、多线程模型、strand

---

## 1. 协程进阶技巧

### 1.1 co_spawn 的第三个参数

`co_spawn` 的第三个参数决定了协程完成后的行为：

```cpp
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

using boost::asio::awaitable;
using boost::asio::use_awaitable;

awaitable<int> compute() { co_return 42; }
awaitable<void> mayFail() { throw std::runtime_error("oops"); co_return; }

void examples(boost::asio::io_context& ioCtx)
{
    // 方式1：detached —— 忽略返回值和异常
    // 适用：独立运行的协程（如连接处理）
    boost::asio::co_spawn(ioCtx, compute(), boost::asio::detached);

    // 方式2：回调 —— 协程完成时执行回调
    // 适用：需要捕获协程异常或获取返回值
    boost::asio::co_spawn(ioCtx, mayFail(),
        [](std::exception_ptr e) {
            if (e) {
                try { std::rethrow_exception(e); }
                catch (const std::exception& ex) {
                    std::cerr << "协程异常: " << ex.what() << "\n";
                }
            }
        });

    // 方式3：use_awaitable —— 在协程中等待另一个协程
    // 适用：父子协程关系
    // （需要在协程内使用）
}

// 方式3 完整示例
awaitable<void> parent(boost::asio::io_context& ioCtx)
{
    // 等待子协程完成并获取返回值
    int result = co_await boost::asio::co_spawn(
        ioCtx, compute(), boost::asio::use_awaitable);
    std::cout << "子协程返回: " << result << "\n";  // 42
}
```

### 1.2 超时控制

生产环境中，你不能无限等待一个操作完成。Asio 提供了 `awaitable_operators` 实现竞争式等待：

```cpp
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <iostream>
#include <chrono>
#include <variant>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;
// 引入 || 和 && 运算符
using namespace boost::asio::experimental::awaitable_operators;

// 带超时的读操作
awaitable<size_t> readWithTimeout(tcp::socket& socket,
                                  char* buf, size_t size,
                                  std::chrono::seconds timeout)
{
    // 获取当前协程的执行器
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    timer.expires_after(timeout);

    // || 运算符：两个 awaitable 同时运行
    // 哪个先完成就返回哪个的结果，另一个自动取消
    auto result = co_await (
        socket.async_read_some(
            boost::asio::buffer(buf, size), use_awaitable)
        || timer.async_wait(use_awaitable)
    );

    // result 类型是 std::variant<size_t, std::monostate>
    // index 0 = read 先完成（返回读取字节数）
    // index 1 = timer 先完成（超时了）
    if (result.index() == 1)
    {
        // 超时：取消 socket 上正在进行的操作
        socket.cancel();
        throw std::runtime_error("读取超时");
    }

    co_return std::get<0>(result);
}

// 使用示例
awaitable<void> handleConnection(tcp::socket socket)
{
    try
    {
        char buf[1024];
        for (;;)
        {
            // 每次读取最多等 30 秒
            size_t n = co_await readWithTimeout(
                socket, buf, sizeof(buf), std::chrono::seconds(30));

            co_await boost::asio::async_write(
                socket, boost::asio::buffer(buf, n), use_awaitable);
        }
    }
    catch (const std::runtime_error& e)
    {
        std::cout << "连接关闭: " << e.what() << "\n";
    }
    catch (const boost::system::system_error& e)
    {
        if (e.code() != boost::asio::error::eof)
            std::cerr << "错误: " << e.what() << "\n";
    }
}
```

### 1.3 并发执行多个协程

使用 `&&` 运算符让多个异步操作并行执行：

```cpp
using namespace boost::asio::experimental::awaitable_operators;

// 模拟两个耗时的异步操作
awaitable<std::string> fetchUserProfile(int userId)
{
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(100));  // 模拟延迟
    co_await timer.async_wait(use_awaitable);
    co_return "User_" + std::to_string(userId);
}

awaitable<std::vector<int>> fetchUserOrders(int userId)
{
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::milliseconds(150));  // 模拟延迟
    co_await timer.async_wait(use_awaitable);
    co_return std::vector<int>{1001, 1002, 1003};
}

// 并发获取用户信息和订单
awaitable<void> getUserData(int userId)
{
    // && 运算符：两个操作并发执行，都完成后才继续
    // 总耗时 = max(100ms, 150ms) = 150ms，而非 250ms
    auto [profile, orders] = co_await (
        fetchUserProfile(userId) && fetchUserOrders(userId)
    );

    std::cout << "用户: " << profile
              << ", 订单数: " << orders.size() << "\n";
}
```

**运算符对比**：

| 运算符 | 语义     | 完成条件   | 返回类型      |
| ------ | -------- | ---------- | ------------- |
| `      |          | `          | 任一完成      | 第一个完成的 | `variant<A, B>` |
| `&&`   | 全部完成 | 两个都完成 | `tuple<A, B>` |

### 1.4 协程中的优雅取消

```cpp
awaitable<void> longRunningTask()
{
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);

    for (int i = 0; i < 100; ++i)
    {
        try
        {
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(use_awaitable);
            std::cout << "步骤 " << i << " 完成\n";
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == boost::asio::error::operation_aborted)
            {
                std::cout << "任务被取消，执行清理...\n";
                // 清理资源
                co_return;  // 优雅退出
            }
            throw;  // 其他错误继续抛出
        }
    }
}
```

---

## 2. 实战项目：多人聊天室

综合所有知识点，构建一个完整的协程式多人聊天室：

```cpp
// chat_server.cpp
// 编译: g++ -std=c++20 -fcoroutines chat_server.cpp \
//        -lboost_system -lpthread -o chat
// 测试: 多个终端分别 nc localhost 8888，输入的消息会广播给所有人

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/streambuf.hpp>
#include <iostream>
#include <set>
#include <memory>
#include <deque>
#include <string>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::use_awaitable;

// ========================================
// 前置声明
// ========================================
class ChatSession;

// ========================================
// 聊天室：管理所有在线用户
// ========================================
class ChatRoom
{
public:
    // 新用户加入
    void join(std::shared_ptr<ChatSession> session)
    {
        sessions_.insert(session);
        std::cout << "[系统] 用户加入，当前在线: "
                  << sessions_.size() << "\n";
    }

    // 用户离开
    void leave(std::shared_ptr<ChatSession> session)
    {
        sessions_.erase(session);
        std::cout << "[系统] 用户离开，当前在线: "
                  << sessions_.size() << "\n";
    }

    // 广播消息给所有用户（除了发送者）
    void broadcast(const std::string& msg,
                   std::shared_ptr<ChatSession> sender);

private:
    std::set<std::shared_ptr<ChatSession>> sessions_;
};

// ========================================
// 聊天会话：处理单个用户的收发
// ========================================
class ChatSession : public std::enable_shared_from_this<ChatSession>
{
public:
    ChatSession(tcp::socket socket, ChatRoom& room)
        : socket_(std::move(socket))
        , room_(room)
        , writeTimer_(socket_.get_executor())
    {
    }

    void start()
    {
        room_.join(shared_from_this());

        // 同时启动读协程和写协程
        // 它们并发运行：读协程接收用户输入，写协程发送广播消息
        boost::asio::co_spawn(
            socket_.get_executor(),
            [self = shared_from_this()]() { return self->readerLoop(); },
            boost::asio::detached);

        boost::asio::co_spawn(
            socket_.get_executor(),
            [self = shared_from_this()]() { return self->writerLoop(); },
            boost::asio::detached);
    }

    // 将消息放入发送队列
    void deliver(const std::string& msg)
    {
        writeQueue_.push_back(msg);
        // 唤醒 writer 协程：通过取消 timer 的等待
        writeTimer_.cancel_one();
    }

private:
    // 读协程：持续读取用户输入，按行分割
    awaitable<void> readerLoop()
    {
        try
        {
            boost::asio::streambuf buf;
            for (;;)
            {
                // async_read_until：读取直到遇到 '\n'
                // 返回值 n 包含了分隔符的长度
                auto n = co_await boost::asio::async_read_until(
                    socket_, buf, '\n', use_awaitable);

                // 从 streambuf 提取一行内容
                std::string line(
                    boost::asio::buffers_begin(buf.data()),
                    boost::asio::buffers_begin(buf.data()) + n);
                buf.consume(n);  // 消耗已读取的数据

                // 广播给聊天室中的其他人
                room_.broadcast(line, shared_from_this());
            }
        }
        catch (std::exception&)
        {
            // 连接断开或出错 → 退出聊天室
            stop();
        }
    }

    // 写协程：等待并发送队列中的消息
    awaitable<void> writerLoop()
    {
        try
        {
            // 设置一个"永不到期"的定时器作为通知机制
            // 当有新消息时，deliver() 会 cancel_one() 来唤醒这个等待
            while (socket_.is_open())
            {
                if (writeQueue_.empty())
                {
                    // 队列为空，等待通知
                    writeTimer_.expires_at(
                        boost::asio::steady_timer::time_point::max());

                    // redirect_error：将异常转为 error_code
                    // 这样 cancel 不会抛异常，而是返回 operation_aborted
                    boost::system::error_code ec;
                    co_await writeTimer_.async_wait(
                        boost::asio::redirect_error(use_awaitable, ec));
                    // ec == operation_aborted 表示被 deliver() 唤醒
                }

                // 发送队列中的所有消息
                while (!writeQueue_.empty())
                {
                    co_await boost::asio::async_write(
                        socket_,
                        boost::asio::buffer(writeQueue_.front()),
                        use_awaitable);
                    writeQueue_.pop_front();
                }
            }
        }
        catch (std::exception&)
        {
            stop();
        }
    }

    void stop()
    {
        room_.leave(shared_from_this());
        boost::system::error_code ec;
        socket_.close(ec);
        writeTimer_.cancel();
    }

    tcp::socket socket_;
    ChatRoom& room_;
    boost::asio::steady_timer writeTimer_;
    std::deque<std::string> writeQueue_;
};

// ChatRoom::broadcast 实现
void ChatRoom::broadcast(const std::string& msg,
                         std::shared_ptr<ChatSession> sender)
{
    for (auto& session : sessions_)
    {
        if (session != sender)  // 不发给发送者自己
        {
            session->deliver(msg);
        }
    }
}

// ========================================
// 监听协程：接受新连接
// ========================================
awaitable<void> listener(tcp::acceptor acceptor, ChatRoom& room)
{
    for (;;)
    {
        // 等待新连接
        tcp::socket socket = co_await acceptor.async_accept(use_awaitable);

        std::cout << "[系统] 新连接: " << socket.remote_endpoint() << "\n";

        // 创建会话并启动
        std::make_shared<ChatSession>(std::move(socket), room)->start();
    }
}

// ========================================
// 主函数
// ========================================
int main()
{
    try
    {
        boost::asio::io_context ioCtx;
        ChatRoom room;

        tcp::acceptor acceptor(ioCtx, tcp::endpoint(tcp::v4(), 8888));
        std::cout << "=== 聊天室服务器启动 ===\n";
        std::cout << "监听端口: 8888\n";
        std::cout << "使用 nc localhost 8888 或 telnet localhost 8888 连接\n";
        std::cout << "========================\n\n";

        boost::asio::co_spawn(ioCtx,
            listener(std::move(acceptor), room),
            boost::asio::detached);

        // 优雅关闭：Ctrl+C 停止服务器
        boost::asio::signal_set signals(ioCtx, SIGINT, SIGTERM);
        signals.async_wait([&ioCtx](auto, auto) {
            std::cout << "\n[系统] 收到停止信号，关闭服务器\n";
            ioCtx.stop();
        });

        ioCtx.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "致命错误: " << e.what() << "\n";
    }
    return 0;
}
```

**这个项目综合展示了**：

| 知识点                | 在代码中的体现                  |
| --------------------- | ------------------------------- |
| co_spawn              | 启动 reader/writer 两个并发协程 |
| async_read_until      | 按行读取用户输入                |
| steady_timer 作为通知 | writer 协程等待新消息的信号机制 |
| redirect_error        | 避免 timer cancel 抛异常        |
| shared_from_this      | 管理 Session 的异步生命周期     |
| signal_set            | Ctrl+C 优雅关闭                 |
| streambuf             | 流式 buffer，适合按分隔符读取   |

---

## 3. 性能调优要点

### 3.1 减少内存分配

```cpp
// ❌ 每次读取都分配新 buffer
awaitable<void> wasteful(tcp::socket& socket)
{
    for (;;)
    {
        auto buf = std::make_unique<char[]>(4096);  // 每次 new
        auto n = co_await socket.async_read_some(
            boost::asio::buffer(buf.get(), 4096), use_awaitable);
        process(buf.get(), n);
    }  // 每次 delete
}

// ✅ 复用 buffer
awaitable<void> efficient(tcp::socket& socket)
{
    char buf[4096];  // 栈上分配一次，循环复用
    for (;;)
    {
        auto n = co_await socket.async_read_some(
            boost::asio::buffer(buf), use_awaitable);
        process(buf, n);
    }
}
```

### 3.2 gather write（减少系统调用）

```cpp
// ❌ 多条消息 → 多次 write 系统调用
for (const auto& msg : messages)
{
    co_await boost::asio::async_write(
        socket, boost::asio::buffer(msg), use_awaitable);
}
// 如果有 10 条消息 → 10 次 write 系统调用

// ✅ 合并成一次 scatter/gather write
std::vector<boost::asio::const_buffer> buffers;
for (const auto& msg : messages)
{
    buffers.push_back(boost::asio::buffer(msg));
}
// 一次系统调用，内核使用 writev() 发送多个 buffer
co_await boost::asio::async_write(socket, buffers, use_awaitable);
```

**原理**：Linux 的 `writev()` / Windows 的 `WSASend()` 支持一次系统调用发送多个不连续的内存块。Asio 的 `async_write` 接受 `BufferSequence`（如 `vector<const_buffer>`）时会自动利用这个特性。

### 3.3 合理的线程数

```
经验法则：
┌─────────────────────────────────────────────────┐
│ 场景                          │ 推荐线程数       │
├─────────────────────────────────────────────────┤
│ 纯 I/O 密集（网络代理）      │ CPU 核心数       │
│ I/O + 少量计算（游戏服务器）  │ CPU 核心数 × 1.5 │
│ 大量慢速长连接（WebSocket）   │ 1~2 线程 + 协程  │
│ 混合 CPU 密集 + I/O          │ I/O 线程 + 计算池│
└─────────────────────────────────────────────────┘
```

**实测建议**：先用 1 个线程跑，只在 profiling 证明 io_context 是瓶颈时才加线程。过早多线程只会增加复杂性。

### 3.4 避免在 handler 中阻塞

```cpp
// ❌ handler 中做 CPU 密集计算 → 阻塞整个事件循环
awaitable<void> bad(tcp::socket& socket)
{
    char buf[4096];
    auto n = co_await socket.async_read_some(
        boost::asio::buffer(buf), use_awaitable);

    // 假设这是一个耗时 500ms 的加密操作
    auto encrypted = heavyEncrypt(buf, n);  // 阻塞 500ms！
    // 在这 500ms 内，同一 io_context 上的所有其他连接都无法处理

    co_await boost::asio::async_write(
        socket, boost::asio::buffer(encrypted), use_awaitable);
}

// ✅ 将 CPU 密集任务投递到独立线程池
awaitable<void> good(tcp::socket& socket,
                     boost::asio::thread_pool& computePool)
{
    char buf[4096];
    auto n = co_await socket.async_read_some(
        boost::asio::buffer(buf), use_awaitable);

    // 将加密操作投递到线程池，co_await 等待完成
    // 期间事件循环继续处理其他连接
    auto encrypted = co_await boost::asio::co_spawn(
        computePool,
        [&]() -> awaitable<std::string> {
            co_return heavyEncrypt(buf, n);
        },
        use_awaitable);

    co_await boost::asio::async_write(
        socket, boost::asio::buffer(encrypted), use_awaitable);
}
```

---

## 4. 最佳实践总结

| 实践                                     | 说明                                                   |
| ---------------------------------------- | ------------------------------------------------------ |
| 使用协程                                 | C++20 协程让异步代码像同步一样直观，且自动管理生命周期 |
| 用 `async_write` 而非 `async_write_some` | 前者保证全部写完                                       |
| 延长 buffer 生命周期                     | 协程栈变量自然安全，回调版用 shared_ptr                |
| handler 中不做耗时操作                   | 会阻塞整个事件循环，用线程池卸载                       |
| 每连接绑定一个线程（或 strand）          | 最简单的线程安全方案                                   |
| 检查 error_code / 捕获异常               | 特别注意 EOF 和 operation_aborted                      |
| 用 work_guard 保持服务器运行             | 防止 run() 在空闲时退出                                |
| 复用 buffer                              | 减少堆分配开销                                         |
| gather write                             | 合并多条消息为一次系统调用                             |
| 先单线程，瓶颈时再加                     | 过早多线程 = 过早优化                                  |

---

## 5. 完整知识图谱

```
Boost.Asio 知识体系
│
├── 基础层（入门篇）
│   ├── Proactor 模式
│   ├── io_context（run/poll/stop）
│   ├── work_guard
│   ├── 异步操作生命周期
│   ├── steady_timer
│   └── post / dispatch
│
├── 网络层（进阶篇）
│   ├── TCP：acceptor / socket
│   ├── 同步 → 回调 → 协程 演进
│   ├── async_read / async_write
│   ├── 多线程模型
│   ├── strand 序列化
│   └── 生命周期管理
│
└── 实战层（实战篇）
    ├── co_spawn 完成模式
    ├── 超时控制（|| 运算符）
    ├── 并发 awaitable（&& 运算符）
    ├── 聊天室完整项目
    └── 性能调优
```

---

## 进一步学习

掌握本系列后，推荐继续学习：

- **Boost.Beast**：在 Asio 之上构建 HTTP/WebSocket 协议栈
- **Boost.MySQL**：异步数据库访问（底层用 Asio）
- **Asio 的 cancellation slot**：更精细的取消控制（Boost 1.82+）
- **io_uring 后端**：Linux 5.1+ 的真正异步 I/O（Asio 实验性支持）
