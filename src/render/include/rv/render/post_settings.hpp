#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <array>
#include <cstddef>

#include <glm/vec2.hpp>

#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::gl {

/// Levels of the bloom chain: the first is half the size of the view, each next one half the previous one.
inline constexpr std::size_t kBloomLevelCount = 6;

/// Peak-to-peak dither noise of the views drawn into 8-bit targets: one quantization step.
inline constexpr float kDitherAmplitude = 1.0f / 255.0f;

/// Exposed brightness (the largest component of radiance * exposure) above which light blooms in full, and the half
/// width of the soft knee below it (PostUniforms::bloomThreshold, bloomKnee): 1 is where the ACES curve starts to
/// compress, so screens, lamp shades, neon and highlights glow while the lit room keeps its crisp detail.
inline constexpr float kBloomThreshold = 1.0f;
inline constexpr float kBloomKnee = 0.5f;

/// What the post-processing does to the HDR scene of a view (gl::PostProcessor): bloom, exposure, vignette, the ACES
/// fitted tonemap and dithering. The defaults change nothing but the tonemap: exposure 1, no bloom, no vignette, no
/// dithering.
struct PostSettings {
    /// > 0: multiplies the scene radiance before tonemapping.
    float exposure = 1.0f;
    /// [0, 1]: how much light above the bloom threshold the bloom spreads, per bloom level (bloomMix()); 0 skips the
    /// bloom chain.
    float bloomStrength = 0.0f;
    /// [0, 1]: how far the bloom spreads (bloomScatter()).
    float bloomRadius = 0.0f;
    /// [0, 1]: darkening of the corners (PostUniforms::vignette).
    float vignette = 0.0f;
    /// [0, 1]: peak-to-peak noise added to the sRGB-encoded output (kDitherAmplitude for 8-bit targets).
    float ditherAmplitude = 0.0f;
};

/// The post-processing a scene's environment asks for: its exposure, bloom strength and radius and vignette, dithered
/// with kDitherAmplitude.
[[nodiscard]] PostSettings postSettingsFor(const scene::EnvironmentSetup& environment) noexcept;

/// The PostUniforms block of `settings` with every value in its documented range: a non-finite or non-positive
/// exposure becomes 1; the bloom mix is bloomMix(bloomStrength); the vignette and the dither amplitude are clamped to
/// [0, 1], non-finite values becoming 0; the bloom threshold and knee are kBloomThreshold and kBloomKnee.
[[nodiscard]] PostUniforms makePostUniforms(const PostSettings& settings) noexcept;

/// Share of the light above the bloom threshold that the bloom spreads (PostUniforms::bloomMix) for a bloom strength:
/// each of the kBloomLevelCount levels takes `strength` of what the levels before it left, 1 - (1 - strength)^n in
/// all, close to n * strength for the usual small strengths (the mix of the summed bloom levels in Jimenez 2014's
/// chain, which a strength of 0.04 to 0.08 keeps subtle). `strength` is clamped to [0, 1], non-finite values becoming
/// 0.
[[nodiscard]] float bloomMix(float strength) noexcept;

/// Weight of the wider level in each upsampling step of the bloom chain, level[i] = mix(level[i],
/// upsampled(level[i + 1]), scatter), from the bloom radius: `radius` clamped to [0, 1], non-finite values becoming 0.
/// Level i of n then reaches the bloom with the weight (1 - scatter) * scatter^i and the last one with
/// scatter^(n - 1); the weights add up to 1, so the chain conserves the energy of the image (its filters do too).
[[nodiscard]] float bloomScatter(float radius) noexcept;

/// Sizes of the bloom levels for a view of `viewSize` pixels: each half the previous one (the first half the view),
/// rounded up so that no edge pixel is lost, at least 1 x 1. Sides below 1 count as 1.
[[nodiscard]] std::array<glm::ivec2, kBloomLevelCount> bloomLevelSizes(glm::ivec2 viewSize) noexcept;

} // namespace rv::gl
