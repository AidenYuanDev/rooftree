# 02 · Duration.h 与 Timer.h：时间

## 1. 是什么

服务器的逻辑都由时间驱动：超时、冷却、刷新间隔、心跳。主循环每帧算出这一帧经过的毫秒数（`diff`），交给各个调度器。

- **`Duration.h`**：`std::chrono` 时长和时钟的短名字，以及全局可用的 chrono 字面量（`500ms`、`2s`、`1min`）。
- **`Timer.h`**：`getMSTime()`，进程启动以来的毫秒数，32 位；以及处理回绕的 `getMSTimeDiff`。

## 2. 核心接口

| 名称 | 定义 | 位置 |
|---|---|---|
| `Milliseconds` / `Seconds` / `Minutes` / `Hours` | `std::chrono::milliseconds` 等 | [Duration.h](../src/common/base/Duration.h) |
| `TimePoint` | `steady_clock::time_point`：单调时钟，用于超时和间隔 | 同上 |
| `SystemTimePoint` | `system_clock::time_point`：墙上时间，用于存储和显示 | 同上 |
| `getMSTime()` | 进程启动以来的毫秒数（`uint32`，单调） | [Timer.h](../src/common/base/Timer.h) |
| `getMSTimeDiff(earlier, later)` | 两个 `getMSTime()` 值之差，跨过回绕也对 | 同上 |

## 3. 关键实现

### 3.1 回绕

`uint32` 毫秒 49.7 天就会回绕到 0。无符号整数的减法本身就是模 2³² 的，所以

```cpp
inline uint32 getMSTimeDiff(uint32 earlier, uint32 later) { return later - earlier; }
```

在回绕前后都给出正确的差值，只要两个时刻相差不到 49.7 天。比较先后也用差值：`int32(a - b) < 0` 表示 a 在 b 之前。**不要用 `a < b`**，回绕时会得出相反的结论。KCP 的计时（[33](33-kcp.md)）就是这样写的。

### 3.2 起点

`getMSTime()` 的起点是第一次调用时的 `steady_clock::now()`，保存在函数内的静态变量里。`inline` 函数里的静态变量在整个程序中只有一份，所以所有翻译单元看到的是同一个起点。

## 4. 线程与安全

都可以在任何线程调用。静态变量的初始化是线程安全的（C++11 起）。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| `getMSTimeDiff` 写成 `(0xFFFFFFFF - old) + new`，回绕时少算 1 ms | 无符号减法 | 更简单，回绕时也精确 |
| `IntervalTimer`、`TimeTracker`、`PeriodicTimer` | 没有 | 代码里没有用到；周期任务用 `FixedTicker`（[29](29-server-core.md)）或 `TaskScheduler`（[11](11-task-scheduler.md)） |
| `FloatMilliseconds` 等浮点时长、`_days` 字面量 | 没有 | 没有用到，C++20 有 `std::chrono::days` |

## 6. 常见的坑

- **用 `getMSTime()` 记录要存起来的时刻**：它只在这个进程里有意义，重启后从 0 开始。要存储或显示的时刻用 `SystemTimePoint`。
- **用 `system_clock` 算超时**：墙上时间会被 NTP 调整、可能倒退。超时和间隔用 `TimePoint`（steady clock）。

## 7. 测试与示例

KCP 的测试（[tests/test_Kcp.cpp](../tests/test_Kcp.cpp)）和主循环的测试（`FixedTicker`，[tests/test_ServerCore.cpp](../tests/test_ServerCore.cpp)）都依赖这里的计时。

## 8. 思考题

1. 两个时刻相差超过 2³¹ ms（24.8 天）时，`int32(a - b) < 0` 会给出什么结果？KCP 的连接为什么不会遇到这种情况？
2. 为什么起点用第一次调用的时间，而不是进程真正启动的时间？对测试有什么好处？
