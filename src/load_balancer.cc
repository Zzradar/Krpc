#include "load_balancer.h"

std::atomic<size_t> RoundRobinLoadBalancer::s_global_index_{0};
