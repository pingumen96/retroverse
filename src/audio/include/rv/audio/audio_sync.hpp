#pragma once

#include <cstdint>

namespace rv::audio {

/// ADR-0016 §9's sync of a clip's sound to its picture: the render clock is the master and the audio follows the
/// video's media time. Within kDeadband nothing is done; from there to kResyncBeyond the source's resampling ratio
/// corrects by up to kMaxCorrection (1%: 10 ms of drift recovered a second); beyond, a hard resync drops samples or
/// pads silence.
inline constexpr double kSyncDeadbandSeconds = 0.020;
inline constexpr double kSyncResyncBeyondSeconds = 0.120;
inline constexpr double kSyncMaxCorrection = 0.01;
/// The correction grows linearly with the error and is full from this error on (a time constant of 3 s below it).
inline constexpr double kSyncFullCorrectionSeconds = 0.030;
/// A correction that started goes on until the error is this small (the hysteresis that keeps the error from resting
/// at the deadband's edge).
inline constexpr double kSyncSettledSeconds = 0.005;

enum class SyncAction : std::uint8_t {
    None,    ///< in sync: ratio 1
    Correct, ///< the ratio speeds the sound up or slows it down a little
    Resync,  ///< too far: drop (sound behind) or pad silence (sound ahead) to the video's time
};

struct SyncDecision {
    SyncAction action = SyncAction::None;
    /// Input frames per output frame: above 1 when the sound is behind the picture (it catches up).
    double ratio = 1.0;
};

/// Decides, block by block, what the sound does about its error against the picture. GPU-free and deterministic.
class AudioSync {
public:
    /// `errorSeconds`: the sound's media time minus the picture's (negative: the sound is behind).
    [[nodiscard]] SyncDecision update(double errorSeconds) noexcept;
    /// Forgets a correction in progress (a new clip, a resync).
    void reset() noexcept { m_correcting = false; }
    [[nodiscard]] bool correcting() const noexcept { return m_correcting; }

private:
    bool m_correcting = false;
};

} // namespace rv::audio
