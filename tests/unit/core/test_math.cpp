#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <rv/core/math.hpp>

using Catch::Approx;

TEST_CASE("core/math: sRGB transfer function round trip", "[core][math][color]") {
    CHECK(rv::core::srgbToLinear(0.0f) == Approx(0.0f));
    CHECK(rv::core::srgbToLinear(1.0f) == Approx(1.0f));
    // Linear segment below the 0.04045 threshold.
    CHECK(rv::core::srgbToLinear(0.04f) == Approx(0.04f / 12.92f));
    for (float v = 0.0f; v <= 1.0f; v += 0.05f) {
        CHECK(rv::core::linearToSrgb(rv::core::srgbToLinear(v)) == Approx(v).margin(1e-5));
    }
    // Out-of-range inputs are clamped.
    CHECK(rv::core::srgbToLinear(2.0f) == Approx(1.0f));
    CHECK(rv::core::linearToSrgb(-1.0f) == Approx(0.0f));
}

TEST_CASE("core/math: hex colors parse as sRGB and convert to linear", "[core][math][color]") {
    const auto gray = rv::core::hexToLinearRgb("#808080");
    REQUIRE(gray.has_value());
    CHECK(gray->x == Approx(0.2158605f).margin(1e-4));
    CHECK(gray->y == Approx(gray->x));
    CHECK(gray->z == Approx(gray->x));

    // Style guide palette: walnut #493024.
    const auto walnut = rv::core::parseHexColor("493024");
    REQUIRE(walnut.has_value());
    CHECK(walnut->x == Approx(0x49 / 255.0f));
    CHECK(walnut->y == Approx(0x30 / 255.0f));
    CHECK(walnut->z == Approx(0x24 / 255.0f));
    CHECK(walnut->w == Approx(1.0f));

    const auto withAlpha = rv::core::parseHexColor("#FFC88A80");
    REQUIRE(withAlpha.has_value());
    CHECK(withAlpha->w == Approx(128.0f / 255.0f));

    const auto lowercase = rv::core::parseHexColor("#ffc88a");
    REQUIRE(lowercase.has_value());
    CHECK(lowercase->x == Approx(1.0f));

    CHECK_FALSE(rv::core::parseHexColor("#12345").has_value());
    CHECK_FALSE(rv::core::parseHexColor("#GG0000").has_value());
    CHECK_FALSE(rv::core::parseHexColor("").has_value());
    CHECK(rv::core::parseHexColor("#12").error().code == rv::ErrorCode::ParseError);
}

TEST_CASE("core/math: angle helpers", "[core][math]") {
    CHECK(rv::core::degreesToRadians(180.0f) == Approx(rv::core::kPi));
    CHECK(rv::core::radiansToDegrees(rv::core::kHalfPi) == Approx(90.0f));
    CHECK(rv::core::wrapAngleRadians(0.0f) == Approx(0.0f).margin(1e-6));
    CHECK(rv::core::wrapAngleRadians(rv::core::kPi) == Approx(rv::core::kPi));
    CHECK(rv::core::wrapAngleRadians(-rv::core::kPi) == Approx(rv::core::kPi));
    CHECK(rv::core::wrapAngleRadians(3.0f * rv::core::kPi) == Approx(rv::core::kPi));
    CHECK(rv::core::wrapAngleRadians(rv::core::kTwoPi + 0.5f) == Approx(0.5f).margin(1e-5));
}

TEST_CASE("core/math: tolerant comparisons and finiteness", "[core][math]") {
    CHECK(rv::core::nearlyEqual(1.0f, 1.0f + 1e-7f));
    CHECK_FALSE(rv::core::nearlyEqual(1.0f, 1.01f));
    CHECK(rv::core::nearlyEqual(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 2.0f, 3.0f + 1e-7f)));
    CHECK(rv::core::isFinite(glm::mat4(1.0f)));
    glm::vec3 bad(1.0f);
    bad.y = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(rv::core::isFinite(bad));
}

TEST_CASE("core/math: ray evaluation", "[core][math]") {
    const rv::core::Ray ray{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, -2.0f}};
    CHECK(rv::core::nearlyEqual(ray.at(1.5f), glm::vec3(1.0f, 2.0f, 0.0f)));
}
