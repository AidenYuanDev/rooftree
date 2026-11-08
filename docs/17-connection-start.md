# 17 · 连接的开始：Start、TLS 握手、开始读

## 1. 是什么

一个连接被接受以后，在开始读数据之前可能还要做几件事：TLS 握手、设置超时、准备 HTTP 解析器。`SocketMgr` 构造好 socket 后调用它的 `Start()`（[18](18-network-thread-and-socket-mgr.md)），这几件事就写在 `Start()` 里：

```cpp
void Start() override                           // 明文 HTTP（Http::Socket）
{
    StartHttp();                                // 新的解析器，然后 StartReading()
}

void Start() override                           // HTTPS（Http::SslSocket）
{
    AcceptTls(*this, [](SslSocket& socket) { socket.StartHttp(); });
}
```

`AcceptTls` 是一个协程：等 TLS 握手完成，成功了再执行后面的一步，失败就关闭连接。

没有协程时，这里常用"连接初始化链"：每一步是一个对象，串成链表，每一步做完调用下一步。有了协程以后，这些步骤可以直接按顺序写。

## 2. 核心接口

| 接口 | 作用 | 位置 |
|---|---|---|
| `Socket::StartReading()` | 开始读循环，每次读到数据调用 `ReadHandler()`（[16](16-socket.md)） | [Socket.h](../src/common/network/Socket.h) |
| `AcceptTls(socket, then)` | 服务端 TLS 握手，成功后在网络线程上调用 `then(socket)`，失败关闭连接 | [SslStream.h](../src/common/network/SslStream.h) |
| `BaseSocket::StartHttp()` | HTTP：新建解析器，然后开始读 | [BaseHttpSocket.h](../src/common/network/http/BaseHttpSocket.h) |

## 3. 关键实现

### 3.1 握手的协程不持有 socket

```cpp
template<typename SocketType, typename Then>
Rooftree::Detached AcceptTls(SocketType& socket, Then then)
{
    std::weak_ptr<SocketType> weak = ...socket.shared_from_this()...;
    auto [error] = co_await Rooftree::Await<boost::system::error_code>([&](auto done)
    {
        socket.underlying_stream().async_handshake(server, std::move(done));
    });

    std::shared_ptr<SocketType> self = weak.lock();   // 握手期间 socket 可能已经没了
    if (!self)
        co_return;
    ...
}
```

协程只拿着一个 `weak_ptr`。一个握手到一半就不说话的客户端，等到请求超时（[19](19-http.md)）被关闭，网络线程放开它，socket 析构时关闭底层 socket，握手随之失败，协程看到 `weak_ptr` 已经失效就结束了。如果协程拿的是 `shared_ptr`，这个 socket 就要等客户端自己断开才会释放。

握手失败（扫描器、过老的 TLS 版本、半路断开）只记一条 debug 日志：这是客户端造成的，而且任何人都能每个连接制造一条，不应该记成错误。

### 3.2 什么时候需要 `then`

握手成功后接着做的事放在 `then` 里，它在握手完成的那个回调里执行，也就是在连接所属的网络线程上。明文和 TLS 只差这一步，所以 `WorldHttpService` 用同一种 socket 同时服务两种连接（[30](30-static-files-and-tls.md)）：

```cpp
void WorldHttpSocket::Start()
{
    SetMaxRequestBodySize(...);
    SetTimeouts(...);
    if (underlying_stream().IsSsl())
        Net::AcceptTls(*this, [](WorldHttpSocket& socket) { socket.StartHttp(); });
    else
        StartHttp();
}
```

## 4. 线程与安全

`Start()` 在接受连接的线程上调用，这时 socket 还没有交给网络线程，但它的 io_context 已经是网络线程的了，所以它发起的异步操作（握手、第一次读）都在网络线程上完成。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| `SocketConnectionInitializer` 链：`ReadConnectionInitializer`、`SslHandshakeConnectionInitializer`、`HttpConnectionInitializer`、`IpBanCheckConnectionInitializer` | `Start()` 里按顺序写，握手是协程 `AcceptTls` | 协程把"做完这一步再做下一步"直接写出来，不需要为每一步定义一个类 |
| 握手失败记 error 日志（`session` logger） | debug（`network` logger） | 见 3.1 |
| 封 IP 的检查在初始化链里，要查数据库 | 在 `SocketMgr::OnSocketOpen` 里，构造 socket 之前（`AddressBlocklist`，[18](18-network-thread-and-socket-mgr.md)） | 被封的连接什么都不分配 |

## 6. 常见的坑

- **在 `Start()` 里忘了开始读**：连接永远不会收到数据，直到超时被关闭。
- **在 `then` 里捕获 socket 的引用**：`then` 收到的 `socket` 就是可以用的那个，不要捕获外面的 `this`。

## 7. 测试与示例

- [tests/test_ServerNetwork.cpp](../tests/test_ServerNetwork.cpp) 的 TLS 用例（`[tls]`）：HTTPS 请求、`wss://`、证书不对时拒绝。
- [examples/echo_server.cpp](../examples/echo_server.cpp)：最简单的 `Start()`，直接 `StartReading()`。

## 8. 思考题

1. 给 `echo_server` 加上 TLS：socket 类型换成 `Socket<SslStream<>>`，`Start()` 里调用 `AcceptTls`。用 `openssl s_client` 连上去测试。
2. 如果要在握手之后、开始读之前查一次数据库（比如检查这个 IP 是否被封），用协程应该怎么写？和回调串起来的初始化链相比哪种更容易读？
