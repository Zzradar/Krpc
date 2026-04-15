# Krpc 功能说明书（实现侧）

本文档面向**已打开仓库、需要对照代码或配置**的读者，按功能说明**谁负责（客户端 / 服务端 / 双方）**、**做什么**、**如何验证**。与 [README.md](../README.md) 的「整体介绍」互补；更细的架构见 [项目说明书_架构与模块.md](项目说明书_架构与模块.md)。

---

## 阅读约定

| 用语 | 含义 |
|------|------|
| **客户端** | 发起 RPC 的进程：建立连接、序列化请求、等待或异步接收响应。 |
| **服务端** | 监听端口、收包、解析协议、执行业务并返回结果的进程。 |
| **双方** | 协议或配置上必须一致，不单独属于某一侧。 |

---

## 1. 协议帧头（双方约定）

| 项目 | 说明 |
|------|------|
| **归属** | **双方**：编解码规则一致；具体序列化在客户端打包、服务端解析。 |
| **做什么** | 在 TCP 字节流上定义固定含义的帧头，避免把多帧粘在一起读错。 |
| **内容** | 魔数、版本、`msg_type`（请求/响应/心跳等）、`request_id`、`body` 长度、可选压缩类型、服务名与方法名等。 |
| **效果** | 可校验连接是否为本协议；请求与响应可按 `request_id` 配对；为心跳、后续消息类型扩展留字段。 |
| **相关代码** | `src/Krpcheader.proto` 及生成代码。 |

---

## 2. 调用超时（客户端）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**：在本地限制「等响应」的最长时间。 |
| **做什么** | 每次调用可设置超时；默认取自配置项 `rpc_timeout_ms`。 |
| **行为** | 超时后判定本次调用失败并关闭本次使用的连接相关资源，避免线程永久阻塞。 |
| **相关代码** | `Krpccontroller`（设置超时）、`KrpcChannel` 内等待可读与超时处理。 |

---

## 3. 心跳保活与断线重连（客户端）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**：独立心跳线程 + 业务线程共用连接时需加锁。 |
| **做什么** | 按配置周期发送心跳帧；收不到对端应答则累计失败次数，超过阈值则关闭连接；业务下次调用时重新建连。 |
| **行为** | 长连接在空闲时仍保持存活；网络抖动或服务器重启后，通过重连恢复后续 RPC。 |
| **相关代码** | `KrpcChannel` 构造时启动心跳线程，与 `HeartbeatActivityNotifier`、socket 互斥配合。 |

---

## 4. 心跳应答与空闲踢连接（服务端）

| 项目 | 说明 |
|------|------|
| **归属** | **服务端**：在 IO 线程处理 Ping/Pong；定时任务扫描连接是否长期无活动。 |
| **做什么** | 收到心跳立即回复并刷新「最后活动时间」；超过阈值未活动的连接被主动关闭。 |
| **行为** | 僵尸连接不会一直占文件描述符；与客户端心跳配合形成可预期的连接生命周期。 |
| **相关代码** | `KrpcProvider::OnMessage`（Ping/Pong）、`connection_states_`、定时 `runEvery` 扫描。 |

---

## 5. 客户端异步调用（多阶段）

### 5.1 在途请求管理与接收线程（客户端）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**。 |
| **做什么** | 用 `request_id` 把「已发出、尚未完成」的请求记在表里；独立接收线程解析 TCP 上的帧，把响应分发给对应等待方或心跳处理。 |
| **行为** | 同步、异步、心跳共用一条读路径，避免多线程抢读同一 socket。 |
| **相关代码** | `PendingCall`、`m_pending_calls`、`RecvLoop`，与心跳失败时批量失败在途请求的逻辑。 |

### 5.2 异步 API、发送队列与超时扫描（客户端）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**。 |
| **做什么** | 提供 `CallAsync` 等接口；发送侧先入队再由单独线程顺序写 socket；后台线程按时间扫描在途请求，超时则完成并回调/置失败。 |
| **行为** | 业务线程可快速返回；超时不必依赖业务自己轮询；成功/失败统一走一套完成逻辑，避免重复回调。 |
| **相关代码** | `SendQueue` / `SendLoop`、`TimeoutManager`、`CompletePending`。 |

---

## 6. 服务端业务线程池（服务端）

| 项目 | 说明 |
|------|------|
| **归属** | **服务端**：网络 IO 与业务执行分离。 |
| **做什么** | IO 线程只负责收包、解析 RPC 并入队；worker 线程从队列取任务并调用用户注册的 `Service::CallMethod`。 |
| **行为** | 慢业务不会阻塞 epoll/读事件；队列有容量上限，满时可阻塞生产者或降级策略（见实现与日志）。 |
| **相关代码** | `KrpcProvider::Run` 启动 worker、队列与 `KrpcMsgpackProvider` 对称逻辑。 |
| **注意** | 心跳仍在 IO 路径快速处理，不进入业务队列。 |

---

## 7. 示例与可执行程序（验证用）

| 项目 | 说明 |
|------|------|
| **归属** | 仓库内示例程序，通常需同时起服务端与客户端。 |
| **内容** | 超时演示、心跳演示、压力客户端、`pool_demo`、`bench_demo`、`async_client` 等。 |
| **用途** | 验证超时、心跳、连接池、同步/异步、长/短连接与指标输出。 |

**典型验证流程**：编译后启动 `server` 与对应 `client` / 脚本；具体命令以 README 或各 example 目录说明为准。

---

## 8. 连接池与地址缓存（客户端）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**（按服务端地址 `ip:port` 缓存连接；与发现结果配合）。 |
| **做什么** | 对同一端点复用已建立的 TCP，减少重复握手；归还前做简单健康检查；可配置每端点最大空闲连接数。 |
| **行为** | 首次连接日志与后续「复用」可区分；服务端踢掉空闲连接后，客户端下次会重建连接。 |
| **配置** | `enable_connection_pool`、`connection_pool_max_idle`（见 `bin/test.conf`）。 |

---

## 9. 负载均衡与服务发现（客户端 + 注册中心）

| 项目 | 说明 |
|------|------|
| **归属** | **服务端**向注册中心登记实例；**客户端**拉取列表并选择地址。 |
| **做什么** | 同一服务方法可对应多个 `ip:port`；客户端轮询选择；失败端点可进入短时冷却，并尝试其他节点。 |
| **无 ZK 时** | 可用环境变量提供静态端点列表，行为与「多节点轮询」一致。 |
| **与连接池** | 先选定端点，再对该端点取/还连接；换节点会建新连到目标地址。 |
| **服务端 ZK 会话** | 实例在 ZK 下的子节点为 **EPHEMERAL**，依赖服务端进程内 **保持长连接**；会话一旦关闭，子节点会消失。实现上 `ZkClient` 挂在 `KrpcProvider` / `KrpcMsgpackProvider` 成员上，与 `Run()` 事件循环同生命周期，避免「注册即关」导致 `ls` 为空。 |

---

## 10. 压测与对比 Demo（客户端为主）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**发压；**服务端**承载请求。 |
| **做什么** | 同步/异步、长连接/短连接等组合下统计 QPS、延迟分位与成功率。 |
| **用途** | 对比不同调用方式与连接策略的大致性能差异。 |

---

## 11. 异步示例（验证客户端 API）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**示例（`async_demo`）。 |
| **做什么** | 演示 future 等待与 callback 两种异步用法及并发、超时参数。 |

---

## 12. 发送路径优化：聚合写（客户端）

| 项目 | 说明 |
|------|------|
| **归属** | **客户端**发送路径。 |
| **做什么** | 将多段缓冲区通过一次系统调用写出，减少拼接大缓冲的拷贝。 |
| **行为** | 与发送队列、长连接复用、错误时清空队列等逻辑一致。 |

---

## 13. Msgpack 通道能力对齐（客户端 + 服务端）

| 项目 | 说明 |
|------|------|
| **归属** | **双方**使用 msgpack 编解码时；超时/异步/发送队列主要在**客户端**；指标暴露在**服务端**（若开启）。 |
| **做什么** | 与 Protobuf 通道类似：单次调用超时、异步回调、发送侧队列与聚合写；服务端可暴露 Prometheus 文本指标（含按方法维度）。 |
| **配置** | 序列化协议在配置中与服务端一致（如 `rpc_codec`）。 |

---

## 14. 日志（glog）与排障

| 项目 | 说明 |
|------|------|
| **归属** | **双方进程**均可通过配置文件初始化 **glog**（入口在 `KrpcApplication`，凡带 `-i` 读配置的 `bin/*` 客户端/服务端都会走同一套初始化逻辑）。 |
| **做什么** | 控制最小日志级别、是否打到 stderr、是否彩色；便于本地调试与保留现场。 |

### 14.1 常用配置项（写在 `*.conf`）

| 配置项 | 含义 |
|--------|------|
| `log_minlevel` | 最小级别：`0=INFO`，`1=WARNING`，`2=ERROR`，`3=FATAL`（默认多为 `1`，压测配置里有时会调到 `2` 降噪）。 |
| `logtostderr` | `1` 输出到 stderr，`0` 否（默认多为 `1`）。 |
| `colorlogtostderr` | stderr 是否彩色（默认多为 `1`）。 |

### 14.2 排障时建议看什么

- **客户端**：连接建立/复用、超时、异步发送队列与 `pending` 清理、负载均衡选端点（日志里常有 `lb`、`zk`、`connect` 等关键词，具体以当前版本输出为准）。
- **服务端**：收包解析、业务线程池、心跳 Ping/Pong、空闲踢连接、Metrics HTTP 启动失败等。
- **跨进程对齐**：若在 Protobuf 路径下使用 **`trace_id`**，可在客户端与服务端日志里按同一 ID 过滤；细节见 [深挖_trace_id_日志传播.md](深挖_trace_id_日志传播.md)。

---

## 15. Metrics（Prometheus 文本，服务端）

| 项目 | 说明 |
|------|------|
| **归属** | **服务端**：在 `KrpcProvider` / `KrpcMsgpackProvider` 启动流程中，若开启则拉起简易 HTTP，对外暴露 **Prometheus 文本格式** 指标（与 RPC 监听端口**不是**同一个端口）。 |
| **做什么** | 便于本地或 Prometheus 拉取：观察 QPS、延迟分位、按方法维度聚合等（具体指标名以 `/metrics` 实际输出为准）。 |

### 15.1 配置项

| 配置项 | 含义 |
|--------|------|
| `metrics_http_enabled` | `1` 开启，`0` 关闭（默认依示例配置而定；本地压测用 `bench_local_*.conf` 时常为 `0`）。 |
| `metrics_http_port` | 监听端口，例如 `9100`。访问路径一般为 **`http://<host>:<port>/metrics`**。 |

### 15.2 多实例时注意

- 每个 **server 进程**各自监听自己的 metrics 端口；**两个实例不能共用同一 `metrics_http_port`**，否则第二个会启动失败或冲突。  
- 例如实例 A 用 `9100`，实例 B 在配置里改为 `9101`（同时 `metrics_http_enabled=1`）。

### 15.3 快速验证

```bash
# 服务端已用 test.conf 启动且 metrics_http_enabled=1、metrics_http_port=9100 时：
curl -sS http://127.0.0.1:9100/metrics | head
```

README 中也有与 `rpc_codec` 切换配合的示例命令，可与本节对照。

---

## 16. 配置项速查（`bin/test.conf` 等）

| 配置项 | 含义 |
|--------|------|
| `rpcserverip` / `rpcserverport` | 服务端地址（直连或配合发现使用）。 |
| `zookeeperip` / `zookeeperport` | 注册中心（可选）。 |
| `heartbeat_interval_ms` | 心跳周期。 |
| `heartbeat_miss_limit` | 允许连续心跳失败次数。 |
| `rpc_timeout_ms` | 默认 RPC 超时。 |
| `lb_fail_cooldown_ms` | 负载均衡失败端点冷却时间。 |
| `log_minlevel` / `logtostderr` / `colorlogtostderr` | 日志级别与输出（详见 **§14**）。 |
| `metrics_http_enabled` / `metrics_http_port` | 服务端 Prometheus 文本导出（详见 **§15**）。 |

更全列表以仓库内示例配置为准。

---

## 17. 后续可扩展点（备忘）

- 心跳失败后的故障转移（重连后状态恢复）。
- 服务端指标：心跳 RTT、idle 关闭次数等。
- 扩展 `msg_type`：单向、推送等。
- 文档与压测数据持续更新。

---

## 文档与代码索引（便于检索）

| 主题 | 主要路径 |
|------|----------|
| 协议头 | `src/Krpcheader.proto` |
| 客户端通道 / 超时 / 异步 / 连接池 | `src/Krpcchannel.cc`、`src/include/Krpcchannel.h` |
| 控制器 | `src/Krpccontroller.cc` |
| 服务端 Protobuf | `KrpcProvider` 相关 |
| 服务端 Msgpack | `KrpcMsgpackProvider` 相关 |
| 日志初始化（glog） | `src/Krpcapplication.cc`、`src/include/KrpcLogger.h` |
| Metrics HTTP | `src/include/metrics_http_server.h`、`KrpcProvider` / `KrpcMsgpackProvider` 启动处、`metrics_export.h` |
| trace_id 深挖 | [深挖_trace_id_日志传播.md](深挖_trace_id_日志传播.md) |
| 故障演练 | [故障演练_稳定性验证.md](故障演练_稳定性验证.md) |
| 超时演示 | `example/timeout_demo/` |
| 心跳演示 | `example/heartbeat_demo/` |
| 连接池演示 | `example/pool_demo/` |
| 压测 | `example/bench_demo/` |
| 异步示例 | `example/async_demo/` |

---

## 附录：常用验证命令（复制即用）

以下均假设已 `cmake` 编译，且**先启动服务端**再跑客户端类程序。工作目录以仓库根为准。

### 连接池（`pool_demo`）

```bash
# 开池：预期首条 connect，后续多 reuse
# 在 test.conf 中 enable_connection_pool=1
POOL_DEMO_MODE=new_channel ./bin/pool_demo -i ./bin/test.conf > /tmp/pool_demo.log 2>&1
grep -E "connect server success|reuse pooled connection" /tmp/pool_demo.log

# 关池对比：enable_connection_pool=0，同上命令，预期每次 connect

# 闲置后验证出池重建（按需调大空闲时间）
POOL_DEMO_MODE=new_channel POOL_DEMO_IDLE_MS=30000 POOL_DEMO_IDLE_AT=1 ./bin/pool_demo -i ./bin/test.conf
```

### 负载均衡静态端点（无需 ZK）

```bash
LB_STATIC_ENDPOINTS=127.0.0.1:8000,127.0.0.1:8001,127.0.0.1:8002 \
  POOL_DEMO_MODE=new_channel ./bin/pool_demo -i ./bin/test.conf > /tmp/pool_demo.log 2>&1
# 日志中应能看到轮询与连接复用（需对应端口有服务）
```

### ZK 发现 + 多实例（`pool_demo` 日志，无需安装 ripgrep）

先起多个 `bin/server`（不同端口、未设置 `skip_zookeeper_registration`），客户端**不要**设置 `LB_STATIC_ENDPOINTS`。跑 demo 后从日志里筛关键词请用系统自带的 **`grep -E`**（未安装 `rg` 时也可用）。

```bash
unset LB_STATIC_ENDPOINTS
```

```bash
POOL_DEMO_MODE=new_channel POOL_DEMO_ITERATIONS=40 POOL_DEMO_SLEEP_MS=200 \
  ./bin/pool_demo -i ./bin/test.conf > /tmp/pool_zk.log 2>&1
```

```bash
grep -E "zk children|lb selected endpoint|connect server success|reuse pooled connection|fail|error" /tmp/pool_zk.log
```

### 压测 `bench_demo`

```bash
BENCH_MODE=sync BENCH_CONN=keepalive BENCH_CONCURRENCY=4 BENCH_REQUESTS=200 ./bin/bench_demo -i ./bin/test.conf
BENCH_MODE=async BENCH_CONN=keepalive BENCH_CONCURRENCY=8 BENCH_REQUESTS=500 ./bin/bench_demo -i ./bin/test.conf
BENCH_MODE=sync BENCH_CONN=short BENCH_CONCURRENCY=4 BENCH_REQUESTS=200 ./bin/bench_demo -i ./bin/test.conf
# 可选 BENCH_SLEEP_MS 控制间隔
```

### 同步 + 长连接 + 不同 payload（观察聚合写与吞吐）

```bash
mkdir -p bench_logs
for KB in 1 4 16 64 256 1024; do
  LOG=bench_logs/sync_${KB}k_zero.log
  BENCH_MODE=sync BENCH_CONN=keepalive BENCH_CONCURRENCY=4 BENCH_REQUESTS=200 BENCH_PAYLOAD_KB=$KB \
    ./bin/bench_demo -i ./bin/test.conf >"$LOG" 2>&1
  grep "=== bench summary ===" -A5 "$LOG"
  echo ""
done
```

### 异步示例

```bash
cmake --build build --target async_client
./bin/async_client -i ./bin/test.conf
ASYNC_MODE=callback ASYNC_CONCURRENCY=4 ASYNC_REQUESTS=20 ./bin/async_client -i ./bin/test.conf
```

### 全量冒烟（与原文档一致）

```bash
cmake --build build && ./bin/server -i ./bin/test.conf
# 另开终端：
./bin/client -i ./bin/test.conf
./bin/timeout_client -i ./bin/test.conf
./bin/heartbeat_client -i ./bin/test.conf
```
