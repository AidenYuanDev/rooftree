# blog

一个中英双语的个人博客，主题是作者的四张油画：山坡上大树下的小屋，白天和夜晚，用来演示怎样在 Rooftree 上写一个完整的 Web 应用。设计决定和进度见 [PLAN.md](PLAN.md)。

```
浏览器 ──> blog 服务器（443 TLS，80 跳转到 https）
             ├─ /zh/…、/en/…   预渲染的页面，从内存回复（网络线程），其余页面走 SPA 回退
             ├─ /uploads/*     上传的图片（磁盘目录，网络线程）
             ├─ /api/*         API，在主线程上按客户端地址分会话处理（rooftree-server）
             ├─ /ws            同一端口上升级的 WebSocket：在线人数、页面实时更新、聊天、协作编辑、二进制包
             └─ /{zh,en}/feed.xml、/sitemap.xml  由 API 生成
```

前面没有 nginx：TLS、静态文件、gzip、超时和连接数限制都由服务器自己做，见 [docs/30](../../docs/30-static-files-and-tls.md)。

内容有文章、日记、一句话（思考）、时间线、留言板、友链、项目和关于页，管理员还能「看云」，看服务器此刻怎么样；管理员开通了网盘的账号可以存文件，分享链接给登录的读者（可设密码、有效期，能看到谁打开过），管理员可以查看和管理每个账号的网盘和分享；读者可以注册、评论（有人回复会收到邮件）、点表情、申请友链、订阅邮件，互加好友，在浮动聊天窗里聊天（广场、私聊、群聊）；开通了写作权限的账号可以写自己的文章，和受邀的协作者一起实时编辑。作者在后台写作、审核和改站点设置（名称、头像、简介、社交链接、关于页），还能管理用户：新建、改资料、改角色（普通用户或版主）、单独收回评论/表情/换头像/申请友链的权限、改密码或发重置邮件、关两步验证、让某台设备退出登录、注销或彻底删除，并看到谁在线、在线多久、正在看哪个页面。版主只能隐藏评论、封禁普通用户、管理广场和群聊的消息（后台没有查看私聊的入口）。

| 功能 | 在哪里 |
|---|---|
| 文章、日记、一句话，都可以有中英两个版本；文章可以定时发布 | `server/PostHandlers.cpp`、`ContentHandlers.cpp` |
| 评论（文章、日记、一句话、留言板）、表情回应、有人回复时发邮件提醒（默认关，读者在账号页打开） | `CommentHandlers.cpp`、`ReactionHandlers.cpp` |
| 段落评论：选中文章的一段来评论，楼主选中处显示 💬 和回复数，讨论在旁边（手机上从底部弹出），点中哪条评论就高亮它的段落；回复默认沿用所回复评论的段落，也可以另选。文章的每个已发布版本都留着，改了以后找不到原文的评论交给作者处理（保留，或在文章里重选），读者在「原文已修改」里看到当时的原文 | `CommentHandlers.cpp`、`PostHandlers.cpp`（`WorkerSyncPostRevisions`）、`web/app/lib/passage.ts`、`components/passages.tsx` |
| 写作者和协作：管理员给账号开通写作权限后，它拥有自己的文章（`/zh/write`），可以邀请协作者，按人给发布、改设置、管理协作者的权限；多人实时编辑工作副本（`post_draft`），发布后读者才看到；在文章页上直接编辑，Markdown 所见即所得；站点可以要求写作者的发布先审核（`site review`）；公开主页 `/zh/u/<用户名>` | `PostHandlers.cpp`、`PostAccess.h`、`Collab.cpp`、`web/app/components/editor/`、`web/app/routes/write/`、`routes/person.tsx` |
| 版本历史：每次发布和手动保存都留一个版本，可以对比、还原 | `PostHandlers.cpp`（`post_revision`） |
| 好友：申请、同意、拒绝、解除，看到好友是否在线 | `UserFriendHandlers.cpp`、`web/app/routes/person.tsx`、`routes/account.tsx` |
| 聊天：广场、好友私聊、群聊（最多 100 人），已读和未读；消息只发给能看的人，版主可以管理广场和群聊 | `ChatHandlers.cpp`、`Presence.cpp`、`web/app/components/chat/chat-dock.tsx` |
| 站点设置、首页统计、时间线、友链（申请 + 审核 + 邮件）、项目 | `SiteHandlers.cpp` |
| 在线人数、新内容提示，打开着的页面实时显示新评论和新正文（WebSocket） | `BlogWebSocket.cpp`、`Presence.cpp`、`web/app/components/live.tsx` |
| 看云（`/zh/status`，只有管理员能看，入口在头像菜单里）：运行时长、在线读者、每分钟请求、主循环耗时、CPU、内存、负载、磁盘，一分钟一个样本，保留一天 | `ServerStatus.cpp`、`web/app/routes/status.tsx`、`status-chart.tsx` |
| 网盘（`/zh/drive`）：文件夹、分片上传（断了接着传）、在线预览图片/音视频/PDF、回收站、打包下载 zip、缩略图、分享链接（`/zh/s/<token>`，登录后可看） | `Drive.cpp`、`DriveHandlers.cpp`、`ZipStream.cpp`、`web/app/routes/drive.tsx`、`share.tsx`、`components/drive/` |
| 阅读统计（后台「统计」）：每天的阅读和访客、读得最多的页面、来源网站；不算爬虫和管理员，不存 IP | `Analytics.cpp`、`StatsHandlers.cpp`、`web/app/components/page-views.tsx`、`routes/admin/stats.tsx` |
| 用户管理、版主、谁在线 | `UserAdminHandlers.cpp`、`AdminHandlers.cpp`、`AdminUsers.cpp`、`web/app/routes/admin/users.tsx`、`user.tsx` |
| 邮件订阅（双重确认、一键退订、发布时限速群发） | `NewsletterHandlers.cpp`、`Newsletter.cpp` |
| 接口的操作表：每个操作登记一次（opcode、谁能调、HTTP 路由、AntiDOS 类型），HTTP 和 `/ws` 的二进制包走同一份处理函数，见 [docs/32](../../docs/32-packets.md) | `ApiOperations.cpp`、`ApiCall.cpp`、`ApiJson.cpp`、`ApiPackets.cpp`、`server/schema/` |
| 读缓存：文章列表和页面、读者看到的评论、站点计数放在内存里，内容变化时丢弃 | `ReadCache.h` |
| 油画背景和动效、logo、主题 | `web/app/components/scenery/`、`web/public/scenery/`、`app.css` |

| 目录 | 内容 |
|---|---|
| `server/` | API 服务器（C++20）。请求在主线程上按客户端地址分会话处理，见 [docs/29](../../docs/29-server-core.md) |
| `db/migrations/` | 表结构，用 [dbmate](https://github.com/amacneil/dbmate) 执行 |
| `web/` | 前端：React 19、React Router 8（框架模式，`ssr: false` + 预渲染）、TanStack Query、Tailwind v4 + shadcn/ui |
| `tests/` | API 测试，连真实的服务器和数据库 |
| `deploy/` | systemd 服务、重建、备份和证书续期脚本，`docker/` 是 Docker 上线用的镜像和 `ship.sh` |
| `blog.yaml.dist` | 服务器配置，每一项都有说明 |

## 本机开发

需要：Rooftree 的编译依赖（见[根目录 README](../../README.md#构建)）、PostgreSQL 14 或更新版本（要带 ICU，发行版的包都带）、Node.js ≥ 22.22、pnpm。

```sh
# 1. 编译服务器（在仓库根目录）
cmake -B build -G Ninja && cmake --build build

# 2. 依赖和数据库（在 apps/blog）
pnpm install
export DATABASE_URL="postgres://postgres:postgres@127.0.0.1:5432/rooftree_blog_dev?sslmode=disable"
pnpm db:up                     # 建库、执行迁移

# 3. 配置：编辑 build/bin/blog.yaml（第一次构建时从 blog.yaml.dist 复制）
#    Database.Blog 的 User/Password/Name；本机用 http，所以 Blog.Auth.CookieSecure: false；
#    Blog.Mail.OutboxDirectory: "mail-outbox"，邮件写成 .eml 文件，不真的发送
cd ../../build/bin

# 4. 作者账号（只能有一个管理员）。按提示输入密码；加 --totp 同时开两步验证，用身份验证器扫输出的二维码
#    （或者加 otpauth 链接、手动输密钥），不加的话之后也可以在账号页开
./blog --create-admin --username me --email me@example.com --display-name "我"

# 5. 启动 API（127.0.0.1:8100，WebSocket 也在这个端口的 /ws），另开一个终端启动前端
#    （http://localhost:5173，/api、/ws、/uploads 转发给 8100）
./blog
pnpm -C ../../apps/blog dev
```

改了 `blog.yaml` 以后 `kill -HUP $(pidof blog)` 就能重新加载，不用重启。

本机开发库 `rooftree_blog_dev` 里的作者账号（只是测试用的，别用在别处）：

| 用户名 | 密码 | TOTP 密钥 |
|---|---|---|
| `admin` | `admin-test-password` | `WJP4B523CNHVJNSGU2LN2FYSS6Y334YM` |

登录要的 6 位码可以把密钥加进身份验证器，也可以直接算（在仓库根目录；在 `apps/blog` 下去掉 `-C apps/blog`）：

```sh
pnpm -s -C apps/blog totp WJP4B523CNHVJNSGU2LN2FYSS6Y334YM
```

### 测试

```sh
pnpm test:api      # API：每次重建测试库 rooftree_blog_test（TEST_DATABASE_URL 可改，默认 postgres://postgres:postgres@127.0.0.1:5432/rooftree_blog_test?sslmode=disable），目前 194 个场景；packets 的要 flatc（FLATC 可改路径）
pnpm test:web      # 前端组件：Vitest + Testing Library + MSW
pnpm test:e2e      # 端到端：Playwright，真实的 API + 前端开发服务器
pnpm lint          # ESLint + Prettier
```

端到端测试需要 Playwright 的 Chromium（`pnpm -C web exec playwright install chromium`）；本机已经装了 Chrome 的话，`PLAYWRIGHT_CHANNEL=chrome pnpm test:e2e` 直接用它。C++ 部分的测试在仓库根目录 `ctest --test-dir build`（串行跑，不加 `-j`）。

## 上线

下面按一台 Linux 服务器、域名 `blog.example.com` 写，换成你的域名。服务器自己监听 80 和 443，前面不需要 nginx。

### 1. 目录和用户

```sh
useradd --system --home /var/lib/blog --shell /usr/sbin/nologin blog
mkdir -p /opt/blog/bin /etc/blog /srv/blog/{site,uploads,drive,backups,acme}
chown -R blog:blog /srv/blog
git clone <这个仓库> /opt/blog/src
```

| 路径 | 用途 |
|---|---|
| `/opt/blog/bin/blog` | 服务器程序 |
| `/opt/blog/src` | 仓库，`apps/blog` 下装好 node_modules（重建页面和 dbmate 要用） |
| `/etc/blog/blog.yaml`、`blog.env` | 配置和密码 |
| `/etc/blog/tls` | 证书和私钥，certbot 续期后由 `deploy/certbot-deploy-hook.sh` 复制过来 |
| `/srv/blog/site/current` | 当前上线的页面，是指向 `releases/` 里某个目录的符号链接 |
| `/srv/blog/uploads`、`backups` | 上传的图片、备份 |
| `/srv/blog/drive` | 网盘里的文件（随机文件名，归属记在数据库里） |
| `/srv/blog/acme` | certbot `--webroot` 的目录，续期时 Let's Encrypt 从 80 端口读里面的文件 |

### 2. 编译和安装

```sh
cd /opt/blog/src
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=0 -DEXAMPLES=0
cmake --build build --target blog
install -m 755 build/bin/blog /opt/blog/bin/
cd apps/blog && pnpm install --frozen-lockfile
chown -R blog:blog web
```

### 3. 数据库

```sql
CREATE USER blog PASSWORD '…';
CREATE DATABASE rooftree_blog OWNER blog;
```

### 4. 证书

第一次申请时服务器还没启动，80 端口空着，用 certbot 自己的临时服务器（`--standalone`）；之后续期改用 `--webroot`，服务器一直在跑，由它回复 Let's Encrypt 的验证请求：

```sh
certbot certonly --standalone -d blog.example.com \
  --deploy-hook /opt/blog/src/apps/blog/deploy/certbot-deploy-hook.sh
certbot reconfigure --cert-name blog.example.com --webroot -w /srv/blog/acme
```

`certbot-deploy-hook.sh` 把证书复制到 `/etc/blog/tls`（属组 `blog`、权限 640，certbot 原来的私钥只有 root 能读），服务器在跑的话再 `systemctl reload blog`：新连接用新证书，已经打开的连接不受影响。

### 5. 配置

```sh
cp apps/blog/blog.yaml.dist /etc/blog/blog.yaml
cp apps/blog/deploy/blog.env.example /etc/blog/blog.env
chown root:blog /etc/blog/blog.yaml /etc/blog/blog.env; chmod 640 /etc/blog/blog.yaml /etc/blog/blog.env
```

`blog.yaml` 里要改的：

- `Blog.PublicOrigin: "https://blog.example.com"`；`Blog.SiteName` 只是站点设置里没填名称时的后备，名称、头像、简介在后台"站点设置"里改
- `Server.Http`：`BindIP: "::"`（IPv4 和 IPv6 都监听）、`Port: 80`、`RedirectToHttps: true`、`AcmeChallengeDirectory: "/srv/blog/acme"`、`TrustedProxies: []`（前面没有代理，谁的 `X-Forwarded-For` 都不信）
- `Server.Https`：`BindIP: "::"`、`Port: 443`、`CertificateFile: "/etc/blog/tls/fullchain.pem"`、`PrivateKeyFile: "/etc/blog/tls/privkey.pem"`
- `Blog.Static.Directory: "/srv/blog/site/current"`
- `Database.Blog` 的 `User`、`Name`（密码放 `blog.env` 的 `ROOFTREE_DATABASE_BLOG_PASSWORD`）
- `Blog.MigrationsDirectory: "/opt/blog/src/apps/blog/db/migrations"`
- `Blog.Mail`：SMTP 地址、发件人；密码放 `ROOFTREE_BLOG_MAIL_PASSWORD`
- `Blog.Newsletter.MailsPerMinute`：订阅邮件的发送速度，按 SMTP 服务商的限制调
- `Blog.OAuth`：GitHub、Google 登录，不填就没有对应的按钮，见下面的“GitHub 和 Google 登录”
- 防止别人用网站刷邮件：注册、订阅、找回密码要填图形验证码（`Blog.Captcha.Enable`，默认开）；同一个邮箱两封邮件至少隔 `Blog.Auth.MailInterval` 秒（默认 60）
- `Blog.Uploads.Directory: "/srv/blog/uploads"`
- `Blog.Drive.Directory: "/srv/blog/drive"`；`Quota`、`AdminQuota`、`MaxFileSize` 按磁盘大小调，磁盘剩余少于 `MinFreeSpace`（默认 2 GiB）时不再接受上传
- `Blog.Rebuild.Command: "/opt/blog/src/apps/blog/deploy/rebuild.sh"`
- 日志：加一个 `Type: 2` 的文件 appender（见文件开头的说明），或者交给 journald

`blog.env` 里的 `BLOG_PUBLIC_ORIGIN`、`BLOG_API_URL` 给 `rebuild.sh` 用：重建时 React Router 通过 `http://127.0.0.1` 读文章，本机的请求不会被跳转到 https。

连接层的防护默认就开着：请求头 60 秒内要收完（`Server.Http.RequestTimeout`），空闲的 keep-alive 连接 75 秒后关闭，每个地址最多同时开 64 个连接（`MaxConnectionsPerIp`），WebSocket 90 秒没消息就关闭。登录、注册、评论这些接口另有 AntiDOS 限制（`Server.AntiDos.Limits`）。

### 6. 启动

```sh
cp apps/blog/deploy/blog.service apps/blog/deploy/blog-backup.{service,timer} /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now blog                 # 先执行 dbmate 迁移，再启动服务器
sudo -u blog /opt/blog/bin/blog --config /etc/blog/blog.yaml --create-admin --username me --email me@example.com
sudo -u blog env $(cat /etc/blog/blog.env | xargs) /opt/blog/src/apps/blog/deploy/rebuild.sh   # 第一次生成页面
systemctl reload blog                       # 读入刚生成的页面
systemctl enable --now blog-backup.timer
```

控制台（`help` 列出命令：改密码、重设 TOTP、改名、封禁、站点资料 `site show|set|import`、`server info`、`server restart`；后台能做的都有命令（`post`、`tag`、`note`、`thinking`、`comment`、`chat`、`friend`、`project`、`upload`、`account show|role|write|drive|kick…`、`stats`、`status`），读者那一侧的用 `api` 调接口，见 [docs/31](../../docs/31-console.md)。自己的客户端用 `/ws` 上的二进制包调读者和后台的操作（登录、文件下载这类只走 HTTP），见 [docs/32](../../docs/32-packets.md)）：`sudo -u blog /opt/blog/bin/blog --config /etc/blog/blog.yaml --console --socket /var/lib/blog/console.sock`。它连到正在运行的服务器，Tab 补全命令，密码不回显，Ctrl+C 或 Ctrl+D 只退出控制台。

`blog.service` 只给了程序绑定 80、443 端口的权限（`CAP_NET_BIND_SERVICE`），程序以 `blog` 用户运行，不需要 root。

### 运行中

- **发布文章**：服务器在最后一次修改 10 秒后（`Blog.Rebuild.Debounce`）自动运行 `rebuild.sh`。它构建到新目录、检查结果（必须有的页面、代码块都已高亮）、压缩文本文件，再原子地切换 `current`；成功后服务器把新版本读进内存，失败就保留旧版本，原因写在服务器日志里。最近 5 个版本保留在 `releases/`，回滚就是把 `current` 指回去再 `systemctl reload blog`。
- **评论、表情、在线人数**由浏览器直接请求 API 和 WebSocket，不需要重新构建。
- **订阅邮件**：文章第一次发布时自动群发；日记在编辑页勾选"发布时邮件通知订阅者"才发。每篇只发一次（`mailed_at`），在单独的线程上按 `MailsPerMinute` 一封一封发，服务器重启时没发完的不会补发。每封邮件带退订链接和 `List-Unsubscribe` 头，邮箱客户端的"退订"按钮可以直接用。
- **网盘**：管理员在后台的用户页给账号开通网盘、设空间大小和回收站保留天数（管理员自己一直有）；删除的文件默认在回收站留 1 天；`drive list` 控制台命令列出各账号用了多少。上传的分片在 `drive/parts/`，一天没传完的自动清掉。
- **聊天**：每个频道有消息上限（广场和群聊 5000 条，私聊 3000 条），超过时删掉最旧的；`Server.AntiDos.Limits` 的 `chat`、`chat.write` 限制频率；`chat` 控制台命令管理广场和群聊。
- **站点设置**在后台"站点设置"里改，保存后自动重新构建。
- **重启和部署**：停止时（`systemctl stop`、`docker stop`、`ship.sh`）先等 `Server.ShutdownDelay`（30 秒），期间打开着的页面会看到"服务器将在 N 秒后重启"，之后自动重连；再发一次停止信号就立刻停。
- **改配置**：`systemctl reload blog`（SIGHUP，下一帧生效）。同时重新读证书和 `current` 里的页面。
- **证书续期**：certbot 的定时任务自动续期，`certbot-deploy-hook.sh` 复制证书并让服务器重新读，不用重启。
- **主线程卡死**：超过 `Server.MaxCoreStuckTime`（60 秒）没有更新，服务器主动退出，systemd 重启它。
- **日志**：每 `Server.StatsInterval`（60 秒）记一次帧数和帧耗时（`server.world`），超出 AntiDOS 限制的请求记在 `server.antidos`。
- **备份**：每天 4 点，`/srv/blog/backups` 里保留 14 天。恢复（到一个空库）：`gunzip -c blog-….sql.gz | psql rooftree_blog`，`tar -xzf uploads-….tar.gz -C /srv/blog`。备份也要复制到别的机器上。网盘（`/srv/blog/drive`）可能很大，不在每天的备份里，要的话单独用 rsync 之类同步到别处（和数据库备份配对：文件名只在数据库里有对应）。

### 升级

```sh
cd /opt/blog/src && git pull
cmake --build build --target blog && install -m 755 build/bin/blog /opt/blog/bin/
(cd apps/blog && pnpm install --frozen-lockfile)
systemctl restart blog        # 新的迁移在启动前执行
sudo -u blog env $(cat /etc/blog/blog.env | xargs) apps/blog/deploy/rebuild.sh
systemctl reload blog         # 读入新页面
```

## GitHub 和 Google 登录

读者可以用 GitHub 或 Google 登录。第一次登录时，用对方验证过的邮箱直接建一个没有密码的账号，不用填验证码，也不发验证邮件；之后可以在账号设置里设密码，绑定或解绑另一个。如果这个邮箱已经注册过，不会自动合并，而是提示先用密码登录，再到账号设置里绑定。开了两步验证的账号（包括管理员），从 GitHub 或 Google 回来后还要输入验证码。

**GitHub**：Settings → Developer settings → OAuth Apps → New OAuth App
- Homepage URL：`https://blog.example.com`
- Authorization callback URL：`https://blog.example.com/api/auth/oauth/github/callback`
- 记下 Client ID，生成一个 Client secret

**Google**：Google Cloud Console → APIs & Services
- OAuth consent screen：填应用名称、支持邮箱；作用域只要 `openid`、`email`、`profile`；发布状态改成 In production，否则只有测试用户能登录
- Credentials → Create credentials → OAuth client ID，类型选 Web application
- Authorized redirect URIs：`https://blog.example.com/api/auth/oauth/google/callback`

然后在 `blog.yaml` 里填 Client ID，密钥放 `blog.env`。`blog.env` 里的变量只覆盖 `blog.yaml` 里已有的键，所以 `ClientSecret: ""` 要留着。环境变量只在启动时读入：改了 `blog.env` 要重启（`systemctl restart blog`，Docker 是 `docker compose up -d --force-recreate blog`），只改 `blog.yaml` 发 HUP 就行：

```yaml
Blog:
  OAuth:
    GitHub:
      ClientId: "Ov23xxxxxxxx"
      ClientSecret: ""
    Google:
      ClientId: "xxxx.apps.googleusercontent.com"
      ClientSecret: ""
```

```sh
ROOFTREE_BLOG_OAUTH_GITHUB_CLIENTSECRET=...
ROOFTREE_BLOG_OAUTH_GOOGLE_CLIENTSECRET=...
```

## 用 Docker 上线

主机装不了依赖时（例如 Ubuntu 24.04：OpenSSL 只有 3.0，框架要 3.2）用这个办法：镜像以 Ubuntu 26.04 为基础，在开发机上构建（C++ 测试在构建时运行），再传到服务器上。数据库也在容器里（PostgreSQL 18）。两个容器都用主机网络，服务器自己监听 80 和 443，看到的就是客户端的真实地址。文件在 `deploy/docker/`。

主机上做一次（需要 root）：

```sh
apt install docker.io docker-compose-v2 && usermod -aG docker <部署用的用户>
groupadd --system --gid 2000 blog && useradd --system --uid 2000 --gid blog --no-create-home --shell /usr/sbin/nologin blog   # 镜像里的 blog 用户
mkdir -p /etc/blog/tls /srv/blog/{site,uploads,drive,backups,acme,postgres}
chown <部署用的用户> /srv/blog && chown blog:blog /srv/blog/{site,uploads,drive,backups,acme}
cp blog.yaml.dist /etc/blog/blog.yaml          # 按上面第 5 步改，Database.Blog.User 是 blog
cp deploy/blog.env.example /etc/blog/blog.env  # BLOG_API_URL=http://127.0.0.1
cp deploy/docker/postgres.env.example /etc/blog/postgres.env   # 和 blog.env 用同一个密码
chown root:blog /etc/blog/blog.yaml && chmod 640 /etc/blog/blog.yaml
chown <部署用的用户> /etc/blog/*.env && chmod 600 /etc/blog/*.env   # docker compose 用调用者的身份读它们
```

之后每次上线（第一次也是）在开发机上：

```sh
apps/blog/deploy/docker/ship.sh user@host   # 构建、传镜像、重启、重新生成页面
```

| 要做的事 | 命令（在主机上） |
|---|---|
| 作者账号 | `docker compose -f /srv/blog/compose.yaml exec blog /opt/blog/bin/blog --config /etc/blog/blog.yaml --create-admin --username me --email me@example.com` |
| 控制台（文章、评论、友链、账号、站点资料、统计…，`help` 列出全部） | `docker compose -f /srv/blog/compose.yaml exec blog /opt/blog/bin/blog --config /etc/blog/blog.yaml --console`，`help` 看命令，Ctrl+D 退出 |
| 从控制台发文章 | 先 `docker compose -f /srv/blog/compose.yaml cp hello.md blog:/tmp/hello.md`，再在控制台里 `post create hello zh "你好" /tmp/hello.md`，看过后 `post publish hello` |
| 改了 `blog.yaml`、换了证书 | `docker compose -f /srv/blog/compose.yaml kill -s HUP blog` |
| 日志 | `docker compose -f /srv/blog/compose.yaml logs -f blog` |
| 手动重建页面 | `docker compose -f /srv/blog/compose.yaml exec blog /opt/blog/src/apps/blog/deploy/rebuild.sh`，再发 HUP |
| 备份 | `deploy/docker/blog-backup.service` 和 `deploy/blog-backup.timer` 复制到 `/etc/systemd/system/`，`systemctl enable --now blog-backup.timer` |

证书和上面第 4 步一样在主机上用 certbot 申请（`/srv/blog/acme` 挂进了容器），`certbot-deploy-hook.sh` 发现 `/srv/blog/compose.yaml` 时用 HUP 通知容器里的服务器。
