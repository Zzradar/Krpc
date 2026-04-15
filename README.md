# Krpc

> **本项目目前只在[知识星球](https://programmercarl.com/other/kstar.html)答疑并维护**。

如果你已有 C++ 语法基础，且做过[知识星球](https://programmercarl.com/other/kstar.html)里的 [基于 Raft 共识算法的 KV 数据库](https://programmercarl.com/other/project_fenbushi.html)、[协程库](https://programmercarl.com/other/project_coroutine.html)，上手会非常快：每天抽 3～4 小时，大约 3 天可以把项目过一遍。若基础较弱、需要补理论，每天 6～8 小时大约两周可以完成。

## Krpc 是什么（给第一次接触的人）

Krpc 是一个用 **C++** 实现的 **RPC（远程过程调用）** 学习与练手项目：你的业务代码在**客户端**像调本地函数一样发起调用，框架把参数打包，通过 **TCP** 发到**服务端**，服务端执行后再把结果发回来。

整体上可以把它理解成四件事：

1. **自定义二进制协议**：解决 TCP 流式传输里的「粘包、半包」问题，让每一帧请求/响应都能被正确切开和配对。  
2. **两种序列化方式**：常用的是 Protobuf；也可切换为 Msgpack，便于对照学习。客户端与服务端必须使用同一种。  
3. **可选的服务发现**：需要多实例、动态扩缩时，可对接 **ZooKeeper** 做注册与发现；简单场景也可以用静态地址列表，不依赖注册中心。  
4. **工程化能力**：例如调用超时、长连接心跳、服务端用线程池跑业务避免阻塞网络线程、连接池与简单负载均衡、基础监控指标等——这些在真实系统里也很常见。

你**不需要**先记住仓库里的类名或文件名；下面「功能概览」从**角色**（客户端 / 服务端）说明能力，细节见 [docs/feature_summary.md](docs/feature_summary.md) 与 [docs/项目说明书_架构与模块.md](docs/项目说明书_架构与模块.md)。

## 功能概览（按角色）

| 能力 | 主要在哪一侧 | 说明 |
|------|----------------|------|
| 协议帧、请求/响应配对 | 双方 | 同一套帧格式，保证在 TCP 上可靠解析。 |
| 调用超时 | 客户端 | 限制单次调用最长等待时间，避免卡死。 |
| 心跳与空闲断开 | 客户端发、服务端答；服务端可踢长期无流量连接 | 保活长连接，回收僵尸连接。 |
| 同步 / 异步调用 | 客户端 | 同步阻塞等待；异步用 future 或回调，不阻塞业务线程。 |
| 业务执行与网络分离 | 服务端 | 网络线程只负责收发包，业务在线程池里跑。 |
| 连接池与多节点 | 客户端为主 | 复用 TCP；可选从注册中心或静态列表选多个地址并轮询。 |
| 监控指标 | 服务端（可选） | 以 HTTP 暴露 Prometheus 文本格式，便于本地观察。 |

## 做完本项目你将收获

* 理解 RPC 在分布式系统里解决什么问题、典型分层长什么样  
* 巩固 C++、STL、常见设计模式  
* 熟悉 Socket、TCP 与高并发 I/O（如 epoll），以及基于 Muduo 的 Reactor 用法  
* 用 Protobuf（及可选 Msgpack）做高效序列化  
* 自己设计协议并处理粘包/拆包  
* 了解用 ZooKeeper 做服务注册与发现、以及简单的多节点与负载均衡思路  
* 从零搭一个可运行的 RPC 框架原型，并有能力按文档继续扩展  

## 为什么要做 C++ 版的 RPC？

1. **性能**：很多低延迟、高吞吐场景（金融、游戏、实时通信）仍依赖 C++ 的控制力与开销。  
2. **基础设施**：数据库、中间件、存储系统大量用 C++，需要与语言贴合的 RPC 形态。  
3. **可移植**：Linux、Windows、嵌入式等环境都能落地。  
4. **可扩展**：序列化、传输方式、线程模型等可按项目需要替换或加深。

常见使用场景包括：微服务间调用、实时业务、分布式存储与共识（如 Raft 节点间通信）、资源受限的嵌入式互联等。

## 项目专栏

专栏里会讲**简历怎么写、性能怎么测、可以怎么优化、面试常问什么**，并配套技术栈、环境、RPC 概念、日志与代码导读。  
（以下为星球内部宣传与截图，获取方式见文末「获取本项目专栏」。）

### 简历写法

专栏里直接给出简历写法，**项目难点**和**个人收获**是面试官最关心的部分。

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103223303.png' width=500 alt=''></img></div>

在 [知识星球](https://programmercarl.com/other/kstar.html) RPC 项目专栏会给出参考简历写法；公众号上为防重复率过高做了打码。

### 性能测试

带大家测 RPC 性能，更直观了解系统表现。

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224011.png' width=500 alt=''></img></div>

### 项目优化

文档中会列出多个可扩展方向（通信、注册发现、负载均衡、零拷贝、日志与监控、健康检测与熔断、重试与超时等），便于你做出差异化。

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224306.png' width=500 alt=''></img></div>

### 代码讲解

整体流程与逐函数说明、日志库导读等均在专栏中。

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224628.png' width=500 alt=''></img></div>

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224733.png' width=500 alt=''></img></div>

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103225024.png' width=500 alt=''></img></div>

### RPC 理论

梳理 RPC 的来龙去脉。

<div align="center"><img src='https://file1.kamacoder.com/i/algo/20250103224858.png' width=500 alt=''></img></div>

### 突击使用

若面试在即，可按专栏简历模板整理经历，并准备专栏中的面试问题。

## 获取本项目专栏

本文档仅为星球内部专享，可加入 [知识星球](https://programmercarl.com/other/kstar.html) 获取。

---

## 文档与运行说明（仓库内）

更细的功能说明、**客户端/服务端分工**与**可复现命令**见：[docs/feature_summary.md](docs/feature_summary.md)。

### 心跳与空闲连接（验证）

配置在 `bin/test.conf`，常见项：

- `heartbeat_interval_ms`：心跳周期（默认 5000 ms）。  
- `heartbeat_miss_limit`：允许连续丢失心跳次数（默认 3）。  
- `rpc_timeout_ms`：默认调用超时；心跳等待也沿用该超时。  
- `rpc_codec`：`protobuf` 或 `msgpack`，**两端需一致**。

**验证步骤**：

1. 编译：`cmake --build build`  
2. 终端 A 启动服务端：`bin/server -i bin/test.conf`  
3. 终端 B 长心跳演示：`HEARTBEAT_IDLE_SECONDS=30 HEARTBEAT_IDLE_ROUNDS=2 bin/heartbeat_client -i bin/test.conf`  
4. 观察 A：约 `heartbeat_interval_ms × (heartbeat_miss_limit + 1)` 后若连接长期无活动，会出现空闲关闭相关日志。  
5. 终端 C 超时演示：`bin/timeout_client -i bin/test.conf`  

可分别验证：空闲时客户端仍保活、服务端会踢空闲连接、超时后客户端可继续重连访问。

### 连接池

- `enable_connection_pool`：是否启用（默认 1）。  
- `connection_pool_max_idle`：单地址最大空闲连接数（默认 4）。  

先启动 server，再运行 `pool_demo`；开池时首条建连后多轮应出现复用日志，关池则每次新建连接。详见 `docs/feature_summary.md`。

### 异步模式示例

编译：`cmake --build build --target async_client`  

- 默认 future 风格：`ASYNC_CONCURRENCY=4 ASYNC_REQUESTS=20 ./bin/async_client -i ./bin/test.conf`  
- 回调风格：`ASYNC_MODE=callback ASYNC_CONCURRENCY=4 ASYNC_REQUESTS=20 ./bin/async_client -i ./bin/test.conf`  

可调：`ASYNC_CONCURRENCY`、`ASYNC_REQUESTS`、`ASYNC_TIMEOUT_MS`、`ASYNC_SLEEP_MS`。使用 msgpack 时同样可配置单次超时。

### 序列化切换（protobuf / msgpack）

两端配置一致即可；默认 protobuf。切换为 msgpack 时，所有示例需同样修改配置后再起服务与客户端。

### 日志（glog）

- `log_minlevel`：最小级别（0=INFO … 3=FATAL），默认 1。  
- `logtostderr` / `colorlogtostderr`：是否输出到 stderr、是否彩色。

### 监控（Prometheus 文本）

- `metrics_http_enabled=1`、`metrics_http_port=9090` 等，浏览器访问 `http://127.0.0.1:9090/metrics`。  
- 含全局与按方法维度的指标（具体以运行输出为准）。

示例：复制 `bin/test.conf` 为 `bin/test_switch.conf`，追加 `rpc_codec=msgpack` 后，服务端与客户端均用该配置启动。

### 负载均衡（多实例）

- **服务端**：同一服务方法可在注册中心下注册多个实例（多节点）。  
- **客户端**：从注册中心或静态环境变量读取多个地址，按轮询等方式选节点；失败节点可短暂冷却并尝试其他节点。  
- 与连接池一起：先选节点，再对该节点复用或新建 TCP。  

细节与验证命令见 [docs/feature_summary.md](docs/feature_summary.md)。
