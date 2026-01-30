#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "Krpccontroller.h"
#include "Krpcmsgpack_channel.h"
#include "KrpcLogger.h"
#include "../user.pb.h"
#include "../common/codec_util.h"
#include "../common/user_types.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

// 发送 RPC 请求的函数，模拟客户端调用远程服务
void send_request_protobuf(int thread_id,
                  std::atomic<int> &success_count,
                  std::atomic<int> &fail_count,
                  int requests_per_thread,
                  std::map<std::string, int> &error_stats,
                  std::mutex &error_mutex) {
    // 创建一个 UserServiceRpc_Stub 对象，用于调用远程的 RPC 方法
    Kuser::UserServiceRpc_Stub stub(new KrpcChannel(false));

    // 设置 RPC 方法的请求参数
    Kuser::LoginRequest request;
    request.set_name("zhangsan");  // 设置用户名
    request.set_pwd("123456");    // 设置密码

    // 定义 RPC 方法的响应参数
    Kuser::LoginResponse response;
    Krpccontroller controller;  // 创建控制器对象，用于处理 RPC 调用过程中的错误
    for (int i = 0; i < requests_per_thread; ++i) {
        controller.Reset();
        // 调用远程的 Login 方法
        stub.Login(&controller, &request, &response, nullptr);

        // 检查 RPC 调用是否成功
        if (controller.Failed()) {  // 如果调用失败
            {
                std::lock_guard<std::mutex> lock(error_mutex);
                ++error_stats[controller.ErrorText()];
            }
            fail_count++;  // 失败计数加 1
        } else {  // 如果调用成功
            if (int{} == response.result().errcode()) {  // 检查响应中的错误码
                success_count++;  // 成功计数加 1
            } else {  // 如果响应中有错误
                {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    ++error_stats[response.result().errmsg()];
                }
                fail_count++;  // 失败计数加 1
            }
        }
    }
}

void send_request_msgpack(int thread_id,
                  std::atomic<int> &success_count,
                  std::atomic<int> &fail_count,
                  int requests_per_thread,
                  std::map<std::string, int> &error_stats,
                  std::mutex &error_mutex) {
    (void)thread_id;
    KrpcMsgpackChannel channel;
    for (int i = 0; i < requests_per_thread; ++i) {
        try {
            auto result = channel.Call<MsgpackUserResult>("UserServiceRpc", "Login",
                                                          std::string("zhangsan"),
                                                          std::string("123456"));
            if (result.errcode == 0) {
                success_count++;
            } else {
                {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    ++error_stats[result.errmsg];
                }
                fail_count++;
            }
        } catch (const std::exception &e) {
            {
                std::lock_guard<std::mutex> lock(error_mutex);
                ++error_stats[e.what()];
            }
            fail_count++;
        }
    }
}

int main(int argc, char **argv) {
    // 初始化 RPC 框架，解析命令行参数并加载配置文件
    KrpcApplication::Init(argc, argv);

    // 创建日志对象
    KrpcLogger logger("MyRPC");

    const int thread_count =20;       // 并发线程数
    const int requests_per_thread = 2000; // 每个线程发送的请求数

    std::vector<std::thread> threads;  // 存储线程对象的容器
    std::atomic<int> success_count(0); // 成功请求的计数器
    std::atomic<int> fail_count(0);    // 失败请求的计数器
    std::map<std::string, int> error_stats;
    std::mutex error_mutex;

    auto start_time = std::chrono::high_resolution_clock::now();  // 记录测试开始时间

    const bool use_msgpack = KrpcUseMsgpack();

    // 启动多线程进行并发测试
    for (int i = 0; i < thread_count; i++) {
        threads.emplace_back([argc, argv, i, use_msgpack, &success_count, &fail_count, requests_per_thread, &error_stats, &error_mutex]() {
            (void)argc;
            (void)argv;
            if (use_msgpack) {
                send_request_msgpack(i, success_count, fail_count, requests_per_thread, error_stats, error_mutex);
            } else {
                send_request_protobuf(i, success_count, fail_count, requests_per_thread, error_stats, error_mutex);
            }
        });
    }

    // 等待所有线程执行完毕
    for (auto &t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();  // 记录测试结束时间
    std::chrono::duration<double> elapsed = end_time - start_time;  // 计算测试耗时

    // 输出统计结果
    LOG(INFO) << "Total requests: " << thread_count * requests_per_thread;  // 总请求数
    LOG(INFO) << "Success count: " << success_count;  // 成功请求数
    LOG(INFO) << "Fail count: " << fail_count;  // 失败请求数
    LOG(INFO) << "Elapsed time: " << elapsed.count() << " seconds";  // 测试耗时
    LOG(INFO) << "QPS: " << (thread_count * requests_per_thread) / elapsed.count();  // 计算 QPS（每秒请求数）

    if (!error_stats.empty()) {
        LOG(WARNING) << "Failure reasons:";
        for (const auto &entry : error_stats) {
            LOG(WARNING) << "  '" << entry.first << "' => " << entry.second;
        }
    }

    return 0;
}
