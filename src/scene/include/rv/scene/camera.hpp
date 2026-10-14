#pragma once

#include <array>
#include <cstddef>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>

namespace rv::scene {

/// Projection parameters. The field of view is HORIZONTAL (style §6.2: 70-85 degrees on desktop) and is
/// converted to the vertical FOV used by the projection matrix with the viewport aspect ratio, so widening
/// the window shows more of the world instead of zooming in. Distances in meters (ADR-0002 defaults).
struct CameraLens {
    float horizontalFovDeg = 75.0f;
    float nearPlane = 0.05f;
    float farPlane = 100.0f;
};

/// Plane `dot(normal, p) + d = 0` with a unit normal: signedDistance() is in meters, positive on the side the
/// normal points to.
struct Plane {
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float d = 0.0f;

    [[nodiscard]] float signedDistance(const glm::vec3& point) const noexcept { return glm::dot(normal, point) + d; }
};

/// View frustum as six inward-facing planes (left, right, bottom, top, near, far): a point is inside when its
/// signed distance to every plane is >= 0.
struct Frustum {
    /// Index into `planes`.
    enum PlaneIndex : std::size_t { Left, Right, Bottom, Top, Near, Far };

    std::array<Plane, 6> planes{};

    /// Gribb-Hartmann extraction from `projection * view` (OpenGL clip space, depth in [-1, 1]).
    [[nodiscard]] static Frustum fromViewProjection(const glm::mat4& viewProjection) noexcept;

    /// Conservative box test: false only when the box is entirely outside one plane, so boxes straddling the
    /// frustum (or near its corners) count as visible. Empty boxes never intersect.
    [[nodiscard]] bool intersects(const core::Aabb& box) const noexcept;
    /// Inclusive point test.
    [[nodiscard]] bool contains(const glm::vec3& point) const noexcept;
};

/// Perspective camera with a yaw/pitch orientation (no roll).
///
/// Conventions (ADR-0002, right-handed, +Y up): yaw 0 looks towards -Z, positive yaw turns right (towards +X),
/// positive pitch looks up. forward = (sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch)); right is always
/// horizontal, (cos(yaw), 0, sin(yaw)), so the basis stays orthonormal at any pitch. Matrices follow glm's
/// right-handed lookAt/perspective with OpenGL clip space (depth in [-1, 1]); `clip = projection * view * p`.
/// A default camera sits at the origin with yaw = pitch = 0, the default lens and a 16:9 aspect ratio.
class Camera {
public:
    /// Smallest and largest accepted horizontal field of view.
    static constexpr float kMinHorizontalFovDeg = 1.0f;
    static constexpr float kMaxHorizontalFovDeg = 179.0f;

    /// Applies a lens. The FOV is clamped to [kMinHorizontalFovDeg, kMaxHorizontalFovDeg]. A lens with
    /// non-finite values, nearPlane <= 0 or farPlane <= nearPlane is rejected and the previous lens is kept.
    void setLens(const CameraLens& lens) noexcept;
    /// Viewport width / height. Non-finite or non-positive values (a minimized window) are ignored.
    void setAspect(float aspect) noexcept;
    /// Eye position in meters and orientation in radians. Non-finite components are ignored (the previous
    /// value is kept) so a corrupted input never poisons the matrices.
    void setPose(const glm::vec3& position, float yawRadians, float pitchRadians) noexcept;

    [[nodiscard]] const CameraLens& lens() const noexcept { return m_lens; }
    [[nodiscard]] float aspect() const noexcept { return m_aspect; }
    [[nodiscard]] const glm::vec3& position() const noexcept { return m_position; }
    [[nodiscard]] float yaw() const noexcept { return m_yaw; }
    [[nodiscard]] float pitch() const noexcept { return m_pitch; }

    /// Unit view direction.
    [[nodiscard]] glm::vec3 forward() const noexcept;
    /// Unit horizontal vector pointing to the right of the view direction.
    [[nodiscard]] glm::vec3 right() const noexcept;
    /// Unit camera up vector, cross(right, forward).
    [[nodiscard]] glm::vec3 up() const noexcept;

    /// World -> view transform: the camera sits at the origin looking down -Z with +Y up.
    [[nodiscard]] glm::mat4 view() const noexcept;
    /// View -> clip transform built from the vertical FOV derived from the lens and the aspect ratio.
    [[nodiscard]] glm::mat4 projection() const noexcept;
    [[nodiscard]] glm::mat4 viewProjection() const noexcept;
    [[nodiscard]] Frustum frustum() const noexcept;
    /// Vertical FOV in radians derived from the lens and the current aspect ratio.
    [[nodiscard]] float verticalFovRadians() const noexcept;

    /// Picking ray through a point in normalized device coordinates (x right, y UP, both in [-1, 1]; window
    /// coordinates must be converted and flipped by the caller). Origin = camera position, unit direction;
    /// rayFromNdc({0, 0}) points along forward().
    [[nodiscard]] core::Ray rayFromNdc(const glm::vec2& ndc) const noexcept;

    /// vertical = 2 atan(tan(horizontal / 2) / aspect). An invalid aspect (<= 0 or non-finite) counts as 1.
    [[nodiscard]] static float verticalFovRadians(float horizontalFovRadians, float aspect) noexcept;

private:
    CameraLens m_lens;
    float m_aspect = 16.0f / 9.0f;
    glm::vec3 m_position{0.0f};
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
};

} // namespace rv::scene
