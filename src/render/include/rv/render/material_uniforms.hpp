#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstdint>

#include <rv/assets/material_desc.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

/// Shader family that draws `material`: ShaderKind::Unlit for assets::ShadingModel::Unlit (screens, neon, the magenta
/// fallback: base color * texture * emissiveStrength, tonemapped like lit surfaces), ShaderKind::Pbr otherwise.
[[nodiscard]] scene::ShaderKind shaderKindOf(const assets::MaterialDesc& material) noexcept;

/// The MaterialUniforms block of `material`. Factors are copied, emissive = emissiveFactor * emissiveStrength, the
/// alpha mode and doubleSided become material_flags bits, and `textureFlags` adds the texture bits of the slots that
/// hold one of the material's textures (non-texture bits of `textureFlags` are ignored). Values the shaders cannot
/// use never reach them: non-finite numbers take the MaterialDesc defaults, metallic, roughness, occlusion strength
/// and alpha cutoff are clamped to [0, 1], colors and emissive strength to >= 0.
[[nodiscard]] MaterialUniforms makeMaterialUniforms(const assets::MaterialDesc& material,
                                                    std::uint32_t textureFlags) noexcept;

} // namespace rv::gl
