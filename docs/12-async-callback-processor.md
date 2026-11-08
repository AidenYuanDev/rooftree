# 12 · AsyncCallbackProcessor：把异步结果交回主线程

## 1. 是什么

数据库查询在数据库线程上执行，但查询结果需要在**主线程**（游戏逻辑线程）上处理，否则就要给游戏对象加锁。`AsyncCallbackProcessor` 就是连接这两者的桥：

1. 主线程发起异步查询，拿到一个"回调对象"（内部是一个 `std::future`），把它交给处理器；
2. 主线程每一帧调用 `ProcessReadyCallbacks()`；
3. 已经完成的回调**在主线程上**执行，然后被移除；没完成的留到下一帧。

每个会话（`ServerSession`）都有一个，在会话的 `Update` 里调用它。

## 2. 核心接口

| 接口 | 作用 | 位置 |
|---|---|---|
| `AddCallback(T&&)` | 加入一个回调对象，返回它的引用 | [AsyncCallbackProcessor.h](../src/common/sched/AsyncCallbackProcessor.h) |
| `ProcessReadyCallbacks()` | 执行所有已完成的回调 | 同上 |
| `Size()` / `Empty()` / `CancelAll()` | 查询 / 放弃所有回调 | 同上 |

它是一个模板，`T` 只要满足 `AsyncCallback` concept 即可：存在一个返回 `bool` 的 `InvokeAsyncCallbackIfReady(T&)` 函数（[AsyncCallbackProcessorFwd.h:24](../src/common/sched/AsyncCallbackProcessorFwd.h#L24)）。数据库层提供了三种：

| 类型 | 来自 | 详见 |
|---|---|---|
| `QueryCallback` | `AsyncQuery()` | [24](24-query-callback-and-holder.md) |
| `TransactionCallback` | `AsyncCommitTransaction()` | [23](23-transaction.md) |
| `SQLQueryHolderCallback` | `DelayQueryHolder()` | [24](24-query-callback-and-holder.md) |

`QueryCallbackProcessor` 就是 `AsyncCallbackProcessor<QueryCallback>` 的别名（[DatabaseEnvFwd.h](../src/database/DatabaseEnvFwd.h)）。

```cpp
QueryCallbackProcessor queryProcessor;          // 属于主线程的对象

queryProcessor.AddCallback(DB.AsyncQuery(stmt).WithPreparedCallback([this](PreparedQueryResult result)
{
    // 在主线程上执行，可以直接修改游戏对象
}));

// 主循环里每帧：
queryProcessor.ProcessReadyCallbacks();
```

## 3. 关键实现

### 3.1 先把列表移出来，再处理

[AsyncCallbackProcessor.h](../src/common/sched/AsyncCallbackProcessor.h)：

```cpp
std::vector<T> polled = std::move(_callbacks);
_callbacks.clear();
std::erase_if(polled, [](T& callback) { return InvokeAsyncCallbackIfReady(callback); });
_callbacks.insert(_callbacks.begin(), ...polled...);   // 没完成的放回最前面
```

为什么不直接在 `_callbacks` 上 `erase_if`？因为回调执行时很可能又调用 `AddCallback` 发起新的查询。如果直接在 `_callbacks` 上遍历，新加入的元素会让 vector 重新分配内存，遍历用的迭代器就失效了。先把列表移出来，回调里新加入的元素就会进入一个空的新 `_callbacks`，最后再把没完成的放回最前面，保持加入的顺序。

### 3.2 "是否完成"的判断不会阻塞

各个回调类型的 `InvokeIfReady()` 用 `future.wait_for(0s)` 检查结果是否就绪（例如 [Transaction.cpp:8](../src/database/Transaction.cpp#L8)），只看一眼，不会等待。所以 `ProcessReadyCallbacks()` 本身不会让主线程卡住。

## 4. 线程与安全

- **处理器本身没有任何同步**，`AddCallback` 和 `ProcessReadyCallbacks` 必须在同一个线程调用。
- 跨线程的部分只有 `std::future`：数据库线程写入结果，主线程读取，这由标准库保证同步。
- 回调在调用 `ProcessReadyCallbacks()` 的线程上执行，测试里专门验证了这一点（`test_Database.cpp` 中比较了线程 ID）。

## 5. 设计取舍

没完成的回调放回列表**前面**，而不是后面（排在回调里新加入的之后），所以列表一直保持加入的顺序。另外有 `Size()`，`SessionMgr` 用它统计会话还有多少工作没完成（[29](29-server-core.md)）。

## 6. 常见的坑

- **忘记调用 `ProcessReadyCallbacks()`**：查询早就完成了，但回调永远不会执行。
- **回调里捕获了可能被销毁的对象**：比如玩家下线、会话被删除，而查询还没返回。处理器放在会话对象里，会话销毁时处理器也跟着销毁，未执行的回调随之被丢弃。
- **`CancelAll()` 不会取消已经发出的查询**：数据库仍然会执行它，只是结果被丢弃了。对于写操作，这意味着写入照样会发生。

## 7. 测试与示例

- [tests/test_Database.cpp](../tests/test_Database.cpp) 的 "Database runs async work and calls back on the polling thread"：三种回调都在测试线程上执行。
- [examples/db_query.cpp](../examples/db_query.cpp)：异步查询后，主循环每 50ms 调用一次 `ProcessReadyCallbacks()`。

## 8. 思考题与练习

1. 不用数据库，自己写一个满足 `AsyncCallback` concept 的类型：内部用 `std::async` 在后台计算一个值，完成后在主线程上打印出来。
2. 在回调里再发起一次查询并 `AddCallback`，验证它在之后的帧里被正确执行。
3. 为什么数据库的回调要回到主线程执行，而不是直接在数据库线程上处理结果？
