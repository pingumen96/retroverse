// rv_audio's mixer, its loops and one-shots (I12 step 7, ADR-0021 §5): a loop played over and over from its first
// sample, ramped in over the cross-fade and out over the pause fade, holding its place while silent; the buses' gains;
// one-shots in a pool of 16 voices, the oldest stolen; the duck over everything but a one-shot that does not duck.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include <rv/audio/analysis.hpp>
#include <rv/audio/mixer.hpp>
#include <rv/audio/spatial.hpp>

using Catch::Approx;
using namespace rv::audio;

namespace {

constexpr int kRate = 48000;
constexpr std::size_t kBlock = 800;

/// A buffer of `length` samples, all `value` (a loop or not).
std::shared_ptr<const PcmBuffer> constant(std::size_t length, float value, bool loop) {
    auto buffer = std::make_shared<PcmBuffer>();
    buffer->samples.assign(length, value);
    buffer->sampleRate = kRate;
    buffer->loop = loop;
    return buffer;
}

/// A ramp 0, 1, 2, ... (a loop's place is readable from its samples).
std::shared_ptr<const PcmBuffer> counting(std::size_t length) {
    auto buffer = std::make_shared<PcmBuffer>();
    for (std::size_t i = 0; i < length; ++i) {
        buffer->samples.push_back(static_cast<float>(i) / static_cast<float>(length));
    }
    buffer->loop = true;
    return buffer;
}

std::vector<float> render(Mixer& mixer, std::size_t frames, double& at) {
    std::vector<float> out(frames * 2);
    mixer.render(out.data(), frames, at);
    at += static_cast<double>(frames) / kRate;
    return out;
}

} // namespace

TEST_CASE("audio/mixer: a loop fades in over the cross-fade, plays round, fades out and holds its place",
          "[audio][mixer][samples]") {
    Mixer mixer;
    const SampleSourceId loop = mixer.addSampleSource(constant(480, 0.4f, true));
    double at = 0.0;
    // Silent until it is asked to play.
    CHECK(rmsOf(render(mixer, kBlock, at)) == 0.0);
    mixer.setSampleControl(loop, SourceControl{0.5, 0.0, true});
    // 300 ms up to its gain (more than its 10 ms length: it plays round).
    std::vector<float> rising = render(mixer, 14400, at);
    CHECK(rising[0] == Approx(0.0).margin(1e-4));
    CHECK(rising.back() == Approx(0.2).margin(1e-3));
    CHECK(mixer.sampleGain(loop) == Approx(0.5));
    const std::vector<float> steady = render(mixer, kBlock, at);
    CHECK(steady[0] == Approx(0.2f));
    CHECK(steady[1] == Approx(0.2f)); // centred: both channels at unity
    // Stopped: down in the pause fade (30 ms), then nothing.
    mixer.setSampleControl(loop, SourceControl{0.5, 0.0, false});
    (void)render(mixer, 1440, at);
    CHECK(mixer.sampleGain(loop) == 0.0);
    CHECK(rmsOf(render(mixer, kBlock, at)) == 0.0);

    // A silent loop holds its place: it starts where it stopped.
    Mixer placed(MixerSettings{kRate, 0.0, 0.0});
    const SampleSourceId ramp = placed.addSampleSource(counting(1000));
    placed.setSampleControl(ramp, SourceControl{1.0, 0.0, true});
    double t = 0.0;
    const std::vector<float> first = render(placed, 250, t);
    CHECK(first[2 * 249] == Approx(249.0f / 1000.0f));
    placed.setSampleControl(ramp, SourceControl{1.0, 0.0, false});
    (void)render(placed, 500, t);
    placed.setSampleControl(ramp, SourceControl{1.0, 0.0, true});
    const std::vector<float> again = render(placed, 1, t);
    CHECK(again[0] == Approx(250.0f / 1000.0f)); // where it stopped
}

TEST_CASE("audio/mixer: the buses' gains, the pool of voices, the duck that a menu tick escapes",
          "[audio][mixer][samples]") {
    Mixer mixer(MixerSettings{kRate, 0.0, 0.0, 0.0, 0.0});
    double at = 0.0;
    // The ambience bus at half: a loop at half.
    const SampleSourceId loop = mixer.addSampleSource(constant(100, 0.4f, true));
    mixer.setBusGain(Bus::Ambience, 0.5);
    mixer.setSampleControl(loop, SourceControl{1.0, 0.0, true});
    std::vector<float> out = render(mixer, kBlock, at);
    CHECK(out[2 * 10] == Approx(0.2f));
    mixer.setSampleControl(loop, SourceControl{0.0, 0.0, false});
    (void)render(mixer, kBlock, at);

    // One-shots: each plays once from its start, then the voice is free.
    CHECK_FALSE(mixer.trigger(nullptr, OneShot{}));
    CHECK(mixer.trigger(constant(400, 0.25f, false), OneShot{1.0, 0.0, Bus::Effects, true}));
    CHECK(mixer.voicesPlaying() == 1);
    out = render(mixer, kBlock, at);
    CHECK(out[0] == Approx(0.25f));
    CHECK(out[2 * 399] == Approx(0.25f));
    CHECK(out[2 * 400] == 0.0f);
    CHECK(mixer.voicesPlaying() == 0);
    // Panned hard right: the left channel quiet.
    CHECK(mixer.trigger(constant(10, 0.25f, false), OneShot{1.0, 1.0, Bus::Effects, true}));
    out = render(mixer, kBlock, at);
    const StereoGains right = equalPowerGains(1.0);
    CHECK(out[0] == Approx(0.25 * right.left).margin(1e-6));
    CHECK(out[1] == Approx(0.25 * right.right));

    // Seventeen at once: the oldest is stolen.
    for (std::size_t i = 0; i < Mixer::kMaxVoices + 1; ++i) {
        CHECK(mixer.trigger(constant(2000, 0.01f, false), OneShot{}));
    }
    CHECK(mixer.voicesPlaying() == Mixer::kMaxVoices);
    CHECK(mixer.voicesStolen() == 1);
    (void)render(mixer, 4000, at);
    CHECK(mixer.voicesPlaying() == 0);

    // The duck (-9 dB) lowers a ducking one-shot, never one that does not duck; the effects bus applies to both.
    mixer.setDucked(true);
    (void)render(mixer, kBlock, at);
    const double duck = gainFromDecibels(kDuckDecibels);
    CHECK(mixer.trigger(constant(100, 0.4f, false), OneShot{1.0, 0.0, Bus::Effects, true}));
    out = render(mixer, kBlock, at);
    CHECK(out[0] == Approx(0.4 * duck));
    CHECK(mixer.trigger(constant(100, 0.4f, false), OneShot{1.0, 0.0, Bus::Effects, false}));
    out = render(mixer, kBlock, at);
    CHECK(out[0] == Approx(0.4f));
    mixer.setBusGain(Bus::Effects, 0.0);
    (void)render(mixer, kBlock, at);
    CHECK(mixer.trigger(constant(100, 0.4f, false), OneShot{1.0, 0.0, Bus::Effects, false}));
    CHECK(rmsOf(render(mixer, kBlock, at)) == 0.0);
}
