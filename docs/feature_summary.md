# 阶段性功能记录

> 记录当前已完成的框架增强项，便于后续继续扩展。

## 1. 协议头扩展
- **改动文件**：`src/Krpcheader.proto` 及生成的 `.pb.cc/.pb.h`。
- **新增字段**：`magic`、`version`、`msg_type`、`request_id`、`body_size`、`compress_type`、`service_name`、`method_name`。
- **作用**：
  - 统一帧格式，校验魔数/版本，防止粘包解析错误。
  - `msg_type` 区分 Request/Response/Ping/Pong，为心跳与后续扩展预留空间。
  - `request_id` 让客户端能够匹配响应，与超时/重试逻辑绑定。

## 2. 客户端调用与超时控制
- **核心文件**：`src/Krpcchannel.cc`、`src/include/Krpcchannel.h`、`src/Krpccontroller.cc`。
- **功能**：
  - 引入 `Krpccontroller::SetTimeoutMs`，RPC 调用可自定义超时时间，默认值来自配置 `rpc_timeout_ms`。
  - 使用 `poll()` 等待可读事件，超时后返回错误并关闭 socket，避免永久阻塞。
  - 保留 `Krpccontroller::Reset` 以便并发压测场景循环复用控制器。

## 3. 客户端心跳与自动重连
- **线程模型**：在 `KrpcChannel` 构造时启动心跳线程，利用 `condition_variable` 结合 `HeartbeatActivityNotifier` 唤醒。
- **心跳流程**：
  1. 每 `heartbeat_interval_ms` 发送 `MSG_TYPE_PING` 帧，沿用请求序列化 + `send()`。
  2. `poll()` 等待 `MSG_TYPE_PONG`，失败会累计 `m_missed_heartbeat_count`。
  3. 连续超时超过 `heartbeat_miss_limit` 时，触发 `HandleHeartbeatFailure` 关闭 fd，业务线程下一次调用时自动重新连接。
- **线程安全**：所有 socket 操作通过 `m_socket_mutex` 保护，避免心跳线程和业务线程抢占导致 `EBADF`。

## 4. 服务端心跳与空闲踢除
- **消息层**：`KrpcProvider::OnMessage` 支持 `MSG_TYPE_PING/PONG`，立刻回复 PONG 并刷新连接活跃时间。
- **连接状态**：`connection_states_` 记录 `last_activity` + `std::weak_ptr<TcpConnection>`。
- **空闲扫描**：
  - `Run()` 启动定时器 `runEvery(interval)`，周期取 `heartbeat_interval_ms`。
  - 每轮计算 `idle_ms = timeDifference(now, last_activity)`；超过 `interval * (miss_limit + 1)` 的连接执行 `forceClose()`。
  - 日志中会输出 `closing idle connection` 以及 Muduo 的 `removeConnectionInLoop`。

## 5. 示例 & 测试
- **timeout_demo**（`example/timeout_demo/TimeoutClient.cc`）：展示正常调用 + 自定义 1s 超时的慢调用。
- **压力客户端**（`example/caller/Kclient.cc`）：支持多线程循环请求，适配新的 `controller.Reset()`。
- **heartbeat_demo**（`example/heartbeat_demo/HeartbeatClient.cc`）：通过环境变量配置空闲时间/轮数，用于观察心跳保活与服务器踢除行为。
- **README 更新**：新增“心跳与空闲连接测试指引”章节，描述配置含义与完整验证步骤。
- **运行验证**：`cmake --build build && ./bin/server -i ./bin/test.conf` 后，在单独终端执行 `./bin/client -i ./bin/test.conf`、`./bin/timeout_client -i ./bin/test.conf`、`./bin/heartbeat_client -i ./bin/test.conf`，覆盖正常压测、超时、心跳稳定性场景。

## 6. 配置项（`bin/test.conf`）
- `rpcserverip` / `rpcserverport`：服务地址。
- `zookeeperip` / `zookeeperport`：注册中心。
- `heartbeat_interval_ms`：心跳周期（默认 5000）。
- `heartbeat_miss_limit`：允许的连续心跳丢失次数（默认 3）。
- `rpc_timeout_ms`：RPC 默认超时（默认 3000）。

## 7. 后续可扩展点
- 引入真正的心跳 failover（重连后自动重新注册订阅）。
- 在服务端补充监控指标（心跳 RTT、idle close 次数）。
- 扩展 `msg_type`：如 ONEWAY 通知、服务端推送等。
- 完善文档中对压测结果、性能指标的记录。

## 8. 客户端异步调用 MVP（Phase 1）
- **待处理映射**：`KrpcChannel` 新增 `PendingCall` 结构（响应指针、Controller、promise/future、回调与 request_id），所有在途请求集中放入 `m_pending_calls`，便于匹配响应与超时清理。
- **通俗说法**：就像把所有挂号单放进取号柜，编号一一对应，服务窗口（服务器响应）一来就能找到原始请求，没人会排错队。
- **CallFuture 流程**：`CallMethod` 退化为 thin wrapper，统一走 `CallFuture` 进行序列化、`EnsureConnection`、注册 pending，再交由 `SendBuffer` 写 socket。传入 `done` 时完全异步执行，不传时使用 `shared_future` 阻塞等待。
- **通俗说法**：业务线程只负责填好表格塞进传送带，后面的流水线会自动连线、发包、等结果；要同步就原地等，要异步就登记一个回拨电话。
- **RecvLoop + 心跳整合**：新增后台接收线程解析 length-prefix 帧、调用 `ResolvePendingCall` 或 `ResolveHeartbeat`。心跳发送端只负责编写 PING，由 recv loop 识别 PONG，避免与业务读抢占。
- **通俗说法**：相当于安排了一个专门的前台负责收快递，正常包裹和“我还活着”的心跳信号都它来拆，别的线程不用再手忙脚乱抢着读 socket。
- **失败兜底**：`HandleHeartbeatFailure`、`RemovePendingCall`、`FailPendingCalls` 统一收口，出现发送异常、心跳超时或连接关闭时能及时关闭 fd、标记 Controller 出错并唤醒回调。
- **通俗说法**：一旦线路掉线，这个“事故处理中心”会立刻通知所有等待的人“今天别等了”，同时把电话线重新插好，避免有人傻站着不走。

## 9. 客户端异步 Phase 2（Callback + Timeout Manager）
- **CallAsync 接口**：新增 `KrpcChannel::CallAsync(..., AsyncCallback cb)`，业务侧可直接传入 `std::function`（签名：`RpcController*, Message*`），无需再手动创建 `Closure`。原有 `CallMethod` 仍兼容同步/`done` 异步调用。
- **通俗说法**：除了“打电话等结果”，现在还能留个号码就走，系统自己回拨，业务不用知道底下是 promise 还是 closure。
- **SendQueue & SendLoop**：`CallFuture` 不再直接 `send()`，而是把序列化后的包入队，由独立 `SendLoop` 串行写 socket，保证业务线程立刻返回，同时与心跳写操作通过同一把锁协作。
- **通俗说法**：所有包裹统一交给物流分拣中心发货，避免大家挤在快递窗口，自然也不会和心跳线程抢同一根网线。
- **TimeoutManager**：后台线程每 50ms 扫描 `m_pending_calls`，超过 `timeout_ms` 的请求会立刻标记失败并触发 promise/回调，异步调用不再依赖业务线程轮询判定超时。
- **通俗说法**：有个计时器专门盯着“谁已经等太久”，超点就发通知，不用业务写额外的 watchdog。
- **统一完成路径**：所有成功/失败场景（正常响应、发送异常、心跳掉线、显式超时）都经 `CompletePending`，确保 `controller->Failed()`、`done->Run()`、`AsyncCallback` 只触发一次。
- **通俗说法**：就像客服工单中心，任何结论都得通过同一个出口广播，避免一个问题被重复通知或完全遗忘。
- **排队清理**：连接断开时会清空发送队列并逐个 `RemovePendingCall`，避免仍在排队的请求挂死；`HandleHeartbeatFailure` 也会唤醒发送/接收/心跳等待者。
- **通俗说法**：一旦发现公交停运，会把站台上排队的人统统劝走重新安排，保证没有“排在队里却永远上不了车”的尴尬。
- **验证方式**：重新 `cmake --build build && ./bin/server -i ./bin/test.conf` 后，在不同终端执行 `./bin/client -i ./bin/test.conf`（压力 + CallAsync stub）、`./bin/timeout_client -i ./bin/test.conf`（观察 TimeoutManager 回调）、`./bin/heartbeat_client -i ./bin/test.conf`（确保心跳丢包后请求被及时 fa123456
il），日志中可看到 send loop、timeout manager 的相关输出。

## 10. 服务端异步 Phase 3（线程池调度 RPC）
- **线程池调度**：`KrpcProvider` 在 `Run()` 时启动固定大小的 worker 组（默认取 `std::thread::hardware_concurrency()`，最少 4）。Muduo IO 线程仅负责解析帧并将 RPC 任务压入队列，`Service::CallMethod` 由 worker 顺序执行，避免耗时 handler 阻塞网络事件。
- **通俗说法**：把“窗口收材料 + 后台审批”拆开，接待窗口只负责收单，真正审件的搬到后仓；窗口就算来了一堆慢单也不会被拖死。
- **任务队列与背压**：借助 `std::deque` + `condition_variable` 实现阻塞队列，容量由 `provider_queue_capacity`（默认 1024）控制。队列满时生产者会等待，框架停机或超出限制时会回退为当前线程同步执行并打印告警，防止无限排队占满内存。
- **通俗说法**：等于在前台放了一个限流取号机，号满了就让人临时原地办理或稍后再试，不会让大厅挤爆。
- **资源/异常处理**：任务持有 request/response/`Closure` 指针，worker 通过 `unique_ptr` 自动释放 request，执行期间捕获异常并写入日志，必要时丢弃响应/回调，确保单个服务崩溃不会毒死整个池子。
- **通俗说法**：审核员的桌子自带碎纸机，谁把材料弄坏了自己清理，别指望保洁；就算有人发脾气砸桌子，也只影响当前办件。
- **心跳与连接管理**：`MSG_TYPE_PING/PONG` 仍在 IO 线程即时应答，不进入队列，`connection_states_` 能持续刷新，长时间运行不会误判为超时。
- **通俗说法**：保安巡逻、心跳打卡仍在原班人马负责，后仓再忙也不耽误门卫登记。
- **验证方式**：`cmake --build build && ./bin/server -i ./bin/test.conf` 后，同时运行 `./bin/client`、`./bin/heartbeat_client`、`./bin/timeout_client`，并在服务实现中注入 `sleep` 模拟慢调用，确认心跳/超时仍按预期触发且 server 日志无阻塞告警。

## 12. 客户端连接池（ZK 缓存 + 复用）
- **功能**：按端点缓存空闲连接，避免每次调用握手；ZK 查询结果做进程内缓存，减少重复读取。
- **配置**（`bin/test.conf`）：`enable_connection_pool`（1 开、0 关，默认 1），`connection_pool_max_idle`（每端点最大空闲连接数，默认 4）。
- **实现要点**：
  - `Krpcchannel` 归还连接前做基础健康检查，超过池上限直接关闭；缓存服务地址（method_path -> ip:port）。
  - 复用/新建日志：`reuse pooled connection` vs `connect server success`。
  - `example/pool_demo` 支持 `POOL_DEMO_MODE=new_channel`，开池时首条握手后续复用，关池时每次握手。
- **验证命令**：
  ```bash
  # 开池复用
  enable_connection_pool=1  # 配置
  POOL_DEMO_MODE=new_channel ./bin/pool_demo -i ./bin/test.conf > /tmp/pool_demo.log 2>&1
  grep -E "connect server success|reuse pooled connection" /tmp/pool_demo.log
  # 关池对比
  enable_connection_pool=0  # 配置
  POOL_DEMO_MODE=new_channel ./bin/pool_demo -i ./bin/test.conf > /tmp/pool_demo.log 2>&1
  grep -E "connect server success|reuse pooled connection" /tmp/pool_demo.log
  ```

## 11. 异步示例 Phase 4（示例/验证）
- **新增示例**：`example/async_demo/AsyncClient.cc`，通过 `KrpcChannel::CallAsync` 演示 future 等待与 callback 回调两种形态，支持并发、请求数、超时、自定义间隔（环境变量：`ASYNC_MODE`=`future|callback`、`ASYNC_CONCURRENCY`、`ASYNC_REQUESTS`、`ASYNC_TIMEOUT_MS`、`ASYNC_SLEEP_MS`）。
- **构建**：`cmake --build build --target async_client`。
- **运行**：
  - future 模式：`./bin/async_client -i ./bin/test.conf`（默认 future，可用 `ASYNC_CONCURRENCY=4 ASYNC_REQUESTS=20` 调整）。
  - callback 模式：`ASYNC_MODE=callback ASYNC_CONCURRENCY=4 ASYNC_REQUESTS=20 ./bin/async_client -i ./bin/test.conf`。
- **输出**：打印总请求数、成功/失败、p50/p95/p99 延迟、总耗时，便于对比同步 vs 异步、future vs callback。
