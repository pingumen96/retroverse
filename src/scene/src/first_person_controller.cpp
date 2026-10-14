#include <rv/scene/first_person_controller.hpp>

#include <algorithm>
#include <cmath>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

constexpr float kMaxPitchLimitDeg = 89.0f;

float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

float finiteAxis(const input::ActionState& actions, input::Action action) noexcept {
    return finiteOr(actions.axis(action), 0.0f);
}

/// Wraps to (-pi, pi], leaving in-range angles bit-exact (wrapAngleRadians adds and subtracts pi).
float wrapYaw(float radians) noexcept {
    return (radians > -core::kPi && radians <= core::kPi) ? radians : core::wrapAngleRadians(radians);
}

} // namespace

void FirstPersonController::setSettings(const FirstPersonSettings& settings) noexcept {
    const FirstPersonSettings defaults;
    FirstPersonSettings applied = settings;
    applied.walkSpeed = std::max(0.0f, finiteOr(settings.walkSpeed, defaults.walkSpeed));
    applied.sprintSpeed = std::max(0.0f, finiteOr(settings.sprintSpeed, defaults.sprintSpeed));
    applied.eyeHeight = std::max(0.0f, finiteOr(settings.eyeHeight, defaults.eyeHeight));
    applied.mouseRadiansPerPixel = finiteOr(settings.mouseRadiansPerPixel, defaults.mouseRadiansPerPixel);
    applied.stickDegreesPerSecond = finiteOr(settings.stickDegreesPerSecond, defaults.stickDegreesPerSecond);
    applied.pitchLimitDeg =
        std::clamp(finiteOr(settings.pitchLimitDeg, defaults.pitchLimitDeg), 0.0f, kMaxPitchLimitDeg);
    applied.capsule.radius = finiteOr(settings.capsule.radius, defaults.capsule.radius);
    applied.capsule.height = finiteOr(settings.capsule.height, defaults.capsule.height);
    m_settings = applied;

    const float limit = core::degreesToRadians(m_settings.pitchLimitDeg);
    m_pitch = std::clamp(m_pitch, -limit, limit);
}

void FirstPersonController::teleport(const glm::vec3& feet, float yawRadians, float pitchRadians) noexcept {
    for (int axis = 0; axis < 3; ++axis) {
        m_feet[axis] = finiteOr(feet[axis], m_feet[axis]);
    }
    if (std::isfinite(yawRadians)) {
        m_yaw = wrapYaw(yawRadians);
    }
    if (std::isfinite(pitchRadians)) {
        const float limit = core::degreesToRadians(m_settings.pitchLimitDeg);
        m_pitch = std::clamp(pitchRadians, -limit, limit);
    }
}

void FirstPersonController::update(const input::ActionState& actions, float dt, Camera& camera,
                                   const CollisionWorld* collision) {
    using input::Action;
    const float seconds = (std::isfinite(dt) && dt > 0.0f) ? dt : 0.0f;

    // Look first, so this frame's movement follows the new heading.
    const float stickRadiansPerSecond = core::degreesToRadians(m_settings.stickDegreesPerSecond);
    const float lookRateX = std::clamp(finiteAxis(actions, Action::LookX), -1.0f, 1.0f);
    const float lookRateY = std::clamp(finiteAxis(actions, Action::LookY), -1.0f, 1.0f);
    const float yawDelta = finiteAxis(actions, Action::LookDeltaX) * m_settings.mouseRadiansPerPixel +
                           lookRateX * stickRadiansPerSecond * seconds;
    const float pitchDelta = finiteAxis(actions, Action::LookDeltaY) * m_settings.mouseRadiansPerPixel +
                             lookRateY * stickRadiansPerSecond * seconds;
    const float pitchLimit = core::degreesToRadians(m_settings.pitchLimitDeg);
    // Overflowing inputs (absurd dt or sensitivities) leave the orientation unchanged.
    if (const float yaw = m_yaw + yawDelta; std::isfinite(yaw)) {
        m_yaw = wrapYaw(yaw);
    }
    if (const float pitch = m_pitch + pitchDelta; std::isfinite(pitch)) {
        m_pitch = std::clamp(pitch, -pitchLimit, pitchLimit);
    }

    glm::vec2 move(finiteAxis(actions, Action::MoveRight), finiteAxis(actions, Action::MoveForward));
    const float moveLength = glm::length(move);
    if (moveLength > 1.0f) {
        move /= moveLength;
    }
    const float speed = actions.held(Action::Sprint) ? m_settings.sprintSpeed : m_settings.walkSpeed;
    const glm::vec3 forwardXZ(std::sin(m_yaw), 0.0f, -std::cos(m_yaw));
    const glm::vec3 rightXZ(std::cos(m_yaw), 0.0f, std::sin(m_yaw));
    const glm::vec3 displacement = (forwardXZ * move.y + rightXZ * move.x) * (speed * seconds);

    if (core::isFinite(displacement)) {
        m_feet = collision != nullptr ? collision->moveCapsule(m_feet, displacement, m_settings.capsule)
                                      : m_feet + displacement;
    }

    camera.setPose(m_feet + glm::vec3(0.0f, m_settings.eyeHeight, 0.0f), m_yaw, m_pitch);
}

} // namespace rv::scene
