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

/// A neighbour's portal behind the eye this far (meters, along the view) leaves its lights out; they are taken back
/// once it is less than kPortalBehindReadmitM behind.
constexpr float kPortalBehindDropM = 0.5f;
constexpr float kPortalBehindReadmitM = 0.25f;

/// How much farther an own light behind the eye's plane counts, per meter behind it (AreaLighting::choose()).
constexpr float kBehindEyeWeight = 2.0f;

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
    : m_areaLights(lightsByArea(lights, areas)), m_neighbours(areaNeighbours(areas)),
      m_lightArea(lights.size(), areas.size()), m_states(lights.size()), m_capacity(capacity),
      m_area(nearestArea(areas, start)) {
    for (std::size_t area = 0; area < m_areaLights.size(); ++area) {
        for (const std::uint32_t index : m_areaLights[area]) {
            m_lightArea[index] = area;
        }
    }
    choose(lights, start, glm::vec3(0.0f), false);
    (void)advance(lights, 1.0f);
}

std::vector<std::uint32_t> AreaLighting::chosen() const {
    std::vector<std::uint32_t> result;
    for (std::size_t index = 0; index < m_states.size(); ++index) {
        if (m_states[index].chosen) {
            result.push_back(static_cast<std::uint32_t>(index));
        }
    }
    return result;
}

bool AreaLighting::update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye,
                          const glm::vec3& forward, float seconds, bool instant) {
    if (empty()) {
        return false;
    }
    bool changed = false;
    if (const auto holding = areaAt(areas, eye); holding && holding != m_area) {
        m_area = holding;
        changed = true;
    }
    const float elapsed = std::isfinite(seconds) && seconds > 0.0f ? seconds : 0.0f;
    for (LightState& state : m_states) {
        state.chosenSeconds = state.chosen ? state.chosenSeconds + elapsed : 0.0f;
        state.leftSeconds = state.chosen ? 1e9f : std::min(state.leftSeconds + elapsed, 1e9f);
    }
    choose(lights, eye, forward, !instant);
    return advance(lights, instant ? 1.0f : elapsed / kLightFadeSeconds) || changed;
}

bool AreaLighting::update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye,
                          const glm::vec3& forward) {
    return update(areas, lights, eye, forward, 0.0f, true);
}

bool AreaLighting::reselect(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward) {
    if (empty()) {
        return false;
    }
    choose(lights, eye, forward, false);
    return advance(lights, 1.0f);
}

bool AreaLighting::behindEye(std::size_t index, const Light& light, const glm::vec3& eye,
                             const glm::vec3& forward) noexcept {
    LightState& state = m_states[index];
    const float ahead = glm::dot(light.position - eye, forward);
    // Left out once kBehindEyeMarginM past its range behind the eye, taken back once its range reaches the eye's plane.
    state.behind = state.behind ? ahead <= -light.range : ahead <= -(light.range + kBehindEyeMarginM);
    return state.behind;
}

void AreaLighting::choose(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward,
                          bool dwell) {
    m_priority.clear();
    m_key.reset();
    for (LightState& state : m_states) {
        state.wasChosen = state.chosen;
        state.chosen = false;
    }
    // A light left out a moment ago stays out (not at a cut).
    const auto resting = [&](std::uint32_t index) {
        return dwell && m_states[index].leftSeconds < kLightDwellSeconds;
    };
    if (!m_area || *m_area >= m_areaLights.size() || lights.size() != m_states.size()) {
        return;
    }
    const std::size_t area = *m_area;

    // The neighbours' lights that may be seen through a portal: how many (the reserve, whatever the view) and, of those
    // in front of the eye, their importance.
    m_candidates.clear();
    std::size_t eligible = 0;
    for (const AreaNeighbour& neighbour : m_neighbours[area]) {
        if (neighbour.area >= m_areaLights.size()) {
            continue;
        }
        // How far ahead of the eye its nearest portal lies (a doorway behind the eye shows nothing of the neighbour).
        float portalAhead = std::numeric_limits<float>::lowest();
        for (const core::Aabb& portal : neighbour.portals) {
            portalAhead = std::max(portalAhead, glm::dot(glm::clamp(eye, portal.min, portal.max) - eye, forward));
        }
        for (const std::uint32_t index : m_areaLights[neighbour.area]) {
            const Light& light = lights[index];
            if (!isPunctual(light)) {
                continue;
            }
            LightState& state = m_states[index];
            state.portalBehind = state.portalBehind ? portalAhead < -kPortalBehindReadmitM
                                                    : portalAhead < -kPortalBehindDropM;
            float nearest = std::numeric_limits<float>::infinity();
            glm::vec3 through(0.0f);
            for (const core::Aabb& portal : neighbour.portals) {
                const glm::vec3 point = glm::clamp(light.position, portal.min, portal.max);
                const float distance = glm::length(light.position - point);
                if (distance < nearest) {
                    nearest = distance;
                    through = point;
                }
            }
            if (!(nearest < kNeighbourReachM + light.range)) {
                continue;
            }
            ++eligible;
            if (behindEye(index, light, eye, forward) || state.portalBehind || resting(index)) {
                continue;
            }
            const glm::vec3 toPortal = through - eye;
            const float portalDistance = glm::length(toPortal);
            const float path = portalDistance + nearest;
            float importance = light.intensity / (1.0f + path * path);
            // Seen through a portal ahead: a doorway beside or behind the eye shows little of its light.
            if (glm::dot(forward, forward) > 0.0f && portalDistance > 1e-3f) {
                const float facing = std::clamp(glm::dot(toPortal / portalDistance, forward) / 0.7f, 0.0f, 1.0f);
                importance *= 0.1f + 0.9f * facing * facing * (3.0f - 2.0f * facing);
            }
            importance *= m_states[index].wasChosen ? kNeighbourLightStick : 1.0f;
            if (dwell && m_states[index].wasChosen && m_states[index].chosenSeconds < kLightDwellSeconds) {
                importance = std::numeric_limits<float>::max(); // chosen a moment ago: it stays
            }
            m_candidates.emplace_back(-importance, index);
        }
    }
    // The neighbours' share: kNeighbourLightReserve (at most half the capacity), or every slot the area's own point and
    // spot lights leave, whichever is more; never more than the neighbours' lights that may be seen. Counts, not the
    // view: the share stays as the eye turns.
    std::size_t ownPunctual = 0;
    for (const std::uint32_t index : m_areaLights[area]) {
        ownPunctual += isPunctual(lights[index]) ? 1 : 0;
    }
    const std::size_t share =
        std::max(std::min(kNeighbourLightReserve, m_capacity / 2), m_capacity - std::min(ownPunctual, m_capacity));
    const std::size_t reserve = std::min(share, eligible);
    const std::size_t neighbourCount = std::min(reserve, m_candidates.size());
    std::partial_sort(m_candidates.begin(), m_candidates.begin() + static_cast<std::ptrdiff_t>(neighbourCount),
                      m_candidates.end());
    m_neighbourPicks.clear();
    for (std::size_t i = 0; i < neighbourCount; ++i) {
        m_neighbourPicks.push_back(m_candidates[i].second);
    }

    // The area's own: its directional lights and its key light, then those nearest the eye in the rest, the share's
    // slots its neighbours cannot use (their lights wholly behind the eye) included: they are taken back as the
    // neighbours' lights come into view.
    std::size_t ownSlots = m_capacity - neighbourCount;
    m_candidates.clear();
    bool keySeen = false;
    for (const std::uint32_t index : m_areaLights[area]) {
        const Light& light = lights[index];
        const bool key = light.castsShadows && !keySeen;
        keySeen = keySeen || light.castsShadows;
        if (!isPunctual(light)) {
            m_states[index].chosen = true;
            m_key = key ? std::optional<std::uint32_t>(index) : m_key;
        } else if (key && ownSlots > 0) {
            m_states[index].chosen = true;
            m_priority.push_back(index);
            m_key = index;
            --ownSlots;
        } else if (!behindEye(index, light, eye, forward) && !resting(index)) {
            const LightState& state = m_states[index];
            const bool held = dwell && state.wasChosen && state.chosenSeconds < kLightDwellSeconds;
            // Nearest the eye first, a light behind the eye's plane counted farther by twice its distance behind it
            // (it lights less of the view than one as near in front).
            const glm::vec3 toLight = light.position - eye;
            const float behind = std::max(0.0f, -glm::dot(toLight, forward));
            const float distance = held ? std::numeric_limits<float>::lowest()
                                        : glm::length(toLight) + kBehindEyeWeight * behind -
                                              (state.wasChosen ? kOwnLightStickM : 0.0f);
            m_candidates.emplace_back(distance, index);
        }
    }
    const std::size_t ownCount = std::min(ownSlots, m_candidates.size());
    std::partial_sort(m_candidates.begin(), m_candidates.begin() + static_cast<std::ptrdiff_t>(ownCount),
                      m_candidates.end());
    for (std::size_t i = 0; i < ownCount; ++i) {
        m_states[m_candidates[i].second].chosen = true;
        m_priority.push_back(m_candidates[i].second);
    }
    for (const std::uint32_t index : m_neighbourPicks) {
        m_states[index].chosen = true;
        m_priority.push_back(index);
    }
    // The lights left out now rest (kLightDwellSeconds).
    for (LightState& state : m_states) {
        if (state.wasChosen && !state.chosen) {
            state.leftSeconds = 0.0f;
        }
    }
}

bool AreaLighting::advance(std::span<const Light> lights, float step) {
    if (lights.size() != m_states.size()) {
        return false; // not the list this was made with
    }
    const bool instant = step >= 1.0f;
    std::size_t packedPunctual = 0;
    m_fading = false;
    for (std::size_t index = 0; index < m_states.size(); ++index) {
        LightState& state = m_states[index];
        const bool punctual = isPunctual(lights[index]);
        if (instant || !punctual) {
            // Directional lights are never faded: one is packed at a time.
            state.packed = state.chosen;
            state.progress = state.chosen ? 1.0f : 0.0f;
        } else if (state.packed) {
            state.progress = state.chosen ? std::min(1.0f, state.progress + step) : std::max(0.0f, state.progress - step);
            state.packed = state.chosen || state.progress > 0.0f;
        }
        if (state.packed && punctual) {
            ++packedPunctual;
        }
    }
    // The lights chosen that are not packed yet, the most important first, while there are free slots; the key light
    // takes the slot of the faintest light fading out.
    for (const std::uint32_t index : m_priority) {
        LightState& state = m_states[index];
        if (state.packed) {
            continue;
        }
        if (packedPunctual >= m_capacity && m_key == index) {
            std::optional<std::size_t> faintest;
            for (std::size_t other = 0; other < m_states.size(); ++other) {
                const LightState& candidate = m_states[other];
                if (candidate.packed && !candidate.chosen && isPunctual(lights[other]) &&
                    (!faintest || candidate.progress < m_states[*faintest].progress)) {
                    faintest = other;
                }
            }
            if (faintest) {
                m_states[*faintest].packed = false;
                m_states[*faintest].progress = 0.0f;
                --packedPunctual;
            }
        }
        if (packedPunctual < m_capacity) {
            state.packed = true;
            state.progress = std::min(1.0f, step);
            ++packedPunctual;
        }
    }

    std::swap(m_previous, m_selection);
    std::swap(m_previousWeights, m_weights);
    std::swap(m_previousPortals, m_portals);
    m_selection.clear();
    m_weights.clear();
    m_portals.clear();
    const std::size_t area = m_area.value_or(m_areaLights.size());
    // A light of an area with a single portal (a room) is clipped to it; a neighbour's light to the portal it shares
    // with the eye's area nearest to it.
    const core::Aabb* ownPortal = nullptr;
    if (area < m_neighbours.size() && m_neighbours[area].size() == 1 && m_neighbours[area][0].portals.size() == 1) {
        ownPortal = &m_neighbours[area][0].portals[0];
    }
    const auto portalOf = [&](std::size_t index) {
        const std::size_t lightArea = m_lightArea[index];
        if (lightArea == area) {
            return ownPortal != nullptr ? *ownPortal : core::Aabb{};
        }
        if (area < m_neighbours.size()) {
            for (const AreaNeighbour& neighbour : m_neighbours[area]) {
                if (neighbour.area != lightArea) {
                    continue;
                }
                const core::Aabb* best = nullptr;
                float nearest = std::numeric_limits<float>::infinity();
                for (const core::Aabb& portal : neighbour.portals) {
                    const float distance = squaredDistance(portal, lights[index].position);
                    if (distance < nearest) {
                        nearest = distance;
                        best = &portal;
                    }
                }
                return best != nullptr ? *best : core::Aabb{};
            }
        }
        return core::Aabb{};
    };
    const auto push = [&](std::size_t index) {
        const LightState& state = m_states[index];
        const float t = state.progress;
        m_selection.push_back(static_cast<std::uint32_t>(index));
        m_weights.push_back(t * t * (3.0f - 2.0f * t));
        m_portals.push_back(portalOf(index));
        m_fading = m_fading || state.progress < 1.0f;
    };
    for (std::size_t index = 0; index < m_states.size(); ++index) {
        if (m_states[index].packed && m_lightArea[index] == area) {
            push(index);
        }
    }
    m_ownCount = m_selection.size();
    for (std::size_t index = 0; index < m_states.size(); ++index) {
        if (m_states[index].packed && m_lightArea[index] != area) {
            push(index);
        }
    }
    for (const std::uint32_t index : m_priority) {
        m_fading = m_fading || !m_states[index].packed;
    }
    if (m_key && !m_states[*m_key].packed) {
        m_key.reset();
    }
    return m_selection != m_previous || m_weights != m_previousWeights || m_portals != m_previousPortals;
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
           a.fillIntensity == b.fillIntensity && a.bloomStrength == b.bloomStrength;
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
    m_current.bloomStrength = mixed(m_from.bloomStrength, m_to.bloomStrength, t);
    return true;
}

} // namespace rv::scene
