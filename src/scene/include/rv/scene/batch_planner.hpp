#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::scene {

/// One instanced draw: `instanceCount` copies of `mesh` with `material`, whose model matrices are
/// BatchPlan::instanceTransforms[firstInstance, firstInstance + instanceCount).
struct Batch {
    ShaderKind shader = ShaderKind::Pbr;
    MaterialHandle material;
    MeshHandle mesh;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
};

/// Draw list for one view: upload `instanceTransforms` once (one mat4 per instance), then issue one instanced
/// draw per batch in order. Batches are sorted by (shader, material, mesh), which minimizes program and
/// material switches.
struct BatchPlan {
    std::vector<Batch> batches;
    std::vector<glm::mat4> instanceTransforms;
};

/// Groups render items into instanced batches (spec §14.4). GPU-free and deterministic: items with the same
/// (shader, material, mesh) key form one batch, batches are ordered by key, and instances keep their submission
/// order inside a batch (stable sort). Items with a non-finite model matrix are dropped.
class BatchPlanner {
public:
    [[nodiscard]] BatchPlan plan(std::span<const RenderItem> items) const;

    /// Same, with frustum culling: an item is kept when localBounds(mesh).transformed(model) intersects the
    /// frustum. An empty local box means "bounds unknown" and is never culled; an empty `localBounds` function
    /// disables culling. localBounds is called once per run of consecutive items sharing a mesh after sorting.
    [[nodiscard]] BatchPlan plan(std::span<const RenderItem> items, const Frustum& frustum,
                                 const std::function<core::Aabb(MeshHandle)>& localBounds) const;
};

} // namespace rv::scene
