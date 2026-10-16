#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

#include "media_test_support.hpp"
#include <rv/core/file_io.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_decoder.hpp>

using Catch::Approx;
using rv::media::FrameSize;
using rv::media::TestVideoCodec;
using rv::media::TestVideoSpec;
using rv::test::MediaTestDir;
using rv::test::smallClip;

TEST_CASE("media/test_video: clips have the size, rate and length asked for", "[media][test_video]") {
    const MediaTestDir dir;
    TestVideoSpec spec = smallClip(256, 144, 25, 50);
    const auto path = rv::test::writeClip(dir, "clip.mp4", spec);
    const auto info = rv::media::probeVideo(path);
    REQUIRE(info.has_value());
    CHECK(info->codec == "mpeg4");
    CHECK(info->codedSize == FrameSize{256, 144});
    CHECK(info->frameRate == Approx(25.0));
    CHECK(info->durationSeconds == Approx(2.0).margin(0.1));

    spec.codec = TestVideoCodec::Mjpeg;
    const auto mjpeg = rv::test::writeClip(dir, "clip.mkv", spec);
    const auto mjpegInfo = rv::media::probeVideo(mjpeg);
    REQUIRE(mjpegInfo.has_value());
    CHECK(mjpegInfo->codec == "mjpeg");
    auto decoder = rv::test::startDecoder(mjpeg);
    const auto frames = rv::test::collectFrames(*decoder, 3);
    REQUIRE(frames.size() == 3);
    CHECK(frames[2].counter == 2u); // full-range JPEG colors read back as well
}

TEST_CASE("media/test_video: specs and containers out of range are refused", "[media][test_video]") {
    const MediaTestDir dir;
    TestVideoSpec spec = smallClip();
    spec.width = 161;
    CHECK(rv::media::writeTestVideo(dir.file("odd.mp4"), spec).error().code == rv::ErrorCode::InvalidArgument);
    spec = smallClip();
    spec.quantizer = 1;
    CHECK_FALSE(rv::media::writeTestVideo(dir.file("q.mp4"), spec).has_value());
    spec = smallClip();
    spec.frameRate = 0;
    CHECK_FALSE(rv::media::writeTestVideo(dir.file("rate.mp4"), spec).has_value());
    spec = smallClip();
    CHECK_FALSE(rv::media::writeTestVideo(dir.file("clip.webm"), spec).has_value());
    spec.codec = TestVideoCodec::Mjpeg;
    CHECK(rv::media::writeTestVideo(dir.file("mjpeg.mp4"), spec).error().code == rv::ErrorCode::Unsupported);
    CHECK_FALSE(std::filesystem::exists(dir.file("mjpeg.mp4")));
}

TEST_CASE("media/test_video: damaged copies keep or cut the bytes as asked", "[media][test_video]") {
    const MediaTestDir dir;
    const auto clip = rv::test::writeClip(dir, "clip.mp4", smallClip(160, 120, 10, 20));
    const auto size = std::filesystem::file_size(clip);

    REQUIRE(rv::media::writeTruncatedCopy(clip, dir.file("half.mp4"), 0.5));
    CHECK(std::filesystem::file_size(dir.file("half.mp4")) == size / 2);

    REQUIRE(rv::media::writeCorruptedCopy(clip, dir.file("corrupt.mp4"), 3));
    const auto original = rv::core::readBinaryFile(clip);
    const auto corrupt = rv::core::readBinaryFile(dir.file("corrupt.mp4"));
    REQUIRE(original);
    REQUIRE(corrupt);
    CHECK(corrupt->size() == original->size());
    CHECK(*corrupt != *original);
    // The first 40% are untouched (the header and the first frames).
    CHECK(
        std::equal(original->begin(), original->begin() + static_cast<std::ptrdiff_t>(size * 2 / 5), corrupt->begin()));

    // Only AVI files name their codec with the codes the copy replaces.
    CHECK(rv::media::writeUnknownCodecCopy(clip, dir.file("unknown.mp4")).error().code == rv::ErrorCode::ParseError);

    REQUIRE(rv::media::writeSilentWav(dir.file("silence.wav"), 1.0));
    CHECK(std::filesystem::file_size(dir.file("silence.wav")) == 44u + 32000u);
    CHECK_FALSE(rv::media::writeSilentWav(dir.file("long.wav"), 61.0).has_value());

    REQUIRE(rv::media::writeRawFile(dir.file("empty.mp4"), {}));
    CHECK(std::filesystem::file_size(dir.file("empty.mp4")) == 0u);
}

TEST_CASE("media/test_video: the FFmpeg build has the native encoders the fixtures use", "[media][test_video]") {
    const auto encoders = rv::media::availableVideoEncoders();
    CHECK(std::is_sorted(encoders.begin(), encoders.end()));
    CHECK(std::find(encoders.begin(), encoders.end(), "mpeg4") != encoders.end());
    CHECK(std::find(encoders.begin(), encoders.end(), "mjpeg") != encoders.end());
    CHECK(std::find(encoders.begin(), encoders.end(), "libx264") == encoders.end()); // LGPL build
    CHECK(rv::media::toString(TestVideoCodec::Mpeg4) == "mpeg4");
}

TEST_CASE("media/test_video: frames that are not test frames have no counter", "[media][test_video]") {
    rv::media::VideoFrame frame;
    CHECK_FALSE(rv::media::decodeFrameCounter(frame));
    REQUIRE(frame.resize(FrameSize{200, 100}));
    std::fill_n(frame.pixels(), frame.sizeBytes(), std::uint8_t{128}); // mid grey: every block ambiguous
    CHECK_FALSE(rv::media::decodeFrameCounter(frame));
    std::fill_n(frame.pixels(), frame.sizeBytes(), std::uint8_t{0}); // black: counter 0
    CHECK(rv::media::decodeFrameCounter(frame) == 0u);
}
