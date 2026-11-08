// Wallpaper of the catalog (surface_textures.hpp, I12 step 1): an original tone-on-tone damask for the rooms' walls
// between the wainscot and the picture rail, tinted per theme pack by its material. Original procedural design.

#include "procedural_noise.hpp"
#include "surface_canvas.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rv::assets::detail {
namespace {

constexpr std::uint32_t kWallpaperSeed = 0xDA3A5C0Bu;
constexpr float kPi = 3.14159265358979f;

/// Meters between two motifs of one set; the second set stands between them (a centred lattice).
constexpr float kMotifRepeat = 0.5f;
/// The onion-shaped frame: its half height and widest half width, meters.
constexpr float kFrameHalfHeight = 0.235f;
constexpr float kFrameHalfWidth = 0.16f;

/// The share of a pixel `pixel` meters wide that a shape covers, from the signed distance to its edge (negative
/// inside).
float coverage(float distance, float pixel) noexcept {
    return std::clamp(0.5f - distance / pixel, 0.0f, 1.0f);
}

/// The frame's half width at `y` meters below the motif's centre (0 beyond its tips): an onion, fuller below.
float frameProfile(float y) noexcept {
    const float t = y / kFrameHalfHeight;
    if (std::abs(t) >= 1.0f) {
        return 0.0f;
    }
    return kFrameHalfWidth * std::pow(std::cos(kPi * t * 0.5f), 1.3f) * (1.0f - 0.18f * t);
}

/// Signed distance, meters, to an ellipse centred at `centre` with semi-axes `axes`, turned by `angle` radians
/// (approximate: exact on the axes, close enough for small leaves).
float ellipseDistance(glm::vec2 p, glm::vec2 centre, glm::vec2 axes, float angle) noexcept {
    const glm::vec2 d = p - centre;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const glm::vec2 local(c * d.x + s * d.y, -s * d.x + c * d.y);
    const float k = glm::length(local / axes);
    return (k - 1.0f) * std::min(axes.x, axes.y);
}

} // namespace

ProceduralSurface makeWallpaperDamask(const ProceduralSurfaceInfo& info) {
    // Motifs 50 cm apart on a centred lattice (the image holds two repeats each way): each an onion-shaped double frame
    // around a rosette in a roundel (eight rounded petals around a ringed disc), a bead above and below it; where four
    // motifs' cells meet, a small quatrefoil (the same seen from each of them, so the cells join without a seam). No
    // fan of pointed leaves: the first design's palmette read as a cannabis leaf from across the room (I12 step 1's
    // review).
    // Lighter, satin ink with a light emboss on a matte paper ground with a fine fibre: a detail surface, the ground
    // about 0.86 and the ink 1, so that the material's base color tints it (about 0.9 of it on average).
    const float tile = info.meters.x;
    const float pixel = tile / static_cast<float>(info.size.x);
    const std::uint32_t seed = subSeed(kWallpaperSeed, 1);
    CanvasDesc desc;
    desc.size = info.size;
    desc.meters = info.meters;
    desc.tiles = true;
    desc.samplesPerAxis = 2;
    return renderSurface(desc, [&](float u, float v) {
        const float x = u * tile;
        const float y = v * tile; // down the wall
        const auto local = [&](float originX, float originY) {
            float lx = x - originX;
            float ly = y - originY;
            lx -= kMotifRepeat * std::round(lx / kMotifRepeat);
            ly -= kMotifRepeat * std::round(ly / kMotifRepeat);
            return glm::vec2(lx, ly);
        };
        const glm::vec2 first = local(0.0f, 0.0f);
        const glm::vec2 second = local(0.5f * kMotifRepeat, 0.5f * kMotifRepeat);
        const glm::vec2 cell =
            std::abs(first.x) + std::abs(first.y) <= std::abs(second.x) + std::abs(second.y) ? first : second;
        // The motif is symmetric about its vertical axis.
        const glm::vec2 p(std::abs(cell.x), cell.y);

        float ink = 0.0f;
        const auto add = [&](float distance) {
            ink = std::max(ink, coverage(distance, pixel));
        };

        // The double frame: an outer band 4 mm wide and an inner line 1.5 mm wide, 12 mm inside it; distances along
        // the profile's normal (its slope from a central difference).
        if (std::abs(p.y) < kFrameHalfHeight) {
            const float step = 0.002f;
            const float slope = (frameProfile(p.y + step) - frameProfile(p.y - step)) / (2.0f * step);
            const float normalScale = std::sqrt(1.0f + slope * slope);
            const float outer = std::abs(p.x - frameProfile(p.y)) / normalScale;
            add(outer - 0.002f);
            const float inner = std::abs(p.x - std::max(frameProfile(p.y) - 0.012f, 0.0f)) / normalScale;
            if (frameProfile(p.y) > 0.02f) {
                add(inner - 0.00075f);
            }
        }

        // The rosette: eight rounded petals, 6 cm long and 3.2 cm wide, every 45 degrees from the top (the right
        // half's, mirrored), around a disc 4.8 cm wide ringed by a ground hairline; the roundel 21.6 cm wide around
        // them; a bead above and below, towards the frame's tips.
        constexpr float kPetalCentre = 0.048f;
        const glm::vec2 petal(0.030f, 0.016f);
        for (const float degreesFromUp : {0.0f, 45.0f, 90.0f, 135.0f, 180.0f}) {
            const float a = degreesFromUp * kPi / 180.0f;
            const glm::vec2 direction(std::sin(a), -std::cos(a));
            add(ellipseDistance(p, direction * kPetalCentre, petal, std::atan2(direction.y, direction.x)));
        }
        const float fromCentre = glm::length(p);
        const float ring = coverage(std::abs(fromCentre - 0.016f) - 0.0012f, pixel);
        ink = std::max(ink, coverage(fromCentre - 0.024f, pixel) * (1.0f - ring));
        add(std::abs(fromCentre - 0.108f) - 0.0012f);
        add(glm::length(p - glm::vec2(0.0f, -0.152f)) - 0.008f);
        add(glm::length(p - glm::vec2(0.0f, 0.152f)) - 0.008f);

        // The quatrefoil where four cells meet: around the nearest of the cell's corners, the same from every cell.
        const float half = 0.5f * kMotifRepeat;
        glm::vec2 corner = glm::abs(cell);
        corner = corner.x >= corner.y ? glm::vec2(corner.x - half, corner.y) : glm::vec2(corner.y - half, corner.x);
        const glm::vec2 q = glm::abs(corner);
        add(glm::length(q - glm::vec2(0.013f, 0.0f)) - 0.008f);
        add(glm::length(q - glm::vec2(0.0f, 0.013f)) - 0.008f);
        add(glm::length(q) - 0.004f);

        // Paper: a fine fibre, faint clouds.
        const float fibre = valueNoise(u * 512.0f, v * 512.0f, 512, 512, subSeed(seed, 0));
        const float clouds = fbm(u, v, 4, 4, 3, 0.5f, subSeed(seed, 1));
        SurfaceSample sample;
        const float ground = 0.86f * (0.985f + 0.03f * fibre) * (1.0f + 0.015f * clouds);
        sample.albedo = glm::vec3(glm::mix(ground, 0.995f, ink));
        sample.roughness = std::clamp(glm::mix(0.98f + 0.02f * fibre, 0.78f, ink), 0.0f, 1.0f);
        sample.metallic = 0.0f;
        sample.height = 0.00003f * ink + 0.000004f * fibre;
        return sample;
    });
}

} // namespace rv::assets::detail
