#pragma once

#include <string>

#include "Krpcmsgpack.h"

struct MsgpackUserResult {
    int errcode{0};
    std::string errmsg;
    bool success{false};

    MSGPACK_DEFINE(errcode, errmsg, success);
};
