#pragma once

#include "user_types.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

inline bool KrpcShouldSleep(const std::string &name) {
    return name == "sleep" || name == "timeout";
}

inline void KrpcMaybeSleep(const std::string &name) {
    if (KrpcShouldSleep(name)) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

inline MsgpackUserResult KrpcHandleLogin(const std::string &name, const std::string &pwd) {
    (void)pwd;
    KrpcMaybeSleep(name);
    MsgpackUserResult result;
    result.success = true;
    return result;
}

inline MsgpackUserResult KrpcHandleRegister(uint32_t id, const std::string &name, const std::string &pwd) {
    (void)id;
    (void)pwd;
    KrpcMaybeSleep(name);
    MsgpackUserResult result;
    result.success = true;
    return result;
}
