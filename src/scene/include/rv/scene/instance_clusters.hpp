#pragma once

// The GPU-free half of the static instance groups (ADR-0011 §3, I3 step 8): instances that never move (the collection's
// cartridges: shells, front labels, spines) sorted once into batches per (pass, shader, material, mesh, shadow), each
// batch's instances ordered by cluster (a shelf level of a module, a display case), one world bounding box per cluster;
// per view only the clusters are culled and the visible ones turned into firstInstance/count ranges, so that no item is
// planned again frame after frame. gl::StaticInstanceGroup uploads the records once and draws the ranges. Also the
// geometry of the hover outline (ADR-0012 §3): its width at an output's height and the pixels a highlighted box covers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/instance_data.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::scene {

/// One instance of a static group as its owner lists it (the source order: InstanceClusters::positionOf maps it to its
/// record).
struct StaticInstance {
    MeshHandle mesh;
    MaterialHandle material;
    ShaderKind shader = ShaderKind::Pbr;
    /// Local -> world (meters, column vectors); finite.
    glm::mat4 model{1.0f};
    /// Drawn into the key light's shadow map (cartridge shells cast, their coplanar labels and spines do not).
    bool castsShadow = true;
    InstanceAttributes attributes;
    /// The cluster it is culled with, below the group's cluster count.
    std::uint32_t cluster = 0;
};

/// What InstanceClusters::build knows about the registered resources (as scene::PlanOptions).
struct StaticGroupOptions {
    /// Mesh-space bounds of a mesh; an empty box means unknown, and a cluster holding such an instance is never culled.
    /// An empty function leaves every mesh unknown.
    std::function<core::Aabb(MeshHandle)> localBounds;
    /// Pass of a material; an empty function makes every instance opaque. Alpha-blended materials are refused: their
    /// items must be drawn back to front, which a pre-sorted group cannot do.
    std::function<DrawPass(MaterialHandle)> passOf;
};

/// One pre-sorted batch: InstanceClusters::instances()[firstInstance, firstInstance + instanceCount), cluster by
/// cluster as ranges()[firstRange, firstRange + rangeCount) list them.
struct StaticBatch {
    DrawPass pass = DrawPass::Opaque;
    ShaderKind shader = ShaderKind::Pbr;
    MaterialHandle material;
    MeshHandle mesh;
    bool castsShadow = true;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstRange = 0;
    std::uint32_t rangeCount = 0;
};

/// The instances of one batch in one cluster, consecutive records.
struct ClusterRange {
    std::uint32_t cluster = 0;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
};

/// One instanced draw of a view: `instanceCount` records from `firstInstance` of batch `batch`.
struct StaticDraw {
    std::uint32_t batch = 0;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;

    bool operator==(const StaticDraw&) const = default;
};

/// What one view draws of a group (InstanceClusters::plan). Keep one per group and view kind: its vectors keep their
/// capacity, so planning allocates nothing once they have grown.
struct StaticDrawList {
    std::vector<StaticDraw> draws;
    /// Clusters that hold instances, and those of them outside the frustum.
    std::size_t clusters = 0;
    std::size_t culledClusters = 0;
    /// Instances the draws cover.
    std::size_t instances = 0;
    /// Scratch: one visibility byte per cluster.
    std::vector<std::uint8_t> visible;
};

/// The pre-sorted layout of a static instance group, built once (ADR-0011 §3):
///  - batches by key (pass, shader, material, mesh, castsShadow), in that order: opaque before alpha-masked, the same
///    program and material order as scene::BatchPlanner's, which keeps program and material switches minimal;
///  - within a batch, the instances by cluster, then in source order, so each cluster is one consecutive range;
///  - each cluster's world bounds: the union of its instances' mesh bounds under their model matrices.
/// Per view, plan() tests each cluster's bounds against the frustum once and emits, batch by batch, the maximal runs
/// of consecutive visible clusters as single draws: number the clusters so that neighbors in the room are neighbors in
/// the numbering (the levels of a module bottom to top, the modules along the wall) and a view draws a few ranges per
/// batch. Attributes (layers, flags) change in place (setAttributes); a change of the instances (a placement added, a
/// matrix moved) builds a new layout. Instances flagged instance_flags::kHidden stay in their batch: the vertex stage
/// draws nothing for them.
class InstanceClusters {
public:
    /// An empty layout (no instance, no cluster).
    InstanceClusters() = default;

    /// Sorts `instances` into batches and clusters. Errors: InvalidArgument for a non-finite model matrix or an
    /// alpha-blended material, OutOfRange for a cluster at or above `clusterCount` or more instances than 32-bit
    /// record indices hold.
    [[nodiscard]] static Expected<InstanceClusters> build(std::span<const StaticInstance> instances,
                                                          std::uint32_t clusterCount,
                                                          const StaticGroupOptions& options = {});

    /// The records in buffer order (each the instance's model matrix and attributes).
    [[nodiscard]] std::span<const InstanceData> instances() const noexcept { return m_instances; }
    [[nodiscard]] std::span<const StaticBatch> batches() const noexcept { return m_batches; }
    [[nodiscard]] std::span<const ClusterRange> ranges() const noexcept { return m_ranges; }
    [[nodiscard]] std::uint32_t clusterCount() const noexcept { return static_cast<std::uint32_t>(m_clusters.size()); }
    /// World bounds of a cluster (empty for one without instances; see clusterCullable()).
    [[nodiscard]] const core::Aabb& clusterBounds(std::uint32_t cluster) const { return m_clusters.at(cluster).bounds; }
    /// False for a cluster holding an instance of unknown bounds: it is drawn in every view.
    [[nodiscard]] bool clusterCullable(std::uint32_t cluster) const { return m_clusters.at(cluster).cullable; }
    /// Clusters that hold at least one instance.
    [[nodiscard]] std::size_t occupiedClusters() const noexcept { return m_occupiedClusters; }
    /// World bounds of the instances that cast shadows (known bounds only): a directional key light's map is fitted to
    /// them with the view's other casters.
    [[nodiscard]] const core::Aabb& casterBounds() const noexcept { return m_casterBounds; }
    /// World bounds of the record at `position` (empty when its mesh's bounds are unknown).
    [[nodiscard]] core::Aabb instanceBounds(std::uint32_t position) const;

    /// The record of the source instance `sourceIndex` (std::out_of_range beyond the instances built).
    [[nodiscard]] std::uint32_t positionOf(std::size_t sourceIndex) const { return m_positions.at(sourceIndex); }
    /// The batch that holds the record at `position` (std::out_of_range beyond the records).
    [[nodiscard]] std::uint32_t batchOf(std::uint32_t position) const;

    /// Replaces the attributes of source instance `sourceIndex`; true when they changed (the record must be written
    /// again), false when they were equal. std::out_of_range beyond the instances built.
    bool setAttributes(std::size_t sourceIndex, const InstanceAttributes& attributes);
    /// The records flagged instance_flags::kHighlighted, ascending: what the hover outline marks.
    [[nodiscard]] std::span<const std::uint32_t> highlighted() const noexcept { return m_highlighted; }

    /// The draws of one view into `out` (its previous content replaced): every batch, or with `shadowCastersOnly` the
    /// batches that cast shadows, restricted to the clusters whose bounds intersect `frustum` (all of them without a
    /// frustum), consecutive visible clusters of a batch merged into one draw. Counts the occupied clusters and those
    /// culled.
    void plan(const Frustum* frustum, bool shadowCastersOnly, StaticDrawList& out) const;

private:
    struct Cluster {
        core::Aabb bounds;
        bool cullable = true;
        bool occupied = false;
    };

    std::vector<InstanceData> m_instances;
    std::vector<StaticBatch> m_batches;
    std::vector<ClusterRange> m_ranges;
    std::vector<Cluster> m_clusters;
    std::size_t m_occupiedClusters = 0;
    /// Mesh-space bounds of each record's mesh (empty: unknown).
    std::vector<core::Aabb> m_meshBounds;
    core::Aabb m_casterBounds;
    /// Source index -> record.
    std::vector<std::uint32_t> m_positions;
    std::vector<std::uint32_t> m_highlighted;
};

// --- The hover outline (ADR-0012 §3) ---------------------------------------------------------------------------------

/// The outline's width at 1080 output rows, in pixels; it scales with the output's height.
inline constexpr float kOutlineWidthAt1080 = 2.0f;
/// The theme's warm white (style §3, "UI testo", #F4EFE6), written into the sRGB output as these bytes.
inline constexpr std::array<std::uint8_t, 3> kOutlineColorSrgb{0xF4, 0xEF, 0xE6};

/// The largest offset, in pixels along either axis, the edge pass reads (RV_OUTLINE_MAX_REACH of
/// outline/outline_edge.frag): widths beyond it draw as if they were this wide.
inline constexpr int kOutlineMaxReach = 8;

/// The outline's width in pixels for an output `height` rows tall: widthAt1080 * height / 1080 (by default 2 at 1080p,
/// 4 at 2160p, 1.33 at 720p); 0 for a height that is not positive or a width that is not positive and finite.
[[nodiscard]] float outlineWidthPixels(int height, float widthAt1080 = kOutlineWidthAt1080) noexcept;

/// A pixel outside the item is outline when a pixel inside it lies within `width`: some offset (dx, dy) with
/// dx^2 + dy^2 <= width^2 (outlineCovers). The largest |dx| or |dy| such an offset can have, floor(width) up to
/// kOutlineMaxReach (0 for a width below 1 or not finite), bounds the neighborhood the edge pass reads.
[[nodiscard]] int outlineReach(float width) noexcept;

/// Whether the offset (dx, dy) lies within `width` of the pixel: dx^2 + dy^2 <= width^2, with the tolerance of the edge
/// pass (outline/outline_edge.frag), so that the CPU and the shader pick the same pixels.
[[nodiscard]] bool outlineCovers(int dx, int dy, float width) noexcept;

/// The pixel rectangle {x, y, width, height} (origin at the bottom-left, as glScissor takes it) that `bounds` covers
/// seen through `viewProjection` in an output of `size`, grown by `margin` pixels on each side and clipped to the
/// output; the whole output when the box reaches behind the eye (some corner with w <= 0), nothing when it lies
/// outside the output or the box is empty or not finite.
[[nodiscard]] std::optional<glm::ivec4> screenRect(const core::Aabb& bounds, const glm::mat4& viewProjection,
                                                   glm::ivec2 size, int margin) noexcept;

/// The smallest rectangle holding both (either may be nothing).
[[nodiscard]] std::optional<glm::ivec4> unionRect(const std::optional<glm::ivec4>& a,
                                                  const std::optional<glm::ivec4>& b) noexcept;

} // namespace rv::scene
