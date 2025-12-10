# 异步调用 & 线程池改造设计

> 目标：让客户端支持非阻塞调用，服务端解耦 IO 与业务执行，整体吞吐从“同步阻塞”提升到“高并发异步”。

## 1. 客户端整体架构

```
+---------------+       +-------------------+
| 业务线程 (N)  |-----> | KrpcChannel       |
| CallAsync()   |       |  - PendingManager |
| CallFuture()  |       |  - SendQueue      |
+---------------+       |  - RecvLoop       |
                        +-------------------+
                                 |
                              Socket
                                 |
                         +---------------+
                         | RPC Server    |
                         +---------------+
```

### 1.1 新增组件

| 组件 | 职责 |
| --- | --- |
| PendingManager | `request_id -> PendingCall` 的上下文管理器，包含回调、promise、超时信息。|
| SendQueue | 锁保护的待发送队列，业务线程只负责入队。|
| RecvLoop  | 专用线程：poll + recv，解析响应后根据 `request_id` 查找 PendingCall 并触发回调。|
| TimeoutManager | （阶段 2 可选）小根堆或时间轮，定期扫描 PendingCall，超时后触发失败回调。|

### 1.2 接口形态

```cpp
class KrpcChannel : public google::protobuf::RpcChannel {
public:
    void CallMethod(...);               // 保留同步：内部复用 future 模式
    void CallAsync(..., Callback cb);   // 新增：立即返回
    RpcFuture CallFuture(...);          // 新增：返回 future
};
```

`PendingCall` 结构：
```
struct PendingCall {
    uint64_t request_id;
    std::chrono::steady_clock::time_point start;
    int timeout_ms;
    std::function<void(Status, ResponsePtr)> callback; // callback 模式
    std::promise<ResponsePtr> promise;                 // future 模式
    google::protobuf::Message *response_proto;         // 反序列化目标
};
```

同步 `CallMethod` 将调用 `CallFuture`，然后 `future.get()`：保持与现有 stub 接口兼容。

### 1.3 请求流程（对应参考文章 17 步）

1. 业务线程调用 `CallAsync`。
2. 分配 `request_id`，构造 `PendingCall`，写入 `PendingManager`。
3. 序列化 request → Packet。
4. 入 `SendQueue`，立即返回。
5. `SendLoop`（可复用心跳线程或新增线程）从队列取 packet，上锁发送。
6. Server 处理并返回。
7. `RecvLoop` poll+recv，解析 header。
8. 根据 `request_id` 在 `PendingManager` 找到上下文。
9. 反序列化 body。
10. 执行回调或 fulfill promise。
11. 清理 `PendingCall`，资源归还。

超时路径：
- TimeoutManager 定期检查 `PendingCall`，若 `now - start > timeout_ms`，则移除并调用回调告知超时；同时需通知 `RecvLoop`，以免真实响应晚到时已经失效（可忽略或打印 warning）。

### 1.4 并发控制
- `PendingManager` 使用 `std::mutex + unordered_map`，也可以后续换成 sharded map。
- `SendQueue` 可用 `std::queue + mutex + condition_variable`。
- `RecvLoop` 独立线程运行，内部需要与心跳线程协调 socket 读写，可考虑将心跳逻辑并入 RecvLoop：
  - poll 事件包含 `POLLIN` + 定时器；
  - 心跳发送仍可由单独线程触发，但写操作需通过统一 `SendLoop`，避免乱序。

## 2. 服务端线程模型

```
          +-------------------+
          |  Reactor (IO)     |
          | OnMessage         |
          +-------------------+
                    |
       +--------------------------+
       | ThreadPool (workers)     |
       +--------------------------+
        |          |           |
  CallMethod   CallMethod   ...
```

### 2.1 改造要点
- `OnMessage` 解析 header + request，构造 `RpcTask`：
  ```cpp
  struct RpcTask {
      google::protobuf::Service *service;
      const google::protobuf::MethodDescriptor *method;
      std::unique_ptr<Message> request;
      std::unique_ptr<Message> response;
      muduo::net::TcpConnectionPtr conn;
      uint64_t request_id;
  };
  ```
- 将 `RpcTask` 投递到线程池：
  ```cpp
  thread_pool.enqueue([task = std::move(task)]() mutable {
      task.service->CallMethod(...);
      provider->SendRpcResponse(task.conn, task.request_id, task.response.release());
  });
  ```
- 心跳帧、协议错误等轻量逻辑仍在 IO 线程立即处理。
- 线程池尺寸与队列行为通过配置控制：`worker_thread_num`、`max_queue_size`、饱和策略（阻塞/丢弃/调用者运行）。

### 2.2 安全与性能
- Muduo 支持跨线程调用 `TcpConnection::send`，无需额外切回 IO 线程。若需要顺序保证，可把 `SendRpcResponse` 改为 `conn->getLoop()->runInLoop(...)`。
- 注意 RPC 方法内抛异常时要捕获，记录日志并返回错误响应，避免 worker 线程退出。
- 为避免 request/response 重复申请，可考虑对象池，但初期以简单稳定为主。

## 3. 配置与扩展
- 新配置项：
  - `async_send_queue_size`
  - `worker_thread_num`
  - `pending_timeout_ms`（单独控制 pending 超时，可默认沿用 `rpc_timeout_ms`）。
- 调试开关：
  - `enable_async_channel=true`：允许逐步灰度发布。
  - `enable_server_threadpool=true`：可单独启用服务端线程池。

## 4. 迁移计划
1. **文档 & 原型**：完成本设计文档，评审通过后动工。
2. **客户端异步实现**（阶段 1）
   - PendingManager + CallAsync/Future。
   - SendQueue + RecvLoop。
   - 与现有心跳互通测试。
3. **TimeoutManager & 错误处理**（阶段 2）
   - 支持 per-call timeout 回调。
   - 断线/重连过程中清理 pending。
4. **服务端线程池**（阶段 3）
   - 引入 ThreadPool。
   - OnMessage 投递任务执行。
5. **示例 & 压测**（阶段 4）
   - 新增 async demo。
   - 对比同步/异步 QPS。
6. **文档更新**（阶段 5）
   - README 心跳章节后面追加“异步模式”说明。
   - `docs/feature_summary.md` 追踪新功能。

## 5. 分阶段实施方案

| 阶段 | 目标 | 主要任务 | 产出 / 验收 |
| --- | --- | --- | --- |
| Phase 0：设计巩固 | 统一接口、线程模型、配置项认知 | 评审本设计文档，若有新增配置提前在 `Krpcconfig` 预留；整理现有测试用例（client/heartbeat/timeout） baseline | 评审通过的设计文档、测试基线记录 |
| Phase 1：客户端异步 MVP | 支持 `CallFuture` + `CallMethod` 复用，完成 Pending 管理 | 1) `PendingManager` + `PendingCall` map；2) `CallFuture` 接口；3) `RecvLoop` 负责读响应并唤醒 promise；4) 同步调用通过 future.get() 实现 | 单元测试：单 client 发 100 并发 future 请求全部成功；旧同步调用无回归 |
| Phase 2：Callback & Timeout Manager | 丰富异步形态，并实现超时清理 | 1) `CallAsync` + 回调注册；2) `TimeoutManager` 定期扫描 pending；3) 连接断开时统一 fail pending；4) SendQueue/SendThread 拆分，保障业务线程无阻塞 | 压测：异步 callback 模式发 1k 并发无阻塞；超时请求能在指定时间内回调失败 |
| Phase 3：服务端线程池 | 解耦 IO 与业务执行 | 1) 引入 ThreadPool（可配置线程数/队列）；2) OnMessage 将 RPC 投递至 worker；3) 心跳/错误仍在 IO 线程处理；4) Worker 异常捕获 + 响应错误 | 压测：模拟慢业务时，IO 线程仍能处理其他请求；监控线程池队列长度 |
| Phase 4：示例/验证 | 产出示例、脚本、配置说明 | 1) `example/async_demo` 展示 future & callback；2) README 增加“异步模式”手册；3) `docs/feature_summary.md` 更新；4) 新增性能对比数据 | Demo 运行截图、README 新章节、性能对比表 |
| Phase 5：优化 & 扩展 | 连接池或更多调优 | 1) 评估连接池需求；2) PendingManager sharding；3) 可选 metrics/监控 | 优化项 backlog、必要的跟进任务 |

## 5. 风险 & 缓解
| 风险 | 缓解措施 |
| --- | --- |
| 回调执行在未知线程，业务代码需保证线程安全 | 文档明确约束，可提供配置让回调在专门线程池运行 |
| 连接断开时 pending 未清理 | 心跳/RecvLoop 一旦 detect 断连，统一 FailPending | 
| 线程池阻塞导致积压 | 支持配置队列最大长度，必要时 back-pressure 到客户端 | 
| Debug 难度上升 | 增加关键日志：注册/完成 request、pending 数量、队列长度 | 

---
后续实现阶段将以此文档为蓝本逐步推进，过程中如有修改会在此补充。