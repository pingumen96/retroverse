#pragma once

#include <cstdint>
#include <string>

#include <glm/vec3.hpp>

#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

/// FreeRoam mode (specification §12.2): first-person walking with capsule collision. The walker starts at the scene
/// camera (feet = camera position - eye height, the camera's yaw, its pitch within the walker's pitch limit) and
/// collides with the scene's collision world (SceneInstance::collisionWorld). GPU-free state; main thread.
class FreeRoam {
public:
    FreeRoam(const scene::SceneDescription& scene, scene::CollisionWorld collision);

    /// Advances the walker by one frame of `deltaSeconds` and writes the camera pose.
    void update(const input::ActionState& actions, float deltaSeconds, scene::Camera& camera);

    /// "feet (x, y, z) m, yaw Y deg, pitch P deg", positions to 0.1 mm and angles to 0.01 degree.
    [[nodiscard]] std::string describePose() const;
    /// Logs the pose reached after `frames` frames and `simulatedSeconds` of simulated time: feet and eye position, yaw
    /// and pitch, the horizontal distance from the start, and the nearest collider at the walker's height with its
    /// clearance (distance - capsule radius).
    void logFinalPose(std::uint64_t frames, double simulatedSeconds) const;

private:
    scene::FirstPersonController m_controller;
    scene::CollisionWorld m_collision;
    glm::vec3 m_startFeet{0.0f};
};

} // namespace rv::app
