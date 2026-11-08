# 23 · Transaction：事务与死锁重试

## 1. 是什么

`Transaction<T>`（[Transaction.h](../src/database/Transaction.h)）是一组要么全部执行、要么都不执行的语句。

```cpp
SQLTransaction<DemoDatabaseConnection> trans = DemoDatabase.BeginTransaction();
trans->Append(insertStatement);
trans->Append("UPDATE demo SET score = 0");
DemoDatabase.CommitTransaction(trans);                       // 排进异步队列
```

| 接口 | 线程 | 结果 |
|---|---|---|
| `CommitTransaction` | 异步队列 | 失败只记日志 |
| `AsyncCommitTransaction` | 异步队列 | `TransactionCallback`，`AfterComplete(bool)` 在轮询它的线程上回调 |
| `DirectCommitTransaction` | 当前线程，阻塞 | 失败只记日志 |

## 2. 关键实现

`DatabaseConnection::ExecuteTransaction` 执行 `BEGIN`、每条语句、`COMMIT`。任何一条失败就 `ROLLBACK`。

**自动重试**：PostgreSQL 报告死锁（SQLSTATE `40P01`）或序列化失败（`40001`），或连接断开时，整个事务最多再试 4 次，每次多等 10 ms。其他错误不重试，比如违反唯一键。

## 3. 和 MySQL 不同的地方

**一条语句失败，事务就废了**：之后的语句全部报错，只能回滚。MySQL 里一条语句失败后事务还能继续。所以事务里不要靠"让它失败"来判断，要用 `ON CONFLICT`。

`lastval()` 在同一条连接上有效，事务内的语句都在同一条连接上，所以可以先插入文章，再用 `lastval()` 插入它的译文：

```sql
INSERT INTO post_translation (post_id, lang, title, summary, body_md) VALUES (lastval(), ?, ?, ?, ?)
```

中间不能有其他用到序列的插入。

## 4. 线程与安全

事务对象只属于创建它的线程，提交之后不要再使用。

## 5. 常见的坑

- **事务里查不到结果**：`Append` 的语句不返回结果。需要查询再决定的逻辑，放到工作线程上用 `Query` 加 `DirectExecute`，或者写成一条语句。
- **`DirectCommitTransaction` 不返回结果**：要知道成败时用 `AsyncCommitTransaction`。

## 6. 测试与示例

[tests/test_Database.cpp](../tests/test_Database.cpp) 的 "rolled back transaction leaves no rows" 和 "async transaction"。
