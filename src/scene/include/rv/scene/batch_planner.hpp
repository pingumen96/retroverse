#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::scene {

/// One instanced draw: `instanceCount` copies of `mesh` with `material` in `pass`, whose model matrices are
/// BatchPlan::instanceTransforms[firstInstance, firstInstance + instanceCount).
struct Batch {
    DrawPass pass = DrawPass::Opaque;
    ShaderKind shader = ShaderKind::Pbr;
    MaterialHandle material;
    MeshHandle mesh;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
};

/// Draw list for one view: upload `instanceTransforms` once (one mat4 per instance), then issue one instanced draw
/// per batch in order.
struct BatchPlan {
    std::vector<Batch> batches;
    std::vector<glm::mat4> instanceTransforms;
    /// Items left out because their world bounds lie outside the view frustum.
    std::size_t culledItems = 0;
};

/// What BatchPlanner::plan knows about the view and the registered resources. The defaults plan every item as opaque,
/// without culling.
struct PlanOptions {
    /// View frustum: an item is culled when the world bounds of its mesh (localBounds under the item's model matrix)
    /// lie entirely outside it. Nothing: no culling.
    std::optional<Frustum> frustum;
    /// Eye position in world space: blended items are drawn from the farthest to the nearest.
    glm::vec3 eye{0.0f};
    /// Mesh-space bounds of a mesh. An empty box means "unknown": such an item is never culled and is sorted by the
    /// origin of its model matrix. An empty function leaves every mesh unknown. Called once per run of consecutive
    /// items sharing a mesh in key order.
    std::function<core::Aabb(MeshHandle)> localBounds;
    /// Pass of a material (drawPassOf its alpha mode). An empty function makes every item opaque. Called once per run
    /// of consecutive items sharing a material in submission order.
    std::function<DrawPass(MaterialHandle)> passOf;
};

/// Groups render items into instanced batches for one view (spec §14.4). GPU-free and deterministic:
///  - items with a non-finite model matrix are dropped, items outside PlanOptions::frustum are culled;
///  - batches come in pass order: opaque, alpha-masked, alpha-blended;
///  - opaque and masked items with the same (shader, material, mesh) form one batch; batches are ordered by that key,
///    which minimizes program and material switches, and instances keep their submission order (stable sort);
///  - blended items are drawn back to front: sorted by decreasing distance from PlanOptions::eye to the center of
///    their world bounds (ties in submission order); consecutive items with the same key share a batch, whose
///    instances are drawn in order, so the drawing order is exactly back to front.
class BatchPlanner {
public:
    [[nodiscard]] BatchPlan plan(std::span<const RenderItem> items, const PlanOptions& options = {}) const;
};

} // namespace rv::scene
