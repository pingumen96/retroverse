#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <glm/vec3.hpp>

#include <rv/render/tonemap.hpp>

using Catch::Approx;

namespace {

void checkColor(const glm::vec3& actual, const glm::vec3& expected) {
    CHECK(actual.r == Approx(expected.r).margin(1e-5));
    CHECK(actual.g == Approx(expected.g).margin(1e-5));
    CHECK(actual.b == Approx(expected.b).margin(1e-5));
}

} // namespace

TEST_CASE("render/tonemap: the CPU mirror gives the ACES fitted values of the shaders", "[render][tonemap]") {
    // Reference values of Hill's fit (input matrix, RRT + ODT curve, output matrix), computed independently.
    checkColor(rv::gl::tonemapAcesFitted(glm::vec3(0.18f)), glm::vec3(0.105591f));
    checkColor(rv::gl::tonemapAcesFitted(glm::vec3(1.0f, 0.5f, 0.25f)), glm::vec3(0.634990f, 0.384600f, 0.203160f));
    checkColor(rv::gl::tonemapAcesFitted(glm::vec3(0.02f, 0.01f, 0.005f)),
               glm::vec3(0.00290310f, 0.00107689f, 0.00033797f));
    checkColor(rv::gl::tonemapAcesFitted(glm::vec3(0.0f)), glm::vec3(0.0f));
    // Grey stays grey; the curve rises monotonically and saturates at 1.
    float previous = -1.0f;
    for (float value = 0.0f; value <= 64.0f; value = value * 1.5f + 0.01f) {
        const glm::vec3 mapped = rv::gl::tonemapAcesFitted(glm::vec3(value));
        CHECK(mapped.r == Approx(mapped.g).margin(1e-5));
        CHECK(mapped.r == Approx(mapped.b).margin(1e-5));
        CHECK(mapped.r >= previous);
        previous = mapped.r;
    }
    checkColor(rv::gl::tonemapAcesFitted(glm::vec3(1e4f)), glm::vec3(1.0f));
}

TEST_CASE("render/tonemap: exposure scales the radiance and bad input becomes black or white", "[render][tonemap]") {
    checkColor(rv::gl::tonemap(glm::vec3(1.0f, 0.5f, 0.25f), 2.0f), glm::vec3(0.829792f, 0.630282f, 0.430278f));
    checkColor(rv::gl::tonemap(glm::vec3(0.09f), 2.0f), rv::gl::tonemapAcesFitted(glm::vec3(0.18f)));

    // Negative and NaN components count as 0, infinite ones as the input limit: never NaN on screen.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    checkColor(rv::gl::tonemap(glm::vec3(-1.0f, nan, 0.0f), 1.0f), glm::vec3(0.0f));
    checkColor(rv::gl::tonemap(glm::vec3(0.18f), nan), glm::vec3(0.0f));
    checkColor(rv::gl::tonemap(glm::vec3(infinity), 1.0f), glm::vec3(1.0f));
    checkColor(rv::gl::tonemap(glm::vec3(infinity, 0.0f, 0.0f), 1.0f),
               rv::gl::tonemapAcesFitted(glm::vec3(rv::gl::kTonemapInputLimit, 0.0f, 0.0f)));
}

TEST_CASE("render/tonemap: the grey radiance a display value needs inverts the tonemap", "[render][tonemap]") {
    // What the sRGB self-check draws: the radiance shown as linear 0.5 (sRGB 188) at exposure 1.
    const float half = rv::gl::greyRadianceForDisplay(0.5f, 1.0f);
    CHECK(half > 0.5f);
    CHECK(half < 1.0f);
    checkColor(rv::gl::tonemap(glm::vec3(half), 1.0f), glm::vec3(0.5f));
    for (const float display : {0.01f, 0.1f, 0.5f, 0.9f}) {
        for (const float exposure : {0.5f, 1.0f, 1.5f}) {
            INFO(display << " at exposure " << exposure);
            const float radiance = rv::gl::greyRadianceForDisplay(display, exposure);
            checkColor(rv::gl::tonemap(glm::vec3(radiance), exposure), glm::vec3(display));
            // Exposure scales the radiance needed down.
            CHECK(radiance * exposure == Approx(rv::gl::greyRadianceForDisplay(display, 1.0f)).epsilon(1e-4));
        }
    }
    // Black needs no light; a bad exposure counts as 1.
    CHECK(rv::gl::greyRadianceForDisplay(0.0f, 1.0f) == 0.0f);
    CHECK(rv::gl::greyRadianceForDisplay(-1.0f, 1.0f) == 0.0f);
    CHECK(rv::gl::greyRadianceForDisplay(0.5f, 0.0f) == half);
    CHECK(rv::gl::greyRadianceForDisplay(0.5f, std::numeric_limits<float>::quiet_NaN()) == half);
}
