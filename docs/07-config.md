# 07 · Config：配置

## 1. 是什么

`sConfigMgr` 是全局唯一的配置管理器。它负责加载 YAML 配置文件（用系统安装的 [yaml-cpp](https://github.com/jbeder/yaml-cpp) 解析），每次加载都生成一个**不可变的快照** `Config`。任何模块都可以取得当前快照，从里面读取单个值，也可以把一整段配置直接读成结构体：

```cpp
std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
uint16 port = config->Get<uint16>("Echo.Port", 7000);
auto realms = config->Get<std::vector<RealmInfo>>("Realms", {});
```

项目里的实际例子是数据库配置：[DatabaseConfig.h](../src/database/DatabaseConfig.h) 为 `DatabaseConnectionInfo` 和 `DatabaseConfig` 定义了 `YAML::convert`，`config->Get<DatabaseConfig>("Database.Demo")` 就能读出整个数据库的设置（见 [22](22-database-worker-pool.md)）。

配置按"快照 + 结构化读取 + 重载通知"设计，见第 5 节。

## 2. 核心接口

### ConfigMgr：加载和发布快照

| 接口 | 作用 | 位置 |
|---|---|---|
| `LoadFile(path, error)` / `LoadString(text, error)` | 解析并发布一个新快照；失败时返回 `false`，当前快照保持不变 | [Config.cpp:169](../src/common/config/Config.cpp#L169)、[:183](../src/common/config/Config.cpp#L183) |
| `Reload(error)` | 重新读取当前快照来自的文件 | [Config.cpp:197](../src/common/config/Config.cpp#L197) |
| `GetConfig()` | 当前快照，永远不为空（加载前是空配置），任意线程可调用 | [Config.cpp:208](../src/common/config/Config.cpp#L208) |
| `Get<T>(path, def)` | 只读一个值时的简写，等于 `GetConfig()->Get<T>(path, def)` | [Config.h:134](../src/common/config/Config.h#L134) |
| `OnLoaded(callback)` | 订阅加载：已加载时立即调用一次，之后每次加载成功都会调用；返回的 `Subscription` 析构时自动取消订阅 | [Config.cpp:227](../src/common/config/Config.cpp#L227) |

### Config：一个快照

| 接口 | 作用 | 位置 |
|---|---|---|
| `Get<T>(path)` / `Get<T>(path, def)` | 用 `YAML::convert<T>` 解码一个值或一整段配置：整数、字符串、`vector`、`map`、自定义结构体……缺失、为空或转换失败时返回 `nullopt` / 默认值 | [Config.h:67](../src/common/config/Config.h#L67) |
| `GetNode(path)` | 一段配置的 YAML 子树（深拷贝），缺失时 `IsDefined()` 为 false | [Config.cpp:138](../src/common/config/Config.cpp#L138) |
| `GetKeys(path)` | 一个映射的键，按文件顺序；`""` 是根 | [Config.cpp:132](../src/common/config/Config.cpp#L132) |
| `Has(path)` / `IsMap(path)` | 是否存在 / 是否是映射 | [Config.cpp:122](../src/common/config/Config.cpp#L122) 起 |
| `GetFilename()` | 来自哪个文件，`LoadString` 加载的为空 | |

### 路径规则

路径就是嵌套映射的各层键用点连起来：

```yaml
Database:
  Demo:
    Host: "127.0.0.1"                           # 路径 Database.Demo.Host
    Port: 3306                                  # 路径 Database.Demo.Port

Logger:
  root: "3,Console"                             # 路径 Logger.root
  server.http: "2,Console"                      # 键本身可以包含点：Logger.server.http
```

### 环境变量覆盖

文件里的每个值都可以用环境变量覆盖：加上 `ROOFTREE_` 前缀，点换成下划线，全部大写（[Config.cpp:22](../src/common/config/Config.cpp#L22)）。

```sh
ROOFTREE_ECHO_PORT=7001 ./echo_server                     # 覆盖 Echo.Port
ROOFTREE_DATABASE_DEMO_PASSWORD=secret ./db_query         # 覆盖 Database.Demo.Password
```

这对容器部署和测试很方便：配置文件放默认值，密码等敏感信息通过环境变量传入。

## 3. 关键实现

### 3.1 不可变快照

`Config::Parse`（[Config.cpp:91](../src/common/config/Config.cpp#L91)）把解析好的 YAML 树包装成一个 `Config`，之后再也不修改它。`ConfigMgr` 只保存一个 `std::shared_ptr<Config const>`：

- **加载**：在锁外解析出新的 `Config`，再在锁内把指针换掉（`Publish`，[Config.cpp:214](../src/common/config/Config.cpp#L214)）。锁只保护这一次指针交换。
- **读取**：`GetConfig()` 在锁内复制一份 `shared_ptr` 就返回，之后的读取都在调用方手里的快照上进行。

这样有两个好处：

1. **多次读取不会混入两个版本**。如果每读一个值都去查"当前配置"，两次读取之间可能发生重载，结果一半来自旧文件、一半来自新文件。从同一个快照读，就不会出现这种情况。
2. **快照的生命周期由 `shared_ptr` 管理**。重载后旧快照仍然有效，直到最后一个持有者放手才释放。

### 3.2 按路径建索引

`Index`（[Config.cpp:61](../src/common/config/Config.cpp#L61)）遍历整棵树，建立两张按路径查找的有序表：

- `Nodes`：每个映射和值对应的 YAML 节点，供 `Get` / `GetNode` / `Has` 使用。
- `Keys`：每个映射的键（按文件顺序），供 `GetKeys` / `IsMap` 使用。

为什么不直接按点拆开路径、一层一层地查？因为 `"Logger.server.http"` 拆成三层就找不到 `Logger` 下面名为 `server.http` 的键，而 logger 的名字本身就带点。

建表时会拒绝**重复的键**，让加载失败。YAML 规范本身就不允许重复键；同一个路径的两种写法 `server.http:` 和 `server: { http: }` 也算重复，否则就说不清哪个生效。

### 3.3 为什么 `Get` 要复制节点

快照不可变，那多个线程直接读同一棵 YAML 树不就行了？对 yaml-cpp 来说不行：它的"读"不一定只读。对非 `const` 节点用 `[]` 查一个**不存在**的键，会往树里插入一个新节点（`yaml-cpp/node/detail/impl.h` 的 `node_data::get`），节点内部还有 `mutable` 缓存。两个线程拿着同一棵树做这种"读"，就是数据竞争。

所以共享的树从不交给调用方：`GetNode`（[Config.cpp:138](../src/common/config/Config.cpp#L138)）在快照自己的锁 `TreeLock` 下用 `YAML::Clone` 复制出一份，`Get<T>` 再在这份拷贝上调用 `as<T>()`。调用方拿到的节点只属于自己，在上面怎么用都不会影响别人。

代价是每次 `Get` 都要复制，单个值约 0.2–0.4 µs，200 个键的表解码成 `map` 约 120 µs（`benchmarks/bench_Base.cpp` 的 "Config lookups"）。所以配置应该在加载时读一次、缓存起来，见第 4 节。

### 3.4 环境变量在加载时套用

`Parse` 在快照发布之前，把每个值对应的 `ROOFTREE_` 环境变量写进树里（[Config.cpp:109](../src/common/config/Config.cpp#L109)）。之后环境变量再怎么变，这个快照都不受影响；下一次 `Reload()` 会重新读取环境变量。只有文件里存在的值才会被覆盖，环境变量不会凭空加出新的键。

### 3.5 类型转换

所有转换都用 yaml-cpp 的 `YAML::convert<T>`，单个值和结构体是同一套规则：

- 不看有没有引号：`Port: "7000"` 也能读成整数 7000，`Get<std::string>` 读 `Port: 7000` 得到 `"7000"`。
- 布尔值只认 `true`/`false`、`yes`/`no`、`on`/`off`，**不认 `1`/`0`**。
- 超出范围也算转换失败：`Get<uint8>` 读 `1234` 返回默认值。

转换失败时返回默认值或 `nullopt`，**不会报错**。需要区分"没写"和"写错"时，用 `Has(path)` 判断。

## 4. 使用方式与线程安全

推荐的用法：**加载时把配置读进结构体缓存起来**，热路径上只用缓存的值。用 `OnLoaded` 订阅，重载后自动刷新：

```cpp
struct EchoSettings { std::string BindIP; uint16 Port; };

class EchoServer
{
public:
    EchoServer()
    {
        // 已经加载过配置时立即调用一次，之后每次 Reload() 成功都会再调用
        _configSubscription = sConfigMgr->OnLoaded([this](Config const& config)
        {
            EchoSettings settings;
            settings.BindIP = config.Get<std::string>("Echo.BindIP", "0.0.0.0");
            settings.Port = config.Get<uint16>("Echo.Port", 7000);
            std::lock_guard lock(_settingsLock);
            _settings = std::move(settings);
        });
    }

private:
    std::mutex _settingsLock;
    EchoSettings _settings;
    ConfigMgr::Subscription _configSubscription;   // 析构时自动取消订阅
};
```

- `GetConfig()`、`Config` 的所有读取函数都可以在任意线程调用。
- 回调在执行加载的线程上调用，一次只调用一个。回调里**不能**再加载配置或订阅、取消订阅，否则会死锁。
- `Subscription` 要在它引用的对象之前析构。把它声明成对象的最后一个成员，就会最先析构。

## 5. 设计取舍

整个模块是新写的，接口也没有保留：

| | 没有采用的做法 | Rooftree |
|---|---|---|
| 文件格式 | INI（Boost.PropertyTree） | YAML（yaml-cpp），支持嵌套映射和列表 |
| 读取 | `GetIntDefault` / `GetBoolDefault` ……按类型区分 | `Get<T>`，任意可以用 `YAML::convert<T>` 解码的类型，包括结构体 |
| 一致性 | 每次读取单独加锁，两次读取之间可能发生重载 | 从同一个快照读，不会混入两个版本 |
| 重载后刷新 | 调用方自己记得重新读（`World::LoadConfigSettings(true)`） | `OnLoaded` 订阅 |
| 环境变量覆盖 | 每次读取时检查 | 加载时套用，快照之后不再变化 |
| 布尔值 | `1`/`0` 也可以 | 只认 `true`/`false`/`yes`/`no`/`on`/`off` |
| 多个配置文件 | 支持 `LoadAdditionalFile`、命令行覆盖 | 只有一个文件 |

## 6. 常见的坑

- **缩进决定层级**：例如 `LogsDir: ""` 如果缩进到 `Appender:` 下面，就会变成 `Appender.LogsDir`。YAML 不允许用 Tab 缩进。
- **字符串最好加引号**：值里出现 `: ` 或 ` #` 会被解析成映射或注释。
- **根节点必须是映射**：`LoadString("key = value")` 在 YAML 里是一个合法的纯标量，会被当成错误拒绝；空文档则视为空配置。
- **开关写成 `1`/`0` 读不出来**：`Get<bool>` 会返回默认值，要写 `true`/`false`。
- **类型写错不会报错**：见 3.5。怀疑配置没生效时，先检查值的类型。
- **键重复会导致加载失败**：错误信息里有重复的完整路径和行号，见 3.2。
- **`Get` 每次都会复制**：不要在热路径上反复调用，见第 4 节。
- **环境变量只能覆盖文件里已有的值**，而且只在加载时读取一次，见 3.4。
- **加载失败时当前快照保持不变**，订阅者也不会被通知。

## 7. 测试与示例

- [tests/test_Framework.cpp](../tests/test_Framework.cpp)：
  - "Config reads values by path"：各种类型、带点的键、`GetKeys` / `Has` / `IsMap`、缺失和类型不符；
  - "Config applies environment overrides when it is loaded"：加载时套用，之后快照不变；
  - "Config rejects invalid files and keeps the current one"：解析失败、重复键、文件不存在；
  - "Config decodes nested YAML into structs"：自定义结构体、映射组成的列表、拷贝和快照互不影响；
  - "ConfigMgr notifies subscribers of every load"：立即调用、`Reload`、加载失败不通知、取消订阅；
  - "Config can be reloaded while other threads read it"：4 个线程读取时重载 500 次，不会读到混合的版本。
- [examples/rooftree.yaml.dist](../examples/rooftree.yaml.dist)：示例程序的配置，每个示例的 `main()` 开头都有加载和读取配置的代码。

## 8. 思考题与练习

1. 给 `echo_server` 加一个配置项 `Echo.MaxConnections`，用 `OnLoaded` 缓存它，在 `OnSocketOpen` 里超过这个数量时直接关闭新连接。改配置后调用 `Reload()`，验证新的上限立即生效。
2. 写一个测试：取得快照后修改环境变量再读取，值会变吗？再调用一次 `LoadString` 呢？这说明环境变量是什么时候读取的？
3. 如果要支持"配置文件包含另一个配置文件"，你会在 `Parse` 的哪一步合并？重复键的检查要怎么处理？
