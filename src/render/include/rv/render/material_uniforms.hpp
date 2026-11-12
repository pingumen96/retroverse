#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstdint>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

/// Shader family that draws `material`: ShaderKind::Unlit for assets::ShadingModel::Unlit (screens, neon, the magenta
/// fallback: base color * texture * emissiveStrength, post-processed like lit surfaces), ShaderKind::Pbr otherwise.
[[nodiscard]] scene::ShaderKind shaderKindOf(const assets::MaterialDesc& material) noexcept;

/// The MaterialUniforms block of `material`. Factors are copied, emissive = emissiveFactor * emissiveStrength, the
/// alpha mode and doubleSided become material_flags bits, and `textureFlags` adds the texture bits of the slots that
/// hold one of the material's textures (non-texture bits of `textureFlags` are ignored). A CRT effect
/// (MaterialDesc::crt) sets material_flags::kCrtScreen and the crt* fields. Values the shaders cannot use never reach
/// them: non-finite numbers take the MaterialDesc (or CrtEffect) defaults, metallic, roughness, occlusion strength,
/// alpha cutoff and the CRT strengths are clamped to [0, 1], the curvature to [0, 0.5], line and triad counts to at
/// least 1, colors and emissive strength to >= 0.
[[nodiscard]] MaterialUniforms makeMaterialUniforms(const assets::MaterialDesc& material,
                                                    std::uint32_t textureFlags) noexcept;

/// The factors of a material the developer overlay edits live (nothing is saved).
struct MaterialFactors {
    glm::vec4 baseColor{1.0f}; ///< linear RGBA: MaterialDesc::baseColorFactor
    float metallic = 0.0f;
    float roughness = 0.5f;
    /// Scales the emissive color of PBR materials and the color of unlit ones.
    float emissiveStrength = 1.0f;
};

/// The factors `material` describes.
[[nodiscard]] MaterialFactors materialFactors(const assets::MaterialDesc& material) noexcept;

/// `uniforms` with `factors` applied as makeMaterialUniforms() would apply them (same validation), the emissive color
/// becoming `emissiveFactor` (linear, MaterialDesc::emissiveFactor) times the new strength; every other field is kept.
/// For the block of a material whose factors are replaced, it equals makeMaterialUniforms() of the material with the
/// new factors.
[[nodiscard]] MaterialUniforms withFactors(const MaterialUniforms& uniforms, const MaterialFactors& factors,
                                           const glm::vec3& emissiveFactor) noexcept;

/// How a flickering emission varies (I12 step 8).
enum class FlickerMode : std::uint8_t {
    Dips,  ///< a neon (I12 step 6): one short stutter now and then
    Blink, ///< an activity LED: on or off in slots of 1 / (8 rate) s, on in about 60 % of them
    Pulse, ///< a lava lamp's wax: one slow breath every 10 / rate s
};

/// A neon's flicker (I12 step 6), an LED's blink or a lamp's breath (I12 step 8): MaterialUniforms' flicker* fields.
struct EmissiveFlicker {
    std::uint32_t seed = 0; ///< below 2^24 (a float holds it exactly)
    float rate = 1.0f;      ///< dips about every 8 s / rate; 0: none (steady)
    float depth = 0.6f;     ///< the share of the emission a dip, an off slot or a breath's low takes away
    FlickerMode mode = FlickerMode::Dips;

    bool operator==(const EmissiveFlicker&) const = default;
};

/// A screen's static (I12 step 8): its seed and speed (MaterialUniforms' flickerSeed and flickerRate with
/// material_flags::kCrtStatic).
struct ScreenStatic {
    std::uint32_t seed = 0; ///< below 2^24
    float rate = 1.0f;      ///< 24 new pictures a second times rate; 0: the snow stands still (reduceMotion)

    bool operator==(const ScreenStatic&) const = default;
};

/// No dip before this many seconds of the run (a frame-limited run's screenshots never catch one).
inline constexpr float kFirstFlickerSeconds = 4.0f;

/// The share of its emission a flickering material shows at `time` (common/material.glsl's materialFlicker, formula for
/// formula): 1 before kFirstFlickerSeconds, with a rate or depth of 0 or a time that is not finite; otherwise the run
/// is cut into slots of 8 / rate seconds, each holding one dip of 80 to 120 ms at a place its seed and index choose, a
/// stutter (1 - depth, then 1 - 0.35 depth, then 1 - 0.8 depth) within it. FlickerMode::Blink cuts the time after
/// kFirstFlickerSeconds into slots of 1 / (8 rate) seconds, each on (1) or off (1 - depth) by its seed and index, on
/// when the hash's unit value is below 0.6; FlickerMode::Pulse is 1 - depth (1 - cos(2 pi t rate / 10)) / 2 with t the
/// seconds after kFirstFlickerSeconds.
[[nodiscard]] float emissiveFlickerFactor(const EmissiveFlicker& flicker, float time) noexcept;

} // namespace rv::gl
