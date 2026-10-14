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

float sanitizedLength(float value, float fallback) noexcept {
    return std::isfinite(value) ? std::max(value, kMinCapsuleSize) : fallback;
}

Capsule sanitized(const Capsule& capsule) noexcept {
    const Capsule defaults;
    return Capsule{sanitizedLength(capsule.radius, defaults.radius), sanitizedLength(capsule.height, defaults.height)};
}

bool blocksAtHeight(const core::Aabb& box, float feetY, float height) noexcept {
    return box.max.y > feetY + CollisionWorld::kVerticalTolerance &&
           box.min.y < feetY + height - CollisionWorld::kVerticalTolerance;
}

/// Minimum translation (x, z) that separates a circle from the XZ rectangle of `box`; zero when the circle
/// penetrates by at most the contact tolerance.
glm::vec2 separation(const glm::vec2& center, float radius, const core::Aabb& box) noexcept {
    const glm::vec2 rectMin(box.min.x, box.min.z);
    const glm::vec2 rectMax(box.max.x, box.max.z);
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

glm::vec2 separationAt(const glm::vec3& feet, const Capsule& capsule, const core::Aabb& box) noexcept {
    if (!blocksAtHeight(box, feet.y, capsule.height)) {
        return glm::vec2(0.0f);
    }
    return separation(glm::vec2(feet.x, feet.z), capsule.radius, box);
}

bool overlapsAny(const glm::vec3& feet, const Capsule& capsule, std::span<const ColliderBox> boxes) noexcept {
    return std::any_of(boxes.begin(), boxes.end(), [&](const ColliderBox& collider) {
        const glm::vec2 push = separationAt(feet, capsule, collider.box);
        return push.x != 0.0f || push.y != 0.0f;
    });
}

/// Pushes `feet` out of every blocking box (in insertion order, repeated until nothing moves). Returns true
/// when the result overlaps nothing; otherwise `feet` holds the best effort after the last pass.
bool resolve(glm::vec3& feet, const Capsule& capsule, std::span<const ColliderBox> boxes) noexcept {
    for (int pass = 0; pass < kMaxResolvePasses; ++pass) {
        bool moved = false;
        for (const ColliderBox& collider : boxes) {
            const glm::vec2 push = separationAt(feet, capsule, collider.box);
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
    if (box.isEmpty() || !core::isFinite(box.min) || !core::isFinite(box.max)) {
        return;
    }
    m_boxes.push_back(ColliderBox{box, std::move(tag)});
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
