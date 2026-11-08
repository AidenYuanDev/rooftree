# 32 · 包：opcode + FlatBuffers，WebSocket 上的二进制协议

## 1. 是什么

游戏的包常是"一个 opcode 说明这是什么包，后面是按 opcode 约定好的二进制字段"。这里保留"opcode + 包体"的思路，包体用 [FlatBuffers](https://flatbuffers.dev)，让 schema（`.fbs`）同时约定服务端和各种客户端的消息格式：

- **一份 schema，多种语言**：`flatc` 能从同一个 `.fbs` 生成 C++、C#（Unity）、TypeScript、Rust、Swift 的代码。
- **读的时候不解析、不拷贝**：字段直接从收到的 buffer 里读出来。以后做游戏时，高频的状态同步最在意这一点。
- **能演进**：字段只往后加、不删除（标成 `deprecated`），旧客户端照样能读新服务器发的包。

包走的是 [WebSocket](27-websocket.md) 的二进制帧，和 JSON 消息共用同一条连接、同一个 [WebSocketRouter](29-server-core.md)、同一套会话和 AntiDOS。

## 2. 核心组成

| 类 / 函数 | 作用 | 位置 |
|---|---|---|
| `PacketHeader`、`ReadPacketHeader`、`WritePacket` | 8 字节的包头：opcode、flags、requestId | [Packet.h](../src/server/shared/packet/Packet.h) |
| `Rooftree.Packets.Error`、`PacketError`、`MakeErrorPayload` | opcode 0：请求失败时的回答 | [Packets.fbs](../src/server/shared/packet/Packets.fbs) |
| `WebSocketRouter::AddPacket(opcode, messageType, verify, handler)` | 注册一个 opcode：先校验，再交给会话处理 | [WorldWebSocket.h](../src/server/shared/websocket/WorldWebSocket.h) |
| `AddPacket<Root>(opcode, messageType, &Session::Handler)` | 用生成代码的类型注册，校验器按 `Root` 自动生成 | 同上 |
| `WebSocketPacketMessage` | 主线程上的一个包：`GetPayload()`、`Reply()`、`ReplyError()` | 同上 |
| `WorldWebSocket::SendPacket` | 任意线程发包；服务器主动推送的包 requestId 为 0 | 同上 |
| `FlatJsonSchema` | 运行时按二进制 schema 把 FlatBuffer 转成 JSON，或把 JSON 转回 FlatBuffer | [FlatJson.h](../src/server/shared/packet/FlatJson.h) |
| `rooftree_add_flatbuffers(target PUBLIC\|PRIVATE schema.fbs...)` | CMake：构建时用 flatc 生成 `_generated.h` 和嵌入二进制 schema 的 `_bfbs_generated.h` | [deps/flatbuffers](../deps/flatbuffers/CMakeLists.txt) |

## 3. 包的格式

```
[uint16 opcode][uint16 flags][uint32 requestId][FlatBuffer]     小端
```

- **opcode**：包是什么，由路由表决定根类型。0 是 `Error`。
- **flags**：目前都是 0，留给以后用（比如压缩）。它占的 2 字节还让包头凑满 8 字节。**FlatBuffers 的校验器会检查对齐**，buffer 的起点按 8 对齐，payload 的起点也就按 8 对齐。包头如果是 6 字节，所有 4 字节以上的字段都会因为对齐不对而校验失败。
- **requestId**：回答和请求的 opcode、requestId 都相同，所以客户端可以同时发出多个请求。失败的请求回一个 opcode 为 0 的 `Error`，里面带有请求的 opcode 和类似 HTTP 的 `status`。限流时 `retryAfter` 给出要等的秒数（HTTP 入口把它变成 `Retry-After` 头）；`details` 放接口自己的附加信息，每项是一个值或一组值，比如封禁的 `until`、`reason`，或者文章有哪些语言的 `translations`。

收到二进制消息后，`WorldWebSocket::OnPacket` 在**网络线程**上依次处理：

1. 不够 8 字节：以 1007 关闭连接。
2. opcode 没有注册：回 `Error{400, unknown_opcode}`。
3. **用校验器检查 payload**：FlatBuffers 读字段时不检查边界，没校验过的 buffer 可能让程序读到任意内存。校验不过就以 1007 关闭，正常的客户端不会发这样的包。
4. 包装成 `WebSocketPacketMessage`，交给连接所属的会话，在主线程上运行。之后的流程和 HTTP 请求一样：先过 AntiDOS（按路由给出的 message type 计数），再交给处理函数。

同一个 message type 可以同时用在 HTTP 路由和包上：博客的管理包和 `/api/admin/...` 都叫 `admin`，读文章的包和 `/api/posts` 都叫 `posts.read`，两者合计算一个限额。

## 4. 处理函数读写 FlatBuffers，JSON 只在入口转换

处理函数直接拿到生成代码的表，自己构建回答。游戏和博客的接口都这样写：

```cpp
router.AddPacket(1, "game.move", &GameSession::HandleMove);    // void HandleMove(WebSocketPacketMessage&, Game::Move const&)

void GameSession::HandleMove(WebSocketPacketMessage& message, Game::Move const& move)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Game::CreatePosition(builder, move.x(), move.y()));
    message.Reply(builder);
}
```

还要讲 JSON 的入口（网页的 HTTP、控制台、GM 工具）用 `FlatJsonSchema` 在入口处转换：请求的 JSON 按 schema 变成表，处理函数回答的表再变回 JSON。同一个处理函数就能同时服务两种客户端，不需要为每个消息写转换代码。

### 博客的操作表

博客把 API 的每个操作在 [ApiOperations.cpp](../apps/blog/server/ApiOperations.cpp) 里登记一次：opcode、谁能调、HTTP 路由、处理函数、请求和回答的表名、message type。HTTP 路由和包的 opcode 都由这张表生成，所以两边不会对不上。表分两份 schema：[Admin.fbs](../apps/blog/server/schema/Admin.fbs)（0x01xx，后台）和 [Reader.fbs](../apps/blog/server/schema/Reader.fbs)（0x02xx，读者），[Api.fbs](../apps/blog/server/schema/Api.fbs) 把两份都 include 进来，入口转换和包的校验读的是它。

谁能调由 `ApiAccess` 决定，HTTP 和包检查的是同一个值：

| `ApiAccess` | 例子 | 不满足时 |
|---|---|---|
| `Anyone` | 文章、评论列表、站点、验证码、`Me` | — （没登录也能调，处理函数用 `GetLogin()` 看有没有登录） |
| `User` | 发评论、表态、网盘、改资料 | 401 `unauthenticated`；被封禁 403 `banned`，`details` 带 `until`、`reason` |
| `UserEvenBanned` | 删自己的评论、看和改邮件订阅 | 401 |
| `Moderator`、`Admin` | 后台 | 403 `forbidden` |

HTTP 上，登录用户的修改靠 CSRF token 防跨站；匿名的修改（订阅、找回密码、浏览计数）在表里标 `CheckOrigin`，入口检查 `Origin`。包的连接在握手时已经检查过 Origin，所以不用再查。

登出、改密码、重设密码也是操作（2026-10-29 起）。它们会改变调用者的登录：处理函数只说"登录结束了"（`ApiCall::EndLogin`）或"换成了这个新会话"（`StartLogin`，改密码会结束账号的所有会话），由入口落实：HTTP 清掉或设置 cookie，包的连接把握手时的登录换掉，之后的包以新会话或匿名身份调用，控制台不用管。

有些路由只能走 HTTP，不在表里：一开始就要设置登录 cookie 的（注册、登录、OAuth 的跳转和回调），文件（下载、zip、缩略图、分片上传），XML 和文本（RSS、sitemap、robots），以及邮件客户端一键退订用的表单（RFC 8058）。登录类路由回答的账号还是用 `Reader.Me` 表构建，再转成 JSON，所以账号只有一种定义。

HTTP 入口还做了几件包用不到的事：`Retry-After` 头来自 Error 的 `retryAfter`；3xx 的 Error 里如果有 `location`，就变成 `Location` 头（改过 slug 的文章答 301 `moved`）；POST 等请求的查询参数也会填进表（网盘的 `?user=`，前提是正文里没有这个字段）；空的正文当作 `{}`。

### 为什么不反过来

一开始博客是反过来做的：处理函数读写 JSON，包先转成 JSON 再交给它。实测之后改成了现在这样。[bench_Packets.cpp](../benchmarks/bench_Packets.cpp) 测的是博客的文章接口，只算协议这部分的 CPU，不含数据库，单位是微秒：

| 场景 | JSON 为核心：HTTP | JSON 为核心：包 | **FB 为核心：包** | FB 为核心：HTTP |
|---|---|---|---|---|
| 新建文章的请求（30 KB 正文） | 20.7 | 36.2 | **0.06** | 23.1 |
| 一篇文章的回答 | 15.7 | 3.0 | **1.2** | 17.6 |
| 文章列表的回答（20 条） | 10.5 | 14.4 | **2.5** | 15.7 |

以 FlatBuffers 为核心，包这条路快了 6 到 600 倍：校验加上读字段只要几十纳秒，不拷贝也不解析。HTTP 多了一次转换，慢 10% 到 50%，也就是每个请求多 2 到 5 微秒，而一次数据库查询要毫秒级。将来游戏的高频包走的正是快的那条路。

### 转换规则

- **JSON → 表**：schema 里没有的键和 null 都会被忽略。类型或范围不对的字段会被**跳过并记下错误**（`posts.0.id`、`out_of_range`），但其余字段照常转换。这样处理函数仍然能一次报出所有出错的字段，和 JSON 版的校验一样。JSON 里有的标量一律写进 buffer，所以请求里的标量都声明成可选（`= null`），才能区分"没发"和"发了 0"。
- **表 → JSON**：每个字段都会输出。buffer 里没有的，按"JSON 里什么都没说"来写：字符串是 `""`，数组是 `[]`，表和可选标量是 `null`，其余标量是默认值。
- **`json_null`**：JSON 可以用 null 表示"删除"，FlatBuffers 表达不了。请求里的 null 会变成空字符串、空表或空数组，处理函数看到"有、但是空的"就知道要删除。回答里空的或者缺少的值输出为 null。例子见 [Admin.fbs](../apps/blog/server/schema/Admin.fbs) 里的 `publishAt` 和翻译。标在可选标量上时，null 存成 0，回答里的 0 和缺少都输出为 null：网盘的 `parentId: null` 是"移到根目录"，和没发（"不动"）不一样，而 0 本来就不是哪个文件夹的 id。
- **`json_omit`**：回答里有些字段只有部分对象才有（用户详情的 `sessions`）。标了这个属性，缺少时 JSON 里就不出现这个键。
- **`json_closed`**：对象的键必须都是表里的字段，否则报 `unknown_key`。用在语言表这类地方，`translations: {de: …}` 这种写错的键应该报错，而不是悄悄丢掉。
- **`json_map`**：JSON 里以任意字符串为键的对象（按图片地址查尺寸、按表情数表态），FlatBuffers 没有对应的类型。标了这个属性的字段是一组表，表里有一个叫 `key` 的字符串字段：JSON 里每一项变成 `key: 值`。表里除 `key` 只有一个字段时，值就是这个字段的值（`{"like": 3}`）；有好几个字段时，值是由它们组成的对象（`{"/a.png": {"width": 1, "height": 2}}`）。出错的值按键报告（`counts.like`）。
- 不支持 struct、union 和定长数组，`Load()` 会指出是哪个字段。

## 5. arena：游戏消息也是包

[apps/arena](../apps/arena) 的消息原来是 `{"type": ...}` 的 JSON，2026-11-02 起换成了包，给原生客户端（[KCP](33-kcp.md)、UE5）用。所有消息都定义在 [Arena.fbs](../apps/arena/schema/Arena.fbs) 里：一个 `Opcode` 枚举，客户端发的是 0x00xx（`Ping`、`Input`、`Chat`），服务器发的是 0x01xx（`Welcome`、`Pong`、`State`、`ChatMessage`、`SystemMessage`），每个 opcode 对应一个同名的表。包头沿用第 3 节的 8 字节，requestId 一律是 0：arena 没有请求和回答，`Pong` 带回 `Ping` 的 `t` 就够了。

| 连接 | 收 | 发 |
|---|---|---|
| KCP | 包 | 包 |
| WebSocket，`/arena?ticket=...`（网页） | 文本帧是 JSON，二进制帧是包 | JSON |
| WebSocket，`&format=packets`（连不上 UDP 的原生客户端） | 同上 | 包 |

网页一行没改：JSON 的 `type` 和字段名都和原来一样。[ArenaPackets.cpp](../apps/arena/ArenaPackets.cpp) 里有一张小表，给每个 opcode 起 JSON 里的名字（客户端的 `Chat` 和服务器的 `ChatMessage` 都叫 `chat`），转换由 `FlatJsonSchema` 按 schema 完成，不用为每个消息写代码。

- **读**：`ReadClientPacket` / `ReadClientJson` 在网络线程上把消息变成 `ClientMessage`（opcode + 校验过的表），世界线程上的处理函数直接拿到表：`void HandleInput(Arena::Input const& input)`。不认识的 opcode 或 type 忽略（可能是更新的客户端）；不是包、表校验不过、不是 JSON 就以 1007 关闭。
- **写**：处理函数和 World 用生成代码构建表，包成 `ServerPacket`。广播（每帧的 `State`）只构建一次；发给网页的 JSON 在第一次需要时转换一次，之后发给其他网页都用这一份。
- **大小**：一个玩家、6 个金币时，`State` 的包是 228 字节，同样内容的 JSON 是 542 字节。

注意的地方：

- **flatc 只为有 `root_type` 的 schema 生成二进制 schema**，生成的类型以根表命名（`Arena::StateBinarySchema`）。包靠 opcode 说明是哪个表，`root_type` 只是为了让它生成。
- **JSON 的键按字母顺序输出**：反射里的字段按名字排序。按名字取字段的代码不受影响。
- **包里的字符串可以是任意字节**：WebSocket 的文本帧已经检查过 UTF-8，二进制帧和 KCP 没有，所以聊天的处理函数自己检查。
- **float 在 JSON 里会变成很长的小数**（`858.4244384765625`）：float 先转成 double 再输出，原来的 JSON 也是这样。包里就是 4 个字节。

## 6. 常见的坑

- **校验必须在分发之前，并且由框架统一做**：不能指望每个处理函数都记得自己校验。
- **包头长度会影响对齐**：见第 3 节。在 JS 里用 `Buffer.subarray(8)` 取 payload 不会有问题。但如果自己的协议在 payload 前面加了别的东西，就要保证 payload 的起点按 8 对齐。
- **Boost.JSON 会把无符号整数存成 `uint64`**：`{ "id", post.Id }`（`uint32`）的 `is_int64()` 返回 false。转换时两种都要接受，否则所有 id 都会报 `out_of_range`。
- **默认值不会写进 buffer**：用生成代码构建时，等于默认值的字段不会写入，flatc 输出的 JSON 里也就没有这个字段（读取时访问器照样返回默认值）。要表达"有没有"，就把字段声明成可选标量。
- **worker 线程上构建的回答要交出字节**：`OnWorker` 的结果要能移动出来再交回主线程，`FlatBufferBuilder` 不行，所以返回 `Bytes(builder)`，回到主线程再 `Respond(payload)`。
- **生成的表私有继承 `flatbuffers::Table`**：要按通用的表处理（比如 `IsEmptyTable`），得 `reinterpret_cast`，FlatBuffers 自己也是这样做的。
- **字段名用 camelCase**：接口的表和 JSON 的键一一对应，所以 flatc 关于 snake_case 的警告用 `--no-warnings` 关掉了。生成的 C++ 访问器名字也是 camelCase。
- **schema 互相 include 时，改一个要重新生成全部**：`rooftree_add_flatbuffers` 让每个 schema 的生成命令依赖同一组的所有 schema。
- **JSON 里以任意值为键的对象，和"null 表示根目录"这样的约定**：分别用 `json_map` 和标在标量上的 `json_null`，见第 4 节的转换规则。

## 7. 测试与示例

- [tests/test_Packets.cpp](../tests/test_Packets.cpp)：包头、`Error` 包；`FlatJsonSchema` 双向转换（可选标量、默认值、枚举名、`json_null`、`json_omit`、`json_closed`、`json_map`、嵌套表和数组），每一种错误的字段路径，以及出错的字段跳过、其余字段保留。
- [benchmarks/bench_Packets.cpp](../benchmarks/bench_Packets.cpp)：第 4 节的表格，`./benchmarks "[packet]"`。
- [tests/test_ServerNetwork.cpp](../tests/test_ServerNetwork.cpp)："WorldWebSocket routes packets by opcode"：用 Beast 客户端测试原生处理函数、回答带回 requestId、未知 opcode、AntiDOS 限额、校验不过就以 1007 关闭，同一条连接上 JSON 消息照常工作。
- [examples/arena_kcp_client.cpp](../examples/arena_kcp_client.cpp)：arena 的原生客户端，用 flatc 生成的头文件构建和校验包，见第 5 节和 [33](33-kcp.md)。
- [apps/blog/tests/packets.test.mjs](../apps/blog/tests/packets.test.mjs)：Admin.fbs 和 Reader.fbs 里的操作。请求和回答都由 `flatc` 命令行生成和解析，不经过服务器的代码；然后与同一操作的 HTTP 结果逐个字段比较。同时测试没登录的连接（读者的读操作可以调，其余 401）、403、被封禁时的 `details`、登出后的情况。`FLATC` 环境变量指定 flatc 的路径。
- [apps/blog/tests/golden.mjs](../apps/blog/tests/golden.mjs)：不是测试，是改"回答怎么生成"时用的对照工具。它在新数据库上按固定顺序调用操作表里的每个接口（后台的和读者的），记下 HTTP 的 JSON 和 `Location`、`Retry-After` 等头。改之前和改之后各跑一次，然后比较两份结果（`BLOG_BIN` 指定旧的二进制）。后台处理函数改成读写表时，98 个回答里只有空站点资料那一个变了：由 `{}` 变成每个字段都在的空资料。读者接口改成操作时，328 个回答里只有改过 slug 的文章变了：301 的正文由 `{"redirect": ...}` 变成 `{"error": {"code": "moved", "redirect": ..., "location": ...}}`，`Location` 头不变（浏览器的 fetch 会自动跟过去，用不到正文）。

## 8. 思考题与练习

1. 给博客加一个服务器主动推送的包：发布文章时，向所有管理员的连接发送 requestId 为 0 的 `Published` 包。需要在哪里记下"这条连接是管理员的"？
2. 用 `flatc --ts` 为 Reader.fbs 生成 TypeScript 代码，写一个最小的浏览器页面，不登录也能通过包列出文章。
3. 用 benchmark 的写法测一次"包 → 处理函数 → 包"的完整路径（包括 `WebSocketPacketMessage` 入队和 `SendPacket`），协议之外的开销有多大？
4. flags 的第一位用来表示"payload 经过 zstd 压缩"，服务端和客户端分别要改哪里？压缩应该放在校验之前还是之后？
