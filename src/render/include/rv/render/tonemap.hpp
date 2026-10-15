#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <array>

#include <glm/vec3.hpp>

namespace rv::gl {

/// CPU mirror of the display transform of the post pass (assets/shaders/common/tonemap.glsl, applied by
/// post/post.frag): exposure, then the ACES fitted curve (Stephen Hill, BakingLab). It tells on the CPU how a scene
/// radiance is displayed (bloom, vignette and dithering aside), as the tests of the colors a view must show do (the
/// model viewer's background). The constants below are pinned against the GLSL by
/// tests/unit/integration/test_shader_sources.cpp.

/// Linear sRGB -> ACES AP1-like space (RRT saturation included): RV_ACES_INPUT, column by column as in the GLSL mat3
/// constructor.
inline constexpr std::array<float, 9> kAcesInputColumns{0.59719f, 0.07600f, 0.02840f, //
                                                        0.35458f, 0.90834f, 0.13383f, //
                                                        0.04823f, 0.01566f, 0.83777f};
/// Back to linear sRGB (ODT saturation included): RV_ACES_OUTPUT, column by column.
inline constexpr std::array<float, 9> kAcesOutputColumns{1.60475f,  -0.10208f, -0.00327f, //
                                                         -0.53108f, 1.10813f,  -0.07276f, //
                                                         -0.07367f, -0.00605f, 1.07602f};

/// Coefficients of the RRT + ODT curve fit applied to each component v: (v * (v + a) - b) / (v * (c * v + d) + e).
struct AcesCurveFit {
    float a;
    float b;
    float c;
    float d;
    float e;
};
inline constexpr AcesCurveFit kAcesCurveFit{0.0245786f, 0.000090537f, 0.983729f, 0.4329510f, 0.238081f};

/// Largest exposed component the curve receives (the largest finite half float).
inline constexpr float kTonemapInputLimit = 65504.0f;

/// tonemapAcesFitted() of tonemap.glsl: input matrix, curve fit per component, output matrix, clamped to [0, 1].
/// `color` is linear sRGB, the result linear.
[[nodiscard]] glm::vec3 tonemapAcesFitted(const glm::vec3& color) noexcept;

/// tonemap() of tonemap.glsl: linear scene radiance -> linear display value. `radiance` * `exposure` with negative and
/// NaN components as 0 and larger ones limited to kTonemapInputLimit, then tonemapAcesFitted(). The sRGB output target
/// encodes the result like the output of the post pass.
[[nodiscard]] glm::vec3 tonemap(const glm::vec3& radiance, float exposure) noexcept;

/// The inverse of tonemap() for greys (the ACES fit keeps grey grey): the radiance r for which tonemap(vec3(r),
/// `exposure`) shows `display` in each component, found by bisection, so that a test or check can draw the radiance a
/// view must show as a given display value (the sRGB self-check: linear 0.5). `display` at or below 0 gives 0, at or
/// above the largest display value the radiance that reaches it; a non-finite or non-positive exposure counts as 1.
[[nodiscard]] float greyRadianceForDisplay(float display, float exposure) noexcept;

} // namespace rv::gl
