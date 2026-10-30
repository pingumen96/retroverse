#include <rv/xr/render_scale.hpp>

#include <algorithm>
#include <cmath>

namespace rv::xr {

RenderScale::RenderScale(const Settings& settings) noexcept : m_settings(settings) {
    m_settings.min = std::clamp(m_settings.min, 0.1f, 1.0f);
    m_settings.max = std::clamp(m_settings.max, m_settings.min, 1.0f);
    m_scale = m_settings.max;
    m_lowest = m_scale;
}

void RenderScale::setMax(float max) noexcept {
    m_settings.max = std::clamp(std::isfinite(max) ? max : 1.0f, m_settings.min, 1.0f);
    if (m_scale > m_settings.max) {
        m_scale = m_settings.max;
        m_lowest = std::min(m_lowest, m_scale);
        m_sinceChange = 0;
        ++m_changes;
    }
}

float RenderScale::update(double gpuMilliseconds, double periodMilliseconds) noexcept {
    if (!std::isfinite(gpuMilliseconds) || !(gpuMilliseconds > 0.0) || !std::isfinite(periodMilliseconds) ||
        !(periodMilliseconds > 0.0)) {
        return m_scale;
    }
    m_smoothed = m_measured ? m_smoothed + m_settings.smoothing * (gpuMilliseconds - m_smoothed) : gpuMilliseconds;
    m_measured = true;
    if (++m_sinceChange < m_settings.settleFrames) {
        return m_scale;
    }
    float next = m_scale;
    if (m_smoothed > m_settings.high * periodMilliseconds) {
        next = std::max(m_settings.min, m_scale - m_settings.step);
    } else if (m_smoothed < m_settings.low * periodMilliseconds) {
        next = std::min(m_settings.max, m_scale + m_settings.step);
    }
    if (next != m_scale) {
        m_scale = next;
        m_lowest = std::min(m_lowest, m_scale);
        m_sinceChange = 0;
        ++m_changes;
    }
    return m_scale;
}

} // namespace rv::xr
