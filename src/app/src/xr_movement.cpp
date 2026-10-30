#include "xr_movement.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include <rv/app/scene_graphics.hpp>
#include <rv/assets/material_desc.hpp>
#include <rv/core/math.hpp>
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
constexpr std::string_view kBodyKey = "xr/controller-body";
constexpr std::string_view kBandKey = "xr/controller-band";
constexpr std::string_view kLaserKey = "xr/laser";
constexpr std::string_view kLandingKey = "xr/teleport-landing";
constexpr std::string_view kBlockedKey = "xr/teleport-blocked";
constexpr std::string_view kPlasticKey = "xr/controller-plastic";
constexpr std::string_view kAccentKey = "xr/controller-accent";
constexpr std::string_view kBeamKey = "xr/laser-beam";
/// The beads along the arc (none at its first point, under the chin), the ring just above the floor.
constexpr float kBeadRadius = 0.022f;
constexpr float kRingRadius = 0.3f;
constexpr float kRingTube = 0.012f;
constexpr float kRingLift = 0.01f;
/// The controller: a handle 13 cm long along the grip's pointing axis, a band around its head; the laser a thin rod
/// (1 m long, scaled to its length) with a bead at its end.
constexpr float kBodyRadius = 0.019f;
constexpr float kBodyLength = 0.13f;
constexpr float kBandRadius = 0.034f;
constexpr float kBandTube = 0.006f;
constexpr float kLaserRadius = 0.0022f;
constexpr float kLaserDotScale = 0.45f;

assets::MaterialDesc glowing(std::string name, const glm::vec3& color, float strength = 3.0f) {
    assets::MaterialDesc desc;
    desc.name = std::move(name);
    desc.baseColorFactor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
    desc.roughnessFactor = 0.9f;
    desc.emissiveFactor = color;
    desc.emissiveStrength = strength;
    return desc;
}

assets::MaterialDesc plastic() {
    assets::MaterialDesc desc;
    desc.name = "xr_controller_plastic";
    desc.baseColorFactor = glm::vec4(0.06f, 0.065f, 0.07f, 1.0f);
    desc.roughnessFactor = 0.45f;
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

glm::mat4 poseMatrix(const xr::Pose& pose) {
    glm::mat4 matrix = glm::mat4_cast(glm::normalize(pose.orientation));
    matrix[3] = glm::vec4(pose.position, 1.0f);
    return matrix;
}

Expected<scene::MeshHandle> meshOf(gl::MeshRegistry& meshes, std::string_view key, geometry::MeshBuilder builder) {
    if (const std::optional<scene::MeshHandle> found = meshes.find(key)) {
        return *found;
    }
    return meshes.add(key, std::move(builder).build(std::string(key)));
}

} // namespace

void XrHands::update(const xr::Frame* frame, const xr::TrackingAnchor& anchor) {
    if (frame == nullptr) {
        m_controllers = xr::ControllerState{};
        m_pad.release();
        m_pointer.clear();
        m_grips = {};
        m_aims = {};
        return;
    }
    m_controllers = frame->controllers;
    m_pad.update(m_controllers.pad);
    m_pointer.update(m_controllers.hands, anchor);
}

void XrHands::applySelect(input::ActionState& actions) const noexcept {
    if (!m_pointer.selectHeld() && !m_pointer.selectPressed() && !m_pointer.selectReleased()) {
        return;
    }
    actions.setButton(Action::Interact, actions.held(Action::Interact) || m_pointer.selectHeld(),
                      actions.pressed(Action::Interact) || m_pointer.selectPressed(),
                      actions.released(Action::Interact) || m_pointer.selectReleased());
}

void XrHands::follow(const xr::TrackingAnchor& anchor) noexcept {
    for (const xr::Hand hand : {xr::Hand::Left, xr::Hand::Right}) {
        const xr::HandState& state = m_controllers.hands[xr::toIndex(hand)];
        m_grips[xr::toIndex(hand)] =
            state.tracked && state.grip ? std::optional<xr::Pose>(xr::toWorld(anchor, *state.grip)) : std::nullopt;
        m_aims[xr::toIndex(hand)] =
            state.tracked && state.aim ? std::optional<xr::Pose>(xr::toWorld(anchor, *state.aim)) : std::nullopt;
    }
}

std::optional<xr::Pose> XrHands::trackedAim(xr::Hand hand) const noexcept {
    const xr::HandState& state = m_controllers.hands[xr::toIndex(hand)];
    return state.tracked ? state.aim : std::nullopt;
}

bool XrHands::anyTracked() const noexcept {
    return m_controllers.hands[0].tracked || m_controllers.hands[1].tracked;
}

XrMovement::XrMovement(const scene::FirstPersonSettings& walker, const xr::XrSettings& settings)
    : m_settings(settings) {
    m_locomotion.setSettings(locomotionSettings(walker, settings));
}

void XrMovement::update(const xr::Frame& frame, const input::ActionState& actions, const XrHands& hands,
                        float deltaSeconds, FreeRoam& freeRoam, scene::Camera& camera) {
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
    // I10 step 4: the right controller (which holds the turning stick) aims the arc, the left one may lead the walk.
    input.arcFrom = hands.trackedAim(xr::Hand::Right);
    input.moveAlong = hands.trackedAim(xr::Hand::Left);
    m_locomotion.update(xr::headOf(frame.eyes), frame.floorOffset, input, deltaSeconds, freeRoam.collision());
    walker.teleport(m_locomotion.feet(), m_locomotion.heading(), m_locomotion.pitch());
    freeRoam.update(input::ActionState{}, 0.0f, camera);
    m_written = std::pair{walker.feetPosition(), walker.yaw()};
}

Status XrProps::ensure(SceneGraphics& graphics, int uniformBufferOffsetAlignment) {
    gl::MaterialRegistry& materials = graphics.materials();
    const std::array<std::pair<std::string_view, scene::MaterialHandle*>, 5> materialSlots{
        {{kLandingKey, &m_landing},
         {kBlockedKey, &m_blocked},
         {kPlasticKey, &m_plastic},
         {kAccentKey, &m_accent},
         {kBeamKey, &m_beam}}};
    bool missing = false;
    for (const auto& [key, handle] : materialSlots) {
        if (const std::optional<scene::MaterialHandle> found = materials.find(key)) {
            *handle = *found;
        } else {
            missing = true;
        }
    }
    if (missing) {
        const gl::MaterialTextures none{};
        m_landing = materials.add(kLandingKey, "XR teleport (landing)",
                                  glowing("xr_teleport_landing", glm::vec3(0.25f, 0.85f, 1.0f)), none);
        m_blocked = materials.add(kBlockedKey, "XR teleport (blocked)",
                                  glowing("xr_teleport_blocked", glm::vec3(1.0f, 0.55f, 0.12f)), none);
        m_plastic = materials.add(kPlasticKey, "XR controller", plastic(), none);
        m_accent = materials.add(kAccentKey, "XR controller (band)",
                                 glowing("xr_controller_accent", glm::vec3(0.25f, 0.85f, 1.0f), 1.0f), none);
        m_beam = materials.add(kBeamKey, "XR laser", glowing("xr_laser", glm::vec3(0.8f, 0.9f, 1.0f), 1.5f), none);
        if (Status uploaded = materials.upload(uniformBufferOffsetAlignment, "MaterialUniforms"); !uploaded) {
            return uploaded;
        }
    }
    gl::MeshRegistry& meshes = graphics.meshes();
    if (meshes.find(kBeadKey) && meshes.find(kRingKey) && meshes.find(kBodyKey) && meshes.find(kBandKey) &&
        meshes.find(kLaserKey)) {
        m_bead = *meshes.find(kBeadKey);
        m_ring = *meshes.find(kRingKey);
        m_body = *meshes.find(kBodyKey);
        m_band = *meshes.find(kBandKey);
        m_laser = *meshes.find(kLaserKey);
        return ok();
    }
    geometry::MeshBuilder bead;
    bead.uvSphere(kBeadRadius, 12, 8);
    geometry::MeshBuilder ring;
    ring.torus(kRingRadius, kRingTube, 48, 8);
    // The handle along -Z (the grip's pointing axis), the band around its front end, the laser a rod along -Z.
    const glm::mat4 alongZ = glm::rotate(glm::mat4(1.0f), core::degreesToRadians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    geometry::MeshBuilder body;
    body.cylinder(kBodyRadius, kBodyLength, 16, true, alongZ);
    geometry::MeshBuilder band;
    band.torus(kBandRadius, kBandTube, 32, 8,
               glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -0.5f * kBodyLength)) * alongZ);
    geometry::MeshBuilder laser;
    laser.cylinder(kLaserRadius, 1.0f, 8, false,
                   glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -0.5f)) * alongZ);
    const std::array<std::tuple<std::string_view, scene::MeshHandle*, geometry::MeshBuilder*>, 5> meshSlots{
        {{kBeadKey, &m_bead, &bead},
         {kRingKey, &m_ring, &ring},
         {kBodyKey, &m_body, &body},
         {kBandKey, &m_band, &band},
         {kLaserKey, &m_laser, &laser}}};
    for (const auto& [key, handle, builder] : meshSlots) {
        auto mesh = meshOf(meshes, key, std::move(*builder));
        if (!mesh) {
            return propagate(mesh.error());
        }
        *handle = *mesh;
    }
    return ok();
}

Status XrProps::appendArc(const xr::TeleportArc& arc, SceneGraphics& graphics, int uniformBufferOffsetAlignment,
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

Status XrProps::appendHands(const XrHands& hands, float laserLength, bool onItem, SceneGraphics& graphics,
                            int uniformBufferOffsetAlignment, std::vector<scene::RenderItem>& items) {
    if (Status ready = ensure(graphics, uniformBufferOffsetAlignment); !ready) {
        return ready;
    }
    for (const xr::Hand hand : {xr::Hand::Left, xr::Hand::Right}) {
        if (const std::optional<xr::Pose>& grip = hands.grip(hand)) {
            const glm::mat4 model = poseMatrix(*grip);
            items.push_back(itemOf(m_body, m_plastic, model));
            items.push_back(itemOf(m_band, m_accent, model));
        }
    }
    if (const std::optional<xr::Hand> pointing = hands.pointer().hand()) {
        if (const std::optional<xr::Pose>& aim = hands.aim(*pointing); aim && laserLength > 0.0f) {
            const glm::mat4 model = poseMatrix(*aim);
            const scene::MaterialHandle beam = onItem ? m_landing : m_beam;
            items.push_back(
                itemOf(m_laser, beam, model * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, laserLength))));
            const glm::mat4 dot = model * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -laserLength)) *
                                  glm::scale(glm::mat4(1.0f), glm::vec3(kLaserDotScale));
            items.push_back(itemOf(m_bead, beam, dot));
        }
    }
    return ok();
}

} // namespace rv::app
