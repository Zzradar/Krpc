#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <muduo/base/Timestamp.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>
#include <muduo/net/TimerId.h>

#include "Krpcmsgpack_dispatcher.h"
#include "Krpcmsgpack_protocol.h"

class KrpcMsgpackProvider {
public:
    KrpcMsgpackProvider();
    ~KrpcMsgpackProvider();

    template <typename F>
    void Bind(const std::string &service, const std::string &method, F func) {
        dispatcher_.bind(service, method, func);
        auto &methods = service_methods_[service];
        methods.push_back(method);
    }

    void Run();

private:
    void OnConnection(const muduo::net::TcpConnectionPtr &conn);
    void OnMessage(const muduo::net::TcpConnectionPtr &conn,
                   muduo::net::Buffer *buffer,
                   muduo::Timestamp receive_time);
    void SendHeartbeatFrame(const muduo::net::TcpConnectionPtr &conn,
                            krpc::MsgpackMsgType type,
                            uint64_t request_id);
    void TouchConnection(const muduo::net::TcpConnectionPtr &conn);
    void RegisterConnection(const muduo::net::TcpConnectionPtr &conn);
    void RemoveConnection(const muduo::net::TcpConnectionPtr &conn);
    void ScheduleIdleScan();
    void OnIdleScan();
    void StartWorkerPool();
    void StopWorkerPool();

    struct MsgpackTask {
        muduo::net::TcpConnectionPtr conn;
        krpc::MsgpackHeader header;
        krpc::msgpack::object body;
        krpc::msgpack::object_handle holder;
        muduo::Timestamp start_time;
    };

    bool EnqueueTask(MsgpackTask &task);
    void WorkerLoop();

    muduo::net::EventLoop loop_;
    krpc::MsgpackDispatcher dispatcher_;
    std::unordered_map<std::string, std::vector<std::string>> service_methods_;

    struct ConnectionState {
        muduo::Timestamp last_activity;
        std::weak_ptr<muduo::net::TcpConnection> weak_conn;
    };
    std::unordered_map<const muduo::net::TcpConnection *, ConnectionState> connection_states_;
    std::mutex connection_states_mutex_;
    muduo::net::TimerId idle_timer_id_;
    int idle_close_threshold_ms_{0};

    std::deque<MsgpackTask> task_queue_;
    std::mutex task_queue_mutex_;
    std::condition_variable task_queue_cv_;
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> stop_workers_{false};
    size_t task_queue_capacity_{1024};
    int worker_thread_count_{0};
};
