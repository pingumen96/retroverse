#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>

#include <rv/core/failure_streak.hpp>

using rv::ErrorCode;
using rv::core::FailureStreak;

TEST_CASE("core/failure_streak: a persistent failure is reported once, then its recovery", "[core][failure_streak]") {
    FailureStreak streak;
    CHECK_FALSE(streak.succeed().has_value()); // nothing failed yet
    CHECK(streak.failures() == 0);

    // The first failure is logged; repeats with the same code are only counted.
    CHECK(streak.fail(ErrorCode::GraphicsError));
    for (int frame = 0; frame < 300; ++frame) {
        CHECK_FALSE(streak.fail(ErrorCode::GraphicsError));
    }
    CHECK(streak.failures() == 301);

    // A different cause is logged once, and so is a return to the first one.
    CHECK(streak.fail(ErrorCode::OutOfRange));
    CHECK_FALSE(streak.fail(ErrorCode::OutOfRange));
    CHECK(streak.fail(ErrorCode::GraphicsError));
    CHECK(streak.failures() == 304);

    // The first success ends the streak with its count; the next ones report nothing.
    CHECK(streak.succeed() == std::optional<std::size_t>(304));
    CHECK(streak.failures() == 0);
    CHECK_FALSE(streak.succeed().has_value());

    // A new streak logs its first failure again, even with the same code as before.
    CHECK(streak.fail(ErrorCode::GraphicsError));
    CHECK(streak.succeed() == std::optional<std::size_t>(1));
}
