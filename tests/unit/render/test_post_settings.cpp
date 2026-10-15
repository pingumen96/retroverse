#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/core/math.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/tonemap.hpp>
#include <rv/scene/scene_description.hpp>

using Catch::Approx;
using rv::gl::PostSettings;
using rv::gl::PostUniforms;

TEST_CASE("render/post_settings: a scene's environment gives the post-processing of its views", "[render][post]") {
    rv::scene::EnvironmentSetup environment;
    environment.exposure = 1.5f;
    environment.bloomStrength = 0.08f;
    environment.bloomRadius = 0.6f;
    environment.vignette = 0.1f;
    const PostSettings settings = rv::gl::postSettingsFor(environment);
    CHECK(settings.exposure == 1.5f);
    CHECK(settings.bloomStrength == 0.08f);
    CHECK(settings.bloomRadius == 0.6f);
    CHECK(settings.vignette == 0.1f);
    // Views go to 8-bit targets: one quantization step of dither noise; their edges are anti-aliased.
    CHECK(settings.ditherAmplitude == rv::gl::kDitherAmplitude);
    CHECK(rv::gl::kDitherAmplitude == Approx(1.0f / 255.0f));
    CHECK(settings.antialiasing);
}

TEST_CASE("render/post_settings: the default settings change nothing but the tonemap", "[render][post]") {
    // What the sRGB self-check relies on: exposure 1, no bloom, no vignette, no dithering, no anti-aliasing pass.
    CHECK_FALSE(PostSettings{}.antialiasing);
    const PostUniforms uniforms = rv::gl::makePostUniforms(PostSettings{});
    CHECK(uniforms.exposure == 1.0f);
    CHECK(uniforms.bloomMix == 0.0f);
    CHECK(uniforms.vignette == 0.0f);
    CHECK(uniforms.ditherAmplitude == 0.0f);
    CHECK(uniforms.bloomThreshold == rv::gl::kBloomThreshold);
    CHECK(uniforms.bloomKnee == rv::gl::kBloomKnee);
    CHECK(uniforms.padding[0] == 0.0f);
    CHECK(uniforms.padding[1] == 0.0f);
}

TEST_CASE("render/post_settings: the post block keeps every value in its documented range", "[render][post]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();

    PostSettings settings;
    settings.exposure = 2.5f;
    settings.bloomStrength = 0.06f;
    settings.vignette = 0.05f;
    settings.ditherAmplitude = rv::gl::kDitherAmplitude;
    PostUniforms uniforms = rv::gl::makePostUniforms(settings);
    CHECK(uniforms.exposure == 2.5f);
    // The strength is the share of the light that blooms moved into the glow: the bloom mix itself.
    CHECK(uniforms.bloomMix == 0.06f);
    CHECK(uniforms.vignette == 0.05f);
    CHECK(uniforms.ditherAmplitude == rv::gl::kDitherAmplitude);

    // Exposure: non-finite or non-positive values become 1.
    for (const float exposure : {0.0f, -1.0f, nan, infinity}) {
        settings.exposure = exposure;
        CHECK(rv::gl::makePostUniforms(settings).exposure == 1.0f);
    }
    // Shares: clamped to [0, 1], non-finite values becoming 0.
    settings.bloomStrength = 2.0f;
    settings.vignette = -0.5f;
    settings.ditherAmplitude = 3.0f;
    uniforms = rv::gl::makePostUniforms(settings);
    CHECK(uniforms.bloomMix == 1.0f);
    CHECK(uniforms.vignette == 0.0f);
    CHECK(uniforms.ditherAmplitude == 1.0f);
    settings.bloomStrength = nan;
    settings.vignette = infinity;
    settings.ditherAmplitude = nan;
    uniforms = rv::gl::makePostUniforms(settings);
    CHECK(uniforms.bloomMix == 0.0f);
    CHECK(uniforms.vignette == 0.0f);
    CHECK(uniforms.ditherAmplitude == 0.0f);

    // The soft knee starts above 0 and never beyond the threshold.
    CHECK(rv::gl::kBloomThreshold > 0.0f);
    CHECK(rv::gl::kBloomKnee >= 0.0f);
    CHECK(rv::gl::kBloomKnee <= rv::gl::kBloomThreshold);
}

TEST_CASE("render/post_settings: the bloom strength is the share of the glowing light moved into the glow",
          "[render][post]") {
    PostSettings settings;
    for (const float strength : {0.0f, 0.04f, 0.06f, 0.08f, 1.0f}) {
        settings.bloomStrength = strength;
        CHECK(rv::gl::makePostUniforms(settings).bloomMix == strength);
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float wrong : {-0.5f, nan, -std::numeric_limits<float>::infinity()}) {
        settings.bloomStrength = wrong;
        CHECK(rv::gl::makePostUniforms(settings).bloomMix == 0.0f);
    }
    settings.bloomStrength = 1.5f;
    CHECK(rv::gl::makePostUniforms(settings).bloomMix == 1.0f);
    // The default of the scene files is subtle (style §4.3): a few percent of the light of screens and lamp shades.
    CHECK(rv::scene::EnvironmentSetup{}.bloomStrength >= 0.04f);
    CHECK(rv::scene::EnvironmentSetup{}.bloomStrength <= 0.08f);
}

TEST_CASE("render/post_settings: only highlights of the reflected light reach the bloom threshold", "[render][post]") {
    // Where the knee starts, the ACES curve shows a lit surface at about sRGB 240 or brighter: the brightest lit paper,
    // labels and wood of the rooms stay below and never bloom (the emitted light always does).
    const float kneeStart = rv::gl::kBloomThreshold - rv::gl::kBloomKnee;
    const float shown = rv::core::linearToSrgb(rv::gl::tonemap(glm::vec3(kneeStart), 1.0f).g) * 255.0f;
    CHECK(shown > 238.0f);
    // The lit paper of the test room's posters, exposed about 2 to 2.6 (sRGB 231 to 238), is well below.
    CHECK(kneeStart >= 3.0f);
    // A mirror-like highlight of a light (10 to 1000 times white) blooms in full.
    CHECK(rv::gl::kBloomThreshold + rv::gl::kBloomKnee < 10.0f);
}

TEST_CASE("render/post_settings: the bloom radius is the scatter of the upsampling blend", "[render][post]") {
    CHECK(rv::gl::bloomScatter(0.7f) == 0.7f);
    CHECK(rv::gl::bloomScatter(0.0f) == 0.0f);
    CHECK(rv::gl::bloomScatter(1.0f) == 1.0f);
    CHECK(rv::gl::bloomScatter(-1.0f) == 0.0f);
    CHECK(rv::gl::bloomScatter(4.0f) == 1.0f);
    CHECK(rv::gl::bloomScatter(std::numeric_limits<float>::infinity()) == 0.0f);
}

TEST_CASE("render/post_settings: bloom levels halve the view, rounded up, down to 1 x 1", "[render][post]") {
    using Sizes = std::array<glm::ivec2, rv::gl::kBloomLevelCount>;
    CHECK(rv::gl::kBloomLevelCount == 6);
    CHECK(rv::gl::bloomLevelSizes(glm::ivec2(1920, 1080)) == Sizes{glm::ivec2(960, 540), glm::ivec2(480, 270),
                                                                   glm::ivec2(240, 135), glm::ivec2(120, 68),
                                                                   glm::ivec2(60, 34), glm::ivec2(30, 17)});
    CHECK(rv::gl::bloomLevelSizes(glm::ivec2(1280, 720)) == Sizes{glm::ivec2(640, 360), glm::ivec2(320, 180),
                                                                  glm::ivec2(160, 90), glm::ivec2(80, 45),
                                                                  glm::ivec2(40, 23), glm::ivec2(20, 12)});
    // Odd sides round up, so the last row and column of the view still reach the bloom.
    CHECK(rv::gl::bloomLevelSizes(glm::ivec2(7, 3)) == Sizes{glm::ivec2(4, 2), glm::ivec2(2, 1), glm::ivec2(1, 1),
                                                             glm::ivec2(1, 1), glm::ivec2(1, 1), glm::ivec2(1, 1)});
    // Tiny or empty views still get valid 1 x 1 levels.
    for (const glm::ivec2 view : {glm::ivec2(1, 1), glm::ivec2(0, 0), glm::ivec2(-4, 8)}) {
        for (const glm::ivec2 level : rv::gl::bloomLevelSizes(view)) {
            CHECK(level.x >= 1);
            CHECK(level.y >= 1);
        }
    }
}
