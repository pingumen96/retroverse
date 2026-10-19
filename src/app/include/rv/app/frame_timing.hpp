#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/run_report.hpp>
#include <rv/render/frame_stats.hpp>

namespace rv::app {

// Frame times of a run, over the whole run and per named phase (I3 step 0d; the budgets of docs/roadmap.md, I3): the
// run report's frameTimes and phases.

/// Frames left out at the start of a run (the first frames compile shaders, generate surfaces and warm the driver up):
/// they count towards the frames of a phase but towards no percentile.
inline constexpr std::uint64_t kFrameTimeWarmUpFrames = 30;

/// The phases the budgets name (a phase is any name; these are the ones the code, the input scripts and the budgets
/// share).
inline constexpr std::string_view kPhaseRefresh = "refresh";     ///< the catalog's refresh (I3 steps 13r, 15b)
inline constexpr std::string_view kPhaseCoverLoad = "coverLoad"; ///< loading the room's covers (I3 step 12)
inline constexpr std::string_view kPhaseAnimation = "animation"; ///< a pickup or inspection animation (I3 step 13a)
inline constexpr std::string_view kPhaseBenchmark = "benchmark"; ///< the benchmark path (I3 step 17b)

/// The GPU passes FrameTiming summarizes (GpuPassRecord's names, in its order; the last one is the sum of the others).
inline constexpr std::array<std::string_view, 7> kGpuPassNames{"shadow", "depth",   "ssao", "scene",
                                                               "post",   "outline", "total"};

/// Collects the time of every frame of a run and the frames of its phases. A phase is opened and closed by the code
/// that runs it (AppContext::frameTiming): a frame belongs to a phase when the phase is open when the frame ends, or
/// was opened and closed during it. Phases nest by name: a phase opened twice stays open until it is closed twice
/// (two animations at once). Main thread only.
class FrameTiming {
public:
    /// `warmUpFrames`: frames left out at the start of the run (frames with a lower index).
    explicit FrameTiming(std::uint64_t warmUpFrames = kFrameTimeWarmUpFrames);

    /// Opens the phase `name` (not empty: an empty name is ignored), or opens it once more.
    void openPhase(std::string_view name);
    /// Closes one opening of `name`; false when it is not open.
    bool closePhase(std::string_view name);
    /// True while `name` is open.
    [[nodiscard]] bool phaseOpen(std::string_view name) const;

    /// Records the end of frame `frameIndex` (counted from 0, increasing): its wall-clock time in milliseconds, from
    /// the previous frame's end to this one's (nothing when unknown, such as the first frame's, or not finite or
    /// negative). Every phase it belongs to counts it.
    void recordFrame(std::uint64_t frameIndex, std::optional<double> milliseconds);
    /// The GPU times the frame being recorded reported for its last view (I3 step 17b: gl::FrameStats's timer query
    /// results, each pass that has one), counted by the next recordFrame() for the run and its phases like its time:
    /// past the warm-up only. A frame that records none (no view) adds no GPU time; a second call in one frame
    /// replaces the first.
    void recordGpuPasses(const gl::FrameStats& stats);

    /// The frame times of the run so far.
    [[nodiscard]] FrameTimesRecord runTimes() const;
    /// Every phase opened so far, in the order of their first opening.
    [[nodiscard]] std::vector<PhaseRecord> phases() const;

private:
    static constexpr std::size_t kGpuPassCount = kGpuPassNames.size();
    /// One frame's GPU times per pass (kGpuPassNames' order).
    using GpuSample = std::array<std::optional<double>, kGpuPassCount>;
    /// Every pass's samples.
    using GpuSamples = std::array<std::vector<double>, kGpuPassCount>;

    static void addGpuSample(GpuSamples& samples, const GpuSample& sample);
    [[nodiscard]] static std::vector<GpuPassRecord> gpuPassRecords(const GpuSamples& samples);

    struct Phase {
        std::string name;
        /// Openings not closed yet.
        std::uint64_t depth = 0;
        std::uint64_t opened = 0;
        std::uint64_t frames = 0;
        /// Opened during the frame being recorded (counted even when closed again before it ends).
        bool touched = false;
        std::vector<double> milliseconds;
        GpuSamples gpu;
    };

    [[nodiscard]] Phase* find(std::string_view name);
    [[nodiscard]] const Phase* find(std::string_view name) const;

    std::uint64_t m_warmUpFrames;
    std::vector<double> m_milliseconds;
    GpuSamples m_gpu;
    /// The GPU times of the frame being recorded (recordGpuPasses), until recordFrame() takes them.
    std::optional<GpuSample> m_pendingGpu;
    std::vector<Phase> m_phases;
};

} // namespace rv::app
