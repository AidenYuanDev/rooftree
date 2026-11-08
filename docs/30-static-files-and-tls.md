# 30 · 静态文件与 TLS：不用 nginx 直接对外

## 1. 是什么

游戏服务器直接面对客户端，前面没有 nginx；Rooftree 的博客原来放在 nginx 后面，由 nginx 负责 HTTPS、静态页面和压缩。为了让框架自己也能直接对外（以后的游戏服同样如此），这些事改由服务器自己做：

- **静态文件**：整个站点读进内存，在网络线程上直接回复，不进主线程（本章 3.1–3.4）。
- **TLS**：同一个服务类型既可以明文也可以 TLS，证书续期后不用重启（本章 3.5–3.8）。

连接层的防护（超时、按地址限制连接数、写队列上限）是另外一部分，见 [16 · 3.6](16-socket.md#36-超时在-update-里比较时间) 和 [18 · 3.5](18-network-thread-and-socket-mgr.md#35-按地址限制连接数Rooftree)。

## 2. 核心组成

| 类 / 函数 | 作用 | 位置 |
|---|---|---|
| `StaticSite` | 一个目录里的所有文件，按 URL 路径（`/zh/index.html`）存在内存里 | [StaticSite.h](../src/server/shared/http/StaticSite.h) |
| `StaticSite::Load(directory, error)` | 读目录（本身可以是指向某个版本的符号链接），`name.gz` 当作 `name` 的 gzip 版本 | [StaticSite.cpp:89](../src/server/shared/http/StaticSite.cpp#L89) |
| `StaticSiteHolder` | 当前在用的站点，任意线程读，整体替换 | [:163](../src/server/shared/http/StaticSite.cpp#L163) |
| `GetRequestPath(request)` | 请求目标的路径，百分号解码、去掉查询，和路由用同一套检查 | [:173](../src/server/shared/http/StaticSite.cpp#L173) |
| `SetStaticResponse(context, file, cacheControl)` | 设置 Content-Type、ETag（命中 `If-None-Match` 回 304）、客户端接受时回 gzip | [:237](../src/server/shared/http/StaticSite.cpp#L237) |
| `ReadWholeFile(path)` | 从磁盘读一个文件，给不属于站点的东西用（上传的图片） | 同上 |
| `DispatcherService::SetFallbackHandler` | 没有路由匹配时调用它，而不是直接 404 | [HttpService.h](../src/common/network/http/HttpService.h) |
| `DispatcherService::SetDefaultHeaders` | 每个响应都带上的头（安全响应头） | [HttpService.h](../src/common/network/http/HttpService.h) |
| `OptionalSslStream<Wrapped>` | 运行时决定是 TLS 还是明文的流，接受连接时选 | [OptionalSslStream.h:23](../src/common/network/OptionalSslStream.h#L23) |
| `CreateServerSslContext(chain, key, error)` | 从 certbot 的 PEM 文件建 TLS context：只开 TLS 1.2/1.3、前向安全的 AEAD 套件 | [SslContext.cpp:34](../src/common/network/SslContext.cpp#L34) |
| `WorldHttpSettings::SslCertificateFile` / `SslPrivateKeyFile` | 设了就是 TLS 服务 | [WorldHttpService.h:143](../src/server/shared/http/WorldHttpService.h#L143) |
| `WorldHttpService::LoadSslContext(error)` | 读证书；`StartNetwork` 之前调用，续期后再调用 | [WorldHttpService.cpp:278](../src/server/shared/http/WorldHttpService.cpp#L278) |
| `WorldHttpSettings::RedirectToOrigin` / `RedirectExempt` | 明文服务把请求 301 到 TLS 地址，ACME 和豁免地址除外 | [WorldHttpService.h:148](../src/server/shared/http/WorldHttpService.h#L148) |

## 3. 关键实现

### 3.1 为什么能不进主线程

[29](29-server-core.md) 的 `WorldHttpService::Route` 注册的处理函数返回 `Async`：请求被包成消息投进 `SessionMgr`，在主线程上处理。但 `DispatcherService` 本来就支持另一种：处理函数在网络线程上直接填好响应，返回 `Handled`，`BaseSocket` 当场发出去（框架自己的 400/404/405 就是这样）。

静态文件用的就是这条路。它们不碰会话、AntiDOS、每帧上限和工作线程池，一个页面带出来的几十个脚本和字体对 API 没有任何影响。代价是 AntiDOS 也管不到它们，所以要先有连接层的防护（超时、按地址限制连接数）。

### 3.2 整个站点放在内存里

预渲染出来的博客只有十几 MB、四百多个文件，全部读进内存比每次读磁盘简单：

- 请求在网络线程上只做一次哈希表查找，不会因为磁盘慢而卡住这个线程上的其他连接。
- ETag 在加载时算好（内容的 SHA-256 前 128 位），gzip 版本由构建脚本提前生成（`gzip -k -9 -n`），服务时不压缩。
- **路径就是表里的键**：只有加载过的文件才能被回复，`../`、符号链接、编码绕过都到不了目录外面。这部分比"按路径去磁盘上找文件"安全得多。

新版本在别的线程加载好，`StaticSiteHolder::Set` 一次换上。正在处理的请求手里拿着旧站点的 `shared_ptr`，用完才释放，所以一个请求看到的要么全是旧版本、要么全是新版本。这和 `rebuild.sh` 切换 `current` 符号链接是同一个思路。

上传的图片会越来越多，不适合放内存，仍然从磁盘读（`ReadWholeFile`），也在网络线程上，读的时候会短暂占住这个线程。图片多了以后可以改成交给工作线程去读，读完直接在网络线程上回复，同样不经过主线程。

### 3.3 兜底处理函数

路由树的参数段 `{name}` 只匹配一段（[28](28-http-routing.md)），静态文件的路径层数不固定（`/zh/posts/hello/`、`/assets/x.js`），没法写成路由。所以加了一个兜底：路由表里找不到时，交给 `SetFallbackHandler` 设置的函数。路径对、方法不对仍然是 405，不会走兜底。

### 3.4 博客怎么用（apps/blog）

`RegisterStaticRoutes`（[StaticHandlers.cpp:117](../apps/blog/server/StaticHandlers.cpp#L117)）照搬了原来 `nginx.conf` 的规则：

| 路径 | 回复 | 缓存 |
|---|---|---|
| `/` | 302 到 `/zh` 或 `/en`（看 `Accept-Language`） | — |
| `/feed.xml` | 301 到 `/zh/feed.xml` | — |
| `/zh/feed.xml`、`/sitemap.xml` | 普通的主线程路由，和 `/api/feed.xml` 是同一个处理函数 | — |
| `/uploads/<name>` | 磁盘上的图片，名字必须是服务器自己起的 | 一年 |
| `/assets/...` | 构建产物，文件名带哈希；没有就 404 | 一年，`immutable` |
| `/zh/posts/-` 等占位页 | 404（让没有文章时构建也能通过的占位） | — |
| `....data` | 客户端导航的数据；没有就 404，不回退到 HTML | `no-cache` |
| 其他 | 文件本身 → `<路径>/index.html` → SPA 回退页 | `no-cache` |
| `/api/...` 没有匹配 | JSON 格式的 404 | — |

404 用 SPA 回退页作为内容，由前端渲染 404 页面。`no-cache` 的意思是浏览器每次都来问，版本没变时得到 304。

站点在三个时候加载（[BlogWorld.cpp:412](../apps/blog/server/BlogWorld.cpp#L412)）：启动时（等它加载完才开端口）、每次重建成功后（在重建线程上）、收到 SIGHUP 时（手动跑过 `rebuild.sh`）。读不了时记错误日志，继续用正在用的那个版本。

安全响应头（CSP、`X-Frame-Options` 等）用 `SetDefaultHeaders` 加在所有响应上，页面和 API 都有。

### 3.5 一个 socket 类型，TLS 或明文

如果在编译期选流，`Http::Socket`（明文）和 `Http::SslSocket`（TLS）是两个类型，一个服务只能是其中一种。博客要同时监听 80 和 443，而且两边路由一样，下一步还要在同一个 socket 上升级 WebSocket，用两个 socket 类型就得有两个 `SocketMgr`、两份上层代码。

`OptionalSslStream` 用 `std::variant` 装着两种流之一（[OptionalSslStream.h:23](../src/common/network/OptionalSslStream.h#L23)），把 `Socket<Stream>` 要用的每个操作 `std::visit` 转发过去，再加一个只有 TLS 才能调用的 `async_handshake`。`WorldHttpSocket` 就建在它上面，`WorldHttpService::CreateSocket` 按服务有没有证书决定给不给它 TLS context；`Start()` 里 TLS 的连接在初始化链最前面多一个 `SslHandshakeConnectionInitializer`（[WorldHttpService.cpp:42](../src/server/shared/http/WorldHttpService.cpp#L42)）。握手也在请求超时里（[19 · 3.5](19-http.md#35-超时Rooftree)）：连上后迟迟不握手的连接一样会被关闭。

### 3.6 TLS context

`CreateServerSslContext`（[SslContext.cpp:34](../src/common/network/SslContext.cpp#L34)）按 Mozilla 的 "intermediate" 配置：

- 只开 TLS 1.2 和 1.3（`SSL_CTX_set_min_proto_version`，再加上 `no_tlsv1`、`no_tlsv1_1`）；
- TLS 1.2 只用 ECDHE + AES-GCM / ChaCha20-Poly1305（[:16](../src/common/network/SslContext.cpp#L16)），去掉了需要 DH 参数文件的 DHE 套件；TLS 1.3 的套件 OpenSSL 默认就都是安全的；
- 证书文件用 `fullchain.pem`（证书加中间证书），否则有的客户端验证不了证书链；
- 读完再检查私钥是不是这张证书的。

不协商 ALPN，所以浏览器总是用 HTTP/1.1。Boost.Beast 不支持 HTTP/2，对博客影响不大：浏览器会对同一个域名多开几个连接。

### 3.7 续期证书：不用重启

context 建好后就不再改。`LoadSslContext` 读到新证书就建一个新的 context，在锁里替换 `shared_ptr`（[WorldHttpService.cpp:278](../src/server/shared/http/WorldHttpService.cpp#L278)）。之后接受的连接用新的；已经打开的连接各自持有旧 context 的 `shared_ptr`（OpenSSL 的 `SSL` 对象引用着它），用完才释放。新文件读不了、或者私钥和证书对不上，就返回 `false`，继续用当前的 context。

博客在收到 SIGHUP 时调用它（[Main.cpp:375](../apps/blog/server/Main.cpp#L375)）。certbot 续期成功后由 `--deploy-hook` 把证书复制到服务器能读的地方，再 `systemctl reload blog`。

### 3.8 80 端口：跳转和 ACME

TLS 服务之外，80 端口还要有一个明文服务：

- **跳转**：有人输入 `http://`，回 301 到 `RedirectToOrigin` 加上原来的路径（[WorldHttpService.cpp:54](../src/server/shared/http/WorldHttpService.cpp#L54)）。只拼接以 `/` 开头的目标，别的形式（`http://别的网站/`）一律跳到首页，避免变成开放跳转。
- **ACME 挑战**：Let's Encrypt 续期时会访问 `http://域名/.well-known/acme-challenge/<token>`。这些路径不跳转，由博客的路由从 certbot `--webroot` 的目录里读文件回复。这样续期时服务器不用停。
- **本机自己的请求**：`RedirectExempt` 里的地址照常处理。重建页面时 React Router 在本机通过 `http://127.0.0.1/api/...` 读文章，不能被跳走，所以博客把 127.0.0.1 和 ::1 设成豁免。

明文服务和 TLS 服务注册同样的路由（[Main.cpp:324](../apps/blog/server/Main.cpp#L324)），只是 TLS 服务多带一个 `Strict-Transport-Security` 头：浏览器记住一年内这个域名只用 https。

## 4. 线程与安全

| 数据 | 谁访问 | 同步方式 |
|---|---|---|
| `StaticSite` | 加载后只读，任意网络线程 | 不需要（不可变） |
| `StaticSiteHolder` 里的指针 | 网络线程读，加载线程写 | 互斥锁，只保护复制 `shared_ptr` 这一下 |
| 兜底函数、默认响应头 | 网络线程读 | 在 `StartNetwork` 之前设置，之后不变 |
| `WorldHttpService` 的 TLS context | 接受连接的线程读，`LoadSslContext`（任意线程）写 | 互斥锁；每个连接自己持有一份 `shared_ptr` |

## 5. 设计取舍

路由里没有的路径先交给兜底函数（博客用它回答静态文件），兜底也没有才 404；每个响应可以带一组默认头。`OptionalSslStream` 在 TLS 流 `SslStream`（握手协程 `AcceptTls`，[17](17-connection-start.md)）和明文流之间选一个。

## 6. 常见的坑

- **网络线程上不要做慢的事**：`ReadWholeFile` 会阻塞，只给小文件用。站点里的文件已经在内存里，没有这个问题。
- **手动重建后要通知服务器**：服务器自己启动的重建，结束后会自动加载新版本；手动跑 `rebuild.sh` 后要 `systemctl reload blog`（SIGHUP），否则继续回复旧版本。
- **静态请求不受 AntiDOS 限制**：靠连接层的超时和连接数限制挡住滥用。
- **私钥的权限**：certbot 生成的私钥只有 root 能读，服务器以 `blog` 用户运行。不要把服务器改成 root 运行，而是在 `--deploy-hook` 里把证书复制到 `/etc/blog/tls`，属组设成 `blog`、权限 640。
- **证书没准备好就启动**：设了 `Server.Https.Port` 但证书读不了时，服务器直接退出，不会降级成明文。第一次申请证书时服务器还没起来，用 `certbot certonly --standalone`；之后续期再用 `--webroot`。

## 7. 测试与示例

- [tests/test_StaticSite.cpp](../tests/test_StaticSite.cpp)：加载目录（`.gz` 配对、符号链接、读不了的目录）、请求路径、ETag/304/gzip/`q=0`。
- [tests/test_Network.cpp](../tests/test_Network.cpp)："HttpService fallback handler and default headers"。
- [tests/test_ServerNetwork.cpp](../tests/test_ServerNetwork.cpp)："WorldHttpService over TLS"（测试时现场生成自签名证书；明文连接得不到回复、TLS 1.1 被拒、续期后新连接拿到新证书而旧连接继续可用、私钥不对时保留当前证书）、"WorldHttpService redirects plain HTTP to the TLS origin"。
- [apps/blog/server/StaticHandlers.cpp](../apps/blog/server/StaticHandlers.cpp)：完整的用法。

## 8. 思考题与练习

1. 一个页面引用了 40 个脚本和字体。分别算一下，它们走 `Route`（主线程，每会话每帧 3 条）和走网络线程时，最快要多久才能全部回复？
2. 为什么 gzip 版本要有自己的 ETag（`"...-gz"`）？如果和原文件用同一个，带 `Vary: Accept-Encoding` 的缓存会出什么问题？
3. 把上传的图片改成在工作线程上读，读完在网络线程上回复。需要哪些东西？（提示：`WorldHttpRequest::Respond` 是怎么回到网络线程的。）
4. `OptionalSslStream::async_read_some` 里的 `std::visit` 会把两个分支都实例化，但每次只执行一个。为什么 `std::forward<ReadHandlerType>(handler)` 写在 lambda 里不会被移动两次？
5. 用 `openssl s_client -connect localhost:8443 -tls1_1` 和 `-tls1_3` 分别连接，对照 3.6 看结果。
