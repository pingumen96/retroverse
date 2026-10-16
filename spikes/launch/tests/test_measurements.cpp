#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include <spike_launch/measurements.hpp>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using rv::spikes::launch::Clock;
using rv::spikes::launch::intervalStats;
using rv::spikes::launch::meetsCriteria;
using rv::spikes::launch::PacingCriterion;
using rv::spikes::launch::ReturnTimes;
using rv::spikes::launch::steadyFrom;
using rv::spikes::launch::summarize;

namespace {

/// Frames presented `intervals` apart (milliseconds), starting at t0.
std::vector<Clock::time_point> framesAt(const std::vector<double>& intervalsMs) {
    std::vector<Clock::time_point> frames{Clock::time_point{} + 1h};
    for (const double interval : intervalsMs) {
        frames.push_back(frames.back() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double, std::milli>(interval)));
    }
    return frames;
}

} // namespace

TEST_CASE("launch_spike/measurements: frame pacing is steady from the first run of short intervals",
          "[launch_spike][measurements]") {
    const PacingCriterion criterion{std::chrono::microseconds(20833), 5};
    // A slow first frame (the swap chain coming back), a dropped frame, then steady 6.1 ms frames (164 Hz).
    const auto frames = framesAt({45.0, 6.1, 6.1, 33.4, 6.1, 6.1, 6.1, 6.1, 6.1, 6.1});
    const std::optional<std::size_t> steady = steadyFrom(frames, criterion);
    REQUIRE(steady.has_value());
    CHECK(*steady == 4); // the frame after the dropped one starts 5 short intervals
    CHECK(steadyFrom(framesAt({16.7, 16.6, 16.7, 16.7, 16.6}), criterion) == std::optional<std::size_t>(0)); // 60 Hz
    CHECK_FALSE(steadyFrom(framesAt({16.7, 33.3, 16.7, 33.3, 16.7, 16.7}), criterion).has_value());
    CHECK_FALSE(steadyFrom({}, criterion).has_value());
    CHECK_FALSE(steadyFrom(framesAt({}), criterion).has_value()); // one frame has no interval
}

TEST_CASE("launch_spike/measurements: frame interval statistics", "[launch_spike][measurements]") {
    std::vector<double> scratch;
    const auto stats = intervalStats(framesAt({10.0, 20.0, 30.0, 40.0}), scratch);
    CHECK(stats.count == 4);
    CHECK_THAT(stats.meanMs, WithinAbs(25.0, 1e-3));
    CHECK_THAT(stats.p50Ms, WithinAbs(20.0, 1e-3)); // nearest rank
    CHECK_THAT(stats.p99Ms, WithinAbs(40.0, 1e-3));
    CHECK_THAT(stats.maxMs, WithinAbs(40.0, 1e-3));
    CHECK(intervalStats(framesAt({}), scratch).count == 0);
}

TEST_CASE("launch_spike/measurements: the return criteria", "[launch_spike][measurements]") {
    ReturnTimes times;
    times.restored = 12.0;
    times.foreground = -3.0; // already in the foreground when the process ended
    times.fullscreen = 15.0;
    times.firstFrame = 20.0;
    times.steady60 = 20.0;
    CHECK_FALSE(times.latest().has_value()); // the input is missing
    CHECK_FALSE(meetsCriteria(times));
    times.input = 1000.0;
    REQUIRE(times.latest().has_value());
    CHECK(*times.latest() == 1000.0);
    CHECK(meetsCriteria(times));
    times.input = 1000.5;
    CHECK_FALSE(meetsCriteria(times));
    CHECK(meetsCriteria(times, 2000.0));

    const nlohmann::json json = rv::spikes::launch::toJson(times);
    CHECK(json["foreground"].get<double>() == -3.0);
    CHECK(json["latest"].get<double>() == 1000.5);
}

TEST_CASE("launch_spike/measurements: median, best and worst", "[launch_spike][measurements]") {
    const auto odd = summarize({30.0, 10.0, 20.0});
    CHECK(odd.count == 3);
    CHECK(odd.median == 20.0);
    CHECK(odd.best == 10.0);
    CHECK(odd.worst == 30.0);
    CHECK(summarize({4.0, 1.0, 3.0, 2.0}).median == 2.5);
    CHECK(summarize({}).count == 0);
}
