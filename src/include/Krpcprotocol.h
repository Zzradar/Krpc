#ifndef KRPC_PROTOCOL_H
#define KRPC_PROTOCOL_H

#include <cstdint>

namespace KrpcProtocol {

// Default magic/version used for framing validation.
constexpr uint32_t kDefaultMagic = 0x6b727063; // 'krpc'
constexpr uint32_t kDefaultVersion = 1;

// Defaults for future heartbeat + timeout features.
constexpr uint32_t kDefaultHeartbeatIntervalMs = 5000;
constexpr uint32_t kDefaultHeartbeatMissLimit = 3;
constexpr uint32_t kDefaultRequestTimeoutMs = 3000;

} // namespace KrpcProtocol

#endif
