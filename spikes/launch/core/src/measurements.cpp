#include <spike_launch/measurements.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

#include <nlohmann/json.hpp>

namespace rv::spikes::launch {
namespace {

/// Nearest-rank percentile of sorted values.
double percentile(const std::vector<double>& sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

nlohmann::json optionalJson(const std::optional<double>& value) {
    return value ? nlohmann::json(std::round(*value * 100.0) / 100.0) : nlohmann::json(nullptr);
}

} // namespace

double millisecondsBetween(Clock::time_point origin, Clock::time_point point) noexcept {
    return std::chrono::duration<double, std::milli>(point - origin).count();
}

std::optional<std::size_t> steadyFrom(std::span<const Clock::time_point> presented, const PacingCriterion& criterion) {
    if (criterion.window == 0) {
        return presented.empty() ? std::nullopt : std::optional<std::size_t>(0);
    }
    std::size_t runStart = 0;
    std::size_t runLength = 0; // intervals within the limit since runStart
    for (std::size_t i = 1; i < presented.size(); ++i) {
        if (presented[i] - presented[i - 1] <= criterion.maxInterval) {
            if (runLength == 0) {
                runStart = i - 1;
            }
            if (++runLength >= criterion.window) {
                return runStart;
            }
        } else {
            runLength = 0;
        }
    }
    return std::nullopt;
}

IntervalStats intervalStats(std::span<const Clock::time_point> presented, std::vector<double>& scratch) {
    IntervalStats stats;
    scratch.clear();
    for (std::size_t i = 1; i < presented.size(); ++i) {
        scratch.push_back(millisecondsBetween(presented[i - 1], presented[i]));
    }
    if (scratch.empty()) {
        return stats;
    }
    stats.count = scratch.size();
    stats.meanMs = std::accumulate(scratch.begin(), scratch.end(), 0.0) / static_cast<double>(scratch.size());
    std::sort(scratch.begin(), scratch.end());
    stats.p50Ms = percentile(scratch, 0.50);
    stats.p95Ms = percentile(scratch, 0.95);
    stats.p99Ms = percentile(scratch, 0.99);
    stats.maxMs = scratch.back();
    return stats;
}

std::optional<double> ReturnTimes::latest() const noexcept {
    double worst = 0.0;
    for (const std::optional<double>& time : {restored, foreground, fullscreen, firstFrame, steady60, input}) {
        if (!time) {
            return std::nullopt;
        }
        worst = std::max(worst, *time);
    }
    return worst;
}

bool meetsCriteria(const ReturnTimes& times, double limitMs) noexcept {
    const std::optional<double> worst = times.latest();
    return worst.has_value() && *worst <= limitMs;
}

Summary summarize(std::vector<double> values) {
    Summary summary;
    if (values.empty()) {
        return summary;
    }
    std::sort(values.begin(), values.end());
    summary.count = values.size();
    summary.best = values.front();
    summary.worst = values.back();
    const std::size_t middle = values.size() / 2;
    summary.median = values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
    return summary;
}

nlohmann::json toJson(const IntervalStats& stats) {
    const auto rounded = [](double value) { return std::round(value * 1000.0) / 1000.0; };
    return nlohmann::json{{"count", stats.count},         {"meanMs", rounded(stats.meanMs)},
                          {"p50Ms", rounded(stats.p50Ms)}, {"p95Ms", rounded(stats.p95Ms)},
                          {"p99Ms", rounded(stats.p99Ms)}, {"maxMs", rounded(stats.maxMs)}};
}

nlohmann::json toJson(const ReturnTimes& times) {
    return nlohmann::json{{"restored", optionalJson(times.restored)},
                          {"foreground", optionalJson(times.foreground)},
                          {"fullscreen", optionalJson(times.fullscreen)},
                          {"firstFrame", optionalJson(times.firstFrame)},
                          {"steady60", optionalJson(times.steady60)},
                          {"input", optionalJson(times.input)},
                          {"latest", optionalJson(times.latest())}};
}

nlohmann::json toJson(const Summary& summary) {
    const auto rounded = [](double value) { return std::round(value * 100.0) / 100.0; };
    return nlohmann::json{{"count", summary.count},
                          {"median", rounded(summary.median)},
                          {"best", rounded(summary.best)},
                          {"worst", rounded(summary.worst)}};
}

} // namespace rv::spikes::launch
