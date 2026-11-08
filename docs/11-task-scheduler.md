# 11 · TaskScheduler：调度 lambda

## 1. 是什么

`TaskScheduler` 和 [EventMap](10-event-map.md) 做的是同一件事，只是直接调度 **lambda**，不用定义编号、写 `switch`：

```cpp
scheduler.Schedule(10min, [this](TaskContext context)
{
    SaveScores();
    context.Repeat();                    // 10 分钟后再执行
});
...
scheduler.Update(Milliseconds(diff));    // 世界的 tick
```

博客用它定时清理缓存、写统计、发布定时文章（[BlogWorld.cpp](../apps/blog/server/BlogWorld.cpp)），arena 用它刷金币、定时保存（[World.cpp](../apps/arena/World.cpp)）。

## 2. 核心接口

| 接口 | 作用 |
|---|---|
| `Schedule(delay, [group,] task)` | `delay` 后执行；`group` 是任意非 0 的数 |
| `Update(elapsed)` | 时间前进 `elapsed`，按到期顺序执行到期的任务 |
| `CancelAll()` / `CancelGroup(group)` | 取消 |
| `RescheduleGroup(group, delay)` | 这一组都改成从现在起 `delay` 后到期 |
| `Empty()` | 还有没有任务 |

任务拿到的 `TaskContext`：

| 接口 | 作用 |
|---|---|
| `Repeat()` / `Repeat(delay)` | 用上次的间隔 / 新的间隔再执行一次 |
| `GetRepeatCounter()` | 这是第几次重复（第一次执行时为 0） |
| `GetGroup()` | 所在的组 |

## 3. 关键实现

[TaskScheduler.cpp](../src/common/sched/TaskScheduler.cpp)：

- 任务按到期时刻存在 `std::multimap` 里。时间是 `Update` 累加出来的。
- **从任务自己的到期时刻重复**：`Update` 执行一个任务前，先把"现在"设成这个任务的到期时刻，`Repeat(d)` 就安排在"到期时刻 + d"。所以一次很长的 `Update` 会**补帧**：`Update(350ms)` 让 100 ms 一次的任务在 100、200、300 ms 各执行一次。卡顿之后，周期任务的总次数不会变少。
- **间隔为 0 的重复放到下一次 `Update`**：否则它会在同一次 `Update` 里无限执行下去。
- 任务里也可以 `Schedule` 新任务，同样从这个任务的时刻算起。
- 任务的 lambda 存在 `shared_ptr` 里，重复时只拷贝指针，不拷贝 lambda 捕获的东西。

## 4. 线程与安全

没有任何同步，只能在一个线程上使用。任务在调用 `Update` 的线程上执行。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| 时间是 `TimePoint`，`Update()` 不带参数时读真实时钟 | 只有 `Update(elapsed)` | 世界的 tick 总是知道经过了多久 |
| 随机延迟、`Async`、`SetValidator`、`DelayAll`/`DelayGroup`、`RescheduleAll`、`CancelGroupsOf`，`TaskContext` 里也能做调度器的所有操作 | 只留了上面列出的接口 | 没有用到 |
| `Repeat(0ms)` 会在同一次 `Update` 里无限执行 | 推到下一次 `Update` | 避免卡死 |
| 实现约 1000 行 | 约 130 行 | 只留了用到的 |

## 6. 常见的坑

- **lambda 捕获 `this`**：调度器必须和对象一起销毁（作为成员），否则任务会访问已经销毁的对象。
- **在任务里 `CancelGroup` 自己的组**：这次执行不受影响，之后调用的 `Repeat` 还是会把它重新加回去。要停下来就不要再 `Repeat`。

## 7. 测试与示例

- [tests/test_Framework.cpp](../tests/test_Framework.cpp)：`"TaskScheduler repeats from when a task was due and catches up"`、`"TaskScheduler runs in order, moves groups, repeats without a delay at the next update"`。
- [examples/scheduler_demo.cpp](../examples/scheduler_demo.cpp)、[benchmarks/bench_Sched.cpp](../benchmarks/bench_Sched.cpp)。

## 8. 思考题

1. 补帧对"每 10 分钟保存一次"有好处吗？对"每 100 ms 刷一个金币"呢？如果不想补帧，任务里应该怎么写？
2. 比较 `EventMap` 和 `TaskScheduler` 的 `Repeat`，各自适合什么样的任务？
