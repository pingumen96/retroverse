#include <rv/render/material_uniforms.hpp>

#include <algorithm>
#include <cmath>

namespace rv::gl {
namespace {

constexpr std::uint32_t kAllTextureBits =
    material_flags::kBaseColorTexture | material_flags::kMetallicRoughnessTexture | material_flags::kNormalTexture |
    material_flags::kOcclusionTexture | material_flags::kEmissiveTexture;

float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

float unitInterval(float value, float fallback) noexcept {
    return std::clamp(finiteOr(value, fallback), 0.0f, 1.0f);
}

float nonNegative(float value, float fallback) noexcept {
    return std::max(finiteOr(value, fallback), 0.0f);
}

} // namespace

scene::ShaderKind shaderKindOf(const assets::MaterialDesc& material) noexcept {
    return material.shading == assets::ShadingModel::Unlit ? scene::ShaderKind::Unlit : scene::ShaderKind::Pbr;
}

MaterialUniforms makeMaterialUniforms(const assets::MaterialDesc& material, std::uint32_t textureFlags) noexcept {
    const assets::MaterialDesc defaults;
    MaterialUniforms block;
    for (int channel = 0; channel < 4; ++channel) {
        block.baseColorFactor[channel] =
            nonNegative(material.baseColorFactor[channel], defaults.baseColorFactor[channel]);
    }
    block.baseColorFactor.a = std::min(block.baseColorFactor.a, 1.0f);
    const float emissiveStrength = nonNegative(material.emissiveStrength, defaults.emissiveStrength);
    for (int channel = 0; channel < 3; ++channel) {
        block.emissive[channel] =
            nonNegative(material.emissiveFactor[channel], defaults.emissiveFactor[channel]) * emissiveStrength;
    }
    block.metallic = unitInterval(material.metallicFactor, defaults.metallicFactor);
    block.roughness = unitInterval(material.roughnessFactor, defaults.roughnessFactor);
    block.normalScale = finiteOr(material.normalScale, defaults.normalScale);
    block.occlusionStrength = unitInterval(material.occlusionStrength, defaults.occlusionStrength);
    block.alphaCutoff = unitInterval(material.alphaCutoff, defaults.alphaCutoff);
    block.emissiveStrength = emissiveStrength;

    std::uint32_t flags = textureFlags & kAllTextureBits;
    switch (material.alphaMode) {
    case assets::AlphaMode::Opaque:
        break;
    case assets::AlphaMode::Mask:
        flags |= material_flags::kAlphaMask;
        break;
    case assets::AlphaMode::Blend:
        flags |= material_flags::kAlphaBlend;
        break;
    }
    if (material.doubleSided) {
        flags |= material_flags::kDoubleSided;
    }
    block.flags = flags;
    return block;
}

} // namespace rv::gl
