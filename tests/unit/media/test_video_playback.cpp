#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "media_test_support.hpp"
#include <rv/media/test_video.hpp>
#include <rv/media/video_playback.hpp>

using rv::media::DecodeFailure;
using rv::media::DecoderSettings;
using rv::media::DecoderState;
using rv::media::VideoPlayback;
using rv::test::MediaTestDir;
using rv::test::waitUntil;

namespace {

constexpr double kRefresh = 1.0 / 60.0;

/// A frame the playback handed over for upload.
struct Uploaded {
    std::uint32_t generation = 0;
    std::optional<std::uint32_t> counter;
};

/// Renders refreshes at 60 Hz on the synthetic clock `now`, waiting for the decoder whenever its queue is empty (so
/// that the result does not depend on the machine's speed), until a frame is handed over for upload; "uploads" it.
/// Nothing within `maxRefreshes`.
std::optional<Uploaded> nextUpload(VideoPlayback& playback, double& now, int maxRefreshes = 600) {
    for (int refresh = 0; refresh < maxRefreshes; ++refresh, now += kRefresh) {
        if (!waitUntil([&] { return !playback.decoder().frames().empty(); })) {
            return std::nullopt;
        }
        const auto update = playback.update(now);
        if (update.frame != nullptr) {
            Uploaded uploaded{update.frame->timing.generation, rv::media::decodeFrameCounter(*update.frame)};
            playback.releaseFrame();
            now += kRefresh;
            return uploaded;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("media/video_playback: frames are shown by timestamp and go back to the pool once uploaded",
          "[media][video_playback]") {
    const MediaTestDir dir;
    const auto path = rv::test::writeClip(dir, "clip.mp4", rv::test::smallClip(160, 120, 10, 10));
    DecoderSettings settings;
    settings.loop = false;
    VideoPlayback playback(rv::test::startDecoder(path, settings));

    // A render loop at 60 Hz on a synthetic clock, which waits for the decoder whenever its queue is empty so that the
    // result does not depend on the machine's speed.
    std::vector<std::uint32_t> counters;
    double now = 0.0;
    for (int refresh = 0; refresh < 200 && counters.size() < 10; ++refresh, now += kRefresh) {
        REQUIRE(waitUntil(
            [&] { return !playback.decoder().frames().empty() || playback.decoder().state() == DecoderState::Ended; }));
        const auto update = playback.update(now);
        if (update.frame != nullptr) {
            REQUIRE(update.clock.newFrame);
            const std::optional<std::uint32_t> counter = rv::media::decodeFrameCounter(*update.frame);
            REQUIRE(counter);
            counters.push_back(*counter);
            playback.releaseFrame(); // "uploaded"
        }
    }
    CHECK(counters == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    const auto& stats = playback.clock().stats();
    CHECK(stats.presented == 10);
    CHECK(stats.dropped == 0);
    CHECK(stats.late == 0);
    CHECK(playback.supersededFrames() == 0);
    CHECK_FALSE(playback.failed());
    // Every frame went back: the pool is whole again once the decoder has ended.
    REQUIRE(waitUntil([&] { return playback.decoder().state() == DecoderState::Ended; }));
}

TEST_CASE("media/video_playback: a frame not uploaded is replaced by the next one", "[media][video_playback]") {
    const MediaTestDir dir;
    const auto path = rv::test::writeClip(dir, "clip.mp4", rv::test::smallClip(160, 120, 10, 10));
    DecoderSettings settings;
    settings.loop = false;
    VideoPlayback playback(rv::test::startDecoder(path, settings));
    REQUIRE(waitUntil([&] { return playback.decoder().frames().size() >= 2; }));
    const auto first = playback.update(0.0);
    REQUIRE(first.frame != nullptr);
    const auto second = playback.update(0.1); // the upload of the first frame was skipped: it is never released
    REQUIRE(second.frame != nullptr);
    CHECK(rv::media::decodeFrameCounter(*second.frame) == 1u);
    CHECK(playback.supersededFrames() == 1);
    playback.releaseFrame();
    CHECK(playback.update(0.1).frame == nullptr); // nothing new to upload
}

TEST_CASE("media/video_playback: a decoder that fails asks for the fallback picture", "[media][video_playback]") {
    const MediaTestDir dir;
    VideoPlayback playback(rv::test::startDecoder(dir.file("missing.mp4")));
    double now = 0.0;
    REQUIRE(waitUntil([&] {
        now += kRefresh;
        return playback.update(now).frame == nullptr && playback.failed();
    }));
    const auto update = playback.update(now);
    CHECK(update.frame == nullptr);
    CHECK_FALSE(update.clock.repeated);
    playback.pause(now);
    CHECK(playback.clock().paused());
    playback.resume(now + 1.0);
    CHECK_FALSE(playback.clock().paused());
    CHECK(playback.failed());
}

TEST_CASE("media/video_playback: the frames decoded before a failure are shown before the fallback",
          "[media][video_playback]") {
    const MediaTestDir dir;
    auto spec = rv::test::smallClip(320, 240, 10, 40);
    spec.quantizer = 2;
    const auto clip = rv::test::writeClip(dir, "long.mp4", spec);
    REQUIRE(rv::media::writeTruncatedCopy(clip, dir.file("truncated.mp4"), 0.5));
    DecoderSettings settings;
    settings.loop = false;
    // The whole cut clip fits in the queue: the decoder fails with every frame it decoded still queued.
    settings.queueCapacity = DecoderSettings::kMaxQueueCapacity;
    VideoPlayback playback(rv::test::startDecoder(dir.file("truncated.mp4"), settings));
    REQUIRE(waitUntil([&] { return playback.decoder().state() == DecoderState::Failed; }));
    CHECK(playback.decoder().failure() == DecodeFailure::Truncated);
    const std::size_t queued = playback.decoder().frames().size();
    REQUIRE(queued > 5);
    CHECK(queued == playback.decoder().stats().framesQueued);

    std::vector<std::uint32_t> counters;
    double now = 0.0;
    double lastShownEnd = 0.0;
    std::optional<double> mediaAtFallback;
    for (int refresh = 0; refresh < 3000 && !mediaAtFallback; ++refresh, now += kRefresh) {
        const auto update = playback.update(now);
        if (update.frame != nullptr) {
            const std::optional<std::uint32_t> counter = rv::media::decodeFrameCounter(*update.frame);
            REQUIRE(counter);
            counters.push_back(*counter);
            lastShownEnd = update.frame->timing.ptsSeconds + update.frame->timing.durationSeconds;
            playback.releaseFrame();
        }
        if (playback.failed()) {
            mediaAtFallback = update.clock.mediaTime.value_or(-1.0);
        }
    }
    // Every frame queued before the failure was shown, in order, before the fallback, and the last one had its time.
    // (Frames just before the cut may be missing: B-frames whose packets come after it in decoding order.)
    REQUIRE(mediaAtFallback);
    CHECK(counters.size() == queued);
    REQUIRE_FALSE(counters.empty());
    CHECK(counters.front() == 0u);
    for (std::size_t index = 1; index < counters.size(); ++index) {
        CHECK(counters[index] > counters[index - 1]);
    }
    CHECK(*mediaAtFallback >= lastShownEnd - 1e-9);
}

TEST_CASE("media/video_playback: seeks while playing or paused never show a frame of the old position",
          "[media][video_playback]") {
    const MediaTestDir dir;
    auto spec = rv::test::smallClip(160, 120, 10, 40);
    spec.keyFrameInterval = 10;
    const auto path = rv::test::writeClip(dir, "seek.mp4", spec);
    VideoPlayback playback(rv::test::startDecoder(path)); // loops
    double now = 0.0;
    REQUIRE(nextUpload(playback, now));

    const std::array<double, 6> targets{2.05, 0.55, 3.35, 1.05, 0.0, 2.75};
    for (std::size_t round = 0; round < targets.size(); ++round) {
        INFO("seek to " << targets[round] << (round % 2 == 1 ? " while paused" : ""));
        const bool paused = round % 2 == 1;
        if (paused) {
            playback.pause(now);
        }
        playback.seek(targets[round]);
        const auto generation = static_cast<std::uint32_t>(round + 1);
        CHECK(playback.decoder().generation() == generation);
        // The first frame handed over after the seek is the one at the target: frame floor(target * 10).
        const auto first = nextUpload(playback, now);
        REQUIRE(first);
        CHECK(first->generation == generation);
        CHECK(first->counter == static_cast<std::uint32_t>(std::floor(targets[round] * 10.0)));
        if (paused) {
            // It is the paused picture: nothing more comes until resume().
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            CHECK(playback.decoder().frames().empty());
            CHECK(playback.update(now + 1.0).frame == nullptr);
            playback.resume(now);
        }
        // Playback goes on from there.
        const auto second = nextUpload(playback, now);
        REQUIRE(second);
        CHECK(second->generation == generation);
        CHECK(second->counter == (*first->counter + 1) % 40);
    }
    CHECK(playback.clock().stats().late == 0);
}

TEST_CASE("media/video_playback: seeking while the render loop updates shows only frames of the latest seek",
          "[media][video_playback]") {
    const MediaTestDir dir;
    auto spec = rv::test::smallClip(160, 120, 30, 60);
    spec.keyFrameInterval = 15;
    const auto path = rv::test::writeClip(dir, "stress.mp4", spec);
    VideoPlayback playback(rv::test::startDecoder(path)); // loops
    // The render loop updates without ever waiting for the decoder, its clock running ahead of the decoder, so that the
    // queue drains and fills while it reads it; each seek comes after a few frames of the previous one, while the
    // decoder is mid-frame or waiting for room. Targets are deterministic.
    double now = 0.0;
    std::uint32_t generation = 0;
    for (int seek = 0; seek < 40; ++seek) {
        INFO("seek " << seek);
        playback.seek(static_cast<double>((seek * 7) % 20) / 10.0);
        ++generation;
        int framesOfThisSeek = 0;
        const auto deadline = std::chrono::steady_clock::now() + rv::test::kDecoderTimeout;
        while (framesOfThisSeek < 3 && std::chrono::steady_clock::now() < deadline) {
            now += 0.004;
            const auto update = playback.update(now);
            if (update.frame != nullptr) {
                ++framesOfThisSeek;
                REQUIRE(update.frame->timing.generation == generation);
                playback.releaseFrame();
            }
            if (update.clock.shown) {
                REQUIRE(update.clock.shown->generation == generation);
            }
            std::this_thread::yield();
        }
        REQUIRE(framesOfThisSeek == 3);
    }
    CHECK(playback.decoder().failure() == DecodeFailure::None);
    CHECK(playback.decoder().stats().seeks >= 1);
}
