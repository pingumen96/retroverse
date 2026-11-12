#pragma once

// The motion of a scene object (I12 step 8, ADR-0021 §6: the rooms' life): a ceiling fan's blades turning, a wall
// clock's hands telling the time. An object with a motion turns about an axis through a pivot, both in its own frame
// (before its transform), by an angle its kind takes from the run's simulated seconds (a spin) or from the time of day
// (a clock's hand). GPU-free: the application moves the object's drawn items by motionDelta() each frame.

#include <cstdint>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace rv::scene {

/// The fastest spin an object may have, turns per second either way.
inline constexpr float kMaxSpinTurnsPerSecond = 4.0f;

struct ObjectMotion {
    enum class Kind : std::uint8_t {
        Spin,        ///< turnsPerSecond turns a second from the run's start (a fan's blades)
        ClockHour,   ///< a clock's hour hand: one turn in 12 hours, clockwise seen down the axis
        ClockMinute, ///< one turn an hour
        ClockSecond, ///< one step a second, a turn a minute (a quartz clock's)
    };

    Kind kind = Kind::Spin;
    /// The point it turns about and the axis it turns about (unit length; the clock's points out of its face), in the
    /// object's own frame.
    glm::vec3 pivot{0.0f};
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    /// Kind::Spin: turns a second, within [-kMaxSpinTurnsPerSecond, kMaxSpinTurnsPerSecond] (positive: counter-
    /// clockwise seen down the axis towards the pivot, the right-hand rule).
    float turnsPerSecond = 0.0f;

    bool operator==(const ObjectMotion&) const = default;
};

/// "spin", "clock-hour", "clock-minute", "clock-second".
[[nodiscard]] std::string_view toString(ObjectMotion::Kind kind) noexcept;

/// The seconds in a day (a clock's time of day is within [0, kSecondsPerDay)).
inline constexpr double kSecondsPerDay = 86400.0;

/// The angle (radians, about the motion's axis by the right-hand rule) `motion` stands at after `seconds` of the run
/// (Kind::Spin, wrapped to one turn) or at `clockSeconds` into the day (the clock's hands: the hour and minute hands
/// sweep, the second hand steps on whole seconds; clockwise seen from the face, so negative about an axis out of it;
/// 12 o'clock is angle 0). A time that is not finite stands at 0.
[[nodiscard]] float motionAngle(const ObjectMotion& motion, double seconds, double clockSeconds) noexcept;

/// What moves an item drawn for an object at `objectTransform` (local -> world) by `angle` about its motion's pivot
/// and axis: objectTransform * T(pivot) * R(axis, angle) * T(-pivot) * inverse(objectTransform), so that the item's
/// model becomes delta * its rest model.
[[nodiscard]] glm::mat4 motionDelta(const glm::mat4& objectTransform, const ObjectMotion& motion, float angle);

} // namespace rv::scene
