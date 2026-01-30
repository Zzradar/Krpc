#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "Krpcmsgpack_channel.h"
#include "../user.pb.h"
#include "../common/codec_util.h"
#include "../common/user_types.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

namespace {

int ParseEnvInt(const char *name, int default_value) {
    if (const char *value = std::getenv(name)) {
        try {
            return std::stoi(value);
        } catch (...) {
            return default_value;
        }
    }
    return default_value;
}

bool InvokeLogin(Kuser::UserServiceRpc_Stub &stub, const std::string &name) {
    Kuser::LoginRequest request;
    request.set_name(name);
    request.set_pwd("123456");

    Kuser::LoginResponse response;
    Krpccontroller controller;
    stub.Login(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        std::cout << "login request (name=" << name << ") failed: "
                  << controller.ErrorText() << std::endl;
        return false;
    }

    std::cout << "login request (name=" << name << ") success="
              << std::boolalpha << response.success() << std::endl;
    return true;
}

bool InvokeLoginMsgpack(KrpcMsgpackChannel &channel, const std::string &name) {
    try {
        auto result = channel.Call<MsgpackUserResult>("UserServiceRpc", "Login",
                                                      name, std::string("123456"));
        std::cout << "login request (name=" << name << ") success="
                  << std::boolalpha << result.success << std::endl;
        return result.success;
    } catch (const std::exception &e) {
        std::cout << "login request (name=" << name << ") failed: "
                  << e.what() << std::endl;
        return false;
    }
}

} // namespace

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    const int idle_seconds = ParseEnvInt("HEARTBEAT_IDLE_SECONDS", 15);
    const int rounds = ParseEnvInt("HEARTBEAT_IDLE_ROUNDS", 4);

    std::cout << "Heartbeat demo using idle_seconds=" << idle_seconds
              << ", rounds=" << rounds << std::endl;

    const bool use_msgpack = KrpcUseMsgpack();
    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false));
    KrpcMsgpackChannel msgpack_channel;

    for (int round = 0; round < rounds; ++round) {
        std::cout << "[round " << (round + 1) << "/" << rounds
                  << "] invoking Login" << std::endl;
        bool ok = false;
        if (use_msgpack) {
            ok = InvokeLoginMsgpack(msgpack_channel, "zhangsan");
        } else {
            ok = InvokeLogin(stub, "zhangsan");
        }
        if (!ok) return EXIT_FAILURE;

        if (round == rounds - 1) {
            break;
        }

        std::cout << "[round " << (round + 1) << "] idle for "
                  << idle_seconds << "s to let heartbeat run..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(idle_seconds));
    }

    std::cout << "Heartbeat stability demo finished successfully." << std::endl;
    return EXIT_SUCCESS;
}
