# 04 · Random.h：随机数

## 1. 是什么

游戏和图片里要用到的快速随机数：金币落在哪里、验证码的扭曲和噪点、KCP 连接号、注册时用户名冲突后加的数字。

**不能用于秘密**：令牌、密钥、盐用 `Crypto::GetRandomBytes`（[26](26-crypto.md)），那里用的是 OpenSSL 的密码学随机数。

## 2. 核心接口

| 函数 | 范围 |
|---|---|
| `urand(min, max)` | `uint32`，两端都包含 |
| `frand(min, max)` | `float`，`[min, max)` |
| `randtime(min, max)` | `Milliseconds`，两端都包含（调度器的随机延迟） |
| `rand32()` | 32 位随机数 |

`min > max` 时 `ASSERT` 失败。

## 3. 关键实现

[Random.cpp](../src/common/base/Random.cpp)：每个线程一个 `std::mt19937`，第一次使用时用 `std::random_device` 的 8 个 32 位数播种，之后再用 `std::uniform_int_distribution` / `std::uniform_real_distribution` 取到要求的范围。

为什么每个线程一个：生成器有状态，多个线程共用一个就要加锁。`thread_local` 让每个线程各用各的，不需要同步。

## 4. 线程与安全

任何线程都可以调用，不需要加锁。每个线程的序列互相独立。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| SFMT（SIMD 版的 Mersenne Twister） | `std::mt19937` | 标准库自带，速度对这里的用途足够 |
| `irand`、`rand_norm`、`rand_chance`、`roll_chance`、`urandms`、`urandweighted`、伪随机分布 `PseudoRandomDistributionState` | 没有 | 代码里没有用到。需要概率判定时写 `frand(0, 100) < chance` |
| 全局的 `RandomEngine` 适配器 | 没有 | 同上 |

## 6. 常见的坑

- **用它生成令牌**：`mt19937` 看到几百个输出就能推出内部状态，之后的输出全部可以预测。
- **`urand(0, n)` 当成 `[0, n)`**：两端都包含，取下标要写 `urand(0, n - 1)`。

## 7. 测试与示例

[tests/test_Framework.cpp](../tests/test_Framework.cpp) 的 `"Random stays within bounds"`；[benchmarks/bench_Base.cpp](../benchmarks/bench_Base.cpp) 的 `Random`。

## 8. 思考题

1. 为什么不直接用 `rand() % (max - min + 1) + min`？分别从分布和线程安全两方面考虑。
2. 每个线程第一次调用时都要读 `random_device`。工作线程池有 16 个线程时，这个开销值得担心吗？
