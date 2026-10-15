// Wood surfaces of the catalog (surface_textures.hpp): oak floor planks and the grain of furniture boards (walnut,
// varnished wood, oak). Original procedural designs. Each board is a flat slice through a log: its growth rings are
// circles around the pith, so the slice shows them as lines at r = sqrt(h^2 + x^2) (h the distance of the face from
// the pith, x the offset across the board): nearly straight far from the pith line, closing into the arches of
// flat-sawn boards ("cathedrals") near it, where the rings drift slowly along the board. Pores, long streaks and a tint
// per board complete the look.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/common.hpp>

#include "procedural_noise.hpp"
#include "surface_canvas.hpp"

namespace rv::assets::detail {
namespace {

// Palette of style §3 (sRGB): the means the woods are tuned around.
constexpr std::uint32_t kWalnutHex = 0x493024;
constexpr std::uint32_t kOakHex = 0x76513A;
constexpr std::uint32_t kVarnishedHex = 0x5B3A26;

/// Seed of the catalog's woods; each surface derives its own.
constexpr std::uint32_t kWoodSeed = 0x0A7C0DE5u;
/// Noise across a board never repeats within it: its lattice period is far larger than any board.
constexpr std::int32_t kAcrossPeriod = 1 << 20;

/// Look of a wood species and its finish.
struct WoodStyle {
    glm::vec3 base{0.1f};      ///< mean albedo, linear
    glm::vec3 earlyTint{1.1f}; ///< multiplier of the light early wood
    glm::vec3 lateTint{0.6f};  ///< multiplier of the dark late wood (the ring lines)
    float ringSpacing = 0.006f;    ///< meters between growth rings
    /// Distance of a board's face from the pith, meters, picked per board between these: close to the pith the rings
    /// close into arches, far from it they run nearly straight (quarter-sawn look).
    float pithDistanceMin = 0.01f;
    float pithDistanceMax = 0.12f;
    float drift = 0.02f;           ///< meters the rings drift across the board along its length (the arches)
    float irregularity = 0.8f;     ///< variation of the ring widths, in ring spacings
    float streakContrast = 0.1f;   ///< long lighter and darker streaks along the grain
    float poreThreshold = 0.72f;   ///< higher: fewer pores
    float poreDarkening = 0.35f;
    float roughness = 0.5f;        ///< the finish
    float roughnessVariation = 0.04f;
    float poreRoughness = 0.15f;   ///< added in the pores
    float poreDepth = 0.00008f;    ///< meters
    float ringRelief = 0.00002f;   ///< meters: late wood stands proud by this much
};

/// One board: where its pith lies and its own seed.
struct Board {
    float pithOffset = 0.0f;   ///< meters across, from the board's center line
    float pithDistance = 0.05f; ///< meters below the face
    std::uint32_t seed = 0;
};

Board boardFrom(const WoodStyle& style, float width, std::uint32_t seed) {
    Board board;
    board.seed = seed;
    board.pithOffset = (hashUnit(0, 1, seed) - 0.5f) * 1.2f * width;
    const float closeness = hashUnit(0, 2, seed);
    board.pithDistance =
        style.pithDistanceMin + (style.pithDistanceMax - style.pithDistanceMin) * closeness * closeness;
    return board;
}

/// Value noise over meters with about the given cell density, repeating every `alongRepeat` meters along the grain
/// (whole cells per repeat) and never across it.
float grainNoise(float along, float across, float cellsAlong, float cellsAcross, float alongRepeat,
                 std::uint32_t seed) {
    const auto period = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(alongRepeat * cellsAlong)));
    return valueNoise(along / alongRepeat * static_cast<float>(period), across * cellsAcross + 4096.0f, period,
                      kAcrossPeriod, seed);
}

/// Two octaves of grainNoise, in [-1, 1].
float grainFbm(float along, float across, float cellsAlong, float cellsAcross, float alongRepeat, std::uint32_t seed) {
    const float first = grainNoise(along, across, cellsAlong, cellsAcross, alongRepeat, subSeed(seed, 0));
    const float second =
        grainNoise(along, across, 2.0f * cellsAlong, 2.0f * cellsAcross, alongRepeat, subSeed(seed, 1));
    return (2.0f * first - 1.0f) * 0.67f + (2.0f * second - 1.0f) * 0.33f;
}

/// Wood at `along` meters along the grain and `across` meters from the center line of `board`; the pattern repeats
/// every `alongRepeat` meters along the grain.
SurfaceSample woodAt(const WoodStyle& style, const Board& board, float along, float across, float alongRepeat) {
    const std::uint32_t seed = board.seed;
    // Growth rings around the pith, drifting slowly along the board and wobbling a little.
    const float x = across - board.pithOffset;
    const float radius = std::sqrt(board.pithDistance * board.pithDistance + x * x);
    const float drift = style.drift * grainFbm(along, across, 1.5f, 0.0f, alongRepeat, subSeed(seed, 10));
    const float wobble =
        0.3f * style.ringSpacing * grainFbm(along, across, 5.0f, 25.0f, alongRepeat, subSeed(seed, 11));
    float phase = (radius + drift + wobble) / style.ringSpacing;
    // Uneven ring widths: the phase is stretched by a slow function of itself, the same all around the pith.
    phase += style.irregularity * (2.0f * valueNoise(phase / 6.0f, 0.5f, kAcrossPeriod, 1, subSeed(seed, 12)) - 1.0f);
    const float t = fract(phase);
    // Light early wood rising into the dark late wood, a sharp boundary to the next ring; contrast varies per ring.
    const float contrast = 0.35f + 0.65f * hashUnit(floorToInt(phase), 0, subSeed(seed, 13));
    const float late = contrast * (t < 0.88f ? smoothStep(0.35f, 0.88f, t) : 1.0f - smoothStep(0.88f, 1.0f, t));

    // Long streaks of lighter and darker timber, short dark pores, slow tone and sheen variation.
    const float streak = grainFbm(along, across, 0.6f, 25.0f, alongRepeat, subSeed(seed, 14));
    const float poreNoise = grainNoise(along, across, 40.0f, 700.0f, alongRepeat, subSeed(seed, 15));
    const float pore = smoothStep(style.poreThreshold, style.poreThreshold + 0.12f, poreNoise);
    const float tone = grainFbm(along, across, 2.0f, 4.0f, alongRepeat, subSeed(seed, 16));
    const float sheen = grainFbm(along, across, 3.0f, 6.0f, alongRepeat, subSeed(seed, 17));

    SurfaceSample sample;
    const glm::vec3 ringTint = glm::mix(style.earlyTint, style.lateTint, late);
    sample.albedo = style.base * ringTint * (1.0f + style.streakContrast * streak) * (1.0f + 0.05f * tone) *
                    (1.0f - style.poreDarkening * pore);
    sample.roughness = std::clamp(style.roughness + style.roughnessVariation * sheen + 0.03f * late +
                                      style.poreRoughness * pore,
                                  0.0f, 1.0f);
    sample.metallic = 0.0f;
    sample.height = style.ringRelief * late - style.poreDepth * pore;
    return sample;
}

// --- Floor planks ---------------------------------------------------------------------------------------------------

/// Planks across one repeat of the floor (u), each 2 m / 14 = 14.3 cm wide; they run along v.
constexpr int kPlankRows = 14;
/// Half the dark gap between two planks, the depth of the gap, and the micro-bevel along every plank edge (meters).
constexpr float kGapHalfWidth = 0.0007f;
constexpr float kGapDepth = 0.0015f;
constexpr float kBevelWidth = 0.0022f;
constexpr float kBevelDepth = 0.0006f;

/// One row of planks along v: one plank over the whole repeat, or two split at `split`; joints shifted by `phase`.
struct PlankRow {
    float phase = 0.0f;
    bool twoPlanks = false;
    float split = 1.0f;
};

std::array<PlankRow, kPlankRows> plankLayout(std::uint32_t seed) {
    std::array<PlankRow, kPlankRows> rows{};
    float phase = hashUnit(0, 0, seed);
    for (int row = 0; row < kPlankRows; ++row) {
        PlankRow& plank = rows[static_cast<std::size_t>(row)];
        // Staggered joints: each row's first joint lies 30 to 70% of the repeat after its neighbor's.
        phase = fract(phase + 0.3f + 0.4f * hashUnit(row, 1, seed));
        plank.phase = phase;
        plank.twoPlanks = hashUnit(row, 2, seed) < 0.7f;
        plank.split = plank.twoPlanks ? 0.36f + 0.28f * hashUnit(row, 3, seed) : 1.0f;
    }
    return rows;
}

} // namespace

ProceduralSurface makeOakPlanks(const ProceduralSurfaceInfo& info) {
    const glm::vec2 meters = info.meters;
    const float plankWidth = meters.x / static_cast<float>(kPlankRows);
    const float pixel = meters.x / static_cast<float>(info.size.x);
    const std::uint32_t seed = subSeed(kWoodSeed, 1);
    const std::array<PlankRow, kPlankRows> rows = plankLayout(seed);

    WoodStyle style;
    style.base = linearFromHex(kOakHex);
    style.earlyTint = glm::vec3(1.16f, 1.15f, 1.13f);
    style.lateTint = glm::vec3(0.6f, 0.56f, 0.54f);
    style.ringSpacing = 0.005f;
    style.pithDistanceMin = 0.008f;
    style.pithDistanceMax = 0.09f;
    style.drift = 0.018f;
    style.irregularity = 0.8f;
    style.streakContrast = 0.08f;
    style.poreThreshold = 0.74f;
    style.poreDarkening = 0.3f;
    // A satin floor varnish, a little worn.
    style.roughness = 0.4f;
    style.roughnessVariation = 0.04f;
    style.poreRoughness = 0.12f;

    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = meters;
    desc.tiles = true;
    return renderSurface(desc, [&](float u, float v) {
        const int row = std::min(floorToInt(u * static_cast<float>(kPlankRows)), kPlankRows - 1);
        const PlankRow& plank = rows[static_cast<std::size_t>(row)];
        const float acrossFraction = u * static_cast<float>(kPlankRows) - static_cast<float>(row);
        const float t = fract(v - plank.phase);
        const bool second = plank.twoPlanks && t >= plank.split;
        const float start = second ? plank.split : 0.0f;
        const float end = plank.twoPlanks && !second ? plank.split : 1.0f;
        const auto plankId = static_cast<std::uint32_t>(row * 2 + (second ? 1 : 0));
        const Board board = boardFrom(style, plankWidth, subSeed(seed, 100 + plankId));
        // A plank is never longer than the repeat: its grain need not repeat within it.
        const float along = (t - start) * meters.y + 7.0f * hashUnit(0, 3, board.seed);
        SurfaceSample sample = woodAt(style, board, along, (acrossFraction - 0.5f) * plankWidth, 1024.0f);

        // Per-plank tint: brightness, warmth, now and then a greyer, weathered board.
        const float brightness = 0.86f + 0.28f * hashUnit(1, 0, board.seed);
        const float warmth = 2.0f * hashUnit(2, 0, board.seed) - 1.0f;
        const glm::vec3 tint(1.0f + 0.05f * warmth, 1.0f + 0.01f * warmth, 1.0f - 0.07f * warmth);
        if (hashUnit(3, 0, board.seed) < 0.12f) {
            const float grey = (sample.albedo.r + sample.albedo.g + sample.albedo.b) / 3.0f;
            sample.albedo = glm::mix(sample.albedo, glm::vec3(grey), 0.25f);
        }
        sample.albedo *= brightness * tint;
        sample.roughness += 0.08f * (hashUnit(4, 0, board.seed) - 0.5f);
        // Slightly cupped boards: each reflects the room a little differently.
        const float centered = 2.0f * acrossFraction - 1.0f;
        sample.height += (hashUnit(5, 0, board.seed) - 0.5f) * 0.0003f * centered * centered;

        // Wear over the floor: lighter, rougher traffic areas (tiles with the repeat).
        const float wear = fbm(u, v, 3, 3, 3, 0.5f, subSeed(seed, 2));
        sample.albedo *= 1.0f + 0.035f * wear;
        sample.roughness += 0.05f * std::max(wear, 0.0f);

        // Joints: a V-shaped micro-bevel along every edge and a dark gap between the boards, covered in proportion to
        // the share of the pixel it takes (no aliasing of the thin lines).
        const float side = std::min(acrossFraction, 1.0f - acrossFraction) * plankWidth;
        const float ends = std::min(t - start, end - t) * meters.y;
        const float edge = std::min(side, ends);
        if (edge < kBevelWidth) {
            sample.height -= kBevelDepth * (1.0f - edge / kBevelWidth);
            sample.albedo *= 0.8f + 0.2f * (edge / kBevelWidth);
        }
        const float gap = std::clamp((kGapHalfWidth - edge) / pixel + 0.5f, 0.0f, 1.0f);
        if (gap > 0.0f) {
            sample.albedo = glm::mix(sample.albedo, style.base * 0.18f, gap);
            sample.roughness = glm::mix(sample.roughness, 0.9f, gap);
            sample.height = glm::mix(sample.height, -kGapDepth, gap);
        }
        sample.roughness = std::clamp(sample.roughness, 0.0f, 1.0f);
        return sample;
    });
}

namespace {

/// A furniture surface: the grain runs along u; across v lie `boards` boards glued edge to edge (a solid panel), each
/// with its own pith, and the whole repeats over `info.meters`.
ProceduralSurface makePanel(const ProceduralSurfaceInfo& info, const WoodStyle& style, int boards,
                            std::uint32_t seed) {
    const float boardWidth = info.meters.y / static_cast<float>(boards);
    const float pixel = info.meters.y / static_cast<float>(info.size.y);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = true;
    return renderSurface(desc, [&](float u, float v) {
        const int index = std::min(floorToInt(v * static_cast<float>(boards)), boards - 1);
        const float across = (v * static_cast<float>(boards) - static_cast<float>(index) - 0.5f) * boardWidth;
        const Board board = boardFrom(style, boardWidth, subSeed(seed, static_cast<std::uint32_t>(index)));
        SurfaceSample sample = woodAt(style, board, u * info.meters.x, across, info.meters.x);
        // Boards of one panel differ a little in tone; the glue line between two is a faint dark hairline.
        sample.albedo *= 0.94f + 0.12f * hashUnit(1, 0, board.seed);
        const float joint = 0.5f * boardWidth - std::abs(across);
        sample.albedo *= 1.0f - 0.25f * std::clamp((0.0004f - joint) / pixel + 0.5f, 0.0f, 1.0f);
        return sample;
    });
}

} // namespace

ProceduralSurface makeWalnut(const ProceduralSurfaceInfo& info) {
    // Dark chocolate walnut with lighter tan streaks, flat-sawn arches and a satin finish; 25 cm boards.
    WoodStyle style;
    style.base = linearFromHex(kWalnutHex);
    style.earlyTint = glm::vec3(1.21f, 1.21f, 1.2f);
    style.lateTint = glm::vec3(0.55f, 0.51f, 0.53f);
    style.ringSpacing = 0.006f;
    style.pithDistanceMin = 0.01f;
    style.pithDistanceMax = 0.14f;
    style.drift = 0.025f;
    style.irregularity = 0.9f;
    style.streakContrast = 0.16f;
    style.poreThreshold = 0.72f;
    style.poreDarkening = 0.35f;
    style.roughness = 0.46f;
    style.roughnessVariation = 0.05f;
    style.poreRoughness = 0.18f;
    return makePanel(info, style, 4, subSeed(kWoodSeed, 3));
}

ProceduralSurface makeVarnishedWood(const ProceduralSurfaceInfo& info) {
    // Reddish-brown boards under a glossy varnish: mostly straight, close grain (boards cut far from the pith); 20 cm
    // boards.
    WoodStyle style;
    style.base = linearFromHex(kVarnishedHex);
    style.earlyTint = glm::vec3(1.1f, 1.1f, 1.09f);
    style.lateTint = glm::vec3(0.68f, 0.63f, 0.61f);
    style.ringSpacing = 0.004f;
    style.pithDistanceMin = 0.04f;
    style.pithDistanceMax = 0.25f;
    style.drift = 0.008f;
    style.irregularity = 0.6f;
    style.streakContrast = 0.08f;
    style.poreThreshold = 0.78f;
    style.poreDarkening = 0.25f;
    style.roughness = 0.3f;
    style.roughnessVariation = 0.03f;
    style.poreRoughness = 0.1f;
    style.poreDepth = 0.00004f;
    return makePanel(info, style, 5, subSeed(kWoodSeed, 4));
}

ProceduralSurface makeOak(const ProceduralSurfaceInfo& info) {
    // Medium oak with open, ring-porous grain: pronounced pores, moderate arches, satin finish; 20 cm boards.
    WoodStyle style;
    style.base = linearFromHex(kOakHex);
    style.earlyTint = glm::vec3(1.2f, 1.2f, 1.19f);
    style.lateTint = glm::vec3(0.6f, 0.56f, 0.54f);
    style.ringSpacing = 0.005f;
    style.pithDistanceMin = 0.01f;
    style.pithDistanceMax = 0.12f;
    style.drift = 0.018f;
    style.irregularity = 0.8f;
    style.streakContrast = 0.1f;
    style.poreThreshold = 0.68f;
    style.poreDarkening = 0.3f;
    style.roughness = 0.5f;
    style.roughnessVariation = 0.04f;
    style.poreRoughness = 0.15f;
    return makePanel(info, style, 5, subSeed(kWoodSeed, 5));
}

} // namespace rv::assets::detail
