#include <spike_xr/xr_math.hpp>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace rv::spikes::xr {

glm::mat4 projectionFromFov(const EyeFov& fov, float nearPlane, float farPlane) noexcept {
    const float tanLeft = std::tan(fov.left);
    const float tanRight = std::tan(fov.right);
    const float tanUp = std::tan(fov.up);
    const float tanDown = std::tan(fov.down);
    const float width = tanRight - tanLeft;
    const float height = tanUp - tanDown;

    // glm is column-major: m[column][row].
    glm::mat4 m(0.0f);
    m[0][0] = 2.0f / width;
    m[1][1] = 2.0f / height;
    m[2][0] = (tanRight + tanLeft) / width;
    m[2][1] = (tanUp + tanDown) / height;
    m[2][2] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    m[2][3] = -1.0f;
    m[3][2] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
    return m;
}

glm::mat4 viewFromPose(const EyePose& pose) noexcept {
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), pose.position) * glm::mat4_cast(pose.orientation);
    return glm::inverse(world);
}

} // namespace rv::spikes::xr
