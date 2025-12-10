#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "../user.pb.h"

#include <chrono>
#include <cstdlib>
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

} // namespace

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    const int idle_seconds = ParseEnvInt("HEARTBEAT_IDLE_SECONDS", 15);
    const int rounds = ParseEnvInt("HEARTBEAT_IDLE_ROUNDS", 4);

    std::cout << "Heartbeat demo using idle_seconds=" << idle_seconds
              << ", rounds=" << rounds << std::endl;

    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false));

    for (int round = 0; round < rounds; ++round) {
        std::cout << "[round " << (round + 1) << "/" << rounds
                  << "] invoking Login" << std::endl;
        if (!InvokeLogin(stub, "zhangsan")) {
            return EXIT_FAILURE;
        }

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
