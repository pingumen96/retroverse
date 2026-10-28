// rv_platform_win's process priority (I5 step 2, ADR-0016 §2): the classes set and read back, the test process left at
// the class it had.

#include <rv/platform_win/process_priority.hpp>

#include <catch2/catch_test_macros.hpp>

using rv::platform_win::ProcessPriority;

TEST_CASE("platform_win/process_priority: above normal and normal are set and read back", "[platform_win]") {
    const ProcessPriority before = rv::platform_win::currentProcessPriority();
    REQUIRE(rv::platform_win::setProcessPriority(ProcessPriority::AboveNormal).has_value());
    CHECK(rv::platform_win::currentProcessPriority() == ProcessPriority::AboveNormal);
    REQUIRE(rv::platform_win::setProcessPriority(ProcessPriority::Normal).has_value());
    CHECK(rv::platform_win::currentProcessPriority() == ProcessPriority::Normal);
    const auto other = rv::platform_win::setProcessPriority(ProcessPriority::Other);
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(rv::platform_win::toString(ProcessPriority::AboveNormal) == "above-normal");
    CHECK(rv::platform_win::toString(ProcessPriority::Normal) == "normal");
    if (before != ProcessPriority::Other) {
        CHECK(rv::platform_win::setProcessPriority(before).has_value());
    }
}
