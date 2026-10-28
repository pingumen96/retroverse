// The clips' sound decoded on the decoder's thread (I5 step 5, ADR-0016 §9): 96, 48 and 44.1 kHz tones resampled by
// swresample to the mixer's 48 kHz stereo float (frequency within 0.5 Hz, level within 0.1 dB, no clipping), mono
// up-mixed at its level, timestamps on the video's timeline (beeps at their flashes' times, loops, seeks), the sound
// disabled costing no decoding.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "media_test_support.hpp"
#include <rv/audio/analysis.hpp>
#include <rv/audio/spatial.hpp>
#include <rv/media/audio_sink.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_decoder.hpp>

using Catch::Approx;
using rv::media::DecoderSettings;
using rv::media::DecoderState;
using rv::test::collectFrames;
using rv::test::MediaTestDir;
using rv::test::startDecoder;
using rv::test::waitUntil;
using rv::test::writeClip;

namespace {

constexpr int kMixRate = 48000;

/// Keeps every chunk the decoder writes (its thread writes, the test reads after waiting).
class CollectingSink final : public rv::media::AudioSink {
public:
    struct Chunk {
        double pts = 0.0;
        std::uint32_t generation = 0;
        std::vector<float> samples;
    };

    void write(std::span<const float> interleaved, double ptsSeconds, std::uint32_t generation) override {
        const std::scoped_lock lock(m_mutex);
        m_chunks.push_back(Chunk{ptsSeconds, generation, std::vector<float>(interleaved.begin(), interleaved.end())});
    }

    [[nodiscard]] std::vector<Chunk> chunks() const {
        const std::scoped_lock lock(m_mutex);
        return m_chunks;
    }

    [[nodiscard]] double seconds() const {
        const std::scoped_lock lock(m_mutex);
        std::size_t frames = 0;
        for (const Chunk& chunk : m_chunks) {
            frames += chunk.samples.size() / 2;
        }
        return static_cast<double>(frames) / kMixRate;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Chunk> m_chunks;
};

rv::media::TestVideoSpec toneClip(int rate, int channels, double seconds, double frequency = 997.0,
                                  double amplitude = 0.5) {
    rv::media::TestVideoSpec spec = rv::test::smallClip(160, 120, 30, static_cast<int>(seconds * 30));
    spec.audioSeconds = seconds;
    spec.audioSampleRate = rate;
    spec.audioChannels = channels;
    spec.toneHz = frequency;
    spec.toneAmplitude = amplitude;
    return spec;
}

/// Decodes `path` to its end with `sink`, the video frames consumed; returns the decoder (ended).
std::unique_ptr<rv::media::VideoDecoder> decodeAll(const std::filesystem::path& path,
                                                   std::shared_ptr<CollectingSink> sink, int videoFrames,
                                                   bool loop = false) {
    DecoderSettings settings;
    settings.loop = loop;
    settings.audioSink = std::move(sink);
    settings.audioSampleRate = kMixRate;
    auto decoder = startDecoder(path, settings);
    static_cast<void>(collectFrames(*decoder, static_cast<std::size_t>(videoFrames)));
    if (!loop) {
        REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Ended; }));
    }
    return decoder;
}

/// The chunks joined, checked to follow one another without a gap (each within 1 ms of the previous one's end).
std::vector<float> joined(const std::vector<CollectingSink::Chunk>& chunks) {
    std::vector<float> samples;
    double expected = chunks.empty() ? 0.0 : chunks.front().pts;
    for (const CollectingSink::Chunk& chunk : chunks) {
        CHECK(chunk.pts == Approx(expected).margin(0.001));
        samples.insert(samples.end(), chunk.samples.begin(), chunk.samples.end());
        expected = chunk.pts + static_cast<double>(chunk.samples.size() / 2) / kMixRate;
    }
    return samples;
}

} // namespace

TEST_CASE("media/audio: 96, 48 and 44.1 kHz tones resampled to 48 kHz stereo: frequency, level, no clipping",
          "[media][audio]") {
    const MediaTestDir dir;
    for (const int rate : {96000, 48000, 44100}) {
        for (const int channels : {1, 2}) {
            INFO(rate << " Hz, " << channels << " channel(s)");
            const auto path = writeClip(dir, "tone_" + std::to_string(rate) + "_" + std::to_string(channels) + ".mkv",
                                        toneClip(rate, channels, 3.0));
            auto sink = std::make_shared<CollectingSink>();
            auto decoder = decodeAll(path, sink, 90);
            const auto info = decoder->streamInfo();
            REQUIRE(info);
            CHECK(info->audioCodec == "pcm_s16le");
            CHECK(info->audioSampleRate == rate);
            CHECK(info->audioChannels == channels);
            CHECK(info->audioDecoded);
            const auto chunks = sink->chunks();
            REQUIRE_FALSE(chunks.empty());
            CHECK(chunks.front().pts == Approx(0.0).margin(0.001)); // on the video's timeline
            const std::vector<float> samples = joined(chunks);
            CHECK(samples.size() / 2 == Approx(3.0 * kMixRate).margin(0.01 * kMixRate));
            const std::vector<float> left = rv::audio::channelOf(samples, 2, 0);
            const std::vector<float> right = rv::audio::channelOf(samples, 2, 1);
            CHECK(left == right); // mono up-mixed to both channels; the tone in both of a stereo clip
            // The steady middle: 0.5 s to 2.5 s.
            const std::span<const float> middle(left.data() + kMixRate / 2, static_cast<std::size_t>(2 * kMixRate));
            CHECK(rv::audio::toneFrequency(middle, kMixRate) == Approx(997.0).margin(0.5));
            const double level = rv::audio::rmsOf(middle) * std::sqrt(2.0);
            CHECK(rv::audio::decibelsFromGain(level / 0.5) == Approx(0.0).margin(0.1));
            CHECK(rv::audio::peakOf(left) < 1.0);
            const auto stats = decoder->stats();
            CHECK(stats.audioFramesWritten == samples.size() / 2);
            CHECK(stats.audioDecodeErrors == 0);
        }
    }
    // A tone near full scale: resampled without clipping.
    for (const int rate : {96000, 44100}) {
        INFO(rate << " Hz near full scale");
        const auto path = writeClip(dir, "loud_" + std::to_string(rate) + ".mkv", toneClip(rate, 1, 2.0, 1000.0, 0.98));
        auto sink = std::make_shared<CollectingSink>();
        auto decoder = decodeAll(path, sink, 60);
        const std::vector<float> samples = joined(sink->chunks());
        const double peak = rv::audio::peakOf(samples);
        CHECK(peak < 1.0);
        CHECK(peak > 0.97);
        const std::vector<float> left = rv::audio::channelOf(samples, 2, 0);
        const std::span<const float> middle(left.data() + kMixRate / 2, static_cast<std::size_t>(kMixRate));
        CHECK(rv::audio::decibelsFromGain(rv::audio::rmsOf(middle) * std::sqrt(2.0) / 0.98) ==
              Approx(0.0).margin(0.1));
    }
}

TEST_CASE("media/audio: the beeps' sound at their flashes' times, on the video's timeline", "[media][audio]") {
    const MediaTestDir dir;
    rv::media::TestVideoSpec spec = toneClip(44100, 1, 4.0, 1000.0, 0.5);
    spec.frameRate = 60;
    spec.beepPeriodSeconds = 1.0;
    spec.beepFirstSeconds = 0.5;
    spec.beepSeconds = 0.1;
    const auto path = writeClip(dir, "beeps.mkv", spec);
    auto sink = std::make_shared<CollectingSink>();
    DecoderSettings settings;
    settings.loop = false;
    settings.audioSink = sink;
    auto decoder = startDecoder(path, settings);
    // The flashed frames, by their timestamps.
    std::vector<double> flashes;
    for (const rv::test::DecodedFrame& frame : collectFrames(*decoder, 240)) {
        const auto time = frame.timing.ptsSeconds;
        const bool inBeep = std::fmod(time - 0.5 + 1e-6, 1.0) < 0.1 && time >= 0.5 - 1e-6;
        if (inBeep && (flashes.empty() || time - flashes.back() > 0.5)) {
            flashes.push_back(time);
        }
    }
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Ended; }));
    const auto chunks = sink->chunks();
    REQUIRE_FALSE(chunks.empty());
    const std::vector<float> samples = joined(chunks);
    const std::vector<double> found = rv::audio::onsets(rv::audio::channelOf(samples, 2, 0), kMixRate, 0.05, 0.3);
    const std::vector<double> expected = rv::media::testBeepTimes(spec);
    REQUIRE(expected.size() == 4);
    REQUIRE(found.size() == expected.size());
    REQUIRE(flashes.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        INFO("beep " << index);
        const double heard = chunks.front().pts + found[index];
        CHECK(heard == Approx(expected[index]).margin(0.001));
        CHECK(flashes[index] == Approx(expected[index]).margin(0.001));
    }
}

TEST_CASE("media/audio: disabled, the sound is discarded undecoded; enabled, it follows from there", "[media][audio]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "tone.mkv", toneClip(48000, 2, 3.0));
    auto sink = std::make_shared<CollectingSink>();
    DecoderSettings settings;
    settings.loop = false;
    settings.audioSink = sink;
    settings.audioEnabled = false;
    auto decoder = startDecoder(path, settings);
    CHECK_FALSE(decoder->audioEnabled());
    const auto first = collectFrames(*decoder, 30); // the first second
    REQUIRE(first.size() == 30);
    CHECK(sink->chunks().empty());
    CHECK(decoder->stats().audioPacketsDiscarded > 0);
    decoder->setAudioEnabled(true);
    static_cast<void>(collectFrames(*decoder, 60));
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Ended; }));
    const auto chunks = sink->chunks();
    REQUIRE_FALSE(chunks.empty());
    // From where the demuxer stood when it was enabled: past the first second's frames, which the queue ran ahead of.
    CHECK(chunks.front().pts > 0.9);
    CHECK(chunks.front().pts < 1.5);
    const std::vector<float> samples = joined(chunks);
    CHECK(chunks.front().pts + static_cast<double>(samples.size() / 2) / kMixRate == Approx(3.0).margin(0.01));
}

TEST_CASE("media/audio: loops keep the timestamps growing, a seek starts its generation at the target",
          "[media][audio]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "short.mkv", toneClip(48000, 1, 1.0));
    auto sink = std::make_shared<CollectingSink>();
    DecoderSettings settings;
    settings.loop = true;
    settings.audioSink = sink;
    auto decoder = startDecoder(path, settings);
    static_cast<void>(collectFrames(*decoder, 70)); // into the third pass
    REQUIRE(waitUntil([&] { return sink->seconds() > 2.0; }));
    const auto looped = sink->chunks();
    CHECK(looped.back().pts > 1.5); // the second pass's sound after the first's
    double previousEnd = 0.0;
    for (const auto& chunk : looped) {
        CHECK(chunk.pts >= previousEnd - 0.001);
        previousEnd = chunk.pts + static_cast<double>(chunk.samples.size() / 2) / kMixRate;
    }
    const std::uint32_t generation = decoder->seek(0.4);
    static_cast<void>(collectFrames(*decoder, 10));
    REQUIRE(waitUntil([&] {
        const auto chunks = sink->chunks();
        return std::any_of(chunks.begin(), chunks.end(),
                           [&](const CollectingSink::Chunk& chunk) { return chunk.generation == generation; });
    }));
    const auto chunks = sink->chunks();
    const auto seeked = std::find_if(chunks.begin(), chunks.end(), [&](const CollectingSink::Chunk& chunk) {
        return chunk.generation == generation;
    });
    REQUIRE(seeked != chunks.end());
    // The first chunk of the seek covers the target (the frame's sound in which it falls), never before it.
    CHECK(seeked->pts <= 0.4 + 0.001);
    CHECK(seeked->pts + static_cast<double>(seeked->samples.size() / 2) / kMixRate > 0.4);
    decoder->stop();
}

TEST_CASE("media/audio: a sound longer or shorter than its picture loops with it, without a step back or a lag",
          "[media][audio]") {
    const MediaTestDir dir;
    struct Row {
        const char* file;
        double audioSeconds;
    };
    // The picture: 30 frames at 30 fps, 1 s a pass.
    for (const Row& row : {Row{"longer.mkv", 1.5}, Row{"shorter.mkv", 0.6}, Row{"longer.avi", 1.5}}) {
        INFO(row.file << ": " << row.audioSeconds << " s of sound for 1 s of picture");
        rv::media::TestVideoSpec spec = toneClip(44100, 1, 1.0);
        spec.audioSeconds = row.audioSeconds;
        const auto path = writeClip(dir, row.file, spec);
        auto sink = std::make_shared<CollectingSink>();
        DecoderSettings settings;
        settings.loop = true;
        settings.audioSink = sink;
        settings.audioSampleRate = kMixRate;
        auto decoder = startDecoder(path, settings);
        constexpr int kPasses = 4;
        static_cast<void>(collectFrames(*decoder, 30 * kPasses + 10)); // into the fifth pass
        const double perPass = std::min(row.audioSeconds, 1.0);
        REQUIRE(waitUntil([&] { return sink->seconds() > perPass * kPasses; }));
        decoder->stop();
        const auto chunks = sink->chunks();
        // Each pass's sound starts at its picture's start and ends at the earlier of its own end and the picture's.
        std::vector<double> starts(kPasses + 1, -1.0);
        std::vector<double> ends(kPasses + 1, -1.0);
        double previousEnd = -1.0;
        for (const CollectingSink::Chunk& chunk : chunks) {
            const double end = chunk.pts + static_cast<double>(chunk.samples.size() / 2) / kMixRate;
            CHECK(chunk.pts >= previousEnd - 0.001); // never a step back (beyond the resampler's sub-sample rounding)
            previousEnd = end;
            const auto pass = static_cast<std::size_t>(std::floor(chunk.pts + 1e-6));
            if (pass > kPasses) {
                continue;
            }
            if (starts[pass] < 0.0) {
                starts[pass] = chunk.pts;
            }
            ends[pass] = end;
        }
        for (std::size_t pass = 0; pass < kPasses; ++pass) {
            INFO("pass " << pass);
            CHECK(starts[pass] == Approx(static_cast<double>(pass)).margin(0.001));
            CHECK(ends[pass] == Approx(static_cast<double>(pass) + perPass).margin(0.001));
            CHECK(ends[pass] <= static_cast<double>(pass) + 1.0 + 1e-6);
        }
        CHECK(decoder->stats().audioDecodeErrors == 0);
    }
}
