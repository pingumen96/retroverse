// The check of a clip's sample (I5 step 7, ADR-0016 §8: what `retroverse-cli media --check-videos` runs on each clip):
// every damaged kind of SC-08 gets the decoder's verdict, a short clip is decoded whole, a long one only at its head,
// middle and end, a long clip cut short whose index is intact fails as truncated without being decoded whole (cut
// before or after its middle), and a file the system cannot read is no verdict.

#include "media_test_support.hpp"

#include <rv/core/utf8.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_check.hpp>
#include <rv/media/video_verdicts.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>

using Catch::Approx;
using rv::media::DecodeFailure;
using rv::media::VideoCheckResult;
using rv::test::MediaTestDir;
using rv::test::smallClip;
using rv::test::writeClip;

namespace {

VideoCheckResult checked(const std::filesystem::path& path) {
    const VideoCheckResult result = rv::media::checkVideo(path);
    INFO(rv::core::pathToUtf8Lossy(path) << ": " << result.detail);
    CHECK(result.completed);
    return result;
}

} // namespace

TEST_CASE("media/video_check: each damaged kind gets its verdict", "[media][video_check]") {
    const MediaTestDir dir;
    const auto clean = writeClip(dir, "clean.mp4", smallClip(160, 120, 10, 30));
    auto large = smallClip(320, 240, 10, 40);
    large.quantizer = 2; // larger frames: the cut and the damage fall well inside the stream
    const auto source = writeClip(dir, "source.mp4", large);
    const auto avi = writeClip(dir, "source.avi", smallClip(160, 120, 10, 10));

    REQUIRE(rv::media::writeRawFile(dir.file("empty.mp4"), {}));
    REQUIRE(rv::media::writeRawFile(dir.file("text.mp4"), "Not a clip: a few words in a file named like one.\n"));
    REQUIRE(rv::media::writeSilentWav(dir.file("sound.mp4"), 0.5)); // a sound file in a clip's name
    REQUIRE(rv::media::writeUnknownCodecCopy(avi, dir.file("unknown.avi")));
    REQUIRE(rv::media::writeTruncatedCopy(source, dir.file("truncated.mp4"), 0.5));
    REQUIRE(rv::media::writeCorruptedCopy(source, dir.file("corrupt.mp4"), 7));

    const VideoCheckResult playable = checked(clean);
    CHECK(playable.failure == DecodeFailure::None);
    CHECK(playable.whole); // 3 s: decoded whole
    CHECK(playable.frames == 30);
    REQUIRE(playable.info);
    CHECK(playable.info->codec == "mpeg4");
    CHECK(playable.stats.decodeErrors == 0);

    CHECK(checked(dir.file("empty.mp4")).failure == DecodeFailure::InvalidFile);
    CHECK(checked(dir.file("text.mp4")).failure == DecodeFailure::InvalidFile);
    const VideoCheckResult sound = checked(dir.file("sound.mp4"));
    CHECK(sound.failure == DecodeFailure::NoVideoStream);
    CHECK(checked(dir.file("unknown.avi")).failure == DecodeFailure::UnsupportedCodec);
    const VideoCheckResult truncated = checked(dir.file("truncated.mp4"));
    CHECK(truncated.failure == DecodeFailure::Truncated);
    CHECK(truncated.frames > 0); // the frames before the cut were read
    const VideoCheckResult corrupt = checked(dir.file("corrupt.mp4"));
    CHECK(corrupt.failure == DecodeFailure::CorruptData);
    CHECK(corrupt.stats.decodeErrors + corrupt.stats.damagedFrames >= 3);

    // A file that is not there is no verdict of a clip: the decoder's FileNotFound.
    CHECK(checked(dir.file("missing.mp4")).failure == DecodeFailure::FileNotFound);
}

TEST_CASE("media/video_check: a long clip is checked at its head, middle and end", "[media][video_check]") {
    const MediaTestDir dir;
    const auto clip = writeClip(dir, "long.mp4", smallClip(160, 120, 10, 200)); // 20 s
    const VideoCheckResult result = checked(clip);
    CHECK(result.failure == DecodeFailure::None);
    CHECK_FALSE(result.whole);
    // About 2 s of the head, a second of the middle and the last second: far fewer than the clip's 200 frames.
    CHECK(result.frames >= 30);
    CHECK(result.frames < 60);
    CHECK(result.lastFrameSeconds == Approx(19.9).margin(0.05));
    CHECK(result.stats.seeks == 2);
}

TEST_CASE("media/video_check: a long clip cut short fails as truncated without being decoded whole",
          "[media][video_check]") {
    const MediaTestDir dir;
    auto spec = smallClip(320, 240, 10, 200); // 20 s
    spec.quantizer = 2;
    const auto clip = writeClip(dir, "long.mp4", spec);
    // The index first (faststart), the last 30% of the frames' bytes gone.
    REQUIRE(rv::media::writeTruncatedCopy(clip, dir.file("cut.mp4"), 0.7));
    const VideoCheckResult result = checked(dir.file("cut.mp4"));
    CHECK(result.failure == DecodeFailure::Truncated);
    CHECK_FALSE(result.whole);
    CHECK(result.frames < 100);
}

TEST_CASE("media/video_check: a long clip cut before its middle fails as truncated", "[media][video_check]") {
    const MediaTestDir dir;
    auto spec = smallClip(320, 240, 10, 200); // 20 s
    spec.quantizer = 2;
    const auto clip = writeClip(dir, "long.mp4", spec);
    // The index first (faststart), 70% of the frames' bytes gone: the stream ends near 6 s, before the middle's seek
    // (9.5 s), which then finds nothing to read.
    REQUIRE(rv::media::writeTruncatedCopy(clip, dir.file("cut.mp4"), 0.3));
    const VideoCheckResult result = checked(dir.file("cut.mp4"));
    CHECK(result.failure == DecodeFailure::Truncated);
    CHECK_FALSE(result.whole);
    CHECK(result.lastFrameSeconds < 9.0);
    CHECK(result.frames < 60);
}

TEST_CASE("media/video_check: a file the system cannot read gives a read error, never a verdict of the clip",
          "[media][video_check]") {
    const MediaTestDir dir;
    const auto clip = writeClip(dir, "held.mp4", smallClip(160, 120, 10, 30));
    // Another program holds it open and shares nothing: the open fails with a sharing violation.
    const HANDLE held =
        CreateFileW(clip.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    const VideoCheckResult result = rv::media::checkVideo(clip);
    CloseHandle(held);
    INFO(result.detail);
    CHECK(result.completed);
    CHECK(result.failure == DecodeFailure::ReadError);
    rv::media::VideoVerdict verdict;
    verdict.path = "snes/videos/held.mp4";
    verdict.failure = result.failure;
    CHECK_FALSE(rv::media::VideoVerdicts{}.set(verdict));
    // Released, the same file checks playable.
    CHECK(checked(clip).failure == DecodeFailure::None);
}

TEST_CASE("media/video_check: a check cancelled or out of time gives no verdict", "[media][video_check]") {
    const MediaTestDir dir;
    const auto clip = writeClip(dir, "long.mp4", smallClip(160, 120, 10, 200));
    const std::atomic<bool> cancel{true};
    rv::media::VideoCheckSettings settings;
    settings.cancel = &cancel;
    const VideoCheckResult cancelled = rv::media::checkVideo(clip, settings);
    CHECK_FALSE(cancelled.completed);
    CHECK(cancelled.detail == "cancelled");

    settings.cancel = nullptr;
    settings.timeout = std::chrono::milliseconds(0);
    const VideoCheckResult late = rv::media::checkVideo(clip, settings);
    CHECK_FALSE(late.completed);
}
