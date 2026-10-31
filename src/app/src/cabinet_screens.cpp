#include <rv/app/cabinet_screens.hpp>

#include <cmath>
#include <utility>

#include <fmt/format.h>
#include <glm/geometric.hpp>

namespace rv::app {
namespace {

/// The simulated times are sums of fixed steps: a dwell of exactly 1 s ends on its frame.
constexpr double kDwellTolerance = 1e-6;

/// The candidate's distance from `eye` when it qualifies for attract (looser for the one playing), else none.
std::optional<double> attractDistance(const CabinetCandidate& candidate, const glm::vec3& eye, bool playing) {
    if (!candidate.visible) {
        return std::nullopt;
    }
    const glm::vec3 toEye = eye - candidate.center;
    const double distance = glm::length(toEye);
    if (!std::isfinite(distance) || distance > (playing ? kAttractLeaveMeters : kAttractMeters)) {
        return std::nullopt;
    }
    const double normalLength = glm::length(candidate.normal);
    if (distance <= 0.0 || normalLength <= 0.0) {
        return std::nullopt;
    }
    const double facing = glm::dot(toEye, candidate.normal) / (distance * normalLength);
    if (facing < (playing ? kAttractLeaveCosine : kAttractFacingCosine)) {
        return std::nullopt;
    }
    return distance;
}

} // namespace

std::optional<world::PickId> nearestAttract(std::span<const CabinetCandidate> candidates, const glm::vec3& eye,
                                            world::PickId current) {
    std::optional<world::PickId> best;
    double bestDistance = 0.0;
    for (const CabinetCandidate& candidate : candidates) {
        if (candidate.pick == world::kNoPick) {
            continue;
        }
        const std::optional<double> distance =
            attractDistance(candidate, eye, current != world::kNoPick && candidate.pick == current);
        if (!distance) {
            continue;
        }
        if (!best || *distance < bestDistance || (*distance == bestDistance && candidate.pick < *best)) {
            best = candidate.pick;
            bestDistance = *distance;
        }
    }
    return best;
}

std::optional<world::PickId> CabinetAttract::update(std::span<const CabinetCandidate> candidates,
                                                    const glm::vec3& eye, double nowSeconds) {
    const std::optional<world::PickId> nearest = nearestAttract(candidates, eye, m_playing.value_or(world::kNoPick));
    if (m_playing && nearest != m_playing) {
        // The one playing stops at once once it no longer qualifies; else it plays on while another's dwell runs.
        bool qualifies = false;
        for (const CabinetCandidate& candidate : candidates) {
            if (candidate.pick == *m_playing) {
                qualifies = attractDistance(candidate, eye, true).has_value();
            }
        }
        if (!qualifies) {
            m_playing.reset();
        }
    }
    if (!nearest || nearest == m_playing) {
        m_candidate.reset();
        return m_playing;
    }
    if (m_candidate != nearest) {
        m_candidate = nearest;
        m_since = nowSeconds;
    }
    if (nowSeconds - m_since + kDwellTolerance >= kAttractDwellSeconds) {
        m_playing = std::exchange(m_candidate, std::nullopt);
    }
    return m_playing;
}

void CabinetAttract::reset() noexcept {
    m_playing.reset();
    m_candidate.reset();
    m_since = 0.0;
}

std::vector<CabinetPlay> chooseCabinets(const CabinetChoice& choice) {
    std::vector<CabinetPlay> plays;
    if (choice.editing || choice.state == WorldState::Edit) {
        return plays;
    }
    if (choice.focused != world::kNoPick && choice.showOnFocus) {
        switch (choice.state) {
        case WorldState::Focus:
            plays.push_back(CabinetPlay{choice.focused, ScreenClass::Focus});
            return plays;
        case WorldState::Inspect:
        case WorldState::AnimatingToConsole:
        case WorldState::Launching:
        case WorldState::Returning:
            plays.push_back(CabinetPlay{choice.focused, ScreenClass::Direct});
            return plays;
        case WorldState::FreeRoam:
        case WorldState::Edit:
            break;
        }
    }
    if (choice.state == WorldState::FreeRoam && choice.attract && choice.attractPick &&
        *choice.attractPick != world::kNoPick) {
        plays.push_back(CabinetPlay{*choice.attractPick, ScreenClass::Ambient});
    }
    return plays;
}

std::string cabinetScreenId(std::string_view room, world::PickId pick) {
    return fmt::format("{}/cabinet:{:08X}", room, pick);
}

} // namespace rv::app
