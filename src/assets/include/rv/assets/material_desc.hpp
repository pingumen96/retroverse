#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

namespace rv::assets {

enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };
enum class ShadingModel : std::uint8_t { Pbr, Unlit };

/// Reference to a texture: either an image embedded in/loaded with a model (`imageIndex` into
/// ModelData::images) or an asset-relative file path. Empty when neither is set.
struct TextureRef {
    std::optional<std::uint32_t> imageIndex;
    std::string path;
    std::uint32_t texCoord = 0;

    [[nodiscard]] bool empty() const noexcept { return !imageIndex.has_value() && path.empty(); }
};

/// Renderer-agnostic material description (glTF 2.0 metallic-roughness model, KHR_materials_emissive_strength).
/// All color factors are LINEAR (glTF convention); hex art-direction colors are converted on load.
struct MaterialDesc {
    std::string name;
    ShadingModel shading = ShadingModel::Pbr;
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 0.5f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    glm::vec3 emissiveFactor{0.0f};
    float emissiveStrength = 1.0f;
    TextureRef baseColorTexture;         ///< sRGB
    TextureRef metallicRoughnessTexture; ///< linear, G = roughness, B = metallic
    TextureRef normalTexture;            ///< linear, tangent space
    TextureRef occlusionTexture;         ///< linear, R channel
    TextureRef emissiveTexture;          ///< sRGB
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

} // namespace rv::assets
