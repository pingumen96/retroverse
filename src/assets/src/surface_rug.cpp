// The ornamental rug of the catalog (surface_textures.hpp): an original design in the style §3 palette (bordeaux
// field, navy, cream and muted gold), in the tradition of medallion rugs: an outer guard, a minor border of dots, a
// main border of diamonds with corner rosettes, an inner guard, a lattice field with small flowers, a central lozenge
// medallion with an eight-point star, pendants and corner spandrels. Wool fibers, abrash (dye bands) and wear vary it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include "procedural_noise.hpp"
#include "surface_canvas.hpp"

namespace rv::assets::detail {
namespace {

constexpr std::uint32_t kRugSeed = 0x2A6E07ADu;
constexpr std::uint32_t kGeometricSeed = 0x9055E0A1u;

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
/// A runner's medallions, one every this many meters along its length (I6 step 3b).
constexpr float kRunnerPeriod = 0.6f;

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

/// The field of a runner (I6 step 3b), relative to its centre: lozenge medallions repeated along its length (y), a
/// small lattice of diamonds between them.
glm::vec3 runnerField(const RugPalette& p, float cx, float cy, float width) {
    const float half = 0.5f * width - kFieldStart;
    const float m = (fract(cy / kRunnerPeriod + 0.5f) - 0.5f) * kRunnerPeriod;
    const float medallion = lozenge(cx, m, 0.92f * half, 0.24f);
    if (medallion < 1.0f) {
        if (medallion >= 0.9f) {
            return p.navy;
        }
        if (medallion >= 0.82f) {
            return p.gold;
        }
        if (medallion >= 0.5f) {
            return p.red;
        }
        if (medallion >= 0.42f) {
            return p.cream;
        }
        const float star = std::min(std::max(std::abs(cx), std::abs(m)) / 0.06f, lozenge(cx, m, 0.09f, 0.09f));
        return star < 1.0f ? p.gold : p.navy;
    }
    const float a = (cx + cy) / 0.09f;
    const float b = (cx - cy) / 0.09f;
    if (std::abs(fract(a) - 0.5f) + std::abs(fract(b) - 0.5f) < 0.16f) {
        return p.cream;
    }
    if (0.5f - std::abs(fract(a) - 0.5f) < 0.04f || 0.5f - std::abs(fract(b) - 0.5f) < 0.04f) {
        return p.lattice;
    }
    return p.field;
}

/// Color of the design at (x, y) meters on a rug `width` x `depth`, a runner's field when `runner`.
glm::vec3 rugDesign(const RugPalette& p, float x, float y, float width, float depth, bool runner) {
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
    if (runner) {
        return runnerField(p, cx, cy, width);
    }
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

namespace {

/// The ornamental rug in `palette`, a runner's field when `runner` (makeOrnateRug()'s, which the variants share).
ProceduralSurface ornateRug(const ProceduralSurfaceInfo& info, const RugPalette& palette, bool runner) {
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
        glm::vec3 color = rugDesign(palette, u * width, v * depth, width, depth, runner);
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

/// The 90s collector's rug (I6 step 3b): confetti of the decade on charcoal inside a teal border and a yellow line.
glm::vec3 geometricDesign(float x, float y, float width, float depth) {
    const glm::vec3 charcoal = linearFromHex(0x26262E);
    const glm::vec3 teal = linearFromHex(0x2A8F88);
    const glm::vec3 yellow = linearFromHex(0xD9AE36);
    const std::array<glm::vec3, 5> confetti = {linearFromHex(0x2A8F88), linearFromHex(0xC2486F),
                                               linearFromHex(0xD9AE36), linearFromHex(0x6B4FA0),
                                               linearFromHex(0xD8D0BE)};
    const float edge = std::min(std::min(x, width - x), std::min(y, depth - y));
    if (edge < 0.07f) {
        return teal;
    }
    if (edge < 0.09f) {
        return charcoal;
    }
    if (edge < 0.105f) {
        return yellow;
    }
    if (edge < 0.13f) {
        return charcoal;
    }
    // One shape per cell of the field, chosen, coloured, placed and turned by the cell's hash.
    constexpr float kCell = 0.2f;
    const auto column = static_cast<std::int32_t>(std::floor(x / kCell));
    const auto row = static_cast<std::int32_t>(std::floor(y / kCell));
    const std::uint32_t h = hashPoint(column, row, kGeometricSeed);
    const float jitterX = (hashUnit(column, row, kGeometricSeed + 1) - 0.5f) * 0.05f;
    const float jitterY = (hashUnit(column, row, kGeometricSeed + 2) - 0.5f) * 0.05f;
    const float lx = x - (static_cast<float>(column) + 0.5f) * kCell - jitterX;
    const float ly = y - (static_cast<float>(row) + 0.5f) * kCell - jitterY;
    const float angle = hashUnit(column, row, kGeometricSeed + 3) * 6.2831853f;
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);
    const float rx = ca * lx - sa * ly;
    const float ry = sa * lx + ca * ly;
    const glm::vec3 colour = confetti[(h >> 8) % confetti.size()];
    switch (h % 6) {
    case 0: // a triangle
        if (ry > -0.035f && ry < 0.045f && std::abs(rx) < 0.5f * (0.045f - ry) * 1.1f) {
            return colour;
        }
        break;
    case 1: { // a ring
        const float d = std::sqrt(lx * lx + ly * ly);
        if (d < 0.05f && d > 0.032f) {
            return colour;
        }
        break;
    }
    case 2: // a zigzag
        if (std::abs(rx) < 0.07f && std::abs(ry - 0.022f * (2.0f * std::abs(fract(rx / 0.035f) - 0.5f) - 0.5f) * 2.0f) <
                                        0.008f) {
            return colour;
        }
        break;
    case 3: // a cluster of dots
        if (std::abs(rx) < 0.05f && std::abs(ry) < 0.05f) {
            const float dx = fract(rx / 0.025f + 0.5f) - 0.5f;
            const float dy = fract(ry / 0.025f + 0.5f) - 0.5f;
            if (dx * dx + dy * dy < 0.09f) {
                return colour;
            }
        }
        break;
    case 4: // a square
        if (std::abs(rx) < 0.03f && std::abs(ry) < 0.03f) {
            return colour;
        }
        break;
    default: // a bar
        if (std::abs(rx) < 0.06f && std::abs(ry) < 0.01f) {
            return colour;
        }
        break;
    }
    return charcoal;
}

} // namespace

ProceduralSurface makeOrnateRug(const ProceduralSurfaceInfo& info) {
    return ornateRug(info, RugPalette{}, false);
}

ProceduralSurface makeOrnateRugBlue(const ProceduralSurfaceInfo& info) {
    RugPalette palette;
    palette.navy = linearFromHex(0x1C2848);
    palette.field = linearFromHex(0x263F78);
    palette.lattice = linearFromHex(0x1A2A55);
    palette.red = linearFromHex(0x7A2C34);
    palette.cream = linearFromHex(0xC4B896);
    palette.gold = linearFromHex(0x9A8A62);
    palette.minor = linearFromHex(0x22305A);
    palette.teal = linearFromHex(0x5A8AC8);
    return ornateRug(info, palette, false);
}

ProceduralSurface makeOrnateRugSage(const ProceduralSurfaceInfo& info) {
    RugPalette palette;
    palette.navy = linearFromHex(0x3B4A44);
    palette.field = linearFromHex(0x6E7B68);
    palette.lattice = linearFromHex(0x58644F);
    palette.red = linearFromHex(0x8E6E52);
    palette.cream = linearFromHex(0xD2C6A4);
    palette.gold = linearFromHex(0xA89060);
    palette.minor = linearFromHex(0x4B5A52);
    palette.teal = linearFromHex(0x5E8A86);
    return ornateRug(info, palette, false);
}

ProceduralSurface makeOrnateRugGreen(const ProceduralSurfaceInfo& info) {
    RugPalette palette;
    palette.navy = linearFromHex(0x1F2A22);
    palette.field = linearFromHex(0x2E4A36);
    palette.lattice = linearFromHex(0x223828);
    palette.red = linearFromHex(0x56602E);
    palette.cream = linearFromHex(0xC8B88E);
    palette.gold = linearFromHex(0x9C8C4E);
    palette.minor = linearFromHex(0x263A2C);
    palette.teal = linearFromHex(0x3E6A5D);
    return ornateRug(info, palette, false);
}

ProceduralSurface makeRunnerRug(const ProceduralSurfaceInfo& info) {
    RugPalette palette;
    palette.navy = linearFromHex(0x16121A);
    palette.field = linearFromHex(0x5E1A26);
    palette.lattice = linearFromHex(0x40121C);
    palette.red = linearFromHex(0xA8343A);
    palette.cream = linearFromHex(0xC9A07A);
    palette.gold = linearFromHex(0xB07A3A);
    palette.minor = linearFromHex(0x2A1820);
    palette.teal = linearFromHex(0x6A3A8C);
    return ornateRug(info, palette, true);
}

ProceduralSurface makeGeometricRug(const ProceduralSurfaceInfo& info) {
    const float width = info.meters.x;
    const float depth = info.meters.y;
    const auto fiberCellsX = static_cast<std::int32_t>(info.size.x / 2);
    const auto fiberCellsY = static_cast<std::int32_t>(info.size.y / 2);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = false;
    desc.samplesPerAxis = 2;
    desc.normalMap = false;
    return renderSurface(desc, [&](float u, float v) {
        glm::vec3 color = geometricDesign(u * width, v * depth, width, depth);
        const float fiber = valueNoise(u * static_cast<float>(fiberCellsX), v * static_cast<float>(fiberCellsY),
                                       fiberCellsX, fiberCellsY, subSeed(kGeometricSeed, 0));
        const float wear = fbm(u, v, 3, 2, 3, 0.5f, subSeed(kGeometricSeed, 2));
        color *= (0.94f + 0.12f * fiber) * (1.0f + 0.03f * wear);
        SurfaceSample sample;
        sample.albedo = color;
        sample.roughness = std::clamp(0.9f + 0.06f * (fiber - 0.5f), 0.0f, 1.0f);
        sample.metallic = 0.0f;
        return sample;
    });
}

} // namespace rv::assets::detail
