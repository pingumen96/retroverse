// A potted plant (I3 step 16, style §8 "decor/plant_*", §13.4: "pianta vicino al lato di una credenza"): an original
// rosette of upright sword leaves in a tapered ceramic pot, narrow enough for the side-wall sockets (0.9 x 0.4 m).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>

#include <rv/core/math.hpp>
#include <rv/core/random.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr std::string_view kKind = "plant";
constexpr int kMaxSeed = 2147483647;
/// Divisible by 4 so the pot reaches its radius exactly on the X and Z axes.
constexpr int kSegments = 40;
/// Bottom radius of the pot relative to its top.
constexpr float kPotBottomRatio = 0.74f;
constexpr float kRimHeight = 0.028f;
constexpr float kSaucerHeight = 0.018f;
constexpr float kRimProud = 0.008f;
/// The soil's top stands this far above the rim's top: the pot reads as filled to the brim.
constexpr float kSoilProud = 0.003f;
constexpr int kLeafSegments = 8;
/// The back face of a leaf stands this far behind its front face (no coincident opposite faces).
constexpr float kLeafThickness = 0.0008f;

Status validate(const PlantParams& p) {
    return firstError({
        checkRange(kKind, "height", p.height, 0.3f, 2.5f),
        checkRange(kKind, "potRadius", p.potRadius, 0.05f, 0.6f),
        checkRange(kKind, "potHeight", p.potHeight, 0.08f, 0.5f * p.height),
        checkRange(kKind, "spread", p.spread, p.potRadius + kRimProud, 1.0f),
        checkRange(kKind, "leaves", p.leaves, 3, 64),
        checkRange(kKind, "seed", p.seed, 0, kMaxSeed),
    });
}

[[nodiscard]] float uniform(core::Random& random, float low, float high) {
    return low + (high - low) * static_cast<float>(random.unit());
}

/// One sword leaf as a two-faced strip: `base` on the soil, rising `height`, its tip leaning out by `lean` along the
/// horizontal unit `out`, `width` at its widest, turned by `twist` radians about its axis from base to tip.
[[nodiscard]] MeshData leaf(const glm::vec3& base, const glm::vec3& out, float height, float lean, float width,
                            float twist) {
    MeshData mesh;
    mesh.name = "leaf";
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    std::array<glm::vec3, kLeafSegments + 1> centres{};
    std::array<glm::vec3, kLeafSegments + 1> across{};
    std::array<float, kLeafSegments + 1> widths{};
    for (int i = 0; i <= kLeafSegments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kLeafSegments);
        centres[static_cast<std::size_t>(i)] = base + up * (height * t) + out * (lean * std::pow(t, 1.6f));
        // Widest at a third of the way up, a narrow foot, a pointed tip.
        const float profile = t < 0.33f ? 0.55f + 0.45f * (t / 0.33f) : std::pow((1.0f - t) / 0.67f, 0.8f);
        widths[static_cast<std::size_t>(i)] = width * profile;
        const float angle = twist * t;
        const glm::vec3 side = glm::normalize(glm::cross(up, out));
        across[static_cast<std::size_t>(i)] = std::cos(angle) * side + std::sin(angle) * out;
    }
    for (int face = 0; face < 2; ++face) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (int i = 0; i <= kLeafSegments; ++i) {
            const auto at = static_cast<std::size_t>(i);
            const std::size_t next = std::min<std::size_t>(at + 1, kLeafSegments);
            const std::size_t previous = at == 0 ? 0 : at - 1;
            const glm::vec3 along = glm::normalize(centres[next] - centres[previous]);
            glm::vec3 normal = glm::normalize(glm::cross(across[at], along));
            if (face == 1) {
                normal = -normal;
            }
            // The back face behind the front one, horizontally (the tip stays at its height).
            const glm::vec3 behind = glm::vec3(normal.x, 0.0f, normal.z);
            const glm::vec3 centre =
                centres[at] + (face == 1 && glm::length(behind) > 1e-6f ? glm::normalize(behind) * kLeafThickness
                                                                         : glm::vec3(0.0f));
            const float v = 1.0f - static_cast<float>(i) / static_cast<float>(kLeafSegments);
            for (int side = 0; side < 2; ++side) {
                Vertex vertex;
                const float sign = side == 0 ? -0.5f : 0.5f;
                vertex.position = centre + across[at] * (sign * widths[at]);
                vertex.normal = normal;
                vertex.uv0 = glm::vec2(face == 0 ? 0.5f + sign : 0.5f - sign, v);
                mesh.vertices.push_back(vertex);
            }
        }
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(kLeafSegments); ++i) {
            const std::uint32_t a = first + 2 * i;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + 2;
            const std::uint32_t d = a + 3;
            if (face == 0) {
                mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
            } else {
                mesh.indices.insert(mesh.indices.end(), {a, d, b, a, c, d});
            }
        }
    }
    computeBounds(mesh);
    computeTangents(mesh);
    return mesh;
}

ProceduralPrefab assemble(const PlantParams& p) {
    detail::PrefabAssembler out(kKind, {"pot", "soil", "leaves", "leaves_alt"});
    core::Random random(static_cast<std::uint64_t>(p.seed) * 4u + 3u);

    // The pot on its saucer (as wide as the leaves' reach, so the bounds are exactly the spread): a tapered body, a
    // rolled rim a little proud of it, the soil filling it to the brim.
    const float body = p.potHeight - kRimHeight;
    const float rimRadius = p.potRadius + kRimProud;
    out.part("pot")
        .frustum(p.spread, p.spread - kSaucerHeight, kSaucerHeight, kSegments, true, false,
                 translation(glm::vec3(0.0f, 0.5f * kSaucerHeight, 0.0f)))
        .frustum(kPotBottomRatio * p.potRadius, p.potRadius, body - kSaucerHeight, kSegments, true, false,
                 translation(glm::vec3(0.0f, kSaucerHeight + 0.5f * (body - kSaucerHeight), 0.0f)))
        .cylinder(rimRadius, kRimHeight, kSegments, true,
                  translation(glm::vec3(0.0f, body + 0.5f * kRimHeight, 0.0f)));
    const float soilTop = p.potHeight + kSoilProud;
    const float soilThickness = 0.01f;
    out.part("soil").cylinder(p.potRadius - 0.012f, soilThickness, kSegments, true,
                              translation(glm::vec3(0.0f, soilTop - 0.5f * soilThickness, 0.0f)));

    // The leaves: a rosette around the axis, the first the tallest (its tip at `height` exactly), each leaning out so
    // that its tip and edges stay within `spread` of the axis.
    const float rise = p.height - soilTop;
    const float maxWidth = std::clamp(0.28f * p.potRadius, 0.025f, 0.07f);
    for (int i = 0; i < p.leaves; ++i) {
        const float angle = core::kTwoPi * (static_cast<float>(i) + uniform(random, -0.3f, 0.3f)) /
                            static_cast<float>(p.leaves);
        const glm::vec3 outward(std::sin(angle), 0.0f, std::cos(angle));
        const float baseRadius = p.potRadius * uniform(random, 0.05f, 0.45f);
        const float height = rise * (i == 0 ? 1.0f : uniform(random, 0.5f, 0.95f));
        const float width = maxWidth * uniform(random, 0.75f, 1.0f);
        const float reach = std::max(0.0f, p.spread - baseRadius - 0.5f * width - 0.004f);
        const float lean = std::min(reach, height * uniform(random, 0.08f, 0.28f));
        const float twist = uniform(random, -0.9f, 0.9f);
        const glm::vec3 base = outward * baseRadius + glm::vec3(0.0f, soilTop - 0.004f, 0.0f);
        out.part(i % 3 == 2 ? "leaves_alt" : "leaves").append(leaf(base, outward, height + 0.004f, lean, width, twist));
    }

    // The bounds collide, as every fixture's do (its saucer is as wide as its leaves).
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildPlant(const PlantParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
