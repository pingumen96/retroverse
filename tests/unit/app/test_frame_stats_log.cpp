#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <optional>
#include <string>

#include <rv/app/frame_stats_log.hpp>

using rv::app::FrameStatsLog;
using rv::gl::FrameStats;

namespace {

FrameStats viewStats(std::size_t drawCalls, std::optional<double> gpuMilliseconds = std::nullopt) {
    FrameStats stats;
    stats.items = 1001;
    stats.culledItems = 3;
    stats.batches = 4;
    stats.drawCalls = drawCalls;
    stats.instances = 998;
    stats.triangles = 11976;
    stats.shaderSwitches = 1;
    stats.materialSwitches = 4;
    stats.gpuMilliseconds = gpuMilliseconds;
    return stats;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("app/frame_stats_log: a summary per interval of recorded frame time", "[app][frame_stats_log]") {
    FrameStatsLog log(1.0);
    CHECK_FALSE(log.takeIntervalSummary().has_value());
    CHECK_FALSE(log.runSummary("Run").has_value());

    // The first frame has no measured duration yet; 0.25 s frames reach the interval after four of them.
    log.record(0.0, viewStats(4, 0.5));
    for (int frame = 0; frame < 3; ++frame) {
        log.record(0.25, viewStats(4, 0.5));
        CHECK_FALSE(log.takeIntervalSummary().has_value());
    }
    log.record(0.25, viewStats(4, 1.5));
    const std::optional<std::string> summary = log.takeIntervalSummary();
    REQUIRE(summary.has_value());
    INFO(*summary);
    CHECK(summary->starts_with("Frame stats: 5 frame(s) in 1.00 s, frame time 250.00 ms (min 250.00, max 250.00)"));
    CHECK(contains(*summary, "GPU 0.70 ms"));
    CHECK(contains(*summary, "last view: 1001 item(s), 3 culled, 4 batch(es), 4 draw call(s), 998 instance(s), "
                             "11976 triangle(s), 1 shader switch(es), 4 material switch(es)"));
    CHECK_FALSE(contains(*summary, "debug line"));
    // The next interval starts empty.
    CHECK_FALSE(log.takeIntervalSummary().has_value());

    log.record(0.5, viewStats(6));
    FrameStats withLines = viewStats(7);
    withLines.debugLines = 667;
    log.record(0.75, withLines);
    const std::optional<std::string> next = log.takeIntervalSummary();
    REQUIRE(next.has_value());
    INFO(*next);
    CHECK(contains(*next, "2 frame(s) in 1.25 s, frame time 625.00 ms (min 500.00, max 750.00), GPU n/a"));
    CHECK(contains(*next, "7 draw call(s)"));
    CHECK(contains(*next, ", 667 debug line(s)"));

    // The run summary covers every frame.
    const std::optional<std::string> run = log.runSummary("Run");
    REQUIRE(run.has_value());
    INFO(*run);
    CHECK(run->starts_with("Run: 7 frame(s) in 2.25 s, frame time 375.00 ms (min 250.00, max 750.00), GPU 0.70 ms"));
}

TEST_CASE("app/frame_stats_log: frames without a usable duration count but are not timed", "[app][frame_stats_log]") {
    FrameStatsLog log(0.0); // an invalid interval falls back to one second
    log.record(std::numeric_limits<double>::quiet_NaN(), viewStats(1));
    log.record(-1.0, viewStats(1));
    log.record(std::numeric_limits<double>::infinity(), viewStats(1));
    CHECK_FALSE(log.takeIntervalSummary().has_value());
    const std::optional<std::string> run = log.runSummary("Run");
    REQUIRE(run.has_value());
    CHECK(run->starts_with("Run: 3 frame(s) in 0.00 s, frame time n/a, GPU n/a"));
    log.record(1.0, viewStats(1, std::numeric_limits<double>::quiet_NaN()));
    const std::optional<std::string> summary = log.takeIntervalSummary();
    REQUIRE(summary.has_value());
    CHECK(contains(*summary, "4 frame(s) in 1.00 s, frame time 1000.00 ms (min 1000.00, max 1000.00), GPU n/a"));
}
