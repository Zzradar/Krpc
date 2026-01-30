#pragma once

#include <cstdint>
#include <string>

#include "Krpcmsgpack.h"
#include "Krpcprotocol.h"

namespace krpc {

enum class MsgpackMsgType : uint8_t {
    Unknown = 0,
    Request = 1,
    Response = 2,
    Ping = 3,
    Pong = 4,
    Oneway = 5
};

struct MsgpackHeader {
    uint32_t magic{KrpcProtocol::kDefaultMagic};
    uint32_t version{KrpcProtocol::kDefaultVersion};
    uint8_t msg_type{static_cast<uint8_t>(MsgpackMsgType::Unknown)};
    uint64_t request_id{0};
    uint32_t timeout_ms{0};
    std::string service_name;
    std::string method_name;

    MSGPACK_DEFINE(magic, version, msg_type, request_id, timeout_ms, service_name, method_name);
};

} // namespace krpc

