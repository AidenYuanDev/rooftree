# Rooftree

**Rooftree** 是旧时对房屋主梁的称呼，就是撑起整座屋顶的那根木头。它是一个 C++ 服务端框架：网络、数据库、调度、日志、配置、加密和主线程上的会话核心，上面跑着一个双语博客和一个多人小游戏，目标是支撑自己的游戏服务器。

它分成 `rooftree-common` / `rooftree-network` / `rooftree-database` 三个静态库，再加上 `rooftree-server`。

- 许可证：GPL-3.0 或更新版本（见 `COPYING`）；第三方组件见 `NOTICE`
- 网络和数据库核心用 C++20 协程，配置用 yaml-cpp，数据库用 PostgreSQL

## 快速开始

依赖都用系统的包，Arch 上：`yay -S boost openssl postgresql-libs fmt yaml-cpp catch2 argon2 utf8cpp curl readline jemalloc flatbuffers qrencode`。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

依赖的版本要求、数据库测试、性能测试和全部 CMake 选项见 [docs/34](docs/34-layout-and-build.md)。

## 上面跑着什么

- **`apps/blog`**：中英双语的个人博客，账号、文章、评论、实时协作、聊天、网盘都在框架的主线程会话里处理，不用 nginx 直接对外。开发和上线见 [apps/blog/README.md](apps/blog/README.md)。
- **`apps/arena`**：多人网页小游戏，浏览器走 WebSocket，原生客户端走 KCP，金币存进 PostgreSQL。
- **`examples/`**：每个程序只演示一个模块：echo、HTTP、WebSocket、调度器、数据库、KCP 客户端。

运行方法见 [docs/35](docs/35-examples-and-apps.md)。

## 文档

- [docs/00-overview.md](docs/00-overview.md)：分层、线程模型、按学习顺序排列的组件文档，以及容易踩的坑。从这里开始。
- [docs/34-layout-and-build.md](docs/34-layout-and-build.md)：目录结构与构建。
- [docs/35-examples-and-apps.md](docs/35-examples-and-apps.md)：示例与应用。
