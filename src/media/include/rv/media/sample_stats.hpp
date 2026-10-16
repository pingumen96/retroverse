#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rv::media {

/// Summary of a series of measurements (durations in milliseconds, for instance). All zero when empty.
struct SampleSummary {
    std::size_t count = 0;
    double mean = 0.0;
    double min = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

/// Percentile `fraction` (0..1, clamped) of `sorted` (ascending), nearest-rank: the smallest value such that at least
/// that fraction of the values are at most it. 0 for an empty series.
[[nodiscard]] double percentileOfSorted(std::span<const double> sorted, double fraction) noexcept;

/// Summary of `values` in any order (copied and sorted: meant for the end of a measurement, not for every frame).
[[nodiscard]] SampleSummary summarize(std::span<const double> values);

/// Records measurements into storage reserved up front: record() never allocates, so it can be called every frame.
/// Values beyond the capacity are not stored but still counted (overflow()) and still update the minimum, maximum and
/// mean. Not thread-safe.
class SampleRecorder {
public:
    explicit SampleRecorder(std::size_t capacity = 0) { m_values.reserve(capacity); }

    /// Adds a value; non-finite values are ignored.
    void record(double value) noexcept;
    /// Summary of the stored values, with min, max, mean and count over every recorded value.
    [[nodiscard]] SampleSummary summary() const;
    /// Values recorded, stored or not.
    [[nodiscard]] std::uint64_t count() const noexcept { return m_count; }
    /// Values recorded after the storage was full.
    [[nodiscard]] std::uint64_t overflow() const noexcept { return m_count - m_values.size(); }
    [[nodiscard]] std::span<const double> values() const noexcept { return m_values; }
    /// Forgets every value, keeping the storage.
    void clear() noexcept;

private:
    std::vector<double> m_values;
    std::uint64_t m_count = 0;
    double m_sum = 0.0;
    double m_min = 0.0;
    double m_max = 0.0;
};

} // namespace rv::media
