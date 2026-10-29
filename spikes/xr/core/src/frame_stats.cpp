#include <spike_xr/frame_stats.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include <nlohmann/json.hpp>

namespace rv::spikes::xr {
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

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
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

    std::size_t refreshes = 0;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const std::int64_t period = frames[i].displayPeriodNs;
        const std::int64_t gap = frames[i].displayTimeNs - frames[i - 1].displayTimeNs;
        if (period <= 0 || gap <= 0) {
            continue;
        }
        const auto spanned = static_cast<std::size_t>(
            std::max<long long>(1, std::llround(static_cast<double>(gap) / static_cast<double>(period))));
        refreshes += spanned;
        summary.missedRefreshes += spanned - 1;
    }
    summary.missedFraction =
        refreshes > 0 ? static_cast<double>(summary.missedRefreshes) / static_cast<double>(refreshes) : 0.0;

    summary.cpu = durationStats(std::move(cpu));
    summary.gpu = durationStats(std::move(gpu));
    return summary;
}

bool meetsCriteria(const FrameSummary& summary, double maxMissedFraction) noexcept {
    return summary.rendered > 0 && summary.missedFraction <= maxMissedFraction;
}

nlohmann::json toJson(const DurationStats& stats) {
    return nlohmann::json{{"count", stats.count},     {"meanMs", rounded(stats.meanMs)},
                          {"p50Ms", rounded(stats.p50Ms)}, {"p95Ms", rounded(stats.p95Ms)},
                          {"p99Ms", rounded(stats.p99Ms)}, {"maxMs", rounded(stats.maxMs)}};
}

nlohmann::json toJson(const FrameSummary& summary) {
    return nlohmann::json{{"frames", summary.frames},
                          {"rendered", summary.rendered},
                          {"displayPeriodMs", rounded(summary.displayPeriodMs)},
                          {"refreshHz", rounded(summary.refreshHz)},
                          {"missedRefreshes", summary.missedRefreshes},
                          {"missedFraction", rounded(summary.missedFraction)},
                          {"cpu", toJson(summary.cpu)},
                          {"gpu", toJson(summary.gpu)}};
}

} // namespace rv::spikes::xr
