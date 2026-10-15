#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <set>

#include <glm/glm.hpp>

#include <rv/render/ssao_settings.hpp>
#include <rv/scene/scene_description.hpp>

using Catch::Approx;

TEST_CASE("render/ssao_settings: the kernel samples the hemisphere above a surface, denser near it", "[render][ssao]") {
    const std::array<glm::vec4, rv::gl::kSsaoSampleCount> kernel = rv::gl::ssaoKernel();
    float total = 0.0f;
    float longest = 0.0f;
    std::set<int> quadrants;
    for (const glm::vec4& sample : kernel) {
        const glm::vec3 offset(sample);
        const float length = glm::length(offset);
        INFO("sample " << offset.x << ", " << offset.y << ", " << offset.z);
        CHECK(sample.w == 0.0f);
        // From a tenth of the radius to the radius.
        CHECK(length >= 0.1f - 1e-5f);
        CHECK(length <= 1.0f + 1e-5f);
        // Above the surface by about 10 degrees at least: a flat surface never occludes its own samples.
        CHECK(offset.z / length > std::sin(0.17f));
        total += length;
        longest = std::max(longest, length);
        quadrants.insert((offset.x >= 0.0f ? 1 : 0) + (offset.y >= 0.0f ? 2 : 0));
    }
    // Some samples reach the full radius; most stay near the point, where occluders matter most.
    CHECK(longest == Approx(1.0f));
    CHECK(total / static_cast<float>(rv::gl::kSsaoSampleCount) < 0.5f);
    // Every direction around the normal is sampled.
    CHECK(quadrants.size() == 4);
    // Deterministic: the same kernel every time.
    CHECK(rv::gl::ssaoKernel() == kernel);
}

TEST_CASE("render/ssao_settings: the block keeps the scene's settings in range", "[render][ssao]") {
    rv::scene::SsaoSetup setup;
    rv::gl::SsaoUniforms uniforms = rv::gl::makeSsaoUniforms(setup);
    CHECK(uniforms.radius == 0.4f);
    CHECK(uniforms.intensity == 1.0f);
    CHECK(uniforms.bias == Approx(rv::gl::kSsaoBiasRadiusFraction * 0.4f));
    CHECK(uniforms.depthTolerance == rv::gl::kSsaoBlurDepthTolerance);
    const std::array<glm::vec4, rv::gl::kSsaoSampleCount> kernel = rv::gl::ssaoKernel();
    for (std::size_t index = 0; index < kernel.size(); ++index) {
        CHECK(uniforms.kernel[index] == kernel[index]);
    }
    // The bias is about a centimeter at room scale: above the depth error of a flat surface, below a contact gap.
    CHECK(uniforms.bias > 0.005f);
    CHECK(uniforms.bias < 0.02f);

    setup.radius = 1.5f;
    setup.intensity = 2.5f;
    uniforms = rv::gl::makeSsaoUniforms(setup);
    CHECK(uniforms.radius == 1.5f);
    CHECK(uniforms.intensity == 2.5f);
    CHECK(uniforms.bias == Approx(rv::gl::kSsaoBiasRadiusFraction * 1.5f));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    setup.radius = 0.0f;
    setup.intensity = -1.0f;
    uniforms = rv::gl::makeSsaoUniforms(setup);
    CHECK(uniforms.radius == rv::gl::kMinSsaoRadius);
    CHECK(uniforms.intensity == 0.0f);
    setup.radius = 100.0f;
    setup.intensity = 100.0f;
    uniforms = rv::gl::makeSsaoUniforms(setup);
    CHECK(uniforms.radius == rv::scene::kMaxSsaoRadius);
    CHECK(uniforms.intensity == rv::scene::kMaxSsaoIntensity);
    setup.radius = nan;
    setup.intensity = nan;
    uniforms = rv::gl::makeSsaoUniforms(setup);
    CHECK(uniforms.radius == rv::scene::SsaoSetup{}.radius);
    CHECK(uniforms.intensity == 1.0f);
    // Switching it off is the renderer's business: the block does not change.
    setup = rv::scene::SsaoSetup{};
    setup.enabled = false;
    CHECK(rv::gl::makeSsaoUniforms(setup).radius == 0.4f);
}

TEST_CASE("render/ssao_settings: the estimate is half the size of the view, rounded up", "[render][ssao]") {
    CHECK(rv::gl::ssaoTargetSize(glm::ivec2(1920, 1080)) == glm::ivec2(960, 540));
    CHECK(rv::gl::ssaoTargetSize(glm::ivec2(1281, 721)) == glm::ivec2(641, 361));
    CHECK(rv::gl::ssaoTargetSize(glm::ivec2(1, 1)) == glm::ivec2(1, 1));
    CHECK(rv::gl::ssaoTargetSize(glm::ivec2(0, -5)) == glm::ivec2(1, 1));
    // Every full-resolution pixel 2 * texel of the estimate exists: the blur reads the estimate at the pixels it was
    // taken at.
    for (const int side : {1, 2, 3, 7, 8, 1079, 1080}) {
        const int half = rv::gl::ssaoTargetSize(glm::ivec2(side)).x;
        CHECK(2 * (half - 1) <= side - 1);
        CHECK(2 * half >= side);
    }
}
