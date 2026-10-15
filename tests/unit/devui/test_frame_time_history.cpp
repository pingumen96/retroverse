#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <rv/devui/frame_time_history.hpp>

using Catch::Approx;
using rv::devui::FrameTimeHistory;

TEST_CASE("devui/frame_time_history: frames are kept in milliseconds, oldest first", "[devui][frame_time_history]") {
    FrameTimeHistory history;
    CHECK(history.size() == 0);
    const FrameTimeHistory::Summary none = history.summary();
    CHECK(none.averageMilliseconds == 0.0f);
    CHECK(none.framesPerSecond == 0.0f);

    history.record(0.010);
    history.record(0.020);
    history.record(0.030);
    REQUIRE(history.size() == 3);
    CHECK(history.milliseconds(0) == Approx(10.0f));
    CHECK(history.milliseconds(2) == Approx(30.0f));
    CHECK(history.milliseconds(3) == 0.0f); // beyond the frames kept
    CHECK(history.ringOffset() == 0);

    const FrameTimeHistory::Summary summary = history.summary();
    CHECK(summary.averageMilliseconds == Approx(20.0f));
    CHECK(summary.minMilliseconds == Approx(10.0f));
    CHECK(summary.maxMilliseconds == Approx(30.0f));
    CHECK(summary.framesPerSecond == Approx(50.0f));
}

TEST_CASE("devui/frame_time_history: frames without a usable duration are ignored", "[devui][frame_time_history]") {
    FrameTimeHistory history;
    history.record(0.0); // the first frame of a run has no duration yet
    history.record(-0.016);
    history.record(std::numeric_limits<double>::quiet_NaN());
    history.record(std::numeric_limits<double>::infinity());
    history.record(1e300);  // overflows a float in milliseconds
    history.record(1e-300); // underflows to 0
    CHECK(history.size() == 0);
    history.record(0.016);
    CHECK(history.size() == 1);
}

TEST_CASE("devui/frame_time_history: a full history drops its oldest frame", "[devui][frame_time_history]") {
    FrameTimeHistory history;
    for (std::size_t frame = 1; frame <= FrameTimeHistory::kCapacity + 10; ++frame) {
        history.record(static_cast<double>(frame) / 1000.0); // frame n lasts n ms
    }
    REQUIRE(history.size() == FrameTimeHistory::kCapacity);
    // Frames 1..10 are gone: the oldest kept is frame 11, the newest frame 250.
    CHECK(history.milliseconds(0) == Approx(11.0f));
    CHECK(history.milliseconds(FrameTimeHistory::kCapacity - 1) == Approx(250.0f));
    // The ring as stored starts at the oldest frame.
    CHECK(history.ringOffset() == 10);
    CHECK(history.ringData()[history.ringOffset()] == Approx(11.0f));
    CHECK(history.ringData()[history.ringOffset() - 1] == Approx(250.0f));

    const FrameTimeHistory::Summary summary = history.summary();
    CHECK(summary.minMilliseconds == Approx(11.0f));
    CHECK(summary.maxMilliseconds == Approx(250.0f));
    CHECK(summary.averageMilliseconds == Approx(130.5f));
}
