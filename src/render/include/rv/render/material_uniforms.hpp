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
/// hold one of the material's textures (non-texture bits of `textureFlags` are ignored). Values the shaders cannot
/// use never reach them: non-finite numbers take the MaterialDesc defaults, metallic, roughness, occlusion strength
/// and alpha cutoff are clamped to [0, 1], colors and emissive strength to >= 0.
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

} // namespace rv::gl
