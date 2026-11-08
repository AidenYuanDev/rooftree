# 24 · QueryCallback 与 QueryHolder：异步回调与批量查询

## 1. 是什么

- **`QueryCallback`**（[QueryCallback.h](../src/database/QueryCallback.h)）：一个正在另一个线程上执行的查询，以及拿到结果后要做的事。回调在**轮询它的线程**上执行，通常是主线程。
- **`SQLQueryHolder<T>`**（[QueryHolder.h](../src/database/QueryHolder.h)）：几条语句在一条连接上依次执行，结果按位置保存。

## 2. QueryCallback

```cpp
session.AddQueryCallback(Database.AsyncQuery(stmt).WithPreparedCallback([&](PreparedQueryResult result)
{
    // 主线程
}));
```

`QueryCallbackProcessor`（[12](12-async-callback-processor.md)）每帧调用 `InvokeIfReady()`。

需要连续查询时用 `WithChainingPreparedCallback`，在回调里 `SetNextQuery`，下一个回调拿到新查询的结果：

```cpp
Database.AsyncQuery(first).WithChainingPreparedCallback([](QueryCallback& callback, PreparedQueryResult result)
{
    callback.SetNextQuery(Database.AsyncQuery(second));
}).WithPreparedCallback([](PreparedQueryResult result) { /* second 的结果 */ });
```

新代码里，多步的流程优先用协程（[29 · 3.7](29-server-core.md)），可读性更好。

## 3. QueryHolder

```cpp
auto holder = std::make_shared<SQLQueryHolder<DemoDatabaseConnection>>();
holder->SetSize(2);
holder->SetPreparedQuery(0, first);
holder->SetPreparedQuery(1, second);

SQLQueryHolderCallback callback = Database.DelayQueryHolder(holder);
callback.AfterComplete([](SQLQueryHolderBase const& done) { done.GetPreparedResult(0); });
```

**它不是事务**：各条语句独立提交，中间可能看到别的连接写入的内容。

## 4. 线程与安全

创建和轮询要在同一个线程上。回调可以放心使用只有这个线程访问的状态。

## 5. 常见的坑

- **回调捕获的对象要活得够久**：放进 `Session` 的回调有会话保证，其他情况要自己保证。
- **回调里不要做同步查询**：回调在主线程上。

## 6. 测试与示例

[tests/test_Database.cpp](../tests/test_Database.cpp) 的 "async query sees earlier async writes"、"query holder runs several queries in one go"。
