// rv_audio's mixer (I5 step 5, ADR-0016 §9) on synthetic rings: the samples copied exactly in sync, the sync's
// correction and resync against the picture's clock, generations, fades (the 300 ms cross-fade, the 30 ms pause), the
// duck and the mute, and an offline mix equal bit for bit over two runs.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include <rv/audio/analysis.hpp>
#include <rv/audio/audio_ring.hpp>
#include <rv/audio/mixer.hpp>
#include <rv/audio/spatial.hpp>

using Catch::Approx;
using namespace rv::audio;

namespace {

constexpr int kRate = 48000;
/// A 60 Hz frame of the null output.
constexpr std::size_t kBlock = 800;
constexpr double kBlockSeconds = static_cast<double>(kBlock) / kRate;

/// Writes a tone into a ring as a decoder would, ahead of the mix: frame n of the clip at media time
/// ptsOffset + n / rate.
struct Feeder {
    std::shared_ptr<AudioRing> ring = std::make_shared<AudioRing>(kRate);
    double frequency = 440.0;
    double amplitude = 0.5;
    std::uint32_t generation = 0;
    double ptsOffset = 0.0;
    std::uint64_t written = 0;

    [[nodiscard]] float sample(std::uint64_t frame) const {
        return static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(frame) /
                                                       static_cast<double>(kRate)));
    }

    /// Writes 10 ms chunks until the ring holds media time `until` (or is full).
    void topUp(double until) {
        constexpr std::size_t kChunk = 480;
        std::vector<float> chunk(kChunk * 2);
        while (ptsOffset + static_cast<double>(written) / kRate < until &&
               ring->availableFrames() + kChunk <= ring->capacityFrames()) {
            for (std::size_t index = 0; index < kChunk; ++index) {
                chunk[index * 2] = sample(written + index);
                chunk[index * 2 + 1] = -sample(written + index);
            }
            ring->write(chunk, ptsOffset + static_cast<double>(written) / kRate, generation);
            written += kChunk;
        }
    }
};

SourceClock clockAt(double media, double at, std::uint32_t generation = 0) {
    return SourceClock{media, at, generation, true};
}

/// Renders `blocks` blocks of the null output from time `start`, the feeders topped up 200 ms ahead of `media(t)` before
/// each; returns the interleaved mix.
template <class MediaOf>
std::vector<float> renderBlocks(Mixer& mixer, std::vector<Feeder*> feeders, std::size_t blocks, double start,
                                MediaOf media) {
    std::vector<float> out(blocks * kBlock * 2);
    for (std::size_t block = 0; block < blocks; ++block) {
        const double at = start + static_cast<double>(block) * kBlockSeconds;
        for (Feeder* feeder : feeders) {
            feeder->topUp(media(at) + 0.2);
        }
        mixer.render(out.data() + block * kBlock * 2, kBlock, at);
    }
    return out;
}

} // namespace

TEST_CASE("audio/mixer: in sync, a centred source at full gain is copied sample for sample", "[audio][mixer]") {
    Mixer mixer;
    Feeder feeder;
    const SourceId source = mixer.addSource();
    mixer.setRing(source, feeder.ring);
    mixer.setControl(source, SourceControl{1.0, 0.0, true});
    mixer.setClock(source, clockAt(0.0, 0.0));
    // 1 s: the gain rises from the first sample over the cross-fade's 300 ms, then the samples pass unchanged.
    const std::vector<float> out = renderBlocks(mixer, {&feeder}, 60, 0.0, [](double t) { return t; });
    CHECK(out[1] == 0.0f); // silent at the very start (gain 0)
    std::size_t differing = 0;
    for (std::size_t frame = 14400; frame < 60 * kBlock; ++frame) {
        differing += out[frame * 2] != feeder.sample(frame) || out[frame * 2 + 1] != -feeder.sample(frame) ? 1 : 0;
    }
    CHECK(differing == 0);
    // Half way through the fade-in, half the level.
    CHECK(std::abs(out[7200 * 2]) <= std::abs(feeder.sample(7200)) * 0.51f);
    const SyncSummary sync = mixer.syncSummary();
    CHECK(sync.resyncs == 0);
    CHECK(sync.corrections == 0);
    CHECK(sync.underrunFrames == 0);
    CHECK(sync.maxMilliseconds < 0.5);
}

TEST_CASE("audio/mixer: a picture running 0.5% fast is followed by the correction within 20 ms, never by a resync",
          "[audio][mixer][sync]") {
    Mixer mixer;
    Feeder feeder;
    const SourceId source = mixer.addSource();
    mixer.setRing(source, feeder.ring);
    mixer.setControl(source, SourceControl{1.0, 0.0, true});
    // Each frame the picture's clock says it is at 1.005 t: without a correction the sound would fall 100 ms behind
    // in 20 s.
    const auto media = [](double t) { return 1.005 * t; };
    std::vector<float> block(kBlock * 2);
    double worst = 0.0;
    for (int frame = 0; frame < 20 * 60; ++frame) {
        const double at = frame * kBlockSeconds;
        mixer.setClock(source, clockAt(media(at), at));
        feeder.topUp(media(at) + 0.2);
        mixer.render(block.data(), kBlock, at);
        if (frame > 60) {
            const std::optional<double> heard = mixer.sourceMediaTime(source);
            REQUIRE(heard);
            worst = std::max(worst, std::abs(*heard - media(at + kBlockSeconds)));
        }
    }
    const SyncSummary sync = mixer.syncSummary();
    CHECK(sync.resyncs == 0);
    CHECK(sync.corrections > 0);
    CHECK(sync.underrunFrames == 0);
    CHECK(worst < 0.022); // held within the deadband (and a block)
    CHECK(sync.p95Milliseconds < 21.0);
    CHECK(sync.p50Milliseconds > 0.0);
    CHECK(sync.p50Milliseconds <= sync.p95Milliseconds);
    CHECK(sync.p95Milliseconds <= sync.maxMilliseconds);
    CHECK(sync.maxMilliseconds < 22.0);
}

TEST_CASE("audio/mixer: 80 ms behind, the sound catches up by the correction alone", "[audio][mixer][sync]") {
    Mixer mixer;
    Feeder feeder;
    const SourceId source = mixer.addSource();
    mixer.setRing(source, feeder.ring);
    mixer.setControl(source, SourceControl{1.0, 0.0, true});
    std::vector<float> block(kBlock * 2);
    double offset = 0.0;
    double error = 0.0;
    for (int frame = 0; frame < 15 * 60; ++frame) {
        const double at = frame * kBlockSeconds;
        if (frame == 60) {
            offset = 0.08; // the picture 80 ms ahead: within the correction's band
        }
        mixer.setClock(source, clockAt(at + offset, at));
        feeder.topUp(at + offset + 0.2);
        mixer.render(block.data(), kBlock, at);
        const std::optional<double> heard = mixer.sourceMediaTime(source);
        REQUIRE(heard);
        error = *heard - (at + kBlockSeconds + offset);
        if (frame == 6 * 60) {
            CHECK(error > -0.040); // 1% recovers 10 ms a second
            CHECK(error < -0.015);
        }
    }
    CHECK(std::abs(error) < 0.020); // in sync again
    CHECK(mixer.syncSummary().resyncs == 0);
    CHECK(mixer.syncSummary().corrections > 300);
}

TEST_CASE("audio/mixer: a jump of the picture resyncs: samples dropped forward, silence padded back",
          "[audio][mixer][sync]") {
    Mixer mixer;
    Feeder feeder;
    const SourceId source = mixer.addSource();
    mixer.setRing(source, feeder.ring);
    mixer.setControl(source, SourceControl{1.0, 0.0, true});
    std::vector<float> block(kBlock * 2);
    double offset = 0.0; // media = t + offset
    for (int frame = 0; frame < 120; ++frame) {
        const double at = frame * kBlockSeconds;
        if (frame == 60) {
            offset = 0.3; // a stall: the picture jumps 300 ms ahead
        }
        mixer.setClock(source, clockAt(at + offset, at));
        feeder.topUp(at + offset + 0.25);
        mixer.render(block.data(), kBlock, at);
        if (frame == 60) {
            const std::optional<double> heard = mixer.sourceMediaTime(source);
            REQUIRE(heard);
            CHECK(*heard == Approx(at + offset + kBlockSeconds).margin(0.001)); // dropped up to the picture at once
        }
    }
    CHECK(mixer.syncSummary().resyncs == 1);
    CHECK(feeder.ring->stats().skippedFrames >= static_cast<std::uint64_t>(0.29 * kRate));

    // The picture goes back 200 ms (the sound is ahead): silence until its time comes.
    const double at = 120 * kBlockSeconds;
    mixer.setClock(source, clockAt(at + offset - 0.2, at));
    mixer.render(block.data(), kBlock, at);
    CHECK(mixer.syncSummary().resyncs == 2);
    CHECK(peakOf(block) == 0.0f); // the whole block padded (200 ms > 16.7 ms)
}

TEST_CASE("audio/mixer: a seek's generation throws the old position's sound away", "[audio][mixer][sync]") {
    Mixer mixer;
    Feeder old;
    const SourceId source = mixer.addSource();
    mixer.setRing(source, old.ring);
    mixer.setControl(source, SourceControl{1.0, 0.0, true});
    mixer.setClock(source, clockAt(0.0, 0.0));
    old.topUp(0.4);
    std::vector<float> block(kBlock * 2);
    mixer.render(block.data(), kBlock, 0.0);
    // The decoder seeks to 10 s: generation 1's chunks follow generation 0's in the same ring.
    Feeder seeked;
    seeked.ring = old.ring;
    seeked.generation = 1;
    seeked.ptsOffset = 10.0;
    seeked.topUp(10.1);
    mixer.setClock(source, clockAt(10.0, kBlockSeconds, 1));
    mixer.render(block.data(), kBlock, kBlockSeconds);
    const std::optional<double> heard = mixer.sourceMediaTime(source);
    REQUIRE(heard);
    CHECK(*heard == Approx(10.0 + kBlockSeconds).margin(0.001));
    CHECK(old.ring->stats().skippedFrames >= static_cast<std::uint64_t>(0.38 * kRate));
}

TEST_CASE("audio/mixer: the cross-fade, the pause's fade, the duck and the mute", "[audio][mixer]") {
    Mixer mixer;
    Feeder a;
    a.frequency = 440.0;
    Feeder b;
    b.frequency = 1000.0;
    const SourceId first = mixer.addSource();
    const SourceId second = mixer.addSource();
    mixer.setRing(first, a.ring);
    mixer.setRing(second, b.ring);
    mixer.setClock(first, clockAt(0.0, 0.0));
    mixer.setClock(second, clockAt(0.0, 0.0));
    mixer.setControl(first, SourceControl{1.0, 0.0, true});
    // The second waits (its ring kept, nothing read) until it is heard.
    mixer.setControl(second, SourceControl{0.0, 0.0, false});
    const auto media = [](double t) { return t; };
    std::vector<float> settled = renderBlocks(mixer, {&a, &b}, 30, 0.0, media); // 0.5 s: the first at full gain
    CHECK(mixer.sourceGain(first) == 1.0);
    CHECK(mixer.sourceGain(second) == 0.0);

    // The audible screen changes: 300 ms later the first is silent and the second at full gain.
    mixer.setControl(first, SourceControl{0.0, 0.0, true});
    mixer.setControl(second, SourceControl{1.0, 0.0, true});
    renderBlocks(mixer, {&a, &b}, 9, 30 * kBlockSeconds, media); // 150 ms
    CHECK(mixer.sourceGain(first) == Approx(0.5).margin(0.01));
    CHECK(mixer.sourceGain(second) == Approx(0.5).margin(0.1)); // rises from its first sample on (a block's resync)
    const std::vector<float> after = renderBlocks(mixer, {&a, &b}, 30, 39 * kBlockSeconds, media);
    CHECK(mixer.sourceGain(first) == 0.0);
    CHECK(mixer.sourceGain(second) == 1.0);
    const std::vector<float> left = channelOf(after, 2, 0);
    const std::span<const float> last(left.data() + left.size() / 2, left.size() / 2);
    CHECK(toneAmplitude(last, 1000.0, kRate) == Approx(0.5).epsilon(0.01));
    CHECK(toneAmplitude(last, 440.0, kRate) < 0.5 * 0.01); // the other's tone more than 40 dB down

    // The duck: -9 dB.
    mixer.setDucked(true);
    const std::vector<float> ducked = renderBlocks(mixer, {&a, &b}, 30, 69 * kBlockSeconds, media);
    const std::vector<float> duckedLeft = channelOf(ducked, 2, 0);
    const std::span<const float> duckedTail(duckedLeft.data() + duckedLeft.size() / 2, duckedLeft.size() / 2);
    CHECK(decibelsFromGain(toneAmplitude(duckedTail, 1000.0, kRate) / 0.5) == Approx(-9.0).margin(0.05));
    mixer.setDucked(false);

    // The mute: silent within 10 ms, and back.
    mixer.setMuted(true);
    CHECK(mixer.muted());
    const std::vector<float> muted = renderBlocks(mixer, {&a, &b}, 3, 99 * kBlockSeconds, media);
    CHECK(peakOf(std::span<const float>(muted).subspan(480 * 2)) == 0.0f);
    mixer.setMuted(false);
    renderBlocks(mixer, {&a, &b}, 3, 102 * kBlockSeconds, media);
    CHECK(mixer.outputGain() == 1.0);

    // A pause: the picture stops, the sound fades out within 30 ms and reads nothing more.
    mixer.setControl(second, SourceControl{1.0, 0.0, false});
    mixer.setClock(second, SourceClock{105 * kBlockSeconds, 105 * kBlockSeconds, 0, false});
    const std::vector<float> paused = renderBlocks(mixer, {&a, &b}, 3, 105 * kBlockSeconds, media);
    CHECK(mixer.sourceGain(second) == 0.0);
    CHECK(peakOf(std::span<const float>(paused).subspan(1440 * 2)) == 0.0f);
    CHECK(peakOf(std::span<const float>(paused).subspan(0, 240 * 2)) > 0.1f); // still sounding as the fade begins
}

TEST_CASE("audio/mixer: a source near full scale panned 0.6 either side never reaches full scale", "[audio][mixer]") {
    for (const double pan : {-0.6, 0.6}) {
        INFO("pan " << pan);
        Mixer mixer;
        Feeder feeder;
        feeder.amplitude = 0.98;
        const SourceId source = mixer.addSource();
        mixer.setRing(source, feeder.ring);
        mixer.setControl(source, SourceControl{1.0, pan, true});
        mixer.setClock(source, clockAt(0.0, 0.0));
        const std::vector<float> out = renderBlocks(mixer, {&feeder}, 60, 0.0, [](double t) { return t; });
        const std::span<const float> settled = std::span<const float>(out).subspan(30 * kBlock * 2);
        CHECK(peakOf(settled) < 1.0f);
        CHECK(peakOf(settled) > 0.97f); // the louder side at unity
        const std::vector<float> louder = channelOf(settled, 2, pan > 0.0 ? 1 : 0);
        const std::vector<float> quieter = channelOf(settled, 2, pan > 0.0 ? 0 : 1);
        CHECK(peakOf(louder) > 0.97f);
        CHECK(peakOf(quieter) == Approx(0.98 * std::tan(0.1 * std::numbers::pi)).epsilon(0.01));
    }
}

TEST_CASE("audio/mixer: samples that are not finite numbers come out as silence, never as NaN", "[audio][mixer]") {
    Mixer mixer;
    auto ring = std::make_shared<AudioRing>(kRate);
    std::vector<float> chunk(kBlock * 2 * 30);
    for (std::size_t index = 0; index < chunk.size(); ++index) {
        chunk[index] = index % 7 == 0   ? std::numeric_limits<float>::quiet_NaN()
                       : index % 7 == 3 ? std::numeric_limits<float>::infinity()
                                        : 0.5f;
    }
    ring->write(chunk, 0.0, 0);
    const SourceId source = mixer.addSource();
    mixer.setRing(source, ring);
    mixer.setControl(source, SourceControl{1.0, 0.25, true});
    mixer.setClock(source, clockAt(0.0, 0.0));
    std::vector<float> out(kBlock * 2 * 20);
    for (std::size_t block = 0; block < 20; ++block) {
        mixer.render(out.data() + block * kBlock * 2, kBlock, static_cast<double>(block) * kBlockSeconds);
    }
    CHECK(std::all_of(out.begin(), out.end(), [](float sample) { return std::isfinite(sample); }));
    CHECK(peakOf(out) <= 1.0f);
    CHECK(peakOf(out) > 0.4f);
}

TEST_CASE("audio/mixer: a block longer than its scratch is rendered in pieces, the samples still exact",
          "[audio][mixer]") {
    Mixer mixer;
    Feeder feeder;
    feeder.ring = std::make_shared<AudioRing>(kRate, 1.0);
    feeder.topUp(0.9);
    const SourceId source = mixer.addSource();
    mixer.setRing(source, feeder.ring);
    mixer.setControl(source, SourceControl{1.0, 0.0, true});
    mixer.setClock(source, clockAt(0.0, 0.0));
    constexpr std::size_t kFrames = Mixer::kMaxBlockFrames * 2 + 3616; // 0.42 s
    std::vector<float> out(kFrames * 2);
    mixer.render(out.data(), kFrames, 0.0);
    CHECK(mixer.framesRendered() == kFrames);
    CHECK(mixer.syncSummary().underrunFrames == 0);
    std::size_t differing = 0;
    for (std::size_t frame = 14400; frame < kFrames; ++frame) { // past the 300 ms fade-in
        differing += out[frame * 2] != feeder.sample(frame) || out[frame * 2 + 1] != -feeder.sample(frame) ? 1 : 0;
    }
    CHECK(differing == 0);
}

TEST_CASE("audio/mixer: an empty ring gives silence and counts its frames", "[audio][mixer]") {
    Mixer mixer;
    const SourceId source = mixer.addSource();
    mixer.setRing(source, std::make_shared<AudioRing>(kRate));
    mixer.setControl(source, SourceControl{1.0, 0.0, true});
    mixer.setClock(source, clockAt(0.0, 0.0));
    std::vector<float> block(kBlock * 2, 1.0f);
    mixer.render(block.data(), kBlock, 0.0);
    CHECK(peakOf(block) == 0.0f);
    CHECK(mixer.syncSummary().underrunFrames == kBlock);
    CHECK(mixer.framesRendered() == kBlock);
}

TEST_CASE("audio/mixer: an offline mix is equal bit for bit over two runs", "[audio][mixer]") {
    // Two sources, a pan, a change of the audible one, a duck, a jump of the picture, a picture running fast.
    const auto run = [] {
        Mixer mixer;
        Feeder a;
        a.frequency = 523.25;
        a.amplitude = 0.7;
        Feeder b;
        b.frequency = 659.25;
        b.amplitude = 0.4;
        const SourceId first = mixer.addSource();
        const SourceId second = mixer.addSource();
        mixer.setRing(first, a.ring);
        mixer.setRing(second, b.ring);
        mixer.setMaster(0.8);
        std::vector<float> mix;
        std::vector<float> block(kBlock * 2);
        for (int frame = 0; frame < 600; ++frame) {
            const double at = frame * kBlockSeconds;
            const double media = 1.003 * at + (frame >= 300 ? 0.25 : 0.0);
            const bool firstHeard = frame < 200 || frame >= 400;
            mixer.setControl(first, SourceControl{firstHeard ? spatialGain(2.0 + frame * 0.01, 12.0) : 0.0,
                                                  panForAzimuth(frame * 0.01), true});
            mixer.setControl(second, SourceControl{firstHeard ? 0.0 : 0.9, -0.3, true});
            mixer.setClock(first, clockAt(media, at));
            mixer.setClock(second, clockAt(media, at));
            mixer.setDucked(frame >= 450 && frame < 500);
            a.topUp(media + 0.2);
            b.topUp(media + 0.2);
            mixer.render(block.data(), kBlock, at);
            mix.insert(mix.end(), block.begin(), block.end());
        }
        return mix;
    };
    const std::vector<float> first = run();
    const std::vector<float> second = run();
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)) == 0);
    CHECK(peakOf(first) > 0.1);
    CHECK(peakOf(first) <= 1.0);
}
