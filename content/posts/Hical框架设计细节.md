+++
title: "我用现代 C++ 写了个 Web 框架，这 25 个设计细节让性能拉满"
date = '2026-05-30'
draft = false
tags = ["C++20", "C++26", "框架设计", "Hical"]
categories = ["Hical框架"]
description = "分享 Hical 框架开发中的一些设计细节。"
+++

## 写在前面

两万多行代码写下来，我把踩过的坑和想明白的事都总结在这了。不讲概念，只聊实战中每个设计"为什么这么做"。

搞 C++ Web 框架这事，从第一行代码到现在，说实话走了不少弯路。今天不聊怎么用，聊聊底层那些设计决策——为什么要这么写，当时碰到了什么问题，怎么一步步改成现在这样的。

---

## 一、编译期就把活干完

### 1. 一套模板搞定 TCP 和 SSL

最早我是分别写了 `TcpConnection` 和 `SslConnection` 两个类，代码重复率 80%以上，改一个 bug 要改两遍。后来换成 `GenericConnection<SocketType>`，内部用 `if constexpr` 做分支：

```cpp
if constexpr (hIsSslStream<SocketType>) {
    co_await stream_.async_handshake(...);
}
```

编译器直接在编译期把不匹配的分支丢掉——纯 TCP 的二进制里**一个字节的 SSL 代码都没有**。维护成本砍半，运行时零开销。这不是什么高深技巧，但真正落地用好的项目不多。

### 2. 用不到的模块，连编译都不参与

数据库、OpenAPI 这些模块，不是每个项目都用得上。我用 CMake option + 宏隔离的方式处理：

```cmake
option(HICAL_WITH_DATABASE "Enable DB middleware" OFF)
```

代码里所有 DB 相关的逻辑都包在 `#ifdef HICAL_HAS_DATABASE` 里。你不开这个 option，这些代码连语法检查都不走，更别说占二进制体积了。比运行时搞个 feature flag 干净太多——运行时 flag 意味着代码虽然不执行，但死代码还在那占着 icache。

### 3. TRACE 日志：Release 下彻底消失

开发时满屏 TRACE 日志方便调试，但生产环境绝对不能有这些开销。我的做法是 `NDEBUG` 下 `HICAL_LOG_TRACE` 直接展开成 `((void)0)`——注意，这不是"判断级别后跳过"，而是**连参数求值的代码都不存在于二进制中**。

你写 `HICAL_LOG_TRACE("cost={}ms", expensive_calc())`，Release 下 `expensive_calc()` 这个调用本身就不会出现在汇编里。其他级别靠 `atomic load` 短路，不命中时开销不到 1 纳秒。

---

## 二、内存分配：能不 malloc 就不 malloc

### 4. 三级内存池，各管各的

这是我觉得框架里最值得细说的设计。内存分配分三层：

| 层级 | 谁的生命周期 | 策略 |
|------|-------------|------|
| 全局池 | 进程级 | `synchronized_pool_resource`，线程安全 |
| 线程本地池 | 线程级 | `unsynchronized_pool_resource`，无锁 |
| 请求级缓冲 | 单次请求 | `monotonic_buffer_resource`，只进不出 |

关键细节在于：请求级缓冲的上游指向线程本地池而不是全局池。这意味着即使请求级缓冲需要扩容，走的也是无锁路径。

另一个容易出问题的点是 GC。`unsynchronized_pool_resource` 不是线程安全的，如果 GC 线程直接去调 `release()` 就是 UB。我的做法是 GC 线程只设一个 `needsRelease` 标志，让拥有这个池的线程自己去做清理。听起来简单，但真踩过坑才知道多重要。

### 5. 栈上搞定 99% 的响应头序列化

HTTP 响应头序列化用 `FixedBuffer<512>`——512 字节直接在栈上分配。实测绝大多数响应头总长不超过 300 字节，栈缓冲完全够用。只有极端情况（一堆 Set-Cookie 之类的）才回退到堆分配。

数字转字符串用 `std::to_chars` 而不是 `snprintf`，既避免了 locale 带来的额外开销，也省了格式字符串解析。单次省的不多，但请求量上去之后就能在火焰图上看到差别。

---

## 三、无锁并发：把锁从热路径上赶走

### 6. 写队列用 Vyukov MPSC 无锁队列

这是整个网络层的核心数据结构。发送数据时，多个协程往队列里 push（生产者），写循环从队列里 pop 出来写 socket（消费者）。

为什么不用 mutex + queue？因为在高并发下，mutex 竞争是肉眼可见的性能瓶颈。这个队列最早是 Dmitry Vyukov 提出的，我读了他的设计之后觉得非常适合这个场景——push 只需要一次原子 `exchange`，wait-free，任何情况下都是 O(1)。消费者是单线程 pop，根本没有竞争。

`tail_` 指针用 `alignas(64)` 独占一条缓存行，消除 false sharing。这个做法在无锁编程圈子里是基本功了，但不做的话相邻变量的写入会无谓地让其他核的缓存行失效，多核下性能差距很明显。

### 7. 节点分配也不 malloc

有了无锁队列，节点分配就成了新的瓶颈。每次 push 都 `new MpscNode` 的话，高并发下 malloc 的锁竞争又回来了。

解决方案很直接：每个线程维护一个 free list，最多缓存 128 个节点。send 完消费掉的节点回收到 free list，下次 send 直接复用。在典型的"IO 线程 send + 同线程 writeLoop 消费"场景下，节点在同一个线程上分配和释放，完全是 O(1) 的用户态操作，**零 malloc/free**。

这个思路受 io_uring 的影响不小——io_uring 的核心理念就是预注册缓冲区、批量提交、尽量减少内核/用户态切换。我虽然底层还是用的 epoll/IOCP（跨平台考虑），但"预分配 + 批量复用 + 减少 syscall"这套思路贯穿了整个写队列和内存池的设计。后续写队列的 `kMaxDrainBatch = 256` 单轮最多取 256 个节点也是类似的想法——攒一批一起处理，比一个一个来高效。

128 这个上限是拍脑袋定的，没做过严格 benchmark。理论上应该根据典型负载下的并发 send 峰值来定，但实际跑下来 128 够用了，没动力去精确调优。

### 8. 写入数据分快慢路径

发送消息时，90% 以上是纯内存数据（比如 JSON 响应），少量是文件传输。我用 tagged union 区分这两种情况：

- **内存数据**：直接存 `shared_ptr<string>`，零虚函数调用
- **文件节点**：存 `shared_ptr<WriteNode>`，走多态

大多数请求走的是内存快速路径，连虚函数表查找都省了。只有真正需要 sendfile 的场景才走慢路径。想法很朴素：把常见的情况做到最快，不常见的保证正确就行。

### 9. Session 时间戳用原子变量

Session 的最后访问时间更新是个典型的高频操作——每个请求都要 touch 一次。最初我用 mutex 保护，后来发现一个隐蔽的问题：如果 `SessionManager` 在持有自己的锁时去 touch 某个 Session，就会产生 `manager.mutex_` → `session.mutex_` 的嵌套锁，潜在死锁风险。

换成 `atomic<int64_t>` 后，问题从根本上消失了。时间戳和 Session 数据之间本来就不需要事务一致性——你不需要"在改数据的同时原子地更新时间戳"，那原子变量就是最自然的选择。

---

## 四、零拷贝：数据能不动就不动

### 10. HTTP 解析全程零堆分配

HTTP 头解析用的是 h2o 项目里的 picohttpparser（纯 C 实现，就几百行代码但快得离谱），解析结果存在栈上的 `phr_header[64]` 数组里。`NativeRequest` 里的 URL 和头部字段全是 `string_view`，直接指向连接级的读缓冲区——**不拷贝，不分配，不构造 string**。选 picohttpparser 是看了 TechEmpower 榜单上前几名的 C++ 框架（drogon、cinatra）都在用它，属于这个圈子里的事实标准了。

body 为什么没做零拷贝？因为 body 可能跨越多次 socket read，不得不拷贝拼接。但头部和 URL 一定在一次 read 的缓冲区里，零拷贝是安全的。我做了延迟 memmove 设计，确保 string_view 在路由分发完成之前始终有效。

### 11. 响应通用头只拼一次

每个 HTTP 响应都有 `Server`、`Connection`、`Date` 这三个头，内容大同小异。我在连接建立时就把这三个头的 wire bytes（大约 90 字节）预拼好存在连接对象上，后续每个请求直接 `memcpy` 过去。

Date 头稍微特殊——它每秒会变。我用线程本地的 `DateCache`，每秒最多更新一次（也就是 29 字节的 memcpy）。这样每个请求省掉了 3 次 `HeaderMap::insert` + 序列化循环。wrk 压满 CPU 的场景下，火焰图上这块的占比明显缩小了。

### 12. 路由查找不创建临时 string

路由表用哈希表存，查找时最怕的就是为了凑 key 去构造一个临时 `std::string`——每次请求都 new 一次就太浪费了。我声明了 `is_transparent` 透明哈希，`find()` 直接拿 `string_view` 做查找，零临时对象。

参数路由（`/user/{id}` 这种）的匹配也全程用 `string_view`，只有最终提取参数值交给用户 handler 时才分配 string。

---

## 五、协程的正确打开方式

### 13. 用定时器当协程信号量

数据库连接池需要一个"等连接"的机制。传统做法是 `condition_variable`，但协程世界里 `cv.wait()` 会阻塞整个线程，其他协程全部被卡住。

这是 Asio 协程编程中比较经典的手法——用 `steady_timer` 做信号量：

```cpp
// 等待：用 acquireTimeout 作为超时，兼具"信号量"和"超时保护"双重作用
auto timer = std::make_shared<steady_timer>(ioCtx, acquireTimeout);
co_await timer->async_wait(redirect_error(use_awaitable, ec));
// 协程挂起，线程不阻塞；超时自然到期 or 被 cancel 唤醒

// 唤醒：归还连接时取消等待者的定时器
waiter.timer->cancel();  // 等待中的协程被唤醒，拿到连接
```

协程挂起期间，io_context 照常跑别的协程。相比"永远不到期 + cancel 唤醒"的纯信号量模式，直接用超时定时器省掉了单独的超时逻辑，一个 timer 同时解决等待和超时两个问题。

这个手法是从 Chris Kohlhoff 的 Asio 官方示例里学来的——`asio/src/examples/cpp20/primitives/` 目录下有专门的 `semaphore.cpp` 和 `condition_variable.cpp`，用的就是 timer + cancel 模式。我在上面改了一下，直接用业务超时作为 timer 的到期时间，省掉了单独的超时管理。

### 14. coSpawn：一个封装解决两个痛点

Asio 默认的 `co_spawn` 配合 `detached` 用，异常直接被吞掉，生产环境出了 bug 连日志都没有。我封装了自己的 `coSpawn()`，同时做两件事：

1. **异常必记录**：用 `logOnException` 替代 `detached`，异常至少会输出到日志
2. **复用 handler 内存**：`bind_allocator(recycling_allocator<void>(), ...)` 利用 thread_local 缓存复用 completion handler 的内存

高并发下每秒可能有几万次 `co_spawn`，每次都 malloc/free 一个 handler 对象的话，分配器压力不小。`recycling_allocator` 是 Asio 作者 Chris Kohlhoff 自己写的，利用 thread_local 缓存让同一个线程上的连续 spawn 复用同一块内存，基本消除了这个开销。我只是把它和 `logOnException` 组合封装到了一起——没有什么原创性，纯粹是把 Asio 文档里散落的最佳实践组合成一个方便的工具函数。

### 15. 优雅关停：不要用 io_context::stop()

这是一个血的教训。最初我在关停时调 `io_context::stop()`，结果 Windows IOCP 上偶发崩溃——因为 `stop()` 会立即中断事件循环，协程帧在 `co_await` 中间被强行析构，IOCP 的两阶段析构机制导致 double-free。

正确做法是重置 `work_guard` 让 io_context 自然排空：所有 pending 的协程正常跑完退出，RAII 守卫正常析构。配合一个 `draining_` 标志让 keep-alive 连接不再保持新请求，整个关停过程平滑无崩溃。

这个 bug 前后改了三版才彻底修干净。第一版只是去掉 `stop()`，但忘了处理 IdleScanner 的定时器还在跑，io_context 永远排不空。第二版加了 `shutdown()` 但调用顺序不对，析构时 timer_service 已经没了。最终版是在 `~HttpServer()` 体内先 shutdown 所有 scanner 再析构 io_context。Windows IOCP 的坑不踩不知道深。

---

## 六、编译时间也是成本

### 16. 800 行模板不让你重编

`GenericConnection` 的模板实现有 800 多行，如果放在头文件里，每个 include 它的翻译单元都要编译一次——改个 Router 的代码，GenericConnection 也要跟着重编，纯浪费。

我把实现提取到 `.hci` 文件，用 `extern template` 声明抑制隐式实例化，只在一个 `.cpp` 里做显式实例化。效果是：用户代码 include 头文件时只看到声明，不触发模板编译。改 Router、Middleware 都不会连带重编 GenericConnection，增量编译时间显著缩短。

### 17. 错误处理函数从模板中剥离

`HICAL_JSON` 宏会为每个用户结构体生成序列化代码，其中包含类型不匹配、字段缺失等错误处理。如果错误处理也是模板代码的一部分，每个结构体都会实例化一份完全相同的错误处理逻辑。

我把 `throwTypeMismatch`、`throwMissingField` 这些函数抽成 `[[noreturn]]` 非模板函数。20 个结构体使用 `HICAL_JSON`，错误处理代码只存在一份，而不是 20 份。减少了 icache 压力，也缩小了二进制体积。

### 18. 重量级 C 库隔离在一个文件里

picohttpparser 是 C 库，WebSocket 帧处理逻辑也不轻。我把这些全部隔离在 `HttpSessionImpl.cpp` 一个翻译单元里。你改 `HttpServer.h` 上的配置接口、中间件管理之类的东西，不会触发这 1200 行的重编译。

这是"编译防火墙"思想的非模板版应用——不是只有模板才需要编译隔离。

---

## 七、中间件：能省的开销都省了

### 19. 连续同步中间件只分配一次

洋葱模型中间件每层都要一个协程帧，N 层中间件 = N 次堆分配。但实际上很多中间件根本不需要 `co_await`——比如 CORS 加个头、日志记个时间，纯同步操作。

`buildOptimizedChain()` 识别连续的同步中间件，合并进同一个协程帧。你挂了 CORS + Log + Auth 三个同步中间件，只产生 1 次堆分配而非 3 次。中间件越多，省得越明显。

### 20. 路由分组：编译时组装，运行时零成本

路由分组 `RouteGroup` 是纯值对象，子组继承父组的中间件配置。注册路由时，组级中间件通过 `buildChainFrom()` 直接包装进 handler lambda 里——到了运行时，Router 拿到的就是一个完整的 handler，不知道也不关心有没有分组。零额外查找，零额外开销。

### 21. 数据库查询日志：不侵入，可插拔

查询日志不修改真实的数据库连接实现，而是包一层 `LoggingDbConnection` 代理，拦截 query/execute 调用并计时。请求结束后批量上报。

不注册这个中间件？零开销——连代理对象都不存在。注册了？也就是多一层函数调用的事。想加慢查询告警？配个 `slowQueryThreshold` 回调就行。这种装饰器模式的好处是完全可逆——随时加上，随时拆掉。

---

## 八、反射：今天能用，明天能升级

### 22. 同一套 API，两种底层实现

C++26 的原生反射（P2996）很香，但大多数项目还在用 C++20。我的做法是提供统一的 `meta::toJson()` API，底层自动适配：

- **C++26 编译器**：用 `^^T` + `nonstatic_data_members_of` 自动发现字段，零注解
- **C++20 编译器**：用 `HICAL_JSON(Type, field1, field2, ...)` 宏手动注册

等你的编译器升级到支持 C++26 了，`HICAL_JSON` 宏变成空操作，用户代码一行不改。这种渐进式迁移比"等 C++26 普及了再说"要务实得多。

### 23. 在预处理器的极限上跳舞

`ALIAS(field, "key")`、`REQUIRED(field)` 这些装饰器语法，在 C 预处理器的能力范围内实现了类似模式匹配的效果。用括号元组编码 tag 类型，`IS_PAREN` 宏检测区分裸字段名和装饰器，然后 tag 前缀分派到不同的处理器。`__VA_OPT__` 递归展开让字段数量没有上限。

这部分宏写得挺费脑子的，调试基本靠 `gcc -E` 一层层展开看。如果现在让我重新选，可能会考虑用 libclang 做代码生成而不是硬推预处理器的极限——但项目已经跑起来了、测试也覆盖了各种 corner case，重写的收益不大，就这样了。

---

## 九、安全：不信任任何输入

### 24. 每一层都有资源上限

不依赖任何单一防线，每层都有独立的 DoS 保护：

- URL 路径深度最多 32 段——防止 `/a/b/c/.../z` 的路径爆破
- 参数值最长 1024 字节——防止超长参数撑爆内存
- multipart 最多 256 个 part——防止恶意文件上传
- Session 总数上限 10 万——防止 Session 洪水
- 连接数默认上限 1 万——防止连接耗尽
- `IdleFd` 预留文件描述符——EMFILE 时还能发 503
- 异步日志写满自动丢弃——防止日志撑爆磁盘

这些数字不是拍脑袋定的，每一个都有对应的攻击场景。纵深防御的核心思想就是：假设任何一层都可能被突破。

### 25. Session ID 登录后必须换

这是 OWASP 安全指南里的标准建议。用户登录成功后，调 `regenerate()` 在写锁保护下原子替换 Session ID。攻击者提前通过 URL 诱导设置的旧 ID 立即失效。`migrateFrom()` 在需要迁移数据时用地址序双锁避免死锁——两个 Session 对象按内存地址排序加锁，无论谁先调用都不会死锁。

---

---

就这些。有些设计我自己也不确定是不是最优解，比如 PMR 三级池在轻负载下收益有限、MPSC 队列的 128 节点缓存上限没做过严格测试、中间件合并只能处理连续同步的情况。但目前跑着没问题，等真出了瓶颈再说。

项目开源在 [GitHub](https://github.com/Hical61/Hical.git)，代码都在那，有疑问直接提 issue 或者评论区聊。
