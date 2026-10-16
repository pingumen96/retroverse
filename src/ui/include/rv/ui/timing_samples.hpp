#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <cstddef>
#include <span>
#include <vector>

namespace rv::ui {

/// Statistics of a series of durations in milliseconds; all zero for an empty series.
struct TimingSummary {
    std::size_t count = 0;
    double mean = 0.0;
    double p50 = 0.0; ///< median, nearest rank
    double p99 = 0.0; ///< nearest rank
    double min = 0.0;
    double max = 0.0;
};

/// The value at `percent` (clamped to [0, 100]) of samples sorted in ascending order, by the nearest-rank method: the
/// smallest sample with at least `percent`% of the samples at or below it (the first one for 0). 0 for no samples.
[[nodiscard]] double percentileOfSorted(std::span<const double> sorted, double percent) noexcept;

/// The most recent `capacity` samples of a duration, for the CPU and GPU times of the UI pass: a ring allocated once,
/// so that recording a frame's time never allocates. Non-finite and negative samples are ignored. Main thread only.
class TimingSamples {
public:
    /// At least one sample is kept.
    explicit TimingSamples(std::size_t capacity = 4096);

    void add(double milliseconds) noexcept;
    void clear() noexcept;

    /// Samples kept (at most the capacity).
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    /// Samples added since the last clear(), the overwritten ones included.
    [[nodiscard]] std::size_t total() const noexcept { return m_total; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_ring.size(); }

    /// Statistics of the samples kept. Sorts a copy, so it allocates: call it once per report, not every frame.
    [[nodiscard]] TimingSummary summarize() const;

private:
    std::vector<double> m_ring;
    std::size_t m_next = 0;
    std::size_t m_size = 0;
    std::size_t m_total = 0;
};

} // namespace rv::ui
