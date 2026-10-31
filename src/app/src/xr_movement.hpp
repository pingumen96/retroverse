#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/xr/controllers.hpp>
#include <rv/xr/locomotion.hpp>
#include <rv/xr/session.hpp>
#include <rv/xr/xr_settings.hpp>

#include "free_roam.hpp"

namespace rv::app {

class SceneGraphics;

/// The controllers of a VR frame (I10 step 4, ADR-0017 §5): their buttons and sticks as a gamepad merged into the
/// input, the pointing hand's ray for the pick and its trigger as interact, the hands' poses in the world for their
/// drawing and for the item held in the hand. Main thread.
class XrHands {
public:
    /// The frame's controllers before its input is read, the tracking space where the last frame left it (the pick
    /// ray's); none (the desktop, a frame not located): every button released, nothing pointing.
    void update(const xr::Frame* frame, const xr::TrackingAnchor& anchor);
    [[nodiscard]] const xr::VirtualPad& pad() const noexcept { return m_pad; }
    [[nodiscard]] const xr::Pointer& pointer() const noexcept { return m_pointer; }
    /// The pointing hand's trigger as interact, over the frame's actions.
    void applySelect(input::ActionState& actions) const noexcept;
    /// The controllers in the world once the frame's movement placed the tracking space.
    void follow(const xr::TrackingAnchor& anchor) noexcept;
    [[nodiscard]] const std::optional<xr::Pose>& grip(xr::Hand hand) const noexcept {
        return m_grips[xr::toIndex(hand)];
    }
    [[nodiscard]] const std::optional<xr::Pose>& aim(xr::Hand hand) const noexcept {
        return m_aims[xr::toIndex(hand)];
    }
    /// A hand's aim pose in the tracking space, while it is tracked.
    [[nodiscard]] std::optional<xr::Pose> trackedAim(xr::Hand hand) const noexcept;
    /// A controller was tracked this frame.
    [[nodiscard]] bool anyTracked() const noexcept;

private:
    xr::VirtualPad m_pad;
    xr::Pointer m_pointer;
    xr::ControllerState m_controllers;
    std::array<std::optional<xr::Pose>, xr::kHandCount> m_grips;
    std::array<std::optional<xr::Pose>, xr::kHandCount> m_aims;
};

/// The walker in VR (I10 step 3, ADR-0017 §5): xr::Locomotion over FreeRoam's walker. Each located frame it takes the
/// walker's pose when something else moved it since the frame before (the start, a ride's destination, the world's
/// ease, the overlay's "go to"), moves with the walker's actions of the frame (the stick along the head's heading or
/// the left controller's, the turns, the teleport aimed with the head or the right controller), and writes the walker
/// back: its feet the capsule's, its yaw and pitch the head's, so that the world's interaction, the rides and the
/// report see the walker where the user stands. A recentering of the runtime keeps the head where it was. Main thread.
class XrMovement {
public:
    XrMovement(const scene::FirstPersonSettings& walker, const xr::XrSettings& settings);

    /// One located frame: `actions` are the walker's of the frame (none while the world drives the walker or a ride
    /// runs); the camera is left at the walker (the head's camera replaces it).
    void update(const xr::Frame& frame, const input::ActionState& actions, const XrHands& hands, float deltaSeconds,
                FreeRoam& freeRoam, scene::Camera& camera);

    [[nodiscard]] const xr::Locomotion& locomotion() const noexcept { return m_locomotion; }

private:
    xr::XrSettings m_settings;
    xr::Locomotion m_locomotion;
    /// The walker's feet and yaw as update() left them.
    std::optional<std::pair<glm::vec3, float>> m_written;
};

/// What the VR mode adds to the scene it draws: the teleport's arc as a row of glowing beads, cyan with a ring on the
/// floor where it lands, amber where it cannot (I10 step 3); the controllers, generic and original, and the pointing
/// hand's laser to where it meets a collider, brighter while it rests on an item (I10 step 4). The meshes and
/// materials are registered in the scene's registries when first drawn (again after a registry forgot them). Main
/// thread, GL context current.
class XrProps {
public:
    /// The laser's reach at most, meters.
    static constexpr float kLaserReach = 4.0f;

    /// The view's items with the arc's after them, in `items` (the view's, copied by the caller). Errors: a material
    /// the registry could not upload.
    [[nodiscard]] Status appendArc(const xr::TeleportArc& arc, SceneGraphics& graphics,
                                   int uniformBufferOffsetAlignment, std::vector<scene::RenderItem>& items);
    /// The controllers in `hands` and the pointing hand's laser, `laserLength` long (`onItem`: it rests on an item).
    [[nodiscard]] Status appendHands(const XrHands& hands, float laserLength, bool onItem, SceneGraphics& graphics,
                                     int uniformBufferOffsetAlignment, std::vector<scene::RenderItem>& items);

private:
    [[nodiscard]] Status ensure(SceneGraphics& graphics, int uniformBufferOffsetAlignment);

    scene::MeshHandle m_bead;
    scene::MeshHandle m_ring;
    scene::MeshHandle m_body;
    scene::MeshHandle m_band;
    scene::MeshHandle m_laser;
    scene::MaterialHandle m_landing;
    scene::MaterialHandle m_blocked;
    scene::MaterialHandle m_plastic;
    scene::MaterialHandle m_accent;
    scene::MaterialHandle m_beam;
};

} // namespace rv::app
