#include <rv/scene/batch_planner.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <tuple>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

/// An item that takes part in the plan.
struct Candidate {
    std::size_t index = 0; ///< into the items
    DrawPass pass = DrawPass::Opaque;
    /// Blended items: squared distance from the eye to the center of the world bounds.
    float depth = 0.0f;
};

auto batchKey(DrawPass pass, const RenderItem& item) noexcept {
    return std::tuple(pass, item.shader, item.material, item.mesh);
}

auto batchKey(const Batch& batch) noexcept {
    return std::tuple(batch.pass, batch.shader, batch.material, batch.mesh);
}

/// Finite items with their pass, in submission order; passOf is called once per run of a material.
std::vector<Candidate> candidatesOf(std::span<const RenderItem> items, const PlanOptions& options) {
    std::vector<Candidate> candidates;
    candidates.reserve(items.size());
    std::optional<MaterialHandle> passMaterial;
    DrawPass materialPass = DrawPass::Opaque;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const RenderItem& item = items[index];
        if (!core::isFinite(item.model)) {
            continue;
        }
        if (options.passOf && passMaterial != item.material) {
            materialPass = options.passOf(item.material);
            passMaterial = item.material;
        }
        candidates.push_back(Candidate{index, options.passOf ? materialPass : DrawPass::Opaque, 0.0f});
    }
    return candidates;
}

} // namespace

BatchPlan BatchPlanner::plan(std::span<const RenderItem> items, const PlanOptions& options) const {
    std::vector<Candidate> candidates = candidatesOf(items, options);
    // Key order: pass, shader, material, mesh. Stable, so items with the same key keep their submission order.
    std::stable_sort(candidates.begin(), candidates.end(), [items](const Candidate& a, const Candidate& b) {
        return batchKey(a.pass, items[a.index]) < batchKey(b.pass, items[b.index]);
    });

    const glm::vec3 eye = core::isFinite(options.eye) ? options.eye : glm::vec3(0.0f);
    BatchPlan plan;
    std::vector<Candidate> kept;
    kept.reserve(candidates.size());
    std::optional<MeshHandle> boundsMesh;
    core::Aabb meshBounds;
    for (Candidate& candidate : candidates) {
        const RenderItem& item = items[candidate.index];
        const bool blended = candidate.pass == DrawPass::Blend;
        core::Aabb world;
        if ((options.frustum || blended) && options.localBounds) {
            if (boundsMesh != item.mesh) {
                meshBounds = options.localBounds(item.mesh);
                boundsMesh = item.mesh;
            }
            if (!meshBounds.isEmpty()) {
                world = meshBounds.transformed(item.model);
            }
        }
        if (options.frustum && !world.isEmpty() && !options.frustum->intersects(world)) {
            ++plan.culledItems;
            continue;
        }
        if (blended) {
            const glm::vec3 offset = (world.isEmpty() ? glm::vec3(item.model[3]) : world.center()) - eye;
            const float depth = glm::dot(offset, offset);
            candidate.depth = std::isnan(depth) ? 0.0f : depth;
        }
        kept.push_back(candidate);
    }

    // Blended items come last (largest pass): back to front, ties in submission order.
    const auto firstBlended =
        std::find_if(kept.begin(), kept.end(), [](const Candidate& c) { return c.pass == DrawPass::Blend; });
    std::sort(firstBlended, kept.end(), [](const Candidate& a, const Candidate& b) {
        return a.depth != b.depth ? a.depth > b.depth : a.index < b.index;
    });

    plan.instanceTransforms.reserve(kept.size());
    for (const Candidate& candidate : kept) {
        const RenderItem& item = items[candidate.index];
        if (plan.batches.empty() || batchKey(plan.batches.back()) != batchKey(candidate.pass, item)) {
            plan.batches.push_back(Batch{candidate.pass, item.shader, item.material, item.mesh,
                                         static_cast<std::uint32_t>(plan.instanceTransforms.size()), 0});
        }
        plan.instanceTransforms.push_back(item.model);
        ++plan.batches.back().instanceCount;
    }
    return plan;
}

} // namespace rv::scene
