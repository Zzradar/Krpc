# 异步模式速记：future vs callback

记录两种常见异步调用形态的概念、适用场景与在 Krpc 中的实现方式，便于后续扩展类似知识。

## future 模式
- **含义**：发起调用立刻返回一个“未来结果”占位符（`std::future`），业务在需要时 `wait()/get()`，写法接近同步但底层异步。
- **适用场景**：批量提交、稍后收割结果；希望调用点决定等待时机；保持代码结构顺序化。
- **缺点**：如果等待放在循环内，容易形成“发一条等一条”的节奏，压测吞吐低于完全异步；持有 future 的线程仍需阻塞等待。
- **Krpc 实现要点**：
  - `KrpcChannel::CallAsync` 注册回调，回调里 `promise.set_value()`，调用线程 `future.wait()` 收割。
  - `example/async_demo/AsyncClient.cc`：每个请求创建 `std::promise<void>`，回调统计耗时并 `set_value()`，线程侧 `future.wait()` 后再发下一次。
  - 生命周期：`shared_ptr` 持有 controller/response/promise，防止回调晚于栈帧结束。

## callback 模式
- **含义**：发起调用立即返回，传入回调函数，响应或错误发生时框架线程执行回调，无需调用方主动等待。
- **适用场景**：无需逐条同步等待的高并发请求；事件驱动/流式处理；希望尽快把请求挂出去。
- **缺点**：回调线程语义需自行保证线程安全；业务逻辑分散在回调里，控制流不如同步写法直观。
- **Krpc 实现要点**：
  - `KrpcChannel::CallAsync` 直接接受回调（签名：`RpcController*, Message*`），完成后回调内处理结果。
  - `example/async_demo/AsyncClient.cc`：批量发起请求，把每个回调绑定一个 `promise` 并记录延迟，主线程收集 `shared_future` 统一等待收尾。
  - 线程语义：回调在框架异步线程触发，业务代码需自担线程安全。

## 对比小结
- **等待方式**：future 由调用方决定何时阻塞；callback 完全事件驱动。
- **并发行为**：future 示例里每次 wait 后再发下一条，节奏线性、受控；callback 可以更快把请求压出，吞吐更高。
- **代码风格**：future 保持同步式写法；callback 更偏事件驱动，需要在回调里收口逻辑。
