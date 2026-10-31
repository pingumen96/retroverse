#pragma once

#include <optional>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>
#include <rv/xr/pose.hpp>

namespace rv::xr {

/// Where the VR mode's UI stands (I10 step 5, ADR-0017 §6): the product UI's whole canvas (the window's layout) drawn
/// on one panel in the world.
///  - World: a UI that has the input (the quick catalog, the directory, the media viewer, a launch's panel) stands in
///    front of the user where it opened, upright, facing them; the controllers' ray points at it.
///  - Head: otherwise the canvas is a heads-up display locked to the head (the hover's label, the prompts, the
///    notices, the card beside the held item), covering hudField(): the UI's anchors projected through that field (the
///    label over its item) land on the panel where the item is.
enum class PanelMode : std::uint8_t { World, Head };

/// The world panel's distance from the eyes, width (meters) and how far its centre stands below them.
inline constexpr float kWorldPanelDistance = 1.3f;
inline constexpr float kWorldPanelWidth = 1.8f;
inline constexpr float kWorldPanelDrop = 0.12f;
/// The heads-up display's distance and its horizontal field (degrees, symmetric).
inline constexpr float kHudDistance = 1.6f;
inline constexpr float kHudFieldDeg = 64.0f;

/// A panel in the world: its centre, its axes (+X to the picture's right, +Y up it, +Z out of its front towards the
/// viewer) and its size in meters.
struct PanelPose {
    glm::vec3 center{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec2 size{1.0f};

    /// The unit square [0, 1]^2 (x right, y up) placed on the panel: gl::Panel::model.
    [[nodiscard]] glm::mat4 model() const noexcept;
};

/// The world panel in front of `head`: kWorldPanelDistance along the head's heading (its yaw alone), kWorldPanelDrop
/// below the eyes, upright and facing back along that heading, kWorldPanelWidth wide at the canvas's `aspect`
/// (width over height).
[[nodiscard]] PanelPose worldPanel(const Pose& head, float aspect) noexcept;

/// The heads-up display of `head`: kHudDistance along its view, turned with it (rolls included), covering hudField()
/// of the canvas's `aspect`.
[[nodiscard]] PanelPose hudPanel(const Pose& head, float aspect) noexcept;

/// The heads-up display's field: kHudFieldDeg across, as tall as the canvas's `aspect` makes it.
[[nodiscard]] scene::EyeFov hudField(float aspect) noexcept;

/// Where `ray` (a unit direction) meets the front of `panel` within its bounds: the canvas's pixel there (x to the
/// right, y down from its top-left corner, in a canvas of `canvasSize` pixels) and the distance along the ray.
struct PanelHit {
    glm::vec2 pixel{0.0f};
    float distance = 0.0f;
};
[[nodiscard]] std::optional<PanelHit> panelHit(const PanelPose& panel, const core::Ray& ray,
                                               glm::vec2 canvasSize) noexcept;

} // namespace rv::xr
