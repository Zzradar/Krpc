# 阶段A短连接优化复盘（面试版）

> 日期：2026-04-15  
> 项目：Krpc  
> 目标：验证“protobuf 与 msgpack 差距是否主要来自实现差异”，并修复 protobuf 短连接路径瓶颈。

---

## 1. 问题定义

阶段A最初结果里，`protobuf` 在 `short` 模式显著慢于 `msgpack`，且出现过无效样本（服务不可达、失败请求混入）。

在同机同参（`BENCH_CONCURRENCY=4 BENCH_REQUESTS=200`）下复盘后，确认问题不只在序列化格式，核心在**短连接实现路径的额外开销**。

---

## 2. 关键证据（优化前）

优化前（同参数）`protobuf sync+short` QPS 约：

- payload 0KB: `17.48`
- payload 1KB: `17.45`
- payload 64KB: `16.20`
- payload 256KB: `15.71`
- payload 1024KB: `11.11`

这个量级明显异常，且和 `msgpack sync+short`（约 `38~163`）差距过大，不符合“仅协议差异”预期。

---

## 3. 根因分析

### 3.1 触发条件（为什么是 short 模式最明显）

`bench_demo` 的 `short` 模式每个请求都会创建并销毁 channel（局部对象离开作用域即析构），因此会高频触发线程回收路径。

- 参考：`example/bench_demo/BenchClient.cc` 中 `short_channel.reset(new KrpcChannel(false));`

### 3.2 旧流程里的阻塞点

旧流程中，`KrpcChannel` 析构会调用 `StopRecvThread()`，它会 `join` 接收线程；但接收线程经常阻塞在 `poll(fd, 200ms)`。

对应代码职责：

- `StopRecvThread`：设置 `m_recv_running=false`，等待接收线程退出（对象生命周期收尾）
- `RecvLoop`：循环 `poll/recv` 处理网络返回

关键细节：

1. `m_recv_cv.notify_all()` 只能唤醒 `condition_variable` 等待，**不能直接打断正在执行的 `poll`**。  
2. 当线程阻塞在 `poll` 且当前没有新数据到来时，`join` 需要等 poll 超时或 fd 状态变化后才返回。  
3. short 模式每请求都析构 channel，这个等待会被放大为稳定的固定开销。  

### 3.3 “接收线程可能阻塞在 poll”具体包括哪些场景

1. 请求已发出但服务端还未返回（socket 暂时无可读事件）。  
2. 请求已完成，接收线程进入下一轮循环后再次等待可读事件。  
3. 连接处于空闲状态但尚未关闭，`poll` 正常等待事件。  

### 3.4 “非接收线程触发 stop”是指什么

在当前实现里，`StopRecvThread()` 的主要调用点是析构函数；析构通常发生在业务调用线程（而不是接收线程本身）。  
因此出现“业务线程 stop，接收线程阻塞在 poll，业务线程 join 等待”的典型阻塞链路。

问题本质不是 protobuf 编解码本身，而是**短连接生命周期下线程回收阻塞**带来的实现成本。

### 3.5 stop 的职责与“唤醒 poll/recv”的目的

`StopRecvThread()` 不是“仅仅关线程”，它承担三件职责：

1. **状态收敛**：把 `m_recv_running` 置为 false，让接收循环进入退出条件。  
2. **生命周期收尾**：保证 channel 析构前，接收线程不再访问 channel 内部状态（避免悬垂访问）。  
3. **等待边界控制**：调用方通过 `join` 等待线程退出，期望这个等待是可预期、有上界的。  

为什么一定要“唤醒阻塞的 poll/recv”：

1. 接收线程在内核态 `poll/recv` 阻塞时，看不到用户态的 `running=false` 标志。  
2. 若不主动唤醒，`join` 只能被动等超时/网络事件，短连接下会放大成每次请求的固定尾部成本。  
3. 主动唤醒后，接收线程能立即回到用户态检查退出条件，尽快结束生命周期，避免把“线程回收等待”混进协议对比结果。  

---

## 4. 实施改动（Step1）

### 4.1 KrpcChannel 短连接回收优化

文件：`src/Krpcchannel.cc`

改动：

1. 在 `StopRecvThread()` 中，当由**非接收线程**触发 stop 时，主动 `shutdown(fd, SHUT_RDWR)`。  
2. 保留 self-stop 分支：若 stop 发生在接收线程自身，不额外做 `shutdown`，避免自触发副作用。  

### 4.2 流程变动与因果

改动前：

1. 业务线程析构 channel。  
2. `StopRecvThread()` 置位 running=false 并 join。  
3. 接收线程若正阻塞在 `poll`，需等超时/事件后才能退出。  
4. 业务线程被动等待，形成短连接固定开销。  

改动后：

1. 业务线程析构 channel。  
2. `StopRecvThread()` 置位 running=false，并对 fd 执行 `shutdown`（仅非接收线程场景）。  
3. `poll/recv` 被立即唤醒返回，接收线程快速退出。  
4. join 快速完成，短连接固定开销显著下降。  

### 4.3 线程职责边界（为什么要区分 self-stop / non-self-stop）

1. `self-stop`（接收线程自己触发）时，不做额外 `shutdown`：避免在同一执行上下文引入重复关闭/异常路径干扰。  
2. `non-self-stop`（业务线程析构触发）时，执行 `shutdown`：目的是跨线程打断阻塞系统调用，让“发起 stop 的线程”不用被动等待。  
3. 这个区分本质是把“控制线程”和“执行阻塞 IO 的线程”解耦，保证 stop 语义稳定。  

### 4.4 基准公平性修正（本轮已一并完成）

文件：`example/bench_demo/BenchClient.cc`

改动：

1. `msgpack async` 改为 callback 统计路径（与 protobuf async 对齐），避免 future/批量收集带来的对比偏差。  
2. 修正分位数计算为 nearest-rank，降低小样本统计失真。  
3. 修复 msgpack async keepalive 回调上下文中的 channel 生命周期持有问题（避免提前析构导致失败）。

文件：`scripts/run_bench_matrix.sh`

改动：

1. 只要 `fail>0` 或 summary 解析失败，即判无效 case，脚本非0退出。  

---

## 5. 优化后结果（同参数、同矩阵，含高分位）

最新结果文件：

- `docs/bench_results_protobuf.txt`
- `docs/bench_results_msgpack.txt`

两侧均为有效样本（各 20/20 case，`fail=0`）。

### 5.1 protobuf sync+short 改善（核心）

优化后 `protobuf sync+short` QPS：

- payload 0KB: `91.45`
- payload 1KB: `72.97`
- payload 64KB: `60.42`
- payload 256KB: `39.15`
- payload 1024KB: `24.55`

相较优化前 `11~17` 区间，提升约 **2.2x ~ 5.2x**。

对应高分位（优化后）：

- payload 0KB: `p95=107ms p99=112ms`
- payload 1KB: `p95=103ms p99=108ms`
- payload 64KB: `p95=107ms p99=111ms`
- payload 256KB: `p95=111ms p99=114ms`
- payload 1024KB: `p95=116ms p99=127ms`

### 5.2 codec 对比（优化后，QPS + p95/p99）

`sync + short` 对比：

- payload 0KB: proto `qps=91.45 p95=107 p99=112`；msg `qps=162.87 p95=20 p99=35`
- payload 1KB: proto `qps=72.97 p95=103 p99=108`；msg `qps=157.85 p95=23 p99=32`
- payload 64KB: proto `qps=60.42 p95=107 p99=111`；msg `qps=153.02 p95=26 p99=32`
- payload 256KB: proto `qps=39.15 p95=111 p99=114`；msg `qps=111.17 p95=38 p99=51`
- payload 1024KB: proto `qps=24.55 p95=116 p99=127`；msg `qps=38.66 p95=121 p99=154`

`async + short` 对比（展示大包场景）：

- payload 256KB: proto `qps=83.75 p95=987 p99=1034`；msg `qps=143.58 p95=760 p99=789`
- payload 1024KB: proto `qps=57.37 p95=2198 p99=2243`；msg `qps=70.75 p95=1858 p99=1976`

按模式平均 `proto/msg` QPS 比值：

- `sync/keepalive`: `0.51`
- `sync/short`: `0.48`
- `async/keepalive`: `0.54`
- `async/short`: `0.69`

结论：仍有差距，但已经从“异常级失衡”收敛到“可解释的实现差异 + 协议差异叠加”。

---

## 6. 面试可复述版本（60秒）

我在阶段A发现 protobuf 与 msgpack 差距异常大，先排除了脏样本问题（把 `fail>0` 全部拦截），然后做了同参复跑。定位发现瓶颈主要在 protobuf 短连接回收路径：`KrpcChannel` 销毁时接收线程可能阻塞，导致每次短连接请求多出固定等待。我在 `StopRecvThread` 增加了非接收线程场景的 `shutdown` 唤醒，显著降低线程回收成本；同时把 msgpack async 测量路径改成与 protobuf 一致，保证对比公平。最终 `protobuf sync+short` QPS 从 11~17 提升到 24~91，提升 2~5 倍，说明原先大差距主要是实现问题而不是单纯协议问题。

---

## 7. 后续建议

1. 继续优化 protobuf `async+short` 的高分位延迟（连接建立与回调调度路径）。  
2. 引入多轮重复运行与统计汇总（均值/方差），降低单次波动。  
3. 在结果汇报中区分“协议差异”和“实现差异”，避免过度归因。
