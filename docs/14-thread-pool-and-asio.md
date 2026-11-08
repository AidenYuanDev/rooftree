# 14 · ThreadPool 与 Asio 封装：IoContext、Strand、DeadlineTimer

## 1. 是什么

网络层和数据库层都建立在 [Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html) 之上。框架没有直接使用 Asio 的类型，而是包了薄薄的一层：

| 封装 | 实质 | 位置 |
|---|---|---|
| `Rooftree::ThreadPool` | `boost::asio::thread_pool` | [ThreadPool.h](../src/common/thread/ThreadPool.h) |
| `Rooftree::Asio::IoContext` | 从 `boost::asio::io_context` 派生的类 | [IoContext.h](../src/common/asio/IoContext.h) |
| `Rooftree::Asio::IoContextExecutor` | `io_context::executor_type` 的别名 | 同上 |
| `Rooftree::Asio::Strand` | 从 `boost::asio::strand<IoContextExecutor>` 派生的类 | [Strand.h](../src/common/asio/Strand.h) |
| `Rooftree::Asio::DeadlineTimer` | 使用 `steady_clock`、`IoContextExecutor` 的定时器 | [DeadlineTimer.h](../src/common/asio/DeadlineTimer.h) |

这一层很薄：`IoContext` 直接继承 `io_context`，`run()`、`poll()`、`stop()`、`get_executor()` 都是继承来的，不需要逐个转发。

**学习这一篇之前，建议先熟悉 Asio 本身**：`io_context` 是什么、`run()` 做了什么、`post` 和异步操作的回调在哪个线程执行、`strand` 解决了什么问题。

## 2. 为什么要包一层

### 2.1 可以前置声明，减少编译时间

Asio 的头文件非常重。`AsioHacksFwd.h`（[AsioHacksFwd.h](../src/common/asio/AsioHacksFwd.h)）前置声明了这些类型，其他头文件只需要包含这个轻量的头文件就能声明 `IoContext*` 指针或引用，只有真正使用的 `.cpp` 才包含 Asio。

但 Asio 的很多类型是嵌套类或者长长的模板别名，没法前置声明。派生出一个同名的新类（`class Strand : public boost::asio::strand<...>`），这个新类就可以前置声明了。日志的头文件就只持有一个 `Strand*`（[08](08-log.md)）。

### 2.2 具体的 executor 类型，避免类型擦除

新版 Asio 的 socket 和定时器默认使用 `any_io_executor`，它通过类型擦除可以装下任何 executor，代价是每次异步操作都多一层间接调用。所以 socket 类型显式指定了 `IoContextExecutor`：

```cpp
using IoContextTcpSocket = boost::asio::basic_stream_socket<boost::asio::ip::tcp, Asio::IoContextExecutor>;
```

这样编译器知道确切的类型，可以直接调用和内联。

## 3. 核心接口

### ThreadPool

| 接口 | 作用 |
|---|---|
| `ThreadPool(n)` | 创建 `n` 个线程（默认等于 CPU 核数） |
| `PostWork(f)` | 把任务交给线程池 |
| `Join()` | 等待所有任务完成、线程退出 |
| `Stop()` | 尽快停止，未执行的任务会被丢弃 |

服务器可以用它运行主 io_context：创建 `ThreadPool` 后，在每个线程里执行一次 `ioContext->run()`。

### IoContext、Strand 与 post

| 接口 | 作用 |
|---|---|
| `run()` / `poll()` / `stop()` / `restart()` / `get_executor()` | 继承自 `io_context` |
| `boost::asio::post(ioContext, f)` | 在运行这个 io_context 的某个线程上执行 `f` |
| `Rooftree::Asio::post(strand, f)` | 在 strand 上执行：同一个 strand 上的任务**串行**、**按顺序**执行 |

### DeadlineTimer

就是一个 Asio 定时器：`expires_after(d)` 设定时间，`async_wait(handler)` 到期后在 io_context 的线程上调用 `handler`。项目里的用法：

- `NetworkThread` 每 1ms 用它触发一次 `Update()`（[18](18-network-thread-and-socket-mgr.md)）；
- HTTP 的 `SessionService` 每分钟用它清理一次过期会话。

## 4. 线程与安全

- `io_context::run()` 可以在多个线程中同时调用，Asio 会把就绪的回调分给它们。
- **同一个 strand 上的回调永远不会并发执行**，这是不加锁就能安全访问共享状态的关键。日志的异步模式正是靠一个 strand 保证写入有序（[08](08-log.md)）。
- 构造 `IoContext` 时可以传一个 `concurrency_hint`。`NetworkThread` 传了 1，告诉 Asio 只会有一个线程运行它，Asio 可以据此省掉内部的一些锁。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| `IoContext` 持有一个 `io_context` 成员，逐个转发方法 | 直接继承 | 更短，接口完全一样 |
| `IoContextExecutor` 是派生类 | 别名 | 派生只是为了前置声明，但没有地方前置声明它 |
| `Strand` 基于已弃用的 `io_context::strand` | 基于 `boost::asio::strand<IoContextExecutor>` | 新接口，可以作为 executor 传给异步操作 |
| `post(executor, f)`、`get_io_context`、`AsioHacksFwd.h` 里一串 Boost 前置声明 | 没有 | 没有用到 |

## 6. 常见的坑

- **`io_context::run()` 在没有任务时会立即返回**：要让线程一直等待新任务，需要一个 `executor_work_guard`。网络测试里的 `ServerContext` 就是这么做的（[test_Network.cpp](../tests/test_Network.cpp)）。
- **`stop()` 会丢弃还没执行的任务**：想等队列执行完再退出，应该先释放 `work_guard`，让 `run()` 自然返回。
- **对象生命周期**：异步操作的回调里如果捕获了 `this`，必须保证对象活得比回调久。`Socket` 用 `shared_from_this()` 解决这个问题（[16](16-socket.md)）；违反这一点的典型例子见 [18](18-network-thread-and-socket-mgr.md) 中 `StopNetwork()` 的调用顺序。

## 7. 测试与示例

- [tests/test_Framework.cpp](../tests/test_Framework.cpp) 的 "ThreadPool runs all work posted"。
- [tests/test_Framework.cpp](../tests/test_Framework.cpp) 的 "Async logging writes every message in order"：2 个线程运行同一个 io_context，日志通过 strand 有序写出。
- [examples/echo_server.cpp](../examples/echo_server.cpp)：主线程运行 io_context，并用 `signal_set` 处理 Ctrl+C。

## 8. 思考题与练习

1. 写一个程序：4 个线程运行同一个 io_context，向它 `post` 1000 个任务，每个任务给一个普通 `int` 加 1。结果对吗？改成通过一个 strand `post` 呢？
2. 用 `DeadlineTimer` 实现一个每秒打印一次的心跳，按 Ctrl+C 时干净地退出。
3. 读 [apps/arena/Main.cpp](../apps/arena/Main.cpp)，画出主线程、线程池和 io_context 之间的关系。
