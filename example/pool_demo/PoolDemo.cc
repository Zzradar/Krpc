#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "Krpcmsgpack_channel.h"
#include "../user.pb.h"
#include "../common/codec_util.h"
#include "../common/user_types.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

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

}

/*
 * 目的：验证“单进程多次创建 Channel”时连接池是否复用连接。
 * 运行前请启动 server： ./bin/server -i ./bin/test.conf
 * 打开/关闭连接池可在 bin/test.conf 设置 enable_connection_pool=1/0。
 *
 * 预期：
 * - 开启连接池：日志中首次会看到 "connect server success"，后续大多为 "reuse pooled connection"。
 * - 关闭连接池：每次新建 Channel 都会打印 "connect server success"。
 */
int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    // 运行模式：single（单 Channel 复用）/ new_channel（每次新建 Channel）
    const std::string mode = ReadEnvString("POOL_DEMO_MODE", "single");
    const int iterations = ReadEnvInt("POOL_DEMO_ITERATIONS", 10);
    const int sleep_ms = ReadEnvInt("POOL_DEMO_SLEEP_MS", 0); // 每次调用间隔
    const int idle_ms = ReadEnvInt("POOL_DEMO_IDLE_MS", 0);   // 中途空闲等待，用于触发服务端踢除或测试出池校验
    int idle_at = ReadEnvInt("POOL_DEMO_IDLE_AT", iterations / 2); // 在第 idle_at 次调用后空闲
    if (idle_at < 0) idle_at = 0;
    if (idle_at > iterations) idle_at = iterations;
    std::cout << "pool demo mode=" << mode << std::endl;
    std::cout << "iterations=" << iterations
              << " sleep_ms=" << sleep_ms
              << " idle_ms=" << idle_ms
              << " idle_at=" << idle_at << std::endl;

    const bool use_msgpack = KrpcUseMsgpack();
    std::shared_ptr<Kuser::UserServiceRpc_Stub> shared_stub;
    std::shared_ptr<KrpcMsgpackChannel> shared_msgpack;
    if (mode == "single") {
        if (use_msgpack) {
            shared_msgpack = std::make_shared<KrpcMsgpackChannel>();
        } else {
            shared_stub.reset(new Kuser::UserServiceRpc_Stub(new KrpcChannel(false)));
        }
    }

    int ok = 0, fail = 0;
    for (int i = 0; i < iterations; ++i) {
        // 每次新建 Channel 的模式用于观察连接池复用：开启池时第二次起应看到“reuse pooled connection”
        // 注意 stub 默认不拥有 Channel，需要手动释放
        std::unique_ptr<KrpcChannel> owned_channel;
        std::unique_ptr<KrpcMsgpackChannel> owned_msgpack;
        if (mode == "new_channel") {
            if (use_msgpack) {
                owned_msgpack.reset(new KrpcMsgpackChannel());
            } else {
                owned_channel.reset(new KrpcChannel(false));
                shared_stub.reset(new Kuser::UserServiceRpc_Stub(owned_channel.get()));
            }
        }

        auto start = std::chrono::steady_clock::now();
        bool call_ok = false;
        if (use_msgpack) {
            KrpcMsgpackChannel *channel_ptr = shared_msgpack ? shared_msgpack.get() : owned_msgpack.get();
            try {
                auto result = channel_ptr->Call<MsgpackUserResult>("UserServiceRpc", "Login",
                                                                  std::string("pool_demo"),
                                                                  std::string("123456"));
                call_ok = result.success;
            } catch (const std::exception &e) {
                std::cout << "[" << i << "] call failed: " << e.what() << std::endl;
                call_ok = false;
            }
        } else {
            Kuser::LoginRequest request;
            request.set_name("pool_demo");
            request.set_pwd("123456");
            Kuser::LoginResponse response;
            Krpccontroller controller;
            shared_stub->Login(&controller, &request, &response, nullptr);
            call_ok = !controller.Failed() && response.success();
            if (!call_ok && controller.Failed()) {
                std::cout << "[" << i << "] call failed: " << controller.ErrorText() << std::endl;
            }
        }

        auto end = std::chrono::steady_clock::now();
        auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        if (call_ok) {
            std::cout << "[" << i << "] call ok, success=true"
                      << " cost_ms=" << cost_ms << std::endl;
            ++ok;
        } else {
            ++fail;
        }

        // new_channel 模式下手动销毁 Channel，确保连接归还到池
        owned_channel.reset();
        owned_msgpack.reset();

        if (idle_ms > 0 && i + 1 == idle_at) {
            std::cout << "idle pause " << idle_ms << " ms to test pooled fd health..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
        }
        if (sleep_ms > 0 && i + 1 < iterations) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
    std::cout << "summary: ok=" << ok << " fail=" << fail << std::endl;
    return 0;
}
