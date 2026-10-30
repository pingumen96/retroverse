#pragma once

#include <cstdint>

namespace rv::xr {

/// The eyes' render scale in VR (I10 step 7, ADR-0017 §9): the share of the swapchain's size the eyes render at, the
/// picture then scaled up into it. Lowered by a step when the GPU's time per frame (both eyes, smoothed) stays above
/// `high` of the display's period, raised by a step when it stays below `low`, within [min, max]; a change waits
/// `settleFrames` frames before the next, so that the timers' latency and one frame's spike move nothing.
/// `max` itself may change (the launched game's "in game" profile asks for less). GPU-free.
class RenderScale {
public:
    struct Settings {
        float min = 0.6f;
        float max = 1.0f;
        float step = 0.05f;
        float high = 0.85f;
        float low = 0.65f;
        int settleFrames = 36;
        /// The smoothing of the GPU's times (the weight of each new one).
        double smoothing = 0.1;
    };

    RenderScale() : RenderScale(Settings{}) {}
    explicit RenderScale(const Settings& settings) noexcept;

    /// One frame: its GPU time and the display's period, milliseconds (a time that is not finite or not above 0
    /// changes nothing). Returns the scale for the next frames.
    float update(double gpuMilliseconds, double periodMilliseconds) noexcept;
    /// The upper bound from now on, within [min, 1] (the scale comes down to it at once).
    void setMax(float max) noexcept;

    [[nodiscard]] float scale() const noexcept { return m_scale; }
    [[nodiscard]] double smoothedMilliseconds() const noexcept { return m_smoothed; }
    [[nodiscard]] std::uint64_t changes() const noexcept { return m_changes; }
    [[nodiscard]] float lowest() const noexcept { return m_lowest; }

private:
    Settings m_settings;
    float m_scale = 1.0f;
    float m_lowest = 1.0f;
    double m_smoothed = 0.0;
    bool m_measured = false;
    int m_sinceChange = 0;
    std::uint64_t m_changes = 0;
};

} // namespace rv::xr
