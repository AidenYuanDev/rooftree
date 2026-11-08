# 13 · 线程间队列：LockedQueue、ProducerConsumerQueue、MPSCQueue

## 1. 是什么

"逻辑单线程、IO 多线程"的模型里，线程之间要传递数据：网络线程把收到的包交给主线程，主线程把任务交给工作线程。这三种队列就是做这件事的工具，各有侧重：

| 队列 | 实现 | 消费者没有数据时 | 适合的场景 | 游戏里的用法 |
|---|---|---|---|---|
| `LockedQueue` | `std::mutex` + `std::deque` | 立即返回 `false` | **轮询**：消费者每帧来取一次 | 会话的收包队列（网络线程 → 主线程的包） |
| `ProducerConsumerQueue` | `std::mutex` + 条件变量 | **阻塞等待** | 工作线程：没活就睡觉 | 地图更新器：每个世界 tick 把各地图的更新分给几个线程 |
| `MPSCQueue` | **无锁**链表 | 立即返回 `false` | 多个生产者、一个消费者，追求高性能 | 连接的发送队列（任意线程 → 网络线程的发送包） |

Rooftree 里的用法：`SessionMgr` 用侵入式 `MPSCQueue` 收网络线程送来的消息、用 `LockedQueue` 收工作线程完成的任务（[29](29-server-core.md)）；WebSocket 和 KCP 的发送队列是非侵入式 `MPSCQueue`。

`MPSCQueue` 的算法见 3.2。`LockedQueue` 的过滤取出和 `ProducerConsumerQueue` 现在还没有调用者，是留给游戏的：一个玩家的包分给地图线程和世界线程处理（3.3），地图更新分给几个工作线程（3.4）。

## 2. 核心接口

### LockedQueue\<T\>

| 接口 | 作用 |
|---|---|
| `add(item)` | 加到队尾 |
| `next(item)` | 取出队头；队列为空时返回 `false`，从不等待 |
| `next(item, filter)` | 只有 `filter(队头)` 返回 `true` 才取出；否则队头留在原处 |
| `readd(first, last)` | 把取出但还不能处理的一批放回队头，保持原来的顺序 |
| `empty()` | 拿到结果时可能已经过期 |

### ProducerConsumerQueue\<T\>

| 接口 | 作用 |
|---|---|
| `Push(item)` | 加入并唤醒一个等待者；取消以后什么都不做 |
| `Pop(item)` | 不等待；空了或者已取消时返回 `false` |
| `WaitAndPop(item)` | 等到有数据；已取消时返回 `false`，工作线程据此退出 |
| `Cancel()` | 丢掉队列里剩下的，唤醒所有等待者 |

### MPSCQueue\<T [, &T::Link]\>

| 接口 | 作用 |
|---|---|
| `Enqueue(T*)` | 任意线程都可以调用，无锁 |
| `Dequeue(T*&)` | **只能由一个线程调用**；空时返回 `false` |

队列拥有放进去的指针，析构时 `delete` 剩下的元素。

## 3. 关键实现

### 3.1 LockedQueue

一个 `std::mutex` 加一个 `std::deque`。消费者每帧来取一次，生产者和消费者都很少，锁的竞争可以忽略。

### 3.2 MPSCQueue：生产者推栈，消费者整批取走

[MPSCQueue.h](../src/common/thread/MPSCQueue.h) 的核心是一个无锁栈 `MPSCStack`：

```cpp
void Push(Item* item)               // 任意线程
{
    Item* top = _top.load(relaxed);
    do
        Link::Get(item).store(top, relaxed);
    while (!_top.compare_exchange_weak(top, item, release, relaxed));
}

Item* Pop()                         // 只有消费者
{
    if (!_ready)                    // 手上的一批取完了
    {
        Item* taken = _top.exchange(nullptr, acquire);  // 一次拿走整个栈
        ... 把 taken 这条链反转，接到 _ready 上 ...
    }
    取出 _ready 的第一个
}
```

- **生产者**：用 CAS 把元素压到栈顶，失败就用新的栈顶重试。
- **消费者**：手上没有元素时，用一次 `exchange` 把整个栈拿走，栈立刻变空，生产者可以继续推。拿到的链是"最新的在前"，反转一次就变成推入的顺序，存在只有消费者访问的 `_ready` 里，之后逐个取出。
- **顺序**：同一个生产者的元素按它推入的顺序出来；不同生产者之间按批次先后。会话的消息只需要前一种保证。
- **内存序**：所有对 `_top` 的修改都是读-改-写操作（CAS、exchange），它们组成同一个 release 序列。消费者的 acquire `exchange` 能看到之前所有生产者写好的 next 指针。

两种形式共用这个栈：

| 形式 | next 指针在哪 | 每个元素的分配 |
|---|---|---|
| `MPSCQueue<T>` | 队列自己分配的节点 | 一个节点 |
| `MPSCQueue<T, &T::Link>`（侵入式） | `T` 自己的 `std::atomic<T*> Link` 成员 | 无。`SessionMgr` 收消息就用这种 |

### 3.3 一个会话的包给两个线程处理（游戏要用）

会话的收包队列是一个 `LockedQueue`。地图线程更新一个地图时，用一个过滤器只取出这张地图上可以安全处理的包（移动、战斗）；世界线程用另一个过滤器取出其余的（聊天、好友、拍卖）。两边轮询同一个队列，各自只拿自己的：

```cpp
auto onMap = [](Packet const& packet) { return packet.HandledOnMap(); };
while (session.Queue.next(packet, onMap))      // 地图线程
    Handle(packet);
```

过滤器拒绝时队头**留在原处**，后面的包也不会越过它被取走：一个玩家发的包，无论分给哪个线程，处理顺序和发送顺序一致。处理到一半发现暂时不能处理（比如玩家正在切换地图）的包，用 `readd` 放回队头，下一次再处理。

Rooftree 现在的会话都在主线程上处理（[29](29-server-core.md)），游戏有了地图线程以后就用这个办法分包。

### 3.4 工作线程等任务（游戏要用）

地图更新器在每个世界 tick 把每张地图的 `Update` 作为一个任务 `Push` 进 `ProducerConsumerQueue`，几个工作线程 `WaitAndPop` 取出执行，全部完成后世界 tick 才继续。没活的时候线程睡在条件变量上，不占 CPU：

```cpp
std::unique_ptr<MapUpdateRequest> request;
while (queue.WaitAndPop(request))              // Cancel() 之后返回 false，线程结束
    request->Run();
```

Rooftree 自己的工作线程用的是 Asio 的线程池（[14](14-thread-pool-and-asio.md)）；地图更新需要"一批任务、等它们全做完"时用这个队列更直接。

## 4. 线程与安全

| | 生产者 | 消费者 |
|---|---|---|
| `LockedQueue` | 任意多个线程 | 任意多个线程 |
| `ProducerConsumerQueue` | 任意多个线程 | 任意多个线程 |
| `MPSCQueue` | 任意多个线程 | **只能一个线程** |

## 5. 设计取舍

| 没有采用的做法 | Rooftree | 原因 |
|---|---|---|
| `MPSCQueue`：Dmitry Vyukov 的链表队列，有一个哑元节点；侵入式版本用一块未初始化的 `T` 大小的内存充当哑元 | 推栈 + 整批取走 | 不需要哑元，侵入式版本也就不需要那块伪造的 `T`。吞吐相当：100 万个元素、1 到 8 个生产者，侵入式快 0–14%，非侵入式在 −7% 到 +13% 之间 |
| `LockedQueue::next(item, checker)`：checker 是一个带 `Process()` 的对象 | `next(item, filter)`：任何返回 `bool` 的可调用对象，lambda 就行 | 写起来更短 |
| `ProducerConsumerQueue::Cancel()` 对指针元素 `delete` | 只是丢掉；要拥有元素就放 `std::unique_ptr` | 队列不知道指针归不归它，别人还拿着的话就重复释放了 |
| `WaitAndPop` 取消时返回 `void`，`value` 保持原样 | 返回 `bool` | 调用者不用另外检查是否取消，也不会把没赋值的变量当成任务 |
| `LockedQueue::cancel`、`pop_front` | 没有 | 没有用到 |

## 6. 常见的坑

- **`MPSCQueue` 析构时会 `delete` 剩下的元素**：放进去的必须是 `new` 出来、归队列所有的指针。
- **侵入式队列里的元素不能同时在两个队列里**：它只有一个 `Link`。
- **`LockedQueue::empty()` 的结果一拿到就可能过期**：判断完 `empty()` 再 `next()` 之间，别的线程可能已经改了队列。直接用 `next()` 的返回值来判断。

## 7. 测试与示例

- [tests/test_Framework.cpp](../tests/test_Framework.cpp)：`"LockedQueue: each thread takes the front item its filter accepts, readd puts items back"`（模拟地图线程和世界线程分包）、`"ProducerConsumerQueue wakes waiting workers, Cancel ends them"`。
- [benchmarks/bench_Thread.cpp](../benchmarks/bench_Thread.cpp)：两种队列在一个和多个生产者下的吞吐。
- [tests/test_ServerCore.cpp](../tests/test_ServerCore.cpp)：会话的消息经过侵入式 `MPSCQueue`，多个线程投递时每个会话的顺序不变。

## 8. 思考题与练习

1. 用 `LockedQueue` 模拟"网络线程 → 主线程"：一个线程不断 `add` 字符串，主线程每 50ms 把队列里的内容全部取出来打印。
2. 写一个测试：8 个线程同时往 `MPSCQueue` 里各放 10 万个元素，一个线程取出，验证总数正确、每个生产者的元素顺序不变。再在 TSan 构建下运行。
3. 为什么网络发送队列选用 `MPSCQueue`，而接收队列选用 `LockedQueue`？从"谁是生产者、谁是消费者、消费者以什么方式取数据"三个角度回答。
