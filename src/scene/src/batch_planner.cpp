#include <rv/scene/batch_planner.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <tuple>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

auto batchKey(const RenderItem& item) noexcept {
    return std::tuple(item.shader, item.material, item.mesh);
}

auto batchKey(const Batch& batch) noexcept {
    return std::tuple(batch.shader, batch.material, batch.mesh);
}

/// Shared implementation; culling is enabled when `frustum` is set and `localBounds` is callable.
BatchPlan buildPlan(std::span<const RenderItem> items, const Frustum* frustum,
                    const std::function<core::Aabb(MeshHandle)>* localBounds) {
    std::vector<std::size_t> order;
    order.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (core::isFinite(items[index].model)) {
            order.push_back(index);
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [items](std::size_t a, std::size_t b) { return batchKey(items[a]) < batchKey(items[b]); });

    const bool culling = frustum != nullptr && localBounds != nullptr && static_cast<bool>(*localBounds);
    std::optional<MeshHandle> boundsMesh;
    core::Aabb meshBounds;

    BatchPlan plan;
    plan.instanceTransforms.reserve(order.size());
    for (const std::size_t index : order) {
        const RenderItem& item = items[index];
        if (culling) {
            if (boundsMesh != item.mesh) {
                meshBounds = (*localBounds)(item.mesh);
                boundsMesh = item.mesh;
            }
            if (!meshBounds.isEmpty() && !frustum->intersects(meshBounds.transformed(item.model))) {
                continue;
            }
        }
        if (plan.batches.empty() || batchKey(plan.batches.back()) != batchKey(item)) {
            plan.batches.push_back(Batch{item.shader, item.material, item.mesh,
                                         static_cast<std::uint32_t>(plan.instanceTransforms.size()), 0});
        }
        plan.instanceTransforms.push_back(item.model);
        ++plan.batches.back().instanceCount;
    }
    return plan;
}

} // namespace

BatchPlan BatchPlanner::plan(std::span<const RenderItem> items) const {
    return buildPlan(items, nullptr, nullptr);
}

BatchPlan BatchPlanner::plan(std::span<const RenderItem> items, const Frustum& frustum,
                             const std::function<core::Aabb(MeshHandle)>& localBounds) const {
    return buildPlan(items, &frustum, &localBounds);
}

} // namespace rv::scene
