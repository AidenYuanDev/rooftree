# 34 · 目录结构与构建

仓库里每个目录放什么、依赖怎么接进来、怎样构建和跑测试，以及所有 CMake 选项。

## 1. 目录结构

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
| `src/common/network/kcp` | `rooftree-network` | **新写**：UDP 上的游戏连接（Kcp::Server / Kcp::Client）：KCP 可靠消息 + 不可靠消息，AES-GCM 数据报，见 [33](33-kcp.md) |
| `src/database` | `rooftree-database` | **新写**：PostgreSQL（libpq）连接池、预处理语句、事务、QueryHolder、异步回调 |
| `src/server/shared` | `rooftree-server` | **新写**：主线程上的会话核心（SessionMgr、AntiDOS、UpdateLoop/FreezeDetector、FixedTicker）、HTTP/WebSocket 接入、内存静态站点、HTTP 客户端、控制台、FlatBuffers 包、安全工具和图形验证码、限流、SMTP 邮件 |
| `contrib/valgrind` | — | helgrind 屏蔽规则（`helgrind.supp`） |
| `docs` | — | 各基础组件的学习文档，按学习顺序编号，从 [docs/00-overview.md](00-overview.md) 开始 |
| `cmake` | — | **新写**：选项、编译器设置、`CollectAndAddSourceFiles`、配置汇总 |
| `deps` | — | 每个系统依赖一个子目录，包装成同名 INTERFACE 目标（`boost`、`postgresql`、`fmt`…）；`deps/kcp` 是唯一自带源码的 |
| `examples` | — | echo_server、http_server、websocket_server、arena_kcp_client、db_query、scheduler_demo + `rooftree.yaml.dist` |
| `apps/arena` | — | 一个完整的多人网页小游戏，把所有模块串起来，见 [35 · 示例与应用](35-examples-and-apps.md) |
| `apps/blog` | — | 中英双语博客：rooftree-server 上的 JSON API + React 预渲染前端 + 部署配置，见 [apps/blog/README.md](../apps/blog/README.md) |
| `tests` | — | Catch2 测试；`test_Network.cpp` 是 SocketMgr / HttpService / WebSocket 的本机回环测试；`test_Database.cpp` 需要 PostgreSQL；`test_ServerCore`、`test_ServerNetwork`、`test_Console`、`test_Packets`、`test_Kcp`、`test_StaticSite`、`test_Captcha`、`test_HttpRequest` 覆盖服务端的各个模块，一共 119 个用例 |

每个库的 `PrecompiledHeaders/` 下是它的预编译头。`rooftree` 是一个把所有库都带上的 INTERFACE 目标，只想用其中一部分时直接链接对应的库即可。

头文件都是按文件名直接 include（`#include "Log.h"`）：源文件由 `CollectAndAddSourceFiles` 递归收集，每个目录都作为 HEADERS file set 的 base dir 加进 include 路径，所以不用写目录。

## 2. 构建

依赖：Boost ≥ 1.83（用到 Boost.JSON、Boost.URL）、OpenSSL ≥ 3.2、libpq ≥ 14（PostgreSQL 的客户端库）、fmt、yaml-cpp、Catch2、libargon2、utf8cpp 4、libcurl、readline、jemalloc、FlatBuffers（库和 flatc，构建时生成包的代码，见 [32](32-packets.md)），博客（`APPS`）另需 libqrencode（Arch：`yay -S boost openssl postgresql-libs fmt yaml-cpp catch2 argon2 utf8cpp curl readline jemalloc flatbuffers qrencode`）。
除 KCP 外全部使用系统包，不会在构建时下载任何东西。KCP 只有一个 C 文件，没有发行版的包，原样放在 `deps/kcp`（1.7.1，MIT）。

每个依赖在 `deps/<名字>/` 里 `find_package` 后包装成同名 INTERFACE 目标，库只链接 `boost`、`openssl`、`postgresql` 这类名字。和某个依赖相关的编译宏也放在它自己的目录里，例如 `BOOST_ASIO_NO_DEPRECATED` 在 `deps/boost`，`FLATBUFFERS_*` 在 `deps/flatbuffers`。以后换掉某个依赖的来源（系统包、自带源码、FetchContent），只需要改它自己的目录。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

测试要串行跑（不加 `-j`）：日志的用例共用同一个临时文件 `rooftree_test.log`，网络的用例依赖时序，并行时偶尔失败或卡住。

数据库测试默认跳过（CTest 中显示为 Skipped），在 `build/bin/rooftree_test.yaml` 的 `Database.Test` 里填好 `Host`、`User`、`Password`、`Name` 后才会运行。这个文件在配置时从 `tests/rooftree_test.yaml.dist` 复制一次，本地修改不会被覆盖，也不会进 git；也可以用 `ROOFTREE_TEST_CONFIG=/path/to/file.yaml` 指定别的文件，或用 `ROOFTREE_DATABASE_TEST_PASSWORD` 这类环境变量逐项覆盖。测试会在该库里创建并删除 `rooftree_test`、`rooftree_test_logs` 表。

性能测试在 `benchmarks/`，用 Catch2 自带的 `BENCHMARK`，默认不构建，也不接入 CTest（计时只有在机器空闲、优化构建下单独跑才有意义）。覆盖 thread、log、base/config、sched、crypto、network、database、server、packet、gameweb（游戏和网页在同一帧，见 [29](29-server-core.md) 3.1.5）十个部分，可以按标签只跑其中一部分。数据库部分同样读取 `rooftree_test.yaml`，会创建并删除 `rooftree_bench` 表：

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
