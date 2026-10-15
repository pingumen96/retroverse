#pragma once

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/viewpoint.hpp>

namespace rv::scene {

/// One frame of input for an OrbitController, device-neutral: the asset preview fills it from mouse drags over the
/// scene, the wheel and a gamepad.
struct OrbitInput {
    /// Pointer motion while orbiting (a left drag), pixels: +X right, +Y down.
    glm::vec2 orbitPixels{0.0f};
    /// Pointer motion while panning (a middle or right drag), pixels: +X right, +Y down. The target follows the
    /// pointer.
    glm::vec2 panPixels{0.0f};
    /// Wheel notches: + away from the user moves closer.
    float zoomNotches = 0.0f;
    /// Orbit rate, [-1, 1] per axis (a stick): +X turns like a drag to the right, +Y raises the eye over the target.
    glm::vec2 orbitRate{0.0f};
    /// Pan rate, [-1, 1] per axis (a stick): +X moves the view right, +Y up.
    glm::vec2 panRate{0.0f};
    /// Zoom rate, [-1, 1] (right trigger minus left trigger): + moves closer.
    float zoomRate = 0.0f;
};

/// Speeds and limits of an OrbitController.
struct OrbitSettings {
    float degreesPerPixel = 0.3f;         ///< orbit per pixel of drag
    float stickDegreesPerSecond = 120.0f; ///< orbit at full stick deflection
    float zoomPerNotch = 1.15f;           ///< the distance shrinks by this factor per wheel notch towards the target
    float zoomPerSecond = 3.0f;           ///< the distance shrinks by this factor per second at full trigger
    float panViewsPerSecond = 0.75f;      ///< pan at full stick deflection, in view heights at the target per second
    float autoRotateDegreesPerSecond = 20.0f;
    float pitchLimitDeg = 89.0f; ///< the eye stays within this angle above and below the target's horizon
    float frameMargin = 1.15f;   ///< distance factor over the tight fit of framed bounds
};

/// An orbit camera around a target point, for inspecting an asset: the eye stays `distance` meters from the target,
/// at `yaw` and `pitch` (Camera conventions: the eye looks along forward(yaw, pitch) at the target, so yaw 0 sees the
/// target from +Z, its front, and a negative pitch looks down on it). Drags orbit, pan and zoom it, an optional
/// auto-rotation turns it slowly around the vertical, and frame() sets it to show given bounds whole. The camera it
/// poses (apply) shows the target at the center of a part of the view (a ViewArea: the area the developer overlay's
/// panels leave visible), with clip planes that follow the distance, so a 1 cm part and a 20 m room can both be
/// inspected. GPU-free; main thread.
class OrbitController {
public:
    explicit OrbitController(const OrbitSettings& settings = {}) noexcept : m_settings(settings) {}

    /// Targets the center of `bounds` from `yawDeg` and `pitchDeg` degrees, at the distance that fits the sphere around
    /// the bounds in `area` of a view of `lens` at `aspect` with OrbitSettings::frameMargin, and keeps the size of the
    /// bounds for the zoom limits and the clip planes. Empty or non-finite bounds are framed as a 1 m cube standing on
    /// the origin.
    void frame(const core::Aabb& bounds, float yawDeg, float pitchDeg, const CameraLens& lens, float aspect,
               const ViewArea& area = {}) noexcept;

    /// Advances one frame: drags and sticks orbit and pan, the wheel and triggers zoom, the auto-rotation turns the
    /// eye. `view` gives the lens and aspect the pan is measured with (the target follows the pointer at its own
    /// depth) and `viewportHeight` the height of the view in pixels. Non-finite input is ignored.
    void update(const OrbitInput& input, float deltaSeconds, const Camera& view, float viewportHeight) noexcept;

    /// Poses `camera`: the eye at its distance from the target, oriented so that the target appears at the center of
    /// `area` (aimAt), and the clip planes of lens() with the camera's field of view.
    void apply(Camera& camera, const ViewArea& area = {}) const noexcept;

    /// The camera lens of the current distance: `fieldOfView` horizontal degrees, a near plane a quarter of the way to
    /// the front of the framed bounds (between 0.5 mm and 5 cm), a far plane well beyond their back (at least 100 m).
    [[nodiscard]] CameraLens lens(float horizontalFovDeg) const noexcept;
    /// The eye position.
    [[nodiscard]] glm::vec3 eye() const noexcept;

    void setAutoRotate(bool enabled) noexcept { m_autoRotate = enabled; }
    [[nodiscard]] bool autoRotate() const noexcept { return m_autoRotate; }

    [[nodiscard]] const glm::vec3& target() const noexcept { return m_target; }
    /// Radians.
    [[nodiscard]] float yaw() const noexcept { return m_yaw; }
    [[nodiscard]] float pitch() const noexcept { return m_pitch; }
    /// Meters between the eye and the target, within [minDistance(), maxDistance()].
    [[nodiscard]] float distance() const noexcept { return m_distance; }
    [[nodiscard]] float minDistance() const noexcept;
    [[nodiscard]] float maxDistance() const noexcept;
    [[nodiscard]] const OrbitSettings& settings() const noexcept { return m_settings; }

private:
    /// Sets the pitch within the limits and wraps the yaw to [-pi, pi].
    void setAngles(float yaw, float pitch) noexcept;

    OrbitSettings m_settings;
    glm::vec3 m_target{0.0f, 0.5f, 0.0f};
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_distance = 3.0f;
    /// Radius of the sphere around the framed bounds.
    float m_radius = 0.866f;
    bool m_autoRotate = false;
};

} // namespace rv::scene
