#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// The rooms' life (I12 step 8, ADR-0021 §6): the time the wall clocks tell and the angle each moving part of a scene
// stands at in a frame (a fan's blades turning with the run's simulated seconds, a clock's hands with the time of day),
// beside what RoomLife (room_life.cpp, rv_app) does with them and with the blinking LEDs and the lava lamp's wax.

#include <optional>
#include <string_view>

#include <rv/scene/object_motion.hpp>

namespace rv::app {

/// The time a frame-limited run's clocks start at without --clock (10:10:00, the hands' showroom pose), so that a
/// golden with a clock is the same at every run.
inline constexpr double kFrameLimitedClockSeconds = 10.0 * 3600.0 + 10.0 * 60.0;

/// "HH:MM" or "HH:MM:SS" (24 hours, two digits each) as seconds into the day; nothing for anything else.
[[nodiscard]] std::optional<double> parseClockTime(std::string_view text);

/// The seconds into the day a clock shows `seconds` after the run started at `startSeconds` (wrapped to a day).
[[nodiscard]] double clockAfter(double startSeconds, double seconds) noexcept;

/// The angle `motion` stands at after `seconds` of the run with the clocks at `clockSeconds`: scene::motionAngle(),
/// but under reduceMotion a spin stands still (0) and a clock's second hand at twelve (its hour and minute hands still
/// tell the time: they never visibly move).
[[nodiscard]] float lifeAngle(const scene::ObjectMotion& motion, double seconds, double clockSeconds,
                              bool reduceMotion) noexcept;

} // namespace rv::app
