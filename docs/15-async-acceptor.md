# 15 · AsyncAcceptor：接受连接

## 1. 是什么

`AsyncAcceptor` 负责监听一个端口、接受新连接，然后把新连接交给回调。它是网络层的入口，通常由 [SocketMgr](18-network-thread-and-socket-mgr.md) 创建和使用，很少需要直接使用它。

## 2. 核心接口

| 接口 | 作用 | 位置 |
|---|---|---|
| `AsyncAcceptor(ioContext, bindIp, port)` | 构造，只记录地址，还不监听 | [AsyncAcceptor.h](../src/common/network/AsyncAcceptor.h) |
| `Bind()` | 打开、设置选项、绑定、开始监听；失败返回 `false` | [AsyncAcceptor.h](../src/common/network/AsyncAcceptor.h) |
| `AsyncAccept(select, accepted)` | 持续接受连接 | [AsyncAcceptor.h](../src/common/network/AsyncAcceptor.h) |
| `Close()` | 停止接受（可以重复调用） | [AsyncAcceptor.h](../src/common/network/AsyncAcceptor.h) |

`AsyncAccept` 的两个参数：

- `select()`：返回一个 `IoContext*`，决定**新连接属于哪个网络线程**；
- `accepted(IoContextTcpSocket&&)`：拿到已建立的连接。

## 3. 关键实现

### 3.1 新 socket 在创建时就属于目标网络线程

`AcceptLoop` 协程里：

```cpp
Asio::IoContext* target = select();
auto [error, socket] = co_await Rooftree::Await<boost::system::error_code, IoContextTcpSocket>([&](auto done)
{
    _acceptor.async_accept(target->get_executor(), std::move(done));
});
```

`async_accept` 的第一个参数指定了**新 socket 使用哪个 executor**。所以：

- 接受连接这个动作运行在 acceptor 自己的 io_context 上（通常是主 io_context）；
- 但接受得到的 socket 一开始就绑定在 `select()` 选出的网络线程的 io_context 上，之后它所有的异步读写回调都在那个线程上执行。

这样就不需要在线程之间"迁移" socket。`SocketMgr` 传入的选择函数是"连接数最少的网络线程"，实现了简单的负载均衡。

### 3.2 循环接受

`AcceptLoop` 是一个循环，直到 acceptor 关闭。每一轮都重新调用 `select()`，所以每个新连接都会重新选择网络线程。协程的写法见 [16 · 3.2](16-socket.md)。

新 socket 被设成非阻塞模式，这是 [Socket](16-socket.md) 在 Linux 上"先尝试同步写，写不完再异步等待"的前提。

### 3.3 `Bind()` 里的几个选项

| 选项 | 作用 | 位置 |
|---|---|---|
| `reuse_address` | 服务器重启后能立刻重新绑定同一个端口，不用等 `TIME_WAIT` 结束 | [AsyncAcceptor.h](../src/common/network/AsyncAcceptor.h) |
| `v6_only(false)` | 监听 IPv6 地址时同时接受 IPv4 连接（某些 BSD 系统默认只接受 IPv6） | [AsyncAcceptor.h](../src/common/network/AsyncAcceptor.h) |
| `listen(max_listen_connections)` | 使用系统允许的最大等待队列长度 | [AsyncAcceptor.h](../src/common/network/AsyncAcceptor.h) |

## 4. 线程与安全

- `AsyncAccept` 的回调运行在 acceptor 所属的 io_context 线程上。
- `_closed` 是原子变量，所以 `Close()` 可以在别的线程调用。
- 但是 **acceptor 对象被销毁之前，必须先让它的 io_context 停下来**：`Close()` 会取消正在等待的 `async_accept`，被取消的回调随后仍然会在 io_context 上执行，而回调里访问了 `this`。Rooftree 的网络测试曾因此被 ASan 报出 use-after-free，详见 [18](18-network-thread-and-socket-mgr.md) 第 6 节。

## 5. 设计取舍

接受循环是协程，`Bind()` 失败时记 error 日志并说明是哪一步（open、bind、listen）。`Close()` 不能关闭 acceptor 后立即返回，因为那时被取消的 `async_accept` 还没有完成；`SocketMgr::StopNetwork` 紧接着销毁这个 acceptor 和所有网络线程，这个操作就要么在已经销毁的 acceptor 上执行回调（回调里读 `_closed`），要么在 acceptor 的 io_context 析构时才被销毁，而它持有的新 socket 属于已经不存在的网络线程 io_context。ASan 在 [test_Network.cpp](../tests/test_Network.cpp) 的 WebSocket 测试里报出了这个 use-after-free。

现在 `Close()` 会等这个操作完成（`_loops` 归零）：io_context 还在运行时，就等它在自己的线程上执行；io_context 已经停了（先停 io_context 再 `StopNetwork`，测试和 arena 都是这个顺序），就在当前线程上 `restart()` + `poll()` 执行一次再停掉。

## 6. 常见的坑

- **构造函数不会检查地址**：地址写错（比如 `"0.0.0.O"`）时，构造函数里的 `make_address` 会抛异常；`SocketMgr::StartNetwork` 会捕获它并返回 `false`。
- **端口被占用时 `Bind()` 返回 `false`**，只会在 `network` logger 上打一条 info 级别的日志。启动失败时记得看日志。
- **它无法告诉你实际绑定的端口**：传入端口 0 让系统自动分配时，没有办法查到分到了哪个端口。网络测试因此先用另一个 socket 找一个空闲端口，再用这个端口启动服务（[test_Network.cpp](../tests/test_Network.cpp) 的 `FindFreePort`）。

## 7. 测试与示例

- [tests/test_Network.cpp](../tests/test_Network.cpp)："SocketMgr echoes data over loopback"，其中 16 个客户端并发连接，会被分到不同的网络线程。
- [examples/echo_server.cpp](../examples/echo_server.cpp)。

## 8. 思考题与练习

1. 在 `echo_server` 的 `Start()` 里打印 `std::this_thread::get_id()`，再在 `ReadHandler()` 里打印一次。两者一样吗？为什么？
2. 用两个 `echo_server` 进程绑定同一个端口，观察第二个进程的输出。
3. 如果把 `select` 改成"总是返回第一个网络线程"，会有什么影响？
