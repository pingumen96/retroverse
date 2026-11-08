// Upholstery surfaces of the catalog (surface_textures.hpp, I12 step 1): a plain weave for the fabric seats and a
// pebbled leather for the leather ones. Both are detail surfaces around 1, so that one map serves every fabric's or
// leather's own color and roughness; both cover 50 cm, the scale of the seats' fabric uvs
// (geometry's kFabricMetersPerTile). Original procedural designs.

#include "procedural_noise.hpp"
#include "surface_canvas.hpp"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace rv::assets::detail {
namespace {

constexpr std::uint32_t kFabricSeed = 0xFAB1C0DEu;
constexpr float kPi = 3.14159265358979f;

int positiveMod(int value, int modulus) noexcept {
    const int remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

/// Distances, in cell units, from (u, v) in [0, 1) to the nearest and the second nearest of a tileable cellular
/// pattern's points: `cells` x `cells` cells over the unit square, one jittered point per cell.
std::pair<float, float> cellular(float u, float v, int cells, std::uint32_t seed) {
    const float x = u * static_cast<float>(cells);
    const float y = v * static_cast<float>(cells);
    const int cellX = floorToInt(x);
    const int cellY = floorToInt(y);
    float nearest = 9.0f;
    float second = 9.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = cellX + dx;
            const int ny = cellY + dy;
            const int wrappedX = positiveMod(nx, cells);
            const int wrappedY = positiveMod(ny, cells);
            const float px = static_cast<float>(nx) + 0.1f + 0.8f * hashUnit(wrappedX, wrappedY, seed);
            const float py = static_cast<float>(ny) + 0.1f + 0.8f * hashUnit(wrappedX, wrappedY, subSeed(seed, 1));
            const float distance = std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
            if (distance < nearest) {
                second = nearest;
                nearest = distance;
            } else if (distance < second) {
                second = distance;
            }
        }
    }
    return {nearest, second};
}

} // namespace

ProceduralSurface makeFabricWeave(const ProceduralSurfaceInfo& info) {
    // A plain weave seen up close: warp threads along v and weft threads along u, 1.56 mm apart, each passing over the
    // thread it crosses and under the next one. Every thread is a rounded strand, darker where it dips into the gaps,
    // a little lighter or darker than its neighbours, with slubs along it and a fuzz of fibres. The base color stays
    // close to 1 (its mean about 0.9) and the roughness near 1, so that they modulate the fabric's own.
    constexpr int kThreads = 320;
    constexpr float kStrandHeight = 0.00012f; // meters
    const std::uint32_t seed = subSeed(kFabricSeed, 1);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = true;
    desc.samplesPerAxis = 2;
    return renderSurface(desc, [&](float u, float v) {
        const float threadsU = u * static_cast<float>(kThreads);
        const float threadsV = v * static_cast<float>(kThreads);
        const int column = std::min(floorToInt(threadsU), kThreads - 1); // the warp thread
        const int row = std::min(floorToInt(threadsV), kThreads - 1);    // the weft thread
        const float acrossWarp = threadsU - static_cast<float>(column);
        const float acrossWeft = threadsV - static_cast<float>(row);
        const bool warpOver = ((column + row) & 1) == 0;
        // Each strand's rounded section across it, and its rise over the thread it crosses (it sinks under the next).
        const float warpSection = std::sin(kPi * acrossWarp);
        const float weftSection = std::sin(kPi * acrossWeft);
        const float warpRise = 0.5f + 0.5f * (warpOver ? 1.0f : -1.0f) * std::sin(kPi * acrossWeft);
        const float weftRise = 0.5f + 0.5f * (warpOver ? -1.0f : 1.0f) * std::sin(kPi * acrossWarp);
        const float warpTop = std::sqrt(warpSection) * (0.35f + 0.65f * warpRise);
        const float weftTop = std::sqrt(weftSection) * (0.35f + 0.65f * weftRise);
        const bool warpSeen = warpTop >= weftTop;
        const float top = std::max(warpTop, weftTop);

        // Threads differ a little in tone; slubs swell and lighten along each one (tiling: whole cells per image).
        const float threadTone = warpSeen ? hashUnit(column, 0, seed) : hashUnit(row, 1, seed);
        const float slub = warpSeen ? valueNoise(v * 40.0f, static_cast<float>(column), 40, kThreads, subSeed(seed, 2))
                                    : valueNoise(u * 40.0f, static_cast<float>(row), 40, kThreads, subSeed(seed, 3));
        const float fuzz = valueNoise(u * 256.0f, v * 256.0f, 256, 256, subSeed(seed, 4));
        const float heather = fbm(u, v, 6, 6, 3, 0.5f, subSeed(seed, 5));

        SurfaceSample sample;
        const float shade = (0.78f + 0.22f * top) * (0.96f + 0.06f * threadTone) * (0.98f + 0.04f * slub) *
                            (0.985f + 0.03f * fuzz) * (1.0f + 0.025f * heather);
        sample.albedo = glm::vec3(std::min(shade, 1.0f));
        sample.roughness = std::clamp(0.96f + 0.04f * fuzz - 0.03f * top * slub, 0.0f, 1.0f);
        sample.metallic = 0.0f;
        sample.height = kStrandHeight * top + 0.00001f * fuzz;
        return sample;
    });
}

ProceduralSurface makeLeather(const ProceduralSurfaceInfo& info) {
    // Upholstery leather: a pebbled grain of small rounded cells (about 3.3 mm) parted by fine creases, a few longer
    // wrinkles along the contours of a slow noise, a soft mottle; darker in the creases and wrinkles, a little
    // polished on the pebbles' tops. The base color stays close to 1 and the roughness under it, so that they modulate
    // the leather's own color and roughness.
    constexpr int kPebbles = 150;
    const std::uint32_t seed = subSeed(kFabricSeed, 2);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = true;
    desc.samplesPerAxis = 2;
    return renderSurface(desc, [&](float u, float v) {
        const auto [nearest, second] = cellular(u, v, kPebbles, subSeed(seed, 0));
        const float crease = 1.0f - smoothStep(0.0f, 0.2f, second - nearest);
        const float dome = 1.0f - smoothStep(0.0f, 0.8f, nearest);
        const float slow = fbm(u, v, 5, 5, 3, 0.55f, subSeed(seed, 1));
        const float wrinkle = 1.0f - smoothStep(0.0f, 0.03f, std::abs(slow));
        const float mottle = fbm(u, v, 9, 9, 3, 0.5f, subSeed(seed, 2));

        SurfaceSample sample;
        const float shade = (1.0f - 0.2f * crease) * (1.0f - 0.06f * wrinkle) * (0.96f + 0.04f * mottle);
        sample.albedo = glm::vec3(std::clamp(shade, 0.0f, 1.0f));
        sample.roughness = std::clamp(0.94f - 0.12f * dome + 0.06f * crease + 0.03f * mottle, 0.0f, 1.0f);
        sample.metallic = 0.0f;
        sample.height = 0.00005f * dome - 0.00004f * crease - 0.00006f * wrinkle;
        return sample;
    });
}

} // namespace rv::assets::detail
