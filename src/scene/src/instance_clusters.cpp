#include <rv/scene/instance_clusters.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <fmt/format.h>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

/// Tolerance of the outline's disc test, in squared pixels: the shader compares the same float expression.
constexpr float kOutlineDiscTolerance = 1e-3f;

struct Candidate {
    std::uint32_t source = 0;
    DrawPass pass = DrawPass::Opaque;
};

auto keyOf(const StaticInstance& instance, DrawPass pass) noexcept {
    // Casters first within a mesh: false < true, so the key holds the negation.
    return std::tuple(pass, instance.shader, instance.material, instance.mesh, !instance.castsShadow);
}

auto keyOf(const StaticBatch& batch) noexcept {
    return std::tuple(batch.pass, batch.shader, batch.material, batch.mesh, !batch.castsShadow);
}

} // namespace

Expected<InstanceClusters> InstanceClusters::build(std::span<const StaticInstance> instances,
                                                   std::uint32_t clusterCount, const StaticGroupOptions& options) {
    if (instances.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return makeError(ErrorCode::OutOfRange, fmt::format("{} instances exceed 32-bit record indices", instances.size()),
                         "static instance group");
    }
    std::vector<Candidate> candidates;
    candidates.reserve(instances.size());
    std::optional<MaterialHandle> passMaterial;
    DrawPass materialPass = DrawPass::Opaque;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const StaticInstance& instance = instances[index];
        if (!core::isFinite(instance.model)) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("instance {} has a non-finite model matrix", index),
                             "static instance group");
        }
        if (instance.cluster >= clusterCount) {
            return makeError(ErrorCode::OutOfRange,
                             fmt::format("instance {} is in cluster {} of {}", index, instance.cluster, clusterCount),
                             "static instance group");
        }
        if (options.passOf && passMaterial != instance.material) {
            materialPass = options.passOf(instance.material);
            passMaterial = instance.material;
        }
        const DrawPass pass = options.passOf ? materialPass : DrawPass::Opaque;
        if (pass == DrawPass::Blend) {
            return makeError(ErrorCode::InvalidArgument,
                             fmt::format("instance {} has an alpha-blended material, which a static group cannot draw "
                                         "back to front",
                                         index),
                             "static instance group");
        }
        candidates.push_back(Candidate{static_cast<std::uint32_t>(index), pass});
    }
    // Batch key, then cluster, then source order: the source index makes the order total, so the result does not
    // depend on the sort's stability.
    std::sort(candidates.begin(), candidates.end(), [instances](const Candidate& a, const Candidate& b) {
        const auto keyA = keyOf(instances[a.source], a.pass);
        const auto keyB = keyOf(instances[b.source], b.pass);
        if (keyA != keyB) {
            return keyA < keyB;
        }
        const std::uint32_t clusterA = instances[a.source].cluster;
        const std::uint32_t clusterB = instances[b.source].cluster;
        return clusterA != clusterB ? clusterA < clusterB : a.source < b.source;
    });

    InstanceClusters layout;
    layout.m_clusters.resize(clusterCount);
    layout.m_instances.reserve(candidates.size());
    layout.m_meshBounds.reserve(candidates.size());
    layout.m_positions.resize(candidates.size());
    std::optional<MeshHandle> boundsMesh;
    core::Aabb meshBounds;
    for (const Candidate& candidate : candidates) {
        const StaticInstance& instance = instances[candidate.source];
        const auto position = static_cast<std::uint32_t>(layout.m_instances.size());
        if (layout.m_batches.empty() || keyOf(instance, candidate.pass) != keyOf(layout.m_batches.back())) {
            StaticBatch batch;
            batch.pass = candidate.pass;
            batch.shader = instance.shader;
            batch.material = instance.material;
            batch.mesh = instance.mesh;
            batch.castsShadow = instance.castsShadow;
            batch.firstInstance = position;
            batch.firstRange = static_cast<std::uint32_t>(layout.m_ranges.size());
            layout.m_batches.push_back(batch);
        }
        StaticBatch& batch = layout.m_batches.back();
        if (batch.rangeCount == 0 || layout.m_ranges.back().cluster != instance.cluster) {
            layout.m_ranges.push_back(ClusterRange{instance.cluster, position, 0});
            ++batch.rangeCount;
        }
        ++layout.m_ranges.back().instanceCount;
        ++batch.instanceCount;

        if (boundsMesh != instance.mesh) {
            meshBounds = options.localBounds ? options.localBounds(instance.mesh) : core::Aabb{};
            boundsMesh = instance.mesh;
        }
        Cluster& cluster = layout.m_clusters[instance.cluster];
        cluster.occupied = true;
        if (meshBounds.isEmpty()) {
            cluster.cullable = false;
        } else {
            const core::Aabb world = meshBounds.transformed(instance.model);
            cluster.bounds.expand(world);
            if (instance.castsShadow) {
                layout.m_casterBounds.expand(world);
            }
        }
        layout.m_meshBounds.push_back(meshBounds);
        layout.m_instances.push_back(InstanceData{instance.model, instance.attributes});
        layout.m_positions[candidate.source] = position;
        if ((instance.attributes.flags & instance_flags::kHighlighted) != 0) {
            layout.m_highlighted.push_back(position);
        }
    }
    layout.m_occupiedClusters = static_cast<std::size_t>(
        std::count_if(layout.m_clusters.begin(), layout.m_clusters.end(), [](const Cluster& c) { return c.occupied; }));
    return layout;
}

core::Aabb InstanceClusters::instanceBounds(std::uint32_t position) const {
    const core::Aabb& local = m_meshBounds.at(position);
    return local.isEmpty() ? core::Aabb{} : local.transformed(m_instances[position].model);
}

std::uint32_t InstanceClusters::batchOf(std::uint32_t position) const {
    if (position >= m_instances.size()) {
        throw std::out_of_range("record beyond the static group");
    }
    // The last batch that starts at or before the record.
    const auto after = std::upper_bound(m_batches.begin(), m_batches.end(), position,
                                        [](std::uint32_t value, const StaticBatch& batch) {
                                            return value < batch.firstInstance;
                                        });
    return static_cast<std::uint32_t>(std::distance(m_batches.begin(), after) - 1);
}

bool InstanceClusters::setAttributes(std::size_t sourceIndex, const InstanceAttributes& attributes) {
    const std::uint32_t position = m_positions.at(sourceIndex);
    InstanceAttributes& current = m_instances[position].attributes;
    if (current == attributes) {
        return false;
    }
    const bool wasHighlighted = (current.flags & instance_flags::kHighlighted) != 0;
    const bool highlighted = (attributes.flags & instance_flags::kHighlighted) != 0;
    current = attributes;
    if (wasHighlighted != highlighted) {
        const auto at = std::lower_bound(m_highlighted.begin(), m_highlighted.end(), position);
        if (highlighted) {
            m_highlighted.insert(at, position);
        } else {
            m_highlighted.erase(at);
        }
    }
    return true;
}

void InstanceClusters::plan(const Frustum* frustum, bool shadowCastersOnly, StaticDrawList& out) const {
    out.draws.clear();
    out.clusters = m_occupiedClusters;
    out.culledClusters = 0;
    out.instances = 0;
    out.visible.assign(m_clusters.size(), 1);
    if (frustum != nullptr) {
        for (std::size_t index = 0; index < m_clusters.size(); ++index) {
            const Cluster& cluster = m_clusters[index];
            if (!cluster.occupied) {
                out.visible[index] = 0;
            } else if (cluster.cullable && !frustum->intersects(cluster.bounds)) {
                out.visible[index] = 0;
                ++out.culledClusters;
            }
        }
    }
    for (std::size_t batchIndex = 0; batchIndex < m_batches.size(); ++batchIndex) {
        const StaticBatch& batch = m_batches[batchIndex];
        if (shadowCastersOnly && !batch.castsShadow) {
            continue;
        }
        bool open = false;
        for (std::uint32_t r = 0; r < batch.rangeCount; ++r) {
            const ClusterRange& range = m_ranges[batch.firstRange + r];
            if (out.visible[range.cluster] == 0) {
                open = false;
                continue;
            }
            // The ranges of a batch are back to back: a visible range right after a visible one extends its draw.
            if (open) {
                out.draws.back().instanceCount += range.instanceCount;
            } else {
                out.draws.push_back(StaticDraw{static_cast<std::uint32_t>(batchIndex), range.firstInstance,
                                               range.instanceCount});
                open = true;
            }
            out.instances += range.instanceCount;
        }
    }
}

float outlineWidthPixels(int height, float widthAt1080) noexcept {
    if (height <= 0 || !std::isfinite(widthAt1080) || widthAt1080 <= 0.0f) {
        return 0.0f;
    }
    return widthAt1080 * static_cast<float>(height) / 1080.0f;
}

int outlineReach(float width) noexcept {
    if (!std::isfinite(width) || width < 1.0f) {
        return 0;
    }
    return std::min(kOutlineMaxReach, static_cast<int>(std::floor(width + kOutlineDiscTolerance)));
}

bool outlineCovers(int dx, int dy, float width) noexcept {
    const auto distanceSquared = static_cast<float>(dx * dx + dy * dy);
    return distanceSquared <= width * width + kOutlineDiscTolerance;
}

std::optional<glm::ivec4> screenRect(const core::Aabb& bounds, const glm::mat4& viewProjection, glm::ivec2 size,
                                     int margin) noexcept {
    if (bounds.isEmpty() || !core::isFinite(bounds.min) || !core::isFinite(bounds.max) || size.x <= 0 ||
        size.y <= 0) {
        return std::nullopt;
    }
    glm::vec2 low(std::numeric_limits<float>::max());
    glm::vec2 high(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                              (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                              (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
        const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
        if (!(clip.w > 1e-6f)) {
            return glm::ivec4(0, 0, size.x, size.y); // reaches behind the eye: its projection is unbounded
        }
        // Window coordinates of the corner, pixels from the bottom-left.
        const glm::vec2 window = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * glm::vec2(size);
        low = glm::min(low, window);
        high = glm::max(high, window);
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(low.x)) - margin);
    const int y0 = std::max(0, static_cast<int>(std::floor(low.y)) - margin);
    const int x1 = std::min(size.x, static_cast<int>(std::ceil(high.x)) + margin);
    const int y1 = std::min(size.y, static_cast<int>(std::ceil(high.y)) + margin);
    if (x1 <= x0 || y1 <= y0) {
        return std::nullopt;
    }
    return glm::ivec4(x0, y0, x1 - x0, y1 - y0);
}

std::optional<glm::ivec4> unionRect(const std::optional<glm::ivec4>& a, const std::optional<glm::ivec4>& b) noexcept {
    if (!a) {
        return b;
    }
    if (!b) {
        return a;
    }
    const int x0 = std::min(a->x, b->x);
    const int y0 = std::min(a->y, b->y);
    const int x1 = std::max(a->x + a->z, b->x + b->z);
    const int y1 = std::max(a->y + a->w, b->y + b->w);
    return glm::ivec4(x0, y0, x1 - x0, y1 - y0);
}

} // namespace rv::scene
