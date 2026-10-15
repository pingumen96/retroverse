// The ornamental rug of the catalog (surface_textures.hpp): an original design in the style §3 palette (bordeaux
// field, navy, cream and muted gold), in the tradition of medallion rugs: an outer guard, a minor border of dots, a
// main border of diamonds with corner rosettes, an inner guard, a lattice field with small flowers, a central lozenge
// medallion with an eight-point star, pendants and corner spandrels. Wool fibers, abrash (dye bands) and wear vary it.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include "procedural_noise.hpp"
#include "surface_canvas.hpp"

namespace rv::assets::detail {
namespace {

constexpr std::uint32_t kRugSeed = 0x2A6E07ADu;

/// The rug's colors (sRGB, derived from style §3: textiles #683D42, tungsten amber, cold accent, ivory, signage
/// green, all muted to dyed wool).
struct RugPalette {
    glm::vec3 navy = linearFromHex(0x1E2537);
    glm::vec3 field = linearFromHex(0x6B3036);
    glm::vec3 lattice = linearFromHex(0x4C1E25);
    glm::vec3 red = linearFromHex(0x8C3339);
    glm::vec3 cream = linearFromHex(0xC9B48E);
    glm::vec3 gold = linearFromHex(0xB08443);
    glm::vec3 minor = linearFromHex(0x5A2730);
    glm::vec3 teal = linearFromHex(0x3E6A5D);
};

// Bands, by distance from the rug's edge (meters).
constexpr float kOuterGuard = 0.025f;
constexpr float kMinorBorder = 0.055f;
constexpr float kMainStart = 0.065f;
constexpr float kMainEnd = 0.215f;
constexpr float kMainCenter = 0.5f * (kMainStart + kMainEnd);
constexpr float kInnerLine = 0.225f;
constexpr float kInnerGuard = 0.255f;
constexpr float kFieldStart = 0.262f;
/// About the length of one motif of the main border, of one dot of the minor border and of the inner guard: each edge
/// fits a whole number of them (motifOffset).
constexpr float kMainPeriod = 0.2f;
constexpr float kMinorPeriod = 0.06f;
constexpr float kInnerPeriod = 0.05f;
/// Spacing of the field's diagonal lattice.
constexpr float kLatticeSpacing = 0.17f;

/// Offset of `s` from the nearest motif center along an edge of `length`, with a whole number of motifs of about
/// `period` between `first` and length - `first`, the first and last centered there (the corners line up).
float motifOffset(float s, float length, float first, float period) {
    const float span = length - 2.0f * first;
    const float fitted = span / std::max(1.0f, std::round(span / period));
    return (fract((s - first) / fitted + 0.5f) - 0.5f) * fitted;
}

/// |x| / a + |y| / b: below 1 inside a lozenge with half diagonals a and b.
float lozenge(float x, float y, float a, float b) {
    return std::abs(x) / a + std::abs(y) / b;
}

/// Color of the design at (x, y) meters on a rug `width` x `depth`.
glm::vec3 rugDesign(const RugPalette& p, float x, float y, float width, float depth) {
    const float dx = std::min(x, width - x);
    const float dy = std::min(y, depth - y);
    const float edge = std::min(dx, dy);
    // Along the nearest edge.
    const bool longEdge = dy <= dx;
    const float along = longEdge ? x : y;
    const float length = longEdge ? width : depth;

    if (edge < kOuterGuard) {
        return p.navy;
    }
    if (edge < kMinorBorder) {
        const float center = 0.5f * (kOuterGuard + kMinorBorder);
        const float m = motifOffset(along, length, center, kMinorPeriod);
        const float q = edge - center;
        return m * m + q * q < 0.008f * 0.008f ? p.cream : p.minor;
    }
    if (edge < kMainStart || (edge >= kMainEnd && edge < kInnerLine)) {
        return p.gold;
    }
    if (edge < kMainEnd) {
        // Corner squares: a rosette of nested diamonds where the two border bands cross.
        if (dx >= kMainStart && dx < kMainEnd && dy >= kMainStart && dy < kMainEnd) {
            const float m = lozenge(dx - kMainCenter, dy - kMainCenter, 1.0f, 1.0f);
            if (m < 0.018f) {
                return p.teal;
            }
            if (m < 0.04f) {
                return p.cream;
            }
            return m < 0.068f ? p.red : p.navy;
        }
        // Whole motifs between the corner squares: the first starts where a corner square ends.
        const float m = motifOffset(along, length, kMainEnd + 0.5f * kMainPeriod, kMainPeriod);
        const float q = edge - kMainCenter;
        if (lozenge(m, q, 0.075f, 0.06f) < 1.0f) {
            if (m * m + q * q < 0.012f * 0.012f) {
                return p.navy;
            }
            return lozenge(m, q, 0.04f, 0.032f) < 1.0f ? p.cream : p.red;
        }
        // A small gold cross between two diamonds.
        const float between = 0.5f * kMainPeriod - std::abs(m);
        if ((between < 0.008f && std::abs(q) < 0.03f) || (std::abs(q) < 0.008f && between < 0.03f)) {
            return p.gold;
        }
        return p.navy;
    }
    if (edge < kInnerGuard) {
        const float center = 0.5f * (kInnerLine + kInnerGuard);
        const float m = motifOffset(along, length, center, kInnerPeriod);
        const float q = edge - center;
        return lozenge(m, q, 0.012f, 0.009f) < 1.0f ? p.gold : p.lattice;
    }
    if (edge < kFieldStart) {
        return p.navy;
    }

    // Field, relative to the rug's center.
    const float cx = x - 0.5f * width;
    const float cy = y - 0.5f * depth;
    const float medallion = lozenge(cx, cy, 0.62f, 0.4f);
    if (medallion < 1.0f) {
        if (medallion >= 0.93f) {
            return p.navy;
        }
        if (medallion >= 0.89f) {
            return p.gold;
        }
        if (medallion >= 0.55f) {
            // Cream ground with a ring of small navy diamonds.
            const float a = (cx + cy) / 0.07f;
            const float b = (cx - cy) / 0.07f;
            const float da = fract(a) - 0.5f;
            const float db = fract(b) - 0.5f;
            return std::abs(da) + std::abs(db) < 0.2f ? p.navy : p.cream;
        }
        if (medallion >= 0.5f) {
            return p.red;
        }
        // Eight-point star: a square and a diamond overlaid, around a gold heart.
        if (cx * cx + cy * cy < 0.04f * 0.04f) {
            return p.gold;
        }
        const float square = std::max(std::abs(cx), std::abs(cy)) / 0.13f;
        const float diamond = lozenge(cx, cy, 0.19f, 0.19f);
        return std::min(square, diamond) < 1.0f ? p.navy : p.red;
    }
    // Pendants at both ends of the medallion's long axis.
    const float pendantX = std::abs(cx) - 0.72f;
    const float pendant = lozenge(pendantX, cy, 0.09f, 0.065f);
    if (pendant < 1.0f) {
        return pendant < 0.4f ? p.teal : p.navy;
    }
    // Corner spandrels: quarter lozenges in the corners of the field.
    const float cornerX = 0.5f * width - kFieldStart;
    const float cornerY = 0.5f * depth - kFieldStart;
    const float spandrel = lozenge(std::abs(cx) - cornerX, std::abs(cy) - cornerY, 0.36f, 0.24f);
    if (spandrel < 1.0f) {
        if (spandrel >= 0.92f) {
            return p.gold;
        }
        const float a = (cx + cy) / 0.06f;
        const float b = (cx - cy) / 0.06f;
        return std::abs(fract(a) - 0.5f) + std::abs(fract(b) - 0.5f) < 0.18f ? p.cream : p.navy;
    }
    // Diagonal lattice with small flowers on its crossings and cream diamonds in its cells.
    const float a = (cx + cy) / kLatticeSpacing;
    const float b = (cx - cy) / kLatticeSpacing;
    const float la = fract(a) - 0.5f;
    const float lb = fract(b) - 0.5f;
    const float crossing = (0.5f - std::abs(la)) + (0.5f - std::abs(lb));
    if (crossing < 0.05f) {
        return p.gold;
    }
    if (crossing < 0.13f) {
        return p.navy;
    }
    if (std::abs(la) + std::abs(lb) < 0.1f) {
        return p.cream;
    }
    if (0.5f - std::abs(la) < 0.03f || 0.5f - std::abs(lb) < 0.03f) {
        return p.lattice;
    }
    return p.field;
}

} // namespace

ProceduralSurface makeOrnateRug(const ProceduralSurfaceInfo& info) {
    const RugPalette palette;
    const float width = info.meters.x;
    const float depth = info.meters.y;
    const std::uint32_t seed = kRugSeed;
    const auto fiberCellsX = static_cast<std::int32_t>(info.size.x / 2);
    const auto fiberCellsY = static_cast<std::int32_t>(info.size.y / 2);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = false;
    desc.samplesPerAxis = 2;
    desc.normalMap = false;
    return renderSurface(desc, [&](float u, float v) {
        glm::vec3 color = rugDesign(palette, u * width, v * depth, width, depth);
        // Wool: fiber noise, abrash (dye lots woven in bands across the length) and a softly worn center.
        const float fiber = valueNoise(u * static_cast<float>(fiberCellsX), v * static_cast<float>(fiberCellsY),
                                       fiberCellsX, fiberCellsY, subSeed(seed, 0));
        const float abrash = 2.0f * valueNoise(u * 14.0f, 0.5f, 14, 1, subSeed(seed, 1)) - 1.0f;
        const float wear = fbm(u, v, 3, 2, 3, 0.5f, subSeed(seed, 2));
        const float centered = 1.0f - smoothStep(0.15f, 0.5f, std::max(std::abs(u - 0.5f), std::abs(v - 0.5f)));
        color *=
            (0.95f + 0.1f * fiber) * (1.0f + 0.035f * abrash) * (1.0f + 0.04f * wear * centered + 0.02f * centered);
        SurfaceSample sample;
        sample.albedo = color;
        sample.roughness = std::clamp(0.92f + 0.06f * (fiber - 0.5f), 0.0f, 1.0f);
        sample.metallic = 0.0f;
        return sample;
    });
}

} // namespace rv::assets::detail
