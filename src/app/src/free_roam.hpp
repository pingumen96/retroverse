#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <glm/vec3.hpp>

#include <rv/app/command_line.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

/// FreeRoam mode (specification §12.2): first-person walking with capsule collision. The walker starts at the scene
/// camera (feet = camera position - eye height, the camera's yaw, its pitch within the walker's pitch limit), or at the
/// spawn point of --spawn (its feet, its yaw and pitch or the scene camera's), and collides with the scene's collision
/// world (SceneInstance::collisionWorld) unless collision is turned off (the developer overlay's noclip). GPU-free
/// state; main thread.
class FreeRoam {
public:
    FreeRoam(const scene::SceneDescription& scene, scene::CollisionWorld collision,
             const std::optional<SpawnPoint>& spawn = std::nullopt);

    /// Another scene in place of the one walked in (the generated room after its shell, I3 step 10b): its collision
    /// world, the walker at its camera or at `spawn`, as the constructor places it; the walker's settings are kept.
    void resetScene(const scene::SceneDescription& scene, scene::CollisionWorld collision,
                    const std::optional<SpawnPoint>& spawn = std::nullopt);

    /// The scene's collision world after a part of it was rebuilt (I4b step 4: a room's static part), the walker where
    /// it stands.
    void setCollision(scene::CollisionWorld collision) noexcept { m_collision = std::move(collision); }

    /// Advances the walker by one frame of `deltaSeconds` and writes the camera pose.
    void update(const input::ActionState& actions, float deltaSeconds, scene::Camera& camera);

    /// Places the walker at `camera`: feet = camera position - eye height, the camera's yaw and pitch (within the
    /// walker's pitch limit). The camera pose follows at the next update().
    void teleportToCamera(const scene::CameraSetup& camera) noexcept;
    /// Walls and furniture stop the walker while enabled (the default); disabled, it walks through them.
    void setCollisionEnabled(bool enabled) noexcept { m_collisionEnabled = enabled; }
    /// The collision world the walker moves through: null while collision is off (the VR mode's movement, I10 step 3).
    [[nodiscard]] const scene::CollisionWorld* collision() const noexcept {
        return m_collisionEnabled ? &m_collision : nullptr;
    }
    /// The walker, for the developer overlay (pose, speeds).
    [[nodiscard]] scene::FirstPersonController& controller() noexcept { return m_controller; }

    /// "feet (x, y, z) m, yaw Y deg, pitch P deg", positions to 0.1 mm and angles to 0.01 degree.
    [[nodiscard]] std::string describePose() const;
    /// The walker's feet, yaw and pitch as --spawn takes them: a walker spawned there stands in the same pose.
    [[nodiscard]] SpawnPoint pose() const;
    /// Logs the pose reached after `frames` frames and `simulatedSeconds` of simulated time: feet and eye position, yaw
    /// and pitch, the horizontal distance from the start, and the nearest collider at the walker's height with its
    /// clearance (distance - capsule radius).
    void logFinalPose(std::uint64_t frames, double simulatedSeconds) const;

private:
    scene::FirstPersonController m_controller;
    scene::CollisionWorld m_collision;
    glm::vec3 m_startFeet{0.0f};
    bool m_collisionEnabled = true;
};

} // namespace rv::app
