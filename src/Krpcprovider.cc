#include "Krpcprovider.h"
#include "Krpcapplication.h"
#include "Krpcheader.pb.h"
#include "KrpcLogger.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <iostream>
#include <vector>

namespace {

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

KrpcProvider::SendResponseClosure::SendResponseClosure(KrpcProvider *provider,
                                                      muduo::net::TcpConnectionPtr conn,
                                                      uint64_t request_id,
                                                      google::protobuf::Message *response)
    : provider_(provider),
      conn_(std::move(conn)),
      request_id_(request_id),
      response_(response) {}

void KrpcProvider::SendResponseClosure::Run() {
    provider_->SendRpcResponse(conn_, request_id_, response_);
    delete this;
}

// 注册服务对象及其方法，以便服务端能够处理客户端的RPC请求
void KrpcProvider::NotifyService(google::protobuf::Service *service) {
    // 服务端需要知道客户端想要调用的服务对象和方法，
    // 这些信息会保存在一个数据结构（如 ServiceInfo）中。
    ServiceInfo service_info;

    // 参数类型设置为 google::protobuf::Service，是因为所有由 protobuf 生成的服务类
    // 都继承自 google::protobuf::Service，这样我们可以通过基类指针指向子类对象，
    // 实现动态多态。

    // 通过动态多态调用 service->GetDescriptor()，
    // GetDescriptor() 方法会返回 protobuf 生成的服务类的描述信息（ServiceDescriptor）。
    const google::protobuf::ServiceDescriptor *psd = service->GetDescriptor();

    // 通过 ServiceDescriptor，我们可以获取该服务类中定义的方法列表，
    // 并进行相应的注册和管理。

    // 获取服务的名字
    std::string service_name = psd->name();
    // 获取服务端对象service的方法数量
    int method_count = psd->method_count();

    // 打印服务名
    std::cout << "service_name=" << service_name << std::endl;

    // 遍历服务中的所有方法，并注册到服务信息中
    for (int i = 0; i < method_count; ++i) {
        // 获取服务中的方法描述
        const google::protobuf::MethodDescriptor *pmd = psd->method(i);
        std::string method_name = pmd->name();
        std::cout << "method_name=" << method_name << std::endl;
        service_info.method_map.emplace(method_name, pmd);  // 将方法名和方法描述符存入map
    }
    service_info.service.reset(service);  // 保存服务对象并托管内存
    service_map.emplace(service_name, std::move(service_info));  // 将服务信息存入服务map
}

// 启动RPC服务节点，开始提供远程网络调用服务
void KrpcProvider::Run() {
    // 读取配置文件中的RPC服务器IP和端口
    std::string ip = KrpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    int port = atoi(KrpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());

    // 使用muduo网络库，创建地址对象
    muduo::net::InetAddress address(ip, port);

    // 创建TcpServer对象
    std::shared_ptr<muduo::net::TcpServer> server = std::make_shared<muduo::net::TcpServer>(&event_loop, address, "KrpcProvider");

    // 绑定连接回调和消息回调，分离网络连接业务和消息处理业务
    server->setConnectionCallback(std::bind(&KrpcProvider::OnConnection, this, std::placeholders::_1));
    server->setMessageCallback(std::bind(&KrpcProvider::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // 设置muduo库的线程数量
    server->setThreadNum(4);

    // 将当前RPC节点上要发布的服务全部注册到ZooKeeper上，让RPC客户端可以在ZooKeeper上发现服务
    ZkClient zkclient;
    zkclient.Start();  // 连接ZooKeeper服务器
    // service_name为永久节点，method_name为临时节点
    for (auto &sp : service_map) {
        // service_name 在ZooKeeper中的目录是"/"+service_name
        std::string service_path = "/" + sp.first;
        zkclient.Create(service_path.c_str(), nullptr, 0);  // 创建服务节点
        for (auto &mp : sp.second.method_map) {
            std::string method_path = service_path + "/" + mp.first;
            char method_path_data[128] = {0};
            sprintf(method_path_data, "%s:%d", ip.c_str(), port);  // 将IP和端口信息存入节点数据
            // ZOO_EPHEMERAL表示这个节点是临时节点，在客户端断开连接后，ZooKeeper会自动删除这个节点
            zkclient.Create(method_path.c_str(), method_path_data, strlen(method_path_data), ZOO_EPHEMERAL);
        }
    }

    // RPC服务端准备启动，打印信息
    std::cout << "RpcProvider start service at ip:" << ip << " port:" << port << std::endl;

    // 启动网络服务
    ScheduleIdleScan();

    server->start();
    event_loop.loop();  // 进入事件循环
}

// 连接回调函数，处理客户端连接事件
void KrpcProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn) {
    if (conn->connected()) {
        RegisterConnection(conn);
        return;
    }
    RemoveConnection(conn);
    conn->shutdown();
}

// 消息回调函数，处理客户端发送的RPC请求
void KrpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buffer, muduo::Timestamp receive_time) {
    std::cout << "OnMessage" << std::endl;

    // 从网络缓冲区中读取远程RPC调用请求的字符流
    std::string recv_buf = buffer->retrieveAllAsString();

    // 使用protobuf的CodedInputStream反序列化RPC请求
    google::protobuf::io::ArrayInputStream raw_input(recv_buf.data(), recv_buf.size());
    google::protobuf::io::CodedInputStream coded_input(&raw_input);

    uint32_t header_size{};
    if (!coded_input.ReadVarint32(&header_size)) {
        KrpcLogger::ERROR("read header_size error");
        return;
    }

    // 根据header_size读取数据头的原始字符流，反序列化数据，得到RPC请求的详细信息
    std::string rpc_header_str;
    google::protobuf::io::CodedInputStream::Limit msg_limit = coded_input.PushLimit(header_size);
    bool header_read_ok = coded_input.ReadString(&rpc_header_str, header_size);
    coded_input.PopLimit(msg_limit);
    if (!header_read_ok) {
        KrpcLogger::ERROR("read header error");
        return;
    }

    Krpc::RpcHeader rpc_header;
    if (!rpc_header.ParseFromString(rpc_header_str)) {
        KrpcLogger::ERROR("krpcHeader parse error");
        return;
    }

    if (rpc_header.magic() != KrpcProtocol::kDefaultMagic) {
        KrpcLogger::ERROR("invalid magic number");
        return;
    }

    if (rpc_header.version() != KrpcProtocol::kDefaultVersion) {
        KrpcLogger::ERROR("invalid rpc version");
        return;
    }

    const std::string service_name = rpc_header.service_name();
    const std::string method_name = rpc_header.method_name();
    const uint32_t body_size = rpc_header.body_size();
    const uint64_t request_id = rpc_header.request_id();

    std::string args_str;  // RPC参数
    if (body_size > 0) {
        if (!coded_input.ReadString(&args_str, body_size)) {
            KrpcLogger::ERROR("read args error");
            return;
        }
    }

    TouchConnection(conn);

    if (rpc_header.msg_type() == Krpc::MSG_TYPE_PING || rpc_header.msg_type() == Krpc::MSG_TYPE_PONG) {
        HandleHeartbeatFrame(conn, rpc_header);
        return;
    }

    if (rpc_header.msg_type() != Krpc::MSG_TYPE_REQUEST) {
        KrpcLogger::ERROR("unsupported msg type");
        return;
    }

    // 获取service对象和method对象
    auto it = service_map.find(service_name);
    if (it == service_map.end()) {
        std::cout << service_name << " is not exist!" << std::endl;
        return;
    }
    auto mit = it->second.method_map.find(method_name);
    if (mit == it->second.method_map.end()) {
        std::cout << service_name << "." << method_name << " is not exist!" << std::endl;
        return;
    }

    google::protobuf::Service *service = it->second.service.get();  // 获取服务对象
    const google::protobuf::MethodDescriptor *method = mit->second;  // 获取方法对象

    // 生成RPC方法调用请求的request和响应的response参数
    google::protobuf::Message *request = service->GetRequestPrototype(method).New();  // 动态创建请求对象
    if (!request->ParseFromString(args_str)) {
        std::cout << service_name << "." << method_name << " parse error!" << std::endl;
        return;
    }
    google::protobuf::Message *response = service->GetResponsePrototype(method).New();  // 动态创建响应对象

    // 绑定回调函数，用于在方法调用完成后发送响应
    google::protobuf::Closure *done = new SendResponseClosure(this, conn, request_id, response);

    // 在框架上根据远端RPC请求，调用当前RPC节点上发布的方法
    service->CallMethod(method, nullptr, request, response, done);  // 调用服务方法
}

// 发送RPC响应给客户端
void KrpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr &conn, uint64_t request_id, google::protobuf::Message *response) {
    std::string response_str;
    if (!response->SerializeToString(&response_str)) {
        std::cout << "serialize error!" << std::endl;
        delete response;
        return;
    }

    Krpc::RpcHeader header;
    header.set_magic(KrpcProtocol::kDefaultMagic);
    header.set_version(KrpcProtocol::kDefaultVersion);
    header.set_msg_type(Krpc::MSG_TYPE_RESPONSE);
    header.set_request_id(request_id);
    header.set_body_size(response_str.size());
    header.set_compress_type(Krpc::COMPRESS_NONE);

    std::string header_str;
    if (!header.SerializeToString(&header_str)) {
        KrpcLogger::ERROR("serialize response header error");
        return;
    }

    std::string send_buf;
    {
        google::protobuf::io::StringOutputStream string_output(&send_buf);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);
        coded_output.WriteVarint32(static_cast<uint32_t>(header_str.size()));
        coded_output.WriteString(header_str);
    }
    send_buf += response_str;
    conn->send(send_buf);
    delete response;
}

void KrpcProvider::SendHeartbeatFrame(const muduo::net::TcpConnectionPtr &conn,
                                      Krpc::MsgType type,
                                      uint64_t request_id) {
    Krpc::RpcHeader header;
    header.set_magic(KrpcProtocol::kDefaultMagic);
    header.set_version(KrpcProtocol::kDefaultVersion);
    header.set_msg_type(type);
    header.set_request_id(request_id);
    header.set_body_size(0);
    header.set_compress_type(Krpc::COMPRESS_NONE);

    std::string header_str;
    if (!header.SerializeToString(&header_str)) {
        KrpcLogger::ERROR("serialize heartbeat header error");
        return;
    }

    std::string send_buf;
    {
        google::protobuf::io::StringOutputStream string_output(&send_buf);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);
        coded_output.WriteVarint32(static_cast<uint32_t>(header_str.size()));
        coded_output.WriteString(header_str);
    }

    conn->send(send_buf);
}

void KrpcProvider::HandleHeartbeatFrame(const muduo::net::TcpConnectionPtr &conn, const Krpc::RpcHeader &header) {
    if (header.msg_type() == Krpc::MSG_TYPE_PING) {
        SendHeartbeatFrame(conn, Krpc::MSG_TYPE_PONG, header.request_id());
        return;
    }
    // MSG_TYPE_PONG received from client — nothing to send back.
}

void KrpcProvider::TouchConnection(const muduo::net::TcpConnectionPtr &conn) {
    std::lock_guard<std::mutex> lock(connection_states_mutex_);
    auto it = connection_states_.find(conn.get());
    if (it == connection_states_.end()) {
        return;
    }
    it->second.last_activity = muduo::Timestamp::now();
    it->second.missed_heartbeats = 0;
}

void KrpcProvider::RegisterConnection(const muduo::net::TcpConnectionPtr &conn) {
    std::lock_guard<std::mutex> lock(connection_states_mutex_);
    ConnectionState state;
    state.last_activity = muduo::Timestamp::now();
    state.missed_heartbeats = 0;
    state.weak_conn = conn;
    connection_states_[conn.get()] = state;
}

void KrpcProvider::RemoveConnection(const muduo::net::TcpConnectionPtr &conn) {
    std::lock_guard<std::mutex> lock(connection_states_mutex_);
    connection_states_.erase(conn.get());
}

void KrpcProvider::ScheduleIdleScan() {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    const int heartbeat_interval_ms = ParseConfigInt(config.Load("heartbeat_interval_ms"),
                                                     KrpcProtocol::kDefaultHeartbeatIntervalMs);
    const int heartbeat_miss_limit = ParseConfigInt(config.Load("heartbeat_miss_limit"),
                                                    KrpcProtocol::kDefaultHeartbeatMissLimit);

    idle_close_threshold_ms_ = heartbeat_interval_ms * (heartbeat_miss_limit + 1);
    double interval_seconds = static_cast<double>(heartbeat_interval_ms) / 1000.0;
    idle_timer_id_ = event_loop.runEvery(interval_seconds, std::bind(&KrpcProvider::OnIdleScan, this));
}

void KrpcProvider::OnIdleScan() {
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

// 析构函数，退出事件循环
KrpcProvider::~KrpcProvider() {
    std::cout << "~KrpcProvider()" << std::endl;
    event_loop.quit();  // 退出事件循环
}