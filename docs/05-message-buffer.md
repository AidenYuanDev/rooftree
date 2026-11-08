# 05 · MessageBuffer：网络收发缓冲区

## 1. 是什么

`MessageBuffer` 是一块带**读指针**和**写指针**的字节缓冲区。它是网络层的底层容器：

- `Socket` 用它接收数据：系统调用把字节写进写指针之后，业务代码从读指针开始解析；
- 待发送的数据也以 `MessageBuffer` 的形式排进 `Socket` 的写队列。

它只管"一段原始字节和两个位置"，不懂任何类型。包的内容是 FlatBuffers（[32](32-packets.md)）、HTTP（Beast）或 WebSocket 帧，由各自的解析器去读。

## 2. 核心接口

```
 _storage:  [ 已读（可丢弃） | 未读数据（active） | 空闲空间（remaining） ]
            0            _rpos               _wpos               size()
```

| 接口 | 作用 |
|---|---|
| `GetReadPointer()` / `GetWritePointer()` | 读、写位置的指针 |
| `ReadCompleted(n)` / `WriteCompleted(n)` | 读写位置前进 `n` 字节 |
| `GetActiveSize()` | 未读数据量 |
| `GetRemainingSpace()` | 写位置之后的空闲空间 |
| `Normalize()` | 把未读数据挪到开头，丢掉已读部分 |
| `EnsureFreeSpace()` | 缓冲区满了就扩容到 1.5 倍（至少 64 字节） |
| `Write(data, size)` | 拷贝到写位置并前进，空间要事先够 |
| `Reset()` | 两个位置归零，不释放内存 |

默认构造分配 4096 字节（[MessageBuffer.h](../src/common/base/MessageBuffer.h)）。

## 3. 关键实现

### 3.1 典型使用流程：接收一个完整的包

TCP 是字节流，一次 `read` 可能只收到半个包，也可能收到两个半包。用 `MessageBuffer` 的处理方式是：

```cpp
// 1. Socket 准备接收：先整理，再保证有空间（Socket.h:54 的 PrepareReadBuffer）
buffer.Normalize();
buffer.EnsureFreeSpace();
// 2. 系统调用把数据写到 GetWritePointer()，然后 WriteCompleted(n)

// 3. 业务代码解析：只要还有完整的包就一直处理
while (buffer.GetActiveSize() >= HeaderSize)
{
    uint16 size = ReadHeader(buffer.GetReadPointer());
    if (buffer.GetActiveSize() < HeaderSize + size)
        break;                                 // 包不完整，等下次数据
    Handle(buffer.GetReadPointer() + HeaderSize, size);
    buffer.ReadCompleted(HeaderSize + size);  // 消费掉这个包
}
```

没处理完的半个包留在缓冲区里，下次接收前 `Normalize()` 把它挪到开头，新数据接在后面。

### 3.2 `Normalize()` 的代价

只有读位置不在开头时才需要整理。数据已经全部读完时，`memmove` 的长度是 0，实际只是把两个位置归零。大多数情况下一次收到的都是完整的包，所以通常没有拷贝开销。

### 3.3 `EnsureFreeSpace()` 只在"完全满了"时扩容

注释写着"确保有一些空间"：它只在 `GetRemainingSpace() == 0` 时扩容。也就是说只要还剩 1 个字节，就不会扩容，那次 `read` 最多只能读 1 个字节。这是在"省内存"和"减少系统调用次数"之间的取舍：大部分连接的流量都很小，不值得给每个连接预留大块空间。

## 4. 线程与安全

没有任何同步。一个 `MessageBuffer` 同一时间只能被一个线程使用。在网络层中，读缓冲区只在所属的网络线程上访问（[16 · Socket](16-socket.md)）。

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| 移动后源对象的存储被移走，但读写位置保留 | 移动后源对象的位置归零 | 被移动的对象处于一致的空状态 |
| `Release() &&` 交出底层 `vector` | 没有 | 没有用到 |
| `EnsureFreeSpace` 对大小为 0 的缓冲区扩到 0 | 至少 64 字节 | 用 `MessageBuffer(0)` 构造后也能接收数据 |

## 6. 常见的坑

- **`Write()` 不检查空间**：直接 `memcpy`，空间不够就会写越界。构造时给足大小，或者先检查 `GetRemainingSpace()`。例如 [echo_server](../examples/echo_server.cpp) 构造回包时就用 `MessageBuffer reply(packet.GetActiveSize())` 预先分配了准确的大小。
- **`GetReadPointer()` 在 `Normalize()` 或扩容后会失效**：不要跨这两个操作保存指针。
- **默认构造会分配 4KB**：大量创建小包时，用带大小的构造函数。

## 7. 测试与示例

- [tests/test_Framework.cpp](../tests/test_Framework.cpp) 的 "MessageBuffer tracks read and write positions"：读写位置、整理、扩容、移动。
- [examples/echo_server.cpp](../examples/echo_server.cpp) 的 `ReadHandler`：读出全部未读数据，构造一个新的 `MessageBuffer` 作为回包。

## 8. 思考题与练习

1. 在纸上画出：缓冲区 4096 字节、`_rpos = 4000`、`_wpos = 4090` 时调用 `Normalize()` 前后的状态。
2. 实现第 3.1 节的拆包循环，写一个测试：把三个包的字节随机切成若干段，逐段 `Write` 进缓冲区，确认三个包都被完整解析出来。
3. 如果攻击者声称包体有 1GB，上面的循环会发生什么？应该在哪里加限制？
