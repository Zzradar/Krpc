#include <catch2/catch_test_macros.hpp>

#include "Krpcheader.pb.h"
#include "Krpcprotocol.h"

TEST_CASE("RpcHeader roundtrip including trace_id") {
    Krpc::RpcHeader h;
    h.set_magic(KrpcProtocol::kDefaultMagic);
    h.set_version(KrpcProtocol::kDefaultVersion);
    h.set_msg_type(Krpc::MSG_TYPE_REQUEST);
    h.set_request_id(42);
    h.set_body_size(7);
    h.set_compress_type(Krpc::COMPRESS_NONE);
    h.set_service_name("UserServiceRpc");
    h.set_method_name("Login");
    h.set_trace_id("bench-trace-1");

    std::string wire;
    REQUIRE(h.SerializeToString(&wire));
    Krpc::RpcHeader h2;
    REQUIRE(h2.ParseFromString(wire));
    REQUIRE(h2.trace_id() == "bench-trace-1");
    REQUIRE(h2.magic() == KrpcProtocol::kDefaultMagic);
    REQUIRE(h2.service_name() == "UserServiceRpc");
}
