#include <rv/core/cpu_timers.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <rv/core/main_thread.hpp>

namespace rv::core {

std::optional<DurationPercentiles> summarizeDurations(std::vector<double>& milliseconds) {
    if (milliseconds.empty()) {
        return std::nullopt;
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    const std::size_t count = milliseconds.size();
    const auto rank = [&](double fraction) {
        const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(count)));
        return milliseconds[std::min(count - 1, index > 0 ? index - 1 : 0)];
    };
    return DurationPercentiles{static_cast<std::uint64_t>(count), rank(0.50), rank(0.95), rank(0.99),
                               milliseconds.back()};
}

void DurationRecorder::add(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
        return;
    }
    ++m_count;
    m_max = m_count == 1 ? milliseconds : std::max(m_max, milliseconds);
    if (m_buckets.empty() && m_samples.size() < kExactSamples) {
        m_samples.push_back(milliseconds);
        return;
    }
    if (m_buckets.empty()) {
        // From exact samples to the histogram: the samples go into it, and their storage goes.
        m_buckets.assign(kBucketCount, 0);
        for (const double sample : m_samples) {
            ++m_buckets[bucketOf(sample)];
        }
        std::vector<double>().swap(m_samples);
    }
    ++m_buckets[bucketOf(milliseconds)];
}

std::optional<DurationPercentiles> DurationRecorder::summary() const {
    if (m_buckets.empty()) {
        std::vector<double> samples = m_samples;
        return summarizeDurations(samples);
    }
    // Nearest rank, as summarizeDurations(): the bucket holding the sample of rank ceil(fraction * count).
    const auto rank = [&](double fraction) {
        const auto wanted = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(m_count))));
        std::uint64_t seen = 0;
        for (std::size_t bucket = 0; bucket < m_buckets.size(); ++bucket) {
            seen += m_buckets[bucket];
            if (seen >= wanted) {
                // The last bucket holds everything past the others: only the largest sample bounds it.
                return bucket + 1 == m_buckets.size() ? m_max : std::min(upperBound(bucket), m_max);
            }
        }
        return m_max;
    };
    return DurationPercentiles{m_count, rank(0.50), rank(0.95), rank(0.99), m_max};
}

std::size_t DurationRecorder::bucketOf(double milliseconds) noexcept {
    if (milliseconds <= kSmallestMs) {
        return 0;
    }
    // Bucket b (from 1) holds (kSmallestMs * ratio^(b-1), kSmallestMs * ratio^b].
    const double steps = std::ceil(std::log(milliseconds / kSmallestMs) / std::log(kBucketRatio));
    const double bucket = std::max(1.0, steps);
    if (!(bucket < static_cast<double>(kBucketCount - 1))) {
        return kBucketCount - 1;
    }
    std::size_t index = static_cast<std::size_t>(bucket);
    // The logarithm's rounding: the sample is at most its bucket's upper bound, and above the previous one's.
    while (index + 1 < kBucketCount && milliseconds > upperBound(index)) {
        ++index;
    }
    while (index > 1 && milliseconds <= upperBound(index - 1)) {
        --index;
    }
    return index;
}

double DurationRecorder::upperBound(std::size_t bucket) noexcept {
    return kSmallestMs * std::pow(kBucketRatio, static_cast<double>(bucket));
}

CpuTimers::SectionId CpuTimers::section(std::string_view name) {
    for (SectionId index = 0; index < m_sections.size(); ++index) {
        if (m_sections[index].name == name) {
            return index;
        }
    }
    m_sections.push_back(Section{std::string(name), std::nullopt, {}});
    return m_sections.size() - 1;
}

void CpuTimers::add(SectionId section, double milliseconds) noexcept {
    if (section >= m_sections.size() || !std::isfinite(milliseconds) || milliseconds < 0.0) {
        return;
    }
    std::optional<double>& current = m_sections[section].current;
    current = current.value_or(0.0) + milliseconds;
}

void CpuTimers::endFrame() {
    for (Section& section : m_sections) {
        if (section.current) {
            section.samples.add(*section.current);
            section.current.reset();
        }
    }
    ++m_frames;
}

std::optional<double> CpuTimers::currentFrameMilliseconds(SectionId section) const {
    if (section >= m_sections.size()) {
        return std::nullopt;
    }
    return m_sections[section].current;
}

std::vector<CpuSectionSummary> CpuTimers::summaries() const {
    std::vector<CpuSectionSummary> result;
    for (const Section& section : m_sections) {
        if (const std::optional<DurationPercentiles> times = section.samples.summary()) {
            result.push_back(CpuSectionSummary{section.name, *times});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const CpuSectionSummary& a, const CpuSectionSummary& b) { return a.name < b.name; });
    return result;
}

void CpuTimers::clear() noexcept {
    m_sections.clear();
    m_frames = 0;
}

CpuTimers::Scope::Scope(CpuTimers& timers, SectionId section) noexcept
    : m_timers(&timers), m_section(section), m_start(Clock::now()) {}

CpuTimers::Scope::Scope(Scope&& other) noexcept
    : m_timers(std::exchange(other.m_timers, nullptr)), m_section(other.m_section), m_start(other.m_start) {}

CpuTimers::Scope::~Scope() {
    stop();
}

void CpuTimers::Scope::stop() noexcept {
    if (m_timers == nullptr) {
        return;
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
    std::exchange(m_timers, nullptr)->add(m_section, milliseconds);
}

CpuTimers& mainThreadCpuTimers() noexcept {
    static CpuTimers timers;
    return timers;
}

CpuSectionScope::CpuSectionScope(std::string_view name) {
    if (!isMainThread()) {
        assertMainThread("CpuSectionScope");
        return;
    }
    CpuTimers& timers = mainThreadCpuTimers();
    m_scope.emplace(timers, timers.section(name));
}

} // namespace rv::core
