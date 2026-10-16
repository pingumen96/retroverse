#include <rv/media/sample_stats.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace rv::media {

double percentileOfSorted(std::span<const double> sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const double clamped = std::isnan(fraction) ? 0.0 : std::clamp(fraction, 0.0, 1.0);
    const auto count = sorted.size();
    const auto rank = static_cast<std::size_t>(std::ceil(clamped * static_cast<double>(count)));
    return sorted[std::clamp<std::size_t>(rank, 1, count) - 1];
}

SampleSummary summarize(std::span<const double> values) {
    SampleSummary summary;
    if (values.empty()) {
        return summary;
    }
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    summary.count = sorted.size();
    summary.min = sorted.front();
    summary.max = sorted.back();
    summary.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    summary.p50 = percentileOfSorted(sorted, 0.50);
    summary.p95 = percentileOfSorted(sorted, 0.95);
    summary.p99 = percentileOfSorted(sorted, 0.99);
    return summary;
}

void SampleRecorder::record(double value) noexcept {
    if (!std::isfinite(value)) {
        return;
    }
    if (m_count == 0) {
        m_min = value;
        m_max = value;
    } else {
        m_min = std::min(m_min, value);
        m_max = std::max(m_max, value);
    }
    ++m_count;
    m_sum += value;
    if (m_values.size() < m_values.capacity()) {
        m_values.push_back(value); // within the reserved capacity: never reallocates
    }
}

SampleSummary SampleRecorder::summary() const {
    SampleSummary result = summarize(m_values);
    if (m_count > 0) {
        result.count = static_cast<std::size_t>(m_count);
        result.mean = m_sum / static_cast<double>(m_count);
        result.min = m_min;
        result.max = m_max;
    }
    return result;
}

void SampleRecorder::clear() noexcept {
    m_values.clear();
    m_count = 0;
    m_sum = 0.0;
    m_min = 0.0;
    m_max = 0.0;
}

} // namespace rv::media
