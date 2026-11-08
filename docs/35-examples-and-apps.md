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

`apps/arena` 是一个多人网页小游戏（Coin Arena）：玩家在浏览器里登录，在同一张地图上移动、捡金币、聊天，金币存到 PostgreSQL。它的作用是演示怎样在这些模块之上写一个游戏服务器：

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

```sh
createdb arena   # 库要先建好，表由服务器创建
# 在 build/bin/arena.yaml 的 Database.Arena 里填好 Host、User、Password、Name
build/bin/arena                                          # 然后浏览器打开 http://127.0.0.1:8090/
kill -HUP $(pidof arena)                                 # 重新加载配置
```

页面和建表脚本在构建时复制到 `build/bin/arena-www/` 和 `build/bin/arena.sql`。页面每次请求都会重新读取，改完重新构建、刷新浏览器就能看到。部署到公网时，在前面放一个 nginx 处理 HTTPS/WSS，再把 `Arena.WebSocket.PublicUrl` 设为 `wss://...`。

`apps/blog` 是一个中英双语、油画（山坡小屋，昼夜两版）主题的个人博客，演示 `rooftree-server`（[29](29-server-core.md)）怎样承载一个 Web 应用：所有 HTTP 请求和 WebSocket 消息都在主线程上按客户端地址分会话处理，有每帧上限和 AntiDOS；账号（邮箱验证、管理员 TOTP、版主和写作者）、文章（译文、旧地址跳转）、多人实时协作和版本历史、时间线、评论（两层、段落评论、封禁）、表情回应、友链审核、搜索、上传、网盘、RSS、在线人数和页面实时更新、好友和聊天、邮件订阅。读者和后台的操作登记在一张操作表里，HTTP 和 WebSocket 二进制包走同一份处理函数，后台的控制台命令也调用它们（[31](31-console.md)、[32](32-packets.md)）。前端是 React Router 预渲染的静态页面，发布文章后服务器自动重新构建；前面不用 nginx，页面从内存回复、TLS、WebSocket 升级和连接防护都由服务器自己做（[30](30-static-files-and-tls.md)）。本机开发、测试和上线步骤见 [apps/blog/README.md](../apps/blog/README.md)。
