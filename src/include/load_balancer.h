#ifndef KRPC_LOAD_BALANCER_H
#define KRPC_LOAD_BALANCER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct Endpoint {
    std::string host;
    uint16_t port{0};
    int weight{1};
};

class ILoadBalancer {
public:
    virtual ~ILoadBalancer() = default;
    virtual void UpdateNodes(const std::vector<Endpoint> &nodes) = 0;
    virtual bool Select(const std::string &key, Endpoint &out) = 0;
};

class RoundRobinLoadBalancer : public ILoadBalancer {
public:
    void UpdateNodes(const std::vector<Endpoint> &nodes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_ = nodes;
    }

    bool Select(const std::string &, Endpoint &out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nodes_.empty()) {
            return false;
        }
        const size_t i = s_global_index_.fetch_add(1, std::memory_order_relaxed) % nodes_.size();
        out = nodes_[i];
        return true;
    }

private:
    std::vector<Endpoint> nodes_;
    std::mutex mutex_;
    static std::atomic<size_t> s_global_index_;
};

#endif // KRPC_LOAD_BALANCER_H
