# UE5 网络架构与 UDP 可靠性封装机制
---

# 一、应用层逻辑封装

在应用层逻辑封装中，**RPC** 和 **复制（Replication）** 是最常用的两种网络同步机制。

## 1. RPC

### 1.1 RPC 的可靠性

RPC 分为可靠和不可靠，默认为不可靠，需要指定为 `Reliable` 才保证可靠。

### 1.2 RPC 的类型

RPC 分为：

- **Server RPC**
- **Client RPC**
- **Multicast RPC**

以 Server RPC 为例子，假如客户端调用 RPC 函数，就会把函数名字加参数打包发送给服务器端，服务器端匹配成功后执行服务器的 `_Implentment` 代码，不会执行客户端的函数逻辑。

Client RPC 同理。

### 1.3 RPC 的顺序保证

UE 的 RPC 没有全局顺序保证，是否保序完全取决于你声明的可靠性模式：

- **不可靠 RPC（默认）**：既不保证到达，也不保证顺序。
- **可靠 RPC（手动加 `Reliable` 关键字）**：仅在「同一个 Actor + 同一传输方向」的范围内，严格按发送顺序到达并执行。

## 2. 复制（Replication）

UE 的属性复制是一种以服务器为权威的状态同步机制，是不可靠的！

它并不保证每次属性变化都到达客户端，而是通过后续复制更新使客户端状态逐渐与服务器一致。

属性同步本身没有像 RPC 那样可供开发者选择的 `Reliable / Unreliable` 标记。但是UE保证最终能够同步最新状态而非丢失属性状态。

复制需要注册，并且可以设置对应的回调函数。

**注册复制属性：**

```cpp
void AMyCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AMyCharacter, Health);
}
```

**声明复制属性及其回调：**

```cpp
UPROPERTY(ReplicatedUsing = OnRep_Health)
int32 Health;

UFUNCTION()
void OnRep_Health();
```

## 1. RPC 与复制的通道关系

同一 Actor 的 RPC 和属性复制通常通过同一个 `ActorChannel` 传输，但二者没有统一的执行顺序保证。

---

# 二、UE 网络架构：从游戏逻辑到网络传输

现在开始建立全面的认知。

在游戏内的逻辑变量会经过 UE 的处理，变成可以传输的网络状态，大致内容和过程如下。

## 1. 网络复制层（Replication）

这一层负责把游戏对象转换为可以发送的网络状态。

传统复制系统中最重要的几个类：

| 类 | 作用 |
|---|---|
| `FObjectReplicator` | 负责对象的属性复制及相关 RPC 序列化 |
| `FRepLayout` | 描述和处理可复制属性的数据布局 |
| `FNetGUIDCache` | 维护网络对象引用的 NetGUID 映射 |
| `UActorChannel` | 协调特定连接上 Actor 及其子对象的复制 |

比如服务器有 1000 个 Actor，某个客户端只需要看到附近的 100 个。

系统需要处理：

1. 哪些 Actor 需要参与复制？
2. 哪些客户端能看到这些 Actor？
3. 哪些属性发生了变化？
4. 哪些数据应该优先发送？
5. 如何把这些数据序列化？

UE5 传统复制系统通常只发送需要同步的属性变化，而不是每一帧把整个 Actor 内存全部发送。

官方文档也明确指出，传统复制过程主要由 `UNetDriver::ServerReplicateActors` 驱动，具体 Actor 复制由 `UActorChannel::ReplicateActor` 处理。

**这一层解决的问题是：哪些对象的哪些数据要发给谁？**

## 2. 网络驱动管理层（NetDriver）

然后进入最核心的部分：**网络驱动管理层（NetDriver）**。

`UNetDriver` 是 UE 网络系统的核心管理类之一。它负责网络驱动的整体生命周期和连接管理，并协调复制与网络收发流程。

`UIpNetDriver` 是面向 IP 网络的实现，承担底层 IP Socket 的初始化、接收分发等职责。

在一个典型 Dedicated Server 中，结构可以理解为：

```text
UIpNetDriver（服务器）
├── Client A
│   └── UNetConnection
├── Client B
│   └── UNetConnection
└── Client C
    └── UNetConnection
```

服务器的 NetDriver 负责管理这些客户端连接。

**这一层解决的问题是：整个网络实例如何运行和调度？**

## 3. 连接管理层（NetConnection）

**核心类：`UNetConnection`。**

`UNetConnection` 表示 UE 网络系统中的一条逻辑连接。UDP 本身没有连接，但 UE 在 UDP 之上维护了自己的连接状态。

它主要负责：

- 维护客户端和服务器之间的逻辑连接。
- 管理该连接上的多个 `UChannel`。
- 处理数据包的序列号和确认信息。
- 维护发送缓冲区、带宽限制、超时与丢包统计。
- 将多个 Bunch 组织进网络 Packet。

也就是一个 NetConnection 对应某一个客户端的连接，这个连接下面有很多不同的 Channel。

## 4. 逻辑通道层（Channel）

**核心类：`UChannel`。**

```text
UChannel
├── UControlChannel
├── UActorChannel
└── UVoiceChannel
```

不同 Channel 的数据可以复用同一条底层 UDP 通信路径。

**这一层解决的问题是：同一个连接里的不同逻辑数据如何独立管理？**

## 5. 数据序列化与 Bunch 层

UE 网络通信中非常重要的概念：**Bunch**。

可以把 Bunch 理解为 Channel 使用的逻辑数据传输单元。

一个概念性的 Bunch 可能包含：

| 内容 | 含义 |
|---|---|
| Channel 信息 | 属于哪个通信通道 |
| 可靠性与控制标志 | 是否可靠、是否开启/关闭通道等 |
| 序列与分片信息 | 按实际情况编码 |
| Payload | 序列化后的 Actor 属性或 RPC 数据 |

一个 Bunch 可能被拆分成多个部分，多个 Bunch 也可能被合并到一个 Packet 中发送。

**Bunch 不等于 UDP Packet。**

## 6. Packet 与可靠传输层

这是“UDP 可靠性封装”主要所在的位置。

**主要职责：**

- Packet 序列号。
- ACK 确认和丢包判定。
- 可靠 Bunch 的缓存与重传。
- 通道内可靠消息的排序。
- Packet 组装与发送。
- 带宽控制。

Packet 丢失后，UE 会根据确认与丢失信息，推动其关联的可靠 Bunch 重传。

但这并不意味着 UE 会原样重发整个 UDP Packet。

**这一层解决的问题是：如何在不可靠的 UDP 网络之上提供选择性的可靠通信？**

---

# 三、UE 如何在 UDP 上封装可靠性机制

基本介绍已经结束，下面进入详细讲解：UE 是怎么在 UDP 上面做了一层可靠性的封装的？

## 1. 先建立总体认知：UNetConnection

**UNetConnection：一条连接的总管理者。**

每个连接维护自己的状态，包括：

- 已创建的 Channel。
- 发送和接收的 Packet 序列号。
- ACK 确认历史。
- 网络数据发送缓冲区。
- 带宽、延迟和丢包统计。
- 连接状态及超时控制。

## 2. Packet、Bunch 与 Channel 的关系

一个 Packet，可能由多个 Bunch 组成；一个 Bunch，也可能被拆分为多个 Packet。

Connection 会维护收到和发送的 Packet，即交流单位是 Packet。

当客户端 A 的某个 Actor 发起 Server RPC 时，UE 首先通过网络复制系统对 RPC 的函数标识和参数进行序列化，并通过该 Actor 在服务器连接上对应的 ActorChannel，将数据封装为 Bunch。

每个 Bunch 都携带所属 Channel 的标识信息。随后，ActorChannel 将 Bunch 交给 UNetConnection，由 Connection 将一个或多个 Bunch 组织到 Packet 中，再通过底层 UDP Socket 发送给服务器。

服务器收到 UDP 数据包后，由对应的 UNetConnection 解析 Packet，从中提取各个 Bunch，再根据 Bunch 携带的 Channel 标识，将其分发到对应的 ActorChannel。最后，ActorChannel 通过相关的反序列化和 RPC 处理逻辑，在服务器端对应的 Actor 上执行 RPC。

## 1. UChannel 的两个关键队列

| 成员 | 含义 |
|---|---|
| `OutRec` | 尚未完成确认的可靠 Bunch 链表 |
| `InRec` | 接收端缓存的乱序可靠 Bunch 链表 |

**关于 OutRec：**

如果 Bunch #12 丢失，Channel 就可以根据自身的发送记录找到它，而不需要重新构造整个 Actor 的所有状态。

## 4. 发送端与接收端的 Packet 通信过程

Packet 是 Connection 级别的，这里分为发送端和接收端，通过一个例子来辅助理解。

**注意最重要的：**

```cpp
// UChannel 中的重要成员
FOutBunch* OutRec;
FInBunch*  InRec;
```

### 发送端

bunch应该分为两种，一种为可靠bunch，一种为不可靠bunch，可靠bunch在发送后会存储到OutRec里面，OutRec通常存储的是已经发送但是尚未确认的可靠bunch，

不可靠bunch发送后不再管理。

**packet分为两部分内容：**

1. 完整的bunch消息副本
2. 关联记录，记录packet里面的bunch来自哪个channel，方便出错时查找重发，只记录可靠bunch

### 发送流程如下

发送端组装bunch，可靠bunch和不可靠bunch都会被组装到packet里面，并且可靠bunch被保存到OutRec里面，然后发送packet，假如packet1没有丢失，

接收端接收到packet1以后，会发送一个类似ack的确认信号进行回复，然后拆出packet里面的bunch来自哪个channel，并且分发到接收端对应的channel里面的InRec，InRec同样只保存可靠bunch，并且记录了序号，会按照序号有序交付bunch，

假如packet2丢失了，packet3正常接收了，此时接收端会把packet3里面的bunch放到对应的channel的InRec里面，进行维护，如果顺序正确，则按序交付，如果不正确，则继续等待，这样体现了一个机制，也许一个channel在等待的同时，另一个channel正在正确交付，此时接收端可能发送NAk信号类似的信息，告诉发送端packet2丢了，发送端接收到NAK之后，会根据NAK的packet序号，查找对应的packet，注意：packet在发送之后的实体信息副本不会保留，但是关联记录会保留，发送端会根据关联记录，找到对应的channel里的bunch，然后把OutRec里面的可靠bunch重新发送，而那些ACK的packet，则会标记OutRec里面的bunch为已确认，这个机制体现了channel才是bunch的真正持有者，并且保证了可靠bunch会重发，但是不可靠bunch不会重发