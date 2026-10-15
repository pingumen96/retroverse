#include <rv/scene/viewpoint.hpp>

#include <algorithm>
#include <cmath>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

/// Half the extent of a box with `halfExtents` along the horizontal unit `direction` (its support distance).
float halfExtentAlong(const glm::vec3& halfExtents, const glm::vec3& direction) noexcept {
    return std::abs(direction.x) * halfExtents.x + std::abs(direction.z) * halfExtents.z;
}

/// Yaw of a walker looking along the horizontal unit `forward` (Camera: forward = (sin yaw, 0, -cos yaw)).
float yawLookingAlong(const glm::vec3& forward) noexcept {
    return std::atan2(forward.x, -forward.z);
}

} // namespace

glm::vec3 frontDirection(const glm::mat4& transform) noexcept {
    const glm::vec3 localFront = glm::vec3(transform * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    const glm::vec3 horizontal(localFront.x, 0.0f, localFront.z);
    const float length = glm::length(horizontal);
    if (!std::isfinite(length) || length < 1e-4f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return horizontal / length;
}

WalkerPose frontViewpoint(const core::Aabb& bounds, const glm::vec3& front, float floorY, float eyeHeight,
                          const CameraLens& lens, float aspect) noexcept {
    const bool usable = !bounds.isEmpty() && core::isFinite(bounds.min) && core::isFinite(bounds.max);
    const glm::vec3 center = usable ? bounds.center() : glm::vec3(0.0f);
    const glm::vec3 halfExtents = usable ? bounds.halfExtents() : glm::vec3(0.0f);

    WalkerPose pose;
    pose.yaw = yawLookingAlong(-front);
    if (2.0f * std::max(halfExtents.x, halfExtents.z) > kLargeObjectExtent) {
        pose.feet = glm::vec3(center.x, floorY, center.z);
        return pose;
    }

    // Fit the object's width (across the view) and height in the view, measured from its front side.
    const glm::vec3 across(-front.z, 0.0f, front.x);
    const float horizontalFov = core::degreesToRadians(lens.horizontalFovDeg);
    const float tanHalfHorizontal = std::tan(0.5f * horizontalFov);
    const float tanHalfVertical = std::tan(0.5f * Camera::verticalFovRadians(horizontalFov, aspect));
    const float fit =
        std::max(halfExtentAlong(halfExtents, across) / tanHalfHorizontal, halfExtents.y / tanHalfVertical);
    const float distance = std::clamp(fit * kViewMargin, kMinViewDistance, kMaxViewDistance);
    const glm::vec3 standOn = center + front * (halfExtentAlong(halfExtents, front) + distance);
    pose.feet = glm::vec3(standOn.x, floorY, standOn.z);

    const glm::vec3 eye = pose.feet + glm::vec3(0.0f, eyeHeight, 0.0f);
    const glm::vec3 toCenter = center - eye;
    pose.pitch = std::atan2(toCenter.y, glm::length(glm::vec2(toCenter.x, toCenter.z)));
    return pose;
}

} // namespace rv::scene
