#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "../user.pb.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// 读取整型环境变量，异常或缺失时返回默认值
int ReadEnvInt(const char *key, int default_value) {
    const char *val = std::getenv(key);
    if (!val) {
        return default_value;
    }
    try {
        return std::stoi(val);
    } catch (...) {
        return default_value;
    }
}

// 读取字符串环境变量
std::string ReadEnvString(const char *key, const std::string &default_value) {
    const char *val = std::getenv(key);
    if (!val) {
        return default_value;
    }
    return std::string(val);
}

// 运行参数：模式、并发、总请求、超时、两次请求间隔
// 环境变量：ASYNC_MODE=future|callback，ASYNC_CONCURRENCY，并发 ASYNC_REQUESTS，总请求 ASYNC_TIMEOUT_MS，间隔 ASYNC_SLEEP_MS
struct Options {
    std::string mode{"future"}; // future | callback
    int concurrency{4};
    int requests{20};
    int timeout_ms{3000};
    int sleep_ms{0};
};

// 采样指标：成功/失败计数与延迟样本，原子变量用于多线程安全累加
struct Metrics {
    std::atomic<int> success{0};
    std::atomic<int> fail{0};
    std::mutex latency_mutex;
    std::vector<double> latencies_ms;
};

// 线程安全地记录一次延迟
void RecordLatency(Metrics &metrics, double latency_ms) {
    std::lock_guard<std::mutex> lock(metrics.latency_mutex);
    metrics.latencies_ms.push_back(latency_ms);
}

// 计算分位数，简单排序插值（线性插值，避免样本稀疏时阶梯跳变）
double Percentile(const std::vector<double> &values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const double rank = (p / 100.0) * (sorted.size() - 1);
    const size_t lower = static_cast<size_t>(rank);
    const size_t upper = std::min(sorted.size() - 1, lower + 1);
    const double frac = rank - lower;
    return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
}

// 从环境变量载入参数，提供兜底
Options LoadOptions() {
    Options opts;
    opts.mode = ReadEnvString("ASYNC_MODE", opts.mode);
    opts.concurrency = ReadEnvInt("ASYNC_CONCURRENCY", opts.concurrency);
    opts.requests = ReadEnvInt("ASYNC_REQUESTS", opts.requests);
    opts.timeout_ms = ReadEnvInt("ASYNC_TIMEOUT_MS", opts.timeout_ms);
    opts.sleep_ms = ReadEnvInt("ASYNC_SLEEP_MS", opts.sleep_ms);
    if (opts.concurrency < 1) {
        opts.concurrency = 1;
    }
    if (opts.requests < 1) {
        opts.requests = 1;
    }
    return opts;
}

// 判定一次调用是否成功：先看 controller（传输/框架错误），再看业务 errcode
bool IsSuccess(const Kuser::LoginResponse &resp, const ::google::protobuf::RpcController *controller) {
    if (controller && controller->Failed()) {
        return false;
    }
    return resp.result().errcode() == 0;
}

// 汇总打印：模式、并发、成功失败数、p50/p95/p99
void PrintSummary(const Options &opts, Metrics &metrics) {
    std::vector<double> latencies_copy;
    {
        std::lock_guard<std::mutex> lock(metrics.latency_mutex);
        latencies_copy = metrics.latencies_ms;
    }

    const int total = metrics.success.load() + metrics.fail.load();
    std::cout << "\n===== async demo summary =====" << std::endl;
    std::cout << "mode=" << opts.mode
              << " concurrency=" << opts.concurrency
              << " requests=" << opts.requests
              << " timeout_ms=" << opts.timeout_ms << std::endl;
    std::cout << "total=" << total
              << " success=" << metrics.success
              << " fail=" << metrics.fail << std::endl;
    if (!latencies_copy.empty()) {
        const double p50 = Percentile(latencies_copy, 50.0);
        const double p95 = Percentile(latencies_copy, 95.0);
        const double p99 = Percentile(latencies_copy, 99.0);
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "latency_ms p50=" << p50
                  << " p95=" << p95
                  << " p99=" << p99 << std::endl;
    }
}

// future 模式：每个线程提交请求，回调里 fulfill promise，再由调用线程 wait
// 采用 shared_ptr 持有 controller/response/promise，保证回调晚于栈帧结束也不会悬挂
void RunFutureMode(KrpcChannel *channel,
                   const ::google::protobuf::MethodDescriptor *method,
                   const Options &opts,
                   Metrics &metrics) {
    const int base = opts.requests / opts.concurrency;
    const int extra = opts.requests % opts.concurrency;
    std::vector<std::thread> threads;
    threads.reserve(opts.concurrency);

    for (int t = 0; t < opts.concurrency; ++t) {
        const int count = base + (t < extra ? 1 : 0);
        if (count <= 0) {
            continue;
        }
        threads.emplace_back([channel, method, count, &opts, &metrics]() {
            for (int i = 0; i < count; ++i) {
                Kuser::LoginRequest request;
                request.set_name("async_future");
                request.set_pwd("123456");

                auto controller = std::make_shared<Krpccontroller>();
                if (opts.timeout_ms > 0) {
                    controller->SetTimeoutMs(opts.timeout_ms);
                }
                auto response = std::make_shared<Kuser::LoginResponse>();
                // 回调通过 promise 唤醒调用线程，避免业务侧手写同步
                // std::promise/std::future 是 C++11 的异步同步原语：set_value() 唤醒 future.wait()
                auto promise_ptr = std::make_shared<std::promise<void>>();
                auto future = promise_ptr->get_future();
                const auto start = std::chrono::steady_clock::now();

                channel->CallAsync(method,
                                   controller.get(),
                                   &request,
                                   response.get(),
                                   [controller, response, promise_ptr, start, &metrics](::google::protobuf::RpcController *ctrl, ::google::protobuf::Message *resp_msg) {
                                       (void)controller;
                                       (void)response;
                                       const auto end = std::chrono::steady_clock::now();
                                       const double latency_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
                                       RecordLatency(metrics, latency_ms);

                                       auto *resp = static_cast<Kuser::LoginResponse *>(resp_msg);
                                       if (IsSuccess(*resp, ctrl)) {
                                           metrics.success.fetch_add(1);
                                       } else {
                                           metrics.fail.fetch_add(1);
                                       }
                                       try {
                                           promise_ptr->set_value();
                                       } catch (const std::future_error &) {
                                       }
                                   });

                future.wait();
                if (opts.sleep_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(opts.sleep_ms));
                }
            }
        });
    }

    for (auto &th : threads) {
        th.join();
    }
}

// callback 模式：回调中直接统计并通过 promise 通知完成，主线程收集所有 future 等待
// 使用 shared_future 让同一个 future 可被多个等待者读取，这里由主线程收集统一 wait
void RunCallbackMode(KrpcChannel *channel,
                     const ::google::protobuf::MethodDescriptor *method,
                     const Options &opts,
                     Metrics &metrics) {
    std::vector<std::shared_future<void>> waiters;
    waiters.reserve(opts.requests);
    std::mutex waiters_mutex;

    const int base = opts.requests / opts.concurrency;
    const int extra = opts.requests % opts.concurrency;
    std::vector<std::thread> threads;
    threads.reserve(opts.concurrency);

    for (int t = 0; t < opts.concurrency; ++t) {
        const int count = base + (t < extra ? 1 : 0);
        if (count <= 0) {
            continue;
        }
        threads.emplace_back([channel, method, count, &opts, &metrics, &waiters, &waiters_mutex]() {
            for (int i = 0; i < count; ++i) {
                Kuser::LoginRequest request;
                request.set_name("async_callback");
                request.set_pwd("123456");

                auto controller = std::make_shared<Krpccontroller>();
                if (opts.timeout_ms > 0) {
                    controller->SetTimeoutMs(opts.timeout_ms);
                }
                auto response = std::make_shared<Kuser::LoginResponse>();
                auto promise_ptr = std::make_shared<std::promise<void>>();
                const auto start = std::chrono::steady_clock::now();

                {
                    std::lock_guard<std::mutex> lock(waiters_mutex);
                    waiters.push_back(promise_ptr->get_future().share());
                }

                channel->CallAsync(method,
                                   controller.get(),
                                   &request,
                                   response.get(),
                                   [controller, response, promise_ptr, start, &metrics](::google::protobuf::RpcController *ctrl, ::google::protobuf::Message *resp_msg) {
                                       (void)controller;
                                       (void)response;
                                       const auto end = std::chrono::steady_clock::now();
                                       const double latency_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
                                       RecordLatency(metrics, latency_ms);

                                       auto *resp = static_cast<Kuser::LoginResponse *>(resp_msg);
                                       if (IsSuccess(*resp, ctrl)) {
                                           metrics.success.fetch_add(1);
                                       } else {
                                           metrics.fail.fetch_add(1);
                                       }
                                       try {
                                           promise_ptr->set_value();
                                       } catch (const std::future_error &) {
                                       }
                                   });

                if (opts.sleep_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(opts.sleep_ms));
                }
            }
        });
    }

    for (auto &th : threads) {
        th.join();
    }

    for (auto &f : waiters) {
        f.wait();
    }
}

} // namespace

int main(int argc, char **argv) {
    // 初始化框架（解析 -i 配置），其余参数走环境变量
    // KrpcApplication::Init 负责加载 test.conf 并初始化 ZK/日志等全局单例
    KrpcApplication::Init(argc, argv);

    const Options opts = LoadOptions();
    std::cout << "async demo starting, mode=" << opts.mode
              << " ASYNC_CONCURRENCY=" << opts.concurrency
              << " ASYNC_REQUESTS=" << opts.requests
              << " ASYNC_TIMEOUT_MS=" << opts.timeout_ms << std::endl;

    auto channel = std::make_shared<KrpcChannel>(false);
    const auto *method = Kuser::UserServiceRpc::descriptor()->FindMethodByName("Login");
    if (method == nullptr) {
        std::cerr << "method Login not found" << std::endl;
        return 1;
    }

    Metrics metrics;
    const auto start = std::chrono::steady_clock::now();
    if (opts.mode == "callback") {
        RunCallbackMode(channel.get(), method, opts, metrics);
    } else {
        RunFutureMode(channel.get(), method, opts, metrics);
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

    PrintSummary(opts, metrics);
    std::cout << "elapsed_sec=" << std::fixed << std::setprecision(3) << elapsed << std::endl;
    return metrics.fail.load() == 0 ? 0 : 1;
}
