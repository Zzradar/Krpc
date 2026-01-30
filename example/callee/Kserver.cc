#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "Krpcapplication.h"
#include "Krpcprovider.h"
#include "Krpcmsgpack_provider.h"
#include "../user.pb.h"
#include "../common/codec_util.h"
#include "../common/user_logic.h"

/*
UserService 原本是一个本地服务，提供了两个本地方法：Login 和 GetFriendLists。
现在通过 RPC 框架，这些方法可以被远程调用。
*/
class UserService : public Kuser::UserServiceRpc // 继承自 protobuf 生成的 RPC 服务基类
{
public:
    // 本地登录方法，用于处理实际的业务逻辑
    bool Login(std::string name, std::string pwd) {
        // 避免频繁的标准输出阻塞，实际业务只返回成功
        (void)pwd;
        return true;  // 模拟登录成功
    }

    /*
    重写基类 UserServiceRpc 的虚函数，这些方法会被 RPC 框架直接调用。
    1. 调用者（caller）通过 RPC 框架发送 Login 请求。
    2. 服务提供者（callee）接收到请求后，调用下面重写的 Login 方法。
    */
    void Login(::google::protobuf::RpcController* controller,
              const ::Kuser::LoginRequest* request,
              ::Kuser::LoginResponse* response,
              ::google::protobuf::Closure* done) {
        // 从请求中获取用户名和密码
        std::string name = request->name();
        std::string pwd = request->pwd();

        auto result = KrpcHandleLogin(name, pwd);

        // 将响应结果写入 response 对象
        Kuser::ResultCode *code = response->mutable_result();
        code->set_errcode(result.errcode);  // 设置错误码
        code->set_errmsg(result.errmsg);  // 设置错误信息
        response->set_success(result.success);  // 设置登录结果

        // 执行回调操作，框架会自动将响应序列化并发送给调用者
        done->Run();
    }

    void Register(::google::protobuf::RpcController* controller,
              const ::Kuser::RegisterRequest* request,
              ::Kuser::RegisterResponse* response,
              ::google::protobuf::Closure* done) override {
        (void)controller;
        auto result = KrpcHandleRegister(request->id(), request->name(), request->pwd());
        Kuser::ResultCode *code = response->mutable_result();
        code->set_errcode(result.errcode);
        code->set_errmsg(result.errmsg);
        response->set_success(result.success);
        done->Run();
    }
};

int main(int argc, char **argv) {
    // 调用框架的初始化操作，解析命令行参数并加载配置文件
    KrpcApplication::Init(argc, argv);

    if (KrpcUseMsgpack()) {
        KrpcMsgpackProvider provider;
        provider.Bind("UserServiceRpc", "Login", [](const std::string &name, const std::string &pwd) {
            return KrpcHandleLogin(name, pwd);
        });
        provider.Bind("UserServiceRpc", "Register", [](uint32_t id, const std::string &name, const std::string &pwd) {
            return KrpcHandleRegister(id, name, pwd);
        });
        provider.Run();
        return 0;
    }

    // protobuf 默认路径
    KrpcProvider provider;
    provider.NotifyService(new UserService());
    provider.Run();

    return 0;
}
