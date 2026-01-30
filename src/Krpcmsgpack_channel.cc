#include "Krpcmsgpack_channel.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Krpcapplication.h"
#include "KrpcLogger.h"
#include "Krpcprotocol.h"
#include "zookeeperutil.h"

namespace {

std::atomic<uint64_t> g_request_id{1};
std::mutex g_data_mutex;

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
        return;
    }
    EndpointCacheEntry entry;
    entry.endpoints = endpoints;
    entry.expire_at = Clock::now() + kEndpointCacheTtl;
    std::lock_guard<std::mutex> lock(g_endpoint_cache_mutex);
    g_endpoint_cache[key] = std::move(entry);
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
}

std::vector<Endpoint> GetStaticEndpoints() {
    std::call_once(g_static_ep_once, InitStaticEndpoints);
    return g_static_endpoints;
}

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

bool CreateConnectionFd(const std::string &ip, uint16_t port, int &out_fd, std::string *errMsg) {
    int clientfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientfd == -1) {
        if (errMsg) {
            *errMsg = std::strerror(errno);
        }
        return false;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (-1 == ::connect(clientfd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr))) {
        ::close(clientfd);
        if (errMsg) {
            *errMsg = std::strerror(errno);
        }
        return false;
    }

    int flag = 1;
    ::setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    out_fd = clientfd;
    return true;
}

bool IsConnectionHealthy(int fd) {
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
                    if (IsConnectionHealthy(fd)) {
                        if (reused) {
                            *reused = true;
                        }
                        return fd;
                    }
                    ::close(fd);
                }
            }
        }
        int fd = -1;
        if (!CreateConnectionFd(ip, port, fd, err)) {
            return -1;
        }
        return fd;
    }

    void Release(const std::string &key, int fd, size_t max_idle) {
        if (fd == -1) {
            return;
        }
        if (!IsConnectionHealthy(fd)) {
            ::close(fd);
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto &entry = pools_[key];
        if (entry.idle.size() >= max_idle) {
            ::close(fd);
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

bool ParseFrame(std::string &buffer, std::string &out) {
    if (buffer.size() < sizeof(uint32_t)) {
        return false;
    }
    uint32_t net_len = 0;
    std::memcpy(&net_len, buffer.data(), sizeof(net_len));
    const uint32_t len = ntohl(net_len);
    if (buffer.size() < sizeof(uint32_t) + len) {
        return false;
    }
    out.assign(buffer.data() + sizeof(uint32_t), len);
    buffer.erase(0, sizeof(uint32_t) + len);
    return true;
}

std::string BuildHeader(const krpc::msgpack::sbuffer &payload) {
    const uint32_t len = static_cast<uint32_t>(payload.size());
    const uint32_t net_len = htonl(len);
    std::string frame;
    frame.resize(sizeof(uint32_t));
    std::memcpy(&frame[0], &net_len, sizeof(net_len));
    return frame;
}

} // namespace

uint64_t KrpcMsgpackChannel::NextRequestId() {
    return g_request_id.fetch_add(1, std::memory_order_relaxed);
}

KrpcMsgpackChannel::KrpcMsgpackChannel() {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    m_ip = config.Load("rpcserverip");
    m_port = static_cast<uint16_t>(std::atoi(config.Load("rpcserverport").c_str()));
    m_use_pool = ParseConfigInt(config.Load("enable_connection_pool"), 1) != 0;
    m_pool_max_idle = ParseConfigInt(config.Load("connection_pool_max_idle"), 4);
    m_request_timeout_ms = ParseConfigInt(config.Load("rpc_timeout_ms"), KrpcProtocol::kDefaultRequestTimeoutMs);
    m_heartbeat_interval_ms = ParseConfigInt(config.Load("heartbeat_interval_ms"),
                                             KrpcProtocol::kDefaultHeartbeatIntervalMs);
    m_heartbeat_miss_limit = ParseConfigInt(config.Load("heartbeat_miss_limit"),
                                            KrpcProtocol::kDefaultHeartbeatMissLimit);
    if (m_request_timeout_ms < 0) {
        m_request_timeout_ms = 0;
    }
    if (m_heartbeat_interval_ms <= 0) {
        m_heartbeat_interval_ms = KrpcProtocol::kDefaultHeartbeatIntervalMs;
    }
    if (m_heartbeat_miss_limit <= 0) {
        m_heartbeat_miss_limit = KrpcProtocol::kDefaultHeartbeatMissLimit;
    }
    int lb_fail_cool_ms = ParseConfigInt(config.Load("lb_fail_cooldown_ms"), 3000);
    if (lb_fail_cool_ms < 0) {
        lb_fail_cool_ms = 0;
    }
    g_endpoint_fail_cooldown_ms.store(lb_fail_cool_ms, std::memory_order_release);
    if (m_pool_max_idle < 1) {
        m_pool_max_idle = 1;
    }
    m_lb = std::unique_ptr<ILoadBalancer>(new RoundRobinLoadBalancer());
    StartHeartbeatThread();
    StartTimeoutThread();
    StartSendThread();
}

KrpcMsgpackChannel::KrpcMsgpackChannel(const std::string &ip, uint16_t port)
    : KrpcMsgpackChannel() {
    m_ip = ip;
    m_port = port;
}

KrpcMsgpackChannel::~KrpcMsgpackChannel() {
    StopTimeoutThread();
    StopHeartbeatThread();
    StopSendThread();
    StopRecvThread();
    std::lock_guard<std::mutex> lock(m_socket_mutex);
    if (socket_fd_ != -1) {
        ReleaseConnection(IsConnectionHealthy(socket_fd_));
    }
    FailAllPending("channel closed");
}

void KrpcMsgpackChannel::StartRecvThread() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);
    recv_thread_ = std::thread(&KrpcMsgpackChannel::RecvLoop, this);
}

void KrpcMsgpackChannel::StopRecvThread() {
    running_.store(false, std::memory_order_release);
    if (socket_fd_ != -1) {
        ::shutdown(socket_fd_, SHUT_RDWR);
    }
    if (recv_thread_.joinable()) {
        if (recv_thread_.get_id() == std::this_thread::get_id()) {
            recv_thread_.detach();
        } else {
            recv_thread_.join();
        }
    }
}

void KrpcMsgpackChannel::RecvLoop() {
    char buffer[4096];
    bool recv_error = false;
    while (running_.load(std::memory_order_acquire)) {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(m_socket_mutex);
            fd = socket_fd_;
        }
        if (fd == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            recv_error = true;
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            recv_error = true;
            break;
        }
        recv_buffer_.append(buffer, static_cast<size_t>(n));
        std::string payload;
        while (ParseFrame(recv_buffer_, payload)) {
            krpc::msgpack::object_handle oh = krpc::msgpack::unpack(payload.data(), payload.size());
            using frame_t = std::tuple<krpc::MsgpackHeader, krpc::msgpack::object>;
            frame_t frame;
            try {
                oh.get().convert(frame);
            } catch (...) {
                continue;
            }
            const krpc::MsgpackHeader &header = std::get<0>(frame);
            if (header.magic != KrpcProtocol::kDefaultMagic || header.version != KrpcProtocol::kDefaultVersion) {
                continue;
            }
            const auto type = static_cast<krpc::MsgpackMsgType>(header.msg_type);
            if (type == krpc::MsgpackMsgType::Pong) {
                ResolveHeartbeat(header);
                continue;
            }
            if (type == krpc::MsgpackMsgType::Ping) {
                krpc::MsgpackHeader resp;
                resp.msg_type = static_cast<uint8_t>(krpc::MsgpackMsgType::Pong);
                resp.request_id = header.request_id;
                krpc::msgpack::sbuffer out_payload;
                auto resp_frame = std::make_tuple(resp, krpc::msgpack::type::nil_t());
                krpc::msgpack::pack(out_payload, resp_frame);
                SendFrame(out_payload);
                continue;
            }
            if (type != krpc::MsgpackMsgType::Response) {
                continue;
            }
            using resp_payload_t = std::tuple<krpc::msgpack::object, krpc::msgpack::object>;
            resp_payload_t resp_payload;
            try {
                std::get<1>(frame).convert(resp_payload);
            } catch (...) {
                continue;
            }

            const krpc::msgpack::object &err_obj = std::get<0>(resp_payload);
            const krpc::msgpack::object &result_obj = std::get<1>(resp_payload);
            const uint64_t request_id = header.request_id;

            PendingCall pending;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_.find(request_id);
                if (it != pending_.end()) {
                    pending = std::move(it->second);
                    pending_.erase(it);
                    found = true;
                }
            }
            if (!found) {
                continue;
            }

            if (!err_obj.is_nil()) {
                try {
                    std::string err_text;
                    err_obj.convert(err_text);
                    CompletePending(std::move(pending), err_text, MakeNilHandle());
                } catch (...) {
                    CompletePending(std::move(pending), "response decode error", MakeNilHandle());
                }
            } else {
                CompletePending(std::move(pending), std::string(), krpc::msgpack::clone(result_obj));
            }
        }
    }
    running_.store(false, std::memory_order_release);
    if (recv_error) {
        std::lock_guard<std::mutex> lock(m_socket_mutex);
        CloseConnectionLocked();
    }
    FailAllPending("connection closed");
    {
        std::lock_guard<std::mutex> wait_lock(heartbeat_wait_mutex_);
        if (waiting_heartbeat_) {
            waiting_heartbeat_ = false;
            waiting_heartbeat_result_ = false;
        }
    }
    heartbeat_wait_cv_.notify_all();
}

bool KrpcMsgpackChannel::SendFrame(const krpc::msgpack::sbuffer &payload) {
    {
        std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
        if (socket_fd_ == -1) {
            return false;
        }
    }
    std::string header = BuildHeader(payload);
    std::string body(payload.data(), payload.size());
    EnqueueSend(std::move(header), std::move(body));
    return true;
}

std::future<krpc::msgpack::object_handle> KrpcMsgpackChannel::EnqueueRequest(uint64_t request_id,
                                                                             krpc::msgpack::sbuffer &&payload,
                                                                             int timeout_ms) {
    PendingCall pending;
    pending.start_time = std::chrono::steady_clock::now();
    pending.timeout_ms = timeout_ms;
    pending.has_promise = true;
    auto future = pending.promise.get_future();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.emplace(request_id, std::move(pending));
    }
    if (!SendFrame(payload)) {
        PendingCall failed;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_.find(request_id);
            if (it != pending_.end()) {
                failed = std::move(it->second);
                pending_.erase(it);
                found = true;
            }
        }
        if (found) {
            CompletePending(std::move(failed), "send failed", MakeNilHandle());
        }
        return future;
    }
    timeout_cv_.notify_all();
    return future;
}

void KrpcMsgpackChannel::EnqueueRequest(uint64_t request_id,
                                        krpc::msgpack::sbuffer &&payload,
                                        int timeout_ms,
                                        AsyncCallback callback) {
    PendingCall pending;
    pending.start_time = std::chrono::steady_clock::now();
    pending.timeout_ms = timeout_ms;
    pending.has_promise = false;
    pending.callback = std::move(callback);
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.emplace(request_id, std::move(pending));
    }
    if (!SendFrame(payload)) {
        PendingCall failed;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_.find(request_id);
            if (it != pending_.end()) {
                failed = std::move(it->second);
                pending_.erase(it);
                found = true;
            }
        }
        if (found) {
            CompletePending(std::move(failed), "send failed", MakeNilHandle());
        }
        return;
    }
    timeout_cv_.notify_all();
}

void KrpcMsgpackChannel::FailAllPending(const std::string &reason) {
    std::unordered_map<uint64_t, PendingCall> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending.swap(pending_);
    }
    for (auto &entry : pending) {
        CompletePending(std::move(entry.second), reason, MakeNilHandle());
    }
}

void KrpcMsgpackChannel::StartHeartbeatThread() {
    if (heartbeat_thread_started_) {
        return;
    }
    heartbeat_running_.store(true, std::memory_order_release);
    heartbeat_thread_ = std::thread(&KrpcMsgpackChannel::HeartbeatLoop, this);
    heartbeat_thread_started_ = true;
}

void KrpcMsgpackChannel::StopHeartbeatThread() {
    if (!heartbeat_thread_started_) {
        return;
    }
    heartbeat_running_.store(false, std::memory_order_release);
    heartbeat_cv_.notify_all();
    heartbeat_wait_cv_.notify_all();
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    heartbeat_thread_started_ = false;
}

void KrpcMsgpackChannel::HeartbeatLoop() {
    std::unique_lock<std::mutex> lock(heartbeat_mutex_);
    while (heartbeat_running_.load(std::memory_order_acquire)) {
        heartbeat_cv_.wait_for(lock, std::chrono::milliseconds(m_heartbeat_interval_ms));
        if (!heartbeat_running_.load(std::memory_order_acquire)) {
            break;
        }
        bool has_connection = false;
        {
            std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
            has_connection = (socket_fd_ != -1);
        }
        if (!has_connection) {
            m_missed_heartbeat_count = 0;
            continue;
        }
        lock.unlock();
        auto result = SendHeartbeatOnce();
        lock.lock();
        if (!heartbeat_running_.load(std::memory_order_acquire)) {
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

KrpcMsgpackChannel::HeartbeatResult KrpcMsgpackChannel::SendHeartbeatOnce() {
    {
        std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
        if (socket_fd_ == -1) {
            return HeartbeatResult::kSuccess;
        }
    }

    const uint64_t request_id = NextRequestId();
    krpc::MsgpackHeader header;
    header.msg_type = static_cast<uint8_t>(krpc::MsgpackMsgType::Ping);
    header.request_id = request_id;
    auto frame = std::make_tuple(header, krpc::msgpack::type::nil_t());
    krpc::msgpack::sbuffer sbuf;
    krpc::msgpack::pack(sbuf, frame);

    {
        std::lock_guard<std::mutex> wait_lock(heartbeat_wait_mutex_);
        waiting_heartbeat_ = true;
        waiting_heartbeat_id_ = request_id;
        waiting_heartbeat_result_ = false;
    }

    if (!SendFrame(sbuf)) {
        {
            std::lock_guard<std::mutex> wait_lock(heartbeat_wait_mutex_);
            waiting_heartbeat_ = false;
            waiting_heartbeat_result_ = false;
        }
        return HeartbeatResult::kFatal;
    }

    std::unique_lock<std::mutex> wait_lock(heartbeat_wait_mutex_);
    const bool got = heartbeat_wait_cv_.wait_for(
        wait_lock,
        std::chrono::milliseconds(m_request_timeout_ms),
        [this] { return !waiting_heartbeat_; });
    if (!got) {
        waiting_heartbeat_ = false;
        return HeartbeatResult::kTimeout;
    }
    return waiting_heartbeat_result_ ? HeartbeatResult::kSuccess : HeartbeatResult::kFatal;
}

void KrpcMsgpackChannel::HandleHeartbeatFailure(const std::string &reason) {
    StopRecvThread();
    {
        std::lock_guard<std::mutex> lock(m_socket_mutex);
        ReleaseConnection(false);
    }
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        std::queue<SendTask> empty;
        send_queue_.swap(empty);
    }
    FailAllPending(reason);
    {
        std::lock_guard<std::mutex> wait_lock(heartbeat_wait_mutex_);
        if (waiting_heartbeat_) {
            waiting_heartbeat_ = false;
            waiting_heartbeat_result_ = false;
        }
    }
    heartbeat_wait_cv_.notify_all();
    heartbeat_cv_.notify_all();
    timeout_cv_.notify_all();
    send_cv_.notify_all();
}

void KrpcMsgpackChannel::ResolveHeartbeat(const krpc::MsgpackHeader &header) {
    std::lock_guard<std::mutex> wait_lock(heartbeat_wait_mutex_);
    if (waiting_heartbeat_ && header.request_id == waiting_heartbeat_id_) {
        waiting_heartbeat_ = false;
        waiting_heartbeat_result_ = true;
        m_last_pong_time = std::chrono::steady_clock::now();
        heartbeat_wait_cv_.notify_all();
    }
}

void KrpcMsgpackChannel::StartTimeoutThread() {
    if (timeout_thread_started_) {
        return;
    }
    timeout_running_.store(true, std::memory_order_release);
    timeout_thread_ = std::thread(&KrpcMsgpackChannel::TimeoutLoop, this);
    timeout_thread_started_ = true;
}

void KrpcMsgpackChannel::StopTimeoutThread() {
    if (!timeout_thread_started_) {
        return;
    }
    timeout_running_.store(false, std::memory_order_release);
    timeout_cv_.notify_all();
    if (timeout_thread_.joinable()) {
        timeout_thread_.join();
    }
    timeout_thread_started_ = false;
}

void KrpcMsgpackChannel::TimeoutLoop() {
    const auto interval = std::chrono::milliseconds(50);
    while (timeout_running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(timeout_mutex_);
            timeout_cv_.wait_for(lock, interval);
            if (!timeout_running_.load(std::memory_order_acquire)) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<PendingCall> expired;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (it->second.timeout_ms > 0 &&
                    now - it->second.start_time >= std::chrono::milliseconds(it->second.timeout_ms)) {
                    expired.emplace_back(std::move(it->second));
                    it = pending_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto &pending : expired) {
            CompletePending(std::move(pending), "timeout", MakeNilHandle());
        }
    }
}

void KrpcMsgpackChannel::StartSendThread() {
    if (send_thread_started_) {
        return;
    }
    send_running_.store(true, std::memory_order_release);
    send_thread_ = std::thread(&KrpcMsgpackChannel::SendLoop, this);
    send_thread_started_ = true;
}

void KrpcMsgpackChannel::StopSendThread() {
    if (!send_thread_started_) {
        return;
    }
    send_running_.store(false, std::memory_order_release);
    send_cv_.notify_all();
    if (send_thread_.joinable()) {
        if (send_thread_.get_id() == std::this_thread::get_id()) {
            send_thread_.detach();
        } else {
            send_thread_.join();
        }
    }
    send_thread_started_ = false;
    std::queue<SendTask> empty;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_queue_.swap(empty);
    }
}

void KrpcMsgpackChannel::SendLoop() {
    while (send_running_.load(std::memory_order_acquire)) {
        SendTask task;
        {
            std::unique_lock<std::mutex> lock(send_mutex_);
            send_cv_.wait(lock, [&] {
                return !send_running_.load(std::memory_order_acquire) || !send_queue_.empty();
            });
            if (!send_running_.load(std::memory_order_acquire) && send_queue_.empty()) {
                return;
            }
            task = std::move(send_queue_.front());
            send_queue_.pop();
        }

        int fd = -1;
        {
            std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
            fd = socket_fd_;
        }
        if (fd == -1) {
            HandleHeartbeatFailure("connection closed");
            continue;
        }
        const char *h = task.header.data();
        const size_t h_len = task.header.size();
        const char *b = task.body.data();
        const size_t b_len = task.body.size();
        size_t idx = 0;
        size_t offset = 0;
        while (idx < 2) {
            struct iovec iov[2];
            int iovcnt = 0;
            if (idx == 0) {
                iov[iovcnt].iov_base = const_cast<char *>(h + offset);
                iov[iovcnt].iov_len = h_len - offset;
                ++iovcnt;
                if (b_len > 0) {
                    iov[iovcnt].iov_base = const_cast<char *>(b);
                    iov[iovcnt].iov_len = b_len;
                    ++iovcnt;
                }
            } else {
                if (offset >= b_len) {
                    break;
                }
                iov[iovcnt].iov_base = const_cast<char *>(b + offset);
                iov[iovcnt].iov_len = b_len - offset;
                ++iovcnt;
            }

            ssize_t n = writev(fd, iov, iovcnt);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                HandleHeartbeatFailure("send failed");
                break;
            }
            if (n == 0) {
                HandleHeartbeatFailure("connection closed");
                break;
            }

            if (idx == 0) {
                const size_t remaining_h = h_len - offset;
                if (static_cast<size_t>(n) < remaining_h) {
                    offset += static_cast<size_t>(n);
                    continue;
                }
                idx = 1;
                offset = static_cast<size_t>(n) - remaining_h;
                if (b_len == 0 || offset >= b_len) {
                    break;
                }
                continue;
            }

            offset += static_cast<size_t>(n);
            if (offset >= b_len) {
                break;
            }
        }
    }
}

void KrpcMsgpackChannel::EnqueueSend(std::string &&header, std::string &&body) {
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_queue_.push(SendTask{std::move(header), std::move(body)});
    }
    send_cv_.notify_one();
}

void KrpcMsgpackChannel::CompletePending(PendingCall &&pending,
                                         const std::string &error,
                                         krpc::msgpack::object_handle &&result) {
    if (pending.callback) {
        try {
            if (!error.empty()) {
                pending.callback(error, MakeNilHandle());
            } else {
                pending.callback(std::string(), std::move(result));
            }
        } catch (...) {
        }
        return;
    }
    if (!pending.has_promise) {
        return;
    }
    if (!error.empty()) {
        pending.promise.set_exception(std::make_exception_ptr(std::runtime_error(error)));
        return;
    }
    pending.promise.set_value(std::move(result));
}

krpc::msgpack::object_handle KrpcMsgpackChannel::MakeNilHandle() {
    auto z = krpc::msgpack::unique_ptr<krpc::msgpack::zone>(new krpc::msgpack::zone());
    krpc::msgpack::object obj(krpc::msgpack::type::nil_t(), *z);
    return krpc::msgpack::object_handle(obj, std::move(z));
}

int KrpcMsgpackChannel::AcquireConnection(const std::string &ip, uint16_t port, std::string *errMsg, bool *from_pool) {
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

void KrpcMsgpackChannel::ReleaseConnection(bool healthy) {
    if (socket_fd_ == -1) {
        return;
    }
    int fd = socket_fd_;
    socket_fd_ = -1;
    recv_buffer_.clear();
    if (!healthy) {
        ::close(fd);
        return;
    }
    if (!m_use_pool) {
        ::close(fd);
        return;
    }
    ConnectionPool::Instance().Release(m_endpoint_key, fd, static_cast<size_t>(m_pool_max_idle));
}

void KrpcMsgpackChannel::CloseConnectionLocked() {
    if (socket_fd_ != -1) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    recv_buffer_.clear();
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        std::queue<SendTask> empty;
        send_queue_.swap(empty);
    }
}

std::vector<Endpoint> KrpcMsgpackChannel::QueryServiceNodes(ZkClient *zkclient,
                                                            const std::string &service_name,
                                                            const std::string &method_name) {
    std::vector<Endpoint> endpoints;
    std::string method_path = "/" + service_name + "/" + method_name;
    LOG(INFO) << "zk method_path: " << method_path;

    std::vector<std::string> children;
    {
        std::unique_lock<std::mutex> lock(g_data_mutex);
        children = zkclient->GetChildren(method_path.c_str());
    }
    if (!children.empty()) {
        LOG(INFO) << "zk children under " << method_path << ": " << children.size();
    } else {
        LOG(WARNING) << "zk children empty under " << method_path;
    }

    for (const auto &child : children) {
        LOG(INFO) << "zk child node: " << child;
        Endpoint ep;
        if (ParseEndpoint(child, ep)) {
            endpoints.push_back(ep);
        }
    }

    if (endpoints.empty()) {
        std::unique_lock<std::mutex> lock(g_data_mutex);
        std::string host_data = zkclient->GetData(method_path.c_str());
        lock.unlock();
        Endpoint ep;
        if (ParseEndpoint(host_data, ep)) {
            LOG(INFO) << "zk fallback data: " << host_data;
            endpoints.push_back(ep);
        }
    }

    return endpoints;
}

bool KrpcMsgpackChannel::EnsureConnection(const std::string &service,
                                          const std::string &method,
                                          std::string *error_text) {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    const int discovery_retry = ParseConfigInt(config.Load("discovery_retry"), 3);
    const int discovery_retry_interval_ms = ParseConfigInt(config.Load("discovery_retry_interval_ms"), 200);

    const std::string cache_key = service + "/" + method;

    std::vector<Endpoint> endpoints = GetStaticEndpoints();
    std::unique_ptr<ZkClient> zkCli;
    if (endpoints.empty()) {
        if (!TryGetCachedEndpoints(cache_key, endpoints)) {
            zkCli.reset(new ZkClient());
            zkCli->Start();
            endpoints = QueryServiceNodes(zkCli.get(), service, method);
            StoreCachedEndpoints(cache_key, endpoints);
        }
    }

    for (int attempt = 1; endpoints.empty() && attempt <= discovery_retry; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(discovery_retry_interval_ms));
        if (!zkCli) {
            zkCli.reset(new ZkClient());
            zkCli->Start();
        }
        endpoints = QueryServiceNodes(zkCli.get(), service, method);
        StoreCachedEndpoints(cache_key, endpoints);
    }

    if (endpoints.empty()) {
        if (error_text) {
            *error_text = "service node not found: " + service + "/" + method;
        }
        return false;
    }

    if (m_lb) {
        m_lb->UpdateNodes(endpoints);
    }
    Endpoint selected;
    if (!m_lb || !m_lb->Select(method, selected)) {
        selected = endpoints.front();
    }

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

        {
            std::lock_guard<std::mutex> lock(m_socket_mutex);
            const bool already_ok = (socket_fd_ != -1 && candidate.host == m_ip && candidate.port == m_port && IsConnectionHealthy(socket_fd_));
            if (already_ok) {
                return true;
            }
        }

        bool need_release = false;
        bool healthy = false;
        {
            std::lock_guard<std::mutex> lock(m_socket_mutex);
            need_release = (socket_fd_ != -1);
            healthy = (socket_fd_ != -1 && IsConnectionHealthy(socket_fd_));
        }
        if (need_release) {
            StopRecvThread();
            ReleaseConnection(healthy);
        }
        {
            std::lock_guard<std::mutex> lock(m_socket_mutex);
            m_ip = candidate.host;
            m_port = candidate.port;
            m_endpoint_key = m_ip + ":" + std::to_string(m_port);
        }

        std::string connect_error;
        bool from_pool = false;
        int fd = AcquireConnection(m_ip, m_port, &connect_error, &from_pool);
        if (fd != -1) {
            {
                std::lock_guard<std::mutex> lock(m_socket_mutex);
                socket_fd_ = fd;
            }
            ClearEndpointFailure(candidate);
            StartRecvThread();
            return true;
        }

        last_error = connect_error.empty() ? "connect server error" : connect_error;
        MarkEndpointFailure(candidate);
    }

    const auto &fallback = endpoints[earliest_index];
    {
        std::lock_guard<std::mutex> lock(m_socket_mutex);
        const bool already_ok = (socket_fd_ != -1 && fallback.host == m_ip && fallback.port == m_port && IsConnectionHealthy(socket_fd_));
        if (already_ok) {
            return true;
        }
    }
    bool need_release = false;
    bool healthy = false;
    {
        std::lock_guard<std::mutex> lock(m_socket_mutex);
        need_release = (socket_fd_ != -1);
        healthy = (socket_fd_ != -1 && IsConnectionHealthy(socket_fd_));
    }
    if (need_release) {
        StopRecvThread();
        ReleaseConnection(healthy);
    }
    {
        std::lock_guard<std::mutex> lock(m_socket_mutex);
        m_ip = fallback.host;
        m_port = fallback.port;
        m_endpoint_key = m_ip + ":" + std::to_string(m_port);
    }

    std::string connect_error;
    bool from_pool = false;
    int fd = AcquireConnection(m_ip, m_port, &connect_error, &from_pool);
    if (fd != -1) {
        {
            std::lock_guard<std::mutex> lock(m_socket_mutex);
            socket_fd_ = fd;
        }
        ClearEndpointFailure(fallback);
        StartRecvThread();
        return true;
    }

    last_error = connect_error.empty() ? "connect server error" : connect_error;
    MarkEndpointFailure(fallback);
    if (error_text) {
        *error_text = last_error;
    }
    return false;
}

bool KrpcMsgpackChannel::EndpointAvailable(const Endpoint &ep,
                                           std::chrono::steady_clock::time_point now,
                                           std::chrono::steady_clock::time_point &next_retry) {
    std::lock_guard<std::mutex> lock(m_fail_mutex);
    const std::string key = ep.host + ":" + std::to_string(ep.port);
    auto it = m_fail_states.find(key);
    if (it == m_fail_states.end()) {
        return true;
    }
    next_retry = it->second.retry_at;
    return now >= it->second.retry_at;
}

void KrpcMsgpackChannel::MarkEndpointFailure(const Endpoint &ep) {
    std::lock_guard<std::mutex> lock(m_fail_mutex);
    const std::string key = ep.host + ":" + std::to_string(ep.port);
    const auto cooldown = std::chrono::milliseconds(g_endpoint_fail_cooldown_ms.load(std::memory_order_acquire));
    m_fail_states[key].retry_at = Clock::now() + cooldown;
}

void KrpcMsgpackChannel::ClearEndpointFailure(const Endpoint &ep) {
    std::lock_guard<std::mutex> lock(m_fail_mutex);
    const std::string key = ep.host + ":" + std::to_string(ep.port);
    m_fail_states.erase(key);
}
