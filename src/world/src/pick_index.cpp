#include <rv/world/pick_index.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <string>

#include <fmt/format.h>

namespace rv::world {

namespace {

/// Below this a direction component counts as parallel to its slab (core::Aabb::intersectRay's rule).
constexpr float kParallel = 1e-12f;

[[nodiscard]] bool isFiniteBox(const core::Aabb& box) noexcept {
    return !box.isEmpty() && core::isFinite(box.min) && core::isFinite(box.max);
}

/// An affine, invertible, finite transform.
[[nodiscard]] bool isUsableFrame(const glm::mat4& frame) noexcept {
    if (!core::isFinite(frame)) {
        return false;
    }
    // Column-major: frame[column][row]; the bottom row of an affine transform is (0, 0, 0, 1).
    if (frame[0][3] != 0.0f || frame[1][3] != 0.0f || frame[2][3] != 0.0f || frame[3][3] != 1.0f) {
        return false;
    }
    const float determinant = glm::determinant(glm::mat3(frame));
    return std::isfinite(determinant) && std::fabs(determinant) > 1e-12f;
}

/// `box` grown (positive `amount`) or shrunk on each side; an axis thinner than twice a shrink becomes its middle
/// plane rather than an inverted (empty) box.
[[nodiscard]] core::Aabb offsetBox(const core::Aabb& box, float amount) noexcept {
    core::Aabb result = box;
    for (int axis = 0; axis < 3; ++axis) {
        if (box.max[axis] - box.min[axis] >= -2.0f * amount) {
            result.min[axis] = box.min[axis] - amount;
            result.max[axis] = box.max[axis] + amount;
        } else {
            const float middle = 0.5f * (box.min[axis] + box.max[axis]);
            result.min[axis] = middle;
            result.max[axis] = middle;
        }
    }
    return result;
}

} // namespace

core::Aabb faceTargetBox(const core::Aabb& itemBox) noexcept {
    if (itemBox.isEmpty()) {
        return itemBox;
    }
    core::Aabb box = itemBox;
    box.min.x -= kFaceInflation;
    box.max.x += kFaceInflation;
    box.min.y -= kFaceInflation;
    box.max.y += kFaceInflation;
    return box;
}

/// A ray made ready for many slab tests: a unit direction, its inverse, the range.
struct PickIndex::PreparedRay {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    glm::vec3 inverse{0.0f};
    std::array<bool, 3> parallel{};
    float maxDistance = 0.0f;

    /// Nothing for a ray that cannot hit anything.
    [[nodiscard]] static std::optional<PreparedRay> from(const core::Ray& ray, float maxDistance) noexcept {
        if (!core::isFinite(ray.origin) || !core::isFinite(ray.direction) || !std::isfinite(maxDistance) ||
            maxDistance <= 0.0f) {
            return std::nullopt;
        }
        const float length = glm::length(ray.direction);
        if (!std::isfinite(length) || length <= 0.0f) {
            return std::nullopt;
        }
        PreparedRay prepared;
        prepared.origin = ray.origin;
        prepared.direction = ray.direction / length;
        for (int axis = 0; axis < 3; ++axis) {
            prepared.parallel[static_cast<std::size_t>(axis)] = std::fabs(prepared.direction[axis]) < kParallel;
            prepared.inverse[axis] = prepared.parallel[static_cast<std::size_t>(axis)]
                                         ? 0.0f
                                         : 1.0f / prepared.direction[axis];
        }
        prepared.maxDistance = maxDistance;
        return prepared;
    }

    /// Where the ray enters the world box `box` within [0, tMax] (0 when the origin is inside), or nothing.
    [[nodiscard]] std::optional<float> enter(const core::Aabb& box, float tMax) const noexcept {
        float tEnter = 0.0f;
        float tExit = tMax;
        for (int axis = 0; axis < 3; ++axis) {
            if (parallel[static_cast<std::size_t>(axis)]) {
                if (origin[axis] < box.min[axis] || origin[axis] > box.max[axis]) {
                    return std::nullopt;
                }
                continue;
            }
            float t0 = (box.min[axis] - origin[axis]) * inverse[axis];
            float t1 = (box.max[axis] - origin[axis]) * inverse[axis];
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            tEnter = std::max(tEnter, t0);
            tExit = std::min(tExit, t1);
            if (tEnter > tExit) {
                return std::nullopt;
            }
        }
        return tEnter;
    }

    /// Where the ray enters `box`, given in the frame `toLocal` maps the world into, within range. The ray's parameter
    /// is kept by the affine map, so the answer is meters along the world ray.
    [[nodiscard]] std::optional<float> enterLocal(const glm::mat4& toLocal, const core::Aabb& box) const noexcept {
        const core::Ray local{glm::vec3(toLocal * glm::vec4(origin, 1.0f)), glm::mat3(toLocal) * direction};
        return box.intersectRay(local, maxDistance);
    }
};

Expected<PickIndex> PickIndex::build(std::span<const PickTarget> targets, std::span<const core::Aabb> occluders) {
    PickIndex index;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const PickTarget& target = targets[i];
        const std::string context = fmt::format("target {}", target.id);
        if (target.id == kNoPick) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("pick target {} has no id", i));
        }
        if (!isFiniteBox(target.box)) {
            return makeError(ErrorCode::InvalidArgument, "the pick target's box is empty or not finite", context);
        }
        if (!isUsableFrame(target.frame)) {
            return makeError(ErrorCode::InvalidArgument,
                             "the pick target's frame is not a finite, invertible affine transform", context);
        }
    }
    for (std::size_t i = 0; i < occluders.size(); ++i) {
        if (!isFiniteBox(occluders[i])) {
            return makeError(ErrorCode::InvalidArgument, "the occluder is empty or not finite",
                             fmt::format("occluder {}", i));
        }
    }

    // Grouped by cluster, the targets of one cluster in the order they came.
    std::vector<std::uint32_t> order(targets.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return targets[a].cluster < targets[b].cluster; });
    index.m_entries.reserve(targets.size());
    for (const std::uint32_t source : order) {
        const PickTarget& target = targets[source];
        Entry entry;
        entry.target = target;
        entry.toLocal = glm::inverse(target.frame);
        entry.core = offsetBox(target.box, -kHysteresisMargin);
        entry.grown = offsetBox(target.box, kHysteresisMargin);
        const core::Aabb worldBounds = entry.grown.transformed(target.frame);
        const auto entryIndex = static_cast<std::uint32_t>(index.m_entries.size());
        if (index.m_clusters.empty() ||
            index.m_entries[index.m_clusters.back().first].target.cluster != target.cluster) {
            index.m_clusters.push_back(Cluster{core::Aabb{}, entryIndex, 0});
        }
        Cluster& cluster = index.m_clusters.back();
        cluster.bounds.expand(worldBounds);
        ++cluster.count;
        index.m_entries.push_back(entry);
        index.m_byId.emplace_back(target.id, entryIndex);
    }
    std::sort(index.m_byId.begin(), index.m_byId.end());
    const auto twice = std::adjacent_find(index.m_byId.begin(), index.m_byId.end(),
                                          [](const auto& a, const auto& b) { return a.first == b.first; });
    if (twice != index.m_byId.end()) {
        return makeError(ErrorCode::InvalidArgument, "two pick targets share an id",
                         fmt::format("target {}", twice->first));
    }
    index.m_occluders.assign(occluders.begin(), occluders.end());
    return index;
}

const PickIndex::Entry* PickIndex::entry(PickId id) const noexcept {
    if (id == kNoPick) {
        return nullptr;
    }
    const auto found = std::lower_bound(m_byId.begin(), m_byId.end(), id,
                                        [](const auto& pair, PickId value) { return pair.first < value; });
    if (found == m_byId.end() || found->first != id) {
        return nullptr;
    }
    return &m_entries[found->second];
}

bool PickIndex::contains(PickId id) const noexcept {
    return entry(id) != nullptr;
}

const PickTarget* PickIndex::target(PickId id) const noexcept {
    const Entry* found = entry(id);
    return found ? &found->target : nullptr;
}

bool PickIndex::occluded(const PreparedRay& ray, float distance) const noexcept {
    for (const core::Aabb& occluder : m_occluders) {
        if (const auto t = ray.enter(occluder, distance); t && *t < distance) {
            return true;
        }
    }
    return false;
}

PickHit PickIndex::nearest(const PreparedRay& ray) const noexcept {
    PickHit best{kNoPick, std::numeric_limits<float>::infinity()};
    for (const Cluster& cluster : m_clusters) {
        // The cluster's bounds hold its targets: entered beyond the best, none of them can be nearer.
        const auto clusterEntry = ray.enter(cluster.bounds, ray.maxDistance);
        if (!clusterEntry || *clusterEntry > best.distance) {
            continue;
        }
        for (std::uint32_t i = cluster.first; i < cluster.first + cluster.count; ++i) {
            const Entry& candidate = m_entries[i];
            const auto t = ray.enterLocal(candidate.toLocal, candidate.target.box);
            if (t && (*t < best.distance || (*t == best.distance && candidate.target.id < best.id))) {
                best = PickHit{candidate.target.id, *t};
            }
        }
    }
    if (best.id == kNoPick || occluded(ray, best.distance)) {
        return PickHit{};
    }
    return best;
}

PickHit PickIndex::pick(const core::Ray& ray, float maxDistance) const noexcept {
    const auto prepared = PreparedRay::from(ray, maxDistance);
    return prepared ? nearest(*prepared) : PickHit{};
}

HoverSample PickIndex::sample(const core::Ray& ray, PickId current, float maxDistance) const noexcept {
    HoverSample sample;
    const auto prepared = PreparedRay::from(ray, maxDistance);
    if (!prepared) {
        return sample;
    }
    sample.hit = nearest(*prepared).id;
    if (const Entry* hit = entry(sample.hit)) {
        const auto t = prepared->enterLocal(hit->toLocal, hit->core);
        sample.onCore = t && !occluded(*prepared, *t);
    }
    if (const Entry* hovered = entry(current)) {
        const auto t = prepared->enterLocal(hovered->toLocal, hovered->grown);
        sample.onCurrent = t && !occluded(*prepared, *t);
    }
    return sample;
}

PickId HoverTracker::update(const HoverSample& sample) noexcept {
    if (sample.hit != kNoPick && sample.hit == m_hovered) {
        keep();
    } else if (m_hovered != kNoPick && sample.onCurrent) {
        // Within the margin of the hovered item: between targets, or slanting past its edge into a neighbour's side.
        keep();
    } else if (sample.hit != kNoPick && sample.onCore) {
        propose(sample.hit);
    } else if (sample.hit != kNoPick) {
        // On the rim of another item's target: not enough to take the hover, nor to drop it.
        keep();
    } else {
        propose(kNoPick);
    }
    return m_hovered;
}

void HoverTracker::reset() noexcept {
    force(kNoPick);
}

void HoverTracker::force(PickId id) noexcept {
    m_hovered = id;
    keep();
}

void HoverTracker::keep() noexcept {
    m_candidate = kNoPick;
    m_candidateFrames = 0;
}

void HoverTracker::propose(PickId id) noexcept {
    if (id == m_hovered) {
        keep();
        return;
    }
    if (m_candidateFrames > 0 && id == m_candidate) {
        ++m_candidateFrames;
    } else {
        m_candidate = id;
        m_candidateFrames = 1;
    }
    if (m_candidateFrames >= kHoverChangeFrames) {
        m_hovered = id;
        keep();
    }
}

} // namespace rv::world
