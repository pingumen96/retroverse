#include <rv/scene/collision.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

constexpr float kMinCapsuleSize = 0.01f;
/// Gauss-Seidel passes over the boxes per sub-step. Perpendicular walls converge in one pass; a gap narrower
/// than the capsule never converges and the sub-step is rejected.
constexpr int kMaxResolvePasses = 8;
/// Relative tolerance under which a placed box axis counts as exactly horizontal, vertical or along a world axis:
/// about 0.0006 degrees, above the float error of a quarter turn and far below the contact tolerance at room sizes.
constexpr float kAxisTolerance = 1e-5f;

float sanitizedLength(float value, float fallback) noexcept {
    return std::isfinite(value) ? std::max(value, kMinCapsuleSize) : fallback;
}

Capsule sanitized(const Capsule& capsule) noexcept {
    const Capsule defaults;
    return Capsule{sanitizedLength(capsule.radius, defaults.radius), sanitizedLength(capsule.height, defaults.height)};
}

bool usable(const core::Aabb& box) noexcept {
    return !box.isEmpty() && core::isFinite(box.min) && core::isFinite(box.max);
}

/// Footprint of `box` placed by `transform` when the transform keeps the box upright and turns it by an angle that is
/// not a multiple of 90 degrees. Nothing when the world box is already exact (no turn, quarter turns), or when the
/// box is tilted or sheared and the world box is the conservative fallback.
std::optional<TurnedFootprint> turnedFootprint(const core::Aabb& box, const glm::mat4& transform) noexcept {
    const glm::vec3 axisX(transform[0]);
    const glm::vec3 axisY(transform[1]);
    const glm::vec3 axisZ(transform[2]);
    const glm::vec2 flatX(axisX.x, axisX.z);
    const glm::vec2 flatZ(axisZ.x, axisZ.z);
    const float lengthX = glm::length(flatX);
    const float lengthZ = glm::length(flatZ);
    const bool upright = lengthX > 0.0f && lengthZ > 0.0f && std::abs(axisX.y) <= kAxisTolerance * lengthX &&
                         std::abs(axisZ.y) <= kAxisTolerance * lengthZ &&
                         glm::length(glm::vec2(axisY.x, axisY.z)) <= kAxisTolerance * std::abs(axisY.y);
    if (!upright) {
        return std::nullopt;
    }
    TurnedFootprint footprint;
    footprint.axisX = flatX / lengthX;
    footprint.axisZ = flatZ / lengthZ;
    const bool sheared = std::abs(glm::dot(footprint.axisX, footprint.axisZ)) > kAxisTolerance;
    const bool quarterTurn = std::min(std::abs(footprint.axisX.x), std::abs(footprint.axisX.y)) <= kAxisTolerance;
    if (sheared || quarterTurn) {
        return std::nullopt;
    }
    const glm::vec3 center(transform * glm::vec4(box.center(), 1.0f));
    const glm::vec3 half = box.halfExtents();
    footprint.center = glm::vec2(center.x, center.z);
    footprint.halfExtents = glm::vec2(half.x * lengthX, half.z * lengthZ);
    return footprint;
}

/// `point` (world x, z) in the frame of a turned footprint: relative to its center, along its axes.
glm::vec2 toFootprint(const TurnedFootprint& footprint, const glm::vec2& point) noexcept {
    const glm::vec2 offset = point - footprint.center;
    return glm::vec2(glm::dot(offset, footprint.axisX), glm::dot(offset, footprint.axisZ));
}

bool blocksAtHeight(const core::Aabb& box, float feetY, float height) noexcept {
    return box.max.y > feetY + CollisionWorld::kVerticalTolerance &&
           box.min.y < feetY + height - CollisionWorld::kVerticalTolerance;
}

/// Minimum translation that separates a circle from the rectangle [rectMin, rectMax] (same 2D frame); zero when the
/// circle penetrates by at most the contact tolerance.
glm::vec2 separation(const glm::vec2& center, float radius, const glm::vec2& rectMin,
                     const glm::vec2& rectMax) noexcept {
    const glm::vec2 delta = center - glm::clamp(center, rectMin, rectMax);
    const float distanceSquared = glm::dot(delta, delta);
    if (distanceSquared > 0.0f) {
        // Center outside the rectangle: push away from the closest point (radially around corners).
        const float distance = std::sqrt(distanceSquared);
        const float penetration = radius - distance;
        return penetration > CollisionWorld::kContactTolerance ? delta * (penetration / distance) : glm::vec2(0.0f);
    }
    // Center inside the rectangle: leave through the nearest edge (fixed order on ties keeps it deterministic).
    const std::pair<float, glm::vec2> exits[] = {
        {center.x - rectMin.x, glm::vec2(-1.0f, 0.0f)},
        {rectMax.x - center.x, glm::vec2(1.0f, 0.0f)},
        {center.y - rectMin.y, glm::vec2(0.0f, -1.0f)},
        {rectMax.y - center.y, glm::vec2(0.0f, 1.0f)},
    };
    const auto* nearest = std::min_element(std::begin(exits), std::end(exits),
                                           [](const auto& a, const auto& b) { return a.first < b.first; });
    return nearest->second * (nearest->first + radius);
}

/// Minimum translation (world x, z) that separates a circle from the footprint of `collider` (see separation()).
glm::vec2 separation(const glm::vec2& center, float radius, const ColliderBox& collider) noexcept {
    if (!collider.turned) {
        return separation(center, radius, glm::vec2(collider.box.min.x, collider.box.min.z),
                          glm::vec2(collider.box.max.x, collider.box.max.z));
    }
    const TurnedFootprint& footprint = *collider.turned;
    const glm::vec2 push =
        separation(toFootprint(footprint, center), radius, -footprint.halfExtents, footprint.halfExtents);
    return push.x * footprint.axisX + push.y * footprint.axisZ;
}

/// Horizontal distance from `point` (world x, z) to the footprint of `collider`, 0 inside it.
float footprintDistance(const glm::vec2& point, const ColliderBox& collider) noexcept {
    if (!collider.turned) {
        const glm::vec2 closest = glm::clamp(point, glm::vec2(collider.box.min.x, collider.box.min.z),
                                             glm::vec2(collider.box.max.x, collider.box.max.z));
        return glm::length(point - closest);
    }
    const TurnedFootprint& footprint = *collider.turned;
    const glm::vec2 local = toFootprint(footprint, point);
    return glm::length(local - glm::clamp(local, -footprint.halfExtents, footprint.halfExtents));
}

glm::vec2 separationAt(const glm::vec3& feet, const Capsule& capsule, const ColliderBox& collider) noexcept {
    if (!blocksAtHeight(collider.box, feet.y, capsule.height)) {
        return glm::vec2(0.0f);
    }
    return separation(glm::vec2(feet.x, feet.z), capsule.radius, collider);
}

bool overlapsAny(const glm::vec3& feet, const Capsule& capsule, std::span<const ColliderBox> boxes) noexcept {
    return std::any_of(boxes.begin(), boxes.end(), [&](const ColliderBox& collider) {
        const glm::vec2 push = separationAt(feet, capsule, collider);
        return push.x != 0.0f || push.y != 0.0f;
    });
}

/// Pushes `feet` out of every blocking box (in insertion order, repeated until nothing moves). Returns true
/// when the result overlaps nothing; otherwise `feet` holds the best effort after the last pass.
bool resolve(glm::vec3& feet, const Capsule& capsule, std::span<const ColliderBox> boxes) noexcept {
    for (int pass = 0; pass < kMaxResolvePasses; ++pass) {
        bool moved = false;
        for (const ColliderBox& collider : boxes) {
            const glm::vec2 push = separationAt(feet, capsule, collider);
            if (push.x != 0.0f || push.y != 0.0f) {
                feet.x += push.x;
                feet.z += push.y;
                moved = true;
            }
        }
        if (!moved) {
            return true;
        }
    }
    return !overlapsAny(feet, capsule, boxes);
}

} // namespace

void CollisionWorld::addBox(const core::Aabb& box, std::string tag) {
    if (usable(box)) {
        m_boxes.push_back(ColliderBox{box, std::move(tag), std::nullopt});
    }
}

void CollisionWorld::addBox(const core::Aabb& box, const glm::mat4& transform, std::string tag) {
    if (!usable(box)) {
        return;
    }
    const core::Aabb world = box.transformed(transform);
    if (usable(world)) {
        m_boxes.push_back(ColliderBox{world, std::move(tag), turnedFootprint(box, transform)});
    }
}

void CollisionWorld::clear() noexcept {
    m_boxes.clear();
}

bool CollisionWorld::overlaps(const glm::vec3& feet, const Capsule& capsule) const {
    if (!core::isFinite(feet)) {
        return false;
    }
    return overlapsAny(feet, sanitized(capsule), m_boxes);
}

std::optional<ColliderDistance> CollisionWorld::nearestBlocking(const glm::vec3& feet, const Capsule& capsule) const {
    if (!core::isFinite(feet)) {
        return std::nullopt;
    }
    const Capsule shape = sanitized(capsule);
    const glm::vec2 axis(feet.x, feet.z);
    std::optional<ColliderDistance> nearest;
    for (std::size_t index = 0; index < m_boxes.size(); ++index) {
        if (!blocksAtHeight(m_boxes[index].box, feet.y, shape.height)) {
            continue;
        }
        const float distance = footprintDistance(axis, m_boxes[index]);
        if (!nearest || distance < nearest->distance) {
            nearest = ColliderDistance{index, distance};
        }
    }
    return nearest;
}

glm::vec3 CollisionWorld::moveCapsule(const glm::vec3& feet, const glm::vec3& displacement,
                                      const Capsule& capsule) const {
    if (!core::isFinite(feet)) {
        return feet;
    }
    const Capsule shape = sanitized(capsule);
    const glm::vec3 delta = core::isFinite(displacement) ? displacement : glm::vec3(0.0f);

    glm::vec2 horizontal(delta.x, delta.z);
    const float length = glm::length(horizontal);
    if (length > kMaxDisplacement) {
        horizontal *= kMaxDisplacement / length;
    }
    const float maxStep = 0.5f * shape.radius;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::min(length, kMaxDisplacement) / maxStep)));

    // Depenetrate the start (best effort), then walk the path. `offset` accumulates the corrections so that an
    // unobstructed move ends exactly at feet + displacement.
    glm::vec3 position = feet;
    resolve(position, shape, m_boxes);
    const glm::vec2 start(feet.x, feet.z);
    glm::vec2 offset(position.x - feet.x, position.z - feet.z);

    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const glm::vec2 planned = start + horizontal * t + offset;
        const float y = feet.y + delta.y * t;

        glm::vec3 candidate(planned.x, y, planned.y);
        const bool resolved = resolve(candidate, shape, m_boxes);
        const glm::vec3 previous(position.x, y, position.z);
        // Reject an unresolvable sub-step (gap narrower than the capsule) unless already stuck, so a capsule
        // spawned in such a gap can still walk out.
        position = (resolved || overlapsAny(previous, shape, m_boxes)) ? candidate : previous;
        offset = glm::vec2(position.x, position.z) - (start + horizontal * t);
    }
    return position;
}

} // namespace rv::scene
