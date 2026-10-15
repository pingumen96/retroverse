#pragma once

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/scene/camera.hpp>

namespace rv::scene {

/// Pose of a first-person walker: feet position in meters, orientation in radians (Camera conventions).
struct WalkerPose {
    glm::vec3 feet{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

/// A rectangle of the view in normalized device coordinates (x right, y up, [-1, 1] across the view, as
/// Camera::rayFromNdc): the part of the view a viewpoint frames its object in. The whole view by default; the area the
/// developer overlay's panels leave see-through while it is open.
struct ViewArea {
    glm::vec2 min{-1.0f, -1.0f};
    glm::vec2 max{1.0f, 1.0f};
};

/// Horizontal extent beyond which an object spanning it along both x and z is a space to stand in, such as a room shell
/// or a floor, rather than a thing to look at. Objects long in one direction only (a long shelf, a counter, a bench)
/// stay things to look at.
inline constexpr float kLargeObjectExtent = 4.0f;
/// Closest and farthest distance between a viewpoint and the front of the object it looks at, in meters.
inline constexpr float kMinViewDistance = 0.8f;
inline constexpr float kMaxViewDistance = 4.0f;
/// Distance factor over the tight fit of an object in the view.
inline constexpr float kViewMargin = 1.2f;

/// Unit horizontal direction the front of an object faces: its local +Z (ADR-0002) under `transform`, projected on the
/// floor. +Z when the transform turns the front straight up or down, or is not finite.
[[nodiscard]] glm::vec3 frontDirection(const glm::mat4& transform) noexcept;

/// Where a walker stands to look at an object with world bounds `bounds` whose front faces `front` (a unit horizontal
/// vector, frontDirection), for the developer overlay's "go to". The feet stay at height `floorY`.
///  - An object spanning at most kLargeObjectExtent along x or z: in front of the side of `bounds` that `front` points
///    out of, at the distance that fits the whole object in `area` of a view of `lens` at `aspect` with kViewMargin,
///    clamped to [kMinViewDistance, kMaxViewDistance]; facing it (along -front), turned and pitched so that its center
///    appears at the center of `area` for the eye `eyeHeight` above the feet.
///  - An object spanning more than kLargeObjectExtent along both x and z (a room the walker stands in): at the center of
///    its footprint, facing along -front (from its front towards its back), level.
/// Empty or non-finite bounds count as a point at the origin; an area that is empty or not finite counts as the whole
/// view.
[[nodiscard]] WalkerPose frontViewpoint(const core::Aabb& bounds, const glm::vec3& front, float floorY, float eyeHeight,
                                        const CameraLens& lens, float aspect, const ViewArea& area = {}) noexcept;

} // namespace rv::scene
