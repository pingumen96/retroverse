#include <rv/scene/scene_areas.hpp>

#include <algorithm>
#include <limits>

#include <glm/glm.hpp>

namespace rv::scene {
namespace {

/// The squared distance from `point` to `box` (0 inside it).
float squaredDistance(const core::Aabb& box, const glm::vec3& point) noexcept {
    const glm::vec3 nearest = glm::clamp(point, box.min, box.max);
    const glm::vec3 delta = point - nearest;
    return glm::dot(delta, delta);
}

bool isPunctual(const Light& light) noexcept {
    return light.type != LightType::Directional;
}

} // namespace

std::optional<std::size_t> areaAt(std::span<const SceneArea> areas, const glm::vec3& point) noexcept {
    for (std::size_t i = 0; i < areas.size(); ++i) {
        if (areas[i].contains(point)) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> nearestArea(std::span<const SceneArea> areas, const glm::vec3& point) noexcept {
    if (const auto holding = areaAt(areas, point)) {
        return holding;
    }
    std::optional<std::size_t> nearest;
    float best = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < areas.size(); ++i) {
        for (const core::Aabb& box : areas[i].boxes) {
            const float distance = squaredDistance(box, point);
            if (!nearest || distance < best) {
                best = distance;
                nearest = i;
            }
        }
    }
    return nearest;
}

std::vector<std::vector<std::uint32_t>> lightsByArea(std::span<const Light> lights, std::span<const SceneArea> areas) {
    std::vector<std::vector<std::uint32_t>> result(areas.size());
    for (std::size_t index = 0; index < lights.size(); ++index) {
        for (std::size_t area = 0; area < areas.size(); ++area) {
            if (areas[area].id == lights[index].area) {
                result[area].push_back(static_cast<std::uint32_t>(index));
                break;
            }
        }
    }
    return result;
}

AreaLighting::AreaLighting(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& start,
                           std::size_t capacity)
    : m_areaLights(lightsByArea(lights, areas)), m_capacity(capacity), m_area(nearestArea(areas, start)) {
    select(lights, start, m_selection);
}

bool AreaLighting::update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye) {
    if (empty()) {
        return false;
    }
    bool changed = false;
    if (const auto holding = areaAt(areas, eye); holding && holding != m_area) {
        m_area = holding;
        changed = true;
    }
    return reselect(lights, eye) || changed;
}

bool AreaLighting::reselect(std::span<const Light> lights, const glm::vec3& eye) {
    if (empty()) {
        return false;
    }
    select(lights, eye, m_next);
    if (m_next == m_selection) {
        return false;
    }
    std::swap(m_next, m_selection);
    return true;
}

void AreaLighting::select(std::span<const Light> lights, const glm::vec3& eye, std::vector<std::uint32_t>& out) {
    out.clear();
    if (!m_area || *m_area >= m_areaLights.size()) {
        return;
    }
    const std::vector<std::uint32_t>& candidates = m_areaLights[*m_area];
    // The directional lights and the key light first; then the nearest point and spot lights while there is room.
    std::size_t punctual = 0;
    bool keySeen = false;
    m_distances.clear();
    for (const std::uint32_t index : candidates) {
        if (index >= lights.size()) {
            continue;
        }
        const Light& light = lights[index];
        const bool key = light.castsShadows && !keySeen;
        keySeen = keySeen || light.castsShadows;
        if (!isPunctual(light)) {
            out.push_back(index);
        } else if (key && punctual < m_capacity) {
            out.push_back(index);
            ++punctual;
        } else {
            const glm::vec3 delta = light.position - eye;
            m_distances.emplace_back(glm::dot(delta, delta), index);
        }
    }
    const std::size_t room = m_capacity - std::min(m_capacity, punctual);
    const std::size_t taken = std::min(room, m_distances.size());
    if (taken < m_distances.size()) {
        std::partial_sort(m_distances.begin(), m_distances.begin() + static_cast<std::ptrdiff_t>(taken),
                          m_distances.end());
    }
    for (std::size_t i = 0; i < taken; ++i) {
        out.push_back(m_distances[i].second);
    }
    std::sort(out.begin(), out.end());
}

} // namespace rv::scene
