#include <rv/app/world_inspect.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <span>

#include <glm/gtc/quaternion.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
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
                            float distance) noexcept {
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
    // turn in them, the centre heldCentre() (left of and below the view's centre).
    const glm::vec3 forward = camera.forward();
    const glm::mat3 cameraAxes(camera.right(), camera.up(), -forward);
    const float yaw = std::isfinite(view.yaw) ? view.yaw : 0.0f;
    const float pitch = std::isfinite(view.pitch) ? view.pitch : 0.0f;
    const glm::mat3 held = cameraAxes * rotationX(-pitch) * rotationY(yaw);
    const float along = std::isfinite(distance) ? distance : InspectSettings{}.distance;
    const scene::RigidPose to{heldCentre(camera, along), glm::normalize(glm::quat_cast(held))};

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

std::optional<ConsoleSocket> consoleSocketOf(const scene::SceneDescription& description,
                                             std::string_view roomPrefix) {
    for (const scene::SceneObject& object : description.objects) {
        if (object.source != scene::SceneObject::Source::Prefab || !object.id.starts_with(roomPrefix) ||
            !object.id.ends_with(kConsoleIdSuffix)) {
            continue;
        }
        const std::span<const glm::mat4> transforms = object.instanceTransforms();
        if (transforms.empty()) {
            continue;
        }
        auto prefab = geometry::buildPrefab(object.kind, object.params);
        if (!prefab) {
            continue;
        }
        const geometry::Socket* socket = geometry::findSocket(prefab->sockets, kConsoleMediaSocket);
        if (socket == nullptr) {
            continue;
        }
        const glm::vec3 size = geometry::socketSize(*socket);
        return ConsoleSocket{transforms.front() * socket->transform, glm::vec2(size.x, size.z)};
    }
    return std::nullopt;
}

glm::vec3 itemExtents(const world::CollectionItem& item) noexcept {
    if (item.slotBox.isEmpty()) {
        return glm::vec3(0.0f);
    }
    // The slot box in the prefab's own (unscaled) space, then scaled as the transform scales each axis.
    const glm::mat4 toPrefab = glm::inverse(item.transform) * item.slotFrame;
    const core::Aabb local = item.slotBox.transformed(toPrefab);
    const glm::vec3 scale(glm::length(glm::vec3(item.transform[0])), glm::length(glm::vec3(item.transform[1])),
                          glm::length(glm::vec3(item.transform[2])));
    const glm::vec3 extents = (local.max - local.min) * scale;
    return finite(extents) ? extents : glm::vec3(0.0f);
}

float insertedScale(const glm::vec3& extents, const glm::vec2& opening) noexcept {
    float scale = 1.0f;
    if (extents.x > 0.0f && opening.x > 0.0f && std::isfinite(extents.x) && std::isfinite(opening.x)) {
        scale = std::min(scale, kInsertedFill * opening.x / extents.x);
    }
    if (extents.z > 0.0f && opening.y > 0.0f && std::isfinite(extents.z) && std::isfinite(opening.y)) {
        scale = std::min(scale, kInsertedFill * opening.y / extents.z);
    }
    // Half of it goes down into the console: no deeper than the console holds.
    if (extents.y > 0.0f && std::isfinite(extents.y)) {
        scale = std::min(scale, 2.0f * kInsertedHalfDepth / extents.y);
    }
    return scale;
}

glm::mat4 insertedItemTransform(const glm::mat4& held, const ConsoleSocket& socket, float scale,
                                float progress) noexcept {
    const float t = clamp01(progress);
    if (!(t > 0.0f)) {
        return held;
    }
    const glm::vec3 heldScale(glm::length(glm::vec3(held[0])), glm::length(glm::vec3(held[1])),
                              glm::length(glm::vec3(held[2])));
    const glm::vec3 socketScale(glm::length(glm::vec3(socket.transform[0])),
                                glm::length(glm::vec3(socket.transform[1])),
                                glm::length(glm::vec3(socket.transform[2])));
    if (!(heldScale.x > 0.0f) || !(heldScale.y > 0.0f) || !(heldScale.z > 0.0f) || !finite(heldScale) ||
        !(socketScale.x > 0.0f) || !(socketScale.y > 0.0f) || !(socketScale.z > 0.0f) || !finite(socketScale)) {
        return held;
    }
    const glm::mat3 heldRotation(glm::vec3(held[0]) / heldScale.x, glm::vec3(held[1]) / heldScale.y,
                                 glm::vec3(held[2]) / heldScale.z);
    const glm::mat3 socketRotation(glm::vec3(socket.transform[0]) / socketScale.x,
                                   glm::vec3(socket.transform[1]) / socketScale.y,
                                   glm::vec3(socket.transform[2]) / socketScale.z);
    const scene::RigidPose from{glm::vec3(held[3]), glm::normalize(glm::quat_cast(heldRotation))};
    const scene::RigidPose to{socket.position(), glm::normalize(glm::quat_cast(socketRotation))};
    const scene::RigidPose pose = t >= 1.0f ? to : scene::interpolatePose(from, to, t);
    const float shrink = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    const float factor = 1.0f + (shrink - 1.0f) * t;
    glm::mat4 transform(glm::mat3_cast(pose.orientation));
    transform[0] *= heldScale.x * factor;
    transform[1] *= heldScale.y * factor;
    transform[2] *= heldScale.z * factor;
    transform[3] = glm::vec4(pose.position, 1.0f);
    return transform;
}

InspectDimming insertionDimming(float lift, float insertion, float baseVignette) noexcept {
    return inspectDimming(clamp01(lift) * (1.0f - clamp01(insertion)), baseVignette);
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
