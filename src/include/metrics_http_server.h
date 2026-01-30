#ifndef KRPC_METRICS_HTTP_SERVER_H
#define KRPC_METRICS_HTTP_SERVER_H

// 启动/停止简易指标 HTTP 服务，返回 true 表示启动成功。
bool StartMetricsHttpServer(int port);
void StopMetricsHttpServer();

#endif // KRPC_METRICS_HTTP_SERVER_H
