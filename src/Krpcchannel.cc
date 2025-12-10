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
#include <vector>
#include <utility>

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
    auto pending = CallFuture(method, controller, request, response, done, AsyncCallback{});
    if (!pending) {
        return;
    }

    if (done != nullptr) {
        // 完全异步调用由回调完成
        return;
    }

    if (!pending->completion_future.valid()) {
        return;
    }

    auto wait_ms = pending->timeout_ms > 0 ? pending->timeout_ms : m_request_timeout_ms;
    auto status = pending->completion_future.wait_for(std::chrono::milliseconds(wait_ms));
    if (status == std::future_status::timeout) {
        RemovePendingCall(pending->request_id, "timeout");
        return;
    }
    pending->completion_future.wait();
}

void KrpcChannel::CallAsync(const ::google::protobuf::MethodDescriptor *method,
                            ::google::protobuf::RpcController *controller,
                            const ::google::protobuf::Message *request,
                            ::google::protobuf::Message *response,
                            AsyncCallback callback) {
    HeartbeatActivityNotifier heartbeat_notifier(m_heartbeat_cv);
    (void)CallFuture(method, controller, request, response, nullptr, std::move(callback));
}

std::shared_ptr<KrpcChannel::PendingCall> KrpcChannel::CallFuture(
        const ::google::protobuf::MethodDescriptor *method,
        ::google::protobuf::RpcController *controller,
        const ::google::protobuf::Message *request,
        ::google::protobuf::Message *response,
        ::google::protobuf::Closure *done,
        AsyncCallback callback) {
    AsyncCallback user_callback = std::move(callback);
    auto fail_immediately = [&](const std::string &reason) -> std::shared_ptr<PendingCall> {
        if (controller && !reason.empty()) {
            controller->SetFailed(reason);
        }
        if (done) {
            done->Run();
        }
        if (user_callback) {
            user_callback(controller, response);
        }
        return nullptr;
    };

    if (method == nullptr || request == nullptr || response == nullptr) {
        return fail_immediately("invalid call parameters");
    }

    auto pending = std::make_shared<PendingCall>();
    pending->response = response;
    pending->controller = controller;
    pending->done = done;
    pending->timeout_ms = m_request_timeout_ms;
    if (controller) {
        if (auto *krpc_controller = dynamic_cast<Krpccontroller *>(controller)) {
            if (krpc_controller->TimeoutMs() > 0) {
                pending->timeout_ms = krpc_controller->TimeoutMs();
            }
        }
    }
    pending->start_time = std::chrono::steady_clock::now();
    pending->completion_future = pending->promise.get_future().share();

    std::string args_str;
    if (!request->SerializeToString(&args_str)) {
        return fail_immediately("serialize request fail");
    }

    Krpc::RpcHeader krpcheader;
    krpcheader.set_magic(KrpcProtocol::kDefaultMagic);
    krpcheader.set_version(KrpcProtocol::kDefaultVersion);
    krpcheader.set_msg_type(Krpc::MSG_TYPE_REQUEST);
    krpcheader.set_body_size(static_cast<uint32_t>(args_str.size()));
    krpcheader.set_compress_type(Krpc::COMPRESS_NONE);

    const google::protobuf::ServiceDescriptor *sd = method->service();
    if (sd == nullptr) {
        return fail_immediately("service descriptor missing");
    }
    const std::string request_service_name = sd->name();
    const std::string request_method_name = method->name();
    krpcheader.set_service_name(request_service_name);
    krpcheader.set_method_name(request_method_name);

    const uint64_t request_id = NextRequestId();
    pending->request_id = request_id;
    krpcheader.set_request_id(request_id);

    std::string rpc_header_str;
    if (!krpcheader.SerializeToString(&rpc_header_str)) {
        return fail_immediately("serialize rpc header error");
    }

    std::string send_rpc_str;
    {
        google::protobuf::io::StringOutputStream string_output(&send_rpc_str);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);
        coded_output.WriteVarint32(static_cast<uint32_t>(rpc_header_str.size()));
        coded_output.WriteString(rpc_header_str);
    }
    send_rpc_str += args_str;

    std::string ensure_error;
    if (!EnsureConnection(method, controller, &ensure_error)) {
        return fail_immediately(ensure_error);
    }

    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        m_pending_calls[pending->request_id] = pending;
    }
    m_timeout_cv.notify_all();

    pending->async_callback = std::move(user_callback);

    EnqueueSend(pending->request_id, std::move(send_rpc_str));

    return pending;
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
    m_recv_cv.notify_all();
    return true;
}

void KrpcChannel::CloseConnectionLocked() {
    if (m_clientfd != -1) {
        close(m_clientfd);
        m_clientfd = -1;
    }
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

bool KrpcChannel::EnsureConnection(const ::google::protobuf::MethodDescriptor *method,
                                   ::google::protobuf::RpcController *controller,
                                   std::string *error_text) {
    const google::protobuf::ServiceDescriptor *sd = method->service();
    if (sd == nullptr) {
        if (controller) {
            controller->SetFailed("service descriptor missing");
        }
        if (error_text) {
            *error_text = "service descriptor missing";
        }
        return false;
    }

    const std::string service_name = sd->name();
    const std::string method_name = method->name();

    std::unique_lock<std::mutex> socket_lock(m_socket_mutex);
    if (m_clientfd != -1) {
        return true;
    }
    socket_lock.unlock();

    ZkClient zkCli;
    zkCli.Start();
    std::string host_data = QueryServiceHost(&zkCli, service_name, method_name, m_idx);
    if (host_data.empty()) {
        const std::string reason = "service node not found: " + service_name + "/" + method_name;
        if (controller) {
            controller->SetFailed(reason);
        }
        if (error_text) {
            *error_text = reason;
        }
        return false;
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
            if (error_text) {
                *error_text = connect_error.empty() ? "connect server error" : connect_error;
            }
            CloseConnectionLocked();
            return false;
        }
        LOG(INFO) << "connect server success";
    }
    return true;
}

bool KrpcChannel::SendBuffer(const std::string &buffer, std::string *error_text) {
    std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
    size_t total_sent = 0;
    while (total_sent < buffer.size()) {
        if (m_clientfd == -1) {
            if (error_text) {
                *error_text = "connection not established";
            }
            return false;
        }
        ssize_t n = send(m_clientfd, buffer.data() + total_sent, buffer.size() - total_sent, 0);
        if (n <= 0) {
            if (errno == EINTR) {
                continue;
            }
            char errtxt[512] = {};
            strerror_r(errno, errtxt, sizeof(errtxt));
            if (error_text) {
                *error_text = errtxt;
            }
            CloseConnectionLocked();
            return false;
        }
        total_sent += static_cast<size_t>(n);
    }
    return true;
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
            m_last_pong_time(std::chrono::steady_clock::now()),
            m_recv_thread(),
            m_recv_running(false),
            m_recv_thread_started(false),
        m_send_queue(),
        m_send_thread(),
        m_send_running(false),
        m_send_thread_started(false),
            m_waiting_heartbeat(false),
            m_waiting_heartbeat_id(0),
        m_waiting_heartbeat_result(false),
        m_timeout_thread(),
        m_timeout_running(false),
        m_timeout_thread_started(false) {

    auto &config = KrpcApplication::GetInstance().GetConfig();
    m_request_timeout_ms = ParseConfigInt(config.Load("rpc_timeout_ms"), KrpcProtocol::kDefaultRequestTimeoutMs);
    m_heartbeat_interval_ms = ParseConfigInt(config.Load("heartbeat_interval_ms"), KrpcProtocol::kDefaultHeartbeatIntervalMs);
    m_heartbeat_miss_limit = ParseConfigInt(config.Load("heartbeat_miss_limit"), KrpcProtocol::kDefaultHeartbeatMissLimit);

    StartSendThread();
    StartRecvThread();
    StartHeartbeatThread();
    StartTimeoutThread();

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
    StopTimeoutThread();
    StopSendThread();
    StopHeartbeatThread();
    StopRecvThread();
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

void KrpcChannel::StartSendThread() {
    if (m_send_thread_started) {
        return;
    }
    m_send_running.store(true, std::memory_order_release);
    m_send_thread = std::thread(&KrpcChannel::SendLoop, this);
    m_send_thread_started = true;
}

void KrpcChannel::StopSendThread() {
    if (!m_send_thread_started) {
        return;
    }
    m_send_running.store(false, std::memory_order_release);
    m_send_cv.notify_all();
    if (m_send_thread.joinable()) {
        m_send_thread.join();
    }
    m_send_thread_started = false;
}

void KrpcChannel::StartRecvThread() {
    if (m_recv_thread_started) {
        return;
    }
    m_recv_running.store(true, std::memory_order_release);
    m_recv_thread = std::thread(&KrpcChannel::RecvLoop, this);
    m_recv_thread_started = true;
}

void KrpcChannel::StopRecvThread() {
    if (!m_recv_thread_started) {
        return;
    }
    m_recv_running.store(false, std::memory_order_release);
    m_recv_cv.notify_all();
    {
        std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
        if (m_clientfd != -1) {
            shutdown(m_clientfd, SHUT_RDWR);
        }
    }
    if (m_recv_thread.joinable()) {
        m_recv_thread.join();
    }
    m_recv_thread_started = false;
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

    {
        std::lock_guard<std::mutex> wait_lock(m_heartbeat_wait_mutex);
        m_waiting_heartbeat = true;
        m_waiting_heartbeat_id = request_id;
        m_waiting_heartbeat_result = false;
    }

    if (-1 == send(m_clientfd, send_buf.c_str(), send_buf.size(), 0)) {
        {
            std::lock_guard<std::mutex> wait_lock(m_heartbeat_wait_mutex);
            m_waiting_heartbeat = false;
        }
        return HeartbeatResult::kFatal;
    }

    socket_lock.unlock();

    std::unique_lock<std::mutex> wait_lock(m_heartbeat_wait_mutex);
    const bool got = m_heartbeat_wait_cv.wait_for(
            wait_lock,
            std::chrono::milliseconds(m_request_timeout_ms),
            [this] { return !m_waiting_heartbeat; });
    if (!got) {
        m_waiting_heartbeat = false;
        return HeartbeatResult::kTimeout;
    }

    return m_waiting_heartbeat_result ? HeartbeatResult::kSuccess : HeartbeatResult::kFatal;
}

void KrpcChannel::HandleHeartbeatFailure(const std::string &reason) {
    {
        std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
        CloseConnectionLocked();
    }
    std::vector<uint64_t> queued_ids;
    {
        std::lock_guard<std::mutex> send_lock(m_send_mutex);
        while (!m_send_queue.empty()) {
            queued_ids.push_back(m_send_queue.front().request_id);
            m_send_queue.pop();
        }
    }
    for (auto request_id : queued_ids) {
        RemovePendingCall(request_id, reason);
    }
    KrpcLogger::ERROR(reason.c_str());
    FailPendingCalls(reason);
    {
        std::lock_guard<std::mutex> wait_lock(m_heartbeat_wait_mutex);
        if (m_waiting_heartbeat) {
            m_waiting_heartbeat = false;
            m_waiting_heartbeat_result = false;
        }
    }
    m_heartbeat_wait_cv.notify_all();
    m_heartbeat_cv.notify_all();
    m_recv_cv.notify_all();
    m_send_cv.notify_all();
}

void KrpcChannel::SendLoop() {
    while (true) {
        SendTask task;
        {
            std::unique_lock<std::mutex> lock(m_send_mutex);
            m_send_cv.wait(lock, [this] {
                return !m_send_running.load(std::memory_order_acquire) || !m_send_queue.empty();
            });
            if (!m_send_running.load(std::memory_order_acquire) && m_send_queue.empty()) {
                break;
            }
            task = std::move(m_send_queue.front());
            m_send_queue.pop();
        }

        std::string send_error;
        if (!SendBuffer(task.buffer, &send_error)) {
            const std::string reason = send_error.empty() ? "send failed" : send_error;
            RemovePendingCall(task.request_id, reason);
            HandleHeartbeatFailure(reason);
        }
    }
}

void KrpcChannel::RecvLoop() {
    while (m_recv_running.load(std::memory_order_acquire)) {
        int current_fd = -1;
        {
            std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
            current_fd = m_clientfd;
        }

        if (current_fd == -1) {
            std::unique_lock<std::mutex> wait_lock(m_recv_mutex);
            m_recv_cv.wait_for(wait_lock, std::chrono::milliseconds(100), [this] {
                return !m_recv_running.load(std::memory_order_acquire) || m_clientfd != -1;
            });
            continue;
        }

        struct pollfd pfd;
        pfd.fd = current_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int poll_ret = poll(&pfd, 1, 200);
        if (!m_recv_running.load(std::memory_order_acquire)) {
            break;
        }
        if (poll_ret <= 0) {
            if (poll_ret < 0 && errno != EINTR) {
                HandleHeartbeatFailure("recv poll failed");
            }
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }

        char recv_buf[4096];
        const int recv_size = recv(current_fd, recv_buf, sizeof(recv_buf), 0);
        if (recv_size <= 0) {
            if (recv_size == 0 || (errno != EINTR && errno != EAGAIN)) {
                HandleHeartbeatFailure("connection closed");
            }
            continue;
        }

        m_recv_buffer.append(recv_buf, recv_size);

        bool parsed = true;
        while (parsed) {
            parsed = false;
            if (m_recv_buffer.empty()) {
                break;
            }

            google::protobuf::io::ArrayInputStream resp_input(m_recv_buffer.data(), static_cast<int>(m_recv_buffer.size()));
            google::protobuf::io::CodedInputStream resp_coded(&resp_input);
            uint32_t resp_header_size{};
            if (!resp_coded.ReadVarint32(&resp_header_size)) {
                break;
            }
            const size_t after_varint = static_cast<size_t>(resp_coded.CurrentPosition());
            if (m_recv_buffer.size() < after_varint + resp_header_size) {
                break;
            }

            std::string resp_header_str;
            auto resp_limit = resp_coded.PushLimit(resp_header_size);
            bool header_ok = resp_coded.ReadString(&resp_header_str, resp_header_size);
            resp_coded.PopLimit(resp_limit);
            if (!header_ok) {
                HandleHeartbeatFailure("read response header error");
                m_recv_buffer.clear();
                break;
            }

            Krpc::RpcHeader resp_header;
            if (!resp_header.ParseFromString(resp_header_str)) {
                HandleHeartbeatFailure("parse response header error");
                m_recv_buffer.clear();
                break;
            }

            const uint32_t body_size = resp_header.body_size();
            const size_t required = static_cast<size_t>(resp_coded.CurrentPosition()) + body_size;
            if (m_recv_buffer.size() < required) {
                break;
            }

            std::string response_payload;
            if (body_size > 0) {
                if (!resp_coded.ReadString(&response_payload, body_size)) {
                    HandleHeartbeatFailure("read response payload error");
                    m_recv_buffer.clear();
                    break;
                }
            }

            const size_t consumed = static_cast<size_t>(resp_coded.CurrentPosition());
            m_recv_buffer.erase(0, consumed);
            parsed = true;

            if (resp_header.magic() != KrpcProtocol::kDefaultMagic) {
                HandleHeartbeatFailure("invalid response magic");
                break;
            }

            if (resp_header.msg_type() == Krpc::MSG_TYPE_RESPONSE) {
                ResolvePendingCall(resp_header, response_payload);
            } else if (resp_header.msg_type() == Krpc::MSG_TYPE_PONG) {
                ResolveHeartbeat(resp_header);
            }
        }
    }
}

void KrpcChannel::StartTimeoutThread() {
    if (m_timeout_thread_started) {
        return;
    }
    m_timeout_running.store(true, std::memory_order_release);
    m_timeout_thread = std::thread(&KrpcChannel::TimeoutLoop, this);
    m_timeout_thread_started = true;
}

void KrpcChannel::StopTimeoutThread() {
    if (!m_timeout_thread_started) {
        return;
    }
    m_timeout_running.store(false, std::memory_order_release);
    m_timeout_cv.notify_all();
    if (m_timeout_thread.joinable()) {
        m_timeout_thread.join();
    }
    m_timeout_thread_started = false;
}

void KrpcChannel::TimeoutLoop() {
    const auto interval = std::chrono::milliseconds(50);
    while (m_timeout_running.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(m_timeout_mutex);
            m_timeout_cv.wait_for(lock, interval);
            if (!m_timeout_running.load(std::memory_order_acquire)) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<PendingCall>> expired;
        {
            std::lock_guard<std::mutex> lock(m_pending_mutex);
            for (auto it = m_pending_calls.begin(); it != m_pending_calls.end();) {
                const auto &pending = it->second;
                if (pending->timeout_ms > 0 &&
                    now - pending->start_time >= std::chrono::milliseconds(pending->timeout_ms)) {
                    expired.emplace_back(pending);
                    it = m_pending_calls.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto &pending : expired) {
            CompletePending(pending, "timeout", false);
        }
    }
}

void KrpcChannel::EnqueueSend(uint64_t request_id, std::string &&buffer) {
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        SendTask task;
        task.request_id = request_id;
        task.buffer = std::move(buffer);
        m_send_queue.push(std::move(task));
    }
    m_send_cv.notify_one();
}

void KrpcChannel::ResolvePendingCall(const Krpc::RpcHeader &header, const std::string &payload) {
    std::shared_ptr<PendingCall> pending;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        auto it = m_pending_calls.find(header.request_id());
        if (it == m_pending_calls.end()) {
            return;
        }
        pending = it->second;
        m_pending_calls.erase(it);
    }

    if (pending->response != nullptr) {
        if (!pending->response->ParseFromString(payload)) {
            CompletePending(pending, "parse response payload error", false);
            return;
        }
    }

    CompletePending(pending, std::string(), true);
}

void KrpcChannel::ResolveHeartbeat(const Krpc::RpcHeader &header) {
    std::lock_guard<std::mutex> wait_lock(m_heartbeat_wait_mutex);
    if (m_waiting_heartbeat && header.request_id() == m_waiting_heartbeat_id) {
        m_waiting_heartbeat = false;
        m_waiting_heartbeat_result = true;
        m_last_pong_time = std::chrono::steady_clock::now();
        m_heartbeat_wait_cv.notify_all();
    }
}

void KrpcChannel::RemovePendingCall(uint64_t request_id, const std::string &reason) {
    std::shared_ptr<PendingCall> pending;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        auto it = m_pending_calls.find(request_id);
        if (it == m_pending_calls.end()) {
            return;
        }
        pending = it->second;
        m_pending_calls.erase(it);
    }

    CompletePending(pending, reason, false);
}

void KrpcChannel::FailPendingCalls(const std::string &reason) {
    std::vector<std::shared_ptr<PendingCall>> pendings;
    {
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        for (auto &entry : m_pending_calls) {
            pendings.emplace_back(entry.second);
        }
        m_pending_calls.clear();
    }

    for (auto &pending : pendings) {
        CompletePending(pending, reason, false);
    }
}

void KrpcChannel::CompletePending(const std::shared_ptr<PendingCall> &pending, const std::string &reason, bool success) {
    if (!pending) {
        return;
    }
    if (!success && pending->controller && !reason.empty()) {
        pending->controller->SetFailed(reason);
    }
    try {
        pending->promise.set_value();
    } catch (const std::future_error &) {
    }
    if (pending->done) {
        pending->done->Run();
    }
    if (pending->async_callback) {
        pending->async_callback(pending->controller, pending->response);
    }
}