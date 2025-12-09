#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "Krpcprotocol.h"
#include "../user.pb.h"
#include <iostream>

namespace {

void InvokeLogin(Kuser::UserServiceRpc_Stub &stub,
                 const std::string &name,
                 int timeout_ms) {
    Kuser::LoginRequest request;
    request.set_name(name);
    request.set_pwd("123456");

    Kuser::LoginResponse response;
    Krpccontroller controller;
    if (timeout_ms > 0) {
        controller.SetTimeoutMs(timeout_ms);
    }

    stub.Login(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        std::cout << "request name='" << name << "' failed: "
                  << controller.ErrorText() << std::endl;
        return;
    }

    std::cout << "request name='" << name << "' success: "
              << std::boolalpha << response.success() << std::endl;
}

}

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    // 使用延迟连接，第一次调用时再去 ZooKeeper 查询并建立连接。
    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false));

    std::cout << "===== normal call =====" << std::endl;
    InvokeLogin(stub, "zhangsan", 0);

    std::cout << "===== timeout call =====" << std::endl;
    // 让服务端休眠 3 秒，但只给 1 秒超时，观察超时错误。
    InvokeLogin(stub, "sleep", 1000);

    return 0;
}
