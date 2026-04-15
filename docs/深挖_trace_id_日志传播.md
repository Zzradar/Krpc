# 深挖点：`trace_id` 日志传播（排障与稳定性叙事）

## 选择理由

面试里“稳定性/可观测性”通常追问：出了问题你怎么定位到请求链路、怎么证明日志是可关联的。  
在 Krpc 里，我们可以用 `trace_id` 让客户端一次调用与服务端处理日志在同一个上下文上对齐，从而把排障从“猜”变成“查”。

## 实现路径（你需要讲清的 3 段）

### 1) 协议层：`RpcHeader` 增加 `trace_id`

- 位置：[`src/Krpcheader.proto`](../src/Krpcheader.proto)
- 含义：服务端收到请求时，可以从 header 里拿到 `trace_id`。

### 2) 客户端层：写入 header（controller 优先，其次读取环境变量）

- controller 字段：[`src/include/Krpccontroller.h`](../src/include/Krpccontroller.h)、[`src/Krpccontroller.cc`](../src/Krpccontroller.cc)
- header 写入逻辑：[`src/Krpcchannel.cc`](../src/Krpcchannel.cc)
  - 若 `Krpccontroller::TraceId()` 非空，使用它
  - 否则尝试环境变量 `KRPC_TRACE_ID`
  - 再否则 fallback 为 `req-<request_id>`

### 3) 服务端层：接收请求并在日志里输出 `trace_id`

- 位置：[`src/Krpcprovider.cc`](../src/Krpcprovider.cc)
- 服务端在 `OnMessage` 解析出 `RpcHeader` 后，会把 `service_name.method_name` 与 `trace_id` 一起打印。

## 验证方式（附数据：单元测试通过）

由于链路传播跨网络涉及 Muduo/运行态，本仓库用“协议与状态机”的单元测试给出可复现的证据链：

1. `trace_id` 在 `RpcHeader` 的序列化/反序列化保持一致  
   - 测试：[`tests/test_header.cc`](../tests/test_header.cc)
2. `Krpccontroller::Reset()` 会清空 `trace_id`，避免复用 controller 时链路串扰  
   - 测试：[`tests/test_controller.cc`](../tests/test_controller.cc)

### 运行命令

```bash
bash scripts/ci_unit_tests.sh
```

该脚本会：
- 配置构建只跑 tests：`-DKRPC_BUILD_FRAMEWORK=OFF -DKRPC_BUILD_TESTS=ON`
- 执行 `ctest --output-on-failure -V`

你在输出中应该能看到类似：`All tests passed`（本次执行已验证通过）。

## 面试话术（建议你背这 4 句）

1. 我们把 `trace_id` 放进 `RpcHeader`，这样服务端无需额外上下文即可关联请求链路。  
2. 客户端写入时 controller 优先，便于业务自定义；没有的话走环境变量和 fallback，保证链路不断。  
3. 可观测性验证我用单元测试覆盖序列化正确性与 controller reset，避免上线后“日志串”的隐患。  
4. 之后如果要上链路追踪系统（如 OpenTelemetry），这个字段就是天然的上下文传播载体。

