#pragma once

#include <array>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <rv/scene/camera.hpp>

namespace rv::xr {

/// A pose in a space (XrPosef): an orientation and a position, meters, +Y up, right-handed, a view looking along -Z
/// (ADR-0002 and OpenXR agree).
struct Pose {
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
};

/// An eye as the runtime located it (XrView): its pose and the four angles of its field.
struct EyeView {
    Pose pose;
    scene::EyeFov fov;
};

/// The left eye, then the right.
using EyeViews = std::array<EyeView, 2>;

/// Where the tracking space sits in the world (ADR-0017 §5): its origin on the walker's feet, turned by the walker's
/// yaw, and raised by `floorOffset` when the space's origin is not on the floor (LOCAL: the head's height when the
/// session started, assumed).
struct TrackingAnchor {
    glm::vec3 feet{0.0f};
    /// The walker's yaw (scene::Camera's: 0 looks along -Z, positive turns right).
    float yawRadians = 0.0f;
    float floorOffset = 0.0f;
};

/// `tracked` (a pose in the tracking space) in the world.
[[nodiscard]] Pose toWorld(const TrackingAnchor& anchor, const Pose& tracked) noexcept;

/// The head of two eyes: midway between them, its orientation halfway between theirs.
[[nodiscard]] Pose headOf(const EyeViews& eyes) noexcept;

/// The field of a view from the head that holds both eyes' fields: the left eye's left edge, the right eye's right
/// edge, the higher top and the lower bottom (the eyes' offsets ignored).
[[nodiscard]] scene::EyeFov combinedField(const EyeViews& eyes) noexcept;

/// An orientation as yaw, pitch and roll, radians: yaw and pitch like scene::Camera's (where its -Z looks), roll the
/// turn about that direction (positive tilts the head to the right).
struct Angles {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

[[nodiscard]] Angles anglesOf(const glm::quat& orientation) noexcept;
/// The orientation of `angles`: anglesOf()'s inverse away from straight up or down.
[[nodiscard]] glm::quat orientationOf(const Angles& angles) noexcept;

} // namespace rv::xr
