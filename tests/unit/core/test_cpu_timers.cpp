#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <rv/core/cpu_timers.hpp>

using rv::core::CpuTimers;
using rv::core::DurationPercentiles;

TEST_CASE("core/cpu_timers: percentiles by nearest rank", "[core][cpu_timers]") {
    std::vector<double> none;
    CHECK_FALSE(rv::core::summarizeDurations(none).has_value());

    std::vector<double> one{4.0};
    CHECK(rv::core::summarizeDurations(one) == DurationPercentiles{1, 4.0, 4.0, 4.0, 4.0});

    // 1 to 100 shuffled: the value at rank ceil(p * 100).
    std::vector<double> hundred;
    for (int i = 100; i >= 1; --i) {
        hundred.push_back(static_cast<double>((i * 37) % 100 + 1));
    }
    CHECK(rv::core::summarizeDurations(hundred) == DurationPercentiles{100, 50.0, 95.0, 99.0, 100.0});
    CHECK(hundred.front() == 1.0); // sorted in place

    // Ten values: p95 and p99 both take the largest (rank 10), p50 the fifth.
    std::vector<double> ten{9, 1, 8, 2, 7, 3, 6, 4, 5, 10};
    CHECK(rv::core::summarizeDurations(ten) == DurationPercentiles{10, 5.0, 10.0, 10.0, 10.0});
}

TEST_CASE("core/cpu_timers: durations kept exactly, then in a bounded histogram within 1%", "[core][cpu_timers]") {
    using rv::core::DurationRecorder;
    DurationRecorder recorder;
    CHECK_FALSE(recorder.summary().has_value());
    recorder.add(-1.0);
    recorder.add(std::numeric_limits<double>::quiet_NaN());
    CHECK(recorder.count() == 0);

    // A long run's frame times (a sawtooth from 0.5 to 50 ms and a few long frames): exact while they fit, the same
    // summary as summarizeDurations() of every sample.
    std::vector<double> all;
    const auto feed = [&](std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t n = all.size();
            const double value = n % 1000 == 999 ? 250.0 + static_cast<double>(n % 7) : 0.5 + 0.05 * (n % 990);
            recorder.add(value);
            all.push_back(value);
        }
    };
    feed(DurationRecorder::kExactSamples);
    CHECK(recorder.exact());
    std::vector<double> sorted = all;
    CHECK(recorder.summary() == rv::core::summarizeDurations(sorted));

    // Past them: a histogram, every percentile at or above the exact one and within 1% of it; count and max exact.
    feed(3 * DurationRecorder::kExactSamples + 12345);
    CHECK_FALSE(recorder.exact());
    sorted = all;
    const DurationPercentiles exact = rv::core::summarizeDurations(sorted).value();
    const DurationPercentiles bounded = recorder.summary().value();
    CHECK(bounded.count == exact.count);
    CHECK(bounded.maxMs == exact.maxMs);
    const std::vector<std::pair<double, double>> pairs{
        {bounded.p50Ms, exact.p50Ms}, {bounded.p95Ms, exact.p95Ms}, {bounded.p99Ms, exact.p99Ms}};
    for (const auto& [approximate, real] : pairs) {
        INFO(approximate << " against " << real);
        CHECK(approximate >= real);
        CHECK(approximate <= real * 1.01);
    }

    // Tiny and huge durations land in the end buckets.
    DurationRecorder ends;
    for (std::size_t index = 0; index <= DurationRecorder::kExactSamples; ++index) {
        ends.add(index % 2 == 0 ? 0.0 : 1.0e9);
    }
    const DurationPercentiles extremes = ends.summary().value();
    CHECK(extremes.p50Ms <= DurationRecorder::kSmallestMs);
    CHECK(extremes.maxMs == 1.0e9);
    CHECK(extremes.p99Ms == 1.0e9);
}

TEST_CASE("core/cpu_timers: a section's frame is the sum of its runs, frames without a run record nothing",
          "[core][cpu_timers]") {
    CpuTimers timers;
    const CpuTimers::SectionId ui = timers.section("productUi");
    CHECK(timers.section("productUi") == ui); // registered once
    const CpuTimers::SectionId upload = timers.section("layerUploads");
    CHECK(upload != ui);

    // Before the first frame (an initialization): counted towards the first.
    timers.add(ui, 0.25);
    CHECK(timers.currentFrameMilliseconds(ui) == 0.25);
    CHECK_FALSE(timers.currentFrameMilliseconds(upload).has_value());
    timers.add(ui, 0.5);
    timers.add(upload, 1.0);
    timers.endFrame();
    CHECK_FALSE(timers.currentFrameMilliseconds(ui).has_value());
    // Frame 2: the UI only, a zero-length run still counts as a run.
    timers.add(ui, 0.0);
    timers.endFrame();
    // Frame 3: the upload only; ignored values change nothing.
    timers.add(upload, 3.0);
    timers.add(upload, -1.0);
    timers.add(upload, std::numeric_limits<double>::quiet_NaN());
    timers.add(upload, std::numeric_limits<double>::infinity());
    timers.add(CpuTimers::SectionId{99}, 5.0);
    timers.endFrame();
    // Frame 4: nothing.
    timers.endFrame();
    CHECK(timers.frames() == 4);
    // What runs after the last frame is not recorded.
    timers.add(ui, 100.0);

    const std::vector<rv::core::CpuSectionSummary> summaries = timers.summaries();
    REQUIRE(summaries.size() == 2);
    // By name.
    CHECK(summaries[0].name == "layerUploads");
    CHECK(summaries[0].times == DurationPercentiles{2, 1.0, 3.0, 3.0, 3.0});
    CHECK(summaries[1].name == "productUi");
    CHECK(summaries[1].times == DurationPercentiles{2, 0.0, 0.75, 0.75, 0.75});

    // A section registered but never run is not reported.
    static_cast<void>(timers.section("videoUpload"));
    CHECK(timers.summaries().size() == 2);

    timers.clear();
    CHECK(timers.summaries().empty());
    CHECK(timers.frames() == 0);
}

TEST_CASE("core/cpu_timers: scopes time their lifetime on the steady clock", "[core][cpu_timers]") {
    CpuTimers timers;
    {
        const CpuTimers::Scope scope = timers.scope("sleep");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const CpuTimers::SectionId sleep = timers.section("sleep");
    const std::optional<double> slept = timers.currentFrameMilliseconds(sleep);
    REQUIRE(slept.has_value());
    CHECK(*slept >= 1.5);
    CHECK(*slept < 2000.0);

    // stop() adds once; a moved-from scope adds nothing.
    CpuTimers::Scope first(timers, timers.section("moved"));
    CpuTimers::Scope second(std::move(first));
    second.stop();
    const std::optional<double> once = timers.currentFrameMilliseconds(timers.section("moved"));
    REQUIRE(once.has_value());
    second.stop();
    CHECK(timers.currentFrameMilliseconds(timers.section("moved")) == once);
    timers.endFrame();
    CHECK(timers.summaries().size() == 2);
}

TEST_CASE("core/cpu_timers: the main thread's sections", "[core][cpu_timers]") {
    CpuTimers& timers = rv::core::mainThreadCpuTimers();
    CHECK(&timers == &rv::core::mainThreadCpuTimers());
    const CpuTimers::SectionId section = timers.section("test.mainThreadSection");
    const std::optional<double> before = timers.currentFrameMilliseconds(section);
    {
        const rv::core::CpuSectionScope timing("test.mainThreadSection");
    }
    const std::optional<double> after = timers.currentFrameMilliseconds(section);
    REQUIRE(after.has_value());
    CHECK(*after >= before.value_or(0.0));
    // The names the budgets share.
    CHECK(rv::core::cpu_section::kCollectionPlanning == "collectionPlanning");
    CHECK(rv::core::cpu_section::kLayerUploads == "layerUploads");
    CHECK(rv::core::cpu_section::kVideoUpload == "videoUpload");
    CHECK(rv::core::cpu_section::kProductUi == "productUi");
    CHECK(rv::core::cpu_section::kSceneSubmit == "sceneSubmit");
}
