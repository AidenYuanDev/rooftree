# 01 · Define.h 与 Errors.h：基础类型和断言

## 1. 是什么

- **`Define.h`**：最底层的头文件，几乎每个文件都间接包含它。它只定义两样东西：整数类型的短名字（`uint32` 等），以及让编译器检查 printf 格式的 `ROOFTREE_PRINTF`。
- **`Errors.h`**：`ASSERT` 和 `ABORT`。遇到不该发生的状态时立刻终止进程，并留下 core dump，而不是带着错乱的状态继续运行、悄悄破坏数据。

两个文件里没有平台宏、字节序、动态链接导出宏、helgrind 注解这些东西：Rooftree 只在 Linux 上静态链接，用不上。

## 2. 核心接口

| 定义 | 位置 | 说明 |
|---|---|---|
| `int8` … `uint64` | [Define.h](../src/common/base/Define.h) | `std::int8_t` 等的别名 |
| `ROOFTREE_PRINTF(格式参数序号, 第一个参数序号)` | [Define.h](../src/common/base/Define.h) | 标在接受 printf 格式的函数上，GCC 和 Clang 都会检查调用处的参数类型 |
| `ASSERT(cond)` / `ASSERT(cond, "格式", 参数...)` | [Errors.h](../src/common/base/Errors.h) | 条件不成立时把位置、条件、消息写到 stderr，然后 `abort()` |
| `ABORT()` / `ABORT_MSG("格式", 参数...)` | [Errors.h](../src/common/base/Errors.h) | 无条件终止，比如卡死检测发现主线程不动了 |

```cpp
ASSERT(index < _parameters.size(), "Statement %u has %zu parameters, set one at %u", _index, _parameters.size(), index);
```

## 3. 关键实现

### 3.1 一个宏，两个重载

```cpp
#define ASSERT(condition, ...) \
    do { if (!(condition)) [[unlikely]] ::Rooftree::AssertionFailed(__FILE__, __LINE__, __func__, #condition __VA_OPT__(,) __VA_ARGS__); } while (0)
```

`__VA_OPT__(,)`（C++20）只在有消息时补上逗号，所以 `ASSERT(x)` 调用四个参数的重载，`ASSERT(x, "...", ...)` 调用带 printf 格式的那个。带格式的重载标了 `ROOFTREE_PRINTF(5, 6)`，`%u` 配上 `std::size_t` 这种错误编译时就会报出来。

### 3.2 输出到 stderr，然后 abort

[Errors.cpp](../src/common/base/Errors.cpp) 直接 `fprintf(stderr, ...)`、`fflush`、`std::abort()`，不经过日志系统：日志可能是异步的，进程马上就要结束，排在队列里的行可能来不及写出去。`abort()` 发出 SIGABRT，systemd 或 Docker 记下退出原因，内核按配置写 core dump，然后服务被重新拉起。

### 3.3 发布版本里也检查

`ASSERT` 在所有构建里都生效，没有标准库 `assert` 那种 `NDEBUG` 开关。它保护的是"一旦破坏就会损坏数据或内存"的不变量，这类检查不应该只在调试版本里存在。所以也**不要在 `ASSERT` 里写有副作用的表达式**：一是读代码的人不会想到条件本身在做事，二是将来如果加上开关，行为就会跟着变。

## 4. 线程与安全

`ASSERT` 可以在任何线程调用。`abort()` 结束的是整个进程，不只是当前线程。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| 写空指针制造段错误，信息存到全局变量供 gdb 查看 | `abort()` | 那是为了 Windows：那里 `abort()` 不触发崩溃报告。Linux 上 SIGABRT 一样会留下 core dump，信息已经在 stderr 里 |
| `WPFatal`（等 10 秒再崩溃）、`WPError`、`WPWarning`、`ASSERT_NOTNULL`、`ASSERT_WITH_SIDE_EFFECTS` | 没有 | 代码里没有用到 |
| `PERFORMANCE_PROFILING` 时 `ASSERT` 变成空操作 | 总是检查 | 见 3.3 |
| 平台、编译器、字节序、导出宏 | 没有 | 只在 Linux 上静态链接 |

## 6. 常见的坑

- **把用户输入的校验写成 `ASSERT`**：客户端发来的非法数据不是"不可能发生"的状态，写成 `ASSERT` 等于给攻击者一个让服务器重启的按钮。网络数据要校验，不合法就回错误或断开连接。
- **格式串和参数对不上**：现在编译器会检查，警告不要忽略。`std::size_t` 用 `%zu`，`std::string` 要 `.c_str()`。

## 7. 测试与示例

断言失败会结束进程，所以没有单元测试直接触发它。可以在任意测试里临时加一行 `ASSERT(false, "test %d", 42);`，运行后看 stderr 的输出和退出码（134 = 128 + SIGABRT）。

## 8. 思考题

1. 为什么 `ASSERT` 要包在 `do { ... } while (0)` 里？试着在 `if (x) ASSERT(y); else ...` 里去掉它看看。
2. `[[unlikely]]` 对生成的代码有什么影响？用 `objdump -d` 比较一下有和没有时的分支布局。
