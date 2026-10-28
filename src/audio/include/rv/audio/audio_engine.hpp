#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <rv/audio/mixer.hpp>
#include <rv/core/expected.hpp>

namespace rv::audio {

/// Where the mix goes (ADR-0016 §9, --audio).
enum class AudioOutput : std::uint8_t {
    Device, ///< miniaudio's shared-mode default endpoint: the interactive runs' default
    Null,   ///< no operating-system audio at all: the main thread pulls the mix (frame-limited runs, tests)
    Off,    ///< no mixer and no audio decoded
};

/// "device", "null", "off".
[[nodiscard]] std::string_view toString(AudioOutput output) noexcept;
[[nodiscard]] std::optional<AudioOutput> audioOutputFromName(std::string_view name) noexcept;

struct AudioEngineOptions {
    /// Device or Null (Off builds no engine).
    AudioOutput output = AudioOutput::Null;
    MixerSettings mixer;
    /// The null output's mix kept (at most kMaxCaptureSeconds) and written to this 32-bit float WAV by finish(): the
    /// offline mix the tests analyse.
    std::optional<std::filesystem::path> capture;
    /// Tests only: the device path opened on miniaudio's null backend (a timer thread, never the operating system's
    /// audio) instead of WASAPI.
    bool deviceOnNullBackend = false;
};

struct AudioEngineStats {
    /// Device: the output's callbacks; null: the pulls.
    std::uint64_t blocks = 0;
    std::uint64_t framesRendered = 0;
    std::uint64_t deviceStarts = 0;
    std::uint64_t deviceStops = 0;
    /// Frames of the capture dropped past kMaxCaptureSeconds.
    std::uint64_t captureFramesDropped = 0;
};

/// The output of the clips' sound (ADR-0016 §9): a Mixer and where its mix goes. A device renders it on miniaudio's
/// thread from the default endpoint's callback (stereo float at the mixer's rate, miniaudio converting to the
/// endpoint's format), its sources' clocks stamped with the steady clock; the null output never touches the operating
/// system's audio stack: the main thread pulls each frame's span of the mix (pull()), its sources' clocks stamped with
/// the run's simulated time, so a --fixed-dt run mixes dt x rate frames a frame, deterministically. Before a launch
/// the mix fades out and the device stops, leaving the endpoint to the emulator; it starts again after the return.
/// Main thread only (the device's callback aside).
class AudioEngine {
public:
    /// The longest capture kept.
    static constexpr double kMaxCaptureSeconds = 300.0;

    /// An engine for `options.output` (Device or Null). Errors: InvalidArgument for Off; PlatformError when the
    /// device cannot be opened or started (the caller may fall back to the null output).
    [[nodiscard]] static Expected<std::unique_ptr<AudioEngine>> create(AudioEngineOptions options);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] AudioOutput output() const noexcept;
    [[nodiscard]] Mixer& mixer() noexcept;
    [[nodiscard]] const Mixer& mixer() const noexcept;
    /// The device's name ("" for the null output).
    [[nodiscard]] const std::string& deviceName() const noexcept;

    /// The time the sources' clocks are stamped with now: `appSeconds` (the run's simulated time) for the null output,
    /// the steady clock for a device.
    [[nodiscard]] double clockSeconds(double appSeconds) const;
    /// The null output: renders `seconds` of the mix starting at `startSeconds` (the frames' fractions carried, so that
    /// the frames follow the time exactly), kept when capturing. A device: nothing (its thread renders).
    void pull(double startSeconds, double seconds);

    /// Before a launch: the mix fades out (MixerSettings::silenceFadeSeconds, waited for on a device) and the device
    /// stops. Idempotent.
    void suspend();
    /// After the return: the device starts again and the mix fades in. Idempotent.
    void resume();
    [[nodiscard]] bool suspended() const noexcept;

    /// Writes the capture, if any (once). Errors: those of writeWavFile.
    [[nodiscard]] Status finish();
    /// The capture's file, and the time of its first frame (startSeconds of the first pull).
    [[nodiscard]] const std::optional<std::filesystem::path>& capturePath() const noexcept;
    [[nodiscard]] std::optional<double> captureStartSeconds() const noexcept;

    [[nodiscard]] AudioEngineStats stats() const;

    struct Impl;

private:
    explicit AudioEngine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::audio
