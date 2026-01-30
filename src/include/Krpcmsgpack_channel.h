#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "Krpcmsgpack_dispatcher.h"
#include "Krpcmsgpack_protocol.h"
#include "load_balancer.h"

class ZkClient;

class KrpcMsgpackChannel {
public:
    KrpcMsgpackChannel();
    KrpcMsgpackChannel(const std::string &ip, uint16_t port);
    ~KrpcMsgpackChannel();

    using AsyncCallback = std::function<void(const std::string &error, krpc::msgpack::object_handle result)>;

    template <typename R, typename... Args>
    typename std::enable_if<!std::is_void<R>::value, R>::type
    Call(const std::string &service, const std::string &method, Args &&...args) {
        auto oh = CallRaw(service, method, std::forward<Args>(args)...);
        R result;
        oh.get().convert(result);
        return result;
    }

    template <typename R = void, typename... Args>
    typename std::enable_if<std::is_void<R>::value, void>::type
    Call(const std::string &service, const std::string &method, Args &&...args) {
        (void)CallRaw(service, method, std::forward<Args>(args)...);
    }

    template <typename R, typename... Args>
    typename std::enable_if<!std::is_void<R>::value, R>::type
    CallWithTimeout(const std::string &service, const std::string &method, int timeout_ms, Args &&...args) {
        auto oh = CallRawWithTimeout(service, method, timeout_ms, std::forward<Args>(args)...);
        R result;
        oh.get().convert(result);
        return result;
    }

    template <typename R = void, typename... Args>
    typename std::enable_if<std::is_void<R>::value, void>::type
    CallWithTimeout(const std::string &service, const std::string &method, int timeout_ms, Args &&...args) {
        (void)CallRawWithTimeout(service, method, timeout_ms, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Notify(const std::string &service, const std::string &method, Args &&...args) {
        std::string err;
        if (!EnsureConnection(service, method, &err)) {
            return;
        }
        krpc::MsgpackHeader header;
        header.msg_type = static_cast<uint8_t>(krpc::MsgpackMsgType::Oneway);
        header.timeout_ms = 0;
        header.service_name = service;
        header.method_name = method;
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
        auto frame = std::make_tuple(header, args_tuple);
        krpc::msgpack::sbuffer sbuf;
        krpc::msgpack::pack(sbuf, frame);
        SendFrame(sbuf);
        heartbeat_cv_.notify_all();
    }

    template <typename... Args>
    krpc::msgpack::object_handle CallRaw(const std::string &service,
                                         const std::string &method,
                                         Args &&...args) {
        return CallAsyncRaw(service, method, std::forward<Args>(args)...).get();
    }

    template <typename... Args>
    krpc::msgpack::object_handle CallRawWithTimeout(const std::string &service,
                                                    const std::string &method,
                                                    int timeout_ms,
                                                    Args &&...args) {
        return CallAsyncRawWithTimeout(service, method, timeout_ms, std::forward<Args>(args)...).get();
    }

    template <typename... Args>
    std::future<krpc::msgpack::object_handle> CallAsyncRaw(const std::string &service,
                                                           const std::string &method,
                                                           Args &&...args) {
        return CallAsyncRawWithTimeout(service, method, m_request_timeout_ms, std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::future<krpc::msgpack::object_handle> CallAsyncRawWithTimeout(const std::string &service,
                                                                      const std::string &method,
                                                                      int timeout_ms,
                                                                      Args &&...args) {
        std::string err;
        if (!EnsureConnection(service, method, &err)) {
            std::promise<krpc::msgpack::object_handle> promise;
            promise.set_exception(std::make_exception_ptr(std::runtime_error(err.empty() ? "connect failed" : err)));
            return promise.get_future();
        }

        const uint64_t request_id = NextRequestId();
        krpc::MsgpackHeader header;
        header.msg_type = static_cast<uint8_t>(krpc::MsgpackMsgType::Request);
        header.request_id = request_id;
        const int clamped_timeout = timeout_ms < 0 ? 0 : timeout_ms;
        header.timeout_ms = static_cast<uint32_t>(clamped_timeout);
        header.service_name = service;
        header.method_name = method;
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
        auto frame = std::make_tuple(header, args_tuple);
        krpc::msgpack::sbuffer sbuf;
        krpc::msgpack::pack(sbuf, frame);
        heartbeat_cv_.notify_all();
        return EnqueueRequest(request_id, std::move(sbuf), clamped_timeout);
    }

    template <typename... Args>
    void CallAsync(const std::string &service,
                   const std::string &method,
                   AsyncCallback callback,
                   Args &&...args) {
        CallAsyncWithTimeout(service, method, m_request_timeout_ms, std::move(callback), std::forward<Args>(args)...);
    }

    template <typename... Args>
    void CallAsyncWithTimeout(const std::string &service,
                              const std::string &method,
                              int timeout_ms,
                              AsyncCallback callback,
                              Args &&...args) {
        std::string err;
        if (!EnsureConnection(service, method, &err)) {
            if (callback) {
                callback(err.empty() ? "connect failed" : err, MakeNilHandle());
            }
            return;
        }

        const uint64_t request_id = NextRequestId();
        krpc::MsgpackHeader header;
        header.msg_type = static_cast<uint8_t>(krpc::MsgpackMsgType::Request);
        header.request_id = request_id;
        const int clamped_timeout = timeout_ms < 0 ? 0 : timeout_ms;
        header.timeout_ms = static_cast<uint32_t>(clamped_timeout);
        header.service_name = service;
        header.method_name = method;
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
        auto frame = std::make_tuple(header, args_tuple);
        krpc::msgpack::sbuffer sbuf;
        krpc::msgpack::pack(sbuf, frame);
        heartbeat_cv_.notify_all();
        EnqueueRequest(request_id, std::move(sbuf), clamped_timeout, std::move(callback));
    }

private:
    enum class HeartbeatResult {
        kSuccess,
        kTimeout,
        kFatal
    };

    struct PendingCall;

    void StartRecvThread();
    void StopRecvThread();
    void RecvLoop();
    void StartHeartbeatThread();
    void StopHeartbeatThread();
    void HeartbeatLoop();
    HeartbeatResult SendHeartbeatOnce();
    void StartTimeoutThread();
    void StopTimeoutThread();
    void TimeoutLoop();
    bool SendFrame(const krpc::msgpack::sbuffer &payload);
    void StartSendThread();
    void StopSendThread();
    void SendLoop();
    void EnqueueSend(std::string &&header, std::string &&body);
    std::future<krpc::msgpack::object_handle> EnqueueRequest(uint64_t request_id,
                                                             krpc::msgpack::sbuffer &&payload,
                                                             int timeout_ms);
    void EnqueueRequest(uint64_t request_id,
                        krpc::msgpack::sbuffer &&payload,
                        int timeout_ms,
                        AsyncCallback callback);
    void FailAllPending(const std::string &reason);
    void HandleHeartbeatFailure(const std::string &reason);
    void ResolveHeartbeat(const krpc::MsgpackHeader &header);
    void CompletePending(PendingCall &&pending,
                         const std::string &error,
                         krpc::msgpack::object_handle &&result);
    static krpc::msgpack::object_handle MakeNilHandle();

    int AcquireConnection(const std::string &ip, uint16_t port, std::string *errMsg, bool *from_pool);
    void ReleaseConnection(bool healthy);
    void CloseConnectionLocked();
    std::vector<Endpoint> QueryServiceNodes(ZkClient *zkclient,
                                            const std::string &service_name,
                                            const std::string &method_name);
    bool EnsureConnection(const std::string &service,
                          const std::string &method,
                          std::string *error_text);
    bool EndpointAvailable(const Endpoint &ep,
                           std::chrono::steady_clock::time_point now,
                           std::chrono::steady_clock::time_point &next_retry);
    void MarkEndpointFailure(const Endpoint &ep);
    void ClearEndpointFailure(const Endpoint &ep);

    static uint64_t NextRequestId();

    int socket_fd_{-1};
    std::mutex m_socket_mutex;
    std::string m_ip;
    uint16_t m_port{0};
    std::string m_endpoint_key;
    bool m_use_pool{true};
    int m_pool_max_idle{4};
    int m_request_timeout_ms{KrpcProtocol::kDefaultRequestTimeoutMs};
    int m_heartbeat_interval_ms{KrpcProtocol::kDefaultHeartbeatIntervalMs};
    int m_heartbeat_miss_limit{KrpcProtocol::kDefaultHeartbeatMissLimit};
    int m_missed_heartbeat_count{0};
    std::chrono::steady_clock::time_point m_last_pong_time{std::chrono::steady_clock::now()};
    std::unique_ptr<ILoadBalancer> m_lb;

    struct EndpointFailState {
        std::chrono::steady_clock::time_point retry_at;
    };
    std::mutex m_fail_mutex;
    std::unordered_map<std::string, EndpointFailState> m_fail_states;

    std::atomic<bool> running_{false};
    std::thread recv_thread_;

    std::mutex pending_mutex_;
    struct PendingCall {
        std::promise<krpc::msgpack::object_handle> promise;
        bool has_promise{false};
        AsyncCallback callback;
        std::chrono::steady_clock::time_point start_time;
        int timeout_ms{0};
    };
    std::unordered_map<uint64_t, PendingCall> pending_;

    struct SendTask {
        std::string header;
        std::string body;
    };
    std::queue<SendTask> send_queue_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::thread send_thread_;
    std::atomic<bool> send_running_{false};
    bool send_thread_started_{false};

    std::mutex timeout_mutex_;
    std::condition_variable timeout_cv_;
    std::thread timeout_thread_;
    std::atomic<bool> timeout_running_{false};
    bool timeout_thread_started_{false};

    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    bool heartbeat_thread_started_{false};
    std::mutex heartbeat_wait_mutex_;
    std::condition_variable heartbeat_wait_cv_;
    bool waiting_heartbeat_{false};
    uint64_t waiting_heartbeat_id_{0};
    bool waiting_heartbeat_result_{false};

    std::string recv_buffer_;
};
