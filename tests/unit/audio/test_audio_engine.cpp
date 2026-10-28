// rv_audio's output (I5 step 5, ADR-0016 §9): the null output pulled frame by frame (dt x rate frames, the fractions
// carried) and its capture, the device path on miniaudio's own null backend (never the system's audio), suspend and
// resume around a launch; WAV files and the tone analysis the audio tests use.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <thread>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/audio/analysis.hpp>
#include <rv/audio/audio_engine.hpp>
#include <rv/audio/wav_file.hpp>
#include <rv/core/file_io.hpp>

using Catch::Approx;
using namespace rv::audio;

namespace {

std::vector<float> tone(double frequency, double amplitude, int rate, double seconds, int channels = 1) {
    const auto frames = static_cast<std::size_t>(seconds * rate);
    std::vector<float> samples(frames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto value = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / rate));
        for (int channel = 0; channel < channels; ++channel) {
            samples[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)] = value;
        }
    }
    return samples;
}

} // namespace

TEST_CASE("audio/analysis: a tone's amplitude, frequency and level; beeps' onsets", "[audio][analysis]") {
    const std::vector<float> a = tone(1000.0, 0.5, 48000, 1.0);
    CHECK(toneAmplitude(a, 1000.0, 48000) == Approx(0.5).epsilon(0.002));
    CHECK(toneAmplitude(a, 440.0, 48000) < 0.5e-3); // far below -40 dB
    CHECK(toneFrequency(a, 48000) == Approx(1000.0).margin(0.01));
    CHECK(rmsOf(a) == Approx(0.5 / std::numbers::sqrt2).epsilon(0.001));
    CHECK(peakOf(a) == Approx(0.5).epsilon(0.001));
    CHECK(toneFrequency(tone(997.3, 0.3, 44100, 2.0), 44100) == Approx(997.3).margin(0.01));
    // Two beeps of 50 ms, at 0.25 s and 0.75 s.
    std::vector<float> beeps(48000, 0.0f);
    for (const double start : {0.25, 0.75}) {
        for (int index = 0; index < 2400; ++index) {
            beeps[static_cast<std::size_t>(start * 48000) + static_cast<std::size_t>(index)] =
                static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * index / 48000.0));
        }
    }
    const std::vector<double> found = onsets(beeps, 48000, 0.05, 0.1);
    REQUIRE(found.size() == 2);
    CHECK(found[0] == Approx(0.25).margin(0.0005));
    CHECK(found[1] == Approx(0.75).margin(0.0005));
    const std::vector<float> stereo = {0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(channelOf(stereo, 2, 1) == std::vector<float>{0.2f, 0.4f});
    CHECK(stereoPower(stereo)[1] == Approx(0.125));
}

TEST_CASE("audio/wav_file: a float WAV round trip, 16-bit PCM read", "[audio][wav]") {
    const rv::test::TempDir dir;
    const std::vector<float> samples = tone(440.0, 0.8, 48000, 0.1, 2);
    const auto file = dir.path() / "mix.wav";
    REQUIRE(writeWavFile(file, samples, 48000, 2).has_value());
    const auto read = readWavFile(file);
    REQUIRE(read.has_value());
    CHECK(read->sampleRate == 48000);
    CHECK(read->channels == 2);
    CHECK(read->samples == samples);
    CHECK(read->frames() == samples.size() / 2);
    CHECK_FALSE(writeWavFile(file, samples, 48000, 7).has_value()); // not whole frames
    const auto empty = dir.path() / "empty.wav";
    REQUIRE(writeWavFile(empty, {}, 48000, 1).has_value());
    CHECK(readWavFile(empty)->samples.empty());
    const auto text = dir.path() / "text.wav";
    REQUIRE(rv::core::writeTextFileAtomic(text, "not a wave").has_value());
    CHECK(readWavFile(text).error().code == rv::ErrorCode::ParseError);
}

TEST_CASE("audio/engine: the null output mixes dt x rate frames a frame and captures them", "[audio][engine]") {
    const rv::test::TempDir dir;
    AudioEngineOptions options;
    options.output = AudioOutput::Null;
    options.capture = dir.path() / "capture.wav";
    auto engine = AudioEngine::create(options);
    REQUIRE(engine.has_value());
    AudioEngine& audio = **engine;
    CHECK(audio.output() == AudioOutput::Null);
    CHECK(audio.clockSeconds(12.5) == 12.5); // the run's own time
    CHECK(audio.deviceName().empty());
    // 600 frames of --fixed-dt 0.016667: 800.016 frames each, the fractions carried.
    for (int frame = 1; frame <= 600; ++frame) {
        audio.pull(frame * 0.016667, 0.016667);
    }
    const AudioEngineStats stats = audio.stats();
    CHECK(stats.blocks == 600);
    CHECK(stats.framesRendered == static_cast<std::uint64_t>(std::floor(600 * 0.016667 * 48000)));
    CHECK(audio.captureStartSeconds() == Approx(0.016667));
    audio.suspend();
    CHECK(audio.suspended());
    audio.resume();
    CHECK_FALSE(audio.suspended());
    CHECK(audio.stats().deviceStops == 0); // no device
    REQUIRE(audio.finish().has_value());
    const auto wav = readWavFile(*options.capture);
    REQUIRE(wav.has_value());
    CHECK(wav->frames() == stats.framesRendered);
    CHECK(peakOf(wav->samples) == 0.0); // no source: silence
    CHECK_FALSE(AudioEngine::create(AudioEngineOptions{AudioOutput::Off}).has_value());
}

TEST_CASE("audio/engine: the device path renders the mix from its callback (miniaudio's null backend, no system audio)",
          "[audio][engine]") {
    AudioEngineOptions options;
    options.output = AudioOutput::Device;
    options.deviceOnNullBackend = true;
    auto engine = AudioEngine::create(options);
    REQUIRE(engine.has_value());
    AudioEngine& audio = **engine;
    CHECK(audio.output() == AudioOutput::Device);
    CHECK(audio.stats().deviceStarts == 1);
    const double before = audio.clockSeconds(0.0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (audio.stats().framesRendered < 4800 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(audio.stats().framesRendered >= 4800); // its thread pulls the mix
    CHECK(audio.clockSeconds(0.0) > before);     // the steady clock, not the run's time
    audio.pull(0.0, 1.0);                        // nothing: the device's thread renders
    audio.suspend();
    CHECK(audio.stats().deviceStops == 1);
    const std::uint64_t stopped = audio.stats().framesRendered;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(audio.stats().framesRendered == stopped);
    audio.resume();
    CHECK(audio.stats().deviceStarts == 2);
}

TEST_CASE("audio/engine: a device lost or not started again is retried every 2 s, never while suspended",
          "[audio][engine]") {
    SECTION("the retry's schedule") {
        DeviceRetry retry;
        CHECK_FALSE(retry.due(0.0));
        retry.lost();
        CHECK(retry.due(5.0)); // a loss: at once
        retry.failed(5.0);
        CHECK_FALSE(retry.due(6.9));
        CHECK(retry.due(7.0));
        retry.failed(); // stamped by the next due()
        CHECK_FALSE(retry.due(10.0));
        CHECK_FALSE(retry.due(11.9));
        CHECK(retry.due(12.0));
        retry.recovered();
        CHECK_FALSE(retry.pending());
        CHECK_FALSE(retry.due(100.0));
    }
    SECTION("a start after a launch fails twice, then the device plays again (miniaudio's null backend)") {
        AudioEngineOptions options;
        options.output = AudioOutput::Device;
        options.deviceOnNullBackend = true;
        options.failDeviceStarts = 2;
        auto engine = AudioEngine::create(options);
        REQUIRE(engine.has_value());
        AudioEngine& audio = **engine;
        audio.suspend();
        audio.resume(); // fails: retried from maintain()
        CHECK(audio.stats().deviceFailures == 1);
        audio.maintain(100.0); // stamps the failure
        audio.maintain(101.9);
        CHECK(audio.stats().deviceFailures == 1);
        audio.maintain(102.0); // the first retry fails too
        CHECK(audio.stats().deviceFailures == 2);
        audio.maintain(103.9);
        CHECK(audio.stats().deviceRecoveries == 0);
        audio.maintain(104.0);
        CHECK(audio.stats().deviceRecoveries == 1);
        CHECK(audio.stats().deviceStarts == 2);
        const std::uint64_t before = audio.stats().framesRendered;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (audio.stats().framesRendered < before + 4800 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(audio.stats().framesRendered >= before + 4800); // rendering again

        // Lost while playing: tried again at the next maintain().
        audio.simulateDeviceLoss();
        audio.maintain(105.0);
        CHECK(audio.stats().deviceLosses == 1);
        CHECK(audio.stats().deviceRecoveries == 2);
        CHECK(audio.stats().deviceStarts == 3);
        audio.maintain(200.0); // nothing pending
        CHECK(audio.stats().deviceStarts == 3);
    }
    SECTION("suspended, the device is left alone") {
        AudioEngineOptions options;
        options.output = AudioOutput::Device;
        options.deviceOnNullBackend = true;
        options.failDeviceStarts = 1;
        auto engine = AudioEngine::create(options);
        REQUIRE(engine.has_value());
        AudioEngine& audio = **engine;
        audio.suspend();
        audio.resume();
        CHECK(audio.stats().deviceFailures == 1);
        audio.suspend(); // another launch before the retry
        audio.maintain(0.0);
        audio.maintain(1000.0);
        CHECK(audio.stats().deviceFailures == 1);
        CHECK(audio.stats().deviceRecoveries == 0);
        audio.resume(); // after it, the start succeeds
        CHECK(audio.stats().deviceFailures == 1);
        CHECK(audio.stats().deviceStarts == 2);
    }
    SECTION("the null output has no device to retry") {
        auto engine = AudioEngine::create(AudioEngineOptions{});
        REQUIRE(engine.has_value());
        (*engine)->maintain(10.0);
        CHECK((*engine)->stats().deviceFailures == 0);
    }
}
