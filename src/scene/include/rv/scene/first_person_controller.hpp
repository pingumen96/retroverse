#pragma once

#include <glm/glm.hpp>

#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>

namespace rv::scene {

/// Tuning of the FreeRoam walker (style §6.2, spec §12). Speeds in m/s, heights in meters.
struct FirstPersonSettings {
    float walkSpeed = 2.5f;
    float sprintSpeed = 4.5f;
    /// Camera height above the feet (style §6.2: 1.60-1.70 m).
    float eyeHeight = 1.65f;
    /// Look sensitivity for LookDeltaX/Y (mouse pixels); negative values invert the axes.
    float mouseRadiansPerPixel = 0.0022f;
    /// Turn rate at full stick deflection for LookX/Y.
    float stickDegreesPerSecond = 180.0f;
    /// Pitch is clamped to +-pitchLimitDeg (at most 89 degrees).
    float pitchLimitDeg = 85.0f;
    Capsule capsule;
};

/// First-person walker driven by logical actions (rv::input::Action), independent of devices.
///
/// Per update: yaw += LookDeltaX * mouseRadiansPerPixel + LookX * radians(stickDegreesPerSecond) * dt, pitch
/// likewise from LookDeltaY / LookY (positive looks up), pitch clamped to +-pitchLimitDeg, yaw wrapped to
/// (-pi, pi]; then the feet move on the XZ plane relative to the new yaw by (MoveRight, MoveForward), a vector
/// normalized when longer than 1 (diagonals are not faster), at sprintSpeed while Sprint is held and walkSpeed
/// otherwise, through the collision world when one is given. Finally the camera pose is written:
/// position = feet + (0, eyeHeight, 0). Orientation conventions are the Camera's.
class FirstPersonController {
public:
    /// Applies settings. Non-finite fields fall back to their defaults, speeds and eye height are clamped to
    /// >= 0, the pitch limit to [0, 89] degrees; the current pitch is re-clamped to the new limit.
    void setSettings(const FirstPersonSettings& settings) noexcept;
    [[nodiscard]] const FirstPersonSettings& settings() const noexcept { return m_settings; }

    /// Places the feet and orientation directly (spawn, teleport). Yaw is wrapped, pitch clamped; non-finite
    /// values are ignored. The camera is updated by the next update() call.
    void teleport(const glm::vec3& feet, float yawRadians, float pitchRadians) noexcept;

    /// Advances by `dt` seconds (non-finite or negative dt counts as 0) and writes the camera pose.
    /// Non-finite action values count as 0; LookX/LookY rates are clamped to [-1, 1].
    void update(const input::ActionState& actions, float dt, Camera& camera, const CollisionWorld* collision = nullptr);

    [[nodiscard]] const glm::vec3& feetPosition() const noexcept { return m_feet; }
    [[nodiscard]] float yaw() const noexcept { return m_yaw; }
    [[nodiscard]] float pitch() const noexcept { return m_pitch; }

private:
    FirstPersonSettings m_settings;
    glm::vec3 m_feet{0.0f};
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
};

} // namespace rv::scene
