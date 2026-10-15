#include "free_roam.hpp"

#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/math.hpp>

namespace rv::app {

FreeRoam::FreeRoam(const scene::SceneDescription& scene, scene::CollisionWorld collision)
    : m_collision(std::move(collision)) {
    const scene::FirstPersonSettings& settings = m_controller.settings();
    teleportToCamera(scene.camera);
    m_startFeet = m_controller.feetPosition();
    spdlog::info("Free roam: {} collider box(es); start {}", m_collision.boxes().size(), describePose());
    spdlog::info("Free roam: walk {} m/s, sprint {} m/s, eye height {} m, capsule radius {} m and height {} m, pitch "
                 "limit {} deg",
                 settings.walkSpeed, settings.sprintSpeed, settings.eyeHeight, settings.capsule.radius,
                 settings.capsule.height, settings.pitchLimitDeg);
    if (m_collision.overlaps(m_startFeet, settings.capsule)) {
        const auto nearest = m_collision.nearestBlocking(m_startFeet, settings.capsule);
        spdlog::warn("The scene camera starts inside collider '{}': the walker is pushed out on its first step",
                     nearest ? m_collision.boxes()[nearest->index].tag : std::string("?"));
    }
}

void FreeRoam::update(const input::ActionState& actions, float deltaSeconds, scene::Camera& camera) {
    m_controller.update(actions, deltaSeconds, camera, m_collisionEnabled ? &m_collision : nullptr);
}

void FreeRoam::teleportToCamera(const scene::CameraSetup& camera) noexcept {
    const glm::vec3 feet = camera.position - glm::vec3(0.0f, m_controller.settings().eyeHeight, 0.0f);
    m_controller.teleport(feet, core::degreesToRadians(camera.yawDeg), core::degreesToRadians(camera.pitchDeg));
}

std::string FreeRoam::describePose() const {
    const glm::vec3& feet = m_controller.feetPosition();
    return fmt::format("feet ({:.4f}, {:.4f}, {:.4f}) m, yaw {:.2f} deg, pitch {:.2f} deg", feet.x, feet.y, feet.z,
                       core::radiansToDegrees(m_controller.yaw()), core::radiansToDegrees(m_controller.pitch()));
}

void FreeRoam::logFinalPose(std::uint64_t frames, double simulatedSeconds) const {
    const scene::FirstPersonSettings& settings = m_controller.settings();
    const glm::vec3& feet = m_controller.feetPosition();
    const glm::vec3 eye = feet + glm::vec3(0.0f, settings.eyeHeight, 0.0f);
    const float walked = glm::length(glm::vec2(feet.x - m_startFeet.x, feet.z - m_startFeet.z));
    spdlog::info("Final camera after {} frame(s) ({:.4f} s simulated): {}, eye ({:.4f}, {:.4f}, {:.4f}) m; {:.4f} m "
                 "from the start ({:.4f}, {:.4f}, {:.4f})",
                 frames, simulatedSeconds, describePose(), eye.x, eye.y, eye.z, walked, m_startFeet.x, m_startFeet.y,
                 m_startFeet.z);
    const auto nearest = m_collision.nearestBlocking(feet, settings.capsule);
    if (!nearest) {
        spdlog::info("Nearest collider: none at the walker's height");
        return;
    }
    spdlog::info("Nearest collider: '{}', {:.4f} m from the capsule axis, clearance {:.4f} m (capsule radius {} m)",
                 m_collision.boxes()[nearest->index].tag, nearest->distance,
                 nearest->distance - settings.capsule.radius, settings.capsule.radius);
}

} // namespace rv::app
