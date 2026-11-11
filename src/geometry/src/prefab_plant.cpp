// A potted plant (I3 step 16, style §8 "decor/plant_*", §13.4: "pianta vicino al lato di una credenza"): an original
// rosette of upright sword leaves in a tapered ceramic pot, narrow enough for the side-wall sockets (0.9 x 0.4 m).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

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
    const float depthSpread = p.depthSpread > 0.0f ? p.depthSpread : p.spread;
    if (Status status = firstError({
            checkRange(kKind, "height", p.height, 0.3f, 2.5f),
            checkRange(kKind, "potRadius", p.potRadius, 0.05f, 0.6f),
            checkRange(kKind, "potHeight", p.potHeight, 0.08f, 0.5f * p.height),
            checkRange(kKind, "spread", p.spread, p.potRadius + kRimProud, 1.0f),
            checkRange(kKind, "leaves", p.leaves, 3, 64),
            checkRange(kKind, "seed", p.seed, 0, kMaxSeed),
            p.depthSpread == 0.0f ? rv::ok()
                                  : checkRange(kKind, "depthSpread", p.depthSpread, p.potRadius + kRimProud, p.spread),
            p.saucerRadius == 0.0f ? rv::ok()
                                   : checkRange(kKind, "saucerRadius", p.saucerRadius, p.potRadius, depthSpread),
        });
        !status) {
        return status;
    }
    if (p.form == PlantForm::Sword && p.depthSpread != 0.0f) {
        return detail::outOfRange(kKind, "depthSpread", "a sword plant's leaves reach as far every way");
    }
    if ((p.form == PlantForm::Palm || p.form == PlantForm::Ficus) && p.height < p.potHeight + 0.5f) {
        return detail::outOfRange(kKind, "height", "a palm or a ficus rises at least 0.5 m over its pot");
    }
    if (p.form == PlantForm::Fern && p.height - p.potHeight - kSoilProud - 0.32f < 0.25f) {
        return detail::outOfRange(kKind, "height", "a fern's stand is at least 0.25 m high under its pot");
    }
    return ok();
}

[[nodiscard]] float uniform(core::Random& random, float low, float high) {
    return low + (high - low) * static_cast<float>(random.unit());
}

/// A two-faced strip along `centres` (base to tip), `across` its unit width direction and `widths` its width at each.
[[nodiscard]] MeshData strip(std::span<const glm::vec3> centres, std::span<const glm::vec3> across,
                             std::span<const float> widths) {
    MeshData mesh;
    mesh.name = "leaf";
    const std::size_t last = centres.size() - 1;
    const auto segments = static_cast<std::uint32_t>(last);
    for (int face = 0; face < 2; ++face) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (std::size_t at = 0; at <= last; ++at) {
            const std::size_t next = std::min<std::size_t>(at + 1, last);
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
            const float v = 1.0f - static_cast<float>(at) / static_cast<float>(last);
            for (int side = 0; side < 2; ++side) {
                Vertex vertex;
                const float sign = side == 0 ? -0.5f : 0.5f;
                vertex.position = centre + across[at] * (sign * widths[at]);
                vertex.normal = normal;
                vertex.uv0 = glm::vec2(face == 0 ? 0.5f + sign : 0.5f - sign, v);
                mesh.vertices.push_back(vertex);
            }
        }
        for (std::uint32_t i = 0; i < segments; ++i) {
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

/// One sword leaf as a two-faced strip: `base` on the soil, rising `height`, its tip leaning out by `lean` along the
/// horizontal unit `out`, `width` at its widest, turned by `twist` radians about its axis from base to tip.
[[nodiscard]] MeshData leaf(const glm::vec3& base, const glm::vec3& out, float height, float lean, float width,
                            float twist) {
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
    return strip(centres, across, widths);
}

/// An arching frond (I12 step 4): from `base` along the horizontal unit `out` by `reach`, rising `rise` at its crest
/// (two fifths of the way) and ending `droop` below its base, `width` at its widest, flat across. The rise and the
/// droop are at most 0.8 and 1.2 times the reach: a short frond never folds back on itself over its crest.
[[nodiscard]] MeshData frond(const glm::vec3& base, const glm::vec3& out, float reach, float rise, float droop,
                             float width) {
    rise = std::min(rise, 0.8f * reach);
    droop = std::min(droop, 1.2f * reach);
    constexpr int kFrondSegments = 10;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 side = glm::normalize(glm::cross(up, out));
    std::array<glm::vec3, kFrondSegments + 1> centres{};
    std::array<glm::vec3, kFrondSegments + 1> across{};
    std::array<float, kFrondSegments + 1> widths{};
    constexpr float kCrest = 0.4f;
    for (int i = 0; i <= kFrondSegments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kFrondSegments);
        // Rising to its crest, then falling past its base to -droop, level at the crest.
        const float before = (kCrest - t) / kCrest;
        const float after = (t - kCrest) / (1.0f - kCrest);
        const float lift = t <= kCrest ? rise * (1.0f - before * before) : rise - (rise + droop) * after * after;
        centres[static_cast<std::size_t>(i)] = base + out * (reach * t) + up * lift;
        const float profile = t < 0.25f ? 0.4f + 0.6f * (t / 0.25f) : std::pow((1.0f - t) / 0.75f, 0.7f);
        widths[static_cast<std::size_t>(i)] = width * profile;
        across[static_cast<std::size_t>(i)] = side;
    }
    return strip(centres, across, widths);
}

/// A small leaf (I12 step 4's ficus): a diamond from `base` along the unit `direction`, `length` long and `width`
/// wide, flat across `side`.
[[nodiscard]] MeshData smallLeaf(const glm::vec3& base, const glm::vec3& direction, const glm::vec3& side, float length,
                                 float width) {
    const std::array<glm::vec3, 3> centres{base, base + direction * (0.45f * length), base + direction * length};
    const std::array<glm::vec3, 3> across{side, side, side};
    const std::array<float, 3> widths{0.15f * width, width, 0.0f};
    return strip(centres, across, widths);
}

/// A frame at `position` whose +Y points along the unit vector `direction` (a cylinder laid along a segment).
[[nodiscard]] glm::mat4 alongY(const glm::vec3& position, const glm::vec3& direction) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::mat4 at = translation(position);
    const float cosine = glm::dot(up, direction);
    if (cosine > 0.9999f) {
        return at;
    }
    const glm::vec3 axis = glm::normalize(glm::cross(up, direction));
    return glm::rotate(at, std::acos(std::clamp(cosine, -1.0f, 1.0f)), axis);
}

/// A cane or a branch from `from` to `to`, `radius` thick.
void stem(MeshBuilder& builder, const glm::vec3& from, const glm::vec3& to, float radius) {
    const glm::vec3 segment = to - from;
    const float length = glm::length(segment);
    if (length > 1e-4f) {
        builder.cylinder(radius, length, 8, true, alongY(0.5f * (from + to), segment / length));
    }
}

/// How far the leaves may reach from the axis along the horizontal unit `out`: the ellipse of the spread across and
/// the depth's spread front to back.
[[nodiscard]] float reachAlong(const glm::vec3& out, float spread, float depthSpread) {
    const float x = out.x / spread;
    const float z = out.z / depthSpread;
    return 1.0f / std::sqrt(x * x + z * z);
}

/// Where a pot's soil is (addPot()'s).
struct Potted {
    float soilTop = 0.0f;
};

/// The pot on its saucer, its foot `lift` above the floor: a tapered body, a rolled rim a little proud of it, the soil
/// filling it to the brim. `saucer` the saucer's radius, in role `saucerRole`.
Potted addPot(detail::PrefabAssembler& out, const PlantParams& p, float lift, float saucer,
              std::string_view saucerRole) {
    const float body = p.potHeight - kRimHeight;
    const float rimRadius = p.potRadius + kRimProud;
    out.part(saucerRole)
        .frustum(saucer, saucer - kSaucerHeight, kSaucerHeight, kSegments, true, false,
                 translation(glm::vec3(0.0f, lift + 0.5f * kSaucerHeight, 0.0f)));
    out.part("pot")
        .frustum(kPotBottomRatio * p.potRadius, p.potRadius, body - kSaucerHeight, kSegments, true, false,
                 translation(glm::vec3(0.0f, lift + kSaucerHeight + 0.5f * (body - kSaucerHeight), 0.0f)))
        .cylinder(rimRadius, kRimHeight, kSegments, true,
                  translation(glm::vec3(0.0f, lift + body + 0.5f * kRimHeight, 0.0f)));
    const float soilTop = lift + p.potHeight + kSoilProud;
    const float soilThickness = 0.01f;
    out.part("soil").cylinder(p.potRadius - 0.012f, soilThickness, kSegments, true,
                              translation(glm::vec3(0.0f, soilTop - 0.5f * soilThickness, 0.0f)));
    return Potted{soilTop};
}

/// The fern's stand's height under its pot: the fronds rise 0.32 m over the soil.
[[nodiscard]] float fernStandHeight(const PlantParams& p) {
    return p.height - p.potHeight - kSoilProud - 0.32f;
}

/// I12 step 4's forms: a palm, a ficus, a fern on its stand.
ProceduralPrefab assembleForm(const PlantParams& p) {
    detail::PrefabAssembler out(kKind, {"pot", "saucer", "soil", "leaves", "leaves_alt", "trunk", "stand"});
    core::Random random(static_cast<std::uint64_t>(p.seed) * 4u + 3u);
    const float depthSpread = p.depthSpread > 0.0f ? p.depthSpread : p.spread;
    const float saucer = p.saucerRadius > 0.0f ? p.saucerRadius : std::min(p.spread, depthSpread);
    const std::string_view saucerRole = p.saucerRadius > 0.0f ? "saucer" : "pot";
    const float lift = p.form == PlantForm::Fern ? fernStandHeight(p) : 0.0f;
    if (p.form == PlantForm::Fern) {
        // The stand: a base disc, a turned column, a top a little wider than the saucer.
        MeshBuilder& stand = out.part("stand");
        const float base = std::min(0.15f, std::min(p.spread, depthSpread));
        stand.cylinder(base, 0.025f, kSegments, true, translation(glm::vec3(0.0f, 0.0125f, 0.0f)));
        stand.cylinder(0.032f, lift - 0.045f, 16, true,
                       translation(glm::vec3(0.0f, 0.025f + 0.5f * (lift - 0.045f), 0.0f)));
        stand.cylinder(std::min(saucer + 0.02f, std::min(p.spread, depthSpread)), 0.02f, kSegments, true,
                       translation(glm::vec3(0.0f, lift - 0.01f, 0.0f)));
    }
    const Potted pot = addPot(out, p, lift, saucer, saucerRole);
    const float rise = p.height - pot.soilTop;
    switch (p.form) {
    case PlantForm::Palm: {
        // Three canes leaning a little apart, two to four fronds arching from each top.
        MeshBuilder& trunk = out.part("trunk");
        const int canes = 3;
        for (int c = 0; c < canes; ++c) {
            const float angle = core::kTwoPi * (static_cast<float>(c) + uniform(random, -0.2f, 0.2f)) / canes;
            const glm::vec3 outward(std::sin(angle), 0.0f, std::cos(angle));
            const glm::vec3 foot = outward * (0.35f * p.potRadius) + glm::vec3(0.0f, pot.soilTop - 0.01f, 0.0f);
            const float caneHeight = rise * uniform(random, 0.45f, 0.62f);
            const glm::vec3 top = foot + glm::vec3(0.0f, caneHeight, 0.0f) + outward * (0.25f * p.potRadius);
            stem(trunk, foot, top, uniform(random, 0.011f, 0.015f));
            const int fronds = p.leaves / canes + (c < p.leaves % canes ? 1 : 0);
            for (int f = 0; f < fronds; ++f) {
                const float turn = angle + core::kTwoPi * (static_cast<float>(f) + uniform(random, -0.25f, 0.25f)) /
                                               static_cast<float>(std::max(1, fronds)) * 0.8f;
                const glm::vec3 dir(std::sin(turn), 0.0f, std::cos(turn));
                const float width = uniform(random, 0.07f, 0.1f);
                const float start = glm::length(glm::vec2(top.x, top.z));
                const float limit = reachAlong(dir, p.spread, depthSpread) - start - 0.5f * width - 0.01f;
                const float reach = std::max(0.05f, std::min(limit, uniform(random, 0.3f, 0.45f)));
                const float up = std::max(0.02f, std::min(p.height - top.y - 0.005f, uniform(random, 0.18f, 0.32f)));
                const float droop = std::min(top.y - pot.soilTop, uniform(random, 0.1f, 0.3f));
                out.part(f % 3 == 2 ? "leaves_alt" : "leaves").append(frond(top, dir, reach, up, droop, width));
            }
        }
        break;
    }
    case PlantForm::Ficus: {
        // A trunk to half the height, branches into a crown of small leaves over the upper half.
        MeshBuilder& trunk = out.part("trunk");
        const glm::vec3 foot(0.0f, pot.soilTop - 0.01f, 0.0f);
        const glm::vec3 bend(0.02f, pot.soilTop + 0.3f * rise, 0.01f);
        const glm::vec3 fork(-0.01f, pot.soilTop + 0.5f * rise, 0.0f);
        stem(trunk, foot, bend, 0.022f);
        stem(trunk, bend, fork, 0.019f);
        const glm::vec3 crown(0.0f, pot.soilTop + 0.72f * rise, 0.0f);
        const glm::vec3 radii(0.82f * p.spread, 0.27f * rise, 0.82f * depthSpread);
        for (int b = 0; b < 4; ++b) {
            const float angle = core::kTwoPi * (static_cast<float>(b) + 0.5f) / 4.0f;
            const glm::vec3 to = crown + glm::vec3(std::sin(angle) * 0.5f * radii.x, uniform(random, -0.05f, 0.1f),
                                                   std::cos(angle) * 0.5f * radii.z);
            stem(trunk, fork, to, 0.009f);
        }
        // A dense crown: about twelve leaves for each the parameters name, spread through its shell and a little
        // inside it, so that it reads as a mass of leaves and not as bare branches.
        const int leaves = std::max(60, p.leaves * 12);
        for (int i = 0; i < leaves; ++i) {
            // A point in the crown's shell (its upper four fifths), the leaf hanging out and a little down from it.
            const float theta = core::kTwoPi * static_cast<float>(random.unit());
            const float height = uniform(random, -0.6f, 1.0f);
            const float ring = std::sqrt(std::max(0.0f, 1.0f - height * height));
            const float depth = uniform(random, 0.55f, 1.0f);
            const glm::vec3 unit(std::sin(theta) * ring, height, std::cos(theta) * ring);
            const glm::vec3 at = crown + unit * radii * depth;
            const glm::vec3 outward = glm::normalize(glm::vec3(unit.x, 0.0f, unit.z) + glm::vec3(0.0f, 1e-3f, 0.0f));
            const glm::vec3 direction = glm::normalize(outward + glm::vec3(0.0f, uniform(random, -0.6f, 0.3f), 0.0f));
            const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), outward));
            const float length = uniform(random, 0.09f, 0.13f);
            // Shortened to stay within the reach and under the top.
            const float room = reachAlong(outward, p.spread, depthSpread) - glm::length(glm::vec2(at.x, at.z)) - 0.01f;
            const float fits = std::min({length, room, p.height - 0.003f - at.y});
            if (fits < 0.03f) {
                continue;
            }
            out.part(i % 3 == 2 ? "leaves_alt" : "leaves")
                .append(smallLeaf(at, direction, side, fits, uniform(random, 0.045f, 0.065f)));
        }
        break;
    }
    case PlantForm::Fern: {
        // Fronds from the soil arching up and over the rim, down past the pot.
        for (int i = 0; i < p.leaves; ++i) {
            const float angle =
                core::kTwoPi * (static_cast<float>(i) + uniform(random, -0.3f, 0.3f)) / static_cast<float>(p.leaves);
            const glm::vec3 dir(std::sin(angle), 0.0f, std::cos(angle));
            const glm::vec3 base = dir * (0.3f * p.potRadius) + glm::vec3(0.0f, pot.soilTop - 0.004f, 0.0f);
            const float width = uniform(random, 0.05f, 0.075f);
            const float limit = reachAlong(dir, p.spread, depthSpread) - 0.3f * p.potRadius - 0.5f * width - 0.01f;
            const float reach = std::max(0.05f, std::min(limit, uniform(random, 0.28f, 0.42f)));
            const float up = std::min(rise - 0.003f, uniform(random, 0.12f, 0.3f));
            const float droop = std::min(pot.soilTop - 0.05f, uniform(random, 0.15f, 0.35f));
            out.part(i % 3 == 2 ? "leaves_alt" : "leaves").append(frond(base, dir, reach, up, droop, width));
        }
        break;
    }
    case PlantForm::Sword:
        break;
    }
    return std::move(out).finish();
}

ProceduralPrefab assemble(const PlantParams& p) {
    if (p.form != PlantForm::Sword) {
        return assembleForm(p);
    }
    detail::PrefabAssembler out(kKind, {"pot", "saucer", "soil", "leaves", "leaves_alt"});
    core::Random random(static_cast<std::uint64_t>(p.seed) * 4u + 3u);

    // The pot on its saucer (by default as wide as the leaves' reach, so the bounds are exactly the spread): a tapered
    // body, a rolled rim a little proud of it, the soil filling it to the brim.
    const float body = p.potHeight - kRimHeight;
    const float rimRadius = p.potRadius + kRimProud;
    const bool ownSaucer = p.saucerRadius > 0.0f;
    const float saucer = ownSaucer ? p.saucerRadius : p.spread;
    out.part(ownSaucer ? "saucer" : "pot")
        .frustum(saucer, saucer - kSaucerHeight, kSaucerHeight, kSegments, true, false,
                 translation(glm::vec3(0.0f, 0.5f * kSaucerHeight, 0.0f)));
    out.part("pot")
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
