#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

// Procedural surface textures (style §3.2 material library, §3.3 moderate ageing): the PBR maps of the shared
// materials, generated at startup by pure, deterministic functions, so that Retroverse needs no texture download.
// Every design is original (wood figure, plaster, an ornamental rug in the style §3 palette and I6 step 3b's rugs of
// the packs, brushed metal, plastic micro-variation, a sconce's wash of light on its wall). A surface's maps are
// generated together and share their layout (the planks of a floor, the grain of a board), and every surface except
// the rugs, the wall wash and the shelf glow (pictures) tiles seamlessly. Every base color is opaque but the wall wash's, whose alpha is the picture.
//
// Maps follow glTF 2.0: the base color is sRGB; the metallic-roughness map is linear with roughness in G and
// metallic in B (R and A are 255 and ignored); the normal map is linear, tangent space, +X along +u and +Y towards
// the top of the image, the "+Y up" of docs/decisions/ADR-0002. Images are RGBA8, rows top to bottom.
//
// Materials and scene files name a single map as "<surface>/<map>", e.g. "oak_planks/baseColor" (materials.json
// writes it "procedural:oak_planks/baseColor" in a texture slot). How a map combines with the material's factors is
// part of each surface's description: "full" surfaces carry the albedo and roughness themselves (use them with a
// white base color and a roughness factor of 1), "detail" surfaces modulate the material's own factors around 1.

/// The maps of a procedural surface.
enum class SurfaceMap : std::uint8_t {
    BaseColor,         ///< sRGB albedo, alpha 255 (the wall wash's alpha its picture)
    MetallicRoughness, ///< linear: G roughness, B metallic
    Normal,            ///< linear tangent-space normal, encoded as n * 0.5 + 0.5
};
inline constexpr std::size_t kSurfaceMapCount = 3;

/// "baseColor", "metallicRoughness", "normal".
[[nodiscard]] std::string_view toString(SurfaceMap map) noexcept;
/// The map named by toString(), or nothing.
[[nodiscard]] std::optional<SurfaceMap> surfaceMapFromString(std::string_view name) noexcept;

/// A procedural surface of the catalog.
struct ProceduralSurfaceInfo {
    std::string_view name;
    /// Pixel size of every map of the surface.
    glm::ivec2 size{0};
    /// Meters covered by the whole image along u and v: one repeat of a tiling surface, or the object a picture is
    /// designed for (the rug).
    glm::vec2 meters{0.0f};
    /// Whether the image repeats seamlessly (every surface but the rug and the wall wash, pictures).
    bool tiles = true;
    bool hasNormalMap = true;
    /// What the surface is for, how its uvs are expected (meters per uv unit) and how its maps combine with the
    /// material factors.
    std::string_view description;
};

/// The catalog, in a fixed order: oak_planks, walnut, varnished_wood, oak, painted_plaster, ornate_rug,
/// ornate_rug_blue, ornate_rug_sage, ornate_rug_green, geometric_rug, runner_rug (I6 step 3b), brushed_metal,
/// aged_plastic, wallwash, shelfglow.
[[nodiscard]] std::span<const ProceduralSurfaceInfo> proceduralSurfaces() noexcept;
/// The surface called `name`, or nullptr.
[[nodiscard]] const ProceduralSurfaceInfo* findProceduralSurface(std::string_view name) noexcept;

/// The maps of one surface.
struct ProceduralSurface {
    ImageData baseColor;
    ImageData metallicRoughness;
    /// Empty (invalid) when the surface has no normal map.
    ImageData normal;

    [[nodiscard]] const ImageData& map(SurfaceMap which) const noexcept;
};

/// Generates every map of the surface `name` at its catalog size. The same name gives the same bytes on every run
/// (integer hashes and plain float arithmetic, no global state); rows are generated in parallel. Errors: NotFound for a
/// name outside the catalog.
[[nodiscard]] Expected<ProceduralSurface> makeProceduralSurface(std::string_view name);

/// A procedural texture: one map of one surface.
struct ProceduralTextureName {
    std::string surface;
    SurfaceMap map = SurfaceMap::BaseColor;

    /// "<surface>/<map>", the spelling parseProceduralTextureName() reads.
    [[nodiscard]] std::string toString() const;
    bool operator==(const ProceduralTextureName&) const = default;
};

/// Prefix of procedural textures in the texture slots of material files ("procedural:oak_planks/baseColor").
inline constexpr std::string_view kProceduralTexturePrefix = "procedural:";

/// Parses "<surface>/<map>". Errors: InvalidArgument when the text is not of that form, NotFound for a surface
/// outside the catalog or a surface without the map (the message lists the valid names).
[[nodiscard]] Expected<ProceduralTextureName> parseProceduralTextureName(std::string_view text);

} // namespace rv::assets
