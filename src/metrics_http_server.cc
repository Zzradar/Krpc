#include "metrics_export.h"
#include "KrpcLogger.h"
#include <atomic>
#include <thread>
#include <string>
#include <sstream>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <poll.h>

namespace {

int CreateListenFd(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string BuildHttpResponse(const std::string &body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n";
    ss << "Content-Type: text/plain; charset=utf-8\r\n";
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << "Connection: close\r\n\r\n";
    ss << body;
    return ss.str();
}

class MetricsHttpServerImpl {
public:
    bool Start(int port) {
        if (running_.load()) {
            return true;
        }
        int fd = CreateListenFd(port);
        if (fd < 0) {
            LOG(WARNING) << "metrics http listen failed on port " << port;
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this, fd]() { Serve(fd); });
        return true;
    }

    void Stop() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void Serve(int listen_fd) {
        while (running_.load()) {
            pollfd pfd{};
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            int rc = ::poll(&pfd, 1, 500);
            if (rc <= 0) {
                continue;
            }
            sockaddr_in cli{};
            socklen_t len = sizeof(cli);
            int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&cli), &len);
            if (cfd < 0) {
                continue;
            }
            HandleClient(cfd);
            ::close(cfd);
        }
        ::close(listen_fd);
    }

    void HandleClient(int fd) {
        // 读取一次请求头即可
        char buf[512];
        (void)::recv(fd, buf, sizeof(buf), 0);
        std::string body = RenderMetricsPrometheus();
        if (body.empty()) {
            body = "# no metrics yet\n";
        }
        const std::string resp = BuildHttpResponse(body);
        (void)::send(fd, resp.data(), resp.size(), 0);
    }

    std::atomic<bool> running_{false};
    std::thread thread_;
};

MetricsHttpServerImpl g_http_server;

} // namespace

bool StartMetricsHttpServer(int port) {
    return g_http_server.Start(port);
}

void StopMetricsHttpServer() {
    g_http_server.Stop();
}
