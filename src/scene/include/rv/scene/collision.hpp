#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
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

/// Horizontal footprint of a collider turned about +Y (ColliderBox::turned): the rectangle centered on `center`
/// (world x, z) with half extents `halfExtents` along `axisX` and `axisZ`, the unit (x, z) directions of the box's own
/// X and Z axes.
struct TurnedFootprint {
    glm::vec2 center{0.0f};
    glm::vec2 halfExtents{0.0f};
    glm::vec2 axisX{1.0f, 0.0f};
    glm::vec2 axisZ{0.0f, 1.0f};
};

/// Static collider: an upright box that blocks between box.min.y and box.max.y. `tag` names the source (object id,
/// "wall_north", ...) for debugging.
struct ColliderBox {
    /// World-space box: the collider itself, or the box around it when it is turned.
    core::Aabb box;
    std::string tag;
    /// Exact footprint of a box turned about +Y by an angle that is not a multiple of 90 degrees, used instead of the
    /// XZ rectangle of `box`; nothing for an axis-aligned box.
    std::optional<TurnedFootprint> turned;
};

/// How far a capsule is from one collider (CollisionWorld::nearestBlocking).
struct ColliderDistance {
    /// Index into CollisionWorld::boxes().
    std::size_t index = 0;
    /// Horizontal distance in meters from the capsule axis (feet.xz) to the collider's footprint (the XZ rectangle of
    /// its box, or its turned footprint), 0 when the axis is inside it. A capsule resting against the box is `radius`
    /// away; distance - radius is the clearance.
    float distance = 0.0f;
};

/// Static collision geometry for first-person navigation (spec §14.3): capsule versus upright boxes, axis-aligned or
/// turned about +Y.
///
/// Only boxes whose vertical extent overlaps the capsule's span block it (a floor slab under the feet or a beam
/// above the head never does; contact closer than kVerticalTolerance is ignored). Horizontal motion is resolved
/// in the XZ plane as circle versus rectangle (in the rectangle's own axes for a turned box); vertical motion passes
/// through (no gravity in M0). Every query is deterministic: boxes are visited in insertion order.
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

    /// Adds an axis-aligned collider (world space). Empty boxes and boxes with non-finite corners are ignored.
    void addBox(const core::Aabb& box, std::string tag = {});
    /// Adds the object-local `box` placed by `transform` (local -> world). A transform that keeps the box upright
    /// (rotation about +Y only, any translation and scale) keeps its exact footprint: turned by an angle that is not
    /// a multiple of 90 degrees, the collider gets ColliderBox::turned. A tilted box (rotation about X or Z) becomes
    /// its world box, Aabb::transformed(transform), which also blocks some free space around it. Ignored when `box`
    /// is empty or not finite, or when the placed box is not finite.
    void addBox(const core::Aabb& box, const glm::mat4& transform, std::string tag = {});
    /// Adds a collider as it is (another world's: I4b step 4 keeps the colliders of the objects a room's rebuilt
    /// static part leaves as they were).
    void addCollider(ColliderBox collider) { m_boxes.push_back(std::move(collider)); }
    void clear() noexcept;
    /// The colliders taken out, the world left empty.
    [[nodiscard]] std::vector<ColliderBox> release() noexcept { return std::exchange(m_boxes, {}); }
    [[nodiscard]] std::span<const ColliderBox> boxes() const noexcept { return m_boxes; }

    /// True when the capsule standing at `feet` penetrates any collider by more than kContactTolerance.
    [[nodiscard]] bool overlaps(const glm::vec3& feet, const Capsule& capsule) const;

    /// The nearest collider among those that block the capsule standing at `feet` at its height (the vertical rule of
    /// moveCapsule); nothing when none does or `feet` is not finite. Ties keep the earliest box. For diagnostics, such
    /// as reporting how close a walker stopped to a wall.
    [[nodiscard]] std::optional<ColliderDistance> nearestBlocking(const glm::vec3& feet, const Capsule& capsule) const;

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
