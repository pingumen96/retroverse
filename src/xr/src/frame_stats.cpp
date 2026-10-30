#include <rv/xr/frame_stats.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>


namespace rv::xr {
namespace {

/// Nearest-rank percentile of sorted values.
double percentile(const std::vector<double>& sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

DurationStats durationStats(std::vector<double> values) {
    DurationStats stats;
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    stats.count = values.size();
    stats.meanMs = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    stats.p50Ms = percentile(values, 0.50);
    stats.p95Ms = percentile(values, 0.95);
    stats.p99Ms = percentile(values, 0.99);
    stats.maxMs = values.back();
    return stats;
}

GapHistogram countGap(GapHistogram histogram, std::int64_t gapNs, std::int64_t periodNs) noexcept {
    const double periods = periodNs > 0 ? static_cast<double>(gapNs) / static_cast<double>(periodNs) : 0.0;
    if (periods < 0.75) {
        ++histogram.underOne;
    } else if (periods < 1.25) {
        ++histogram.one;
    } else if (periods < 1.75) {
        ++histogram.oneAndHalf;
    } else if (periods < 2.5) {
        ++histogram.two;
    } else {
        ++histogram.more;
    }
    return histogram;
}

struct Stretch {
    std::size_t refreshes = 0;
    std::size_t missed = 0;
};

/// The refreshes a stretch of frames with one display period spans, and those no new frame reached.
Stretch countStretch(std::span<const FrameRecord> frames) noexcept {
    if (frames.size() < 2) {
        return {};
    }
    const std::int64_t period = frames.back().displayPeriodNs;
    const std::int64_t span = frames.back().displayTimeNs - frames.front().displayTimeNs;
    if (period <= 0 || span <= 0) {
        return {};
    }
    const auto spanned = static_cast<std::size_t>(
        std::max<long long>(1, std::llround(static_cast<double>(span) / static_cast<double>(period))));
    const std::size_t intervals = frames.size() - 1;
    return {spanned, spanned > intervals ? spanned - intervals : 0};
}

} // namespace

FrameSummary summarizeFrames(std::span<const FrameRecord> frames) {
    FrameSummary summary;
    summary.frames = frames.size();
    if (frames.empty()) {
        return summary;
    }

    std::vector<double> periods;
    std::vector<double> cpu;
    std::vector<double> gpu;
    periods.reserve(frames.size());
    cpu.reserve(frames.size());
    for (const FrameRecord& frame : frames) {
        if (frame.displayPeriodNs > 0) {
            periods.push_back(static_cast<double>(frame.displayPeriodNs) / 1.0e6);
        }
        cpu.push_back(frame.cpuMs);
        if (frame.gpuMs) {
            gpu.push_back(*frame.gpuMs);
        }
        summary.rendered += frame.rendered ? 1 : 0;
    }
    if (!periods.empty()) {
        std::sort(periods.begin(), periods.end());
        summary.displayPeriodMs = percentile(periods, 0.50);
        summary.refreshHz = summary.displayPeriodMs > 0.0 ? 1000.0 / summary.displayPeriodMs : 0.0;
    }

    for (std::size_t i = 1; i < frames.size(); ++i) {
        summary.gaps = countGap(summary.gaps, frames[i].displayTimeNs - frames[i - 1].displayTimeNs,
                                frames[i].displayPeriodNs);
    }

    // Stretches of one display period: a frame whose period differs from the previous one's ends a stretch, and that
    // interval opens the next one (the previous frame is then the first of both).
    std::size_t refreshes = 0;
    std::size_t first = 0;
    for (std::size_t i = 1; i <= frames.size(); ++i) {
        const bool last = i == frames.size();
        if (last || (i > first + 1 && frames[i].displayPeriodNs != frames[i - 1].displayPeriodNs)) {
            const Stretch stretch = countStretch(frames.subspan(first, i - first));
            refreshes += stretch.refreshes;
            summary.missedRefreshes += stretch.missed;
            first = i - 1;
        }
    }
    summary.missedFraction =
        refreshes > 0 ? static_cast<double>(summary.missedRefreshes) / static_cast<double>(refreshes) : 0.0;

    summary.cpu = durationStats(std::move(cpu));
    summary.gpu = durationStats(std::move(gpu));
    return summary;
}

} // namespace rv::xr
