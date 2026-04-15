#include "Krpcmsgpack_provider.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <tuple>

#include <muduo/base/Timestamp.h>

#include "Krpcapplication.h"
#include "KrpcLogger.h"
#include "Krpcmsgpack_protocol.h"
#include "metrics_export.h"
#include "metrics_http_server.h"

namespace {

bool ReadFrame(muduo::net::Buffer *buffer, std::string &out) {
    const size_t readable = buffer->readableBytes();
    if (readable < sizeof(uint32_t)) {
        return false;
    }
    uint32_t net_len = 0;
    std::memcpy(&net_len, buffer->peek(), sizeof(net_len));
    const uint32_t len = ntohl(net_len);
    if (readable < sizeof(uint32_t) + len) {
        return false;
    }
    buffer->retrieve(sizeof(uint32_t));
    out.assign(buffer->peek(), len);
    buffer->retrieve(len);
    return true;
}

void AppendFrame(muduo::net::Buffer *buffer, const krpc::msgpack::sbuffer &payload) {
    const uint32_t len = static_cast<uint32_t>(payload.size());
    const uint32_t net_len = htonl(len);
    buffer->append(&net_len, sizeof(net_len));
    buffer->append(payload.data(), payload.size());
}

int ParseConfigInt(const std::string &value, int default_value) {
    if (value.empty()) {
        return default_value;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return default_value;
    }
}

} // namespace

KrpcMsgpackProvider::KrpcMsgpackProvider() = default;

KrpcMsgpackProvider::~KrpcMsgpackProvider() {
    StopWorkerPool();
}

void KrpcMsgpackProvider::Run() {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    const std::string ip = config.Load("rpcserverip");
    const int port = std::atoi(config.Load("rpcserverport").c_str());

    muduo::net::InetAddress address(ip, port);
    muduo::net::TcpServer server(&loop_, address, "KrpcMsgpackProvider");

    server.setConnectionCallback(
        std::bind(&KrpcMsgpackProvider::OnConnection, this, std::placeholders::_1));
    server.setMessageCallback(
        std::bind(&KrpcMsgpackProvider::OnMessage, this, std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));

    server.setThreadNum(4);
    StartWorkerPool();

    const bool metrics_enabled = ParseConfigInt(config.Load("metrics_http_enabled"), 0) != 0;
    const int metrics_port = ParseConfigInt(config.Load("metrics_http_port"), 0);
    if (metrics_enabled && metrics_port > 0) {
        if (StartMetricsHttpServer(metrics_port)) {
            LOG(INFO) << "metrics http server started at 0.0.0.0:" << metrics_port;
        } else {
            LOG(WARNING) << "metrics http server failed to start at port " << metrics_port;
        }
    }

    const bool skip_zk = ParseConfigInt(config.Load("skip_zookeeper_registration"), 0) != 0;
    if (!skip_zk) {
        zk_registry_.reset(new ZkClient());
        zk_registry_->Start();
        for (const auto &svc : service_methods_) {
            const std::string service_path = "/" + svc.first;
            zk_registry_->Create(service_path.c_str(), nullptr, 0);
            for (const auto &method : svc.second) {
                const std::string method_path = service_path + "/" + method;
                zk_registry_->Create(method_path.c_str(), nullptr, 0);
                char addr[128] = {0};
                std::snprintf(addr, sizeof(addr), "%s:%d", ip.c_str(), port);
                const std::string child_path = method_path + "/" + addr;
                zk_registry_->Create(child_path.c_str(), addr, std::strlen(addr), ZOO_EPHEMERAL);
                LOG(INFO) << "zk register ok " << child_path;
            }
        }
    } else {
        LOG(WARNING) << "skip_zookeeper_registration=1: ZooKeeper registration skipped (use LB_STATIC_ENDPOINTS on clients)";
    }

    std::cout << "KrpcMsgpackProvider start service at ip:" << ip << " port:" << port << std::endl;

    ScheduleIdleScan();

    server.start();
    loop_.loop();
}

void KrpcMsgpackProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        conn->setTcpNoDelay(true);
        RegisterConnection(conn);
        return;
    }
    RemoveConnection(conn);
    conn->shutdown();
}

void KrpcMsgpackProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn,
                                    muduo::net::Buffer *buffer,
                                    muduo::Timestamp receive_time) {
    (void)receive_time;
    std::string payload;
    while (ReadFrame(buffer, payload)) {
        krpc::msgpack::object_handle oh = krpc::msgpack::unpack(payload.data(), payload.size());
        using frame_t = std::tuple<krpc::MsgpackHeader, krpc::msgpack::object>;
        frame_t frame;
        try {
            oh.get().convert(frame);
        } catch (...) {
            continue;
        }

        const krpc::MsgpackHeader &header = std::get<0>(frame);
        const krpc::msgpack::object &body = std::get<1>(frame);

        if (header.magic != KrpcProtocol::kDefaultMagic || header.version != KrpcProtocol::kDefaultVersion) {
            continue;
        }

        TouchConnection(conn);

        const auto type = static_cast<krpc::MsgpackMsgType>(header.msg_type);
        if (type == krpc::MsgpackMsgType::Ping) {
            SendHeartbeatFrame(conn, krpc::MsgpackMsgType::Pong, header.request_id);
            continue;
        }

        if (type == krpc::MsgpackMsgType::Pong) {
            continue;
        }

        if (type != krpc::MsgpackMsgType::Request &&
            type != krpc::MsgpackMsgType::Oneway) {
            continue;
        }

        MsgpackTask task;
        task.conn = conn;
        task.header = header;
        task.body = body;
        task.holder = std::move(oh);
        task.start_time = muduo::Timestamp::now();

        if (!EnqueueTask(task)) {
            // 若线程池不可用则回退到当前线程执行
            if (type == krpc::MsgpackMsgType::Oneway) {
                const bool handled = dispatcher_.Dispatch(task.header.service_name, task.header.method_name, task.body, nullptr, nullptr);
                const bool ok = handled;
                const double cost_ms = muduo::timeDifference(muduo::Timestamp::now(), task.start_time) * 1000.0;
                const std::string label = task.header.service_name + "." + task.header.method_name;
                RecordMetricsSampleWithLabel(label, ok, static_cast<int64_t>(cost_ms));
                continue;
            }

            krpc::msgpack::object_handle result;
            std::string error;
            bool handled = dispatcher_.Dispatch(task.header.service_name, task.header.method_name, task.body, &result, &error);
            if (!handled && error.empty()) {
                const double cost_ms = muduo::timeDifference(muduo::Timestamp::now(), task.start_time) * 1000.0;
                const std::string label = task.header.service_name + "." + task.header.method_name;
                RecordMetricsSampleWithLabel(label, false, static_cast<int64_t>(cost_ms));
                continue;
            }

            krpc::MsgpackHeader resp;
            resp.msg_type = static_cast<uint8_t>(krpc::MsgpackMsgType::Response);
            resp.request_id = task.header.request_id;
            resp.service_name = task.header.service_name;
            resp.method_name = task.header.method_name;

            krpc::msgpack::sbuffer out_payload;
            if (error.empty()) {
                auto payload_tuple = std::make_tuple(krpc::msgpack::type::nil_t(), result.get());
                auto resp_frame = std::make_tuple(resp, payload_tuple);
                krpc::msgpack::pack(out_payload, resp_frame);
            } else {
                auto payload_tuple = std::make_tuple(error, result.get());
                auto resp_frame = std::make_tuple(resp, payload_tuple);
                krpc::msgpack::pack(out_payload, resp_frame);
            }

            muduo::net::Buffer out;
            AppendFrame(&out, out_payload);
            task.conn->send(&out);

            const bool ok = handled && error.empty();
            const double cost_ms = muduo::timeDifference(muduo::Timestamp::now(), task.start_time) * 1000.0;
            const std::string label = task.header.service_name + "." + task.header.method_name;
            RecordMetricsSampleWithLabel(label, ok, static_cast<int64_t>(cost_ms));
        }
    }
}

void KrpcMsgpackProvider::SendHeartbeatFrame(const muduo::net::TcpConnectionPtr &conn,
                                             krpc::MsgpackMsgType type,
                                             uint64_t request_id) {
    krpc::MsgpackHeader header;
    header.msg_type = static_cast<uint8_t>(type);
    header.request_id = request_id;
    krpc::msgpack::sbuffer out_payload;
    auto resp_frame = std::make_tuple(header, krpc::msgpack::type::nil_t());
    krpc::msgpack::pack(out_payload, resp_frame);
    muduo::net::Buffer out;
    AppendFrame(&out, out_payload);
    conn->send(&out);
}

void KrpcMsgpackProvider::TouchConnection(const muduo::net::TcpConnectionPtr &conn) {
    std::lock_guard<std::mutex> lock(connection_states_mutex_);
    auto it = connection_states_.find(conn.get());
    if (it == connection_states_.end()) {
        return;
    }
    it->second.last_activity = muduo::Timestamp::now();
}

void KrpcMsgpackProvider::StartWorkerPool() {
    if (!worker_threads_.empty()) {
        return;
    }

    auto &config = KrpcApplication::GetInstance().GetConfig();
    const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
    const int default_threads = static_cast<int>(std::max(4u, hw_threads));
    int configured_threads = ParseConfigInt(config.Load("provider_worker_threads"), default_threads);
    if (configured_threads <= 0) {
        configured_threads = default_threads;
    }
    int configured_capacity = ParseConfigInt(config.Load("provider_queue_capacity"), 1024);
    if (configured_capacity <= 0) {
        configured_capacity = 1024;
    }

    task_queue_capacity_ = static_cast<size_t>(configured_capacity);
    worker_thread_count_ = configured_threads;
    stop_workers_.store(false);
    worker_threads_.reserve(static_cast<size_t>(configured_threads));
    for (int i = 0; i < configured_threads; ++i) {
        worker_threads_.emplace_back(&KrpcMsgpackProvider::WorkerLoop, this);
    }
    KrpcLogger::Info("msgpack worker pool started threads=" + std::to_string(worker_thread_count_) +
                     " queue_capacity=" + std::to_string(task_queue_capacity_));
}

void KrpcMsgpackProvider::StopWorkerPool() {
    if (worker_threads_.empty()) {
        return;
    }

    stop_workers_.store(true);
    task_queue_cv_.notify_all();
    for (auto &worker : worker_threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    worker_threads_.clear();

    std::lock_guard<std::mutex> lock(task_queue_mutex_);
    task_queue_.clear();
}

bool KrpcMsgpackProvider::EnqueueTask(MsgpackTask &task) {
    std::unique_lock<std::mutex> lock(task_queue_mutex_);
    task_queue_cv_.wait(lock, [&] {
        return stop_workers_.load() || task_queue_.size() < task_queue_capacity_;
    });

    if (stop_workers_.load()) {
        return false;
    }

    task_queue_.emplace_back(std::move(task));
    task_queue_cv_.notify_one();
    return true;
}

void KrpcMsgpackProvider::WorkerLoop() {
    while (true) {
        MsgpackTask task;
        {
            std::unique_lock<std::mutex> lock(task_queue_mutex_);
            task_queue_cv_.wait(lock, [&] {
                return stop_workers_.load() || !task_queue_.empty();
            });

            if (stop_workers_.load() && task_queue_.empty()) {
                return;
            }

            task = std::move(task_queue_.front());
            task_queue_.pop_front();
            task_queue_cv_.notify_all();
        }

        const auto type = static_cast<krpc::MsgpackMsgType>(task.header.msg_type);
        if (type == krpc::MsgpackMsgType::Oneway) {
            const bool handled = dispatcher_.Dispatch(task.header.service_name, task.header.method_name, task.body, nullptr, nullptr);
            const bool ok = handled;
            const double cost_ms = muduo::timeDifference(muduo::Timestamp::now(), task.start_time) * 1000.0;
            const std::string label = task.header.service_name + "." + task.header.method_name;
            RecordMetricsSampleWithLabel(label, ok, static_cast<int64_t>(cost_ms));
            continue;
        }

        krpc::msgpack::object_handle result;
        std::string error;
        bool handled = dispatcher_.Dispatch(task.header.service_name, task.header.method_name, task.body, &result, &error);
        if (!handled && error.empty()) {
            const double cost_ms = muduo::timeDifference(muduo::Timestamp::now(), task.start_time) * 1000.0;
            const std::string label = task.header.service_name + "." + task.header.method_name;
            RecordMetricsSampleWithLabel(label, false, static_cast<int64_t>(cost_ms));
            continue;
        }

        krpc::MsgpackHeader resp;
        resp.msg_type = static_cast<uint8_t>(krpc::MsgpackMsgType::Response);
        resp.request_id = task.header.request_id;
        resp.service_name = task.header.service_name;
        resp.method_name = task.header.method_name;

        krpc::msgpack::sbuffer out_payload;
        if (error.empty()) {
            auto payload_tuple = std::make_tuple(krpc::msgpack::type::nil_t(), result.get());
            auto resp_frame = std::make_tuple(resp, payload_tuple);
            krpc::msgpack::pack(out_payload, resp_frame);
        } else {
            auto payload_tuple = std::make_tuple(error, result.get());
            auto resp_frame = std::make_tuple(resp, payload_tuple);
            krpc::msgpack::pack(out_payload, resp_frame);
        }

        muduo::net::Buffer out;
        AppendFrame(&out, out_payload);
        task.conn->send(&out);

        const bool ok = handled && error.empty();
        const double cost_ms = muduo::timeDifference(muduo::Timestamp::now(), task.start_time) * 1000.0;
        const std::string label = task.header.service_name + "." + task.header.method_name;
        RecordMetricsSampleWithLabel(label, ok, static_cast<int64_t>(cost_ms));
    }
}

void KrpcMsgpackProvider::RegisterConnection(const muduo::net::TcpConnectionPtr &conn) {
    std::lock_guard<std::mutex> lock(connection_states_mutex_);
    ConnectionState state;
    state.last_activity = muduo::Timestamp::now();
    state.weak_conn = conn;
    connection_states_[conn.get()] = state;
}

void KrpcMsgpackProvider::RemoveConnection(const muduo::net::TcpConnectionPtr &conn) {
    std::lock_guard<std::mutex> lock(connection_states_mutex_);
    connection_states_.erase(conn.get());
}

void KrpcMsgpackProvider::ScheduleIdleScan() {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    const int heartbeat_interval_ms = ParseConfigInt(config.Load("heartbeat_interval_ms"),
                                                     KrpcProtocol::kDefaultHeartbeatIntervalMs);
    const int heartbeat_miss_limit = ParseConfigInt(config.Load("heartbeat_miss_limit"),
                                                    KrpcProtocol::kDefaultHeartbeatMissLimit);

    idle_close_threshold_ms_ = heartbeat_interval_ms * (heartbeat_miss_limit + 1);
    double interval_seconds = static_cast<double>(heartbeat_interval_ms) / 1000.0;
    idle_timer_id_ = loop_.runEvery(interval_seconds, std::bind(&KrpcMsgpackProvider::OnIdleScan, this));
}

void KrpcMsgpackProvider::OnIdleScan() {
    std::vector<muduo::net::TcpConnectionPtr> to_close;
    const muduo::Timestamp now = muduo::Timestamp::now();
    {
        std::lock_guard<std::mutex> lock(connection_states_mutex_);
        for (auto &entry : connection_states_) {
            const auto &state = entry.second;
            const double idle_ms = muduo::timeDifference(now, state.last_activity) * 1000.0;
            if (idle_ms >= static_cast<double>(idle_close_threshold_ms_)) {
                auto conn = state.weak_conn.lock();
                if (conn) {
                    to_close.push_back(conn);
                }
            }
        }
    }

    for (auto &conn : to_close) {
        KrpcLogger::ERROR("closing idle connection");
        conn->forceClose();
    }
}
