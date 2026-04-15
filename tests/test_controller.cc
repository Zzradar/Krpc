#include <catch2/catch_test_macros.hpp>

#include "Krpccontroller.h"

TEST_CASE("Krpccontroller trace id cleared on Reset") {
    Krpccontroller c;
    c.SetTraceId("trace-abc");
    REQUIRE(c.TraceId() == "trace-abc");
    c.Reset();
    REQUIRE(c.TraceId().empty());
}
