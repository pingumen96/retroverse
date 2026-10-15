#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <rv/render/frame_stats.hpp>

namespace rv::app {

/// Aggregates frame times and the statistics of the main view for the log: a summary per interval (developer builds
/// log one per second) and one for the whole run (frame-limited runs log it at exit).
///
/// A summary line reads: "<label>: <n> frame(s) in <s> s, frame time <avg> ms (min <a>, max <b>), GPU <g> ms; last
/// view: <items> item(s), <culled> culled, <batches> batch(es), <draws> draw call(s), <instances> instance(s),
/// <triangles> triangle(s), <shaders> shader switch(es), <materials> material switch(es)", followed by
/// ", <lines> debug line(s)" when debug lines were drawn. "<n> frame(s)" reads "<n> frame(s) (<k> without a view)" when
/// some frames rendered no view. The GPU time averages, over the frames with a view, the latest timer query result
/// each reported (FrameStats::gpuMilliseconds; "n/a" without any); "last view" is the last view recorded.
class FrameStatsLog {
public:
    /// `intervalSeconds` (> 0) of recorded frame time between two interval summaries.
    explicit FrameStatsLog(double intervalSeconds = 1.0);

    /// Records a frame: `frameSeconds` is its wall-clock duration (the time since the previous frame; values that are
    /// not finite and positive, such as the 0 of the first frame, do not count towards the times) and `stats` what its
    /// main view drew.
    void record(double frameSeconds, const gl::FrameStats& stats);
    /// Records a frame that rendered no view (a minimized window): it counts towards the frames and their times, but
    /// adds no GPU time and leaves the last view as it is.
    void recordWithoutView(double frameSeconds);

    /// Once the frames recorded since the previous interval summary add up to the interval: their summary, labeled
    /// "Frame stats"; the next interval starts empty. Nothing before that.
    [[nodiscard]] std::optional<std::string> takeIntervalSummary();

    /// Summary of every frame recorded so far, labeled `label`; nothing before the first frame.
    [[nodiscard]] std::optional<std::string> runSummary(std::string_view label) const;

private:
    /// Totals over a span of frames.
    struct Window {
        std::size_t frames = 0;
        std::size_t framesWithoutView = 0;
        std::size_t timedFrames = 0;
        double seconds = 0.0;
        double minSeconds = 0.0;
        double maxSeconds = 0.0;
        std::size_t gpuSamples = 0;
        double gpuMilliseconds = 0.0;

        /// Adds a frame; `stats` is null for a frame without a view.
        void add(double frameSeconds, const gl::FrameStats* stats);
    };

    [[nodiscard]] std::string format(std::string_view label, const Window& window) const;

    double m_intervalSeconds;
    Window m_interval;
    Window m_run;
    gl::FrameStats m_last;
};

} // namespace rv::app
