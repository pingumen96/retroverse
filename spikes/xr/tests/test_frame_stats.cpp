#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <vector>

#include <spike_xr/frame_stats.hpp>
#include <spike_xr/swapchain_format.hpp>

using Catch::Matchers::WithinAbs;
using rv::spikes::xr::chooseColorFormat;
using rv::spikes::xr::FrameRecord;
using rv::spikes::xr::glFormatName;
using rv::spikes::xr::kGlRgb10A2;
using rv::spikes::xr::kGlRgba16f;
using rv::spikes::xr::kGlRgba8;
using rv::spikes::xr::kGlSrgb8Alpha8;
using rv::spikes::xr::meetsCriteria;
using rv::spikes::xr::summarizeFrames;

namespace {

constexpr std::int64_t kPeriod72 = 13'888'889; // 72 Hz, nanoseconds

/// Frames whose predicted display times advance by `steps` display periods each (1: no refresh missed).
std::vector<FrameRecord> framesAdvancing(const std::vector<int>& steps) {
    std::vector<FrameRecord> frames;
    std::int64_t time = 5'000'000'000;
    frames.push_back({time, kPeriod72, true, 3.0, 4.0});
    for (const int step : steps) {
        time += kPeriod72 * step;
        frames.push_back({time, kPeriod72, true, 3.0, 4.0});
    }
    return frames;
}

} // namespace

TEST_CASE("xr_spike/frames: a steady run misses no refresh", "[xr_spike][frames]") {
    const auto frames = framesAdvancing(std::vector<int>(99, 1));
    const auto summary = summarizeFrames(frames);
    CHECK(summary.frames == 100);
    CHECK(summary.rendered == 100);
    CHECK(summary.missedRefreshes == 0);
    CHECK_THAT(summary.missedFraction, WithinAbs(0.0, 1e-12));
    CHECK_THAT(summary.refreshHz, WithinAbs(72.0, 1e-3));
    CHECK(meetsCriteria(summary));
}

TEST_CASE("xr_spike/frames: a gap of several periods counts the refreshes it skipped", "[xr_spike][frames]") {
    // 98 steady intervals, one skipping a refresh, one skipping two: 3 of 103 refreshes missed.
    std::vector<int> steps(98, 1);
    steps.push_back(2);
    steps.push_back(3);
    const auto summary = summarizeFrames(framesAdvancing(steps));
    CHECK(summary.missedRefreshes == 3);
    CHECK_THAT(summary.missedFraction, WithinAbs(3.0 / 103.0, 1e-9));
    CHECK_FALSE(meetsCriteria(summary));
    CHECK(meetsCriteria(summary, 0.05));
}

TEST_CASE("xr_spike/frames: durations use nearest-rank percentiles, the GPU only where it arrived",
          "[xr_spike][frames]") {
    std::vector<FrameRecord> frames;
    for (int i = 1; i <= 100; ++i) {
        FrameRecord frame{static_cast<std::int64_t>(i) * kPeriod72, kPeriod72, i % 2 == 0, static_cast<double>(i)};
        if (i > 10) {
            frame.gpuMs = 2.0;
        }
        frames.push_back(frame);
    }
    const auto summary = summarizeFrames(frames);
    CHECK(summary.rendered == 50);
    CHECK_THAT(summary.cpu.p50Ms, WithinAbs(50.0, 1e-9));
    CHECK_THAT(summary.cpu.p99Ms, WithinAbs(99.0, 1e-9));
    CHECK_THAT(summary.cpu.maxMs, WithinAbs(100.0, 1e-9));
    CHECK(summary.gpu.count == 90);
    CHECK_THAT(summary.gpu.p50Ms, WithinAbs(2.0, 1e-9));
}

TEST_CASE("xr_spike/frames: nothing rendered never meets the criteria", "[xr_spike][frames]") {
    CHECK_FALSE(meetsCriteria(summarizeFrames({})));
    std::vector<FrameRecord> frames{{kPeriod72, kPeriod72, false, 1.0, std::nullopt},
                                    {2 * kPeriod72, kPeriod72, false, 1.0, std::nullopt}};
    CHECK_FALSE(meetsCriteria(summarizeFrames(frames)));
}

TEST_CASE("xr_spike/format: sRGB first, then RGBA8, in whatever order the runtime offers them",
          "[xr_spike][format]") {
    const std::vector<std::int64_t> both{kGlRgba16f, kGlRgba8, kGlSrgb8Alpha8};
    const auto srgb = chooseColorFormat(both);
    REQUIRE(srgb);
    CHECK(srgb->format == kGlSrgb8Alpha8);
    CHECK(srgb->srgb);

    const std::vector<std::int64_t> linear{kGlRgb10A2, kGlRgba8};
    const auto rgba = chooseColorFormat(linear);
    REQUIRE(rgba);
    CHECK(rgba->format == kGlRgba8);
    CHECK_FALSE(rgba->srgb);

    const std::vector<std::int64_t> neither{kGlRgba16f};
    CHECK_FALSE(chooseColorFormat(neither));

    CHECK(glFormatName(kGlSrgb8Alpha8) == "GL_SRGB8_ALPHA8");
    CHECK(glFormatName(0x1234) == "0x1234");
}
