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

/// Writes the fields of `block` that `factors` and `emissiveFactor` decide.
void applyFactors(MaterialUniforms& block, const MaterialFactors& factors, const glm::vec3& emissiveFactor) noexcept {
    const assets::MaterialDesc defaults;
    for (int channel = 0; channel < 4; ++channel) {
        block.baseColorFactor[channel] = nonNegative(factors.baseColor[channel], defaults.baseColorFactor[channel]);
    }
    block.baseColorFactor.a = std::min(block.baseColorFactor.a, 1.0f);
    const float emissiveStrength = nonNegative(factors.emissiveStrength, defaults.emissiveStrength);
    for (int channel = 0; channel < 3; ++channel) {
        block.emissive[channel] =
            nonNegative(emissiveFactor[channel], defaults.emissiveFactor[channel]) * emissiveStrength;
    }
    block.metallic = unitInterval(factors.metallic, defaults.metallicFactor);
    block.roughness = unitInterval(factors.roughness, defaults.roughnessFactor);
    block.emissiveStrength = emissiveStrength;
}

} // namespace

scene::ShaderKind shaderKindOf(const assets::MaterialDesc& material) noexcept {
    return material.shading == assets::ShadingModel::Unlit ? scene::ShaderKind::Unlit : scene::ShaderKind::Pbr;
}

MaterialUniforms makeMaterialUniforms(const assets::MaterialDesc& material, std::uint32_t textureFlags) noexcept {
    const assets::MaterialDesc defaults;
    MaterialUniforms block;
    applyFactors(block, materialFactors(material), material.emissiveFactor);
    block.normalScale = finiteOr(material.normalScale, defaults.normalScale);
    block.occlusionStrength = unitInterval(material.occlusionStrength, defaults.occlusionStrength);
    block.alphaCutoff = unitInterval(material.alphaCutoff, defaults.alphaCutoff);

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
    if (material.crt) {
        flags |= material_flags::kCrtScreen;
        const assets::CrtEffect crtDefaults;
        const assets::CrtEffect& crt = *material.crt;
        block.crtLineCount = std::clamp(finiteOr(crt.lineCount, crtDefaults.lineCount), 1.0f, 4096.0f);
        block.crtMaskTriadCount =
            std::clamp(finiteOr(crt.maskTriadCount, crtDefaults.maskTriadCount), 1.0f, 8192.0f);
        block.crtScanlineStrength = unitInterval(crt.scanlineStrength, crtDefaults.scanlineStrength);
        block.crtMaskStrength = unitInterval(crt.maskStrength, crtDefaults.maskStrength);
        block.crtCurvature = std::clamp(finiteOr(crt.curvature, crtDefaults.curvature), 0.0f, 0.5f);
        block.crtVignette = unitInterval(crt.vignette, crtDefaults.vignette);
        block.crtGlassReflection = unitInterval(crt.glassReflection, crtDefaults.glassReflection);
    }
    block.flags = flags;
    return block;
}

MaterialFactors materialFactors(const assets::MaterialDesc& material) noexcept {
    return MaterialFactors{material.baseColorFactor, material.metallicFactor, material.roughnessFactor,
                           material.emissiveStrength};
}

MaterialUniforms withFactors(const MaterialUniforms& uniforms, const MaterialFactors& factors,
                             const glm::vec3& emissiveFactor) noexcept {
    MaterialUniforms block = uniforms;
    applyFactors(block, factors, emissiveFactor);
    return block;
}

} // namespace rv::gl
