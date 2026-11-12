#include <rv/scene/object_motion.hpp>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

namespace rv::scene {
namespace {

constexpr double kTwoPi = 6.283185307179586;

/// The share of a turn `value` is into a turn of `period`, in [0, 1).
double turnShare(double value, double period) {
    const double share = std::fmod(value / period, 1.0);
    return share < 0.0 ? share + 1.0 : share;
}

} // namespace

std::string_view toString(ObjectMotion::Kind kind) noexcept {
    switch (kind) {
    case ObjectMotion::Kind::Spin:
        return "spin";
    case ObjectMotion::Kind::ClockHour:
        return "clock-hour";
    case ObjectMotion::Kind::ClockMinute:
        return "clock-minute";
    case ObjectMotion::Kind::ClockSecond:
        return "clock-second";
    }
    return "spin";
}

float motionAngle(const ObjectMotion& motion, double seconds, double clockSeconds) noexcept {
    double share = 0.0;
    switch (motion.kind) {
    case ObjectMotion::Kind::Spin:
        if (!std::isfinite(seconds)) {
            return 0.0f;
        }
        share = turnShare(seconds * static_cast<double>(motion.turnsPerSecond), 1.0);
        return static_cast<float>(share * kTwoPi);
    case ObjectMotion::Kind::ClockHour:
        share = turnShare(clockSeconds, 12.0 * 3600.0);
        break;
    case ObjectMotion::Kind::ClockMinute:
        share = turnShare(clockSeconds, 3600.0);
        break;
    case ObjectMotion::Kind::ClockSecond:
        share = std::isfinite(clockSeconds) ? turnShare(std::floor(clockSeconds), 60.0) : 0.0;
        break;
    }
    if (!std::isfinite(clockSeconds)) {
        return 0.0f;
    }
    return static_cast<float>(-share * kTwoPi);
}

glm::mat4 motionDelta(const glm::mat4& objectTransform, const ObjectMotion& motion, float angle) {
    const glm::mat4 identity(1.0f);
    const glm::mat4 local = glm::translate(identity, motion.pivot) * glm::rotate(identity, angle, motion.axis) *
                            glm::translate(identity, -motion.pivot);
    return objectTransform * local * glm::inverse(objectTransform);
}

} // namespace rv::scene
