#include "Krpcchannel.h"
#include "Krpcheader.pb.h"
#include "zookeeperutil.h"
#include "Krpcapplication.h"
#include "Krpccontroller.h"
#include "Krpcprotocol.h"
#include "memory"
#include <errno.h>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <atomic>
#include <chrono>
#include <poll.h>
#include "KrpcLogger.h"

std::mutex g_data_mutx;  // 全局互斥锁，用于保护共享数据的线程安全

namespace {
std::atomic<uint64_t> g_request_id{1};

uint64_t NextRequestId() {
    return g_request_id.fetch_add(1, std::memory_order_relaxed);
}

class HeartbeatActivityNotifier {
public:
    explicit HeartbeatActivityNotifier(std::condition_variable &cv) : cv_(cv) {}
    ~HeartbeatActivityNotifier() { cv_.notify_all(); }

private:
    std::condition_variable &cv_;
};

int ParseConfigInt(const std::string &value, int default_value) {
    if (value.empty()) {
        return default_value;
    }
    try {
        return std::stoi(value);
    } catch (const std::exception &) {
        return default_value;
    }
}
} // namespace

// RPC调用的核心方法，负责将客户端的请求序列化并发送到服务端，同时接收服务端的响应
void KrpcChannel::CallMethod(const ::google::protobuf::MethodDescriptor *method,
                             ::google::protobuf::RpcController *controller,
                             const ::google::protobuf::Message *request,
                             ::google::protobuf::Message *response,
                             ::google::protobuf::Closure *done)
{
    HeartbeatActivityNotifier heartbeat_notifier(m_heartbeat_cv);

    const google::protobuf::ServiceDescriptor *sd = method->service();
    service_name = sd->name();
    method_name = method->name();

    // 将请求参数序列化为字符串，并计算其长度
    uint32_t args_size{};
    std::string args_str;
    if (request->SerializeToString(&args_str)) {  // 序列化请求参数
        args_size = args_str.size();  // 获取序列化后的长度
    } else {
        controller->SetFailed("serialize request fail");  // 序列化失败，设置错误信息
        return;
    }

    const uint64_t request_id = NextRequestId();

    // 定义RPC请求的头部信息
    Krpc::RpcHeader krpcheader;
    krpcheader.set_magic(KrpcProtocol::kDefaultMagic);
    krpcheader.set_version(KrpcProtocol::kDefaultVersion);
    krpcheader.set_msg_type(Krpc::MSG_TYPE_REQUEST);
    krpcheader.set_request_id(request_id);
    krpcheader.set_body_size(args_size);
    krpcheader.set_compress_type(Krpc::COMPRESS_NONE);
    krpcheader.set_service_name(service_name);  // 设置服务名
    krpcheader.set_method_name(method_name);  // 设置方法名

    // 将RPC头部信息序列化为字符串，并计算其长度
    uint32_t header_size = 0;
    std::string rpc_header_str;
    if (krpcheader.SerializeToString(&rpc_header_str)) {  // 序列化头部信息
        header_size = rpc_header_str.size();  // 获取序列化后的长度
    } else {
        controller->SetFailed("serialize rpc header error!");  // 序列化失败，设置错误信息
        return;
    }

    // 将头部长度和头部信息拼接成完整的RPC请求报文
    std::string send_rpc_str;
    {
        google::protobuf::io::StringOutputStream string_output(&send_rpc_str);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);
        coded_output.WriteVarint32(static_cast<uint32_t>(header_size));  // 写入头部长度
        coded_output.WriteString(rpc_header_str);  // 写入头部信息
    }
    send_rpc_str += args_str;  // 拼接请求参数

    int timeout_ms = m_request_timeout_ms;
    if (controller) {
        if (auto *krpc_controller = dynamic_cast<Krpccontroller *>(controller)) {
            if (krpc_controller->TimeoutMs() > 0) {
                timeout_ms = krpc_controller->TimeoutMs();
            }
        }
    }

    std::unique_lock<std::mutex> socket_lock(m_socket_mutex);
    if (m_clientfd == -1) {
        socket_lock.unlock();
        ZkClient zkCli;
        zkCli.Start();
        std::string host_data = QueryServiceHost(&zkCli, service_name, method_name, m_idx);
        if (host_data.empty()) {
            if (controller) {
                controller->SetFailed("service node not found: " + service_name + "/" + method_name);
            }
            return;
        }
        std::string target_ip = host_data.substr(0, m_idx);
        uint16_t target_port = static_cast<uint16_t>(atoi(host_data.substr(m_idx + 1).c_str()));
        socket_lock.lock();
        if (m_clientfd == -1) {
            m_ip = target_ip;
            m_port = target_port;
            std::string connect_error;
            if (!newConnect(m_ip.c_str(), m_port, &connect_error)) {
                if (controller) {
                    controller->SetFailed(connect_error.empty() ? "connect server error" : connect_error);
                }
                return;
            }
            LOG(INFO) << "connect server success";
        }
    }

    // 发送RPC请求到服务器
    if (-1 == send(m_clientfd, send_rpc_str.c_str(), send_rpc_str.size(), 0)) {
        close(m_clientfd);  // 发送失败，关闭socket
        m_clientfd = -1;  // 重置socket文件描述符
        char errtxt[512] = {};
        std::cout << "send error: " << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        if (controller) {
            controller->SetFailed(errtxt);
        }
        return;
    }

    struct pollfd pfd;
    pfd.fd = m_clientfd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int poll_ret = poll(&pfd, 1, timeout_ms);
    if (poll_ret == 0) {
        close(m_clientfd);
        m_clientfd = -1;
        if (controller) {
            controller->SetFailed("timeout");
        }
        return;
    }

    if (poll_ret < 0) {
        char errtxt[512] = {};
        std::cout << "poll error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;
        if (controller) {
            controller->SetFailed(errtxt);
        }
        return;
    }

    if (!(pfd.revents & POLLIN)) {
        if (controller) {
            controller->SetFailed("poll no data");
        }
        return;
    }

    // 接收服务器的响应
    char recv_buf[2048] = {0};
    int recv_size = 0;
    if (-1 == (recv_size = recv(m_clientfd, recv_buf, sizeof(recv_buf), 0))) {
        char errtxt[512] = {};
        std::cout << "recv error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        if (controller) {
            controller->SetFailed(errtxt);
        }
        return;
    }

    google::protobuf::io::ArrayInputStream resp_input(recv_buf, recv_size);
    google::protobuf::io::CodedInputStream resp_coded(&resp_input);
    uint32_t resp_header_size{};
    if (!resp_coded.ReadVarint32(&resp_header_size)) {
        controller->SetFailed("read response header size error");
        return;
    }

    std::string resp_header_str;
    auto resp_limit = resp_coded.PushLimit(resp_header_size);
    bool header_ok = resp_coded.ReadString(&resp_header_str, resp_header_size);
    resp_coded.PopLimit(resp_limit);
    if (!header_ok) {
        controller->SetFailed("read response header error");
        return;
    }

    Krpc::RpcHeader resp_header;
    if (!resp_header.ParseFromString(resp_header_str)) {
        controller->SetFailed("parse response header error");
        return;
    }

    if (resp_header.magic() != KrpcProtocol::kDefaultMagic) {
        controller->SetFailed("invalid response magic");
        return;
    }

    if (resp_header.msg_type() != Krpc::MSG_TYPE_RESPONSE) {
        controller->SetFailed("invalid response msg type");
        return;
    }

    if (resp_header.request_id() != request_id) {
        controller->SetFailed("response request id mismatch");
        return;
    }

    std::string response_payload;
    const uint32_t response_size = resp_header.body_size();
    if (response_size > 0) {
        if (!resp_coded.ReadString(&response_payload, response_size)) {
            controller->SetFailed("read response payload error");
            return;
        }
    }

    // 将接收到的响应数据反序列化为response对象
    if (!response->ParseFromString(response_payload)) {
        close(m_clientfd);  // 反序列化失败，关闭socket
        m_clientfd = -1;  // 重置socket文件描述符
        char errtxt[512] = {};
        std::cout << "parse error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        controller->SetFailed(errtxt);  // 设置错误信息
        return;
    }

   // close(m_clientfd);  // 关闭socket连接
}

// 创建新的socket连接
bool KrpcChannel::newConnect(const char *ip, uint16_t port, std::string *errMsg) {
    // 创建socket
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd) {
        char errtxt[512] = {0};
        std::cout << "socket error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        LOG(ERROR) << "socket error:" << errtxt;  // 记录错误日志
        if (errMsg) {
            *errMsg = errtxt;
        }
        return false;
    }

    // 设置服务器地址信息
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;  // IPv4地址族
    server_addr.sin_port = htons(port);  // 端口号
    server_addr.sin_addr.s_addr = inet_addr(ip);  // IP地址

    // 尝试连接服务器
    if (-1 == connect(clientfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
        close(clientfd);  // 连接失败，关闭socket
        char errtxt[512] = {0};
        std::cout << "connect error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        LOG(ERROR) << "connect server error" << errtxt;  // 记录错误日志
        if (errMsg) {
            *errMsg = errtxt;
        }
        return false;
    }

    m_clientfd = clientfd;  // 保存socket文件描述符
    return true;
}

// 从ZooKeeper查询服务地址
std::string KrpcChannel::QueryServiceHost(ZkClient *zkclient, std::string service_name, std::string method_name, int &idx) {
    std::string method_path = "/" + service_name + "/" + method_name;  // 构造ZooKeeper路径
    std::cout << "method_path: " << method_path << std::endl;

    std::unique_lock<std::mutex> lock(g_data_mutx);  // 加锁，保证线程安全
    std::string host_data_1 = zkclient->GetData(method_path.c_str());  // 从ZooKeeper获取数据
    lock.unlock();  // 解锁

    if (host_data_1 == "") {  // 如果未找到服务地址
        LOG(ERROR) << method_path + " is not exist!";  // 记录错误日志
        return "";
    }

    idx = host_data_1.find(":");  // 查找IP和端口的分隔符
    if (idx == -1) {  // 如果分隔符不存在
        LOG(ERROR) << method_path + " address is invalid!";  // 记录错误日志
        return "";
    }

    return host_data_1;  // 返回服务地址
}

// 构造函数，支持延迟连接
KrpcChannel::KrpcChannel(bool connectNow)
        : m_clientfd(-1),
            m_idx(0),
            m_request_timeout_ms(KrpcProtocol::kDefaultRequestTimeoutMs),
            m_heartbeat_interval_ms(KrpcProtocol::kDefaultHeartbeatIntervalMs),
            m_heartbeat_miss_limit(KrpcProtocol::kDefaultHeartbeatMissLimit),
            m_heartbeat_thread(),
            m_heartbeat_running(false),
            m_heartbeat_thread_started(false),
            m_missed_heartbeat_count(0),
            m_last_pong_time(std::chrono::steady_clock::now()) {

    auto &config = KrpcApplication::GetInstance().GetConfig();
    m_request_timeout_ms = ParseConfigInt(config.Load("rpc_timeout_ms"), KrpcProtocol::kDefaultRequestTimeoutMs);
    m_heartbeat_interval_ms = ParseConfigInt(config.Load("heartbeat_interval_ms"), KrpcProtocol::kDefaultHeartbeatIntervalMs);
    m_heartbeat_miss_limit = ParseConfigInt(config.Load("heartbeat_miss_limit"), KrpcProtocol::kDefaultHeartbeatMissLimit);

        StartHeartbeatThread();

    if (!connectNow) {  // 如果不需要立即连接
        return;
    }

    // 尝试连接服务器，最多重试3次
    std::string errtxt;
    auto rt = newConnect(m_ip.c_str(), m_port, &errtxt);
    int count = 3;  // 重试次数
    while (!rt && count--) {
        rt = newConnect(m_ip.c_str(), m_port, &errtxt);
    }
}

KrpcChannel::~KrpcChannel() {
    StopHeartbeatThread();
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    if (m_clientfd != -1) {
        close(m_clientfd);
        m_clientfd = -1;
    }
}

void KrpcChannel::StartHeartbeatThread() {
    if (m_heartbeat_thread_started) {
        return;
    }
    m_heartbeat_running.store(true, std::memory_order_release);
    m_heartbeat_thread = std::thread(&KrpcChannel::HeartbeatLoop, this);
    m_heartbeat_thread_started = true;
}

void KrpcChannel::StopHeartbeatThread() {
    if (!m_heartbeat_thread_started) {
        return;
    }
    m_heartbeat_running.store(false, std::memory_order_release);
    m_heartbeat_cv.notify_all();
    if (m_heartbeat_thread.joinable()) {
        m_heartbeat_thread.join();
    }
    m_heartbeat_thread_started = false;
}

void KrpcChannel::HeartbeatLoop() {
    std::unique_lock<std::mutex> heartbeat_lock(m_heartbeat_mutex);
    while (m_heartbeat_running.load(std::memory_order_acquire)) {
        m_heartbeat_cv.wait_for(heartbeat_lock, std::chrono::milliseconds(m_heartbeat_interval_ms));
        if (!m_heartbeat_running.load(std::memory_order_acquire)) {
            break;
        }

        bool has_connection = false;
        {
            std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
            has_connection = (m_clientfd != -1);
        }
        if (!has_connection) {
            m_missed_heartbeat_count = 0;
            continue;
        }

        heartbeat_lock.unlock();
        auto result = SendHeartbeatOnce();
        heartbeat_lock.lock();

        if (!m_heartbeat_running.load(std::memory_order_acquire)) {
            break;
        }

        switch (result) {
            case HeartbeatResult::kSuccess:
                m_missed_heartbeat_count = 0;
                m_last_pong_time = std::chrono::steady_clock::now();
                break;
            case HeartbeatResult::kTimeout:
                ++m_missed_heartbeat_count;
                if (m_missed_heartbeat_count >= m_heartbeat_miss_limit) {
                    HandleHeartbeatFailure("heartbeat timeout");
                    m_missed_heartbeat_count = 0;
                }
                break;
            case HeartbeatResult::kFatal:
                HandleHeartbeatFailure("heartbeat fatal error");
                m_missed_heartbeat_count = 0;
                break;
        }
    }
}

KrpcChannel::HeartbeatResult KrpcChannel::SendHeartbeatOnce() {
    std::unique_lock<std::mutex> socket_lock(m_socket_mutex);
    if (m_clientfd == -1) {
        return HeartbeatResult::kSuccess;
    }

    const uint64_t request_id = NextRequestId();

    Krpc::RpcHeader header;
    header.set_magic(KrpcProtocol::kDefaultMagic);
    header.set_version(KrpcProtocol::kDefaultVersion);
    header.set_msg_type(Krpc::MSG_TYPE_PING);
    header.set_request_id(request_id);
    header.set_body_size(0);
    header.set_compress_type(Krpc::COMPRESS_NONE);

    std::string header_str;
    if (!header.SerializeToString(&header_str)) {
        return HeartbeatResult::kFatal;
    }

    std::string send_buf;
    {
        google::protobuf::io::StringOutputStream string_output(&send_buf);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);
        coded_output.WriteVarint32(static_cast<uint32_t>(header_str.size()));
        coded_output.WriteString(header_str);
    }

    if (-1 == send(m_clientfd, send_buf.c_str(), send_buf.size(), 0)) {
        return HeartbeatResult::kFatal;
    }

    struct pollfd pfd;
    pfd.fd = m_clientfd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int poll_ret = poll(&pfd, 1, m_request_timeout_ms);
    if (poll_ret == 0) {
        return HeartbeatResult::kTimeout;
    }
    if (poll_ret < 0 || !(pfd.revents & POLLIN)) {
        return HeartbeatResult::kFatal;
    }

    char recv_buf[512] = {0};
    int recv_size = recv(m_clientfd, recv_buf, sizeof(recv_buf), 0);
    if (recv_size <= 0) {
        return HeartbeatResult::kFatal;
    }

    google::protobuf::io::ArrayInputStream resp_input(recv_buf, recv_size);
    google::protobuf::io::CodedInputStream resp_coded(&resp_input);
    uint32_t resp_header_size{};
    if (!resp_coded.ReadVarint32(&resp_header_size)) {
        return HeartbeatResult::kFatal;
    }

    std::string resp_header_str;
    auto resp_limit = resp_coded.PushLimit(resp_header_size);
    bool header_ok = resp_coded.ReadString(&resp_header_str, resp_header_size);
    resp_coded.PopLimit(resp_limit);
    if (!header_ok) {
        return HeartbeatResult::kFatal;
    }

    Krpc::RpcHeader resp_header;
    if (!resp_header.ParseFromString(resp_header_str)) {
        return HeartbeatResult::kFatal;
    }

    if (resp_header.magic() != KrpcProtocol::kDefaultMagic ||
        resp_header.msg_type() != Krpc::MSG_TYPE_PONG ||
        resp_header.request_id() != request_id) {
        return HeartbeatResult::kFatal;
    }

    return HeartbeatResult::kSuccess;
}

void KrpcChannel::HandleHeartbeatFailure(const std::string &reason) {
    {
        std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
        if (m_clientfd != -1) {
            close(m_clientfd);
            m_clientfd = -1;
        }
    }
    KrpcLogger::ERROR(reason.c_str());
    m_heartbeat_cv.notify_all();
}