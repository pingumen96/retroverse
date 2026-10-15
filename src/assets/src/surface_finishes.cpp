// Finishes of the catalog (surface_textures.hpp): painted plaster walls, brushed metal and the micro-variation of aged
// plastic. Original procedural designs.

#include <algorithm>
#include <cstdint>

#include <glm/common.hpp>

#include "procedural_noise.hpp"
#include "surface_canvas.hpp"

namespace rv::assets::detail {
namespace {

// Palette of style §3 (sRGB).
constexpr std::uint32_t kWallHex = 0x383735;
constexpr std::uint32_t kBrushedMetalHex = 0xB9BBBE;

constexpr std::uint32_t kFinishSeed = 0x5EED0F1Bu;

} // namespace

ProceduralSurface makePaintedPlaster(const ProceduralSurfaceInfo& info) {
    // Smoky grey-brown paint over hand-troweled plaster: soft, low-frequency clouds of tone and sheen, a faint warm and
    // cool drift, a fine sand grain, and a gentle undulation the grazing key light picks up.
    const glm::vec3 base = linearFromHex(kWallHex);
    const std::uint32_t seed = subSeed(kFinishSeed, 1);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = true;
    return renderSurface(desc, [&](float u, float v) {
        const float clouds = fbm(u, v, 3, 3, 4, 0.55f, subSeed(seed, 0));
        const float drift = fbm(u, v, 2, 2, 2, 0.5f, subSeed(seed, 1));
        const float sand = 2.0f * valueNoise(u * 192.0f, v * 192.0f, 192, 192, subSeed(seed, 2)) - 1.0f;
        const float trowel = fbm(u, v, 5, 4, 3, 0.5f, subSeed(seed, 3));
        const float strokes = fbm(u, v, 20, 12, 2, 0.5f, subSeed(seed, 4));
        SurfaceSample sample;
        sample.albedo = base * (1.0f + 0.12f * clouds + 0.04f * sand) *
                        glm::vec3(1.0f + 0.04f * drift, 1.0f, 1.0f - 0.04f * drift);
        // Matte paint; the trowel's smoother passes are a little less rough.
        sample.roughness = std::clamp(0.88f + 0.04f * clouds - 0.06f * smoothStep(0.2f, 0.7f, trowel), 0.0f, 1.0f);
        sample.metallic = 0.0f;
        sample.height = 0.001f * trowel + 0.0004f * strokes + 0.00008f * sand;
        return sample;
    });
}

ProceduralSurface makeBrushedMetal(const ProceduralSurfaceInfo& info) {
    // Satin steel brushed along u: long streaks of brightness and sheen, fine scratches across the brushing direction.
    const glm::vec3 base = linearFromHex(kBrushedMetalHex);
    const std::uint32_t seed = subSeed(kFinishSeed, 2);
    const auto cellsAcross = static_cast<std::int32_t>(info.size.y / 2);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = true;
    return renderSurface(desc, [&](float u, float v) {
        const float streak = 2.0f * valueNoise(u * 6.0f, v * 160.0f, 6, 160, subSeed(seed, 0)) - 1.0f;
        const float scratch =
            2.0f * valueNoise(u * 12.0f, v * static_cast<float>(cellsAcross), 12, cellsAcross, subSeed(seed, 1)) -
            1.0f;
        const float smudge = fbm(u, v, 3, 3, 3, 0.5f, subSeed(seed, 2));
        SurfaceSample sample;
        sample.albedo = base * (1.0f + 0.035f * streak + 0.02f * scratch);
        sample.roughness = std::clamp(0.3f + 0.05f * streak + 0.04f * scratch + 0.05f * smudge, 0.0f, 1.0f);
        sample.metallic = 1.0f;
        sample.height = 0.000012f * scratch + 0.000008f * streak;
        return sample;
    });
}

ProceduralSurface makeAgedPlastic(const ProceduralSurfaceInfo& info) {
    // A detail surface around 1: blotches of yellowing, fine speckle, smoother handled patches and an orange-peel
    // micro-relief, all subtle. The material keeps its own color and roughness, which these maps modulate.
    const std::uint32_t seed = subSeed(kFinishSeed, 3);
    const glm::vec3 yellowed(1.0f, 0.972f, 0.91f);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = true;
    return renderSurface(desc, [&](float u, float v) {
        const float ageing = smoothStep(-0.1f, 0.6f, fbm(u, v, 3, 3, 3, 0.55f, subSeed(seed, 0)));
        const float speckle = valueNoise(u * 160.0f, v * 160.0f, 160, 160, subSeed(seed, 1));
        const float handled = smoothStep(0.1f, 0.6f, fbm(u, v, 5, 5, 3, 0.5f, subSeed(seed, 2)));
        const float peel = fbm(u, v, 48, 48, 2, 0.5f, subSeed(seed, 3));
        SurfaceSample sample;
        sample.albedo = glm::mix(glm::vec3(1.0f), yellowed, 0.7f * ageing) * (1.0f - 0.035f * speckle);
        sample.roughness = 1.0f - 0.12f * handled - 0.04f * speckle;
        sample.metallic = 0.0f;
        sample.height = 0.000008f * peel;
        return sample;
    });
}

} // namespace rv::assets::detail
