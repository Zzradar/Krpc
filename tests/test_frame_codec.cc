#include <catch2/catch_test_macros.hpp>

#include "Krpcheader.pb.h"
#include "Krpcprotocol.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <string>

namespace {

std::string EncodeRpcFrame(const Krpc::RpcHeader &header, const std::string &body) {
    std::string header_bytes;
    header.SerializeToString(&header_bytes);
    std::string out;
    google::protobuf::io::StringOutputStream sos(&out);
    google::protobuf::io::CodedOutputStream cos(&sos);
    cos.WriteVarint32(static_cast<uint32_t>(header_bytes.size()));
    cos.WriteRaw(header_bytes.data(), static_cast<int>(header_bytes.size()));
    cos.WriteRaw(body.data(), static_cast<int>(body.size()));
    return out;
}

bool DecodeOneFrame(const char *data, int len, int *consumed, Krpc::RpcHeader *out_header, std::string *out_body) {
    google::protobuf::io::ArrayInputStream raw_input(data, len);
    google::protobuf::io::CodedInputStream coded_input(&raw_input);

    uint32_t header_size = 0;
    if (!coded_input.ReadVarint32(&header_size)) {
        return false;
    }
    std::string rpc_header_str;
    auto msg_limit = coded_input.PushLimit(static_cast<int>(header_size));
    if (!coded_input.ReadString(&rpc_header_str, static_cast<int>(header_size))) {
        return false;
    }
    coded_input.PopLimit(msg_limit);

    if (!out_header->ParseFromString(rpc_header_str)) {
        return false;
    }
    const uint32_t body_size = out_header->body_size();
    if (!coded_input.ReadString(out_body, static_cast<int>(body_size))) {
        return false;
    }
    *consumed = coded_input.CurrentPosition();
    return true;
}

} // namespace

TEST_CASE("length-prefixed frames parse back-to-back (sticky buffer)") {
    Krpc::RpcHeader h1;
    h1.set_magic(KrpcProtocol::kDefaultMagic);
    h1.set_version(KrpcProtocol::kDefaultVersion);
    h1.set_msg_type(Krpc::MSG_TYPE_REQUEST);
    h1.set_request_id(1);
    h1.set_body_size(4);
    h1.set_service_name("S");
    h1.set_method_name("M");
    h1.set_trace_id("t1");

    Krpc::RpcHeader h2 = h1;
    h2.set_request_id(2);
    h2.set_body_size(3);
    h2.set_trace_id("t2");

    const std::string f1 = EncodeRpcFrame(h1, "abcd");
    const std::string f2 = EncodeRpcFrame(h2, "xyz");
    const std::string buf = f1 + f2;

    Krpc::RpcHeader rh1;
    std::string body1;
    int c1 = 0;
    REQUIRE(DecodeOneFrame(buf.data(), static_cast<int>(buf.size()), &c1, &rh1, &body1));
    REQUIRE(body1 == "abcd");
    REQUIRE(rh1.trace_id() == "t1");

    Krpc::RpcHeader rh2;
    std::string body2;
    int c2 = 0;
    const char *rest = buf.data() + c1;
    const int rest_len = static_cast<int>(buf.size()) - c1;
    REQUIRE(DecodeOneFrame(rest, rest_len, &c2, &rh2, &body2));
    REQUIRE(body2 == "xyz");
    REQUIRE(rh2.trace_id() == "t2");
    REQUIRE(c1 + c2 == static_cast<int>(buf.size()));
}
