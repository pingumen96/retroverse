#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/xr/locomotion.hpp>
#include <rv/xr/session.hpp>
#include <rv/xr/xr_settings.hpp>

#include "free_roam.hpp"

namespace rv::app {

class SceneGraphics;

/// The walker in VR (I10 step 3, ADR-0017 §5): xr::Locomotion over FreeRoam's walker. Each located frame it takes the
/// walker's pose when something else moved it since the frame before (the start, a ride's destination, the world's
/// ease, the overlay's "go to"), moves with the walker's actions of the frame (the stick along the head's heading, the
/// turns, the teleport), and writes the walker back: its feet the capsule's, its yaw and pitch the head's, so that the
/// world's interaction, the rides and the report see the walker where the user stands. A recentering of the runtime
/// keeps the head where it was. Main thread.
class XrMovement {
public:
    XrMovement(const scene::FirstPersonSettings& walker, const xr::XrSettings& settings);

    /// One located frame: `actions` are the walker's of the frame (none while the world drives the walker or a ride
    /// runs); the camera is left at the walker (the head's camera replaces it).
    void update(const xr::Frame& frame, const input::ActionState& actions, float deltaSeconds, FreeRoam& freeRoam,
                scene::Camera& camera);

    [[nodiscard]] const xr::Locomotion& locomotion() const noexcept { return m_locomotion; }

private:
    xr::XrSettings m_settings;
    xr::Locomotion m_locomotion;
    /// The walker's feet and yaw as update() left them.
    std::optional<std::pair<glm::vec3, float>> m_written;
};

/// What the VR mode adds to the scene it draws (I10 step 3): the teleport's arc as a row of glowing beads, cyan with a
/// ring on the floor where it lands, amber where it cannot. Its meshes and materials are registered in the scene's
/// registries when first drawn (again after a registry forgot them). Main thread, GL context current.
class XrProps {
public:
    /// The view's items with the arc's after them, in `items` (reused from frame to frame). Errors: a material the
    /// registry could not upload (the arc is then not drawn).
    [[nodiscard]] Status append(const xr::TeleportArc& arc, SceneGraphics& graphics, int uniformBufferOffsetAlignment,
                                std::vector<scene::RenderItem>& items);

private:
    [[nodiscard]] Status ensure(SceneGraphics& graphics, int uniformBufferOffsetAlignment);

    scene::MeshHandle m_bead;
    scene::MeshHandle m_ring;
    scene::MaterialHandle m_landing;
    scene::MaterialHandle m_blocked;
};

} // namespace rv::app
