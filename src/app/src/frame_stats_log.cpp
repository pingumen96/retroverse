#include <rv/app/frame_stats_log.hpp>

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

namespace rv::app {

FrameStatsLog::FrameStatsLog(double intervalSeconds)
    : m_intervalSeconds(std::isfinite(intervalSeconds) && intervalSeconds > 0.0 ? intervalSeconds : 1.0) {}

void FrameStatsLog::Window::add(double frameSeconds, const gl::FrameStats* stats) {
    ++frames;
    if (std::isfinite(frameSeconds) && frameSeconds > 0.0) {
        minSeconds = timedFrames == 0 ? frameSeconds : std::min(minSeconds, frameSeconds);
        maxSeconds = timedFrames == 0 ? frameSeconds : std::max(maxSeconds, frameSeconds);
        seconds += frameSeconds;
        ++timedFrames;
    }
    if (stats == nullptr) {
        ++framesWithoutView;
    } else if (stats->gpuMilliseconds && std::isfinite(*stats->gpuMilliseconds)) {
        gpuMilliseconds += *stats->gpuMilliseconds;
        ++gpuSamples;
    }
}

void FrameStatsLog::record(double frameSeconds, const gl::FrameStats& stats) {
    m_interval.add(frameSeconds, &stats);
    m_run.add(frameSeconds, &stats);
    m_last = stats;
}

void FrameStatsLog::recordWithoutView(double frameSeconds) {
    m_interval.add(frameSeconds, nullptr);
    m_run.add(frameSeconds, nullptr);
}

std::optional<std::string> FrameStatsLog::takeIntervalSummary() {
    if (m_interval.frames == 0 || m_interval.seconds < m_intervalSeconds) {
        return std::nullopt;
    }
    std::string summary = format("Frame stats", m_interval);
    m_interval = Window{};
    return summary;
}

std::optional<std::string> FrameStatsLog::runSummary(std::string_view label) const {
    if (m_run.frames == 0) {
        return std::nullopt;
    }
    return format(label, m_run);
}

std::string FrameStatsLog::format(std::string_view label, const Window& window) const {
    const std::string frames = window.framesWithoutView == 0 ? fmt::format("{} frame(s)", window.frames)
                                                             : fmt::format("{} frame(s) ({} without a view)",
                                                                           window.frames, window.framesWithoutView);
    const std::string frameTime = window.timedFrames == 0
                                      ? std::string("n/a")
                                      : fmt::format("{:.2f} ms (min {:.2f}, max {:.2f})",
                                                    1000.0 * window.seconds / static_cast<double>(window.timedFrames),
                                                    1000.0 * window.minSeconds, 1000.0 * window.maxSeconds);
    const std::string gpuTime =
        window.gpuSamples == 0
            ? std::string("n/a")
            : fmt::format("{:.2f} ms", window.gpuMilliseconds / static_cast<double>(window.gpuSamples));
    std::string summary = fmt::format("{}: {} in {:.2f} s, frame time {}, GPU {}; last view: {} item(s), {} culled, {} "
                                      "batch(es), {} draw call(s), {} instance(s), {} triangle(s), {} shader "
                                      "switch(es), {} material switch(es)",
                                      label, frames, window.seconds, frameTime, gpuTime, m_last.items,
                                      m_last.culledItems, m_last.batches, m_last.drawCalls, m_last.instances,
                                      m_last.triangles, m_last.shaderSwitches, m_last.materialSwitches);
    if (m_last.debugLines > 0) {
        summary += fmt::format(", {} debug line(s)", m_last.debugLines);
    }
    return summary;
}

} // namespace rv::app
