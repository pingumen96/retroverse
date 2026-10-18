#include <rv/scene/pose_animator.hpp>

#include <algorithm>
#include <cmath>

#include <rv/core/math.hpp>

namespace rv::scene {

namespace {

[[nodiscard]] float finiteOrZero(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}

} // namespace

float ease(Easing easing, float t) noexcept {
    const float x = std::clamp(finiteOrZero(t), 0.0f, 1.0f);
    if (x <= 0.0f || x >= 1.0f) {
        return x;
    }
    switch (easing) {
    case Easing::Linear:
        return x;
    case Easing::EaseInOut:
        return x * x * (3.0f - 2.0f * x);
    case Easing::EaseOut: {
        const float rest = 1.0f - x;
        return 1.0f - rest * rest * rest;
    }
    }
    return x;
}

float interpolatePose(float a, float b, float t) noexcept {
    if (!(t > 0.0f)) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    return a + (b - a) * t;
}

glm::vec3 interpolatePose(const glm::vec3& a, const glm::vec3& b, float t) noexcept {
    if (!(t > 0.0f)) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    return a + (b - a) * t;
}

WalkerPose interpolatePose(const WalkerPose& a, const WalkerPose& b, float t) noexcept {
    if (!(t > 0.0f)) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    WalkerPose pose;
    pose.feet = interpolatePose(a.feet, b.feet, t);
    pose.yaw = core::wrapAngleRadians(a.yaw + core::wrapAngleRadians(b.yaw - a.yaw) * t);
    pose.pitch = interpolatePose(a.pitch, b.pitch, t);
    return pose;
}

RigidPose interpolatePose(const RigidPose& a, const RigidPose& b, float t) noexcept {
    if (!(t > 0.0f)) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    // Along the shorter arc: q and -q are one orientation.
    const glm::quat to = glm::dot(a.orientation, b.orientation) < 0.0f ? -b.orientation : b.orientation;
    return RigidPose{interpolatePose(a.position, b.position, t), glm::normalize(glm::slerp(a.orientation, to, t))};
}

bool samePose(float a, float b) noexcept {
    return std::fabs(a - b) <= kSameValueTolerance;
}

bool samePose(const glm::vec3& a, const glm::vec3& b) noexcept {
    return std::fabs(a.x - b.x) <= kSamePositionTolerance && std::fabs(a.y - b.y) <= kSamePositionTolerance &&
           std::fabs(a.z - b.z) <= kSamePositionTolerance;
}

bool samePose(const WalkerPose& a, const WalkerPose& b) noexcept {
    return samePose(a.feet, b.feet) && std::fabs(core::wrapAngleRadians(a.yaw - b.yaw)) <= kSameAngleTolerance &&
           std::fabs(a.pitch - b.pitch) <= kSameAngleTolerance;
}

bool samePose(const RigidPose& a, const RigidPose& b) noexcept {
    // For unit quaternions a small angle between them is about twice the distance between q and the nearer of r and -r
    // (a dot product cannot resolve 1e-5 radians in floats).
    const float distance = std::min(glm::length(a.orientation - b.orientation),
                                    glm::length(a.orientation + b.orientation));
    return samePose(a.position, b.position) && 2.0f * distance <= kSameAngleTolerance;
}

void EasedProgress::moveTo(float goal, float fullDurationSeconds) noexcept {
    m_goal = goal < 0.5f ? 0.0f : 1.0f;
    if (m_reduceMotion || !std::isfinite(fullDurationSeconds) || fullDurationSeconds <= 0.0f) {
        m_value = m_goal;
        m_rate = 0.0f;
        return;
    }
    m_rate = 1.0f / fullDurationSeconds;
}

void EasedProgress::jumpTo(float value) noexcept {
    m_value = std::clamp(finiteOrZero(value), 0.0f, 1.0f);
    m_goal = m_value;
    m_rate = 0.0f;
}

void EasedProgress::advance(float seconds) noexcept {
    if (!moving()) {
        return;
    }
    const float step = std::max(finiteOrZero(seconds), 0.0f) * m_rate;
    m_value = m_goal > m_value ? std::min(m_goal, m_value + step) : std::max(m_goal, m_value - step);
}

void EasedProgress::finish() noexcept {
    m_value = m_goal;
}

void EasedProgress::setReduceMotion(bool on) noexcept {
    m_reduceMotion = on;
    if (on) {
        finish();
    }
}

} // namespace rv::scene
