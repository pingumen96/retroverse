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

double ElevatorRide::fadeMs() const noexcept {
    return m_settings.reduceMotion || !(m_settings.fadeSeconds > 0.0) ? 0.0 : m_settings.fadeSeconds * 1000.0;
}

double ElevatorRide::timeoutMs() const noexcept {
    return std::isfinite(m_settings.viewTimeoutSeconds) ? std::max(0.0, m_settings.viewTimeoutSeconds) * 1000.0 : 0.0;
}

double ElevatorRide::step(double deltaSeconds) const noexcept {
    if (!(fadeMs() > 0.0)) {
        return 1.0;
    }
    const double dt = std::isfinite(deltaSeconds) && deltaSeconds > 0.0 ? deltaSeconds : 0.0;
    return dt / m_settings.fadeSeconds;
}

void ElevatorRide::beginFadeIn(const RideFrame& frame) {
    m_phase = RidePhase::FadingIn;
    m_fadeInFrom = m_fade;
    m_fadeInStartMs = frame.elapsedMs;
    m_fadeInMs = fadeMs() * m_fade;
    if (!m_settings.lockstep) {
        // Begun late (a long frame before this one): faster, so that the view is back within the timeout.
        m_fadeInMs = std::min(m_fadeInMs, std::max(0.0, timeoutMs() - frame.elapsedMs));
    }
}

RideAction ElevatorRide::finish(const RideFrame& frame) {
    m_fade = 0.0;
    m_record.fadedInFrame = frame.frame;
    m_record.totalMs = frame.elapsedMs;
    m_record.outcome = m_failed ? "failed" : m_timedOut ? "timed-out" : "arrived";
    m_phase = RidePhase::Done;
    return RideAction::Finish;
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
            beginFadeIn(frame);
            return RideAction::Fail;
        }
        if (m_settings.lockstep) {
            m_fade = std::min(1.0, m_fade + step(frame.deltaSeconds));
        } else {
            // The real clock since the request (a frame's time is clamped in a long frame).
            const double fade = fadeMs() > 0.0 ? frame.elapsedMs / fadeMs() : 1.0;
            m_fade = std::clamp(std::max(m_fade, fade), 0.0, 1.0);
        }
        if (m_fade < 1.0 - kFadeEpsilon) {
            return RideAction::None;
        }
        // Black from this frame on; the floor swaps from the next one (the same frame whenever the job answered).
        m_fade = 1.0;
        m_record.fadedOutFrame = frame.frame;
        m_phase = RidePhase::AwaitingFloor;
        return RideAction::None;
    case RidePhase::AwaitingFloor:
        if (m_failed) {
            beginFadeIn(frame);
            return RideAction::Fail;
        }
        return m_built ? RideAction::Swap : RideAction::None;
    case RidePhase::AwaitingView: {
        const bool late = !m_settings.lockstep && frame.elapsedMs >= viewDeadlineMs();
        if (!frame.viewResident && !late) {
            return RideAction::None;
        }
        if (frame.viewResident) {
            m_record.residentMs = frame.elapsedMs;
        } else {
            m_timedOut = true;
        }
        beginFadeIn(frame);
        if (!m_settings.lockstep && frame.elapsedMs >= timeoutMs()) {
            return finish(frame); // past the timeout already: the view back at once
        }
        return RideAction::None;
    }
    case RidePhase::FadingIn:
        if (m_settings.lockstep) {
            m_fade = std::max(0.0, m_fade - step(frame.deltaSeconds));
        } else {
            const double progress = m_fadeInMs > 0.0 ? (frame.elapsedMs - m_fadeInStartMs) / m_fadeInMs : 1.0;
            m_fade = m_fadeInFrom * std::clamp(1.0 - progress, 0.0, 1.0);
        }
        if (m_fade > kFadeEpsilon) {
            return RideAction::None;
        }
        return finish(frame);
    case RidePhase::Done:
        return RideAction::None;
    }
    return RideAction::None;
}

double ElevatorRide::viewDeadlineMs() const noexcept {
    return std::max(0.0, timeoutMs() - fadeMs() - kViewDeadlineMarginSeconds * 1000.0);
}

void ElevatorRide::swapped(std::uint64_t frame) {
    if (m_phase != RidePhase::AwaitingFloor || !m_built) {
        return;
    }
    m_record.swappedFrame = frame;
    m_phase = RidePhase::AwaitingView;
}

} // namespace rv::app
