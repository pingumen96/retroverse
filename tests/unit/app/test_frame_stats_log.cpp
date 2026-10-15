#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <optional>
#include <string>

#include <rv/app/frame_stats_log.hpp>

using rv::app::FrameStatsLog;
using rv::gl::FrameStats;

namespace {

FrameStats viewStats(std::size_t drawCalls, std::optional<double> gpuMilliseconds = std::nullopt,
                     std::optional<double> postMilliseconds = std::nullopt) {
    FrameStats stats;
    stats.items = 1001;
    stats.culledItems = 3;
    stats.batches = 4;
    stats.drawCalls = drawCalls;
    stats.instances = 998;
    stats.triangles = 11976;
    stats.shaderSwitches = 1;
    stats.materialSwitches = 4;
    stats.sceneGpuMilliseconds = gpuMilliseconds;
    stats.postGpuMilliseconds = postMilliseconds;
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
    log.record(0.0, viewStats(4, 0.5, 0.25));
    for (int frame = 0; frame < 3; ++frame) {
        log.record(0.25, viewStats(4, 0.5, 0.25));
        CHECK_FALSE(log.takeIntervalSummary().has_value());
    }
    log.record(0.25, viewStats(4, 1.5, 0.75));
    const std::optional<std::string> summary = log.takeIntervalSummary();
    REQUIRE(summary.has_value());
    INFO(*summary);
    CHECK(summary->starts_with("Frame stats: 5 frame(s) in 1.00 s, frame time 250.00 ms (min 250.00, max 250.00)"));
    // Scene pass and post-processing, each averaged over its own results.
    CHECK(contains(*summary, "GPU shadow n/a, depth n/a, SSAO n/a, scene 0.70 ms, post 0.35 ms; last view"));
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
    CHECK(contains(*next, "2 frame(s) in 1.25 s, frame time 625.00 ms (min 500.00, max 750.00), GPU shadow n/a, "
                          "depth n/a, SSAO n/a, scene n/a, post n/a"));
    CHECK(contains(*next, "7 draw call(s)"));
    CHECK(contains(*next, ", 667 debug line(s)"));

    // The run summary covers every frame.
    const std::optional<std::string> run = log.runSummary("Run");
    REQUIRE(run.has_value());
    INFO(*run);
    CHECK(run->starts_with("Run: 7 frame(s) in 2.25 s, frame time 375.00 ms (min 250.00, max 750.00), GPU shadow "
                           "n/a, depth n/a, SSAO n/a, scene 0.70 ms, post 0.35 ms"));
}

TEST_CASE("app/frame_stats_log: frames without a view count their time only", "[app][frame_stats_log]") {
    FrameStatsLog log(1.0);
    log.record(0.25, viewStats(4, 2.0, 0.5));
    // A minimized window: three frames render nothing; the renderer's statistics still describe the view above.
    for (int frame = 0; frame < 3; ++frame) {
        log.recordWithoutView(0.25);
    }
    const std::optional<std::string> summary = log.takeIntervalSummary();
    REQUIRE(summary.has_value());
    INFO(*summary);
    // Every frame counts towards the frame times; the GPU time is the one view's, not repeated for every frame.
    CHECK(summary->starts_with(
        "Frame stats: 4 frame(s) (3 without a view) in 1.00 s, frame time 250.00 ms (min 250.00, max 250.00), GPU "
        "shadow n/a, depth n/a, SSAO n/a, scene 2.00 ms, post 0.50 ms; last view: 1001 item(s)"));

    // Intervals without a view: no GPU time at all.
    for (int frame = 0; frame < 4; ++frame) {
        log.recordWithoutView(0.25);
    }
    const std::optional<std::string> idle = log.takeIntervalSummary();
    REQUIRE(idle.has_value());
    INFO(*idle);
    CHECK(idle->starts_with("Frame stats: 4 frame(s) (4 without a view) in 1.00 s"));
    CHECK(contains(*idle, "GPU shadow n/a, depth n/a, SSAO n/a, scene n/a, post n/a"));

    // Intervals where every frame has a view do not mention it.
    log.record(1.0, viewStats(4, 1.0));
    const std::optional<std::string> drawn = log.takeIntervalSummary();
    REQUIRE(drawn.has_value());
    CHECK(drawn->starts_with("Frame stats: 1 frame(s) in 1.00 s"));

    const std::optional<std::string> run = log.runSummary("Run");
    REQUIRE(run.has_value());
    INFO(*run);
    CHECK(run->starts_with("Run: 9 frame(s) (7 without a view) in 3.00 s"));
    // The post time of the view without one does not count as 0.
    CHECK(contains(*run, "scene 1.50 ms, post 0.50 ms"));
}

TEST_CASE("app/frame_stats_log: frames without a usable duration count but are not timed", "[app][frame_stats_log]") {
    FrameStatsLog log(0.0); // an invalid interval falls back to one second
    log.record(std::numeric_limits<double>::quiet_NaN(), viewStats(1));
    log.record(-1.0, viewStats(1));
    log.record(std::numeric_limits<double>::infinity(), viewStats(1));
    CHECK_FALSE(log.takeIntervalSummary().has_value());
    const std::optional<std::string> run = log.runSummary("Run");
    REQUIRE(run.has_value());
    CHECK(run->starts_with("Run: 3 frame(s) in 0.00 s, frame time n/a, GPU shadow n/a, depth n/a, SSAO n/a, scene "
                           "n/a, post n/a"));
    log.record(1.0, viewStats(1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()));
    const std::optional<std::string> summary = log.takeIntervalSummary();
    REQUIRE(summary.has_value());
    CHECK(contains(*summary, "4 frame(s) in 1.00 s, frame time 1000.00 ms (min 1000.00, max 1000.00), GPU shadow "
                             "n/a, depth n/a, SSAO n/a, scene n/a, post n/a"));
}

TEST_CASE("app/frame_stats_log: the shadow map, the depth pre-pass and the ambient occlusion are timed and counted",
          "[app][frame_stats_log]") {
    FrameStatsLog log(1.0);
    FrameStats lit = viewStats(47, 0.3, 0.2);
    lit.shadowGpuMilliseconds = 0.05;
    lit.depthGpuMilliseconds = 0.02;
    lit.ssaoGpuMilliseconds = 0.25;
    lit.shadowDrawCalls = 44;
    lit.shadowInstances = 68;
    lit.depthDrawCalls = 47;
    log.record(0.5, lit);
    // A view without a key light reports no shadow time: the average covers the views that had a shadow map.
    FrameStats unshadowed = lit;
    unshadowed.shadowGpuMilliseconds.reset();
    unshadowed.depthGpuMilliseconds = 0.04;
    unshadowed.ssaoGpuMilliseconds = 0.35;
    unshadowed.shadowDrawCalls = 0;
    unshadowed.shadowInstances = 0;
    log.record(0.5, unshadowed);
    const std::optional<std::string> summary = log.takeIntervalSummary();
    REQUIRE(summary.has_value());
    INFO(*summary);
    CHECK(contains(*summary,
                   "GPU shadow 0.05 ms, depth 0.03 ms, SSAO 0.30 ms, scene 0.30 ms, post 0.20 ms; last view"));
    // The counts of the last view's passes close the line.
    CHECK(summary->ends_with("; shadow map 0 draw call(s), 0 instance(s); depth pre-pass 47 draw call(s)"));

    log.record(1.0, lit);
    const std::optional<std::string> next = log.takeIntervalSummary();
    REQUIRE(next.has_value());
    INFO(*next);
    CHECK(next->ends_with("; shadow map 44 draw call(s), 68 instance(s); depth pre-pass 47 draw call(s)"));
}
