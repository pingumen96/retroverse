#include <rv/core/frame_clock.hpp>

#include <algorithm>

namespace rv::core {

void FrameClock::tick() {
    tickAt(Clock::now());
}

void FrameClock::tickAt(Clock::time_point now) {
    if (!m_start) {
        m_start = now;
        m_last = now;
        m_delta = 0.0;
        m_total = 0.0;
        m_frames = 1;
        return;
    }
    const double delta = std::chrono::duration<double>(now - *m_last).count();
    m_delta = std::max(delta, 0.0);
    if (now > *m_last) {
        m_last = now;
    }
    m_total = std::chrono::duration<double>(*m_last - *m_start).count();
    m_smoothed = (m_frames == 1) ? m_delta : m_smoothed + kSmoothing * (m_delta - m_smoothed);
    ++m_frames;
}

double FrameClock::clampedDeltaSeconds(double maxDelta) const noexcept {
    return std::clamp(m_delta, 0.0, std::max(maxDelta, 0.0));
}

double FrameClock::framesPerSecond() const noexcept {
    return m_smoothed > 0.0 ? 1.0 / m_smoothed : 0.0;
}

} // namespace rv::core
