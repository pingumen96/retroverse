#pragma once

#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>

namespace rv::scene {

/// Vertical player capsule, meters. The reference position is the FEET: the capsule spans
/// [feet.y, feet.y + height] vertically and is a circle of `radius` around feet.xz horizontally (it is resolved
/// as an upright cylinder; the rounded caps do not matter without stairs or slopes).
struct Capsule {
    float radius = 0.30f;
    float height = 1.75f;
};

/// Static axis-aligned collider. `tag` names the source (object id, "wall_north", ...) for debugging.
struct ColliderBox {
    core::Aabb box;
    std::string tag;
};

/// Static collision geometry for first-person navigation (spec §14.3): capsule versus axis-aligned boxes.
///
/// Only boxes whose vertical extent overlaps the capsule's span block it (a floor slab under the feet or a beam
/// above the head never does; contact closer than kVerticalTolerance is ignored). Horizontal motion is resolved
/// in the XZ plane as circle versus rectangle; vertical motion passes through (no gravity in M0). Every query
/// is deterministic: boxes are visited in insertion order.
class CollisionWorld {
public:
    /// Vertical overlap smaller than this (meters) does not count, so a capsule standing exactly on a floor
    /// slab or touching a beam with its head is not blocked by it.
    static constexpr float kVerticalTolerance = 1e-3f;
    /// Horizontal penetration up to this depth (meters) counts as touching, not overlapping.
    static constexpr float kContactTolerance = 1e-4f;
    /// Upper bound on the horizontal distance handled by one moveCapsule() call (longer requests are truncated
    /// along their direction); a walking controller moves a few centimeters per frame.
    static constexpr float kMaxDisplacement = 100.0f;

    /// Adds a collider. Empty boxes and boxes with non-finite corners are ignored.
    void addBox(const core::Aabb& box, std::string tag = {});
    void clear() noexcept;
    [[nodiscard]] std::span<const ColliderBox> boxes() const noexcept { return m_boxes; }

    /// True when the capsule standing at `feet` penetrates any collider by more than kContactTolerance.
    [[nodiscard]] bool overlaps(const glm::vec3& feet, const Capsule& capsule) const;

    /// Moves the capsule by `displacement` and returns the new feet position.
    ///
    /// The horizontal part is split into sub-steps no longer than radius / 2, so even a sprint step
    /// (4.5 m/s at dt 0.1 s) cannot tunnel through a 2 cm board. After each sub-step the capsule is pushed out
    /// of every blocking box along the minimum separation (circle versus rectangle), which makes it slide
    /// along walls and around corners instead of stopping dead. A capsule that starts inside a box is first
    /// pushed out the same way. If a sub-step cannot be resolved (a gap narrower than the capsule), the capsule
    /// stays where the previous sub-step left it. The vertical part is applied as is. An unobstructed move
    /// returns exactly feet + displacement.
    ///
    /// Invalid input never hangs or crashes: a non-finite `feet` is returned unchanged, a non-finite
    /// displacement is treated as zero, a non-finite capsule radius or height takes the Capsule default and one
    /// below 1 cm is raised to 1 cm (so walls still block a degenerate capsule).
    [[nodiscard]] glm::vec3 moveCapsule(const glm::vec3& feet, const glm::vec3& displacement,
                                        const Capsule& capsule) const;

private:
    std::vector<ColliderBox> m_boxes;
};

} // namespace rv::scene
