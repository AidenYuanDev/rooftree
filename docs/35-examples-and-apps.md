# 35 · 示例与应用

`examples/` 里每个程序只演示一个模块；`apps/` 里是两个完整的应用，把所有模块串起来。构建方法见 [34](34-layout-and-build.md)。

## 1. 示例

在 `build/bin` 目录下运行，会读取同目录的 `rooftree.yaml`。仓库里只有 `examples/rooftree.yaml.dist`：每次配置都会刷新 `build/bin/rooftree.yaml.dist`，而 `rooftree.yaml` 只在不存在时从它复制一次，所以本地修改（数据库密码等）不会被覆盖，也不会进 git。配置文件里的每个值都可以用环境变量覆盖（加载时读取），例如 `Echo.Port` → `ROOFTREE_ECHO_PORT`。

| 程序 | 链接的库 | 演示内容 | 试一下 |
|---|---|---|---|
| `echo_server` | `rooftree-network` | Socket / NetworkThread / SocketMgr / 连接的开始（`Start()`） | 用一个保持连接的客户端，见 [00 · 容易踩的坑](00-overview.md#6-容易踩的坑) 第 1 条 |
| `http_server` | `rooftree-network` | HttpService + 路由 + Session | `curl -i localhost:8080/hello`，`curl -d ping localhost:8080/echo` |
| `scheduler_demo` | `rooftree-common` | TaskScheduler / EventMap 在固定 tick 主循环里的用法 | 直接运行 |
| `websocket_server` | `rooftree-network` | WebSocket::BaseSocket：握手、`OnHandshake` 拒绝连接、消息回显 | 浏览器打开 `build/bin/websocket.html` |
| `arena_kcp_client` | `rooftree-network`、flatbuffers | Kcp::Client：arena 的原生客户端，登录、KCP 握手、可靠和不可靠消息，FlatBuffers 包 | arena 运行时 `build/bin/arena_kcp_client <用户名> <密码>` |
| `db_query` | `rooftree-database` | 定义自己的 DatabaseConnection、同步查询、事务、异步查询 + 回调回到主线程 | 先在 `rooftree.yaml` 的 `Database.Demo` 里填 `Host`、`User`、`Password`、`Name`，每项都可以用环境变量覆盖，例如 `ROOFTREE_DATABASE_DEMO_PASSWORD` |

## 2. 应用

`apps/arena` 是一个多人网页小游戏（Coin Arena）：玩家在浏览器里登录，在同一张地图上移动、捡金币、聊天、互相用技能打，金币存到 PostgreSQL。它的作用是演示怎样在这些模块之上写一个游戏服务器：

| 部分 | 文件 | 用到的模块 |
|---|---|---|
| HTTP：页面、注册、登录、排行榜 | `AuthService` | HttpService；Argon2id；耗时的处理放进 ThreadPool，返回 `Async`，结果再投递回连接所在的网络线程 |
| 登录票据 | `TicketMgr` | CryptoRandom，一次性、60 秒有效 |
| 消息 | `Arena.fbs`、`ArenaPackets` | FlatBuffers 包（opcode + 表），网络线程上校验；网页的 JSON 在入口按 schema 转换（[32](32-packets.md)） |
| 游戏连接 | `ArenaSocket` | WebSocket::BaseSocket；在网络线程上读取 JSON 或包，排进 Session 的 MPSCQueue |
| 原生客户端的连接 | `ArenaKcp` | Kcp::Server：同一个 ticket 加登录返回的密钥，同样的包，每帧的 state 不可靠发送（[33](33-kcp.md)） |
| 会话 | `ArenaSession` | 消息类型 → 处理函数 + 允许的状态（相当于 opcode 表）；异步加载角色；空闲超时；防刷屏 |
| 世界 | `World` | 固定帧率的主循环；TaskScheduler（刷金币、定时存档、数据库保活）；EventMap（定时的"淘金热"事件）；SIGHUP 时在帧间重载配置 |
| 数据库 | `ArenaDatabase`、`sql/arena.sql` | DatabaseWorkerPool、预处理语句、事务；启动时执行建表脚本 |
| 战斗 | `combat/`（库 `arena-combat`） | 不依赖网络和数据库：Unit（血、蓝、施法、冷却和公共冷却、Buff）、CombatMap（按 id 找单位，收集本帧的战斗事件）、SpellStore（技能表）。规则全在服务端，见下文 |

```sh
createdb arena   # 库要先建好，表由服务器创建
# 在 build/bin/arena.yaml 的 Database.Arena 里填好 Host、User、Password、Name
build/bin/arena                                          # 然后浏览器打开 http://127.0.0.1:8090/
kill -HUP $(pidof arena)                                 # 重新加载配置
```

页面和建表脚本在构建时复制到 `build/bin/arena-www/` 和 `build/bin/arena.sql`。页面每次请求都会重新读取，改完重新构建、刷新浏览器就能看到。部署到公网时，在前面放一个 nginx 处理 HTTPS/WSS，再把 `Arena.WebSocket.PublicUrl` 设为 `wss://...`。

### 2.1 战斗

Tab 锁定式：客户端只发请求，`SetTarget`（看着谁）、`CastSpell`（技能、目标、客户端自己的编号）、`CancelCast`，能不能放、打多少全由服务端决定：

- **开始施法时**检查：活着、没在读条、没被晕、读条技能要站着不动、技能冷却和公共冷却（1 秒）、蓝够、目标活着且在射程内（两个单位边缘之间的距离）。不行就只回给施法者一个 `CastFailed`，带原因和那个编号，什么都不开始。公共冷却在开始时就计时。
- **读条中**：移动、被晕、死亡、`CancelCast` 会打断。
- **读条结束时**目标要再检查一遍（可能已经死了、走远了、下线了），然后才扣蓝、开始冷却、执行效果（伤害、治疗、上 Buff），可以暴击（1.5 倍）。
- **Buff**：持续伤害每个周期跳一次，最后一跳在结束时；同一个人再上一次就从头计时，另一个人上的是另一个。晕：不能动、不能施法，正在读条的被打断。
- **死亡**：读条打断、Buff 清掉、记下击杀者；`Arena.Combat.RespawnTime` 毫秒后在别处满血复活。
- **自动攻击**：`Attack`（目标，0 是停）之后，目标在近战范围内就每 2 秒挥一刀（8-12，可暴击），不占公共冷却；读条、被晕、死了不挥，出了范围就等着，回到范围立刻挥。目标死了或离开、自己死了就停。每一刀是一个 `Swing` 事件，伤害跟着一个技能号为 0 的 `Damage`。

移动：`Input` 可以给任意方向（`x`、`y`，长度不管），服务端归一化后按 `Arena.World.PlayerSpeed` 走；不是有限数的当没有。没给方向时用老的 `dx`、`dy`（八个方向），网页和机器人还是这样发。`facing` 是玩家看的方向（弧度，场地的坐标系），跟着 `State` 发给别人；不给就是走的方向。

登录时 `Welcome` 之后发 `Spells`（技能表：读条、冷却、射程、耗蓝），客户端按它显示技能栏。每帧发生的战斗事件（开始读条、施法完成、打断、伤害、治疗、Buff 增减、死亡、挥刀）合进一个 `CombatLog` 可靠地发给看得见的人（见 2.2），`State` 里每个玩家多了血、蓝、目标、击杀和死亡数、朝向、在打谁。技能现在写在 `combat/SpellStore.cpp` 里。

测试：`build/bin/arena-tests`（ctest 也会跑），每条规则一个用例，伤害随机数用固定种子。

机器人：`build/bin/arena_bots <数量> [主机] [HTTP 端口] [秒数] [前缀]`。每个机器人注册（已有就跳过）并以 `<前缀><4 位编号>` 登录，走 KCP，找最近的活人打：双数号近战（走过去打、晕），单数号远程（站在射程里上毒、放火球），血少了就治疗。它们只按服务端发来的东西行动，被拒绝的施法会按原因计数。每 5 秒和结束时打印施法、拒绝原因、战斗事件、每个机器人每秒收到的字节。服务端每 10 秒在 `arena.world` 日志里记一次世界帧的平均和最长耗时。

作弊客户端：`build/bin/arena_rogue [主机] [HTTP 端口] [前缀]` 发各种不守规矩的包（坏的 FlatBuffers、越界的输入、冒名的施法、重复用票据……），检查服务端都挡住了，最后打印每项 PASS / FAIL。要对一个没别人在玩的服务器跑。

### 2.2 视野（AOI）

一个客户端只收到它的玩家看得见的东西：`State` 里只有 `Arena.World.ViewDistance`（默认 600）以内最近的 `Arena.World.MaxVisible`（默认 30，含自己）个玩家和这个距离内的金币，`CombatLog` 里只有施法者或目标看得见的事件。系统消息（谁杀了谁）还是发给所有人。

`combat/Visibility` 每帧在玩家移动之后算一次：按视野距离分格子，每人只看周围 3×3 格，取最近的；同时记下每个单位被谁看见，战斗事件按它分发。协议没变：网页和机器人照常工作，只是看到的人少了。

本机测过（4000×4000 的场地）：AOI 之前 500 人时有 182 个连接被 `3002 send buffer full` 断开；之后 300 人和 500 人都没有断线，世界一帧平均 1 ms 和 2.2 ms，一个 `State` 平均 19 人约 1.45 KB、28 人约 2 KB，每个客户端每秒约 41 KB。一个玩家在 `State` 里约 75 字节，超过 15 人就装不进一个 UDP 包、改走可靠通道（500 人时 98%），所以下一步是把每帧都变的（位置、血、蓝、目标）和很少变的（名字、上限、击杀）分开发。服务端每 10 秒在 `arena.world` 日志里记一次 `State` 的平均人数、字节数和走可靠通道的比例。

`apps/blog` 是一个中英双语、油画（山坡小屋，昼夜两版）主题的个人博客，演示 `rooftree-server`（[29](29-server-core.md)）怎样承载一个 Web 应用：所有 HTTP 请求和 WebSocket 消息都在主线程上按客户端地址分会话处理，有每帧上限和 AntiDOS；账号（邮箱验证、管理员 TOTP、版主和写作者）、文章（译文、旧地址跳转）、多人实时协作和版本历史、时间线、评论（两层、段落评论、封禁）、表情回应、友链审核、搜索、上传、网盘、RSS、在线人数和页面实时更新、好友和聊天、邮件订阅。读者和后台的操作登记在一张操作表里，HTTP 和 WebSocket 二进制包走同一份处理函数，后台的控制台命令也调用它们（[31](31-console.md)、[32](32-packets.md)）。前端是 React Router 预渲染的静态页面，发布文章后服务器自动重新构建；前面不用 nginx，页面从内存回复、TLS、WebSocket 升级和连接防护都由服务器自己做（[30](30-static-files-and-tls.md)）。本机开发、测试和上线步骤见 [apps/blog/README.md](../apps/blog/README.md)。
