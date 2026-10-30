#include "xr_movement.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include <rv/app/scene_graphics.hpp>
#include <rv/assets/material_desc.hpp>
#include <rv/geometry/mesh_builder.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>

namespace rv::app {
namespace {

using input::Action;

xr::LocomotionSettings locomotionSettings(const scene::FirstPersonSettings& walker, const xr::XrSettings& settings) {
    xr::LocomotionSettings locomotion;
    locomotion.walkSpeed = walker.walkSpeed;
    locomotion.sprintSpeed = walker.sprintSpeed;
    locomotion.eyeHeight = walker.eyeHeight;
    locomotion.capsule = walker.capsule;
    locomotion.posture = settings.posture;
    locomotion.comfort = settings.comfort;
    return locomotion;
}

constexpr std::string_view kBeadKey = "xr/teleport-bead";
constexpr std::string_view kRingKey = "xr/teleport-ring";
constexpr std::string_view kLandingKey = "xr/teleport-landing";
constexpr std::string_view kBlockedKey = "xr/teleport-blocked";
/// The beads along the arc (none at its first point, under the chin), the ring just above the floor.
constexpr float kBeadRadius = 0.022f;
constexpr float kRingRadius = 0.3f;
constexpr float kRingTube = 0.012f;
constexpr float kRingLift = 0.01f;

assets::MaterialDesc glowing(std::string name, const glm::vec3& color) {
    assets::MaterialDesc desc;
    desc.name = std::move(name);
    desc.baseColorFactor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
    desc.roughnessFactor = 0.9f;
    desc.emissiveFactor = color;
    desc.emissiveStrength = 3.0f;
    return desc;
}

scene::RenderItem itemOf(scene::MeshHandle mesh, scene::MaterialHandle material, const glm::mat4& model) {
    scene::RenderItem item;
    item.mesh = mesh;
    item.material = material;
    item.model = model;
    item.castsShadow = false;
    return item;
}

} // namespace

XrMovement::XrMovement(const scene::FirstPersonSettings& walker, const xr::XrSettings& settings)
    : m_settings(settings) {
    m_locomotion.setSettings(locomotionSettings(walker, settings));
}

void XrMovement::update(const xr::Frame& frame, const input::ActionState& actions, float deltaSeconds,
                        FreeRoam& freeRoam, scene::Camera& camera) {
    scene::FirstPersonController& walker = freeRoam.controller();
    // The overlay may change the walker's speeds and capsule at any time.
    m_locomotion.setSettings(locomotionSettings(walker.settings(), m_settings));
    if (!m_written || walker.feetPosition() != m_written->first || walker.yaw() != m_written->second) {
        m_locomotion.place(walker.feetPosition(), walker.yaw());
    }
    if (frame.spaceChanged) {
        m_locomotion.recenter();
    }
    xr::LocomotionInput input;
    input.move = glm::vec2(actions.axis(Action::MoveRight), actions.axis(Action::MoveForward));
    input.sprint = actions.held(Action::Sprint);
    input.turn = actions.axis(Action::LookX);
    input.turnRadians = actions.axis(Action::LookDeltaX) * walker.settings().mouseRadiansPerPixel;
    input.aim = actions.axis(Action::LookY);
    m_locomotion.update(xr::headOf(frame.eyes), frame.floorOffset, input, deltaSeconds, freeRoam.collision());
    walker.teleport(m_locomotion.feet(), m_locomotion.heading(), m_locomotion.pitch());
    freeRoam.update(input::ActionState{}, 0.0f, camera);
    m_written = std::pair{walker.feetPosition(), walker.yaw()};
}

Status XrProps::ensure(SceneGraphics& graphics, int uniformBufferOffsetAlignment) {
    gl::MaterialRegistry& materials = graphics.materials();
    const std::optional<scene::MaterialHandle> landing = materials.find(kLandingKey);
    const std::optional<scene::MaterialHandle> blocked = materials.find(kBlockedKey);
    if (landing && blocked) {
        m_landing = *landing;
        m_blocked = *blocked;
    } else {
        const gl::MaterialTextures none{};
        m_landing = materials.add(kLandingKey, "XR teleport (landing)",
                                  glowing("xr_teleport_landing", glm::vec3(0.25f, 0.85f, 1.0f)), none);
        m_blocked = materials.add(kBlockedKey, "XR teleport (blocked)",
                                  glowing("xr_teleport_blocked", glm::vec3(1.0f, 0.55f, 0.12f)), none);
        if (Status uploaded = materials.upload(uniformBufferOffsetAlignment, "MaterialUniforms"); !uploaded) {
            return uploaded;
        }
    }
    gl::MeshRegistry& meshes = graphics.meshes();
    const std::optional<scene::MeshHandle> beadFound = meshes.find(kBeadKey);
    const std::optional<scene::MeshHandle> ringFound = meshes.find(kRingKey);
    if (beadFound && ringFound) {
        m_bead = *beadFound;
        m_ring = *ringFound;
        return ok();
    }
    geometry::MeshBuilder beadBuilder;
    beadBuilder.uvSphere(kBeadRadius, 12, 8);
    auto bead = meshes.add(kBeadKey, std::move(beadBuilder).build("xr_teleport_bead"));
    if (!bead) {
        return propagate(bead.error());
    }
    geometry::MeshBuilder ringBuilder;
    ringBuilder.torus(kRingRadius, kRingTube, 48, 8);
    auto ring = meshes.add(kRingKey, std::move(ringBuilder).build("xr_teleport_ring"));
    if (!ring) {
        return propagate(ring.error());
    }
    m_bead = *bead;
    m_ring = *ring;
    return ok();
}

Status XrProps::append(const xr::TeleportArc& arc, SceneGraphics& graphics, int uniformBufferOffsetAlignment,
                       std::vector<scene::RenderItem>& items) {
    if (Status ready = ensure(graphics, uniformBufferOffsetAlignment); !ready) {
        return ready;
    }
    const scene::MaterialHandle material = arc.landing ? m_landing : m_blocked;
    // A bead on every point and halfway to the next (about 10 cm apart at the arc's speed).
    for (std::size_t index = 1; index < arc.points.size(); ++index) {
        const glm::vec3 halfway = 0.5f * (arc.points[index - 1] + arc.points[index]);
        items.push_back(itemOf(m_bead, material, glm::translate(glm::mat4(1.0f), halfway)));
        items.push_back(itemOf(m_bead, material, glm::translate(glm::mat4(1.0f), arc.points[index])));
    }
    if (arc.landing) {
        items.push_back(itemOf(m_ring, m_landing,
                               glm::translate(glm::mat4(1.0f), *arc.landing + glm::vec3(0.0f, kRingLift, 0.0f))));
    }
    return ok();
}

} // namespace rv::app
