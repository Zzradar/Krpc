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
