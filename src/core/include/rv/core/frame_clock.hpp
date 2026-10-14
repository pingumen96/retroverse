#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace rv::core {

/// Per-frame timing based on std::chrono::steady_clock. tickAt() makes it deterministic in tests.
class FrameClock {
public:
    using Clock = std::chrono::steady_clock;

    /// Samples Clock::now() and advances one frame.
    void tick();
    /// Advances one frame at an explicit time point. Time going backwards yields a zero delta.
    void tickAt(Clock::time_point now);

    /// Seconds between the last two ticks (0 after the first tick).
    [[nodiscard]] double deltaSeconds() const noexcept { return m_delta; }
    /// Delta clamped to `maxDelta`, so a breakpoint or a window drag does not teleport the camera.
    [[nodiscard]] double clampedDeltaSeconds(double maxDelta = 0.1) const noexcept;
    /// Exponential moving average of the delta (alpha = 0.1).
    [[nodiscard]] double smoothedDeltaSeconds() const noexcept { return m_smoothed; }
    /// Seconds since the first tick.
    [[nodiscard]] double totalSeconds() const noexcept { return m_total; }
    /// Number of ticks so far.
    [[nodiscard]] std::uint64_t frameIndex() const noexcept { return m_frames; }
    /// 1 / smoothed delta, or 0 before two ticks.
    [[nodiscard]] double framesPerSecond() const noexcept;

private:
    static constexpr double kSmoothing = 0.1;

    std::optional<Clock::time_point> m_start;
    std::optional<Clock::time_point> m_last;
    double m_delta = 0.0;
    double m_smoothed = 0.0;
    double m_total = 0.0;
    std::uint64_t m_frames = 0;
};

} // namespace rv::core
