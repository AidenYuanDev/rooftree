# 28 · HTTP 路由与请求工具：Router、路径参数、Cookie、multipart

## 1. 是什么

为了写 JSON API（[apps/blog](../apps/blog/PLAN.md)）加的。每个方法一张"完整路径 → 处理函数"的表，只够几个固定地址用；REST 接口需要 PUT、PATCH、DELETE 和 `/api/posts/{id}` 这样的路径，还要读查询参数、表单、Cookie 和上传的文件。

`RegisterHandler` 的写法不变，原来的处理函数（arena、http_server）不用改：

```cpp
RegisterHandler(http::verb::delete_, "/api/posts/{id}", [](std::shared_ptr<MySession> session, RequestContext& context)
{
    std::string_view id = context.pathParameters.Get("id");   // 已经百分号解码
    Optional<std::string> lang = GetQueryParameter(ToStdStringView(context.request.target()), "lang");
    ...
});
```

## 2. 核心组成

| 类 / 函数 | 作用 | 位置 |
|---|---|---|
| `Router<Handler>` | 按路径段组织的路由树：字面段、`{name}` 参数段、按方法存处理函数 | [HttpRouter.h](../src/common/network/http/HttpRouter.h) |
| `PathParameters` | 匹配到的 `{name}` 的值，放在 `RequestContext::pathParameters` | 同上 |
| `DecodePathSegments` | 目标地址 → 解码后的路径段，非法地址返回空 | [HttpRequestUtils.h](../src/common/network/http/HttpRequestUtils.h) |
| `GetQueryParameters` / `GetQueryParameter` | 查询参数（`+` 当作空格） | 同上 |
| `ParseUrlEncodedForm` | `application/x-www-form-urlencoded` 请求体 | 同上 |
| `GetCookie` / `Cookie` / `SetCookie` | 读 `Cookie` 头；写 `Set-Cookie`，默认 `HttpOnly; Secure; SameSite=Lax` | 同上 |
| `ParseMultipartFormData` | `multipart/form-data` 请求体拆成若干部分，`Data` 指向原请求体，不复制 | 同上 |
| `BaseSocket::SetMaxRequestBodySize` | 请求体上限（Beast 默认 1 MiB），超过回 413 | [BaseHttpSocket.h](../src/common/network/http/BaseHttpSocket.h) |
| `DispatcherService::SetFallbackHandler` / `SetDefaultHeaders` | 没有路由匹配时的兜底（静态文件）、每个响应都带的头，见 [30](30-static-files-and-tls.md) | [HttpService.h](../src/common/network/http/HttpService.h) |

解码都交给 Boost.URL，但头文件只暴露 `std` 类型，使用方不需要依赖 Boost.URL。

## 3. 关键实现

### 3.1 路由树

每个节点有三样东西：字面子节点表、最多一个参数子节点、按方法存的处理函数。注册 `/api/posts/{id}` 就是沿着 `api` → `posts` → `{id}` 往下建节点，最后在叶子上放处理函数。

匹配时**先走字面分支，走不通再走参数分支**（[HttpRouter.h](../src/common/network/http/HttpRouter.h) 的私有 `Find`）。所以同时注册 `/posts/{id}` 和 `/posts/slugs` 时，`/posts/slugs` 总是到后者，与注册顺序无关；而 `/posts/slugs/comments/7` 在字面分支里找不到路由，会回退到 `/posts/{id}/comments/{commentId}`。参数值在递归返回时才加入，走不通的分支不会留下参数。

几个规则：

- 参数段匹配任意**一个非空**段，`/posts/` 不会匹配 `/posts/{id}`。
- 同一个位置的参数只能有一个名字：先注册 `/a/{id}` 再注册 `/a/{slug}/b` 会失败，否则一个值要对应两个名字。
- 路径匹配但方法不对，返回 **405**，`Allow` 头列出这个路径支持的方法；HEAD 复用 GET 的处理函数。
- 注册非法或重复的路由会 `ABORT`，这是程序错误，启动时就应该暴露出来。

路由在 `StartNetwork` 之前注册完，之后只读，所以多个网络线程可以同时匹配，不需要加锁。

### 3.2 路径解码

`DecodePathSegments` 用 Boost.URL 的 `parse_origin_form` 解析目标地址，再逐段百分号解码。解码后含有 `/` 或 NUL 的段直接判为非法（回 400）：`%2F` 会让一个段冒充两个段，NUL 会让后面按 C 字符串处理的代码提前截断。

### 3.3 一个连接同时只处理一个请求

如果 `ReadHandler` 把读缓冲区里所有完整的请求一口气解析完，某个请求返回 `Async`，后面的请求会先处理完、先写出响应，**同一连接上的响应顺序就乱了**（HTTP/1.1 要求按请求顺序响应）。

现在 `BaseSocket` 记录"有请求正在处理"（`_requestInProgress`）：

1. 处理函数返回 `Async` 后，`ProcessReadBuffer()` 不再解析后面的请求，`ReadHandler()` 返回 `Stop`，**暂停读取**。这样客户端也没法往读缓冲区里堆请求。
2. 异步结果回来，`SendResponse()` 在网络线程上写出响应，然后接着处理缓冲区里剩下的请求，再重新开始读取。

`_insideRequestHandler` 用来区分 `SendResponse()` 是同步调用（在 `HandleMessage` 里）还是异步完成，避免同步路径上递归处理缓冲区。

### 3.4 解析错误不再让连接卡住

`ParseRequest` 不能忽略 `parser.put()` 的错误码，否则格式错误的请求、超过 `body_limit` 的请求体，解析器永远不会完成，数据也不会被消耗，连接就一直挂着，读缓冲区还会随着每次读取变大。所以它会报告错误，`BaseSocket` 回 400（请求体过大回 413）并在写完后关闭连接。

## 4. 线程与安全

| 函数 | 线程 |
|---|---|
| `Router::Add`（`RegisterHandler`） | 启动时，`StartNetwork` 之前 |
| `Router::Find` | 任意网络线程，只读 |
| `HttpRequestUtils.h` 里的函数 | 任意线程，无状态 |
| `SendResponse`（Async 完成时） | 必须投递到连接所属的网络线程（参考 arena 的 `AuthService::RunAsync`） |

## 5. 设计取舍

- `DispatcherService` 内部用 `Router`，支持所有方法、路径参数和 405。
- `RequestContext` 带着 `pathParameters`。
- `BaseSocket`：同一连接上的请求按顺序处理；解析错误回 400/413；可以设置请求体上限。

## 6. 常见的坑

- **Cookie 的值原样写出**：`Cookie::Value` 只能用 cookie 允许的字符（base64url、十六进制等），不要直接放用户输入。
- **multipart 的 `Data` 指向请求体**：`RequestContext` 销毁以后就失效了，要保存就先复制。
- **请求体上限在 `Start()` 之前设置**：`SetMaxRequestBodySize` 在每次重置解析器时生效，参考测试里的 `TestHttpSession::Start`。
- **`Get(name)` 对不存在的参数返回空字符串**，不会报错，参数名写错了要靠测试发现。

## 7. 测试与示例

- [tests/test_HttpRequest.cpp](../tests/test_HttpRequest.cpp)：路由匹配（字面优先、回退、405、非法注册）、路径/查询/表单解码、Cookie、multipart。
- [tests/test_Network.cpp](../tests/test_Network.cpp) 的 "HttpService routes requests over loopback"：真实连接上的路径参数、405、`%2F` 回 400、管线化请求在 Async 请求之后仍然按顺序响应、请求体过大回 413、格式错误回 400。

## 8. 思考题与练习

1. 为什么参数值要在递归**返回**时才加入 `PathParameters`，而不是进入参数分支时就加入？
2. 如果要支持 `/files/{path...}` 这种匹配多个段的通配参数，路由树要怎么改？
3. 暂停读取以后，客户端断开连接，服务器什么时候才会发现？
