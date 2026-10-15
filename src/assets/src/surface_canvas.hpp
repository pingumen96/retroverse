#pragma once

// Internal: turns a per-point description of a procedural surface into its maps (surface_textures.hpp). Not installed.

#include <cstdint>
#include <functional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/assets/surface_textures.hpp>

namespace rv::assets::detail {

/// The surface at one point: linear albedo, roughness and metallic in [0, 1], and the height of the surface along its
/// normal in meters (relief for the normal map).
struct SurfaceSample {
    glm::vec3 albedo{0.5f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    float height = 0.0f;
};

/// How renderSurface() samples and encodes a surface.
struct CanvasDesc {
    glm::ivec2 size{0};
    /// Meters covered by the image along u and v: the size of a pixel for the slopes of the height.
    glm::vec2 meters{1.0f};
    /// The image repeats: the normal map's differences wrap around its edges (otherwise they are clamped).
    bool tiles = true;
    /// Samples per pixel along each axis (1: the pixel center; n: an n x n grid averaged, against aliasing of hard
    /// edges such as the motifs of a rug).
    int samplesPerAxis = 1;
    /// Builds a normal map from the heights.
    bool normalMap = true;
    /// Scales the slopes of the height in the normal map (1: as modeled).
    float normalStrength = 1.0f;
};

/// Surface description: the sample at (u, v), u along the image's width and v down its height, both in [0, 1).
using SurfaceFunction = std::function<SurfaceSample(float u, float v)>;

/// Renders the maps of a surface: every pixel averages `desc.samplesPerAxis`^2 samples of `surface` (albedo averaged
/// in linear space, then sRGB-encoded); the normal map comes from central differences of the averaged heights. Rows
/// are rendered in parallel, so `surface` must be a pure function of (u, v).
[[nodiscard]] ProceduralSurface renderSurface(const CanvasDesc& desc, const SurfaceFunction& surface);

/// The palette color `hex` (sRGB 0xRRGGBB, style §3) in linear RGB.
[[nodiscard]] glm::vec3 linearFromHex(std::uint32_t hex) noexcept;

/// Generators of the catalog's surfaces (surface_wood.cpp, surface_rug.cpp, surface_finishes.cpp), each at the size
/// of its catalog entry.
[[nodiscard]] ProceduralSurface makeOakPlanks(const ProceduralSurfaceInfo& info);
[[nodiscard]] ProceduralSurface makeWalnut(const ProceduralSurfaceInfo& info);
[[nodiscard]] ProceduralSurface makeVarnishedWood(const ProceduralSurfaceInfo& info);
[[nodiscard]] ProceduralSurface makeOak(const ProceduralSurfaceInfo& info);
[[nodiscard]] ProceduralSurface makePaintedPlaster(const ProceduralSurfaceInfo& info);
[[nodiscard]] ProceduralSurface makeOrnateRug(const ProceduralSurfaceInfo& info);
[[nodiscard]] ProceduralSurface makeBrushedMetal(const ProceduralSurfaceInfo& info);
[[nodiscard]] ProceduralSurface makeAgedPlastic(const ProceduralSurfaceInfo& info);

} // namespace rv::assets::detail
