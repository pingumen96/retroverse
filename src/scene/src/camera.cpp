#include <rv/scene/camera.hpp>

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace rv::scene {
namespace {

/// Row `index` of a column-major glm matrix.
glm::vec4 row(const glm::mat4& m, int index) noexcept {
    return {m[0][index], m[1][index], m[2][index], m[3][index]};
}

Plane normalizedPlane(const glm::vec4& coefficients) noexcept {
    const glm::vec3 normal(coefficients);
    const float length = glm::length(normal);
    if (!(length > 0.0f) || !std::isfinite(length)) {
        // Degenerate matrix: a plane that rejects nothing keeps culling conservative.
        return Plane{glm::vec3(0.0f), 0.0f};
    }
    return Plane{normal / length, coefficients.w / length};
}

bool isValidLens(const CameraLens& lens) noexcept {
    return std::isfinite(lens.horizontalFovDeg) && std::isfinite(lens.nearPlane) && std::isfinite(lens.farPlane) &&
           lens.nearPlane > 0.0f && lens.farPlane > lens.nearPlane;
}

} // namespace

Frustum Frustum::fromViewProjection(const glm::mat4& viewProjection) noexcept {
    const glm::vec4 r0 = row(viewProjection, 0);
    const glm::vec4 r1 = row(viewProjection, 1);
    const glm::vec4 r2 = row(viewProjection, 2);
    const glm::vec4 r3 = row(viewProjection, 3);

    Frustum frustum;
    frustum.planes[Left] = normalizedPlane(r3 + r0);
    frustum.planes[Right] = normalizedPlane(r3 - r0);
    frustum.planes[Bottom] = normalizedPlane(r3 + r1);
    frustum.planes[Top] = normalizedPlane(r3 - r1);
    frustum.planes[Near] = normalizedPlane(r3 + r2);
    frustum.planes[Far] = normalizedPlane(r3 - r2);
    return frustum;
}

bool Frustum::intersects(const core::Aabb& box) const noexcept {
    if (box.isEmpty()) {
        return false;
    }
    for (const Plane& plane : planes) {
        // Corner of the box furthest along the plane normal: if even that one is outside, the box is.
        const glm::vec3 positive(plane.normal.x >= 0.0f ? box.max.x : box.min.x,
                                 plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                                 plane.normal.z >= 0.0f ? box.max.z : box.min.z);
        if (plane.signedDistance(positive) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::contains(const glm::vec3& point) const noexcept {
    return std::all_of(planes.begin(), planes.end(),
                       [&point](const Plane& plane) { return plane.signedDistance(point) >= 0.0f; });
}

void Camera::setLens(const CameraLens& lens) noexcept {
    if (!isValidLens(lens)) {
        return;
    }
    m_lens = lens;
    m_lens.horizontalFovDeg = std::clamp(lens.horizontalFovDeg, kMinHorizontalFovDeg, kMaxHorizontalFovDeg);
}

void Camera::setAspect(float aspect) noexcept {
    if (std::isfinite(aspect) && aspect > 0.0f) {
        m_aspect = aspect;
    }
}

void Camera::setPose(const glm::vec3& position, float yawRadians, float pitchRadians) noexcept {
    for (int axis = 0; axis < 3; ++axis) {
        if (std::isfinite(position[axis])) {
            m_position[axis] = position[axis];
        }
    }
    if (std::isfinite(yawRadians)) {
        m_yaw = yawRadians;
    }
    if (std::isfinite(pitchRadians)) {
        m_pitch = pitchRadians;
    }
}

glm::vec3 Camera::forward() const noexcept {
    const float cosPitch = std::cos(m_pitch);
    return {std::sin(m_yaw) * cosPitch, std::sin(m_pitch), -std::cos(m_yaw) * cosPitch};
}

glm::vec3 Camera::right() const noexcept {
    return {std::cos(m_yaw), 0.0f, std::sin(m_yaw)};
}

glm::vec3 Camera::up() const noexcept {
    return glm::cross(right(), forward());
}

glm::mat4 Camera::view() const noexcept {
    return glm::lookAtRH(m_position, m_position + forward(), up());
}

glm::mat4 Camera::projection() const noexcept {
    return glm::perspectiveRH_NO(verticalFovRadians(), m_aspect, m_lens.nearPlane, m_lens.farPlane);
}

glm::mat4 Camera::viewProjection() const noexcept {
    return projection() * view();
}

Frustum Camera::frustum() const noexcept {
    return Frustum::fromViewProjection(viewProjection());
}

float Camera::verticalFovRadians() const noexcept {
    return verticalFovRadians(core::degreesToRadians(m_lens.horizontalFovDeg), m_aspect);
}

core::Ray Camera::rayFromNdc(const glm::vec2& ndc) const noexcept {
    const float tanHalfVertical = std::tan(0.5f * verticalFovRadians());
    const float tanHalfHorizontal = tanHalfVertical * m_aspect;
    const glm::vec3 direction = forward() + right() * (ndc.x * tanHalfHorizontal) + up() * (ndc.y * tanHalfVertical);
    return core::Ray{m_position, glm::normalize(direction)};
}

float Camera::verticalFovRadians(float horizontalFovRadians, float aspect) noexcept {
    const float safeAspect = (std::isfinite(aspect) && aspect > 0.0f) ? aspect : 1.0f;
    return 2.0f * std::atan(std::tan(0.5f * horizontalFovRadians) / safeAspect);
}

} // namespace rv::scene
