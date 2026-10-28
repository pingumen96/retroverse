#include <rv/audio/audio_engine.hpp>

#include <rv/audio/wav_file.hpp>
#include <rv/core/text.hpp>

#include "miniaudio_config.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

namespace rv::audio {

std::string_view toString(AudioOutput output) noexcept {
    switch (output) {
    case AudioOutput::Device:
        return "device";
    case AudioOutput::Null:
        return "null";
    case AudioOutput::Off:
        return "off";
    }
    return "off";
}

std::optional<AudioOutput> audioOutputFromName(std::string_view name) noexcept {
    if (name == "device") {
        return AudioOutput::Device;
    }
    if (name == "null") {
        return AudioOutput::Null;
    }
    if (name == "off") {
        return AudioOutput::Off;
    }
    return std::nullopt;
}

struct AudioEngine::Impl {
    explicit Impl(AudioEngineOptions engineOptions)
        : options(std::move(engineOptions)), mixer(options.mixer), origin(std::chrono::steady_clock::now()),
          failStarts(std::max(0, options.failDeviceStarts)) {}

    AudioEngineOptions options;
    Mixer mixer;
    std::chrono::steady_clock::time_point origin;
    std::string deviceName;

    // The device (Device only).
    bool contextReady = false;
    bool deviceReady = false;
    bool running = false;
    ma_context context{};
    ma_device device{};
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> deviceFrames{0};
    /// Set before our own starts, cleared before our own stops: a stop while set is a loss.
    std::atomic<bool> expectRunning{false};
    std::atomic<bool> lostFlag{false};
    std::atomic<std::uint64_t> reroutes{0};
    DeviceRetry retry;
    int failStarts = 0;
    std::uint64_t failures = 0;
    std::uint64_t losses = 0;
    std::uint64_t recoveries = 0;
    /// When the launch's fade began (beginSuspend()).
    std::optional<std::chrono::steady_clock::time_point> fadeStarted;

    // The null output.
    double carry = 0.0;
    std::vector<float> block;
    std::vector<float> captured;
    std::optional<double> captureStart;
    std::uint64_t pulls = 0;
    std::uint64_t pulledFrames = 0;
    std::uint64_t captureDropped = 0;
    bool finished = false;

    bool suspended = false;
    std::uint64_t starts = 0;
    std::uint64_t stops = 0;

    [[nodiscard]] double steadySeconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
    }

    static void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
        auto* impl = static_cast<Impl*>(device->pUserData);
        impl->mixer.render(static_cast<float*>(output), frames, impl->steadySeconds());
        impl->callbacks.fetch_add(1, std::memory_order_relaxed);
        impl->deviceFrames.fetch_add(frames, std::memory_order_relaxed);
    }

    /// miniaudio's thread: a stop we did not ask for is a loss (maintain() restores it); a reroute is counted.
    static void notificationCallback(const ma_device_notification* notification) {
        auto* impl = static_cast<Impl*>(notification->pDevice->pUserData);
        switch (notification->type) {
        case ma_device_notification_type_stopped:
            if (impl->expectRunning.load(std::memory_order_acquire)) {
                impl->lostFlag.store(true, std::memory_order_release);
            }
            break;
        case ma_device_notification_type_rerouted:
            impl->reroutes.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    }

    Status openDevice() {
        const ma_backend wasapi[] = {ma_backend_wasapi};
        const ma_backend null[] = {ma_backend_null};
        const ma_backend* backends = options.deviceOnNullBackend ? null : wasapi;
        ma_context_config contextConfig = ma_context_config_init();
        if (const ma_result result = ma_context_init(backends, 1, &contextConfig, &context); result != MA_SUCCESS) {
            return makeError(ErrorCode::PlatformError,
                             fmt::format("no audio context ({})", ma_result_description(result)));
        }
        contextReady = true;
        if (Status initialized = initDevice(); !initialized) {
            return initialized;
        }
        return startDevice();
    }

    Status initDevice() {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = kMixChannels;
        config.playback.shareMode = ma_share_mode_shared;
        config.sampleRate = static_cast<ma_uint32>(mixer.settings().sampleRate);
        config.dataCallback = &Impl::dataCallback;
        config.notificationCallback = &Impl::notificationCallback;
        config.pUserData = this;
        if (const ma_result result = ma_device_init(&context, &config, &device); result != MA_SUCCESS) {
            return makeError(ErrorCode::PlatformError,
                             fmt::format("the default audio endpoint cannot be opened ({})",
                                         ma_result_description(result)));
        }
        deviceReady = true;
        deviceName = device.playback.name;
        return ok();
    }

    Status startDevice() {
        if (!deviceReady) {
            return makeError(ErrorCode::PlatformError, "the audio device is not open");
        }
        if (running) {
            return ok();
        }
        expectRunning.store(true, std::memory_order_release);
        if (const ma_result result = ma_device_start(&device); result != MA_SUCCESS) {
            expectRunning.store(false, std::memory_order_release);
            return makeError(ErrorCode::PlatformError,
                             fmt::format("the audio device cannot start ({})", ma_result_description(result)));
        }
        running = true;
        ++starts;
        return ok();
    }

    void stopDevice() {
        if (deviceReady && running) {
            expectRunning.store(false, std::memory_order_release);
            ma_device_stop(&device);
            running = false;
            ++stops;
        }
    }

    void uninitDevice() {
        if (deviceReady) {
            expectRunning.store(false, std::memory_order_release);
            ma_device_uninit(&device);
            deviceReady = false;
            running = false;
        }
    }

    /// The device tried again: started where it is, else opened anew on the default endpoint (it may have changed).
    Status restartDevice() {
        if (failStarts > 0) {
            --failStarts;
            return makeError(ErrorCode::PlatformError, "the audio device cannot start (a failure the test asked for)");
        }
        if (deviceReady && !running) {
            if (Status started = startDevice(); started) {
                return started;
            }
        }
        uninitDevice();
        if (Status initialized = initDevice(); !initialized) {
            return initialized;
        }
        return startDevice();
    }

    void closeDevice() {
        stopDevice();
        uninitDevice();
        if (contextReady) {
            ma_context_uninit(&context);
            contextReady = false;
        }
    }
};

AudioEngine::AudioEngine(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

Expected<std::unique_ptr<AudioEngine>> AudioEngine::create(AudioEngineOptions options) {
    if (options.output == AudioOutput::Off) {
        return makeError(ErrorCode::InvalidArgument, "the audio output is off: no engine");
    }
    auto impl = std::make_unique<Impl>(std::move(options));
    if (impl->options.output == AudioOutput::Device) {
        if (Status opened = impl->openDevice(); !opened) {
            impl->closeDevice();
            return propagate(opened.error());
        }
        spdlog::info("Audio: the device '{}' plays the mix ({} Hz stereo, shared mode{})", impl->deviceName,
                     impl->mixer.settings().sampleRate,
                     impl->options.deviceOnNullBackend ? ", miniaudio's null backend" : "");
    } else {
        spdlog::info("Audio: the null output ({} Hz stereo, pulled each frame{})", impl->mixer.settings().sampleRate,
                     impl->options.capture ? ", the mix captured" : "");
    }
    return std::unique_ptr<AudioEngine>(new AudioEngine(std::move(impl)));
}

AudioEngine::~AudioEngine() {
    if (m_impl) {
        m_impl->closeDevice();
    }
}

AudioOutput AudioEngine::output() const noexcept {
    return m_impl->options.output;
}

Mixer& AudioEngine::mixer() noexcept {
    return m_impl->mixer;
}

const Mixer& AudioEngine::mixer() const noexcept {
    return m_impl->mixer;
}

const std::string& AudioEngine::deviceName() const noexcept {
    return m_impl->deviceName;
}

double AudioEngine::clockSeconds(double appSeconds) const {
    return m_impl->options.output == AudioOutput::Device ? m_impl->steadySeconds() : appSeconds;
}

void AudioEngine::pull(double startSeconds, double seconds) {
    Impl& impl = *m_impl;
    if (impl.options.output != AudioOutput::Null || !std::isfinite(seconds) || seconds <= 0.0) {
        return;
    }
    const double exact = impl.carry + seconds * static_cast<double>(impl.mixer.settings().sampleRate);
    const double whole = std::floor(exact);
    impl.carry = exact - whole;
    const auto frames = static_cast<std::size_t>(whole);
    if (frames == 0) {
        return;
    }
    impl.block.resize(frames * kMixChannels);
    impl.mixer.render(impl.block.data(), frames, startSeconds);
    ++impl.pulls;
    impl.pulledFrames += frames;
    if (impl.options.capture) {
        if (!impl.captureStart) {
            impl.captureStart = startSeconds;
        }
        const auto limit = static_cast<std::size_t>(kMaxCaptureSeconds * impl.mixer.settings().sampleRate) *
                           static_cast<std::size_t>(kMixChannels);
        const std::size_t room = limit > impl.captured.size() ? limit - impl.captured.size() : 0;
        const std::size_t kept = std::min(room, impl.block.size());
        impl.captured.insert(impl.captured.end(), impl.block.begin(),
                             impl.block.begin() + static_cast<std::ptrdiff_t>(kept));
        impl.captureDropped += (impl.block.size() - kept) / kMixChannels;
    }
}

void AudioEngine::beginSuspend() {
    Impl& impl = *m_impl;
    if (impl.suspended || impl.fadeStarted) {
        return;
    }
    impl.fadeStarted = std::chrono::steady_clock::now();
    impl.mixer.setSilenced(true);
}

void AudioEngine::suspend() {
    Impl& impl = *m_impl;
    if (impl.suspended) {
        return;
    }
    beginSuspend();
    impl.suspended = true;
    if (impl.options.output == AudioOutput::Device && impl.running) {
        // The fade plays out before the device stops (a little more than its length: a period's latency); only what
        // remains of it since beginSuspend() is waited for.
        const auto fade = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(impl.mixer.settings().silenceFadeSeconds + 0.02));
        const auto end = *impl.fadeStarted + fade;
        if (const auto now = std::chrono::steady_clock::now(); now < end) {
            std::this_thread::sleep_for(end - now);
        }
        impl.stopDevice();
        spdlog::info("Audio: the device stopped for the launch");
    }
}

void AudioEngine::resume() {
    Impl& impl = *m_impl;
    if (!impl.suspended && !impl.fadeStarted) {
        return;
    }
    impl.suspended = false;
    impl.fadeStarted.reset();
    impl.mixer.setSilenced(false);
    if (impl.options.output == AudioOutput::Device && !impl.running) {
        if (Status started = impl.restartDevice(); !started) {
            ++impl.failures;
            impl.retry.failed();
            spdlog::warn("Audio: {}; tried again every {:g} s", started.error().toString(), DeviceRetry::kRetrySeconds);
        } else {
            impl.retry.recovered();
            spdlog::info("Audio: the device started again after the launch");
        }
    }
}

void AudioEngine::maintain(double nowSeconds) {
    Impl& impl = *m_impl;
    if (impl.options.output != AudioOutput::Device) {
        return;
    }
    if (impl.lostFlag.exchange(false, std::memory_order_acq_rel)) {
        impl.expectRunning.store(false, std::memory_order_release);
        impl.running = false;
        ++impl.losses;
        impl.retry.lost();
        spdlog::warn("Audio: the device stopped by itself; trying it again");
    }
    if (impl.suspended || impl.fadeStarted || !impl.retry.due(nowSeconds)) {
        return;
    }
    if (Status started = impl.restartDevice(); !started) {
        ++impl.failures;
        impl.retry.failed(nowSeconds);
        spdlog::debug("Audio: {}; tried again in {:g} s", started.error().toString(), DeviceRetry::kRetrySeconds);
        return;
    }
    ++impl.recoveries;
    impl.retry.recovered();
    spdlog::info("Audio: the device '{}' plays again", impl.deviceName);
}

void AudioEngine::simulateDeviceLoss() {
    Impl& impl = *m_impl;
    if (impl.options.output != AudioOutput::Device || !impl.running) {
        return;
    }
    // As miniaudio's stopped notification reports a device gone: not one of our stops.
    impl.expectRunning.store(false, std::memory_order_release);
    ma_device_stop(&impl.device);
    impl.lostFlag.store(true, std::memory_order_release);
}

bool AudioEngine::suspended() const noexcept {
    return m_impl->suspended;
}

Status AudioEngine::finish() {
    Impl& impl = *m_impl;
    if (impl.finished || !impl.options.capture) {
        return ok();
    }
    impl.finished = true;
    if (impl.captureDropped > 0) {
        spdlog::warn("Audio: the capture keeps its first {:g} s; {} frame(s) left out", kMaxCaptureSeconds,
                     impl.captureDropped);
    }
    spdlog::info("Audio: the mix's {} frame(s) written to {}", impl.captured.size() / kMixChannels,
                 core::pathToUtf8(*impl.options.capture));
    return writeWavFile(*impl.options.capture, impl.captured, impl.mixer.settings().sampleRate, kMixChannels);
}

const std::optional<std::filesystem::path>& AudioEngine::capturePath() const noexcept {
    return m_impl->options.capture;
}

std::optional<double> AudioEngine::captureStartSeconds() const noexcept {
    return m_impl->captureStart;
}

AudioEngineStats AudioEngine::stats() const {
    const Impl& impl = *m_impl;
    AudioEngineStats stats;
    if (impl.options.output == AudioOutput::Device) {
        stats.blocks = impl.callbacks.load(std::memory_order_relaxed);
        stats.framesRendered = impl.deviceFrames.load(std::memory_order_relaxed);
    } else {
        stats.blocks = impl.pulls;
        stats.framesRendered = impl.pulledFrames;
    }
    stats.deviceStarts = impl.starts;
    stats.deviceStops = impl.stops;
    stats.deviceFailures = impl.failures;
    stats.deviceLosses = impl.losses;
    stats.deviceRecoveries = impl.recoveries;
    stats.deviceReroutes = impl.reroutes.load(std::memory_order_relaxed);
    stats.captureFramesDropped = impl.captureDropped;
    return stats;
}

} // namespace rv::audio
