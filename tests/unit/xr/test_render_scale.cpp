// The eyes' dynamic render scale in VR (I10 step 7, ADR-0017 §9): down by steps while the GPU's time stays above the
// display's budget, up again once it stays below, never past its bounds, never twice within the settle frames.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <rv/xr/render_scale.hpp>

using Catch::Approx;
using rv::xr::RenderScale;

namespace {

constexpr double kPeriod = 1000.0 / 72.0;

float run(RenderScale& scale, double gpuMs, int frames) {
    float last = scale.scale();
    for (int frame = 0; frame < frames; ++frame) {
        last = scale.update(gpuMs, kPeriod);
    }
    return last;
}

} // namespace

TEST_CASE("xr/render_scale: down while the GPU is over budget, up once it has room", "[xr][render_scale]") {
    RenderScale scale;
    CHECK(scale.scale() == 1.0f);
    // 13 ms of a 13.9 ms period: above 85%; the first step after the settle frames.
    run(scale, 13.0, 35);
    CHECK(scale.scale() == 1.0f);
    run(scale, 13.0, 1);
    CHECK(scale.scale() == Approx(0.95f));
    // Still over: a step every 36 frames, down to the minimum and no further.
    run(scale, 13.0, 36 * 20);
    CHECK(scale.scale() == Approx(0.6f));
    CHECK(scale.lowest() == Approx(0.6f));
    // In between (75%): it holds.
    const std::uint64_t changes = scale.changes();
    run(scale, 0.75 * kPeriod, 200);
    CHECK(scale.changes() == changes);
    // Light (5 ms): back up by steps, to the maximum.
    run(scale, 5.0, 36 * 20);
    CHECK(scale.scale() == Approx(1.0f));
}

TEST_CASE("xr/render_scale: the in-game bound, a spike, bad times", "[xr][render_scale]") {
    RenderScale scale;
    // One frame's spike moves nothing: the smoothing holds it.
    run(scale, 5.0, 50);
    run(scale, 40.0, 1);
    run(scale, 5.0, 40);
    CHECK(scale.scale() == 1.0f);
    // The "in game" bound brings it down at once, and keeps it there however light the frames.
    scale.setMax(0.8f);
    CHECK(scale.scale() == Approx(0.8f));
    run(scale, 2.0, 36 * 10);
    CHECK(scale.scale() == Approx(0.8f));
    scale.setMax(1.0f);
    run(scale, 2.0, 36 * 10);
    CHECK(scale.scale() == Approx(1.0f));
    // Times that are not finite or not above 0 change nothing.
    const float before = scale.scale();
    CHECK(scale.update(std::numeric_limits<double>::quiet_NaN(), kPeriod) == before);
    CHECK(scale.update(10.0, 0.0) == before);
    CHECK(scale.update(-3.0, kPeriod) == before);
}
