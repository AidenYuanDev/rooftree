# 26 · 加密：哈希、随机字节、Argon2、TOTP

## 1. 是什么

`common/crypto` 是 OpenSSL 和 libargon2 的一组小封装，只包含框架和应用实际用到的部分：

| 需求 | 用什么 | 谁在用 |
|---|---|---|
| 令牌、密钥、盐、TOTP 密钥 | `GetRandomBytes` | 会话 ID、登录令牌、KCP 密钥、验证码 ID |
| 摘要 | `SHA1`、`SHA256` | WebSocket 握手（SHA-1）、令牌在数据库里只存哈希、OAuth 的 PKCE |
| 存储密码 | `Argon2::Hash` / `Verify` | 博客和 arena 的账号、网盘分享的密码 |
| 两步验证 | `TOTP::GenerateToken` | 博客的登录 |

KCP 的数据报加密（AES-128-GCM）直接调用 OpenSSL 的 EVP 接口，写在 [KcpProtocol.cpp](../src/common/network/kcp/KcpProtocol.cpp) 里（[33](33-kcp.md)），不经过这里。

模块里只有用到的东西：没有 `AES`、`HMAC`、`Ed25519`、`RSA`、大整数，也没有只为兼容某个现成客户端才需要的 SRP6、ARC4。

## 2. 核心接口

| 组件 | 接口 | 位置 |
|---|---|---|
| 随机字节 | `GetRandomBytes(ptr, size)`、`GetRandomBytes(span)`、`GetRandomBytes<N>()` 返回 `std::array` | [CryptoRandom.h](../src/common/crypto/CryptoRandom.h) |
| 摘要 | `SHA256::GetDigestOf(part, ...)`，返回 `SHA256::Digest`（`std::array<uint8, 32>`）；每个部分是文本或字节 | [CryptoHash.h](../src/common/crypto/CryptoHash.h) |
| 密码 | `Argon2::Hash(password, iterations, memoryCost)`、`Argon2::Verify(password, hash)` | [Argon2Hash.h](../src/common/crypto/Argon2Hash.h) |
| TOTP | `TOTP::GenerateToken(secret, time)`、`TOTP::SecretSize`（20 字节） | [TOTP.h](../src/common/crypto/TOTP.h) |

```cpp
// 存储密码：盐和参数都编码在结果里
Optional<std::string> encoded = Rooftree::Crypto::Argon2::Hash(password, 2, 19456);
bool ok = Rooftree::Crypto::Argon2::Verify(input, *encoded);

// WebSocket 握手：两部分连在一起的 SHA-1
Crypto::SHA1::Digest digest = Crypto::SHA1::GetDigestOf(key, HandshakeGuid);
```

## 3. 关键实现与要点

### 3.1 随机字节：失败就终止

`GetRandomBytes` 调用 OpenSSL 的 `RAND_bytes`，失败时 `ASSERT`。安全随机数生成失败时，最危险的做法是"降级成普通随机数继续运行"，所以宁可终止。

注意和 [04 · Random](04-random.md) 区分：`urand` 等函数只能用于游戏和图片，**凡是安全相关的随机数都必须用这里的 `GetRandomBytes`**。

### 3.2 摘要：一个非模板的实现

`SHA1` 和 `SHA256` 都是 `Hash<算法, 长度>` 的别名。模板只负责把各个部分转成 `std::span<uint8 const>`（文本按字节），真正的计算在 [CryptoHash.cpp](../src/common/crypto/CryptoHash.cpp) 的一个函数里：`EVP_DigestInit_ex`、对每个部分 `EVP_DigestUpdate`、`EVP_DigestFinal_ex`。头文件不需要包含 OpenSSL。

### 3.3 Argon2：参数跟着哈希走

`Argon2::Hash` 每次生成 16 字节的随机盐，用 Argon2id 算出 16 字节的哈希，返回编码后的文本：

```
$argon2id$v=19$m=19456,t=2,p=1$<盐>$<哈希>
```

编码里带着算法、版本、内存、迭代次数、并行度和盐，所以 `Verify` 只需要存下来的这段文本。调整参数后，旧的哈希照样能验证，新密码用新参数。

Argon2 故意很慢（博客默认 19 MiB、2 次迭代，几十毫秒），**只能在工作线程上调用**（`co_await OnWorker`，[29](29-server-core.md)）。

### 3.4 TOTP

RFC 6238：时间按 30 秒一步，把步数写成 8 字节大端，对它做 HMAC-SHA1，再按 RFC 4226 的"动态截断"取出 31 位，对 10⁶ 取余得到 6 位数。和身份验证器 App 的算法相同。

博客允许前后各一步的误差，并记住用过的最后一步，同一个码不能用两次（[AuthHandlers.cpp](../apps/blog/server/AuthHandlers.cpp) 的 `VerifyTotp`）。

## 4. 线程与安全

所有函数都可以在任何线程调用，没有共享状态。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| `Argon2::Hash(password, BigNumber salt, ...)`，盐由调用者用 `BigNumber::SetRand(128)` 生成 | 盐在函数里生成 | 调用者不用为了一个盐去碰大整数；长度一样（16 字节），已有的哈希照样验证 |
| `Argon2` 默认参数 10 次迭代、128 MiB | 没有默认值，调用者传入 | 博客和 arena 都从配置读 |
| `UpdateData`/`Finalize` 的分步接口，SHA-512、MD5 | 一次算完的 `GetDigestOf`，只有 SHA-1 和 SHA-256 | 没有用到其他的 |
| `TOTP::ValidateToken`（前后各一步） | 没有 | 博客要拒绝重复使用的码，自己做了检查 |
| `OpenSSLCrypto` 加载 legacy provider | 没有 | 只有 MD4、RC4 这类旧算法需要 |

## 6. 常见的坑

- **用普通随机数生成安全相关的值**：见 3.1。
- **用 SHA-256 存储密码**：应该用 Argon2。SHA-256 很快，正因为快，才容易被暴力破解。
- **在主线程上调用 Argon2**：每次几十毫秒，主线程上所有会话都要等它。

## 7. 测试与示例

[tests/test_Framework.cpp](../tests/test_Framework.cpp)：

| 测试 | 验证内容 |
|---|---|
| Hashes match known vectors | SHA-1 / SHA-256 的标准向量，分段输入，RFC 6455 握手示例 |
| Argon2 hashes carry their parameters and a salt of their own | 编码格式、验证、每次的盐不同 |
| TOTP matches RFC 6238's SHA1 vectors | RFC 的测试向量、30 秒的步长边界 |

[benchmarks/bench_Crypto.cpp](../benchmarks/bench_Crypto.cpp)：哈希、TOTP、随机字节、Argon2 的代价。

## 8. 思考题与练习

1. Argon2 的内存参数为什么比迭代次数更能抵抗 GPU 暴力破解？
2. 写一个小程序，用 `GetRandomBytes` 生成一个 TOTP 密钥，用 Base32 编码后生成 `otpauth://` URI（见博客的 `TotpSetup.h`），用手机的身份验证器扫描，再用 `GenerateToken` 验证手机上的码。
