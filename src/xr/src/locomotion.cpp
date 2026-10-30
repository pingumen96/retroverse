#include <rv/xr/locomotion.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <rv/core/math.hpp>

namespace rv::xr {
namespace {

constexpr glm::vec3 kUp(0.0f, 1.0f, 0.0f);
constexpr glm::vec3 kForward(0.0f, 0.0f, -1.0f);

glm::quat yawRotation(float yawRadians) noexcept {
    return glm::angleAxis(-yawRadians, kUp);
}

/// Wraps to (-pi, pi], leaving in-range angles bit-exact.
float wrapYaw(float radians) noexcept {
    return (radians > -core::kPi && radians <= core::kPi) ? radians : core::wrapAngleRadians(radians);
}

float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

glm::vec2 xz(const glm::vec3& v) noexcept {
    return glm::vec2(v.x, v.z);
}

bool finitePose(const Pose& pose) noexcept {
    return core::isFinite(pose.position) && std::isfinite(pose.orientation.w) && std::isfinite(pose.orientation.x) &&
           std::isfinite(pose.orientation.y) && std::isfinite(pose.orientation.z) &&
           glm::dot(pose.orientation, pose.orientation) > 0.0f;
}

float smoothstep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// The fraction of the segment from `from` to `to` where it enters `box` (0 when it starts inside), if it does.
std::optional<float> entersBox(const glm::vec3& from, const glm::vec3& to, const core::Aabb& box) noexcept {
    float enter = 0.0f;
    float leave = 1.0f;
    const glm::vec3 delta = to - from;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(delta[axis]) < 1e-9f) {
            if (from[axis] < box.min[axis] || from[axis] > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float into = (box.min[axis] - from[axis]) / delta[axis];
        float out = (box.max[axis] - from[axis]) / delta[axis];
        if (into > out) {
            std::swap(into, out);
        }
        enter = std::max(enter, into);
        leave = std::min(leave, out);
        if (enter > leave) {
            return std::nullopt;
        }
    }
    return enter;
}

/// The first collider standing above the floor that the segment enters: the fraction where it does.
std::optional<float> firstCollider(const glm::vec3& from, const glm::vec3& to, float floorY,
                                   const scene::CollisionWorld& collision) noexcept {
    std::optional<float> first;
    for (const scene::ColliderBox& collider : collision.boxes()) {
        if (collider.box.isEmpty() || collider.box.max.y <= floorY + kArcFloorTolerance) {
            continue;
        }
        const std::optional<float> enters = entersBox(from, to, collider.box);
        if (enters && (!first || *enters < *first)) {
            first = enters;
        }
    }
    return first;
}

} // namespace

std::string_view toString(TurnMode mode) noexcept {
    switch (mode) {
    case TurnMode::Snap:
        return "snap";
    case TurnMode::Smooth:
        return "smooth";
    }
    return "snap";
}

std::string_view toString(Posture posture) noexcept {
    switch (posture) {
    case Posture::Standing:
        return "standing";
    case Posture::Seated:
        return "seated";
    }
    return "standing";
}

TeleportArc computeTeleportArc(const glm::vec3& start, const glm::vec3& direction, float floorY,
                               const scene::CollisionWorld* collision, const scene::Capsule& capsule) {
    TeleportArc arc;
    const float length = glm::length(direction);
    if (!core::isFinite(start) || !core::isFinite(direction) || !std::isfinite(floorY) || !(length > 1e-6f)) {
        return arc;
    }
    const glm::vec3 velocity = direction * (kArcSpeed / length);
    const int steps = static_cast<int>(std::lround(kArcMaxSeconds / kArcStepSeconds));
    arc.points.reserve(static_cast<std::size_t>(steps) + 1);
    arc.points.push_back(start);
    glm::vec3 previous = start;
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) * kArcStepSeconds;
        const glm::vec3 next = start + velocity * t + glm::vec3(0.0f, -0.5f * kArcGravity * t * t, 0.0f);
        // Down through the floor in this step: where, unless a collider comes first.
        std::optional<float> floorAt;
        if (previous.y > floorY && next.y <= floorY) {
            floorAt = (previous.y - floorY) / (previous.y - next.y);
        }
        if (collision != nullptr) {
            const glm::vec3 end = floorAt ? glm::mix(previous, next, *floorAt) : next;
            if (const std::optional<float> hit = firstCollider(previous, end, floorY, *collision)) {
                arc.points.push_back(glm::mix(previous, end, *hit));
                return arc;
            }
        }
        if (floorAt) {
            glm::vec3 landing = glm::mix(previous, next, *floorAt);
            landing.y = floorY;
            arc.points.push_back(landing);
            if (collision == nullptr || !collision->overlaps(landing, capsule)) {
                arc.landing = landing;
            }
            return arc;
        }
        arc.points.push_back(next);
        previous = next;
    }
    return arc;
}

float castRay(const core::Ray& ray, float maxDistance, float floorY, const scene::CollisionWorld* collision) noexcept {
    if (collision == nullptr || !core::isFinite(ray.origin) || !core::isFinite(ray.direction) ||
        !std::isfinite(maxDistance) || !std::isfinite(floorY)) {
        return maxDistance;
    }
    float reach = maxDistance;
    // Down to the floor at most.
    if (ray.direction.y < 0.0f && ray.origin.y > floorY) {
        reach = std::min(reach, (floorY - ray.origin.y) / ray.direction.y);
    }
    const glm::vec3 end = ray.origin + ray.direction * reach;
    if (const std::optional<float> hit = firstCollider(ray.origin, end, floorY, *collision)) {
        return *hit * reach;
    }
    return reach;
}

void Locomotion::setSettings(const LocomotionSettings& settings) noexcept {
    const LocomotionSettings defaults;
    LocomotionSettings applied = settings;
    applied.walkSpeed = std::max(0.0f, finiteOr(settings.walkSpeed, defaults.walkSpeed));
    applied.sprintSpeed = std::max(0.0f, finiteOr(settings.sprintSpeed, defaults.sprintSpeed));
    applied.eyeHeight = std::max(0.0f, finiteOr(settings.eyeHeight, defaults.eyeHeight));
    applied.comfort.snapDegrees =
        std::clamp(finiteOr(settings.comfort.snapDegrees, defaults.comfort.snapDegrees), 1.0f, 180.0f);
    applied.comfort.smoothDegreesPerSecond = std::clamp(
        finiteOr(settings.comfort.smoothDegreesPerSecond, defaults.comfort.smoothDegreesPerSecond), 1.0f, 720.0f);
    applied.comfort.vignette = std::clamp(finiteOr(settings.comfort.vignette, defaults.comfort.vignette), 0.0f, 1.0f);
    m_settings = applied;
}

void Locomotion::place(const glm::vec3& feet, float yawRadians) noexcept {
    if (core::isFinite(feet) && std::isfinite(yawRadians)) {
        m_placement = Placement{feet, wrapYaw(yawRadians)};
    }
}

void Locomotion::recenter() noexcept {
    m_recenter = true;
}

glm::vec3 Locomotion::worldOf(const glm::vec3& tracked) const noexcept {
    return m_origin + yawRotation(m_yaw) * (tracked + glm::vec3(0.0f, m_floorOffset + m_heightOffset, 0.0f));
}

void Locomotion::anchorAt(const glm::vec2& world, const glm::vec3& tracked) noexcept {
    const glm::vec3 turned = yawRotation(m_yaw) * tracked;
    m_origin.x = world.x - turned.x;
    m_origin.z = world.y - turned.z;
}

void Locomotion::update(const Pose& head, float floorOffset, const LocomotionInput& input, float dt,
                        const scene::CollisionWorld* collision) {
    if (!finitePose(head)) {
        return;
    }
    const float seconds = (std::isfinite(dt) && dt > 0.0f) ? dt : 0.0f;
    const scene::Capsule& capsule = m_settings.capsule;
    m_floorOffset = finiteOr(floorOffset, 0.0f);
    m_blink = std::max(0.0f, m_blink - seconds);
    const Angles tracked = anglesOf(head.orientation);

    // The seated posture's height, then where the anchor goes: a placement (the first update's at the walker's start),
    // or a recentering that keeps the head where it was.
    if (!m_started && !m_placement) {
        m_placement = Placement{m_feet, m_heading};
    }
    if (!m_started || m_recenter) {
        m_heightOffset = m_settings.posture == Posture::Seated
                             ? m_settings.eyeHeight - (head.position.y + m_floorOffset)
                             : 0.0f;
    }
    if (m_placement) {
        m_yaw = wrapYaw(m_placement->yaw - tracked.yaw);
        m_feet = m_placement->feet;
        m_origin.y = m_feet.y;
        anchorAt(xz(m_feet), head.position);
        m_placement.reset();
        m_aiming = false;
        m_arc.reset();
    } else if (m_recenter) {
        m_yaw = wrapYaw(m_heading - tracked.yaw);
        anchorAt(xz(m_head.position), head.position);
    }
    m_started = true;
    m_recenter = false;

    // Turning, about the head: it stays where it is in the world.
    float turn = finiteOr(input.turnRadians, 0.0f);
    const float turnAxis = std::clamp(finiteOr(input.turn, 0.0f), -1.0f, 1.0f);
    bool smoothTurn = turn != 0.0f;
    if (m_settings.comfort.turn == TurnMode::Snap) {
        if (std::abs(turnAxis) < kSnapRelease) {
            m_snapArmed = true;
        } else if (m_snapArmed && std::abs(turnAxis) >= kSnapPress) {
            turn += std::copysign(core::degreesToRadians(m_settings.comfort.snapDegrees), turnAxis);
            m_snapArmed = false;
            ++m_counters.snapTurns;
        }
    } else if (const float smooth =
                   turnAxis * core::degreesToRadians(m_settings.comfort.smoothDegreesPerSecond) * seconds;
               smooth != 0.0f) {
        turn += smooth;
        smoothTurn = true;
    }
    if (turn != 0.0f) {
        const glm::vec3 headWorld = worldOf(head.position);
        m_yaw = wrapYaw(m_yaw + turn);
        anchorAt(xz(headWorld), head.position);
    }
    m_heading = wrapYaw(m_yaw + tracked.yaw);
    m_pitch = tracked.pitch;

    // The stick walks along the head's heading (or the left controller's), through the capsule, carrying the anchor.
    float walkHeading = m_heading;
    if (m_settings.comfort.moveWithHand && input.moveAlong && finitePose(*input.moveAlong)) {
        walkHeading = wrapYaw(m_yaw + anglesOf(input.moveAlong->orientation).yaw);
    }
    glm::vec2 move(finiteOr(input.move.x, 0.0f), finiteOr(input.move.y, 0.0f));
    if (const float length = glm::length(move); length > 1.0f) {
        move /= length;
    }
    const float speed = input.sprint ? m_settings.sprintSpeed : m_settings.walkSpeed;
    const glm::vec3 forward(std::sin(walkHeading), 0.0f, -std::cos(walkHeading));
    const glm::vec3 right(std::cos(walkHeading), 0.0f, std::sin(walkHeading));
    const glm::vec3 step = (forward * move.y + right * move.x) * (speed * seconds);
    const bool walking = (step.x != 0.0f || step.z != 0.0f) && core::isFinite(step);
    if (walking) {
        const glm::vec3 moved =
            collision != nullptr ? collision->moveCapsule(m_feet, step, capsule) : m_feet + step;
        m_origin += glm::vec3(moved.x - m_feet.x, 0.0f, moved.z - m_feet.z);
        m_feet = moved;
    }

    // The capsule follows the head; a head leaning beyond it fades the view, and further than kMaxHeadOffset is
    // pushed back.
    glm::vec3 headWorld = worldOf(head.position);
    const glm::vec3 under(headWorld.x, m_feet.y, headWorld.z);
    m_feet = collision != nullptr ? collision->moveCapsule(m_feet, under - m_feet, capsule) : under;
    const glm::vec2 offset = xz(headWorld) - xz(m_feet);
    float leaning = glm::length(offset);
    m_counters.largestHeadOffset = std::max(m_counters.largestHeadOffset, leaning);
    if (leaning > kMaxHeadOffset) {
        const glm::vec2 push = offset * (1.0f - kMaxHeadOffset / leaning);
        m_origin.x -= push.x;
        m_origin.z -= push.y;
        headWorld = worldOf(head.position);
        leaning = kMaxHeadOffset;
        ++m_counters.pushedFrames;
    }
    m_wallFade = smoothstep(kFadeStart, kMaxHeadOffset, leaning);

    // The teleport: aimed while the stick is up, the walker lands where the arc shown last came down once let go.
    const float aim = finiteOr(input.aim, 0.0f);
    if (!m_aiming && aim >= kAimPress) {
        m_aiming = true;
    }
    if (m_aiming && aim < kAimRelease) {
        if (m_arc && m_arc->landing) {
            const glm::vec3 landing = *m_arc->landing;
            m_origin += glm::vec3(landing.x - headWorld.x, landing.y - m_feet.y, landing.z - headWorld.z);
            m_feet = landing;
            headWorld = worldOf(head.position);
            m_blink = kBlinkSeconds;
            m_wallFade = 0.0f;
            ++m_counters.teleports;
        }
        m_aiming = false;
        m_arc.reset();
    } else if (m_aiming) {
        if (input.arcFrom && finitePose(*input.arcFrom)) {
            const glm::quat orientation = yawRotation(m_yaw) * input.arcFrom->orientation;
            m_arc = computeTeleportArc(worldOf(input.arcFrom->position), orientation * kForward, m_feet.y, collision,
                                       capsule);
        } else {
            const glm::quat orientation = yawRotation(m_yaw) * head.orientation;
            m_arc = computeTeleportArc(headWorld - glm::vec3(0.0f, kArcBelowHead, 0.0f), orientation * kForward,
                                       m_feet.y, collision, capsule);
        }
    }

    // The comfort vignette closes in while the stick walks or turns the user smoothly.
    const float target = walking || smoothTurn ? m_settings.comfort.vignette : 0.0f;
    const float rate = seconds / kTunnelSeconds;
    m_tunnel = target > m_tunnel ? std::min(target, m_tunnel + rate) : std::max(target, m_tunnel - rate);

    m_head = Pose{glm::normalize(yawRotation(m_yaw) * head.orientation), headWorld};
}

TrackingAnchor Locomotion::anchor() const noexcept {
    return TrackingAnchor{m_origin, m_yaw, m_floorOffset + m_heightOffset};
}

float Locomotion::fade() const noexcept {
    return std::max(m_wallFade, m_blink / kBlinkSeconds);
}

} // namespace rv::xr
