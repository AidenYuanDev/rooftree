# 33 · KCP：UDP 上的游戏连接

## 1. 是什么

客户端固定的游戏只能用它定下的协议。我们自己写客户端（Unreal、Unity），可以用 UDP。TCP 丢一个包时，后面所有数据都要等它重传（队头阻塞），延迟会突然跳到几百毫秒；UDP 上丢一个包只耽误它自己。

`Rooftree::Net::Kcp` 在 UDP 上提供两种消息：

- **可靠消息**（`Send`）：交给 [KCP](https://github.com/skywind3000/kcp) 这个 ARQ 算法，保证到达、保证顺序。适合技能、聊天、交易。
- **不可靠消息**（`SendUnreliable`）：一个数据报一条消息，可能丢、可能乱序。适合"新的覆盖旧的"的数据，比如每帧的位置和状态。

UDP 没有连接，握手、鉴权、加密、心跳、超时、断开这些 TCP 白给的东西都要自己做，这就是这一篇的代码。用法和 [WebSocket](27-websocket.md) 一样，继承两个类：

```cpp
class GameConnection final : public Kcp::Connection
{
    void OnMessage(std::string&& message, bool reliable) override { Send(message); }
};

class GameServer final : public Kcp::Server
{
    std::shared_ptr<Kcp::Connection> OnHello(std::string_view token, boost::asio::ip::address const&,
        std::function<bool(Kcp::Key const&)> const& verify) override
    {
        // 按 token 找到它的密钥，verify(密钥) 为 true 才用掉 token
        return std::make_shared<GameConnection>();
    }
};

GameServer server;
server.Start("0.0.0.0", 8092);
```

KCP 本身是一个 C 文件，没有发行版的包，原样放在 [deps/kcp](../deps/kcp)（1.7.1，MIT）。

## 2. 核心组成

| 类 / 函数 | 作用 | 位置 |
|---|---|---|
| `Kcp::Server` | 一个 UDP 端口和一个线程，所有连接都在这个线程上：相当于 TCP 的 `SocketMgr` + `NetworkThread` | [KcpServer.h:147](../src/common/network/kcp/KcpServer.h#L147) |
| `OnHello(token, address, verify)` | 服务器线程：一个 Hello。按 token 找密钥，`verify` 通过才用掉 token，返回要打开的连接；`nullptr` 拒绝，客户端收不到任何回答 | [:180](../src/common/network/kcp/KcpServer.h#L180) |
| `Kcp::Connection` | 一个客户端：`OnOpen` / `OnMessage` / `OnClose` 在服务器线程上调用 | [:59](../src/common/network/kcp/KcpServer.h#L59) |
| `Send` / `SendUnreliable` / `Close` | **任意线程都可以调用**，进入无锁队列，服务器线程 1 ms 内发出 | [:69](../src/common/network/kcp/KcpServer.h#L69) 起 |
| `Kcp::Client` | 客户端，用于测试和工具，也是游戏客户端实现协议时的参照 | [KcpClient.h:30](../src/common/network/kcp/KcpClient.h#L30) |
| `Kcp::Link` | 两端共用：数据报的加密和解密、KCP 控制块 | [KcpLink.h:42](../src/common/network/kcp/KcpLink.h#L42) |
| `Kcp::Settings` | KCP 参数、消息上限、超时（两端各自设置） | [KcpLink.h:22](../src/common/network/kcp/KcpLink.h#L22) |
| `Kcp::Cipher` | AES-128-GCM，nonce 是方向 + 计数器 | [KcpProtocol.h:120](../src/common/network/kcp/KcpProtocol.h#L120) |
| `Kcp::ReplayWindow` | 每个方向的计数器窗口，丢掉见过的数据报 | [:142](../src/common/network/kcp/KcpProtocol.h#L142) |
| `SetMaxConnections` / `SetMaxConnectionsPerAddress` / `SetAddressBlocklist` | 和 `SocketMgr` 一样，复用 `ConnectionLimiter` 和 `AddressBlocklist` | [KcpServer.h](../src/common/network/kcp/KcpServer.h) |

## 3. 关键实现

### 3.1 数据报格式

所有数据报都是一个格式，小端：

```
conv u32 | type u8 | counter u64 | 明文部分 | 正文（加密）| tag 16
\________ 头 13 字节 ____________/
```

| type | 方向 | 正文 |
|---|---|---|
| Hello | 客户端 → 服务器 | 版本 u8 + nonce u64。明文部分是 token（u8 长度 + 字节）。收到 Welcome 之前每 250 ms 重发一次 |
| Welcome | 服务器 → 客户端 | 版本 u8。头里的 conv 就是这条连接的 conv |
| Data | 双向 | KCP 的段，可靠消息 |
| Unreliable | 双向 | 一条消息 |
| Ping / Pong | 客户端 ↔ 服务器 | 至多 8 字节，Pong 原样返回（客户端的时钟，用来算往返时间） |
| Close | 双向 | 状态码 u16 + 原因，编号和 WebSocket 一样 |

数据报最大 1200 字节：IPv6 保证每条路径都能走 1280 字节，再留出 IP、UDP 头和一两层隧道的余量。去掉头和 tag 剩 1171 字节，这就是 KCP 的 MTU，也是不可靠消息的上限。更大的不可靠消息自动改为可靠发送。

### 3.2 鉴权和加密

密钥是**每次登录一个**：客户端通过 HTTPS 登录，拿到 token 和一个随机密钥，和会话密钥的做法一样。之后的每个数据报都用 AES-128-GCM 加密，头和明文部分作为附加数据（AAD）参与认证：

- **nonce 不会重复**：nonce = 方向（1 字节）+ 0（3 字节）+ 计数器（8 字节）。两个方向用同一个密钥，但方向不同。客户端在拿到 conv 之前发的 Hello 也占用计数器，`Link` 从 Hello 之后的计数器接着发。客户端每次连接从一个 63 位的随机数开始：同一个密钥可能被另一个客户端再用一次（重试同一个 token），都从 0 开始的话 nonce 就重复了。自己实现客户端时也要这样做。
- **改了任何一个字节都通不过**：包括头里的 conv、type 和 counter。
- **重放没用**：每个方向记住最高的计数器和它前面 64 个（DTLS 的做法），见过的、更旧的都丢掉。
- **不回答通不过认证的数据报**：没有密钥的人发什么都得不到回答。服务器只回答通过认证的 Hello，回答也比请求小，所以不能被用来做反射放大攻击。
- **token 只在 Hello 被认证之后才用掉**：`OnHello` 拿 token 去找密钥，把密钥交给 `verify`。`verify` 检查 Hello 是不是用这个密钥加密的，返回 `true` 之后，应用才把 token 作废。如果先作废再检查，看到 token 的人（它是明文）就能抢先发一个假 Hello 把它用掉。arena 的 `TicketMgr::Consume(ticket, accept)` 在同一把锁里完成"检查再作废"。

### 3.3 握手

1. 客户端发 Hello，正文里有一个随机的 nonce。
2. 服务器：检查限制（总连接数、每个地址的连接数、黑名单）→ `OnHello` → 分配一个随机的 conv → 回 Welcome → `OnOpen`。
3. Welcome 丢了，客户端会重发 Hello。这时 token 已经用掉了，服务器按 token 找到已经打开的连接，用这条连接的密钥认证，**nonce 也要相同**，然后再发一次 Welcome。同一个 token 和密钥、但 nonce 不同的 Hello 是另一个客户端，不回答（测试"a used one gets no answer"）。
4. 版本号不同但认证通过的 Hello：回一个 conv 为 0 的 Close（ProtocolError），客户端知道是版本问题，不会一直等到超时。这时 token 还没用掉，换对版本后同一个密钥还能连上，新连接的计数器从 0 开始，所以这个 Close 的计数器固定用最大值（`RefusalCounter`），不和它重复。

### 3.4 一个服务器线程

和 TCP 的 `NetworkThread` 一样，所有连接都在一个线程上，每 1 ms 更新一次：

- 收到数据报：查 conv → 解密、检查重放 → 交给 KCP（`ikcp_input`）→ 把完整的消息取出来交给 `OnMessage`。
- 每次更新：把各个连接发送队列里的消息交给 KCP，立即 `ikcp_flush`，不等 KCP 的 10 ms 间隔，所以消息在 1 ms 内发出；然后跑 KCP 的重传计时器（`ikcp_check` 决定什么时候需要）；最后检查超时和关闭。
- 发送：非阻塞的 `send_to`。socket 缓冲区满时这个数据报丢掉，相当于网络上丢了一个包，KCP 会重传。

收和更新各是一个协程（`ReceiveLoop`、`UpdateLoop`，用的是 [Socket](16-socket.md) 的 `Rooftree::Detached` 和 `Await`），在这条线程上运行；客户端也一样，它的两个协程按值持有 `self`，连接关闭后才释放。

一个 UDP socket 一个线程就能处理很多连接，因为每个数据报的工作量很小（一次 AES-GCM 加一次 KCP 输入）。真的不够时，可以开几个 `Server`，各用一个端口，登录时分配。

### 3.5 连接跟着客户端换地址

手机切换网络、NAT 忘掉了映射，客户端的地址和端口都会变。conv 和密钥不变，所以只要一个通过认证、**并且计数器比以前都大**的数据报从新地址来，连接就改为向新地址发送。计数器这个条件防止有人截下一个旧数据报，从别的地址重放，把连接引走。`GetRemoteAddress()` 和日志里的地址仍然是打开时的地址，`ConnectionLimiter` 也按这个地址计数。

### 3.6 关闭和超时

| 情况 | 状态码 |
|---|---|
| `Close(code, reason)`：之前发送的可靠消息先发完（最多等 `CloseTimeout`，2 秒），再发 Close | 应用给的 |
| 对方发来 Close | 对方给的 |
| `IdleTimeout`（15 秒）内没有收到任何数据报。客户端在 `PingInterval`（5 秒）内什么都没发时会发一个 Ping | 3000 Timeout |
| 一个段重传了 20 次还没有确认（KCP 的 dead link） | 3001 DeadLink |
| 发出但没有确认的段超过 `MaxWaitingSegments`（2048）：对方确认得比我们发得慢 | 3002 SendBufferFull |
| 收到的可靠消息超过 `MaxMessageSize`（64 KiB） | 1009 |
| `OnMessage` 抛出异常：只关闭这一条连接，服务器和其他连接不受影响 | 1011 |
| `Server::Stop()` | 1001 |

`OnClose` 无论哪种情况都只调用一次，`IsOpen()` 在它之前就变成 `false`，所以主线程只要看 `IsOpen()` 就知道连接已经没了。

### 3.7 KCP 的参数

默认就是 KCP README 推荐给游戏的设置，`ikcp_nodelay(1, 10, 2, 1)`：

- `NoDelay`：丢包时 RTO 不翻倍，最小 RTO 30 ms（默认 100 ms）。
- `Interval` 10 ms：重传计时器的精度。消息本身不等这个间隔，见 3.4。
- `FastResend` 2：一个段被后面的两个确认跳过时立即重传，不等超时。
- 关掉拥塞窗口：游戏发的数据很少，拥塞控制只会增加延迟。

消息上限有一个硬限制：KCP 一条消息最多分成 127 个段，而且这些段要能同时放进接收窗口，`Settings::GetMaxMessageSizeLimit()` 算出来是 127 × 1147 ≈ 142 KiB。`MaxMessageSize` 超过它时按它算。

## 4. arena：WebSocket 和 KCP 共用一个会话

arena 的 `ArenaSession` 原来直接持有 `ArenaSocket`（WebSocket）。现在它通过 `ArenaConnection` 接口发送消息（[ArenaConnection.h](../apps/arena/ArenaConnection.h)），不知道底下是哪种连接：

| | WebSocket（浏览器） | KCP（原生客户端） |
|---|---|---|
| 连接类 | `ArenaSocket` | `ArenaKcpConnection`（[ArenaKcp.h](../apps/arena/ArenaKcp.h)） |
| 凭证 | ticket 放在握手的查询字符串里 | ticket 是 Hello 的 token，Hello 用登录时返回的 `kcpKey` 加密 |
| 消息 | JSON 文本，在入口按 schema 转换（`format=packets` 时直接收发包） | FlatBuffers 包（[Arena.fbs](../apps/arena/schema/Arena.fbs)），见 [32](32-packets.md) 第 5 节 |
| 每帧的 state | 照常发送 | 不可靠发送，丢了由下一帧补上；超过 1171 字节自动改为可靠发送 |

登录的回答多了 `kcpPort` 和 `kcpKey`（十六进制）。ticket 仍然只能用一次，WebSocket 和 KCP 谁先用都可以。配置在 `Arena.Kcp.*`，见 `arena.yaml.dist`。

[examples/arena_kcp_client.cpp](../examples/arena_kcp_client.cpp) 是一个完整的原生客户端：登录、连接、等到 `Welcome` 之后发聊天、移动和 Ping，校验并打印收到的包，然后离开。它用的是 flatc 从 `Arena.fbs` 生成的 C++ 头文件，UE5 客户端用的也是这一份。

```sh
build/bin/arena                                              # Arena.Kcp.Enabled 默认打开
build/bin/arena_kcp_client <用户名> <密码> 127.0.0.1 8090 5
```

## 5. 常见的坑

- **先把消息取出来，再发确认**：确认里带着自己的接收窗口（还能收多少个段）。一个丢了的段重传到达时，排在它后面的段一下子全部完整，最多 128 个进入接收队列。如果这时候发确认，告诉对方的窗口就是 0，对方会停止发送，等 KCP 的窗口探测，第一次要等 **7 秒**。客户端原来就是这个顺序，20% 丢包的测试里 1% 的运行卡了 7 秒以上；改成先取消息、再 `Flush` 之后，200 次运行最慢 0.7 秒。测试里等待的时间因此从 15 秒收紧到 5 秒，再出现这种卡顿就会失败。
- **不可靠消息会超过可靠消息**：可靠消息要等下一次 flush 才发，不可靠消息立即发。所以发不可靠消息之前，先把排在前面的可靠消息 flush 掉，没有丢包时两者的顺序就和发送的顺序一致（测试"without loss what was sent reliably first arrives first"）。有丢包时仍然可能乱序，客户端要能处理"先收到 state、后收到 welcome"。
- **Hello 的 token 是明文**：所以不能在认证之前用掉它（3.2）。登录也必须走 HTTPS，否则密钥和 token 一起被看到，加密就没有意义了。
- **一个 token 两个客户端**：如果没有 Hello 里的 nonce，第二个客户端的 Hello 会被当成第一个的重发，拿到同一个 conv。
- **UDP 会被封**：有些公司和学校的网络不放行 UDP。客户端连不上 KCP 时应该回到 WebSocket，arena 两条路都开着就是为了这个。
- **在服务器线程里调用 `Stop()` 会死锁**：`Stop` 要等服务器线程把所有连接关闭。

## 6. 测试与示例

- [tests/test_Kcp.cpp](../tests/test_Kcp.cpp)：
  - 数据报：重放窗口（乱序、窗口外）；加密后改头、改正文、换方向、换密钥都打不开；Hello 的 token 是明文但被认证；Close 的正文；消息上限的计算。
  - 回环：200 条可靠消息按顺序到达，不可靠消息标为不可靠，60 KB 的消息分段；服务器先发完再关闭，客户端收到状态码和原因；客户端关闭；错误的密钥、未知的 token、用过的 token 都收不到回答，错误的密钥不会用掉 token；`OnMessage` 抛异常只关闭这一条连接；`Stop()` 以 1001 关闭客户端。
  - 限制：空闲超时（Ping 让连接保持打开）、服务器的消息上限、每个地址的连接数、黑名单、其他协议版本收到 Close。
  - 坏网络：一个 UDP 代理随机丢掉 20%、打乱 10% 的数据报，300 条消息和一条 20 KB 的消息全部按顺序到达；代理重放数据报，服务器全部丢掉；代理换一个端口（NAT 忘了映射），连接跟过去。
- [examples/arena_kcp_client.cpp](../examples/arena_kcp_client.cpp)：见第 4 节。

## 7. 思考题与练习

1. 一个玩家、6 个金币时，state 的包是 228 字节，同样内容的 JSON 是 542 字节。多少个玩家时 state 的包会超过 `MaxBodySize`、改为可靠发送？玩家的名字只在变化时发送，能省多少？
2. 客户端连不上 UDP 时回到 WebSocket（`/arena?ticket=...&format=packets`，收发的是同样的包）：在 `arena_kcp_client` 里加上，判断条件是什么？等多久？
3. 一个 `Server` 只有一个线程。用 `SO_REUSEPORT` 开几个 socket、几个线程监听同一个端口，内核按四元组分配数据报。连接换地址（3.5）以后会出什么问题？
4. 位置同步只要最新的一份：客户端的输入能不能也走不可靠消息？丢了一个"停止移动"的输入会怎样，怎么避免？
