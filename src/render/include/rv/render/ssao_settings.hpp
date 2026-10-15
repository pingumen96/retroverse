#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <array>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::gl {

/// Self-occlusion bias of the ambient occlusion estimate as a share of its radius (SsaoUniforms::bias): 1.2 cm at the
/// default 0.4 m, above the depth error of a flat surface between two pixels, below the gap of a contact.
inline constexpr float kSsaoBiasRadiusFraction = 0.03f;
/// Relative depth difference at which a tap of the ambient occlusion blur stops counting
/// (SsaoUniforms::depthTolerance). Taps are compared with the depth the center pixel's surface has at their place (its
/// plane), so flat surfaces blur fully even at grazing angles while the occlusion of one surface never bleeds onto
/// another 2% nearer or farther (4 cm at 2 m: the front of a shelf and the wall behind it stay apart).
inline constexpr float kSsaoBlurDepthTolerance = 0.02f;
/// Smallest ambient occlusion radius the passes use, meters (a scene's radius is > 0; edits are clamped to this).
inline constexpr float kMinSsaoRadius = 0.01f;

/// The kSsaoSampleCount sample offsets of the ambient occlusion estimate (SsaoUniforms::kernel), deterministic: unit
/// directions of a Hammersley set over the hemisphere around +Z, cosine-weighted (most samples near the normal, the
/// lowest about 10 degrees above the surface, so a flat surface never occludes itself), scaled from about a tenth of
/// the radius to the full radius, denser near the point (quadratic), in an order that does not follow the directions.
/// w is 0.
[[nodiscard]] std::array<glm::vec4, kSsaoSampleCount> ssaoKernel() noexcept;

/// The SsaoUniforms of a view: the kernel, `setup`'s radius clamped to [kMinSsaoRadius, kMaxSsaoRadius] (non-finite
/// values counting as the default 0.4 m) and intensity clamped to [0, kMaxSsaoIntensity] (non-finite values counting as
/// 1), the bias kSsaoBiasRadiusFraction of the radius and the blur's kSsaoBlurDepthTolerance. `setup.enabled` is the
/// renderer's switch and does not reach the block.
[[nodiscard]] SsaoUniforms makeSsaoUniforms(const scene::SsaoSetup& setup) noexcept;

/// Size of the half-resolution occlusion target of a view of `viewSize` pixels (ViewTargets::ambientOcclusionRaw()):
/// half of each side rounded up, at least 1 x 1; sides below 1 count as 1.
[[nodiscard]] glm::ivec2 ssaoTargetSize(glm::ivec2 viewSize) noexcept;

} // namespace rv::gl
