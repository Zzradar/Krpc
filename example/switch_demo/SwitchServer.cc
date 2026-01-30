#include "Krpcapplication.h"
#include "Krpcmsgpack_provider.h"
#include "Krpcprovider.h"
#include "../user.pb.h"
#include "user_types.h"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <string>
#include <thread>

namespace {

bool ShouldSleep(const std::string &name) {
    return name == "sleep" || name == "timeout";
}

void MaybeSleep(const std::string &name) {
    if (ShouldSleep(name)) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

MsgpackUserResult HandleLogin(const std::string &name, const std::string &pwd) {
    (void)pwd;
    MaybeSleep(name);
    MsgpackUserResult result;
    result.success = true;
    return result;
}

MsgpackUserResult HandleRegister(uint32_t id, const std::string &name, const std::string &pwd) {
    (void)id;
    (void)pwd;
    MaybeSleep(name);
    MsgpackUserResult result;
    result.success = true;
    return result;
}

class UserService : public Kuser::UserServiceRpc {
public:
    void Login(::google::protobuf::RpcController *controller,
               const ::Kuser::LoginRequest *request,
               ::Kuser::LoginResponse *response,
               ::google::protobuf::Closure *done) override {
        (void)controller;
        auto result = HandleLogin(request->name(), request->pwd());
        auto *code = response->mutable_result();
        code->set_errcode(result.errcode);
        code->set_errmsg(result.errmsg);
        response->set_success(result.success);
        done->Run();
    }

    void Register(::google::protobuf::RpcController *controller,
                  const ::Kuser::RegisterRequest *request,
                  ::Kuser::RegisterResponse *response,
                  ::google::protobuf::Closure *done) override {
        (void)controller;
        auto result = HandleRegister(request->id(), request->name(), request->pwd());
        auto *code = response->mutable_result();
        code->set_errcode(result.errcode);
        code->set_errmsg(result.errmsg);
        response->set_success(result.success);
        done->Run();
    }
};

std::string LoadCodec() {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    std::string codec = config.Load("rpc_codec");
    for (auto &ch : codec) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return codec;
}

} // namespace

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    const std::string codec = LoadCodec();
    if (codec == "msgpack") {
        KrpcMsgpackProvider provider;
        provider.Bind("UserServiceRpc", "Login", [](const std::string &name, const std::string &pwd) {
            return HandleLogin(name, pwd);
        });
        provider.Bind("UserServiceRpc", "Register", [](uint32_t id, const std::string &name, const std::string &pwd) {
            return HandleRegister(id, name, pwd);
        });
        provider.Run();
        return 0;
    }

    KrpcProvider provider;
    provider.NotifyService(new UserService());
    provider.Run();
    return 0;
}
