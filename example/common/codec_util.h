#pragma once

#include <cctype>
#include <string>

#include "Krpcapplication.h"

inline std::string KrpcLoadCodec() {
    auto &config = KrpcApplication::GetInstance().GetConfig();
    std::string codec = config.Load("rpc_codec");
    if (codec.empty()) {
        return "protobuf";
    }
    for (auto &ch : codec) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return codec;
}

inline bool KrpcUseMsgpack() {
    return KrpcLoadCodec() == "msgpack";
}
