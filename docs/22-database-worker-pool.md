# 22 · DatabaseWorkerPool：连接池

## 1. 是什么

`DatabaseWorkerPool<T>`（[DatabaseWorkerPool.h](../src/database/DatabaseWorkerPool.h)）是一个数据库的所有连接，以及应用的线程怎样共用它们。`T` 是你的 `DatabaseConnection` 子类（[20](20-connection-and-statements.md)）。逻辑在非模板的基类 `DatabasePool` 里，模板只加上语句类型。

它有两组连接：

| | 同步连接 | 异步连接 |
|---|---|---|
| 配置 | `SynchThreads` | `WorkerThreads` |
| 谁用 | 调用线程阻塞等待一条空闲连接 | 每条连接有自己的线程，从一个队列里取任务 |
| 接口 | `Query`、`DirectExecute`、`DirectInsert`、`DirectCommitTransaction` | `Execute`、`AsyncQuery`、`CommitTransaction`、`AsyncCommitTransaction`、`DelayQueryHolder` |

## 2. 用法

```cpp
DemoDatabase.SetConnectionInfo(config->Connection, config->WorkerThreads, config->SynchThreads);
if (DemoDatabase.Open())                    // 0 表示全部连上
    return 1;
DemoDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS ...");   // 语句依赖的表要先有
if (!DemoDatabase.PrepareStatements())
    return 1;
...
DemoDatabase.Close();                       // 队列里的任务会执行完
```

配置（`Database.<名字>`）：`Host`、`Port`（默认 5432）、`User`、`Password`、`Name`、`Socket`（Unix socket 所在的目录）、`Ssl`、`WorkerThreads`、`SynchThreads`，见 [DatabaseConfig.h](../src/database/DatabaseConfig.h)。

## 3. 关键实现

### 3.1 异步：一个队列，N 条连接

`Post` 把任务放进带锁的队列，空闲的工作线程取走执行。只有一条异步连接时，任务严格按提交顺序执行；多条时只保证开始的顺序。

`AsyncQuery` 返回 `QueryCallback`（[24](24-query-callback-and-holder.md)），结果通过 `std::promise` 交回来。

### 3.2 同步：等一条空闲连接

`Lease` 从空闲列表里取一条连接，用完放回去；都在用时在条件变量上等待。`SynchThreads` 决定同时能跑多少个阻塞查询，通常和工作线程数一样。

### 3.3 语句对象的所有权

`GetPreparedStatement` 用 `new` 创建语句，**交给哪个调用，就由它销毁**。放进事务的语句由事务持有。不要在调用之后再使用语句。

### 3.4 预编译

`PrepareStatements` 在每条连接上执行，异步连接的预编译放在它自己的线程上，所以一条连接始终只被一个线程使用。

## 4. 线程与安全

所有接口都可以在任意线程调用。但**主线程不要调用同步接口**：会阻塞整个主循环。[29](29-server-core.md) 里的 `co_await OnWorker` 就是在工作线程上调用它们的。

## 5. 设计取舍

连接池用普通的队列和条件变量，而不是拿 `io_context` 当队列、同步连接轮流 `try_lock`。连接池模板由使用方自己显式实例化，所以没有 `DatabaseWorkerPoolImpl.h`。

## 6. 常见的坑

- **同步连接数太少**：工作线程比同步连接多时，多出来的线程只能等待，看起来像数据库慢。
- **`Execute` 没有结果**：失败只记日志。需要知道结果时用 `AsyncCommitTransaction` 或 `co_await OnWorker` 里的同步接口。
- **异步连接多于一条时不保证顺序**：两条有先后关系的语句要放进一个事务。

## 7. 测试与示例

[tests/test_Database.cpp](../tests/test_Database.cpp)、[examples/db_query.cpp](../examples/db_query.cpp)、[benchmarks/bench_Database.cpp](../benchmarks/bench_Database.cpp)。
