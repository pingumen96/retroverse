#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <rv/audio/audio_ring.hpp>
#include <rv/audio/audio_sync.hpp>

namespace rv::audio {

/// A source of a Mixer: one decoder slot's sound.
using SourceId = std::uint32_t;

/// The mixer's fades (ADR-0016 §9).
struct MixerSettings {
    int sampleRate = kMixRate;
    /// A source's gain moves at most from 0 to 1 in this time: the 300 ms cross-fade between two screens, and the
    /// smoothing of the spatial gain as the eye moves.
    double crossFadeSeconds = 0.3;
    /// A paused source fades out in this time (and a stopped one).
    double pauseFadeSeconds = 0.03;
    /// Mute and unmute ("at once": just short enough not to click).
    double muteFadeSeconds = 0.01;
    /// The duck goes down and up in this time.
    double duckFadeSeconds = 0.1;
    /// The fade before a launch stops the output.
    double silenceFadeSeconds = 0.05;
};

/// What the main thread asks of a source each frame.
struct SourceControl {
    /// The source's own gain: the bus, the screen's volume and its spatial gain (0: not audible). The master, the duck
    /// and the mute apply over it.
    double gain = 0.0;
    /// -1 left .. +1 right (equalPowerGains).
    double pan = 0.0;
    /// False: the picture is paused or stopped; the sound fades out in pauseFadeSeconds and reads nothing more.
    bool playing = false;
};

/// The picture's clock a source follows: the video's media time `mediaSeconds` at time `atSeconds` of the clock the
/// mixer is rendered with (the run's simulated time for the null output, the steady clock for a device), advancing from
/// there while `running`.
struct SourceClock {
    std::optional<double> mediaSeconds;
    double atSeconds = 0.0;
    std::uint32_t generation = 0;
    bool running = false;
};

/// The sync of every source over the mixer's life: each block rendered while a source was audible measured the error
/// between its sound and its picture (before that block's correction).
struct SyncSummary {
    std::uint64_t blocks = 0;
    std::uint64_t resyncs = 0;
    std::uint64_t corrections = 0;
    /// Frames of an audible source the ring could not give (the decoder behind or the clip without sound).
    std::uint64_t underrunFrames = 0;
    double p50Milliseconds = 0.0;
    double p95Milliseconds = 0.0;
    double maxMilliseconds = 0.0;
};

/// Mixes the screens' sources into interleaved stereo float frames at MixerSettings::sampleRate (ADR-0016 §9). Each
/// source reads its AudioRing (a clip's decoded sound) at its picture's media time: within the sync's deadband it copies
/// the samples exactly; within its correction band it reads them at a ratio up to 1% off by linear interpolation; past
/// it, it drops samples or pads silence (AudioSync). Gains ramp: a source's at most from 0 to 1 in crossFadeSeconds (so
/// that a change of audible screen cross-fades), down in pauseFadeSeconds when it pauses; then the master, the duck
/// (kDuckDecibels), the mute and the launch's silence, each with its own fade. The sum is clamped to [-1, 1].
///
/// Deterministic: the same calls with the same rings give the same samples, bit for bit (no clock is read). Thread-safe:
/// the main thread sets the controls while the output's thread renders (one mutex, held for a block).
class Mixer {
public:
    explicit Mixer(MixerSettings settings = {});
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    [[nodiscard]] const MixerSettings& settings() const noexcept { return m_settings; }

    /// A new source, silent, without a ring.
    [[nodiscard]] SourceId addSource();
    /// The ring of the source's next clip (none: silent): the source starts again (a resync at its first block), its
    /// gain from 0.
    void setRing(SourceId source, std::shared_ptr<AudioRing> ring);
    void setControl(SourceId source, const SourceControl& control);
    void setClock(SourceId source, const SourceClock& clock);

    /// The master gain (video.json's master times its video bus).
    void setMaster(double gain);
    void setDucked(bool ducked);
    void setMuted(bool muted);
    /// Fades everything out (silenceFadeSeconds) before a launch stops the output, or back in.
    void setSilenced(bool silenced);
    [[nodiscard]] bool muted() const;
    [[nodiscard]] bool ducked() const;

    /// Renders `frames` frames into `out` (2 * frames floats, overwritten), the block starting at `blockStartSeconds`
    /// of the clock the sources' SourceClocks are stamped with.
    void render(float* out, std::size_t frames, double blockStartSeconds);

    /// The source's gain as rendered last (its ramp; 0: silent), its target included in none of the master gains.
    [[nodiscard]] double sourceGain(SourceId source) const;
    /// The media time of the next sample the source plays; none before its first.
    [[nodiscard]] std::optional<double> sourceMediaTime(SourceId source) const;
    /// The product of the master, duck, mute and silence gains as rendered last.
    [[nodiscard]] double outputGain() const;
    [[nodiscard]] SyncSummary syncSummary() const;
    /// Frames rendered so far.
    [[nodiscard]] std::uint64_t framesRendered() const;

private:
    struct Ramp {
        double current = 0.0;
        double target = 0.0;
        /// Largest change per frame.
        double step = 1.0;

        double advance() noexcept;
    };
    struct Source {
        std::shared_ptr<AudioRing> ring;
        SourceControl control;
        SourceClock clock;
        Ramp gain;
        AudioSync sync;
        float previous[2] = {0.0f, 0.0f};
        double previousTime = 0.0;
        bool hasPrevious = false;
        double fraction = 0.0;
        bool needResync = true;
        /// The generation the source played last (a seek starts it again).
        std::uint32_t generation = 0;
    };

    /// Renders the source into m_scratch; false when it gave nothing (silent, no picture yet, nothing decoded).
    [[nodiscard]] bool renderSource(Source& source, std::size_t frames, double blockStartSeconds);
    [[nodiscard]] bool consumeOne(Source& source);
    [[nodiscard]] bool peekNext(Source& source, float* next);
    /// Drops the sound up to the picture's time `target`, or pads silence from `index` up to it, and takes its first
    /// sample (needResync stays set until it could).
    void resync(Source& source, double target, std::size_t frames, std::size_t& index);
    void recordError(double seconds);

    MixerSettings m_settings;
    mutable std::mutex m_mutex;
    std::vector<Source> m_sources;
    std::vector<float> m_scratch;
    Ramp m_master;
    Ramp m_duck;
    Ramp m_mute;
    Ramp m_silence;
    bool m_ducked = false;
    bool m_muted = false;
    std::uint64_t m_framesRendered = 0;

    // The sync's measures.
    std::vector<float> m_errorsMs;
    SyncSummary m_counts;
};

} // namespace rv::audio
