#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL, FFmpeg or miniaudio header is included.
//
// The clips' sound as the application decides it (I5 step 5, ADR-0016 §9): which output a run uses (--audio, RV_AUDIO,
// frame-limited runs), which screen is audible, its gain and pan, and when the sound ducks.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <rv/app/screen_scheduler.hpp>
#include <rv/audio/audio_engine.hpp>

namespace rv::app {

/// The run report's lists of the audio section keep their first entries only.
inline constexpr std::size_t kMaxAudioTimelineEntries = 2048;
/// The audible screen's level is sampled this often (the run's simulated time).
inline constexpr double kAudioLevelSampleSeconds = 0.25;

/// The output a run uses and why ("RV_AUDIO", "--audio", "frame-limited", "interactive").
struct AudioOutputChoice {
    audio::AudioOutput output = audio::AudioOutput::Null;
    std::string reason;
};

/// The run's output: RV_AUDIO's "null" or "off" (`environment`) forces it, whatever the command line asks (the test
/// presets and scripts/real-library.ps1 set RV_AUDIO=null, so that no test can play sound on the user's machine; any
/// other value is ignored); else --audio (`commandLine`); else null for a frame-limited run, the device otherwise.
[[nodiscard]] AudioOutputChoice resolveAudioOutput(std::optional<audio::AudioOutput> commandLine,
                                                   std::optional<std::string_view> environment, bool frameLimited);

/// A screen that may be heard this frame.
struct AudibleCandidate {
    std::string screen;
    ScreenClass screenClass = ScreenClass::Room;
    double distance = 0.0;
    /// In the view and in the room that holds the eye.
    bool visible = false;
    /// It shows a clip (its slot plays; a paused or stopped one is silent).
    bool playing = false;
};

/// The audible screen (ADR-0016 §9, "one dominant source"): among the visible ones playing within `proximityMeters`,
/// the best class, then the nearest, then the smallest screen id; none when there is none. With hysteresis: while the
/// screen `current` (the one heard so far) still qualifies, it stays audible unless a screen of a better class
/// qualifies, or one of its class is nearer by at least kAudibleHysteresisMeters (the ambient requests' margin), so
/// that two screens at about the same distance do not trade the sound back and forth.
inline constexpr double kAudibleHysteresisMeters = kAmbientHysteresisMeters;
[[nodiscard]] std::optional<std::size_t> chooseAudible(std::span<const AudibleCandidate> candidates,
                                                       double proximityMeters, std::string_view current = {});

/// The spatial part of an audible screen's sound: its gain (audio::spatialGain) and pan (from its azimuth).
struct SpatialSound {
    double gain = 0.0;
    double pan = 0.0;
};
/// A screen `distanceMeters` from the eye, seen at `azimuthRadians` (azimuthOf), within `proximityMeters`.
[[nodiscard]] SpatialSound spatialSound(double distanceMeters, double azimuthRadians, double proximityMeters) noexcept;
/// The azimuth of `target` seen from `eye` looking along `forward` in the horizontal plane: 0 ahead, positive to the
/// right, in (-pi, pi]; 0 when either direction has no horizontal length.
[[nodiscard]] double azimuthOf(double eyeX, double eyeZ, double forwardX, double forwardZ, double targetX,
                               double targetZ) noexcept;

/// The sound ducks (audio::kDuckDecibels) while the world's card, the directory or the quick catalog is open.
[[nodiscard]] constexpr bool audioDucked(bool cardShown, bool directoryOpen, bool catalogOpen) noexcept {
    return cardShown || directoryOpen || catalogOpen;
}

} // namespace rv::app
