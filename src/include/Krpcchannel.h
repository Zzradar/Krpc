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
#include <deque>
#include "zookeeperutil.h"
#include "Krpcheader.pb.h"
#include "load_balancer.h"
class KrpcChannel : public google::protobuf::RpcChannel,
                    public std::enable_shared_from_this<KrpcChannel>
{
public:
    KrpcChannel(bool connectNow);
    virtual ~KrpcChannel();
    static bool CreateConnectionFd(const std::string &ip, uint16_t port, int &out_fd, std::string *errMsg);
    static bool IsConnectionHealthy(int fd);
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
    int m_pool_max_idle;
    std::thread m_heartbeat_thread;
    std::atomic<bool> m_heartbeat_running;
    bool m_heartbeat_thread_started;
    std::condition_variable m_heartbeat_cv;
    std::mutex m_heartbeat_mutex;
    std::mutex m_socket_mutex;
    int m_missed_heartbeat_count;
    std::chrono::steady_clock::time_point m_last_pong_time;
    std::string m_endpoint_key;
    bool m_use_pool{true};
    std::unique_ptr<ILoadBalancer> m_lb;
    struct EndpointFailState {
        std::chrono::steady_clock::time_point retry_at;
    };
    std::mutex m_fail_mutex;
    std::unordered_map<std::string, EndpointFailState> m_fail_states;
    std::string m_last_service;
    std::string m_last_method;
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
        std::string service_name;
        std::string method_name;
        std::string peer;
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
    int AcquireConnection(const std::string &ip, uint16_t port, std::string *errMsg, bool *from_pool = nullptr);
    void ReleaseConnection(bool healthy);
    bool newConnect(const char *ip, uint16_t port, std::string *errMsg = nullptr);
    std::vector<Endpoint> QueryServiceNodes(ZkClient *zkclient, const std::string &service_name, const std::string &method_name);
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
    bool EndpointAvailable(const Endpoint &ep, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point &next_retry);
    void MarkEndpointFailure(const Endpoint &ep);
    void ClearEndpointFailure(const Endpoint &ep);
    void CloseConnectionLocked();
    void EnqueueSend(uint64_t request_id, std::string &&buffer);
    void CompletePending(const std::shared_ptr<PendingCall> &pending, const std::string &reason, bool success);
};
#endif
