#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace rv::spikes::launch {

using Clock = std::chrono::steady_clock;

/// Milliseconds from `origin` to `point`, negative when `point` comes first.
[[nodiscard]] double millisecondsBetween(Clock::time_point origin, Clock::time_point point) noexcept;

/// When frame pacing counts as back at 60 FPS: `window` consecutive frame intervals of at most `maxInterval` (20.8 ms,
/// 1.25 frames at 60 Hz: one missed vblank at 60 Hz is 33.3 ms, so a display at 60 Hz or faster passes only without
/// dropped frames).
struct PacingCriterion {
    std::chrono::microseconds maxInterval{20833};
    std::size_t window = 30;
};

/// The index of the first frame that starts `criterion.window` consecutive intervals within `criterion.maxInterval`,
/// given the time each frame was presented (in order); nothing when no such run exists.
[[nodiscard]] std::optional<std::size_t> steadyFrom(std::span<const Clock::time_point> presented,
                                                    const PacingCriterion& criterion = {});

/// Statistics of frame intervals, in milliseconds (nearest-rank percentiles).
struct IntervalStats {
    std::size_t count = 0;
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};

/// The intervals between consecutive presented frames. `scratch` holds them (reused: no allocation once grown).
[[nodiscard]] IntervalStats intervalStats(std::span<const Clock::time_point> presented, std::vector<double>& scratch);

/// The return from an emulator, in milliseconds after its exit (the system's exit time of the authoritative process):
/// the roadmap's criteria for I1 (f) with the pad input.
struct ReturnTimes {
    std::optional<double> restored;   ///< (a) the window is no longer minimized
    std::optional<double> foreground; ///< (b) the window is the foreground window again
    std::optional<double> fullscreen; ///< (c) borderless fullscreen over its monitor again
    std::optional<double> firstFrame; ///< (d) the first frame submitted after the return (SwapBuffers returned; with
                                      ///< vsync it reaches the screen later, which steady60 includes)
    std::optional<double> steady60;   ///< (e) frame pacing back at 60 FPS (steadyFrom)
    std::optional<double> input;      ///< (f) a gamepad press injected after the return reached the input collector

    /// The latest of the six; nothing when one is missing.
    [[nodiscard]] std::optional<double> latest() const noexcept;
};

/// Every time of `times` is present and at most `limitMs` (1000: "within 1 s of the emulator's exit").
[[nodiscard]] bool meetsCriteria(const ReturnTimes& times, double limitMs = 1000.0) noexcept;

/// Median, best (smallest) and worst of a set of measurements.
struct Summary {
    std::size_t count = 0;
    double median = 0.0;
    double best = 0.0;
    double worst = 0.0;
};

/// Summary of `values`; count 0 when empty. The median of an even count is the mean of the middle two.
[[nodiscard]] Summary summarize(std::vector<double> values);

[[nodiscard]] nlohmann::json toJson(const IntervalStats& stats);
[[nodiscard]] nlohmann::json toJson(const ReturnTimes& times);
[[nodiscard]] nlohmann::json toJson(const Summary& summary);

} // namespace rv::spikes::launch
