#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "Krpcmsgpack_channel.h"
#include "../user.pb.h"
#include "user_types.h"

#include <cctype>
#include <exception>
#include <iostream>
#include <string>

namespace {

std::string LoadCodec() {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    std::string codec = config.Load("rpc_codec");
    for (auto &ch : codec) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return codec;
}

void PrintMsgpackResult(const std::string &label, const MsgpackUserResult &result) {
    std::cout << label << " errcode=" << result.errcode
              << " success=" << std::boolalpha << result.success
              << " errmsg=" << result.errmsg << std::endl;
}

void RunMsgpackClient() {
    KrpcMsgpackChannel channel;
    auto login = channel.Call<MsgpackUserResult>("UserServiceRpc", "Login",
                                                 std::string("zhangsan"),
                                                 std::string("123456"));
    PrintMsgpackResult("msgpack login", login);

    auto reg = channel.Call<MsgpackUserResult>("UserServiceRpc", "Register",
                                               static_cast<uint32_t>(1),
                                               std::string("lisi"),
                                               std::string("123456"));
    PrintMsgpackResult("msgpack register", reg);
}

void RunProtobufClient() {
    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false));

    Kuser::LoginRequest login_req;
    login_req.set_name("zhangsan");
    login_req.set_pwd("123456");
    Kuser::LoginResponse login_resp;
    Krpccontroller login_ctl;
    stub.Login(&login_ctl, &login_req, &login_resp, nullptr);
    if (login_ctl.Failed()) {
        std::cout << "protobuf login failed: " << login_ctl.ErrorText() << std::endl;
    } else {
        std::cout << "protobuf login success: " << std::boolalpha << login_resp.success() << std::endl;
    }

    Kuser::RegisterRequest reg_req;
    reg_req.set_id(1);
    reg_req.set_name("lisi");
    reg_req.set_pwd("123456");
    Kuser::RegisterResponse reg_resp;
    Krpccontroller reg_ctl;
    stub.Register(&reg_ctl, &reg_req, &reg_resp, nullptr);
    if (reg_ctl.Failed()) {
        std::cout << "protobuf register failed: " << reg_ctl.ErrorText() << std::endl;
    } else {
        std::cout << "protobuf register success: " << std::boolalpha << reg_resp.success() << std::endl;
    }
}

} // namespace

int main(int argc, char **argv) {
    KrpcApplication::Init(argc, argv);

    const std::string codec = LoadCodec();
    try {
        if (codec == "msgpack") {
            RunMsgpackClient();
        } else {
            RunProtobufClient();
        }
    } catch (const std::exception &e) {
        std::cout << "client failed: " << e.what() << std::endl;
    }
    return 0;
}
