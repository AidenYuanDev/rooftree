# 03 · StringFormat、StringConvert、Util：字符串

## 1. 是什么

- **`StringFormat.h`**：[{fmt}](https://github.com/fmtlib/fmt) 的薄封装。日志（`RT_LOG_*`）、控制台输出、错误信息，所有拼字符串的地方都用它。
- **`StringConvert.h`**：`Rooftree::StringTo<T>(text)`，把整段文本读成一个数或 bool，读不了就返回空。用来解析配置值、URL 参数、命令参数。
- **`Util.h`**：几个小工具：按分隔符切分、十六进制、UTF-8 长度和截断、"1d12h" 这样的时长。

## 2. 核心接口

### StringFormat.h

| 接口 | 作用 |
|---|---|
| `Rooftree::StringFormat(format, args...)` | 格式化，返回 `std::string` |
| `Rooftree::StringFormatTo(out, format, args...)` | 写到输出迭代器，不产生中间字符串 |
| `Rooftree::FormatString<Args...>` | 即 `fmt::format_string`：格式串和参数在**编译期**检查 |
| `Rooftree::StringVFormat(format, args)` | 参数已经打包成 `FormatArgs` 时用（日志的实现），格式不对时返回说明文字而不是抛异常 |
| `fmt::formatter<Optional<T>>` | `Optional` 直接格式化，空值输出 `(none)` |
| `format_as(enum)` | 枚举按底层整数格式化 |

```cpp
std::string s = Rooftree::StringFormat("{} has {} hp ({:.1f}%)", name, hp, hp * 100.0 / maxHp);
```

### StringConvert.h

`StringTo<T>(text, base = 10)` 返回 `Optional<T>`：

```cpp
StringTo<uint32>("42")          == 42
StringTo<uint32>(" 42")         == nullopt   // 前后不能有空格
StringTo<uint32>("42tail")      == nullopt   // 不能有多余字符
StringTo<int32>("+42")          == nullopt   // 不接受 '+'
StringTo<uint32>("5000000000")  == nullopt   // 超出范围
StringTo<uint32>("0xff")        == nullopt   // 默认 10 进制
StringTo<uint32>("0xff", 0)     == 255       // base 0：按 0x / 0b 前缀决定，没有前缀就是 10 进制
StringTo<double>("2.5e-3")      == 0.0025
StringTo<bool>("yes") / ("on") / ("1") == true    // 也接受 true、y，大小写不限
```

### Util.h

| 接口 | 作用 |
|---|---|
| `Tokenize(text, separator, keepEmpty)` | 切分成 `string_view`，指向原文本 |
| `StringEqualI` / `StringStartsWithI` / `CharToLower` / `CharToUpper` | 只处理 ASCII 字母的大小写 |
| `ToHex(bytes)` / `FromHex(text)` | 大写十六进制，两位一个字节；`FromHex` 遇到非法字符或奇数长度返回空 |
| `Utf8Length(text)` / `Utf8Truncate(text, n)` | 按码点计数和截断；不是合法 UTF-8 时分别返回 0、清空 |
| `ParseDuration("1d12h")` / `FormatDuration(seconds)` | 控制台的封禁时长、停服延迟 |

## 3. 关键实现

### 3.1 编译期检查格式串

`FormatString<Args...>` 的构造函数是 `consteval` 的：`{}` 的数量和参数类型不匹配就编译失败。日志宏和 `CommandHandler::PSendSysMessage` 也接受 `FormatString`，所以它们的格式串同样在编译期检查。

### 3.2 StringTo 用 `std::from_chars`

[StringConvert.h](../src/common/base/StringConvert.h) 用 `if constexpr` 分成整数、浮点数、bool 三种情况，整数和浮点数都交给 `std::from_chars`。它不看 locale、不跳过空白、不分配内存，只要求整段文本都被读完（`stop == end`）且没有出错。`from_chars` 本身就会检查溢出和范围，所以 `StringTo<uint8>("256")` 直接返回空。

### 3.3 十六进制是大写

`ToHex` 输出大写。这不是风格问题：会话令牌、邮件令牌的 SHA-256 哈希就是这样写进数据库的，改成小写的话，已有的登录全部会失效。

## 4. 线程与安全

都是纯函数，可以在任何线程调用。`Tokenize` 返回的 `string_view` 指向传入的文本，文本必须比结果活得久。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| `StringTo<double>` 用 `std::stold`，接受前导空白和十六进制浮点 | `std::from_chars`，只接受十进制和科学计数法 | 和整数一致：整段文本必须恰好是一个数 |
| `Rooftree::ToString` | 没有 | 用 `std::to_string` 或 `StringFormat` |
| `Util.h` 600 行：Windows 控制台、宽字符转换、金钱字符串、IP 掩码等 | 只剩上面的十几个函数 | 代码里没有用到 |
| `utf8length` 遇到非法 UTF-8 会**清空传入的字符串** | `Utf8Length` 只读、返回 0 | 计算长度不应该改动参数 |
| `utf8truncate` 先转成宽字符串再截断 | 按码点直接找截断位置 | Linux 上 `wchar_t` 是 32 位，那种写法要经过 UTF-16，绕了一圈 |
| `secsToTimeString` 三种格式，`TimeStringToSecs` 单位可以重复、乱序，没有单位的数字被忽略 | `FormatDuration` 一种格式，`ParseDuration` 要求单位按 d h m s 的顺序、各出现一次 | 控制台只用到这一种；写错的时长报错，而不是被悄悄读成别的值 |
| 格式化出错时返回错误说明 | `StringFormat` 直接抛出（编译期检查过，几乎不会发生），`StringVFormat` 返回说明 | 编译期已经检查过的格式不需要运行时兜底 |

## 6. 常见的坑

- **把 `Tokenize` 的结果存起来**：它们只是视图。要保存就拷贝成 `std::string`。
- **用 `StringTo<int>` 读用户输入的数字再去比较大小**：先检查是否为空，再检查范围。空的 `Optional` 和 0 是不同的。

## 7. 测试与示例

- [tests/test_StringConvert.cpp](../tests/test_StringConvert.cpp)：`StringTo` 的整数、进制、范围、浮点数、bool。
- [tests/test_Framework.cpp](../tests/test_Framework.cpp)：`"UTF-8 is counted and cut by code point"`、`"Text helpers: split, hex, case, durations"`。
- [benchmarks/bench_Base.cpp](../benchmarks/bench_Base.cpp)：`StringFormat` 对 `fmt::format` 和 `snprintf`，`StringTo`，UTF-8。

## 8. 思考题

1. `StringTo<int8>("-128")` 能读出来，`StringTo<int8>("-0x80", 0)` 呢？为什么？
2. 用户名比较为什么用 `StringEqualI` 而不是先转小写再比较？对于非 ASCII 的用户名，这两种做法有什么区别？
