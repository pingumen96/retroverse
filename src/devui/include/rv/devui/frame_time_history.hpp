#pragma once

// GPU-free (target rv_devui_model): no SDL, OpenGL or Dear ImGui header is included.

#include <array>
#include <cstddef>

namespace rv::devui {

/// Frame times of the most recent frames for the overlay's Stats panel: a ring of the last kCapacity wall-clock frame
/// durations, in milliseconds, with their average, extremes and frame rate. Recording never allocates.
class FrameTimeHistory {
public:
    /// Frames kept: four seconds at 60 Hz.
    static constexpr std::size_t kCapacity = 240;

    /// Summary of the frames kept.
    struct Summary {
        float averageMilliseconds = 0.0f;
        float minMilliseconds = 0.0f;
        float maxMilliseconds = 0.0f;
        /// 1000 / averageMilliseconds.
        float framesPerSecond = 0.0f;
    };

    /// Records one frame that took `seconds`. Values that are not finite and positive (the 0 of a first frame) are
    /// ignored; once full, the oldest frame is dropped.
    void record(double seconds) noexcept;

    /// Frames kept, at most kCapacity.
    [[nodiscard]] std::size_t size() const noexcept { return m_count; }
    /// Duration of the `index`-th oldest frame kept, in milliseconds (index < size()).
    [[nodiscard]] float milliseconds(std::size_t index) const noexcept;

    /// The ring as stored, for plots that take a ring buffer (ImGui::PlotLines): `size()` values starting at
    /// ringOffset() and wrapping around.
    [[nodiscard]] const float* ringData() const noexcept { return m_milliseconds.data(); }
    /// Position of the oldest frame in ringData().
    [[nodiscard]] std::size_t ringOffset() const noexcept;

    /// Average, extremes and frame rate of the frames kept; all zero when empty.
    [[nodiscard]] Summary summary() const noexcept;

private:
    std::array<float, kCapacity> m_milliseconds{};
    /// Where the next frame is written.
    std::size_t m_next = 0;
    std::size_t m_count = 0;
};

} // namespace rv::devui
