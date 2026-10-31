#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <nlohmann/json_fwd.hpp>

namespace rv::spikes::xr {

/// One frame of the session loop, as xrWaitFrame predicted it and as the spike measured it.
struct FrameRecord {
    /// XrFrameState::predictedDisplayTime and predictedDisplayPeriod, nanoseconds.
    std::int64_t displayTimeNs = 0;
    std::int64_t displayPeriodNs = 0;
    /// XrFrameState::shouldRender, and whether the spike then submitted both eyes.
    bool rendered = false;
    /// CPU time from xrWaitFrame's return to xrEndFrame's return, milliseconds.
    double cpuMs = 0.0;
    /// GPU time of both eyes (GL_TIME_ELAPSED, collected frames later); nothing while it has not arrived.
    std::optional<double> gpuMs;
};

/// Statistics of a set of durations, milliseconds (nearest-rank percentiles).
struct DurationStats {
    std::size_t count = 0;
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};

/// How far apart consecutive predicted display times were, in display periods. A steady runtime puts every interval
/// in `one`; a runtime whose predictions jitter (Meta's over Link) spreads them, a long interval followed by a short
/// one, without missing anything.
struct GapHistogram {
    std::size_t underOne = 0;   ///< below 0.75 periods (or not after the previous frame)
    std::size_t one = 0;        ///< 0.75 to 1.25
    std::size_t oneAndHalf = 0; ///< 1.25 to 1.75
    std::size_t two = 0;        ///< 1.75 to 2.5
    std::size_t more = 0;       ///< 2.5 and above
};

/// What a run of frames shows about pacing. xrWaitFrame lets at most one frame through per refresh, so the refreshes
/// a run spans (its first to its last predicted display time, over the display period, rounded) less the intervals
/// between its frames are the refreshes no new frame reached: the runtime showed the previous one again. Counted over
/// each stretch of one display period (a refresh rate change starts another). Counting each interval on its own would
/// take a prediction's jitter for misses.
struct FrameSummary {
    std::size_t frames = 0;
    std::size_t rendered = 0;
    /// The median display period, and the refresh rate it implies.
    double displayPeriodMs = 0.0;
    double refreshHz = 0.0;
    std::size_t missedRefreshes = 0;
    /// Missed refreshes over every refresh the run spanned.
    double missedFraction = 0.0;
    GapHistogram gaps;
    DurationStats cpu;
    DurationStats gpu;
};

[[nodiscard]] FrameSummary summarizeFrames(std::span<const FrameRecord> frames);

/// The spike's criterion (ADR-0017's acceptable performance): at most `maxMissedFraction` of the refreshes missed,
/// and at least one frame rendered.
[[nodiscard]] bool meetsCriteria(const FrameSummary& summary, double maxMissedFraction = 0.01) noexcept;

[[nodiscard]] nlohmann::json toJson(const DurationStats& stats);
[[nodiscard]] nlohmann::json toJson(const FrameSummary& summary);

} // namespace rv::spikes::xr
