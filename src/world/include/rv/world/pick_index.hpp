#pragma once

// Picking in the generated room (ADR-0012 §2; I3 step 7): which collection item a ray from the eye points at, and the
// hover that follows it without jitter. GPU-free: boxes and rays, no mesh.
//
// - Targets: one box per placed item, in the frame of its slot (local to world; the slot's +Z faces the viewer), its
//   visible face inflated by kFaceInflation on each side (faceTargetBox: a spine's target is 2.2 cm wide in its 2.4
//   cm pitch), grouped in clusters (a level of a module, a display case) whose bounds are tested first.
// - Occluders: world boxes that hide what lies behind them: the shelves' boards and sides, the walls, the furniture.
//   They are not the walking colliders (a module's collider is its whole box, which holds every slot): the caller
//   (step 10a's collectionLayout) lists the solid pieces. Glass is no occluder.
// - The ray reaches kMaxPickDistance (2.5 m) from its origin, the eye.
// - The hover (HoverTracker) changes only when the ray has been, for kHoverChangeFrames consecutive frames, off the
//   hovered item's target grown by kHysteresisMargin on each side and on another item's target shrunk by the same
//   margin (or on no target at all, to hover nothing): a still ray never changes it, and a ray jittered by 1 mm around
//   a hovered item keeps it.
//
// Every answer is deterministic: the nearest entry along the ray wins, equal distances go to the smaller PickId, and a
// target and an occluder at the same distance count the target as in front (a face flush with a board's edge).

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/math.hpp>

namespace rv::world {

/// A collection item as picking and the renderer know it: the pick id of its instance record
/// (scene::InstanceAttributes::pickId, I3 step 0c). 0 is no item.
using PickId = std::uint32_t;
inline constexpr PickId kNoPick = 0;

/// The farthest an item can be picked from the eye, in meters.
inline constexpr float kMaxPickDistance = 2.5f;
/// How much a target's visible face is inflated on each side, in meters (1 mm).
inline constexpr float kFaceInflation = 0.001f;
/// How much a target is shrunk to take the hover from another item, and grown to keep it, on each side (2 mm).
inline constexpr float kHysteresisMargin = 0.002f;
/// Consecutive frames the ray must spend on a new candidate before the hover changes to it.
inline constexpr int kHoverChangeFrames = 3;

/// One item's pick target.
struct PickTarget {
    PickId id = kNoPick;
    /// The cluster it is tested with (a level of a module, a display case: any number the caller chooses).
    std::uint32_t cluster = 0;
    /// Local to world: the slot's frame (any invertible affine transform; a rigid one keeps meters as meters).
    glm::mat4 frame{1.0f};
    /// The target in `frame`: the item's box with its visible face inflated (faceTargetBox).
    core::Aabb box;
};

/// `itemBox` (in a slot's frame, its visible face towards +Z) with that face inflated by kFaceInflation on each side:
/// grown along X and Y, its depth along Z kept. An empty box stays empty.
[[nodiscard]] core::Aabb faceTargetBox(const core::Aabb& itemBox) noexcept;

/// The nearest item a ray reaches.
struct PickHit {
    PickId id = kNoPick;
    /// From the ray's origin to where it enters the target, meters.
    float distance = 0.0f;

    bool operator==(const PickHit&) const = default;
};

/// What one ray tells the hover (PickIndex::sample).
struct HoverSample {
    /// The nearest target the ray enters within range, in front of every occluder; kNoPick when none.
    PickId hit = kNoPick;
    /// The ray also enters `hit`'s target shrunk by kHysteresisMargin, in front of every occluder.
    bool onCore = false;
    /// The ray enters the current hover's target grown by kHysteresisMargin, within range and in front of every
    /// occluder (false when there is no current hover).
    bool onCurrent = false;

    bool operator==(const HoverSample&) const = default;
};

/// The room's pick targets and occluders, built once per plan and asked every frame.
class PickIndex {
public:
    /// An empty index: no target, no occluder.
    PickIndex() = default;

    /// Validates and indexes. Errors (InvalidArgument, the target's id or the occluder's index as context): a target
    /// with id kNoPick, an id twice, an empty or non-finite box, a frame that is not a finite, invertible affine
    /// transform; an empty or non-finite occluder.
    [[nodiscard]] static Expected<PickIndex> build(std::span<const PickTarget> targets,
                                                   std::span<const core::Aabb> occluders);

    /// The nearest target `ray` enters within `maxDistance` of its origin, unless an occluder comes before it (then
    /// nothing: whatever lies behind the occluder is hidden too). The direction need not be unit; a zero or non-finite
    /// ray, or a `maxDistance` that is not positive and finite, hits nothing.
    [[nodiscard]] PickHit pick(const core::Ray& ray, float maxDistance = kMaxPickDistance) const noexcept;

    /// pick() with what the hover needs about `current` (the item hovered so far, kNoPick for none).
    [[nodiscard]] HoverSample sample(const core::Ray& ray, PickId current,
                                     float maxDistance = kMaxPickDistance) const noexcept;

    [[nodiscard]] bool contains(PickId id) const noexcept;
    /// The target of `id`, as it was given, or nullptr.
    [[nodiscard]] const PickTarget* target(PickId id) const noexcept;

    [[nodiscard]] std::size_t targetCount() const noexcept { return m_entries.size(); }
    [[nodiscard]] std::size_t clusterCount() const noexcept { return m_clusters.size(); }
    [[nodiscard]] std::size_t occluderCount() const noexcept { return m_occluders.size(); }

private:
    struct Entry {
        PickTarget target;
        glm::mat4 toLocal{1.0f};
        /// The target shrunk and grown by kHysteresisMargin (an axis thinner than twice the margin shrinks to its
        /// middle plane).
        core::Aabb core;
        core::Aabb grown;
    };
    struct Cluster {
        /// World bounds of its targets grown by the margin.
        core::Aabb bounds;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    struct PreparedRay;

    [[nodiscard]] const Entry* entry(PickId id) const noexcept;
    [[nodiscard]] PickHit nearest(const PreparedRay& ray) const noexcept;
    [[nodiscard]] bool occluded(const PreparedRay& ray, float distance) const noexcept;

    /// Grouped by cluster, the clusters in increasing cluster number, each in the order its targets were given.
    std::vector<Entry> m_entries;
    std::vector<Cluster> m_clusters;
    /// (id, index into m_entries), sorted by id.
    std::vector<std::pair<PickId, std::uint32_t>> m_byId;
    std::vector<core::Aabb> m_occluders;
};

/// The hover of ADR-0012 §2 over the frames' samples (see the file comment). Feed it one sample per frame in which a
/// ray was cast; frames without one change nothing.
class HoverTracker {
public:
    /// Takes this frame's sample; returns the hovered item after it.
    PickId update(const HoverSample& sample) noexcept;
    /// Hovers nothing and forgets any candidate.
    void reset() noexcept;
    /// Hovers `id` now and forgets any candidate.
    void force(PickId id) noexcept;

    [[nodiscard]] PickId hovered() const noexcept { return m_hovered; }
    /// The candidate the ray has been on, and for how many consecutive frames (0 when none).
    [[nodiscard]] PickId candidate() const noexcept { return m_candidate; }
    [[nodiscard]] int candidateFrames() const noexcept { return m_candidateFrames; }

private:
    void keep() noexcept;
    void propose(PickId id) noexcept;

    PickId m_hovered = kNoPick;
    PickId m_candidate = kNoPick;
    int m_candidateFrames = 0;
};

} // namespace rv::world
