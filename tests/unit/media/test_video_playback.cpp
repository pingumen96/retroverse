#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

#include "media_test_support.hpp"
#include <rv/media/test_video.hpp>
#include <rv/media/video_playback.hpp>

using rv::media::DecoderSettings;
using rv::media::DecoderState;
using rv::media::VideoPlayback;
using rv::test::MediaTestDir;
using rv::test::waitUntil;

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
    for (int refresh = 0; refresh < 200 && counters.size() < 10; ++refresh, now += 1.0 / 60.0) {
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
    REQUIRE(waitUntil([&] { return playback.failed(); }));
    const auto update = playback.update(0.0);
    CHECK(update.frame == nullptr);
    CHECK_FALSE(update.clock.repeated);
    playback.pause(0.0);
    CHECK(playback.clock().paused());
    playback.resume(1.0);
    CHECK_FALSE(playback.clock().paused());
}
