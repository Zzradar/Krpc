#ifndef _Krpcchannel_h_
#define _Krpcchannel_h_
// 此类是继承自google::protobuf::RpcChannel
// 目的是为了给客户端进行方法调用的时候，统一接收的
#include <google/protobuf/service.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <future>
#include <unordered_map>
#include <memory>
#include <functional>
#include <queue>
#include "zookeeperutil.h"
#include "Krpcheader.pb.h"
class KrpcChannel : public google::protobuf::RpcChannel
{
public:
    KrpcChannel(bool connectNow);
    virtual ~KrpcChannel();
    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override; // override可以验证是否是虚函数
    using AsyncCallback = std::function<void(::google::protobuf::RpcController *, ::google::protobuf::Message *)>;
    void CallAsync(const ::google::protobuf::MethodDescriptor *method,
                   ::google::protobuf::RpcController *controller,
                   const ::google::protobuf::Message *request,
                   ::google::protobuf::Message *response,
                   AsyncCallback callback);
private:
    int m_clientfd; // 存放客户端套接字
    std::string m_ip;
    uint16_t m_port;
    int m_idx; // 用来划分服务器ip和port的下标
    int m_request_timeout_ms;
    int m_heartbeat_interval_ms;
    int m_heartbeat_miss_limit;
    std::thread m_heartbeat_thread;
    std::atomic<bool> m_heartbeat_running;
    bool m_heartbeat_thread_started;
    std::condition_variable m_heartbeat_cv;
    std::mutex m_heartbeat_mutex;
    std::mutex m_socket_mutex;
    int m_missed_heartbeat_count;
    std::chrono::steady_clock::time_point m_last_pong_time;
    struct PendingCall {
        google::protobuf::Message *response{nullptr};
        ::google::protobuf::RpcController *controller{nullptr};
        std::promise<void> promise;
        std::shared_future<void> completion_future;
        int timeout_ms{0};
        std::chrono::steady_clock::time_point start_time;
        uint64_t request_id{0};
        ::google::protobuf::Closure *done{nullptr};
        AsyncCallback async_callback;
    };
    std::mutex m_pending_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<PendingCall>> m_pending_calls;
    std::thread m_recv_thread;
    std::atomic<bool> m_recv_running;
    bool m_recv_thread_started;
    std::mutex m_recv_mutex;
    std::condition_variable m_recv_cv;
    std::string m_recv_buffer;
    struct SendTask {
        uint64_t request_id{0};
        std::string buffer;
    };
    std::queue<SendTask> m_send_queue;
    std::mutex m_send_mutex;
    std::condition_variable m_send_cv;
    std::thread m_send_thread;
    std::atomic<bool> m_send_running;
    bool m_send_thread_started;
    std::mutex m_heartbeat_wait_mutex;
    std::condition_variable m_heartbeat_wait_cv;
    bool m_waiting_heartbeat;
    uint64_t m_waiting_heartbeat_id;
    bool m_waiting_heartbeat_result;
    std::thread m_timeout_thread;
    std::atomic<bool> m_timeout_running;
    bool m_timeout_thread_started;
    std::condition_variable m_timeout_cv;
    std::mutex m_timeout_mutex;
    bool newConnect(const char *ip, uint16_t port, std::string *errMsg = nullptr);
    std::string QueryServiceHost(ZkClient *zkclient, std::string service_name, std::string method_name, int &idx);
    void StartHeartbeatThread();
    void StopHeartbeatThread();
    void HeartbeatLoop();
    enum class HeartbeatResult {
        kSuccess,
        kTimeout,
        kFatal
    };
    HeartbeatResult SendHeartbeatOnce();
    void HandleHeartbeatFailure(const std::string &reason);
    void StartSendThread();
    void StopSendThread();
    void SendLoop();
    void StartRecvThread();
    void StopRecvThread();
    void RecvLoop();
    void StartTimeoutThread();
    void StopTimeoutThread();
    void TimeoutLoop();
    void FailPendingCalls(const std::string &reason);
    void ResolvePendingCall(const Krpc::RpcHeader &header, const std::string &payload);
    void ResolveHeartbeat(const Krpc::RpcHeader &header);
    std::shared_ptr<PendingCall> CallFuture(const ::google::protobuf::MethodDescriptor *method,
                                            ::google::protobuf::RpcController *controller,
                                            const ::google::protobuf::Message *request,
                                            ::google::protobuf::Message *response,
                                            ::google::protobuf::Closure *done,
                                            AsyncCallback callback);
    void RemovePendingCall(uint64_t request_id, const std::string &reason);
    bool EnsureConnection(const ::google::protobuf::MethodDescriptor *method,
                          ::google::protobuf::RpcController *controller,
                          std::string *error_text);
    bool SendBuffer(const std::string &buffer, std::string *error_text);
    void CloseConnectionLocked();
    void EnqueueSend(uint64_t request_id, std::string &&buffer);
    void CompletePending(const std::shared_ptr<PendingCall> &pending, const std::string &reason, bool success);
};
#endif
