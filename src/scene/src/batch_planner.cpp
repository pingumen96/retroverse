#include <rv/scene/batch_planner.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <tuple>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

auto batchKey(DrawPass pass, const RenderItem& item) noexcept {
    return std::tuple(pass, item.shader, item.material, item.mesh);
}

auto batchKey(const Batch& batch) noexcept {
    return std::tuple(batch.pass, batch.shader, batch.material, batch.mesh);
}

} // namespace

void BatchPlanner::plan(std::span<const RenderItem> items, const PlanOptions& options, BatchPlan& out) {
    out.batches.clear();
    out.instanceTransforms.clear();
    out.culledItems = 0;

    // Finite items with their pass, in submission order; passOf is called once per run of a material.
    m_candidates.clear();
    m_candidates.reserve(items.size());
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
        m_candidates.push_back(Candidate{index, options.passOf ? materialPass : DrawPass::Opaque, 0.0f});
    }

    // Key order: pass, shader, material, mesh. The submission index breaks ties, so items with the same key keep their
    // submission order (the order of a stable sort, without its temporary buffer).
    std::sort(m_candidates.begin(), m_candidates.end(), [items](const Candidate& a, const Candidate& b) {
        const auto keyA = batchKey(a.pass, items[a.index]);
        const auto keyB = batchKey(b.pass, items[b.index]);
        return keyA != keyB ? keyA < keyB : a.index < b.index;
    });

    // Culling, in place: the candidates that survive move to the front, in order.
    const glm::vec3 eye = core::isFinite(options.eye) ? options.eye : glm::vec3(0.0f);
    std::size_t kept = 0;
    std::optional<MeshHandle> boundsMesh;
    core::Aabb meshBounds;
    for (std::size_t position = 0; position < m_candidates.size(); ++position) {
        Candidate candidate = m_candidates[position];
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
            ++out.culledItems;
            continue;
        }
        if (blended) {
            const glm::vec3 offset = (world.isEmpty() ? glm::vec3(item.model[3]) : world.center()) - eye;
            const float depth = glm::dot(offset, offset);
            candidate.depth = std::isnan(depth) ? 0.0f : depth;
        }
        m_candidates[kept++] = candidate;
    }
    m_candidates.resize(kept);

    // Blended items come last (largest pass): back to front, ties in submission order.
    const auto firstBlended = std::find_if(m_candidates.begin(), m_candidates.end(),
                                           [](const Candidate& c) { return c.pass == DrawPass::Blend; });
    std::sort(firstBlended, m_candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.depth != b.depth ? a.depth > b.depth : a.index < b.index;
    });

    out.instanceTransforms.reserve(m_candidates.size());
    for (const Candidate& candidate : m_candidates) {
        const RenderItem& item = items[candidate.index];
        if (out.batches.empty() || batchKey(out.batches.back()) != batchKey(candidate.pass, item)) {
            out.batches.push_back(Batch{candidate.pass, item.shader, item.material, item.mesh,
                                        static_cast<std::uint32_t>(out.instanceTransforms.size()), 0});
        }
        out.instanceTransforms.push_back(item.model);
        ++out.batches.back().instanceCount;
    }
}

BatchPlan BatchPlanner::plan(std::span<const RenderItem> items, const PlanOptions& options) {
    BatchPlan result;
    plan(items, options, result);
    return result;
}

} // namespace rv::scene
