#include <rv/app/world_inspect.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/quaternion.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/pose_animator.hpp>

namespace rv::app {
namespace {

[[nodiscard]] bool finite(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] float clamp01(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

/// A rotation of `radians` about +Y (+Z turns towards +X for a positive angle).
[[nodiscard]] glm::mat3 rotationY(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return glm::mat3(glm::vec3(c, 0.0f, -s), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(s, 0.0f, c));
}

/// A rotation of `radians` about +X (+Z turns towards -Y for a positive angle).
[[nodiscard]] glm::mat3 rotationX(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return glm::mat3(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, c, s), glm::vec3(0.0f, -s, c));
}

} // namespace

glm::vec3 heldCentre(const scene::Camera& camera, float distance) noexcept {
    const InspectSettings settings;
    const float fade = distance <= settings.distance
                           ? 1.0f
                           : std::clamp((settings.maxDistance - distance) / (settings.maxDistance - settings.distance),
                                        0.0f, 1.0f);
    const glm::vec2 offset = kHeldViewOffset * (distance * fade);
    return camera.position() + camera.forward() * distance + camera.right() * offset.x + camera.up() * offset.y;
}

float heldRadius(const world::CollectionItem& item) noexcept {
    return item.slotBox.isEmpty() ? 0.0f : glm::length(item.slotBox.halfExtents());
}

float heldDistanceLimit(const core::Aabb& clusterBounds, float radius, const glm::vec3& eye,
                        const glm::vec3& forward) noexcept {
    constexpr float kNone = std::numeric_limits<float>::infinity();
    if (clusterBounds.isEmpty() || !finite(clusterBounds.min) || !finite(clusterBounds.max) || !finite(eye) ||
        !finite(forward) || !std::isfinite(radius) || glm::length(forward) <= 0.0f) {
        return kNone;
    }
    const core::Aabb grown = clusterBounds.inflated(kHeldClearance);
    if (grown.contains(eye)) {
        return kNone;
    }
    const std::optional<float> entry = grown.intersectRay(core::Ray{eye, glm::normalize(forward)});
    return entry ? *entry - std::max(radius, 0.0f) : kNone;
}

float heldDistance(float distance, float limit) noexcept {
    const float wanted = std::isfinite(distance) ? distance : InspectSettings{}.distance;
    const float bound = std::isnan(limit) ? wanted : std::min(wanted, limit);
    return std::max(bound, kHeldMinDistance);
}

glm::mat4 heldItemTransform(const glm::mat4& pulled, const InspectView& view, const scene::Camera& camera,
                            float distance, const std::optional<scene::RigidPose>& hand) noexcept {
    const float lift = clamp01(view.lift);
    if (!(lift > 0.0f)) {
        return pulled; // at its pulled-out pose exactly
    }
    // The pulled pose as a rigid pose and its scale (the prefab's axes may be scaled with the item's size).
    const glm::vec3 scale(glm::length(glm::vec3(pulled[0])), glm::length(glm::vec3(pulled[1])),
                          glm::length(glm::vec3(pulled[2])));
    if (!(scale.x > 0.0f) || !(scale.y > 0.0f) || !(scale.z > 0.0f) || !finite(scale)) {
        return pulled;
    }
    const glm::mat3 pulledRotation(glm::vec3(pulled[0]) / scale.x, glm::vec3(pulled[1]) / scale.y,
                                   glm::vec3(pulled[2]) / scale.z);
    const scene::RigidPose from{glm::vec3(pulled[3]), glm::normalize(glm::quat_cast(pulledRotation))};

    // In the hand: the camera's axes (right, up, back: right-handed, since up = right x forward), the inspection's
    // turn in them, the centre heldCentre() (left of and below the view's centre). In VR the controller's grip in
    // their place, the item in front of it.
    const float yaw = std::isfinite(view.yaw) ? view.yaw : 0.0f;
    const float pitch = std::isfinite(view.pitch) ? view.pitch : 0.0f;
    scene::RigidPose to;
    if (hand && finite(hand->position)) {
        const glm::mat3 handAxes = glm::mat3_cast(glm::normalize(hand->orientation));
        const glm::mat3 held = handAxes * rotationX(-pitch) * rotationY(yaw);
        to = scene::RigidPose{hand->position - handAxes[2] * kHandHoldDistance, glm::normalize(glm::quat_cast(held))};
    } else {
        const glm::vec3 forward = camera.forward();
        const glm::mat3 cameraAxes(camera.right(), camera.up(), -forward);
        const glm::mat3 held = cameraAxes * rotationX(-pitch) * rotationY(yaw);
        const float along = std::isfinite(distance) ? distance : InspectSettings{}.distance;
        to = scene::RigidPose{heldCentre(camera, along), glm::normalize(glm::quat_cast(held))};
    }

    const scene::RigidPose pose = lift >= 1.0f ? to : scene::interpolatePose(from, to, lift);
    glm::mat4 transform(glm::mat3_cast(pose.orientation));
    transform[0] *= scale.x;
    transform[1] *= scale.y;
    transform[2] *= scale.z;
    transform[3] = glm::vec4(pose.position, 1.0f);
    return transform;
}

InspectDimming inspectDimming(float lift, float baseVignette) noexcept {
    const float share = clamp01(lift);
    const float base = clamp01(baseVignette);
    return InspectDimming{kInspectDimShare * share, std::min(1.0f, base + kInspectVignette * share)};
}

std::optional<std::string> screenSelection(const std::optional<std::string>& catalogSelection, WorldState state,
                                           std::string_view focusedGame) {
    if (catalogSelection) {
        return catalogSelection;
    }
    if (state == WorldState::FreeRoam || focusedGame.empty()) {
        return std::nullopt;
    }
    return std::string(focusedGame);
}

} // namespace rv::app
