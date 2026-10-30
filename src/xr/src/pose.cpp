#include <rv/xr/pose.hpp>

#include <algorithm>
#include <cmath>

namespace rv::xr {
namespace {

constexpr glm::vec3 kUp(0.0f, 1.0f, 0.0f);
constexpr glm::vec3 kRight(1.0f, 0.0f, 0.0f);
constexpr glm::vec3 kForward(0.0f, 0.0f, -1.0f);

glm::quat yawRotation(float yawRadians) noexcept {
    return glm::angleAxis(-yawRadians, kUp);
}

} // namespace

Pose toWorld(const TrackingAnchor& anchor, const Pose& tracked) noexcept {
    const glm::quat turn = yawRotation(anchor.yawRadians);
    const glm::vec3 offset = tracked.position + glm::vec3(0.0f, anchor.floorOffset, 0.0f);
    return Pose{glm::normalize(turn * tracked.orientation), anchor.feet + turn * offset};
}

Pose headOf(const EyeViews& eyes) noexcept {
    return Pose{glm::normalize(glm::slerp(eyes[0].pose.orientation, eyes[1].pose.orientation, 0.5f)),
                0.5f * (eyes[0].pose.position + eyes[1].pose.position)};
}

scene::EyeFov combinedField(const EyeViews& eyes) noexcept {
    return scene::EyeFov{eyes[0].fov.left, eyes[1].fov.right, std::max(eyes[0].fov.up, eyes[1].fov.up),
                         std::min(eyes[0].fov.down, eyes[1].fov.down)};
}

Angles anglesOf(const glm::quat& orientation) noexcept {
    const glm::quat q = glm::normalize(orientation);
    const glm::vec3 forward = q * kForward;
    Angles angles;
    angles.yaw = std::atan2(forward.x, -forward.z);
    angles.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    // What is left once the yaw and the pitch are undone is a turn about -Z: the right vector's tilt.
    const glm::quat rest =
        glm::inverse(yawRotation(angles.yaw) * glm::angleAxis(angles.pitch, kRight)) * q;
    const glm::vec3 right = rest * kRight;
    angles.roll = std::atan2(-right.y, right.x);
    return angles;
}

glm::quat orientationOf(const Angles& angles) noexcept {
    return glm::normalize(yawRotation(angles.yaw) * glm::angleAxis(angles.pitch, kRight) *
                          glm::angleAxis(angles.roll, kForward));
}

} // namespace rv::xr
