# Krpc

> **本项目目前只在[知识星球](https://programmercarl.com/other/kstar.html)答疑并维护**。

本项目如果是有C++语法基础的录友，且做过[知识星球](https://programmercarl.com/other/kstar.html)的：[基于Raft共识算法的KV数据库](https://programmercarl.com/other/project_fenbushi.html)，[协程库](https://programmercarl.com/other/project_coroutine.html)，那大家上手这个项目的时间会非常快：

学习时间：一天只需要抽3-4个小时，看3天左右基本能看完整个项目。

如果你还是新手，很多理论知识还要从头学习，如果一天学6-8小时，大概需要两周基本能完成这个RPC项目。

## 做完本项目你讲收获

* 深入理解RPC框架原理与分布式系统设计
* 夯实C++面向对象、STL、设计模式核心功底
* 掌握Socket、TCP/UDP及高并发I/O模型（epoll）
* 基于Muduo库实现Reactor网络模型，解耦业务与通信
* 熟练使用Protobuf定义消息、实现高效序列化/反序列化
* 设计自定义协议，解决TCP粘包/拆包问题
* 集成Zookeeper作为注册中心，实现服务注册与发现
* 运用Watcher机制动态感知服务状态，保障高可用
* 从0到1打造高性能RPC框架，获得分布式系统开发经验
* 提升解决复杂工程问题（协议设计、高并发、解耦）的能力

## 为什么要做c++版的rpc？

1.高性能需求

c++以其高效的内存管理和底层控制能力，成为性能要求比较高的系统(如金融、游戏服务器、实时通信系统)的首选语言。

在这些场景下，RPC框架需要尽可能减少通信开销，而C++天生的性能优势可以满足这一需要求。


2.系统级开发

很多底层基础设施(如数据库、中间件、分布式存储系统)都是用c++开发。

这些系统需要一个与语言无缝结合的高效RPC框架，避免因语言间的切换导致性能损耗


3.跨平台

C++的可移植性在不同平台(如linux、Windows、嵌入式系统)上广泛使用。

一个C++RPC框架能够为这些多平台环境提供统一的通信接口，降低开发成本。

4.灵活性与可扩展性

与某些语言的封闭生态不同，C++允许开发者灵活地调整底层实现。例如：可以定制序列化协议(如Protobuf、Thrift)、网络传输方式(如TCP、UDP、QUIC)等，以满足不同场景的需求。

关于C++版RPC框架的使用场景：

* 微服务架构：在微服务架构中，服务通常分布在不同的网络和不同的服务器上，此时就需要一个高效的通信手段就是我们的rpc。
* 实时通信：如在线游戏、视频直播、即时通信等场景，要求低延迟和高吞吐。C++RPC可以通过优化网络传输协议和序列化协议，提供实时性保障。
* 分布式存储与计算：像hadoop、或者你做个raft的共识算法的话，也可也发现我们在不同节点之间使用rpc传递数据进行通信。
* 嵌入式系统： 在嵌入式设备之间的通信中，资源有限且性能要求严格。C++的轻量级特性使其成为嵌入式RPC实现的理想选择。
* 跨语言调用： C++ RPC框架通常支持多语言绑定（如Python、Java），可以用作跨语言调用的桥梁。例如，在后端服务使用C++开发的情况下，前端服务可以通过RPC框架调用其功能。

## 项目专栏

在项目专栏中， 该**项目简历如何写、性能如何测试、项目怎么优化、面试都会问哪些问题**，都安排好了。

不仅如此，还有 「技术栈需求」「运行环境」「RPC理论」「日志库」「代码解读」

### 简历写法

专栏里直接给出简历写法， 项目难点 和 个人收获是面试官最关心的部分。

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103223303.png' width=500 alt=''></img></div>

在[知识星球](https://programmercarl.com/other/kstar.html)RPC项目专栏 会给出本项目的参考简历写法，为了不让 这些写法重复率太高，所以公众号上是打码的。

### 性能测试

带大家测试RPC的性能，更充分了解 系统的表现。

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224011.png' width=500 alt=''></img></div>

### 项目优化

项目文档列出的十几个优化点：

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224306.png' width=500 alt=''></img></div>

涉及到 「通信模块」 「服务注册与发现模块」「负载均衡模块」「零拷贝优化技术」「日志与监控模块」「健康检测与熔断机制」「重试与超时处理」

从各个方面，带大家去了解项目如何进一步优化，帮助大家找到可以拓展的方向，打造自己的项目竞争力，也避免了项目重复。

### 代码讲解

给出项目整体流程图：

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224628.png' width=500 alt=''></img></div>

其中项目的所有代码以及每个函数和类都有详细解释，根本不用担心自己看不懂：

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224733.png' width=500 alt=''></img></div>

同时我们对项目中需要用到的日志库做了详细的分析：

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103225024.png' width=500 alt=''></img></div>

### RPC理论

项目文档帮大家梳理清楚 RPC 的来龙去脉 ：

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224858.png' width=500 alt=''></img></div>

### 突击来用

如果大家面试在即，实在没时间做项目了，可以直接按照专栏给出的简历写法，写到简历上，然后把项目专栏里的面试问题，都认真背一背就好了，基本覆盖 绝大多数 RPC项目问题。


## 获取本项目专栏

本文档仅为星球内部专享，大家可以加入[知识星球](https://programmercarl.com/other/kstar.html)里获取。

## 心跳与空闲连接测试指引

当前源码已经内置**心跳保活**和**服务端空闲踢除**机制，相关配置项位于 `bin/test.conf`：

- `heartbeat_interval_ms`：客户端心跳发送周期（默认 5000ms）。
- `heartbeat_miss_limit`：允许连续丢失的心跳次数（默认 3 次）。
- `rpc_timeout_ms`：同步 RPC 的默认超时时间，心跳往返也沿用该超时。

### 验证步骤

1. **编译**
	```bash
	cmake --build build
	```
2. **启动服务端**（终端 A）
	```bash
	bin/server -i bin/test.conf
	```
3. **长时间心跳演示**（终端 B）
	```bash
	HEARTBEAT_IDLE_SECONDS=30 HEARTBEAT_IDLE_ROUNDS=2 bin/heartbeat_client -i bin/test.conf
	```
	- 每轮执行一次 `Login`，然后空闲 30s。期间客户端日志应持续成功，表明心跳在空闲时仍保持连接。
4. **空闲踢除观察**
	- 继续观察终端 A，约 `heartbeat_interval_ms × (heartbeat_miss_limit + 1)` 时间后，会看到 `closing idle connection` 与 Muduo 的 `removeConnectionInLoop` 日志，表示服务端检测到连接长时间未活动并主动关闭。
5. **超时/重连示例**（终端 C）
	```bash
	bin/timeout_client -i bin/test.conf
	```
	- 正常请求会成功，`sleep` 用户名的请求因 1s 自定义超时被客户端关闭，对应服务端会打印 `Broken pipe`，验证 RPC 超时路径不会影响心跳。

通过以上流程，可以分别验证：
- 客户端在空闲阶段仍发送 PING/PONG，保持连接（步骤 3）。
- 服务端基于 `connection_states_` 的 `last_activity` 自动剔除长期无活动的连接（步骤 4）。
- 客户端超时后能自动重连继续访问（步骤 5）。

## 连接池与验证

- 配置项（`bin/test.conf`）：
  - `enable_connection_pool`：1 开启、0 关闭（默认 1）。
  - `connection_pool_max_idle`：单端点最大空闲连接数（默认 4）。
- 示例验证（需要先启动 server）：  
  - 开池复用：`enable_connection_pool=1`  
    ```bash
    POOL_DEMO_MODE=new_channel ./bin/pool_demo -i ./bin/test.conf > /tmp/pool_demo.log 2>&1
    grep -E "connect server success|reuse pooled connection" /tmp/pool_demo.log
    ```  
    预期：首条握手，后续多数为 `reuse pooled connection`。
  - 关池对比：`enable_connection_pool=0`，同样命令，预期每次都是 `connect server success`。

## 异步模式示例

- **编译**：`cmake --build build --target async_client`
- **future 模式（默认）**：
	```bash
	ASYNC_CONCURRENCY=4 ASYNC_REQUESTS=20 ./bin/async_client -i ./bin/test.conf
	```
- **callback 模式**：
	```bash
	ASYNC_MODE=callback ASYNC_CONCURRENCY=4 ASYNC_REQUESTS=20 ./bin/async_client -i ./bin/test.conf
	```
- **可调参数**（环境变量）：`ASYNC_CONCURRENCY` 并发线程，`ASYNC_REQUESTS` 请求总数，`ASYNC_TIMEOUT_MS` 单次超时，`ASYNC_SLEEP_MS` 请求间隔毫秒。
- **输出**：展示成功/失败数、p50/p95/p99 延迟与总耗时，便于对比同步 vs 异步、future vs callback。
