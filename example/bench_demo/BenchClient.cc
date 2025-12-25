#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "../user.pb.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <google/protobuf/stubs/common.h>

namespace {

std::string ReadEnvString(const char *key, const std::string &fallback) {
    const char *val = std::getenv(key);
    if (!val) {
        return fallback;
    }
    return std::string(val);
}

int ReadEnvInt(const char *key, int fallback) {
    const char *val = std::getenv(key);
    if (!val) {
        return fallback;
    }
    try {
        return std::stoi(val);
    } catch (...) {
        return fallback;
    }
}

struct BenchConfig {
    std::string mode;       // sync | async
    std::string conn_mode;  // keepalive | short
    int concurrency{4};
    int total_requests{100};
    int sleep_ms{0};
};

struct Metrics {
    std::atomic<int> success{0};
    std::atomic<int> fail{0};
    std::mutex mu;
    std::vector<int64_t> costs;
};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Record(Metrics &m, bool ok, int64_t cost_ms) {
    if (ok) {
        m.success.fetch_add(1, std::memory_order_relaxed);
    } else {
        m.fail.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(m.mu);
    m.costs.push_back(cost_ms);
}

void PrintSummary(const BenchConfig &cfg, Metrics &m, int64_t total_cost_ms) {
    const int succ = m.success.load();
    const int fail = m.fail.load();
    const int total = succ + fail;
    std::vector<int64_t> samples;
    {
        std::lock_guard<std::mutex> lock(m.mu);
        samples = m.costs;
    }
    std::sort(samples.begin(), samples.end());
    auto perc = [&](double p) -> int64_t {
        if (samples.empty()) return 0;
        const size_t idx = static_cast<size_t>((p / 100.0) * samples.size() + 0.5);
        return samples[std::min(idx, samples.size() - 1)];
    };
    double qps = total_cost_ms > 0 ? (static_cast<double>(total) * 1000.0 / total_cost_ms) : 0.0;
    std::cout << "=== bench summary ===" << std::endl;
    std::cout << "mode=" << cfg.mode << " conn=" << cfg.conn_mode
              << " concurrency=" << cfg.concurrency
              << " requests=" << total << std::endl;
    std::cout << "success=" << succ << " fail=" << fail
              << " qps=" << qps
              << " total_cost_ms=" << total_cost_ms << std::endl;
    if (!samples.empty()) {
        std::cout << "latency_ms p50=" << perc(50) << " p95=" << perc(95)
                  << " p99=" << perc(99)
                  << " max=" << samples.back() << std::endl;
    }
}

struct AsyncContext {
    std::shared_ptr<KrpcChannel> channel;
    std::unique_ptr<Kuser::UserServiceRpc_Stub> stub;
};

struct AsyncCallCtx {
    int64_t start_ms{0};
    std::shared_ptr<Krpccontroller> controller;
    std::shared_ptr<Kuser::LoginResponse> response;
    std::shared_ptr<AsyncContext> ctx_holder;
    Metrics *metrics{nullptr};
    std::atomic<int> *pending{nullptr};
};

void OnAsyncDone(AsyncCallCtx *ctx) {
    const auto cost = NowMs() - ctx->start_ms;
    const bool ok = !ctx->controller->Failed();
    Record(*ctx->metrics, ok, cost);
    ctx->pending->fetch_sub(1);
    delete ctx;
}

} // namespace

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    BenchConfig cfg;
    cfg.mode = ReadEnvString("BENCH_MODE", "sync");           // sync | async
    cfg.conn_mode = ReadEnvString("BENCH_CONN", "keepalive"); // keepalive | short
    cfg.concurrency = ReadEnvInt("BENCH_CONCURRENCY", 4);
    cfg.total_requests = ReadEnvInt("BENCH_REQUESTS", 100);
    cfg.sleep_ms = ReadEnvInt("BENCH_SLEEP_MS", 0);

    std::cout << "bench mode=" << cfg.mode
              << " conn=" << cfg.conn_mode
              << " concurrency=" << cfg.concurrency
              << " total_requests=" << cfg.total_requests
              << " sleep_ms=" << cfg.sleep_ms
              << std::endl;

    Metrics metrics;
    std::atomic<int> pending{cfg.total_requests};

    const int64_t start_ts = NowMs();
    auto worker = [&](int tid) {
        std::unique_ptr<KrpcChannel> sync_channel;
        std::unique_ptr<Kuser::UserServiceRpc_Stub> sync_stub;
        std::shared_ptr<AsyncContext> async_ctx;
        if (cfg.conn_mode == "keepalive") {
            if (cfg.mode == "sync") {
                sync_channel.reset(new KrpcChannel(false));
                sync_stub.reset(new Kuser::UserServiceRpc_Stub(sync_channel.get()));
            } else {
                async_ctx = std::make_shared<AsyncContext>();
                async_ctx->channel = std::make_shared<KrpcChannel>(false);
                async_ctx->stub.reset(new Kuser::UserServiceRpc_Stub(async_ctx->channel.get()));
            }
        }

        for (int i = tid; i < cfg.total_requests; i += cfg.concurrency) {
            Kuser::LoginRequest request;
            request.set_name("bench");
            request.set_pwd("123456");

            if (cfg.mode == "sync") {
                std::unique_ptr<KrpcChannel> short_channel;
                std::unique_ptr<Kuser::UserServiceRpc_Stub> short_stub;
                if (cfg.conn_mode == "keepalive") {
                    // keepalive reuse sync_channel/sync_stub
                } else {
                    short_channel.reset(new KrpcChannel(false));
                    short_stub.reset(new Kuser::UserServiceRpc_Stub(short_channel.get()));
                }
                Kuser::LoginResponse response;
                Krpccontroller controller;
                auto t0 = std::chrono::steady_clock::now();
                Kuser::UserServiceRpc_Stub *stub_ptr = cfg.conn_mode == "keepalive"
                                                       ? sync_stub.get()
                                                       : short_stub.get();
                stub_ptr->Login(&controller, &request, &response, nullptr);
                auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                if (controller.Failed()) {
                    Record(metrics, false, cost);
                } else {
                    Record(metrics, true, cost);
                }
                pending.fetch_sub(1, std::memory_order_release);
                if (cfg.sleep_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.sleep_ms));
                }
                continue;
            }

            // async 模式
            std::shared_ptr<AsyncContext> ctx = async_ctx;
            if (cfg.conn_mode == "short") {
                ctx = std::make_shared<AsyncContext>();
                ctx->channel = std::make_shared<KrpcChannel>(false);
                ctx->stub.reset(new Kuser::UserServiceRpc_Stub(ctx->channel.get()));
            }

            auto *call_ctx = new AsyncCallCtx();
            call_ctx->start_ms = NowMs();
            call_ctx->controller = std::make_shared<Krpccontroller>();
            call_ctx->response = std::make_shared<Kuser::LoginResponse>();
            call_ctx->ctx_holder = ctx;
            call_ctx->metrics = &metrics;
            call_ctx->pending = &pending;
            auto cb = google::protobuf::NewCallback(&OnAsyncDone, call_ctx);
            ctx->stub->Login(call_ctx->controller.get(), &request, call_ctx->response.get(), cb);
            if (cfg.sleep_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg.sleep_ms));
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(cfg.concurrency);
    for (int i = 0; i < cfg.concurrency; ++i) {
        threads.emplace_back(worker, i);
    }

    while (pending.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto &t : threads) {
        if (t.joinable()) t.join();
    }
    const int64_t total_cost = NowMs() - start_ts;
    PrintSummary(cfg, metrics, total_cost);
    return 0;
}
