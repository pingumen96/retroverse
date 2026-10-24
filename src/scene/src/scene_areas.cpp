#include <rv/scene/scene_areas.hpp>

#include <algorithm>
#include <cmath>
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

/// A point or spot light whose whole range lies behind the plane through `eye` across `forward` (a unit vector; zero
/// for none): it lights nothing in front of the eye.
bool behindEye(const Light& light, const glm::vec3& eye, const glm::vec3& forward) noexcept {
    return glm::dot(light.position - eye, forward) <= -light.range;
}

/// Boxes closer than this (meters) touch: a floor scene's coordinates are millimetres written as meters.
constexpr float kTouchEpsilonM = 1e-4f;

/// The common part of `a` and `b` when they touch or overlap over a face (at least two axes of extent), else nothing.
std::optional<core::Aabb> portalBetween(const core::Aabb& a, const core::Aabb& b) noexcept {
    const glm::vec3 low = glm::max(a.min, b.min);
    const glm::vec3 high = glm::min(a.max, b.max);
    int spanned = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const float extent = high[axis] - low[axis];
        if (extent < -kTouchEpsilonM) {
            return std::nullopt;
        }
        spanned += extent > kTouchEpsilonM ? 1 : 0;
    }
    if (spanned < 2) {
        return std::nullopt;
    }
    return core::Aabb::fromMinMax(low, glm::max(low, high));
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

std::vector<std::vector<AreaNeighbour>> areaNeighbours(std::span<const SceneArea> areas) {
    std::vector<std::vector<AreaNeighbour>> result(areas.size());
    for (std::size_t first = 0; first < areas.size(); ++first) {
        for (std::size_t second = first + 1; second < areas.size(); ++second) {
            std::vector<core::Aabb> portals;
            for (const core::Aabb& a : areas[first].boxes) {
                for (const core::Aabb& b : areas[second].boxes) {
                    if (const auto portal = portalBetween(a, b)) {
                        portals.push_back(*portal);
                    }
                }
            }
            if (!portals.empty()) {
                result[first].push_back(AreaNeighbour{second, portals});
                result[second].push_back(AreaNeighbour{first, std::move(portals)});
            }
        }
    }
    for (std::vector<AreaNeighbour>& list : result) {
        std::sort(list.begin(), list.end(),
                  [](const AreaNeighbour& a, const AreaNeighbour& b) { return a.area < b.area; });
    }
    return result;
}

AreaLighting::AreaLighting(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& start,
                           std::size_t capacity)
    : m_areaLights(lightsByArea(lights, areas)), m_neighbours(areaNeighbours(areas)), m_capacity(capacity),
      m_area(nearestArea(areas, start)) {
    select(lights, start, glm::vec3(0.0f), m_selection, m_ownCount);
}

bool AreaLighting::update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye,
                          const glm::vec3& forward) {
    if (empty()) {
        return false;
    }
    bool changed = false;
    if (const auto holding = areaAt(areas, eye); holding && holding != m_area) {
        m_area = holding;
        changed = true;
    }
    return reselect(lights, eye, forward) || changed;
}

bool AreaLighting::reselect(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward) {
    if (empty()) {
        return false;
    }
    std::size_t ownCount = 0;
    select(lights, eye, forward, m_next, ownCount);
    if (m_next == m_selection && ownCount == m_ownCount) {
        return false;
    }
    std::swap(m_next, m_selection);
    m_ownCount = ownCount;
    return true;
}

void AreaLighting::select(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward,
                          std::vector<std::uint32_t>& out, std::size_t& ownCount) {
    out.clear();
    ownCount = 0;
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
        } else if (!behindEye(light, eye, forward)) {
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
    punctual += taken;
    std::sort(out.begin(), out.end());
    ownCount = out.size();

    // The neighbours' point and spot lights near the portals they share, while there is room: up to
    // kNeighbourLightsPerArea of each (those nearest a portal), the nearest the eye first.
    const std::size_t left = m_capacity - std::min(m_capacity, punctual);
    if (left == 0 || *m_area >= m_neighbours.size()) {
        return;
    }
    m_neighbourPicks.clear();
    for (const AreaNeighbour& neighbour : m_neighbours[*m_area]) {
        if (neighbour.area >= m_areaLights.size()) {
            continue;
        }
        m_neighbourDistances.clear();
        for (const std::uint32_t index : m_areaLights[neighbour.area]) {
            if (index >= lights.size() || !isPunctual(lights[index]) || behindEye(lights[index], eye, forward)) {
                continue;
            }
            const Light& light = lights[index];
            float nearest = std::numeric_limits<float>::infinity();
            for (const core::Aabb& portal : neighbour.portals) {
                nearest = std::min(nearest, squaredDistance(portal, light.position));
            }
            const float reach = std::min(kNeighbourReachM, light.range);
            if (std::isfinite(nearest) && nearest < reach * reach) {
                m_neighbourDistances.emplace_back(nearest, index);
            }
        }
        const std::size_t kept = std::min(kNeighbourLightsPerArea, m_neighbourDistances.size());
        std::partial_sort(m_neighbourDistances.begin(),
                          m_neighbourDistances.begin() + static_cast<std::ptrdiff_t>(kept),
                          m_neighbourDistances.end());
        for (std::size_t i = 0; i < kept; ++i) {
            const std::uint32_t index = m_neighbourDistances[i].second;
            const glm::vec3 delta = lights[index].position - eye;
            m_neighbourPicks.emplace_back(glm::dot(delta, delta), index);
        }
    }
    const std::size_t fitted = std::min(left, m_neighbourPicks.size());
    if (fitted < m_neighbourPicks.size()) {
        std::partial_sort(m_neighbourPicks.begin(), m_neighbourPicks.begin() + static_cast<std::ptrdiff_t>(fitted),
                          m_neighbourPicks.end());
    }
    for (std::size_t i = 0; i < fitted; ++i) {
        out.push_back(m_neighbourPicks[i].second);
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(ownCount), out.end());
}

namespace {

glm::vec3 mixed(const glm::vec3& from, const glm::vec3& to, float t) noexcept {
    return from + (to - from) * t;
}

float mixed(float from, float to, float t) noexcept {
    return from + (to - from) * t;
}

} // namespace

bool sameAreaTerms(const EnvironmentSetup& a, const EnvironmentSetup& b) noexcept {
    return a.exposure == b.exposure && a.ambientSky == b.ambientSky && a.ambientGround == b.ambientGround &&
           a.ambientIntensity == b.ambientIntensity && a.fillColor == b.fillColor &&
           a.fillIntensity == b.fillIntensity;
}

void EnvironmentEase::retarget(const EnvironmentSetup& target, bool instant) {
    if (instant || sameAreaTerms(m_current, target)) {
        snap(target);
        return;
    }
    m_from = m_current;
    m_to = target;
    m_progress = 0.0f;
}

void EnvironmentEase::snap(const EnvironmentSetup& environment) {
    m_from = environment;
    m_to = environment;
    m_current = environment;
    m_progress = 1.0f;
}

bool EnvironmentEase::advance(float seconds) {
    if (m_progress >= 1.0f || !std::isfinite(seconds) || seconds < 0.0f) {
        return false;
    }
    m_progress = std::min(1.0f, m_progress + seconds / kAreaEnvironmentEaseSeconds);
    if (m_progress >= 1.0f) {
        m_current = m_to;
        return true;
    }
    const float t = m_progress * m_progress * (3.0f - 2.0f * m_progress);
    m_current = m_to;
    m_current.exposure = mixed(m_from.exposure, m_to.exposure, t);
    m_current.ambientSky = mixed(m_from.ambientSky, m_to.ambientSky, t);
    m_current.ambientGround = mixed(m_from.ambientGround, m_to.ambientGround, t);
    m_current.ambientIntensity = mixed(m_from.ambientIntensity, m_to.ambientIntensity, t);
    m_current.fillColor = mixed(m_from.fillColor, m_to.fillColor, t);
    m_current.fillIntensity = mixed(m_from.fillIntensity, m_to.fillIntensity, t);
    return true;
}

} // namespace rv::scene
