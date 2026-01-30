#ifndef KRPC_METRICS_EXPORT_H
#define KRPC_METRICS_EXPORT_H

#include <cstdint>
#include <string>

struct MetricsSnapshot {
    int64_t window_ms{0};
    double qps{0.0};
    int success{0};
    int fail{0};
    double fail_rate{0.0};
    int64_t p50{0};
    int64_t p95{0};
    int64_t p99{0};
    int64_t min{0};
    int64_t max{0};
    double avg{0.0};
};

// 取当前窗口的指标，reset=true 时顺便重置窗口。
bool GetMetricsSnapshot(MetricsSnapshot &out, bool reset);

// 以 Prometheus 文本格式输出当前指标；若暂无数据，返回空字符串。
std::string RenderMetricsPrometheus();

// 追加一个样本（成功/失败，耗时 ms），用于复用同一套聚合器。
void RecordMetricsSample(bool success, int64_t cost_ms);

// 追加一个带标签的样本（例如 service.method），同时更新全局与分组指标。
void RecordMetricsSampleWithLabel(const std::string &label, bool success, int64_t cost_ms);

#endif // KRPC_METRICS_EXPORT_H
