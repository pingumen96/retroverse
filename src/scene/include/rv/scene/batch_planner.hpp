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
#include <rv/scene/instance_data.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::scene {

/// One instanced draw: `instanceCount` copies of `mesh` with `material` in `pass`, whose instance records are
/// BatchPlan::instances[firstInstance, firstInstance + instanceCount).
struct Batch {
    DrawPass pass = DrawPass::Opaque;
    ShaderKind shader = ShaderKind::Pbr;
    MaterialHandle material;
    MeshHandle mesh;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
};

/// Draw list for one view: upload `instances` once (one InstanceData per instance: the item's model matrix and its
/// attributes), then issue one instanced draw per batch in order.
struct BatchPlan {
    std::vector<Batch> batches;
    std::vector<InstanceData> instances;
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
    /// Plans a shadow map: only the items that cast shadows (RenderItem::castsShadow) and are not alpha-blended take
    /// part (blended surfaces, glass, cast no shadow); the others are left out without counting as culled.
    bool shadowCastersOnly = false;
};

/// Groups render items into instanced batches for one view (spec §14.4). GPU-free and deterministic:
///  - items with a non-finite model matrix are dropped, and so are those that cast no shadow when planning a shadow
///    map (PlanOptions::shadowCastersOnly); items outside PlanOptions::frustum are culled;
///  - batches come in pass order: opaque, alpha-masked, alpha-blended;
///  - opaque and masked items with the same (shader, material, mesh) form one batch; batches are ordered by that key,
///    which minimizes program and material switches, and instances keep their submission order; each instance is the
///    item's model matrix with its attributes (RenderItem::attributes), which never split a batch;
///  - blended items are drawn back to front: sorted by decreasing distance from PlanOptions::eye to the center of
///    their world bounds (ties in submission order); consecutive items with the same key share a batch, whose
///    instances are drawn in order, so the drawing order is exactly back to front.
/// The planner keeps scratch space between calls: a renderer that keeps one planner and one BatchPlan plans every
/// view without allocating once both have grown to its largest view (spec §14.4: no allocations in the hot loop).
class BatchPlanner {
public:
    /// Plans `items` into `out`, replacing its content. The vectors of `out` keep their capacity.
    void plan(std::span<const RenderItem> items, const PlanOptions& options, BatchPlan& out);
    /// Plans `items` into a new plan.
    [[nodiscard]] BatchPlan plan(std::span<const RenderItem> items, const PlanOptions& options = {});

private:
    /// An item that takes part in the plan.
    struct Candidate {
        std::size_t index = 0; ///< into the items
        DrawPass pass = DrawPass::Opaque;
        /// Blended items: squared distance from the eye to the center of the world bounds.
        float depth = 0.0f;
    };

    /// Finite items with their pass, then the ones that survive culling, in plan order.
    std::vector<Candidate> m_candidates;
};

/// World bounds of what a shadow map can draw of `items`: the items a plan with PlanOptions::shadowCastersOnly keeps
/// before culling (a finite model matrix, castsShadow, not alpha-blended by `options.passOf`), each the bounds of its
/// mesh (`options.localBounds`; meshes of unknown bounds add nothing) under its model matrix. Empty when nothing casts.
/// The orthographic shadow map of a directional key light is fitted to them. The frustum, eye and shadowCastersOnly of
/// `options` are not used.
[[nodiscard]] core::Aabb shadowCasterBounds(std::span<const RenderItem> items, const PlanOptions& options);

} // namespace rv::scene
