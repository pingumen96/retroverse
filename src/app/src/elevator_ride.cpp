#include <rv/app/elevator_ride.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace rv::app {
namespace {

/// A fade within this of its end has ended (the fixed frame time's 15 steps of 0.016667 s fall short of 0.25 s by a
/// rounding's worth).
constexpr double kFadeEpsilon = 1e-3;

} // namespace

std::string_view toString(RideReason reason) noexcept {
    switch (reason) {
    case RideReason::Floor:
        return "floor";
    case RideReason::Room:
        return "room";
    case RideReason::ShowInRoom:
        return "showInRoom";
    }
    return "unknown";
}

std::string_view toString(RidePhase phase) noexcept {
    switch (phase) {
    case RidePhase::FadingOut:
        return "fading-out";
    case RidePhase::AwaitingFloor:
        return "awaiting-floor";
    case RidePhase::AwaitingView:
        return "awaiting-view";
    case RidePhase::FadingIn:
        return "fading-in";
    case RidePhase::Done:
        return "done";
    }
    return "unknown";
}

ElevatorRide::ElevatorRide(RideRequest request, RideSettings settings, std::uint64_t frame)
    : m_request(std::move(request)), m_settings(settings) {
    m_record.from = m_request.from;
    m_record.to = m_request.to;
    m_record.reason = std::string(toString(m_request.reason));
    m_record.room = m_request.room;
    m_record.game = m_request.game;
    m_record.requestedFrame = frame;
    m_record.outcome = "running";
}

double ElevatorRide::step(double deltaSeconds) const noexcept {
    if (m_settings.reduceMotion || !(m_settings.fadeSeconds > 0.0)) {
        return 1.0;
    }
    const double dt = std::isfinite(deltaSeconds) && deltaSeconds > 0.0 ? deltaSeconds : 0.0;
    return dt / m_settings.fadeSeconds;
}

void ElevatorRide::floorAnswered(bool built, double elapsedMs, std::optional<double> jobMs) {
    if (m_built || m_failed || hasSwapped()) {
        return;
    }
    m_built = built;
    m_failed = !built;
    m_record.builtMs = elapsedMs;
    m_record.jobMs = jobMs;
}

RideAction ElevatorRide::update(const RideFrame& frame) {
    switch (m_phase) {
    case RidePhase::FadingOut:
        if (m_failed) {
            // Nothing to ride to: the view comes back from where its fade stands.
            m_phase = RidePhase::FadingIn;
            return RideAction::Fail;
        }
        m_fade = std::min(1.0, m_fade + step(frame.deltaSeconds));
        if (m_fade < 1.0 - kFadeEpsilon) {
            return RideAction::None;
        }
        m_fade = 1.0;
        m_record.fadedOutFrame = frame.frame;
        m_phase = RidePhase::AwaitingFloor;
        [[fallthrough]];
    case RidePhase::AwaitingFloor:
        if (m_failed) {
            m_phase = RidePhase::FadingIn;
            return RideAction::Fail;
        }
        if (!m_built) {
            return RideAction::None;
        }
        m_swapElapsedMs = frame.elapsedMs; // the view's timeout counts from the swap
        return RideAction::Swap;
    case RidePhase::AwaitingView: {
        const double sinceSwapSeconds = (frame.elapsedMs - m_swapElapsedMs.value_or(frame.elapsedMs)) / 1000.0;
        if (frame.viewResident) {
            m_record.residentMs = frame.elapsedMs;
            m_phase = RidePhase::FadingIn;
        } else if (!m_settings.lockstep && sinceSwapSeconds >= m_settings.viewTimeoutSeconds) {
            m_timedOut = true;
            m_phase = RidePhase::FadingIn;
        }
        return RideAction::None;
    }
    case RidePhase::FadingIn:
        m_fade = std::max(0.0, m_fade - step(frame.deltaSeconds));
        if (m_fade > kFadeEpsilon) {
            return RideAction::None;
        }
        m_fade = 0.0;
        m_record.fadedInFrame = frame.frame;
        m_record.totalMs = frame.elapsedMs;
        m_record.outcome = m_failed ? "failed" : m_timedOut ? "timed-out" : "arrived";
        m_phase = RidePhase::Done;
        return RideAction::Finish;
    case RidePhase::Done:
        return RideAction::None;
    }
    return RideAction::None;
}

void ElevatorRide::swapped(std::uint64_t frame) {
    if (m_phase != RidePhase::AwaitingFloor || !m_built) {
        return;
    }
    m_record.swappedFrame = frame;
    m_phase = RidePhase::AwaitingView;
}

} // namespace rv::app
