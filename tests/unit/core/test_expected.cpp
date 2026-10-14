#include <catch2/catch_test_macros.hpp>

#include <rv/core/expected.hpp>

namespace {

rv::Expected<int> parsePositive(int value) {
    if (value <= 0) {
        return rv::makeError(rv::ErrorCode::InvalidArgument, "value must be positive", "parsePositive");
    }
    return value;
}

} // namespace

TEST_CASE("core/expected: value and error branches", "[core][expected]") {
    const auto good = parsePositive(3);
    REQUIRE(good.has_value());
    CHECK(*good == 3);

    const auto bad = parsePositive(-1);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(bad.error().toString() == "invalid-argument: value must be positive [parsePositive]");
}

TEST_CASE("core/expected: Status and propagate", "[core][expected]") {
    const rv::Status success = rv::ok();
    CHECK(success.has_value());

    const auto inner = parsePositive(0);
    const rv::Status outer = [&]() -> rv::Status {
        if (!inner) {
            return rv::propagate(inner.error());
        }
        return rv::ok();
    }();
    REQUIRE_FALSE(outer.has_value());
    CHECK(outer.error().message == "value must be positive");
}

TEST_CASE("core/expected: every error code has a name", "[core][expected]") {
    for (int code = static_cast<int>(rv::ErrorCode::Unknown); code <= static_cast<int>(rv::ErrorCode::PlatformError);
         ++code) {
        CHECK_FALSE(rv::toString(static_cast<rv::ErrorCode>(code)).empty());
    }
    CHECK(rv::Error{rv::ErrorCode::NotFound, "missing", {}}.toString() == "not-found: missing");
}
