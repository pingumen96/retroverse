#include <rv/devui/frame_time_history.hpp>

#include <algorithm>
#include <cmath>

namespace rv::devui {

void FrameTimeHistory::record(double seconds) noexcept {
    // Also refuses durations that would not survive the conversion to float milliseconds (0 or infinite).
    const auto milliseconds = static_cast<float>(seconds * 1000.0);
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0f) {
        return;
    }
    m_milliseconds[m_next] = milliseconds;
    m_next = (m_next + 1) % kCapacity;
    m_count = std::min(m_count + 1, kCapacity);
}

std::size_t FrameTimeHistory::ringOffset() const noexcept {
    return m_count < kCapacity ? 0 : m_next;
}

float FrameTimeHistory::milliseconds(std::size_t index) const noexcept {
    return index < m_count ? m_milliseconds[(ringOffset() + index) % kCapacity] : 0.0f;
}

FrameTimeHistory::Summary FrameTimeHistory::summary() const noexcept {
    Summary summary;
    if (m_count == 0) {
        return summary;
    }
    double total = 0.0;
    summary.minMilliseconds = m_milliseconds[ringOffset()];
    summary.maxMilliseconds = summary.minMilliseconds;
    for (std::size_t index = 0; index < m_count; ++index) {
        const float value = milliseconds(index);
        total += value;
        summary.minMilliseconds = std::min(summary.minMilliseconds, value);
        summary.maxMilliseconds = std::max(summary.maxMilliseconds, value);
    }
    summary.averageMilliseconds = static_cast<float>(total / static_cast<double>(m_count));
    // The average of positive values is positive; only absurdly short frames could overflow the rate.
    const float framesPerSecond = static_cast<float>(1000.0 * static_cast<double>(m_count) / total);
    summary.framesPerSecond = std::isfinite(framesPerSecond) ? framesPerSecond : 0.0f;
    return summary;
}

} // namespace rv::devui
