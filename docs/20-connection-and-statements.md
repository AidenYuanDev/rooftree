# 20 · DatabaseConnection 与预处理语句

## 1. 是什么

- **`DatabaseConnection`**（[DatabaseConnection.h](../src/database/DatabaseConnection.h)）：一条到 PostgreSQL 的连接（libpq）。它保存这条连接上预编译好的语句，负责执行语句和事务，以及断线重连。
- **`PreparedStatement<T>`**（[PreparedStatement.h](../src/database/PreparedStatement.h)）：业务代码使用的语句对象：通过编号从连接池取得，设置参数，交给连接池执行。它不属于任何一条连接。

这一层建在 libpq 上，只支持 PostgreSQL。接口是语句编号、连接池和事务对象。

业务代码几乎不直接用 `DatabaseConnection`，而是通过[连接池](22-database-worker-pool.md)。但定义自己的数据库时要继承它，列出所有语句。

## 2. 定义一个数据库

[examples/db_query.cpp](../examples/db_query.cpp) 是最小的例子：

```cpp
enum DemoDatabaseStatements : uint32
{
    DEMO_INS_PLAYER,
    DEMO_SEL_PLAYER_BY_NAME,
};

class DemoDatabaseConnection : public DatabaseConnection
{
public:
    using Statements = DemoDatabaseStatements;      // 连接池要用这个枚举
    using DatabaseConnection::DatabaseConnection;

    void DoPrepareStatements() override
    {
        PrepareStatement(DEMO_INS_PLAYER, "INSERT INTO demo (name, score) VALUES (?, ?)");
        PrepareStatement(DEMO_SEL_PLAYER_BY_NAME, "SELECT id, name, score FROM demo WHERE name = ?");
    }
};

using DemoDatabaseWorkerPool = DatabaseWorkerPool<DemoDatabaseConnection>;
```

使用时：

```cpp
PreparedStatement<DemoDatabaseConnection>* stmt = DemoDatabase.GetPreparedStatement(DEMO_SEL_PLAYER_BY_NAME);
stmt->setString(0, "Thrall");
if (PreparedQueryResult rows = DemoDatabase.Query(stmt))     // Query 负责销毁 stmt
    RT_LOG_INFO("demo", "{}", rows->Fetch()[2].GetInt32());
```

## 3. 关键实现

### 3.1 占位符和启动时检查

语句里写 `?`，`PrepareStatement` 把引号以外的 `?` 依次换成 PostgreSQL 的 `$1`、`$2`，再用 `PQprepare` 在**每条连接**上预编译。

PostgreSQL 在预编译时就检查表、列和类型，所以**写错的语句在启动时就失败**，而不是用到时才出错。`PrepareStatements()` 返回 `false` 时应用应该退出，所以表必须先存在。

参数的类型由 PostgreSQL 根据语句推断。没有上下文可以推断时要显式转换：

```sql
INSERT INTO post_tag (post_id, tag_id) SELECT ?::bigint, id FROM tag WHERE slug = ?
```

### 3.2 参数都以文本发送

`setUInt32`、`setDate` 等都把值转成文本，由 PostgreSQL 按它推断的类型解析（`setBinary` 例外，用二进制格式）。所以 setter 的类型不必和列完全一致：

- `setBool` 发送 `1` 或 `0`，布尔列和整数列都接受。
- `setDate` 发送 UTC 时间，精确到秒。
- `setUInt64` 超过 `bigint` 上限时取上限：PostgreSQL 没有无符号类型，`id < 18446744073709551615` 这种"所有之前的"游标否则会让整条语句失败。
- `setString` 去掉 NUL 字符：PostgreSQL 的文本不能包含它。

### 3.3 错误和重连

语句失败时记一行 `sql.sql` 的 ERROR，包含 SQL 和错误信息，**不包含参数**（里面可能有密码）。

连接断开时重新连接、重新预编译，事务之外的语句再执行一次。

### 3.4 连接设置

每条连接都设置 `client_encoding=UTF8`、`TimeZone=UTC`。低于 PostgreSQL 14 时 `Open` 失败。

## 4. 和 MySQL 的差异（从 MySQL 迁移时踩过的）

这些差异大多不会让语句报错，只会让结果悄悄变样：

| 差异 | 怎么处理 |
|---|---|
| `user` 是保留字 | 表名是 `users` |
| 没有无符号类型 | id 和计数都用 `bigint` |
| `utf8mb4` 默认不区分大小写，文本 `=` 和唯一键都受影响 | 用户名、邮箱用排序规则 `ci`（ICU，`deterministic = false`）；网盘文件名用 `COLLATE "C"`（按字节比较） |
| `ENUM` 按声明顺序排序，换成 `text` 后按字母 | 语言要 `zh` 在前：`ORDER BY lang DESC` |
| 降序时 NULL 排在最前面，MySQL 排在最后 | 可能为 NULL 的列写 `DESC NULLS LAST` |
| `LIKE` 区分大小写 | 用 `ILIKE`（`ci` 排序规则的列要先 `COLLATE "default"`） |
| 布尔和整数不能混用 | `SUM(x = y)` 写成 `SUM(CASE WHEN x = y THEN 1 ELSE 0 END)`；`COALESCE(flag, false)` |
| 一条语句失败后整个事务都废了 | 见 [23](23-transaction.md)，用 `ON CONFLICT` 而不是让它失败 |
| `INSERT IGNORE`、`REPLACE`、`ON DUPLICATE KEY` | `ON CONFLICT DO NOTHING`、`ON CONFLICT (主键) DO UPDATE` |
| `LAST_INSERT_ID()` | `lastval()`，同样按连接计算；`DirectInsert` 用它 |
| `UTC_TIMESTAMP()` | 迁移脚本定义的 `utc_now()`，精确到秒，和 `timestamp(0)` 一致 |

## 5. 线程与安全

一条连接同一时间只能被一个线程使用，由连接池保证。语句对象创建后只被一个线程使用。

## 6. 常见的坑

- **参数按位置，从 0 开始**：同一个值用两次就要设置两次，`WHERE a = ? OR b = ?` 是两个参数。
- **忘记设置的参数就是 NULL**：和 `setNull` 效果一样，不会报错。
- **不要依赖连接上的状态**（`SET`、临时表、`lastval()`）：两次调用可能拿到不同的连接。需要同一条连接时用事务。

## 7. 测试与示例

- [tests/test_Database.cpp](../tests/test_Database.cpp)：需要 PostgreSQL，在 `build/bin/rooftree_test.yaml` 的 `Database.Test` 里配置。"what PostgreSQL does differently" 覆盖了第 3 节的大部分内容。
- [examples/db_query.cpp](../examples/db_query.cpp)：同步查询、事务、异步查询加回调。
