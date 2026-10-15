#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

namespace rv::assets {

enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };
enum class ShadingModel : std::uint8_t { Pbr, Unlit };

/// Reference to a texture: an image embedded in/loaded with a model (`imageIndex` into ModelData::images), an
/// asset-relative file path, or a procedural texture of the surface catalog (`procedural`, "<surface>/<map>", see
/// rv/assets/surface_textures.hpp). At most one is set; empty when none is.
struct TextureRef {
    std::optional<std::uint32_t> imageIndex;
    std::string path;
    std::string procedural;
    std::uint32_t texCoord = 0;

    [[nodiscard]] bool empty() const noexcept {
        return !imageIndex.has_value() && path.empty() && procedural.empty();
    }
};

/// The CRT_Subtle screen effect (style §15.2, §25) of an unlit screen material: its picture is shown the way a picture
/// tube shows it, on the emissive path, without hurting legibility. Scanlines and the phosphor mask keep the mean
/// brightness and fade out as they shrink below a few pixels (no moiré at a distance), so the picture stays readable
/// from any distance. The material parser enforces the ranges below.
struct CrtEffect {
    /// Scanlines over the picture's height, [16, 2048] (240: the picture of a 16-bit console).
    float lineCount = 240.0f;
    /// [0, 1]: depth of the dark gaps between scanlines.
    float scanlineStrength = 0.3f;
    /// RGB phosphor triads across the picture's width (the aperture grille), [16, 4096].
    float maskTriadCount = 320.0f;
    /// [0, 1]: how strongly each triad separates red, green and blue.
    float maskStrength = 0.2f;
    /// [0, 0.5]: barrel curvature: the picture bows out and its corners round off into the tube's dark border.
    float curvature = 0.05f;
    /// [0, 1]: darkening of the picture towards its edges.
    float vignette = 0.25f;
    /// [0, 1]: share of the scene's lights and ambient the screen's glass reflects (a dielectric's reflectance).
    float glassReflection = 1.0f;

    bool operator==(const CrtEffect&) const = default;
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
    /// The picture-tube look of an unlit screen material; none for every other material.
    std::optional<CrtEffect> crt;
};

} // namespace rv::assets
