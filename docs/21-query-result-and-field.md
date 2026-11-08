# 21 · ResultSet 与 Field：读取查询结果

## 1. 是什么

`ResultSet`（[QueryResult.h](../src/database/QueryResult.h)）是一次查询返回的行，`Field`（[Field.h](../src/database/Field.h)）是其中的一个值。`QueryResult` 和 `PreparedQueryResult` 都是 `std::shared_ptr<ResultSet>`。

## 2. 用法

```cpp
if (QueryResult rows = Database.Query(stmt))     // 没有行时是空指针
{
    do
    {
        Field* fields = rows->Fetch();
        uint32 id = fields[0].GetUInt32();
        std::string name = fields[1].GetString();
        Optional<std::string> nick = fields[2].GetStringOrNull();
    } while (rows->NextRow());
}
```

| 接口 | 作用 |
|---|---|
| `Fetch()` | 当前行的 `Field` 数组 |
| `NextRow()` | 下一行，没有了返回 `false` |
| `GetRowCount()`、`GetFieldCount()` | 行数、列数 |
| `(*rows)["name"]` | 按 SELECT 里的列名或别名取 |
| `Field::GetUInt8` … `GetInt64`、`GetFloat`、`GetDouble`、`GetBool` | 数字 |
| `Field::GetString`、`GetStringView`、`GetCString` | 文本 |
| `Field::GetDate` | 时间，按 UTC |
| `Field::GetBinary`、`GetBinaryView` | `bytea` |
| `Field::IsNull`、`Get...OrNull` | 区分 NULL 和 0 或空字符串 |

## 3. 关键实现

### 3.1 结果是文本，按需解析

结果都以文本格式接收，getter 在调用时解析。所以 getter 不用和列的宽度完全一致，`GetUInt32` 能读任何宽度的整数列，`SUM` 返回的 `numeric` 也能读。

- 布尔值是 `t` 或 `f`，数字 getter 会读成 1 或 0，所以 `SELECT a = b` 可以用 `GetBool` 读。
- `bytea` 列（OID 17）在切换到这一行时解码，`GetBinary` 得到原始字节。
- 解析失败时记一行 ERROR，返回 0，不会崩溃。也可以在类型不匹配时 ASSERT：查询结果都由自己的 SQL 决定，但不值得为此让服务器崩溃。

### 3.2 空结果用空指针表示

没有行时 `Query` 返回空指针，所以不用再判断 `GetRowCount() == 0`。语句失败时也是空指针，错误记在日志里。

## 4. 线程与安全

`ResultSet` 不是线程安全的，但可以把它交给另一个线程，比如从工作线程到主线程。

## 5. 常见的坑

- **`Field*` 只在当前行有效**：`NextRow()` 之后不要再用。要保留就用 `GetString()` 这样的副本。
- **列的位置要和 SELECT 一致**：多处共用列的时候，用宏定义列（比如博客的 `BLOG_USER_COLUMNS`），改的时候一起改。

## 6. 测试与示例

[tests/test_Database.cpp](../tests/test_Database.cpp) 的 "Database round-trips values synchronously"。
