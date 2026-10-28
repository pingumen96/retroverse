#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <rv/audio/audio_output.hpp>
#include <rv/audio/mixer.hpp>
#include <rv/core/expected.hpp>

namespace rv::audio {

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
    /// Tests only: this many of the device's restarts (after a launch, or a retry) fail, as a device taken or gone
    /// would, so that the retries are proven on the null backend.
    int failDeviceStarts = 0;
};

/// When a device that stopped against our will is tried again (ADR-0016 §9): a device lost (miniaudio's stopped
/// notification) is tried at the next AudioEngine::maintain(), one that failed to start every kRetrySeconds, while the
/// output is not suspended. Times are those of maintain().
class DeviceRetry {
public:
    static constexpr double kRetrySeconds = 2.0;

    /// The device stopped by itself: tried at the next maintain().
    void lost() noexcept {
        m_pending = true;
        m_next = 0.0;
    }
    /// A start failed: tried again kRetrySeconds after `nowSeconds` (none: after the next maintain()'s time).
    void failed(std::optional<double> nowSeconds = std::nullopt) noexcept {
        m_pending = true;
        m_next = nowSeconds ? std::optional<double>(*nowSeconds + kRetrySeconds) : std::nullopt;
    }
    void recovered() noexcept { m_pending = false; }
    [[nodiscard]] bool pending() const noexcept { return m_pending; }
    /// Whether to try now (a failure not yet stamped is stamped with `nowSeconds`).
    [[nodiscard]] bool due(double nowSeconds) noexcept {
        if (!m_pending) {
            return false;
        }
        if (!m_next) {
            m_next = nowSeconds + kRetrySeconds;
        }
        return nowSeconds >= *m_next;
    }

private:
    bool m_pending = false;
    std::optional<double> m_next;
};

struct AudioEngineStats {
    /// Device: the output's callbacks; null: the pulls.
    std::uint64_t blocks = 0;
    std::uint64_t framesRendered = 0;
    std::uint64_t deviceStarts = 0;
    std::uint64_t deviceStops = 0;
    /// The device's trouble after its first start: starts that failed (after a launch, or a retry), stops it made by
    /// itself (lost), retries that brought it back, and reroutes miniaudio followed to a new default endpoint.
    std::uint64_t deviceFailures = 0;
    std::uint64_t deviceLosses = 0;
    std::uint64_t deviceRecoveries = 0;
    std::uint64_t deviceReroutes = 0;
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

    /// Before a launch, first: the mix starts fading out (MixerSettings::silenceFadeSeconds) without waiting, so that
    /// the fade runs while the screens' decoders stop. Idempotent.
    void beginSuspend();
    /// Then: the fade (begun here if beginSuspend() was not called) plays out, waiting only what remains of it on a
    /// device, and the device stops. Idempotent.
    void suspend();
    /// After the return: the device starts again and the mix fades in; a start that fails is retried by maintain().
    /// Idempotent.
    void resume();
    [[nodiscard]] bool suspended() const noexcept;

    /// Each frame (main thread), `nowSeconds` on any steadily increasing clock: a device lost, or not started again,
    /// is tried again (DeviceRetry) while the output is not suspended. Nothing for the null output.
    void maintain(double nowSeconds);
    /// Tests only: the device stops as a lost one would (miniaudio's stopped notification), for maintain() to restore.
    void simulateDeviceLoss();

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
