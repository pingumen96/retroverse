// rv_audio's synthesized sounds (rv/audio/synth.hpp, I12 step 7, ADR-0021 §5): every sound's length, its peak, no NaN,
// bit for bit the same per seed, the loops flowing on at their seams, the hum's 50 Hz, the hiss above the hum, the
// footsteps' brightness by surface (parquet over the runner over the rug).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <span>
#include <string>

#include <rv/audio/analysis.hpp>
#include <rv/audio/synth.hpp>

using namespace rv::audio;

namespace {

constexpr int kRate = 48000;

/// Sign changes per second: a rough measure of how bright a sound is.
double zeroCrossingRate(std::span<const float> samples, int rate) {
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f)) {
            ++crossings;
        }
    }
    return static_cast<double>(crossings) * rate / static_cast<double>(samples.size());
}

/// The largest step between two neighbouring samples.
double largestStep(std::span<const float> samples) {
    double step = 0.0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        step = std::max(step, static_cast<double>(std::abs(samples[i] - samples[i - 1])));
    }
    return step;
}

} // namespace

TEST_CASE("audio/synth: every sound has its length, its peak and no NaN, the same per seed", "[audio][synth]") {
    for (const SoundId sound : kSoundIds) {
        INFO(std::string(toString(sound)));
        const PcmBuffer buffer = synthesize(sound, 3, kRate);
        CHECK(buffer.sampleRate == kRate);
        CHECK(buffer.loop == isLoop(sound));
        CHECK(buffer.samples.size() == static_cast<std::size_t>(std::llround(soundSeconds(sound) * kRate)));
        bool finite = true;
        for (const float sample : buffer.samples) {
            finite = finite && std::isfinite(sample);
        }
        CHECK(finite);
        CHECK(peakOf(buffer.samples) == kSynthPeak);
        CHECK(rmsOf(buffer.samples) > 0.0);
        // Pure: twice the same, bit for bit; another seed varies what has noise in it.
        CHECK(synthesize(sound, 3, kRate) == buffer);
        if (sound != SoundId::UiTick) {
            CHECK(synthesize(sound, 4, kRate).samples != buffer.samples);
        }
        // Another rate: the same length in seconds.
        CHECK(synthesize(sound, 3, 44100).samples.size() ==
              static_cast<std::size_t>(std::llround(soundSeconds(sound) * 44100)));
    }
    CHECK(toString(SoundId::FootstepRug) == "footstep-rug");
}

TEST_CASE("audio/synth: the loops flow on at their seams", "[audio][synth]") {
    for (const SoundId sound : {SoundId::CrtHum, SoundId::PhosphorHiss, SoundId::NeonBuzz, SoundId::RoomMurmur}) {
        INFO(std::string(toString(sound)));
        const PcmBuffer buffer = synthesize(sound, 11, kRate);
        const double seam = std::abs(buffer.samples.front() - buffer.samples.back());
        CHECK(seam <= largestStep(buffer.samples));
    }
    // The clock's ticks stand clear of its seam: both ends quiet.
    const PcmBuffer clock = synthesize(SoundId::ClockTick, 11, kRate);
    CHECK(std::abs(clock.samples.front()) < 0.01f);
    CHECK(std::abs(clock.samples.back()) < 0.01f);
}

TEST_CASE("audio/synth: the hum at 50 Hz, the hiss bright, the footsteps by surface", "[audio][synth]") {
    const PcmBuffer hum = synthesize(SoundId::CrtHum, 1, kRate);
    const double at50 = toneAmplitude(hum.samples, 50.0, kRate);
    CHECK(at50 > 0.1);
    CHECK(at50 > 5.0 * toneAmplitude(hum.samples, 73.0, kRate));
    CHECK(toneAmplitude(hum.samples, 100.0, kRate) > 0.05);

    const PcmBuffer hiss = synthesize(SoundId::PhosphorHiss, 1, kRate);
    CHECK(zeroCrossingRate(hiss.samples, kRate) > 10.0 * zeroCrossingRate(hum.samples, kRate));

    const PcmBuffer buzz = synthesize(SoundId::NeonBuzz, 1, kRate);
    CHECK(toneAmplitude(buzz.samples, 100.0, kRate) > 5.0 * toneAmplitude(buzz.samples, 130.0, kRate));

    // The first 60 ms (the heel) of each step: wood brightest, the rug dullest.
    const auto heel = [](SoundId sound) {
        const PcmBuffer step = synthesize(sound, 5, kRate);
        return zeroCrossingRate(std::span<const float>(step.samples).first(kRate * 6 / 100), kRate);
    };
    const double parquet = heel(SoundId::FootstepParquet);
    const double runner = heel(SoundId::FootstepRunner);
    const double rug = heel(SoundId::FootstepRug);
    CHECK(parquet > runner);
    CHECK(runner > rug);

    // The tick is short and high.
    const PcmBuffer tick = synthesize(SoundId::UiTick, 0, kRate);
    CHECK(toneAmplitude(tick.samples, 2600.0, kRate) > toneAmplitude(tick.samples, 1000.0, kRate));
}
