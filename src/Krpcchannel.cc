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
#include <algorithm>

std::mutex g_data_mutx;  // 全局互斥锁，用于保护共享数据的线程安全

namespace {
std::atomic<uint64_t> g_request_id{1};

uint64_t NextRequestId() {
    return g_request_id.fetch_add(1, std::memory_order_relaxed);
}

using Clock = std::chrono::steady_clock;
struct EndpointCacheEntry {
    std::vector<Endpoint> endpoints;
    Clock::time_point expire_at;
};

const auto kEndpointCacheTtl = std::chrono::milliseconds(5000);
std::atomic<int> g_endpoint_fail_cooldown_ms{3000};
std::mutex g_endpoint_cache_mutex;
std::unordered_map<std::string, EndpointCacheEntry> g_endpoint_cache;

bool TryGetCachedEndpoints(const std::string &key, std::vector<Endpoint> &out) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_endpoint_cache_mutex);
    auto it = g_endpoint_cache.find(key);
    if (it == g_endpoint_cache.end()) {
        return false;
    }
    if (now >= it->second.expire_at) {
        g_endpoint_cache.erase(it);
        return false;
    }
    out = it->second.endpoints;
    return true;
}

void StoreCachedEndpoints(const std::string &key, const std::vector<Endpoint> &endpoints) {
    if (endpoints.empty()) {
        return; // 不缓存空列表，避免挡住后续注册
    }
    EndpointCacheEntry entry;
    entry.endpoints = endpoints;
    entry.expire_at = Clock::now() + kEndpointCacheTtl;
    std::lock_guard<std::mutex> lock(g_endpoint_cache_mutex);
    g_endpoint_cache[key] = std::move(entry);
}

class ConnectionPool {
public:
    static ConnectionPool &Instance() {
        static ConnectionPool pool;
        return pool;
    }

    int Acquire(const std::string &key, const std::string &ip, uint16_t port, std::string *err, bool *reused) {
        if (reused) {
            *reused = false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pools_.find(key);
            if (it != pools_.end()) {
                while (!it->second.idle.empty()) {
                    int fd = it->second.idle.back();
                    it->second.idle.pop_back();
                    if (KrpcChannel::IsConnectionHealthy(fd)) {
                        if (reused) {
                            *reused = true;
                        }
                        return fd;
                    }
                    close(fd);
                }
            }
        }
        int fd = -1;
        if (!KrpcChannel::CreateConnectionFd(ip, port, fd, err)) {
            return -1;
        }
        return fd;
    }

    void Release(const std::string &key, int fd, size_t max_idle) {
        if (fd == -1) {
            return;
        }
        if (!KrpcChannel::IsConnectionHealthy(fd)) {
            close(fd);
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto &entry = pools_[key];
        if (entry.idle.size() >= max_idle) {
            close(fd);
            return;
        }
        entry.idle.push_back(fd);
    }

private:
    struct PoolEntry {
        std::deque<int> idle;
    };
    std::mutex mutex_;
    std::unordered_map<std::string, PoolEntry> pools_;
};

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

bool ParseEndpoint(const std::string &addr, Endpoint &out) {
    const auto pos = addr.find(':');
    if (pos == std::string::npos) {
        return false;
    }
    out.host = addr.substr(0, pos);
    try {
        out.port = static_cast<uint16_t>(std::stoi(addr.substr(pos + 1)));
    } catch (...) {
        return false;
    }
    if (out.port == 0) {
        return false;
    }
    return true;
}

std::once_flag g_static_ep_once;
std::vector<Endpoint> g_static_endpoints;

void InitStaticEndpoints() {
    const char *env_list = std::getenv("LB_STATIC_ENDPOINTS");
    if (env_list == nullptr) {
        return;
    }
    std::string list = env_list;
    size_t start = 0;
    while (start < list.size()) {
        auto comma = list.find(',', start);
        const std::string token = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        Endpoint ep;
        if (ParseEndpoint(token, ep)) {
            g_static_endpoints.push_back(ep);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    LOG(INFO) << "LB_STATIC_ENDPOINTS parsed once, count=" << g_static_endpoints.size();
}

std::vector<Endpoint> GetStaticEndpoints() {
    std::call_once(g_static_ep_once, InitStaticEndpoints);
    return g_static_endpoints;
}

class MetricsAggregator {
public:
    void AddSample(bool success, int64_t cost_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (success) {
            ++success_;
        } else {
            ++fail_;
        }
        samples_.push_back(cost_ms);
        const auto now = Clock::now();
        if (now - last_flush_ >= flush_interval_) {
            FlushLocked(now);
        }
    }

private:
    void FlushLocked(const Clock::time_point &now) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count();
        if (elapsed <= 0) {
            return;
        }
        const int total = success_ + fail_;
        if (total == 0) {
            last_flush_ = now;
            return;
        }
        std::sort(samples_.begin(), samples_.end());
        auto perc = [&](double p) -> int64_t {
            if (samples_.empty()) return 0;
            const size_t idx = static_cast<size_t>((p / 100.0) * samples_.size() + 0.5) - 1;
            return samples_[std::min(idx, samples_.size() - 1)];
        };
        const double qps = (static_cast<double>(total) * 1000.0) / static_cast<double>(elapsed);
        const double fail_rate = total > 0 ? (static_cast<double>(fail_) / total) * 100.0 : 0.0;
        LOG(INFO) << "[metrics] window_ms=" << elapsed
                  << " qps=" << qps
                  << " success=" << success_
                  << " fail=" << fail_
                  << " fail_rate=" << fail_rate << "%"
                  << " p50=" << perc(50)
                  << " p95=" << perc(95)
                  << " p99=" << perc(99)
                  << " max=" << (samples_.empty() ? 0 : samples_.back());
        success_ = fail_ = 0;
        samples_.clear();
        last_flush_ = now;
    }

    std::mutex mutex_;
    int success_{0};
    int fail_{0};
    std::vector<int64_t> samples_;
    Clock::time_point last_flush_{Clock::now()};
    const std::chrono::milliseconds flush_interval_{std::chrono::milliseconds(10000)};
};

MetricsAggregator g_metrics;

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
    pending->service_name = request_service_name;
    pending->method_name = request_method_name;
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
    pending->peer = m_endpoint_key;

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
    int fd = -1;
    if (!CreateConnectionFd(ip, port, fd, errMsg)) {
        return false;
    }
    m_clientfd = fd;  // 保存socket文件描述符
    m_recv_cv.notify_all();
    return true;
}

bool KrpcChannel::CreateConnectionFd(const std::string &ip, uint16_t port, int &out_fd, std::string *errMsg) {
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

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (-1 == connect(clientfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
        close(clientfd);
        char errtxt[512] = {0};
        std::cout << "connect error" << strerror_r(errno, errtxt, sizeof(errtxt)) << std::endl;  // 打印错误信息
        LOG(ERROR) << "connect server error" << errtxt;  // 记录错误日志
        if (errMsg) {
            *errMsg = errtxt;
        }
        return false;
    }

    out_fd = clientfd;
    return true;
}

bool KrpcChannel::IsConnectionHealthy(int fd) {
    if (fd == -1) {
        return false;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR | POLLHUP;
    pfd.revents = 0;
    const int pr = poll(&pfd, 1, 0);
    if (pr < 0) {
        return false;
    }
    if (pfd.revents & (POLLERR | POLLHUP)) {
        return false;
    }
    if (pfd.revents & POLLIN) {
        char buf;
        const ssize_t n = recv(fd, &buf, 1, MSG_PEEK);
        if (n == 0) {
            return false;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
    }
    return true;
}

int KrpcChannel::AcquireConnection(const std::string &ip, uint16_t port, std::string *errMsg, bool *from_pool) {
    if (from_pool) {
        *from_pool = false;
    }
    if (!m_use_pool) {
        int fd = -1;
        if (!CreateConnectionFd(ip, port, fd, errMsg)) {
            return -1;
        }
        return fd;
    }
    const std::string key = ip + ":" + std::to_string(port);
    return ConnectionPool::Instance().Acquire(key, ip, port, errMsg, from_pool);
}

void KrpcChannel::ReleaseConnection(bool healthy) {
    if (m_clientfd == -1) {
        return;
    }
    int fd = m_clientfd;
    m_clientfd = -1;
    m_recv_buffer.clear();
    if (!healthy) {
        close(fd);
        return;
    }
    if (!m_use_pool) {
        close(fd);
        return;
    }
    // 连接池健康检查：当前阶段先信任活跃连接，后续可启用更严格检测
    ConnectionPool::Instance().Release(m_endpoint_key, fd, static_cast<size_t>(m_pool_max_idle));
}

void KrpcChannel::CloseConnectionLocked() {
    if (m_clientfd != -1) {
        close(m_clientfd);
        m_clientfd = -1;
    }
}

// 从ZooKeeper查询服务地址列表
std::vector<Endpoint> KrpcChannel::QueryServiceNodes(ZkClient *zkclient, const std::string &service_name, const std::string &method_name) {
    std::vector<Endpoint> endpoints;
    std::string method_path = "/" + service_name + "/" + method_name;
    std::cout << "method_path: " << method_path << std::endl;

    std::vector<std::string> children;
    {
        std::unique_lock<std::mutex> lock(g_data_mutx);
        children = zkclient->GetChildren(method_path.c_str());
    }
    if (!children.empty()) {
        LOG(INFO) << "zk children under " << method_path << ": " << children.size();
    } else {
        LOG(WARNING) << "zk children empty under " << method_path;
    }
    for (const auto &child : children) {
        LOG(INFO) << "child node: " << child;
        Endpoint ep;
        if (ParseEndpoint(child, ep)) {
            endpoints.push_back(ep);
        }
    }

    // 兼容旧格式：直接在方法节点数据存放 "ip:port"
    if (endpoints.empty()) {
        std::unique_lock<std::mutex> lock(g_data_mutx);
        std::string host_data = zkclient->GetData(method_path.c_str());
        lock.unlock();
        Endpoint ep;
        if (ParseEndpoint(host_data, ep)) {
            LOG(INFO) << "fallback to method data: " << host_data;
            endpoints.push_back(ep);
        } else {
            LOG(ERROR) << method_path + " is not exist or invalid!";
        }
    }

    return endpoints;
}

bool KrpcChannel::EnsureConnection(const ::google::protobuf::MethodDescriptor *method,
                                   ::google::protobuf::RpcController *controller,
                                   std::string *error_text) {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    const int discovery_retry = ParseConfigInt(config.Load("discovery_retry"), 3);
    const int discovery_retry_interval_ms = ParseConfigInt(config.Load("discovery_retry_interval_ms"), 200);
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
    const std::string cache_key = service_name + "/" + method_name;

    std::unique_lock<std::mutex> socket_lock(m_socket_mutex);
    socket_lock.unlock();

    std::vector<Endpoint> endpoints = GetStaticEndpoints();
    std::unique_ptr<ZkClient> zkCli;
    if (endpoints.empty()) {
        if (!TryGetCachedEndpoints(cache_key, endpoints)) {
            zkCli.reset(new ZkClient());
            zkCli->Start();
            endpoints = QueryServiceNodes(zkCli.get(), service_name, method_name);
            StoreCachedEndpoints(cache_key, endpoints);
        } else {
            LOG(INFO) << "use cached endpoints for " << cache_key << ", count=" << endpoints.size();
        }
    } else {
        LOG(INFO) << "use static endpoints from env, count=" << endpoints.size();
    }

    for (int attempt = 1; endpoints.empty() && attempt <= discovery_retry; ++attempt) {
        LOG(WARNING) << "service node not found, retry discovery attempt " << attempt << "/" << discovery_retry;
        std::this_thread::sleep_for(std::chrono::milliseconds(discovery_retry_interval_ms));
        if (!zkCli) {
            zkCli.reset(new ZkClient());
            zkCli->Start();
        }
        endpoints = QueryServiceNodes(zkCli.get(), service_name, method_name);
        StoreCachedEndpoints(cache_key, endpoints);
    }

    if (endpoints.empty()) {
        const std::string reason = "service node not found: " + service_name + "/" + method_name;
        if (controller) {
            controller->SetFailed(reason);
        }
        if (error_text) {
            *error_text = reason;
        }
        return false;
    }

    if (m_lb) {
        m_lb->UpdateNodes(endpoints);
    }
    LOG(INFO) << "lb endpoints count=" << endpoints.size();
    Endpoint selected;
    if (!m_lb || !m_lb->Select(method_name, selected)) {
        selected = endpoints.front();
    }
    LOG(INFO) << "lb selected endpoint=" << selected.host << ":" << selected.port;

    size_t start_index = 0;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (endpoints[i].host == selected.host && endpoints[i].port == selected.port) {
            start_index = i;
            break;
        }
    }

    std::string last_error;
    Clock::time_point earliest_retry = Clock::time_point::max();
    size_t earliest_index = start_index;
    const auto now = Clock::now();

    for (size_t i = 0; i < endpoints.size(); ++i) {
        const size_t idx = (start_index + i) % endpoints.size();
        const auto &candidate = endpoints[idx];

        Clock::time_point retry_at{};
        if (!EndpointAvailable(candidate, now, retry_at)) {
            if (retry_at < earliest_retry) {
                earliest_retry = retry_at;
                earliest_index = idx;
            }
            continue;
        }

        socket_lock.lock();
        const bool already_ok = (m_clientfd != -1 && candidate.host == m_ip && candidate.port == m_port);
        if (already_ok) {
            socket_lock.unlock();
            return true;
        }
        if (m_clientfd != -1) {
            ReleaseConnection(true);
        }
        m_ip = candidate.host;
        m_port = candidate.port;
        m_endpoint_key = m_ip + ":" + std::to_string(m_port);
        socket_lock.unlock();

        std::string connect_error;
        bool from_pool = false;
        int fd = AcquireConnection(m_ip, m_port, &connect_error, &from_pool);
        if (fd != -1) {
            socket_lock.lock();
            m_clientfd = fd;
            socket_lock.unlock();
            ClearEndpointFailure(candidate);
            if (from_pool) {
                LOG(INFO) << "reuse pooled connection to " << m_ip << ":" << m_port;
            } else {
                LOG(INFO) << "connect server success to " << m_ip << ":" << m_port;
            }
            return true;
        }

        last_error = connect_error.empty() ? "connect server error" : connect_error;
        MarkEndpointFailure(candidate);
        LOG(WARNING) << "endpoint " << candidate.host << ":" << candidate.port << " unavailable: " << last_error;
    }

    // 全部处于冷却时，尝试最早可重试的端点（或默认起点）
    const auto &fallback = endpoints[earliest_index];
    socket_lock.lock();
    const bool already_ok = (m_clientfd != -1 && fallback.host == m_ip && fallback.port == m_port);
    if (already_ok) {
        socket_lock.unlock();
        return true;
    }
    if (m_clientfd != -1) {
        ReleaseConnection(true);
    }
    m_ip = fallback.host;
    m_port = fallback.port;
    m_endpoint_key = m_ip + ":" + std::to_string(m_port);
    socket_lock.unlock();

    std::string connect_error;
    bool from_pool = false;
    int fd = AcquireConnection(m_ip, m_port, &connect_error, &from_pool);
    if (fd != -1) {
        socket_lock.lock();
        m_clientfd = fd;
        socket_lock.unlock();
        ClearEndpointFailure(fallback);
        if (from_pool) {
            LOG(INFO) << "reuse pooled connection to " << m_ip << ":" << m_port;
        } else {
            LOG(INFO) << "connect server success to " << m_ip << ":" << m_port;
        }
        return true;
    }

    last_error = connect_error.empty() ? "connect server error" : connect_error;
    MarkEndpointFailure(fallback);

    if (controller) {
        controller->SetFailed(last_error.empty() ? "connect server error" : last_error);
    }
    if (error_text) {
        *error_text = last_error.empty() ? "connect server error" : last_error;
    }
    return false;
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
            auto rc = strerror_r(errno, errtxt, sizeof(errtxt));
            (void)rc;
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
            m_pool_max_idle(4),
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
    m_pool_max_idle = ParseConfigInt(config.Load("connection_pool_max_idle"), 4);
    m_use_pool = ParseConfigInt(config.Load("enable_connection_pool"), 1) != 0;
    int lb_fail_cool_ms = ParseConfigInt(config.Load("lb_fail_cooldown_ms"), 3000);
    if (lb_fail_cool_ms < 0) {
        lb_fail_cool_ms = 0;
    }
    g_endpoint_fail_cooldown_ms.store(lb_fail_cool_ms, std::memory_order_release);
    if (m_pool_max_idle < 1) {
        m_pool_max_idle = 1;
    }
    m_lb = std::unique_ptr<ILoadBalancer>(new RoundRobinLoadBalancer());

    StartSendThread();
    StartRecvThread();
    StartHeartbeatThread();
    StartTimeoutThread();

    if (!connectNow) {  // 如果不需要立即连接
        return;
    }

    // 尝试连接服务器，最多重试3次
    std::string errtxt;
    m_endpoint_key = m_ip + ":" + std::to_string(m_port);
    bool from_pool = false;
    auto fd = AcquireConnection(m_ip, m_port, &errtxt, &from_pool);
    bool rt = (fd != -1);
    int count = 3;  // 重试次数
    while (!rt && count--) {
        fd = AcquireConnection(m_ip, m_port, &errtxt, &from_pool);
        rt = (fd != -1);
    }
    if (rt) {
        m_clientfd = fd;
        if (from_pool) {
            LOG(INFO) << "reuse pooled connection";
        } else {
            LOG(INFO) << "connect server success";
        }
    }
}

KrpcChannel::~KrpcChannel() {
    StopTimeoutThread();
    StopSendThread();
    StopHeartbeatThread();
    StopRecvThread();
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    if (m_clientfd != -1) {
        ReleaseConnection(true);
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
    if (m_recv_thread.joinable()) {
        if (m_recv_thread.get_id() == std::this_thread::get_id()) {
            // 避免在线程自身上调用 join 导致的 deadlock 异常
            m_recv_thread.detach();
        } else {
            m_recv_thread.join();
        }
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
    // 确保异步场景下 RecvLoop 运行期间对象不会在回调里被销毁
    std::shared_ptr<KrpcChannel> self_guard;
    for (int i = 0; i < 3 && !self_guard; ++i) {
        try {
            self_guard = shared_from_this();
        } catch (const std::bad_weak_ptr &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

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
    const auto end_time = std::chrono::steady_clock::now();
    const auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - pending->start_time).count();

    if (!success && pending->controller && !reason.empty()) {
        pending->controller->SetFailed(reason);
    }
    const std::string peer = pending->peer.empty() ? m_endpoint_key : pending->peer;
    if (success) {
        LOG(INFO) << "[req " << pending->request_id << "] "
                  << pending->service_name << "." << pending->method_name
                  << " cost=" << cost_ms << "ms "
                  << "peer=" << peer
                  << " status=OK";
    } else {
        LOG(WARNING) << "[req " << pending->request_id << "] "
                     << pending->service_name << "." << pending->method_name
                     << " cost=" << cost_ms << "ms "
                     << "peer=" << peer
                     << " status=FAIL err=" << reason;
    }
    g_metrics.AddSample(success, cost_ms);

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

bool KrpcChannel::EndpointAvailable(const Endpoint &ep, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point &next_retry) {
    std::lock_guard<std::mutex> lock(m_fail_mutex);
    const std::string key = ep.host + ":" + std::to_string(ep.port);
    auto it = m_fail_states.find(key);
    if (it == m_fail_states.end()) {
        return true;
    }
    next_retry = it->second.retry_at;
    return now >= it->second.retry_at;
}

void KrpcChannel::MarkEndpointFailure(const Endpoint &ep) {
    std::lock_guard<std::mutex> lock(m_fail_mutex);
    const std::string key = ep.host + ":" + std::to_string(ep.port);
    const auto cooldown = std::chrono::milliseconds(g_endpoint_fail_cooldown_ms.load(std::memory_order_acquire));
    m_fail_states[key].retry_at = Clock::now() + cooldown;
}

void KrpcChannel::ClearEndpointFailure(const Endpoint &ep) {
    std::lock_guard<std::mutex> lock(m_fail_mutex);
    const std::string key = ep.host + ":" + std::to_string(ep.port);
    m_fail_states.erase(key);
}
