#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rv/core/frame_clock.hpp>

using Catch::Approx;
using rv::core::FrameClock;
using namespace std::chrono_literals;

TEST_CASE("core/frame_clock: deltas, totals and frame index", "[core][frame_clock]") {
    FrameClock clock;
    const auto t0 = FrameClock::Clock::time_point{} + 100s;

    clock.tickAt(t0);
    CHECK(clock.frameIndex() == 1);
    CHECK(clock.deltaSeconds() == Approx(0.0));
    CHECK(clock.framesPerSecond() == Approx(0.0));

    clock.tickAt(t0 + 16ms);
    CHECK(clock.frameIndex() == 2);
    CHECK(clock.deltaSeconds() == Approx(0.016));
    CHECK(clock.smoothedDeltaSeconds() == Approx(0.016));
    CHECK(clock.totalSeconds() == Approx(0.016));

    clock.tickAt(t0 + 32ms + 500ms); // long hitch
    CHECK(clock.deltaSeconds() == Approx(0.516));
    CHECK(clock.clampedDeltaSeconds() == Approx(0.1));
    CHECK(clock.clampedDeltaSeconds(1.0) == Approx(0.516));
    CHECK(clock.smoothedDeltaSeconds() == Approx(0.016 + 0.1 * (0.516 - 0.016)));
    CHECK(clock.totalSeconds() == Approx(0.532));
}

TEST_CASE("core/frame_clock: time going backwards yields a zero delta", "[core][frame_clock]") {
    FrameClock clock;
    const auto t0 = FrameClock::Clock::time_point{} + 10s;
    clock.tickAt(t0);
    clock.tickAt(t0 + 10ms);
    clock.tickAt(t0 + 5ms);
    CHECK(clock.deltaSeconds() == Approx(0.0));
    CHECK(clock.totalSeconds() == Approx(0.010));
    CHECK(clock.frameIndex() == 3);
}

TEST_CASE("core/frame_clock: real clock advances", "[core][frame_clock]") {
    FrameClock clock;
    clock.tick();
    clock.tick();
    CHECK(clock.frameIndex() == 2);
    CHECK(clock.deltaSeconds() >= 0.0);
}
