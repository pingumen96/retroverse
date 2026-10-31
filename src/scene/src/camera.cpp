#include <rv/scene/camera.hpp>

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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

bool isValidFov(const EyeFov& fov) noexcept {
    constexpr float kLimit = 1.5707f; // just under 90 degrees: the tangents stay finite
    const auto edge = [](float angle) { return std::isfinite(angle) && std::abs(angle) < kLimit; };
    return edge(fov.left) && edge(fov.right) && edge(fov.up) && edge(fov.down) && fov.right > fov.left &&
           fov.up > fov.down;
}

} // namespace

glm::mat4 offAxisProjection(const EyeFov& fov, float nearPlane, float farPlane) noexcept {
    const float tanLeft = std::tan(fov.left);
    const float tanRight = std::tan(fov.right);
    const float tanUp = std::tan(fov.up);
    const float tanDown = std::tan(fov.down);
    const float width = tanRight - tanLeft;
    const float height = tanUp - tanDown;
    glm::mat4 m(0.0f);
    m[0][0] = 2.0f / width;
    m[1][1] = 2.0f / height;
    m[2][0] = (tanRight + tanLeft) / width;
    m[2][1] = (tanUp + tanDown) / height;
    m[2][2] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    m[2][3] = -1.0f;
    m[3][2] = -2.0f * farPlane * nearPlane / (farPlane - nearPlane);
    return m;
}

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

Frustum Frustum::enclosing(std::span<const glm::mat4> viewProjections) noexcept {
    if (viewProjections.empty()) {
        Frustum all;
        for (Plane& plane : all.planes) {
            plane = Plane{glm::vec3(0.0f), 0.0f};
        }
        return all;
    }
    Frustum frustum = fromViewProjection(viewProjections.front());
    for (const glm::mat4& other : viewProjections.subspan(1)) {
        for (const glm::vec3& corner : corners(other)) {
            for (Plane& plane : frustum.planes) {
                plane.d = std::max(plane.d, -glm::dot(plane.normal, corner));
            }
        }
    }
    return frustum;
}

std::array<glm::vec3, 8> Frustum::corners(const glm::mat4& viewProjection) noexcept {
    const glm::mat4 inverse = glm::inverse(viewProjection);
    std::array<glm::vec3, 8> points{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const glm::vec4 ndc((i & 1u) != 0 ? 1.0f : -1.0f, (i & 2u) != 0 ? 1.0f : -1.0f, (i & 4u) != 0 ? 1.0f : -1.0f,
                            1.0f);
        const glm::vec4 world = inverse * ndc;
        points[i] = glm::vec3(world) / world.w;
    }
    return points;
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
    m_eyeFov.reset();
}

void Camera::setEye(const glm::vec3& position, const glm::quat& orientation, const EyeFov& fov) noexcept {
    if (isValidFov(fov)) {
        m_eyeFov = fov;
    } else if (!m_eyeFov) {
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (std::isfinite(position[axis])) {
            m_position[axis] = position[axis];
        }
    }
    const float length = glm::length(orientation);
    if (std::isfinite(length) && length > 1e-6f) {
        m_orientation = orientation / length;
    }
    // yaw() and pitch() describe where the eye looks, as forward() does in the yaw/pitch mode.
    const glm::vec3 ahead = m_orientation * glm::vec3(0.0f, 0.0f, -1.0f);
    m_yaw = std::atan2(ahead.x, -ahead.z);
    m_pitch = std::asin(std::clamp(ahead.y, -1.0f, 1.0f));
}

glm::quat Camera::orientation() const noexcept {
    if (m_eyeFov) {
        return m_orientation;
    }
    // Yaw turns right about +Y (a negative right-handed angle), then pitch raises about the camera's right.
    return glm::angleAxis(-m_yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(m_pitch, glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::vec3 Camera::forward() const noexcept {
    if (m_eyeFov) {
        return m_orientation * glm::vec3(0.0f, 0.0f, -1.0f);
    }
    const float cosPitch = std::cos(m_pitch);
    return {std::sin(m_yaw) * cosPitch, std::sin(m_pitch), -std::cos(m_yaw) * cosPitch};
}

glm::vec3 Camera::right() const noexcept {
    if (m_eyeFov) {
        return m_orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    }
    return {std::cos(m_yaw), 0.0f, std::sin(m_yaw)};
}

glm::vec3 Camera::up() const noexcept {
    if (m_eyeFov) {
        return m_orientation * glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return glm::cross(right(), forward());
}

glm::mat4 Camera::view() const noexcept {
    if (m_eyeFov) {
        return glm::mat4_cast(glm::conjugate(m_orientation)) * glm::translate(glm::mat4(1.0f), -m_position);
    }
    return glm::lookAtRH(m_position, m_position + forward(), up());
}

glm::mat4 Camera::projection() const noexcept {
    if (m_eyeFov) {
        return offAxisProjection(*m_eyeFov, m_lens.nearPlane, m_lens.farPlane);
    }
    return glm::perspectiveRH_NO(verticalFovRadians(), m_aspect, m_lens.nearPlane, m_lens.farPlane);
}

glm::mat4 Camera::viewProjection() const noexcept {
    return projection() * view();
}

Frustum Camera::frustum() const noexcept {
    return Frustum::fromViewProjection(viewProjection());
}

float Camera::verticalFovRadians() const noexcept {
    if (m_eyeFov) {
        return m_eyeFov->up - m_eyeFov->down;
    }
    return verticalFovRadians(core::degreesToRadians(m_lens.horizontalFovDeg), m_aspect);
}

core::Ray Camera::rayFromNdc(const glm::vec2& ndc) const noexcept {
    if (m_eyeFov) {
        const float x = glm::mix(std::tan(m_eyeFov->left), std::tan(m_eyeFov->right), 0.5f * (ndc.x + 1.0f));
        const float y = glm::mix(std::tan(m_eyeFov->down), std::tan(m_eyeFov->up), 0.5f * (ndc.y + 1.0f));
        return core::Ray{m_position, glm::normalize(forward() + right() * x + up() * y)};
    }
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
