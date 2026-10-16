#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <rv/media/video_frame.hpp>

namespace rv::media {

/// The frames waiting to be shown, oldest first, as the PresentationClock sees them: a FrameQueue in the application
/// (VideoPlayback adapts it), a plain list of timings in tests. Only the clock removes frames.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    /// Timing of the frame `index` places from the front (0: the oldest); nothing past the back.
    [[nodiscard]] virtual std::optional<FrameTiming> peek(std::size_t index) const = 0;
    /// Removes the oldest frame: `present` is true when it becomes the frame on screen, false when it is skipped.
    virtual void pop(bool present) = 0;
};

/// Tuning of a PresentationClock.
struct ClockSettings {
    /// A frame is due once its timestamp is at most this far ahead of the media time. It absorbs the jitter of the
    /// render loop's timestamps, so that a 30 fps video on a 60 Hz display shows every frame for exactly two refreshes
    /// instead of alternating one and three when a refresh lands a hair before a frame's time.
    double dueToleranceSeconds = 0.002;
    /// When a frame arrives this much later than its time, the clock jumps to it (a resync: the media time moves back
    /// to the frame) instead of racing to catch up, which would only drop the frames that follow. Happens after a long
    /// stall of the render loop or when a decoder cannot keep up.
    double resyncThresholdSeconds = 0.25;
};

/// What the presentation has done so far. Every count covers the clock's whole life (resetStats() clears them).
struct PresentationStats {
    /// Updates of the clock (render frames).
    std::uint64_t updates = 0;
    /// Frames shown.
    std::uint64_t presented = 0;
    /// Frames shown late: the frame was already due at the update before the one that showed it, but was not in the
    /// queue yet (the decoder fell behind). A frame that becomes due while the render loop stalls is not late: it is
    /// shown at the first update after its time.
    std::uint64_t late = 0;
    /// Due frames skipped because a newer frame was due at the same update (render stalls, videos faster than the
    /// display).
    std::uint64_t dropped = 0;
    /// Updates that kept showing a frame past its duration because the queue held nothing newer, while the decoder was
    /// still running (see update()'s `sourceEnded`): the presentation starved.
    std::uint64_t repeated = 0;
    /// Times the clock jumped to a frame that arrived more than ClockSettings::resyncThresholdSeconds late.
    std::uint64_t resyncs = 0;
    /// Frames of an older seek generation thrown away.
    std::uint64_t stale = 0;
    /// Media time skipped by resyncs, seconds.
    double resyncSeconds = 0.0;
    /// Largest delay between a frame's time and the update that showed it, seconds (render sampling included).
    double maxLatenessSeconds = 0.0;
    /// Sum of those delays, seconds: divided by `presented`, the mean.
    double totalLatenessSeconds = 0.0;
};

/// Result of one PresentationClock::update().
struct ClockUpdate {
    /// A frame was taken from the source to be shown (FrameSource::pop(true)): upload it.
    bool newFrame = false;
    /// Timing of the frame shown now (the new one, or the one kept); nothing before the first frame.
    std::optional<FrameTiming> shown;
    /// Media time of this update; nothing before the first frame.
    std::optional<double> mediaTime;
    /// Frames skipped at this update.
    std::uint32_t dropped = 0;
    /// The new frame was late (PresentationStats::late).
    bool late = false;
    /// The update kept a frame past its duration (PresentationStats::repeated).
    bool repeated = false;
    /// The clock jumped to the new frame (PresentationStats::resyncs).
    bool resynced = false;
};

/// Decides, from the render loop's time, which decoded frame is on screen (specification §9.4: presentation driven by
/// timestamps, not one frame per render frame). The media time starts at the timestamp of the first frame shown, at
/// the render time of the update that shows it, and then advances with the render time (stopped while paused). Each
/// update shows the newest frame whose timestamp is due, skipping older due frames (dropped), and keeps the current
/// frame when nothing newer is due (counted as repeated when the queue starved). Frames of a newer seek generation
/// start the clock again; older ones are thrown away. GPU-free and single-threaded (the render thread); deterministic
/// for given times, so tests drive it with synthetic frames.
class PresentationClock {
public:
    explicit PresentationClock(ClockSettings settings = {}) noexcept : m_settings(settings) {}

    /// Advances to render time `nowSeconds` (any monotonic seconds) and takes from `frames` what is due. `sourceEnded`
    /// tells that no more frames will come (the decoder reached the end without looping, or failed): an expired frame
    /// is then not counted as repeated. While paused nothing is taken.
    ClockUpdate update(double nowSeconds, FrameSource& frames, bool sourceEnded = false);

    /// Freezes the media time at render time `nowSeconds`. No-op when already paused.
    void pause(double nowSeconds) noexcept;
    /// Lets the media time run again from where pause() froze it. No-op when not paused.
    void resume(double nowSeconds) noexcept;
    [[nodiscard]] bool paused() const noexcept { return m_paused; }

    /// Forgets the current frame and the media time: the next update starts again at the first frame available (a new
    /// clip). Keeps the statistics and the generation.
    void restart() noexcept;

    /// Media time at render time `nowSeconds`; nothing before the first frame was shown.
    [[nodiscard]] std::optional<double> mediaTime(double nowSeconds) const noexcept;
    /// Timing of the frame on screen; nothing before the first frame.
    [[nodiscard]] const std::optional<FrameTiming>& current() const noexcept { return m_current; }
    /// Seek generation of the frames the clock accepts.
    [[nodiscard]] std::uint32_t generation() const noexcept { return m_generation; }

    [[nodiscard]] const PresentationStats& stats() const noexcept { return m_stats; }
    void resetStats() noexcept { m_stats = {}; }
    [[nodiscard]] const ClockSettings& settings() const noexcept { return m_settings; }

private:
    struct Anchor {
        double renderTime = 0.0; ///< render time at which the media time was `mediaTime`
        double mediaTime = 0.0;
    };

    /// Throws away frames of older generations and restarts on a newer one.
    void skipStaleFrames(FrameSource& frames);

    ClockSettings m_settings;
    std::optional<Anchor> m_anchor;
    std::optional<FrameTiming> m_current;
    /// Media time of the previous update (late-frame detection).
    std::optional<double> m_previousMediaTime;
    std::uint32_t m_generation = 0;
    bool m_paused = false;
    /// Media time frozen by pause() (meaningful with an anchor).
    double m_pausedMediaTime = 0.0;
    PresentationStats m_stats;
};

} // namespace rv::media
