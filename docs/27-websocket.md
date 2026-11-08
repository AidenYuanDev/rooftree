# 27 · WebSocket：Connection、BaseSocket、HTTP 升级与帧编解码

## 1. 是什么

浏览器不能直接建立 TCP 连接，要和服务器保持长连接、实时双向收发消息，只能用 WebSocket（RFC 6455）。`WebSocket::BaseSocket` 让一个普通的 [Socket](16-socket.md) 讲 WebSocket 协议，上层只需要处理"一条完整的消息"。

它和 [HTTP](19-http.md) 的做法一样：只覆盖 `ReadHandler()`，`Socket` 的读写队列、关闭流程和 [NetworkThread](18-network-thread-and-socket-mgr.md) 的线程模型完全复用。用法和普通 TCP 服务器相同，`SocketMgr` 的 `SocketType` 换成自己的 WebSocket 类即可：

```cpp
class ChatSocket final : public WebSocket::BaseSocket<>
{
    using BaseSocket::BaseSocket;
    void OnMessage(WebSocket::Opcode opcode, std::string&& payload) override { SendText(payload); }
};
```

握手之后的协议（拆帧、分片、ping、关闭握手、跨线程发送）放在 `WebSocket::Connection` 里，和 socket 分开。有两种 socket 可以运行一个 `Connection`：

- **`WebSocket::BaseSocket`**：自己占一个端口，自己读握手请求（上面的写法，arena 用的就是它）。
- **升级后的 HTTP 连接**：HTTP 处理函数用 101 回应握手请求后调用 `UpgradeToWebSocket`，这条连接从此不再是 HTTP。页面和它的 WebSocket 共用一个端口、一套 TLS、同一个源，`wss://` 不需要单独的端口和证书。博客走的是这条路（3.5）。

## 2. 核心组成

| 类 / 函数 | 作用 | 位置 |
|---|---|---|
| `WebSocket::Connection` | 握手之后的协议：拆帧、合并分片、回 ping、关闭握手，向上交付完整消息；无锁发送队列 | [WebSocketConnection.h:40](../src/common/network/websocket/WebSocketConnection.h#L40) |
| `WebSocket::ConnectionTransport` | `Connection` 眼里的 socket：写一帧、写完关闭、是否还开着 | [:28](../src/common/network/websocket/WebSocketConnection.h#L28) |
| `Open` / `Read` / `Flush` / `Closed` | 给运行它的 socket 调用：101 已入队、读到的帧、把发送队列移到写队列、socket 关了 | [:63](../src/common/network/websocket/WebSocketConnection.h#L63) 起 |
| `WebSocket::BaseSocket<Stream>` | 自己占一个端口的 WebSocket：读握手，然后运行自己的 `Connection` | [WebSocketSocket.h](../src/common/network/websocket/WebSocketSocket.h) |
| `Http::BaseSocket::UpgradeToWebSocket(connection, idle, maxQueue)` | HTTP 连接在 101 发出后改为运行 `connection` | [BaseHttpSocket.h](../src/common/network/http/BaseHttpSocket.h) |
| `OnHandshake(request, response)` | 握手请求通过协议检查后调用，返回 `false` 拒绝连接（可以设置状态码，例如 401） | 同上 |
| `OnOpen()` / `OnMessage()` | 连接建立、收到完整的 Text（已校验 UTF-8）或 Binary 消息（`Connection` 的虚函数） | [WebSocketConnection.h](../src/common/network/websocket/WebSocketConnection.h) |
| `OnClose()` | 连接关闭（`Socket` 的虚函数，只有 `BaseSocket` 有） | [Socket.h](../src/common/network/Socket.h) |
| `SendText` / `SendBinary` / `Close` | 发送，**任意线程都可以调用** | [WebSocketConnection.h](../src/common/network/websocket/WebSocketConnection.h) |
| `IsConnected()` | 任意线程：打开之后、socket 关闭之前 | 同上 |
| `SetTimeouts(handshake, idle)` | Rooftree：握手必须在 `handshake` 内完成；连接建立后 `idle` 内没收到任何数据就关闭（[16 · 3.6](16-socket.md#36-超时在-update-里比较时间)） | [WebSocketSocket.h:88](../src/common/network/websocket/WebSocketSocket.h#L88) |
| `ParseClientFrame` / `SerializeFrame` | 帧编解码，直接操作 `MessageBuffer` | [WebSocketFrame.h](../src/common/network/websocket/WebSocketFrame.h) |
| `AcceptHandshake` / `ComputeAcceptKey` | 检查握手请求，生成 `Sec-WebSocket-Accept` | 同上 |

## 3. 关键实现

### 为什么不用 Beast 的 `websocket::stream`

Beast 自带完整的 WebSocket 实现，但 `websocket::stream` 要自己驱动 `async_read`/`async_write`，而 `Rooftree::Net::Socket` 已经拥有这条读写循环（`AsyncRead` → `ReadHandler`，`QueuePacket` → `Update` 写出）。两套循环没法叠在一起。所以这里只借用 Beast 的 HTTP 解析器读握手请求，帧的格式自己解析。RFC 6455 的帧结构很简单，`WebSocketFrame.cpp` 两百多行就写完了。

### 一次连接的过程

1. **握手**：`ReadHandler()` 先把数据喂给 Beast 的 `request_parser`（限制头部 8 KiB、不允许请求体）。读完后 `AcceptHandshake` 检查 `Upgrade: websocket`、`Connection: Upgrade`、`Sec-WebSocket-Version: 13`、`Sec-WebSocket-Key`；然后调用 `OnHandshake`。通过就回 `101 Switching Protocols`，再调用 `OnOpen()`；不通过就回错误状态码，写完后关闭连接。
2. **拆帧**：`ParseClientFrame` 每次解析一帧。数据不够时什么都不消耗，返回 `NeedMoreData`，等下次读取。客户端发来的帧必须带掩码，RSV 位必须为 0（没有协商任何扩展），控制帧不能分片、长度不超过 125。
3. **分片**：Text/Binary 帧 `FIN=0` 时开始累积，后续 Continuation 帧追加，直到 `FIN=1` 才交给 `OnMessage`。累计长度超过 `SetMaxMessageSize`（默认 64 KiB）就以 1009 关闭连接。
4. **控制帧**：Ping 直接回 Pong；Close 回一个相同状态码的 Close，再关闭 TCP（服务器先关）。
5. **出错**：协议错误回 1002，Text 不是合法 UTF-8 回 1007，然后关闭连接。
6. **超时**：设了 `SetTimeouts` 时，从连上到握手完成是一个截止时间，握手完成后换成空闲超时。浏览器不会自己发 ping，所以页面要定时发点什么（博客每 25 秒报一次当前页面）。服务器主动推送的连接最好再设 `SetMaxWriteQueueSize`，见 [16 · 3.7](16-socket.md#37-写队列的上限)。

### 跨线程发送

`SendText` 只是把已编码的帧放进一个无锁的 `MPSCQueue`。运行它的 socket 在 `Update()` 里调用 `Flush`，在网络线程上把它们移到 `Socket` 的写队列。这样游戏主线程可以随时给任何连接发消息，而 `QueuePacket` 本身不需要加锁。`Close()` 也走这个队列，保证关闭帧排在之前发送的消息后面。

### 3.4 为什么把协议从 socket 里拆出来

原来协议直接写在 `BaseSocket` 里，它是一个 `Socket<Stream>`，只能占自己的端口。博客不用 nginx 以后，页面在 443 端口，WebSocket 如果还在别的端口，就要再开一个 TLS 监听、多管一张证书，有的网络还会封非标准端口。所以协议拆成了 `Connection`：它不知道自己跑在什么 socket 上，只通过 `ConnectionTransport` 写帧、请求关闭。`BaseSocket` 和 HTTP 的 `BaseSocket` 都实现了这个接口，运行同一份协议代码。

`BaseSocket` 的公开接口没有变（`OnHandshake`、`OnOpen`、`OnMessage`、`SendText`……），arena 和示例不用改。

### 3.5 在 HTTP 端口上升级

```
GET /ws  Upgrade: websocket ...      →  路由的处理函数（网络线程）
                                          AcceptHandshake 填好 101
                                          UpgradeToWebSocket(connection)   记下来，还没生效
                                       ←  SendResponse：101 入队
                                          FinishUpgrade：从此读到的字节交给 connection
```

- 101 必须先于任何一帧写出，所以 `UpgradeToWebSocket` 只是记下来，等 `SendResponse` 把响应入队以后，才在 `FinishResponse` 里切换。响应不是 101（处理函数又拒绝了）就作废。
- 切换后 HTTP 的超时不再适用：清掉截止时间，换成 WebSocket 的空闲超时，再设上写队列上限（HTTP 连接没有上限，一次响应可能很大）。
- 客户端可能在握手请求后面紧跟着发帧，和请求在同一次读取里。`ProcessReadBuffer` 在切换后停止按 HTTP 解析，把剩下的字节交给 `Connection`。
- 之后 `ReadHandler` 直接把数据交给 `Connection::Read`，`Update` 先 `Flush` 再写。

`rooftree-server` 的 `WorldHttpService::WebSocketRoute(path, factory)` 把这些包好了（[29](29-server-core.md)）：工厂函数在网络线程上决定接不接受（比如检查 `Origin`），返回一个 `Connection`。

## 4. 线程与安全

| 函数 | 线程 |
|---|---|
| `ReadHandler`、`OnHandshake`、`OnOpen`、`OnMessage`、`OnClose`，`Connection` 的 `Open`/`Read`/`Flush` | 连接所属的网络线程 |
| `SendText`、`SendBinary`、`Close`、`IsConnected` | 任意线程 |
| `SetMaxMessageSize` | 网络线程，在 `Start()` 之前（参考 `ArenaSocket::Start`） |

`OnMessage` 里不要做耗时操作，它会阻塞同一个网络线程上的所有连接。像 [apps/arena](../apps/arena/ArenaSocket.cpp) 那样，在网络线程上只做解析，然后把消息排队交给主线程处理。

## 5. 设计取舍

不支持的功能：

- **扩展**：不协商 `permessage-deflate`，所以 RSV 位必须为 0
- **子协议**：需要的话，在 `OnHandshake` 里自己设置 `Sec-WebSocket-Protocol`
- **TLS**：`BaseSocket` 只支持明文。要 `wss://`，在 HTTP 服务上升级（3.5），它和页面共用 TLS（[30](30-static-files-and-tls.md)）

## 6. 常见的坑

### 第一次读取就关闭的连接，响应会丢失

这个问题是写这个模块时发现的。`NetworkThread::AddNewSockets()` 原来会直接丢掉"不是 open 状态"的新 socket，而"正在关闭"（`DelayedCloseSocket()` 之后、写队列还没发完）也不算 open。连接的第一次读取通常比 `AddNewSockets()` 更早完成（后者 1ms 才执行一次），如果第一次读取就决定关闭，比如拒绝握手，或者 HTTP 响应带 `Connection: close`，排好的响应就永远发不出去。现在已经在 `NetworkThread` 里修复，见 [18 · 设计取舍](18-network-thread-and-socket-mgr.md#5-设计取舍)。

### 其他

- **浏览器不能给 WebSocket 握手加自定义请求头**：凭据只能放在 URL 的查询参数里（`/arena?ticket=...`），所以要用一次性、短时效的票据，不要直接传密码或长期有效的 token。
- **只有 Text 消息会校验 UTF-8**：二进制数据要用 `SendBinary`，否则浏览器会以 1007 断开。
- **`OnMessage` 在 `Close()` 之后不会再被调用**：发出关闭帧以后，对方再发来的消息都会被忽略。

## 7. 测试与示例

- [tests/test_Network.cpp](../tests/test_Network.cpp)："WebSocket frame codec" 测试帧编解码和 RFC 的握手示例；"WebSocket server talks to a Beast client over loopback" 用 **Beast 的 WebSocket 客户端**作为独立实现，测试文本、二进制、分片、大消息、ping、双方发起的关闭、非法 UTF-8 和拒绝握手；"An HTTP socket upgrades to WebSocket"：先发 HTTP 请求再在同一连接上握手、握手和第一帧在同一个包里、被拒绝的握手仍然是 HTTP。
- [tests/test_ServerNetwork.cpp](../tests/test_ServerNetwork.cpp)："WebSocketRoute over TLS, closed when silent"：`wss://` 和空闲超时。
- [examples/websocket_server.cpp](../examples/websocket_server.cpp)：最小的回显服务器。用浏览器打开 `build/bin/websocket.html` 就能测试。
- [apps/arena](../apps/arena/)：完整的应用，包括票据鉴权、把消息交给主线程，以及多人同步。
- 二进制消息作为带 opcode 的 FlatBuffers 包：见 [32-packets](32-packets.md)。

## 8. 思考题与练习

1. 把 `examples/websocket_server.cpp` 改成聊天室：在 `SocketMgr` 里保存所有连接，`OnMessage` 时转发给每个连接。哪些数据需要加锁？为什么 `SendText` 不需要？
2. 让服务器每 20 秒发一次 Ping，30 秒内没收到 Pong 就断开。应该在哪个函数里计时？（提示：`Connection::Flush` 每 1ms 被调用一次；空闲超时已经能处理"30 秒没收到任何东西"，Ping 让不发消息的客户端也能保持连接。）
3. 用浏览器开发者工具的 Network → WS 面板观察一次连接，找到 101 响应和每一帧。
4. 读一遍 Beast 的 `websocket::stream`，对比它的帧解析和 `ParseClientFrame`。它支持哪些这里不支持的功能？
5. 如果 `UpgradeToWebSocket` 立刻切换、不等 101 入队，客户端会收到什么？用 "a frame in the same packet as the handshake" 这个测试的写法验证。
