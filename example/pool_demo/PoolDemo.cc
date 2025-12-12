#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "../user.pb.h"
#include <chrono>
#include <iostream>
#include <string>

namespace {

std::string ReadEnvString(const char *key, const std::string &fallback) {
    const char *val = std::getenv(key);
    if (!val) {
        return fallback;
    }
    return std::string(val);
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
    std::cout << "pool demo mode=" << mode << std::endl;

    std::shared_ptr<Kuser::UserServiceRpc_Stub> shared_stub;
    if (mode == "single") {
        shared_stub.reset(new Kuser::UserServiceRpc_Stub(new KrpcChannel(false)));
    }

    const int iterations = 10; // 调用次数
    for (int i = 0; i < iterations; ++i) {
        // 每次新建 Channel 的模式用于观察连接池复用：开启池时第二次起应看到“reuse pooled connection”
        // 注意 stub 默认不拥有 Channel，需要手动释放
        std::unique_ptr<KrpcChannel> owned_channel;
        if (mode == "new_channel") {
            owned_channel.reset(new KrpcChannel(false));
            shared_stub.reset(new Kuser::UserServiceRpc_Stub(owned_channel.get()));
        }

        Kuser::LoginRequest request;
        request.set_name("pool_demo");
        request.set_pwd("123456");

        Kuser::LoginResponse response;
        Krpccontroller controller;

        auto start = std::chrono::steady_clock::now();
        shared_stub->Login(&controller, &request, &response, nullptr);
        auto end = std::chrono::steady_clock::now();
        auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        if (controller.Failed()) {
            std::cout << "[" << i << "] call failed: " << controller.ErrorText() << std::endl;
        } else {
            std::cout << "[" << i << "] call ok, success=" << std::boolalpha << response.success()
                      << " cost_ms=" << cost_ms << std::endl;
        }
        // new_channel 模式下手动销毁 Channel，确保连接归还到池
        owned_channel.reset();
    }
    return 0;
}
