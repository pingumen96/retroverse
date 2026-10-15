#pragma once

#include <rv/core/expected.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/uniform_blocks.hpp>

namespace rv::gl {

/// Built-in textures: the neutral value of every material texture slot, bound where a material has no texture of its
/// own so that every sampler reads a complete texture, and the checker that stands in for missing or broken textures
/// (style §21: visible, never a crash). Main thread only.
class TextureDefaults {
public:
    /// Creates the four textures; `anisotropy` (already clamped to the device limit, see clampAnisotropy) applies to
    /// the checker.
    [[nodiscard]] static Expected<TextureDefaults> create(float anisotropy);

    /// 1x1 opaque white (255, 255, 255, 255), linear: 1.0 in every channel whether read as color or data.
    [[nodiscard]] const Texture2D& white() const noexcept { return m_white; }
    /// 1x1 opaque black (0, 0, 0, 255), linear.
    [[nodiscard]] const Texture2D& black() const noexcept { return m_black; }
    /// 1x1 (128, 128, 255, 255), linear: the tangent-space normal +Z (no perturbation).
    [[nodiscard]] const Texture2D& flatNormal() const noexcept { return m_flatNormal; }
    /// 64x64 sRGB "checker" pattern (assets::makeNamedPattern: 8 x 8 ivory and wall-grey cells), repeating, mipmapped.
    [[nodiscard]] const Texture2D& checker() const noexcept { return m_checker; }

    /// Neutral texture of a material slot: white for base color, metallic-roughness and occlusion (the factors apply
    /// unchanged), the flat normal for normal maps, black for emissive.
    [[nodiscard]] const Texture2D& neutral(MaterialTextureSlot slot) const noexcept;

private:
    Texture2D m_white;
    Texture2D m_black;
    Texture2D m_flatNormal;
    Texture2D m_checker;
};

} // namespace rv::gl
