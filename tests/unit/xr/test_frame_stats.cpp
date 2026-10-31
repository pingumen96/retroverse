#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <vector>

#include <rv/xr/frame_stats.hpp>
#include <rv/xr/swapchain_format.hpp>

using Catch::Matchers::WithinAbs;
using rv::xr::chooseColorFormat;
using rv::xr::FrameRecord;
using rv::xr::glFormatName;
using rv::xr::kGlRgb10A2;
using rv::xr::kGlRgba16f;
using rv::xr::kGlRgba8;
using rv::xr::kGlSrgb8Alpha8;
using rv::xr::summarizeFrames;

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

/// Frames at the given predicted display times, in periods of `period` after an arbitrary start.
std::vector<FrameRecord> framesAt(const std::vector<double>& periods, std::int64_t period = kPeriod72) {
    std::vector<FrameRecord> frames;
    for (const double at : periods) {
        const auto offset = static_cast<std::int64_t>(at * static_cast<double>(period));
        frames.push_back({5'000'000'000 + offset, period, true, 3.0, 4.0});
    }
    return frames;
}

} // namespace

TEST_CASE("xr/frames: a steady run misses no refresh (I10 step 2)", "[xr][frames]") {
    const auto frames = framesAdvancing(std::vector<int>(99, 1));
    const auto summary = summarizeFrames(frames);
    CHECK(summary.frames == 100);
    CHECK(summary.rendered == 100);
    CHECK(summary.missedRefreshes == 0);
    CHECK_THAT(summary.missedFraction, WithinAbs(0.0, 1e-12));
    CHECK_THAT(summary.refreshHz, WithinAbs(72.0, 1e-3));
}

TEST_CASE("xr/frames: a gap of several periods counts the refreshes it skipped (I10 step 2)", "[xr][frames]") {
    // 98 steady intervals, one skipping a refresh, one skipping two: 3 of 103 refreshes missed.
    std::vector<int> steps(98, 1);
    steps.push_back(2);
    steps.push_back(3);
    const auto summary = summarizeFrames(framesAdvancing(steps));
    CHECK(summary.missedRefreshes == 3);
    CHECK_THAT(summary.missedFraction, WithinAbs(3.0 / 103.0, 1e-9));
}

TEST_CASE("xr/frames: predictions that jitter around the refreshes miss nothing (I10 step 2)", "[xr][frames]") {
    // One frame per refresh, some predicted 0.6 of a period late and the next on time again (Meta's runtime over Link):
    // the long intervals round to two periods, the short ones to one, and no refresh went without a frame.
    std::vector<double> at;
    for (int i = 0; i < 100; ++i) {
        at.push_back(i % 10 == 3 ? i + 0.6 : static_cast<double>(i));
    }
    const auto summary = summarizeFrames(framesAt(at));
    CHECK(summary.missedRefreshes == 0);
    CHECK(summary.gaps.one == 79);
    CHECK(summary.gaps.oneAndHalf == 10);
    CHECK(summary.gaps.underOne == 10);
}

TEST_CASE("xr/frames: a refresh rate change starts another stretch (I10 step 2)", "[xr][frames]") {
    // 50 frames at 72 Hz, then 90 Hz with one refresh skipped: 1 missed of 49 + 51 refreshes.
    constexpr std::int64_t kPeriod90 = 11'111'111;
    std::vector<FrameRecord> frames = framesAt([] {
        std::vector<double> at;
        for (int i = 0; i < 50; ++i) {
            at.push_back(i);
        }
        return at;
    }());
    std::int64_t time = frames.back().displayTimeNs;
    for (int i = 1; i <= 50; ++i) {
        time += kPeriod90 * (i == 20 ? 2 : 1);
        frames.push_back({time, kPeriod90, true, 3.0, 4.0});
    }
    const auto summary = summarizeFrames(frames);
    CHECK(summary.missedRefreshes == 1);
    CHECK_THAT(summary.missedFraction, WithinAbs(1.0 / 100.0, 1e-9));
    CHECK(summary.gaps.two == 1);
    CHECK(summary.gaps.one == 98);
}

TEST_CASE("xr/frames: durations use nearest-rank percentiles, the GPU only where it arrived (I10 step 2)",
          "[xr][frames]") {
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

TEST_CASE("xr/format: sRGB first, then RGBA8, in whatever order the runtime offers them (I10 step 2)",
          "[xr][format]") {
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
