# msgpack async callback: ZK 节点偶发不可见（排查记录）

## 复现/验证命令

### 1) 启动服务端
```bash
/home/zzz/Krpc/bin/server -i /home/zzz/Krpc/bin/test_msgpack_async.conf
```

### 2) 运行 async callback 客户端
```bash
ASYNC_MODE=callback ASYNC_CONCURRENCY=2 ASYNC_REQUESTS=20 ASYNC_TIMEOUT_MS=5000 /home/zzz/Krpc/bin/async_client -i /home/zzz/Krpc/bin/test_msgpack_async.conf
```

### 3) ZK 查询（在另一个终端）
```bash
/usr/share/zookeeper/bin/zkCli.sh -server 127.0.0.1:2181
```

进入 zkCli 后执行：
```
ls /UserServiceRpc/Login
```

退出 zkCli：
```
quit
```
