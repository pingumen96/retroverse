#include <rv/ui/timing_samples.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace rv::ui {

double percentileOfSorted(std::span<const double> sorted, double percent) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const double clamped = std::isfinite(percent) ? std::clamp(percent, 0.0, 100.0) : 0.0;
    const auto count = static_cast<double>(sorted.size());
    // Nearest rank: ceil(p / 100 * N), counted from 1, at least 1.
    const auto rank = static_cast<std::size_t>(std::max(1.0, std::ceil(clamped / 100.0 * count)));
    return sorted[std::min(rank, sorted.size()) - 1];
}

TimingSamples::TimingSamples(std::size_t capacity) : m_ring(std::max<std::size_t>(capacity, 1), 0.0) {}

void TimingSamples::add(double milliseconds) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
        return;
    }
    m_ring[m_next] = milliseconds;
    m_next = (m_next + 1) % m_ring.size();
    m_size = std::min(m_size + 1, m_ring.size());
    ++m_total;
}

void TimingSamples::clear() noexcept {
    m_next = 0;
    m_size = 0;
    m_total = 0;
}

TimingSummary TimingSamples::summarize() const {
    TimingSummary summary;
    if (m_size == 0) {
        return summary;
    }
    // The kept samples: the whole ring once it wrapped, else its first m_size entries.
    std::vector<double> sorted(m_ring.begin(), m_ring.begin() + static_cast<std::ptrdiff_t>(m_size));
    std::sort(sorted.begin(), sorted.end());
    summary.count = sorted.size();
    summary.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    summary.p50 = percentileOfSorted(sorted, 50.0);
    summary.p99 = percentileOfSorted(sorted, 99.0);
    summary.min = sorted.front();
    summary.max = sorted.back();
    return summary;
}

} // namespace rv::ui
