# Rooftree

**Rooftree** 是旧时对房屋主梁的称呼，就是撑起整座屋顶的那根木头。它是一个 C++ 服务端框架：网络、数据库、调度、日志、配置、加密和主线程上的会话核心，上面跑着一个双语博客和一个多人小游戏，目标是支撑自己的游戏服务器。

它分成 `rooftree-common` / `rooftree-network` / `rooftree-database` 三个静态库，再加上 `rooftree-server`。

- 许可证：GPL-3.0 或更新版本（见 `COPYING`）；第三方组件见 `NOTICE`
- 网络和数据库核心用 C++20 协程，配置用 yaml-cpp，数据库用 PostgreSQL

想系统地学习这些组件，从 [docs/00-overview.md](docs/00-overview.md) 开始：它给出了分层、线程模型和按顺序排列的 33 篇组件文档。

## 目录结构

目录和库的划分：`common` 是基础库，`network` 放在 `common` 下面但单独成库，`database` 只依赖 `common`。

| 目录 | 库 | 内容 |
|---|---|---|
| `src/common/base` | `rooftree-common` | **新写**：Define、Errors、Util、StringFormat、StringConvert、Random、Timer、MessageBuffer |
| `src/common/asio` | `rooftree-common` | **新写**：IoContext、Strand、DeadlineTimer、AsioHacksFwd |
| `src/common/thread` | `rooftree-common` | **新写**：ThreadPool、LockedQueue、ProducerConsumerQueue、MPSCQueue |
| `src/common/sched` | `rooftree-common` | **新写**：TaskScheduler、EventMap、AsyncCallbackProcessor；协程（Task.h） |
| `src/common/crypto` | `rooftree-common` | **新写**：SHA-1/SHA-256、Argon2、TOTP、CryptoRandom |
| `src/common/log` | `rooftree-common` | **新写**：日志：Appender（控制台/文件）、Logger、按层级回退、strand 异步 |
| `src/common/config` | `rooftree-common` | **新写**：yaml-cpp 实现，不可变快照 + `Get<T>` 结构化读取 + `OnLoaded` 重载通知 |
| `src/common/network` | `rooftree-network` | **新写**：Socket、AsyncAcceptor、SocketMgr、NetworkThread、SslStream（握手协程 `AcceptTls`）、IpAddress |
| `src/common/network/http` | `rooftree-network` | 基于 Boost.Beast 的 HTTP 服务（HttpService、HttpSocket、HttpSslSocket）；**新增**路由树和请求工具（HttpRouter、HttpRequestUtils） |
| `src/common/network/websocket` | `rooftree-network` | **新写**：WebSocket 服务端（RFC 6455），在 `Socket` 之上实现握手与帧，任意线程可发送 |
| `src/common/network/kcp` | `rooftree-network` | **新写**：UDP 上的游戏连接（Kcp::Server / Kcp::Client）：KCP 可靠消息 + 不可靠消息，AES-GCM 数据报，见 [docs/33](docs/33-kcp.md) |
| `src/database` | `rooftree-database` | **新写**：PostgreSQL（libpq）连接池、预处理语句、事务、QueryHolder、异步回调 |
| `src/server/shared` | `rooftree-server` | **新写**：主线程上的会话核心（SessionMgr、AntiDOS、UpdateLoop/FreezeDetector、FixedTicker）、HTTP/WebSocket 接入、内存静态站点、HTTP 客户端、控制台、FlatBuffers 包、安全工具和图形验证码、限流、SMTP 邮件 |
| `contrib/valgrind` | — | helgrind 屏蔽规则（`helgrind.supp`） |
| `docs` | — | 各基础组件的学习文档，按学习顺序编号，从 [docs/00-overview.md](docs/00-overview.md) 开始 |
| `cmake` | — | **新写**：选项、编译器设置、`CollectAndAddSourceFiles`、配置汇总 |
| `deps` | — | 每个系统依赖一个子目录，包装成同名 INTERFACE 目标（`boost`、`postgresql`、`fmt`…）；`deps/kcp` 是唯一自带源码的 |
| `examples` | — | echo_server、http_server、websocket_server、arena_kcp_client、db_query、scheduler_demo + `rooftree.yaml.dist` |
| `apps/arena` | — | 一个完整的多人网页小游戏，把所有模块串起来，见下方"应用" |
| `apps/blog` | — | 中英双语博客：rooftree-server 上的 JSON API + React 预渲染前端 + 部署配置，见 [apps/blog/README.md](apps/blog/README.md) |
| `tests` | — | Catch2 测试；`test_Network.cpp` 是 SocketMgr / HttpService / WebSocket 的本机回环测试；`test_Database.cpp` 需要 PostgreSQL；`test_ServerCore`、`test_ServerNetwork`、`test_Console`、`test_Packets`、`test_Kcp`、`test_StaticSite`、`test_Captcha`、`test_HttpRequest` 覆盖新写的模块，一共 127 个用例 |

每个库的 `PrecompiledHeaders/` 下是它的预编译头。`rooftree` 是一个把所有库都带上的 INTERFACE 目标，只想用其中一部分时直接链接对应的库即可。

头文件都是按文件名直接 include（`#include "Log.h"`）：源文件由 `CollectAndAddSourceFiles` 递归收集，每个目录都作为 HEADERS file set 的 base dir 加进 include 路径，所以不用写目录。

## 构建

依赖：Boost ≥ 1.83（用到 Boost.JSON、Boost.URL）、OpenSSL ≥ 3.2、libpq ≥ 14（PostgreSQL 的客户端库）、fmt、yaml-cpp、Catch2、libargon2、utf8cpp 4、libcurl、readline、jemalloc、FlatBuffers（库和 flatc，构建时生成包的代码，见 [docs/32](docs/32-packets.md)），博客（`APPS`）另需 libqrencode（Arch：`yay -S boost openssl postgresql-libs fmt yaml-cpp catch2 argon2 utf8cpp curl readline jemalloc flatbuffers qrencode`）。
除 KCP 外全部使用系统包，不会在构建时下载任何东西。KCP 只有一个 C 文件，没有发行版的包，原样放在 `deps/kcp`（1.7.1，MIT）。

每个依赖在 `deps/<名字>/` 里 `find_package` 后包装成同名 INTERFACE 目标，库只链接 `boost`、`openssl`、`postgresql` 这类名字。和某个依赖相关的编译宏也放在它自己的目录里，例如 `BOOST_ASIO_NO_DEPRECATED` 在 `deps/boost`，`FLATBUFFERS_*` 在 `deps/flatbuffers`。以后换掉某个依赖的来源（系统包、自带源码、FetchContent），只需要改它自己的目录。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

测试要串行跑（不加 `-j`）：日志的用例共用同一个临时文件 `rooftree_test.log`，网络的用例依赖时序，并行时偶尔失败或卡住。

数据库测试默认跳过（CTest 中显示为 Skipped），在 `build/bin/rooftree_test.yaml` 的 `Database.Test` 里填好 `Host`、`User`、`Password`、`Name` 后才会运行。这个文件在配置时从 `tests/rooftree_test.yaml.dist` 复制一次，本地修改不会被覆盖，也不会进 git；也可以用 `ROOFTREE_TEST_CONFIG=/path/to/file.yaml` 指定别的文件，或用 `ROOFTREE_DATABASE_TEST_PASSWORD` 这类环境变量逐项覆盖。测试会在该库里创建并删除 `rooftree_test`、`rooftree_test_logs` 表。

性能测试在 `benchmarks/`，用 Catch2 自带的 `BENCHMARK`，默认不构建，也不接入 CTest（计时只有在机器空闲、优化构建下单独跑才有意义）。覆盖 thread、log、base/config、sched、crypto、network、database、server、packet、gameweb（游戏和网页在同一帧，见 [docs/29](docs/29-server-core.md) 3.1.5）十个部分，可以按标签只跑其中一部分。数据库部分同样读取 `rooftree_test.yaml`，会创建并删除 `rooftree_bench` 表：

```sh
cmake -S . -B build -G Ninja -DBUILD_BENCHMARKS=ON
cmake --build build --target benchmarks
build/bin/benchmarks                          # 全部，默认每项 100 个样本
build/bin/benchmarks "[thread]"               # 只跑某个模块
build/bin/benchmarks --benchmark-samples 10   # 快速过一遍
```

所有可执行文件都输出到 `build/bin/`。`cmake/` 下四个文件：

- `Options.cmake`：下表的选项
- `Compiler.cmake`：GCC 12 / Clang 16 以上、C++20、输出目录、链接器；警告和 sanitizer 都挂在 INTERFACE 目标 `rooftree-core-interface` 上，我们自己的每个目标都链接它，系统的库不受影响
- `SourceFiles.cmake`：`CollectAndAddSourceFiles`，把一个目录下的源文件和头文件（含子目录）加进目标
- `Summary.cmake`：配置时打印一份选项汇总

| 选项 | 默认 | 说明 |
|---|---|---|
| `EXAMPLES` | ON | 构建示例 |
| `APPS` | ON | 构建 `apps/` 下的应用（需要 Boost.JSON） |
| `BUILD_TESTING` | ON | 构建测试 |
| `BUILD_BENCHMARKS` | OFF | 构建性能测试（`benchmarks/`） |
| `USE_COREPCH` | ON | 三个库使用预编译头 |
| `WITH_WARNINGS` | ON | `-Wall -Wextra` 等 |
| `WITH_WARNINGS_AS_ERRORS` | OFF | 警告视为错误，GCC 和 Clang 下都能零警告通过 |
| `NOJEM` | OFF | 不链接 jemalloc。默认所有可执行文件都用 jemalloc 替换 `malloc`；开启 `ASAN`/`TSAN`/`MSAN`/`HELGRIND` 时会自动关闭，因为 sanitizer 要自己接管 `malloc`，valgrind 默认只跟踪 libc 的 `malloc` |
| `WITH_COREDEBUG` | OFF | 加 `-g3`，调试信息里带上宏定义 |
| `HELGRIND` | OFF | 定义 `HELGRIND`，把 libstdc++ 的同步标记接到 helgrind 注解上（`Define.h`），需要 valgrind 头文件（`deps/valgrind`）。配合 `valgrind --tool=helgrind --suppressions=contrib/valgrind/helgrind.supp` 使用：Boost.Asio 内部用自己的 futex 锁，helgrind 看不见，会产生大量误报，这个文件把它们屏蔽掉（只匹配栈顶在 Asio 内部的访问，回调代码里的真实竞争仍会报告） |
| `ASAN` / `UBSAN` / `TSAN` / `MSAN` | OFF | 各种 sanitizer，`MSAN` 仅 Clang |
| `WITH_LINKER` | 空 | 传给 `-fuse-ld=`，例如 `mold`、`lld` |

## 示例

在 `build/bin` 目录下运行，会读取同目录的 `rooftree.yaml`。仓库里只有 `examples/rooftree.yaml.dist`：每次配置都会刷新 `build/bin/rooftree.yaml.dist`，而 `rooftree.yaml` 只在不存在时从它复制一次，所以本地修改（数据库密码等）不会被覆盖，也不会进 git。配置文件里的每个值都可以用环境变量覆盖（加载时读取），例如 `Echo.Port` → `ROOFTREE_ECHO_PORT`。

| 程序 | 链接的库 | 演示内容 | 试一下 |
|---|---|---|---|
| `echo_server` | `rooftree-network` | Socket / NetworkThread / SocketMgr / 连接初始化链，结构和 WorldSocketMgr 一致 | 用一个保持连接的客户端，见下方"注意点 1" |
| `http_server` | `rooftree-network` | HttpService + 路由 + Session，结构和 bnetserver 的 LoginRESTService 一致 | `curl -i localhost:8080/hello`，`curl -d ping localhost:8080/echo` |
| `scheduler_demo` | `rooftree-common` | TaskScheduler / EventMap 在固定 tick 主循环里的用法 | 直接运行 |
| `websocket_server` | `rooftree-network` | WebSocket::BaseSocket：握手、`OnHandshake` 拒绝连接、消息回显 | 浏览器打开 `build/bin/websocket.html` |
| `arena_kcp_client` | `rooftree-network`、flatbuffers | Kcp::Client：arena 的原生客户端，登录、KCP 握手、可靠和不可靠消息，FlatBuffers 包 | arena 运行时 `build/bin/arena_kcp_client <用户名> <密码>` |
| `db_query` | `rooftree-database` | 定义自己的 DatabaseConnection、同步查询、事务、异步查询 + 回调回到主线程 | 先在 `rooftree.yaml` 的 `Database.Demo` 里填 `Host`、`User`、`Password`、`Name`，每项都可以用环境变量覆盖，例如 `ROOFTREE_DATABASE_DEMO_PASSWORD` |

## 应用

`apps/arena` 是一个多人网页小游戏（Coin Arena）：玩家在浏览器里登录，在同一张地图上移动、捡金币、聊天，金币存到 PostgreSQL。它的作用是演示怎样在这些模块之上写一个游戏服务器：

| 部分 | 文件 | 用到的模块 |
|---|---|---|
| HTTP：页面、注册、登录、排行榜 | `AuthService` | HttpService；Argon2id；耗时的处理放进 ThreadPool，返回 `Async`，结果再投递回连接所在的网络线程 |
| 登录票据 | `TicketMgr` | CryptoRandom，一次性、60 秒有效 |
| 消息 | `Arena.fbs`、`ArenaPackets` | FlatBuffers 包（opcode + 表），网络线程上校验；网页的 JSON 在入口按 schema 转换（[docs/32](docs/32-packets.md)） |
| 游戏连接 | `ArenaSocket` | WebSocket::BaseSocket；在网络线程上读取 JSON 或包，排进 Session 的 MPSCQueue |
| 原生客户端的连接 | `ArenaKcp` | Kcp::Server：同一个 ticket 加登录返回的密钥，同样的包，每帧的 state 不可靠发送（[docs/33](docs/33-kcp.md)） |
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

`apps/blog` 是一个中英双语、油画（山坡小屋，昼夜两版）主题的个人博客，演示 `rooftree-server`（[docs/29](docs/29-server-core.md)）怎样承载一个 Web 应用：所有 HTTP 请求和 WebSocket 消息都在主线程上按客户端地址分会话处理，有每帧上限和 AntiDOS；账号（邮箱验证、管理员 TOTP、版主和写作者）、文章和日记（译文、旧地址跳转）、多人实时协作和版本历史、一句话、时间线、评论（两层、段落评论、封禁）、表情回应、友链审核、搜索、上传、网盘、RSS、在线人数和页面实时更新、好友和聊天、邮件订阅。读者和后台的操作登记在一张操作表里，HTTP 和 WebSocket 二进制包走同一份处理函数，后台的控制台命令也调用它们（[docs/31](docs/31-console.md)、[docs/32](docs/32-packets.md)）。前端是 React Router 预渲染的静态页面，发布文章后服务器自动重新构建；前面不用 nginx，页面从内存回复、TLS、WebSocket 升级和连接防护都由服务器自己做（[docs/30](docs/30-static-files-and-tls.md)）。本机开发、测试和上线步骤见 [apps/blog/README.md](apps/blog/README.md)。

## 注意点

1. **Socket 不支持半关闭**：读到 EOF 就立即 `CloseSocket()`，写队列里还没发出去的数据会被丢弃。`printf hi | nc host 7000` 这种"发完就 shutdown(SHUT_WR)"的客户端收不到回显，需要保持连接的客户端（游戏客户端正是如此）。
2. **写操作不是立即发出的**：`QueuePacket()` 只是入队，由 `NetworkThread` 每 1ms 调用一次 `Socket::Update()` 统一 flush（非 IOCP 平台）。这是为了合并小包。
3. **TaskScheduler 会补帧**：一次 `Update(350ms)` 会让周期 100ms 的任务执行 3 次，因为 `Repeat()` 是相对于任务自己的到期时间重新调度的（见 `tests/test_Framework.cpp`）。
4. **调度器都不自带线程**：TaskScheduler / EventMap / QueryCallbackProcessor 都依赖外部主循环调用 `Update(diff)` 或 `ProcessReadyCallbacks()`，这是"逻辑单线程 + IO/DB 多线程"模型的核心。
5. **数据库的预处理语句在 `PrepareStatements()` 时向服务器预编译**，所以表必须已经存在，写错的语句在启动时就会失败。
6. **`StopNetwork()` 与 acceptor 所在 io_context 的先后顺序**：如果 `Close()` 不等，就必须先停 io_context 再调用 `StopNetwork()`，否则被取消的 `async_accept` 回调会访问已经销毁的 `AsyncAcceptor`；即使按这个顺序，io_context 析构时仍会销毁一个持有已不存在网络线程 socket 的操作（ASan 发现）。Rooftree 的 `AsyncAcceptor::Close()` 会等这个操作完成，两种顺序现在都安全，见 [15 · 设计取舍](docs/15-async-acceptor.md#5-设计取舍)。
7. **日志重载和多线程写文件是安全的**（[docs/08](docs/08-log.md)）：运行中 `LoadFromConfig()` 时其他线程照常写日志，同步模式下多个线程写同一个文件，行不会交错。每行立即 `fflush`（崩溃不丢日志）；重载时文件 appender 按配置重新打开（`w` 模式会清空文件）；异步模式下重载前已排队、重载后才执行的消息会被丢弃。
