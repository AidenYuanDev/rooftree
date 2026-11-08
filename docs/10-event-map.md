# 10 · EventMap：编号的定时事件

## 1. 是什么

`EventMap` 管理一组"过一段时间后到期"的事件，每个事件只是一个编号。它自己不调用任何东西，由循环每帧来问"有什么到期了"：

```cpp
events.ScheduleEvent(EVENT_GOLD_RUSH_START, 5min);
...
events.Update(diff);
while (uint16 id = events.ExecuteEvent())
{
    switch (id)
    {
        case EVENT_GOLD_RUSH_START:
            StartGoldRush();
            events.ScheduleEvent(EVENT_GOLD_RUSH_END, 30s);
            break;
        case EVENT_GOLD_RUSH_END:
            EndGoldRush();
            events.Repeat(5min);
            break;
    }
}
```

游戏的 Boss 脚本常这样写。Rooftree 的 arena 用它安排"金币雨"的开始和结束（[World.cpp](../apps/arena/World.cpp)）。它只有常用的功能。

## 2. 核心接口

| 接口 | 作用 |
|---|---|
| `Update(diff)` | 时间前进 `diff` 毫秒（也接受 `Milliseconds`） |
| `ScheduleEvent(id, delay, group = 0, phase = 0)` | `delay` 后到期；同一个编号可以安排多个 |
| `RescheduleEvent(id, delay, ...)` | 先取消这个编号的所有事件，再安排一个 |
| `CancelEvent(id)` / `CancelEventGroup(group)` | 取消 |
| `DelayEvents(delay, group = 0)` | 推迟一组，或者全部 |
| `ExecuteEvent()` | 取出最早到期、当前阶段可以执行的一个，返回编号；没有就返回 0 |
| `Repeat(delay)` | 把 `ExecuteEvent` 上次给出的事件在 `delay` 后再安排一次 |
| `SetPhase` / `AddPhase` / `RemovePhase` / `IsInPhase` | 阶段，1 到 8，可以同时处于多个 |
| `GetTimeUntilEvent(id)` / `Empty()` / `Reset()` | 查询和清空 |

## 3. 关键实现

[EventMap.h](../src/common/sched/EventMap.h)：

- 事件存在一个 `std::multimap<Milliseconds, Event>` 里，键是到期时刻，所以 `ExecuteEvent` 从头往后找就是按到期顺序。
- 时间是 `Update` 累加出来的，不读时钟。测试里推进多少就是多少，和真实时间无关。
- **分组**和**阶段**都存成位掩码（1 到 8 各占一位）。事件带阶段时，只有地图不处于任何阶段、或者处于事件的阶段之一，才会被 `ExecuteEvent` 取出；不在阶段内的事件**留在原处等待**，不会被丢掉。Boss 转阶段时，上一阶段的技能就这样暂停了。
- `Repeat` 从现在算起，而不是从事件原来的到期时刻算起：一次很长的 `Update` 之后，重复的事件不会补执行多次。这一点和 [TaskScheduler](11-task-scheduler.md) 不同。

## 4. 线程与安全

没有任何同步，只能在一个线程上使用（世界的 tick）。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| 时间是 `TimePoint`，`Update` 也接受真实时间 | `Update` 累加的毫秒数 | 只有一种用法，测试也简单 |
| `ScheduleEvent(id, min, max)` 随机延迟、"事件序列"（一个事件依次用几个不同的间隔重复） | 没有 | 没有用到；随机延迟可以写 `ScheduleEvent(id, randtime(min, max))` |
| `Repeat(min, max)`、`DelayEvents` 的各种重载、`GetNextEventTime`、`HasEventScheduled` | 只留了上面列出的接口 | 同上 |

## 6. 常见的坑

- **编号 0 不是事件**：`ExecuteEvent()` 用 0 表示"没有"，所以安排编号 0 会 `ASSERT`。
- **`Repeat` 只对 `ExecuteEvent` 刚给出的事件有效**：在 `switch` 外面调用，重复的是上一次执行过的那个事件。
- **一帧里可能有多个事件到期**：要用 `while` 循环取完，而不是 `if`。

## 7. 测试与示例

- [tests/test_EventMap.cpp](../tests/test_EventMap.cpp)：到期顺序、重复、重新安排、取消、分组、阶段、`Reset`。
- [examples/scheduler_demo.cpp](../examples/scheduler_demo.cpp)：在 50 ms 的固定 tick 里和 `TaskScheduler` 一起使用。
- [benchmarks/bench_Sched.cpp](../benchmarks/bench_Sched.cpp)：一个有 8 个重复技能的 Boss，每 tick 的代价。

## 8. 思考题

1. 一个 Boss 有两个阶段，第一阶段放火球，第二阶段放冰箭。用阶段写出来，说明转阶段时为什么不需要取消火球。
2. `Repeat` 为什么从"现在"算起？如果改成从原来的到期时刻算起，服务器卡顿 2 秒之后会发生什么？
