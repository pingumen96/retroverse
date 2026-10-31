#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace rv::spikes::xr {

/// A view's field of view as OpenXR reports it (XrFovf): the angles of its four edges from the view direction, in
/// radians; left and down are negative for a view that contains its own direction. The two eyes of a headset have
/// asymmetric fields (the inner edge is narrower than the outer one).
struct EyeFov {
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

/// A pose in a reference space (XrPosef): an orientation and a position, meters, +Y up, right-handed, the view looking
/// along -Z (ADR-0002 and OpenXR agree).
struct EyePose {
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
};

/// The off-axis perspective projection of `fov` with OpenGL's clip space (depth in [-1, 1], like the renderer's
/// glm::perspectiveRH_NO): a direction on an edge of the field maps to that edge of the NDC square.
[[nodiscard]] glm::mat4 projectionFromFov(const EyeFov& fov, float nearPlane, float farPlane) noexcept;

/// The view matrix of an eye at `pose`: the inverse of its rigid transform.
[[nodiscard]] glm::mat4 viewFromPose(const EyePose& pose) noexcept;

} // namespace rv::spikes::xr
