// The hero screen's spill light following its picture (rv/app/crt_glow.hpp, I12 step 6): the picture's hue mixed into
// the light's colour, its brightness scaling the light's intensity within bounds, eased over 0.6 s, at once when asked.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

#include <glm/glm.hpp>

#include <rv/app/crt_glow.hpp>

using Catch::Approx;
using rv::app::CrtGlow;

TEST_CASE("app/crt_glow: the glow takes the picture's hue and follows its brightness", "[app][atmosphere]") {
    const CrtGlow base{glm::vec3(0.6f, 0.7f, 1.0f), 2.0f};
    // A black picture: the light's colour, at its dimmest.
    const CrtGlow black = rv::app::crtGlowFor(base, {0.0f, 0.0f, 0.0f});
    CHECK(black.color == base.color);
    CHECK(black.intensity == Approx(2.0f * rv::app::kCrtGlowDimmest));
    // A red picture: the colour moves towards red, the intensity with its luminance (0.2126 * 0.5).
    const CrtGlow red = rv::app::crtGlowFor(base, {0.5f, 0.0f, 0.0f});
    CHECK(red.color.r == Approx(0.6f + (1.0f - 0.6f) * rv::app::kCrtGlowHue));
    CHECK(red.color.g == Approx(0.7f * (1.0f - rv::app::kCrtGlowHue)));
    CHECK(red.intensity == Approx(2.0f * (0.35f + 2.6f * 0.2126f * 0.5f)));
    // A white picture: never brighter than kCrtGlowBrightest times the light's.
    CHECK(rv::app::crtGlowFor(base, {1.0f, 1.0f, 1.0f}).intensity == Approx(2.0f * rv::app::kCrtGlowBrightest));
    // Values that are not finite count as black.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(rv::app::crtGlowFor(base, {nan, nan, nan}) == black);
}

TEST_CASE("app/crt_glow: the glow eases over its time constant, at once when asked", "[app][atmosphere]") {
    const CrtGlow from{glm::vec3(1.0f), 1.0f};
    const CrtGlow to{glm::vec3(0.0f), 3.0f};
    CHECK(rv::app::easedCrtGlow(from, to, 0.1f, true) == to);
    const CrtGlow step = rv::app::easedCrtGlow(from, to, rv::app::kCrtGlowSeconds, false);
    const float t = 1.0f - std::exp(-1.0f);
    CHECK(step.intensity == Approx(1.0f + 2.0f * t));
    CHECK(step.color.r == Approx(1.0f - t));
    CHECK(rv::app::easedCrtGlow(from, to, 0.0f, false) == from);
    // Many small steps reach where one long one does.
    CrtGlow walked = from;
    for (int i = 0; i < 60; ++i) {
        walked = rv::app::easedCrtGlow(walked, to, 0.01f, false);
    }
    CHECK(walked.intensity == Approx(step.intensity).epsilon(1e-4));
}
