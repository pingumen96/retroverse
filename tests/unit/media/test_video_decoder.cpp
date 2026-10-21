#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "media_test_support.hpp"
#include <rv/core/text.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_decoder.hpp>

using Catch::Approx;
using rv::media::DecodeFailure;
using rv::media::DecoderSettings;
using rv::media::DecoderState;
using rv::media::FrameSize;
using rv::test::collectFrames;
using rv::test::MediaTestDir;
using rv::test::smallClip;
using rv::test::startDecoder;
using rv::test::waitUntil;
using rv::test::writeClip;

namespace {

DecoderSettings noLoop() {
    DecoderSettings settings;
    settings.loop = false;
    return settings;
}

/// Starts a decoder of `path` and waits until it has failed; returns its failure.
DecodeFailure failureOf(const std::filesystem::path& path) {
    auto decoder = startDecoder(path, noLoop());
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Failed; }));
    const auto error = decoder->error();
    REQUIRE(error);
    CHECK(error->code == rv::media::errorCodeFor(decoder->failure()));
    CHECK(error->context == rv::core::pathToUtf8(path));
    decoder->stop();
    CHECK(decoder->state() == DecoderState::Failed); // stop() keeps the failure
    return decoder->failure();
}

} // namespace

TEST_CASE("media/video_decoder: frames arrive in order with their timestamps and the stream is described",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "clip.mp4", smallClip(160, 120, 10, 10));
    auto decoder = startDecoder(path, noLoop());
    const auto frames = collectFrames(*decoder, 10);
    REQUIRE(frames.size() == 10);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        INFO("frame " << index);
        CHECK(frames[index].timing.ptsSeconds == Approx(index * 0.1).margin(0.001));
        CHECK(frames[index].timing.durationSeconds == Approx(0.1).margin(0.001));
        CHECK(frames[index].timing.generation == 0);
        CHECK(frames[index].serial == index);
        CHECK(frames[index].loop == 0);
        CHECK(frames[index].size == FrameSize{160, 120});
        CHECK(frames[index].counter == index); // B-frames reordered, colors converted, rows top to bottom
    }
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Ended; }));
    CHECK(decoder->failure() == DecodeFailure::None);
    CHECK_FALSE(decoder->error());

    const auto info = decoder->streamInfo();
    REQUIRE(info);
    CHECK(info->codec == "mpeg4");
    CHECK(info->container.find("mp4") != std::string::npos);
    CHECK(info->codedSize == FrameSize{160, 120});
    CHECK(info->outputSize == FrameSize{160, 120});
    CHECK(info->frameRate == Approx(10.0));
    CHECK(info->durationSeconds == Approx(1.0).margin(0.15));
    CHECK(info->pixelFormat == "yuv420p");
    CHECK(info->decoderThreads == 1);

    const auto stats = decoder->stats();
    CHECK(stats.framesDecoded == 10);
    CHECK(stats.framesQueued == 10);
    CHECK(stats.decodeErrors == 0);
    CHECK(stats.decodeMilliseconds.count == 10);
    CHECK(stats.convertMilliseconds.count == 10);
    CHECK(stats.frameAllocations <= decoder->settings().queueCapacity + 2);
    CHECK(decoder->threadCpuSeconds() >= 0.0);
}

TEST_CASE("media/video_decoder: looping keeps timestamps growing and reuses the pool's frames",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "loop.mp4", smallClip(160, 120, 10, 10));
    auto decoder = startDecoder(path);
    const auto frames = collectFrames(*decoder, 35);
    REQUIRE(frames.size() == 35);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        INFO("frame " << index);
        CHECK(frames[index].timing.ptsSeconds == Approx(index * 0.1).margin(0.001));
        CHECK(frames[index].counter == index % 10);
        CHECK(frames[index].loop == index / 10);
        CHECK(frames[index].serial == index);
    }
    const auto stats = decoder->stats();
    CHECK(stats.loops >= 3);
    // Steady state: no frame allocation beyond the pool's frames, however many frames were decoded.
    CHECK(stats.frameAllocations <= decoder->settings().queueCapacity + 2);
    CHECK(decoder->state() == DecoderState::Playing);
}

TEST_CASE("media/video_decoder: frames taller than maxHeight are scaled down, never up", "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "320x240.mp4", smallClip(320, 240, 10, 3));
    DecoderSettings smaller = noLoop();
    smaller.maxHeight = 120;
    auto scaled = startDecoder(path, smaller);
    const auto scaledFrames = collectFrames(*scaled, 3);
    REQUIRE(scaledFrames.size() == 3);
    CHECK(scaledFrames[0].size == FrameSize{160, 120});
    CHECK(scaledFrames[2].counter == 2u);
    REQUIRE(scaled->streamInfo());
    CHECK(scaled->streamInfo()->outputSize == FrameSize{160, 120});

    auto unscaled = startDecoder(path, noLoop()); // maxHeight 720
    const auto unscaledFrames = collectFrames(*unscaled, 1);
    REQUIRE(unscaledFrames.size() == 1);
    CHECK(unscaledFrames[0].size == FrameSize{320, 240});
}

TEST_CASE("media/video_decoder: non-ASCII paths and every container of the fixtures decode", "[media][video_decoder]") {
    const MediaTestDir dir;
    for (const char* name : {"vidéo 日本語/clip.mp4", "clip.mkv", "clip.avi"}) {
        INFO(name);
        const auto path = writeClip(dir, name, smallClip(160, 120, 10, 5));
        const auto probed = rv::media::probeVideo(path);
        REQUIRE(probed.has_value());
        CHECK(probed->codedSize == FrameSize{160, 120});
        auto decoder = startDecoder(path, noLoop());
        const auto frames = collectFrames(*decoder, 5);
        REQUIRE(frames.size() == 5);
        for (std::size_t index = 0; index < frames.size(); ++index) {
            CHECK(frames[index].timing.ptsSeconds == Approx(index * 0.1).margin(0.001));
            CHECK(frames[index].counter == index);
        }
    }
}

TEST_CASE("media/video_decoder: a seek discards the queue and continues from the target in a new generation",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    auto spec = smallClip(160, 120, 10, 20);
    spec.keyFrameInterval = 20; // one key frame: the seek decodes from the start and skips what precedes the target
    const auto path = writeClip(dir, "seek.mp4", spec);
    auto decoder = startDecoder(path, noLoop());
    const auto before = collectFrames(*decoder, 2);
    REQUIRE(before.size() == 2);
    CHECK(decoder->generation() == 0);
    CHECK(decoder->seek(1.25) == 1u);
    CHECK(decoder->generation() == 1);
    // seek() discards the frames of the old position at once: the next frame popped is the one showing 1.25 s, frame
    // 12 (1.2 s to 1.3 s).
    const auto first = collectFrames(*decoder, 1);
    REQUIRE(first.size() == 1);
    CHECK(first[0].timing.generation == 1);
    CHECK(first[0].timing.ptsSeconds == Approx(1.2).margin(0.001));
    CHECK(first[0].counter == 12u);
    const auto rest = collectFrames(*decoder, 7);
    REQUIRE(rest.size() == 7);
    CHECK(rest.back().counter == 19u);
    CHECK(rest.back().timing.generation == 1);
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Ended; }));
    CHECK(decoder->stats().seeks == 1);
    CHECK(decoder->stats().framesSkipped >= 1);

    // A seek also restarts an ended stream.
    CHECK(decoder->seek(0.0) == 2u);
    CHECK(decoder->state() == DecoderState::Playing);
    const auto again = collectFrames(*decoder, 1);
    REQUIRE(again.size() == 1);
    CHECK(again[0].counter == 0u);
    CHECK(again[0].timing.generation == 2);
}

TEST_CASE("media/video_decoder: a seek is carried out at once while the decoder waits on a full queue",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    auto spec = smallClip(160, 120, 10, 30);
    spec.keyFrameInterval = 10;
    const auto path = writeClip(dir, "full_seek.mp4", spec);
    auto decoder = startDecoder(path); // loops; nobody pops: the thread waits for room
    REQUIRE(waitUntil([&] { return decoder->frames().size() == decoder->frames().capacity(); }));
    const auto queuedBefore = decoder->stats().framesQueued;
    decoder->seek(2.05);
    // The frames of the old position are gone before seek() returns (frames of the target may follow at once).
    const auto front = decoder->frames().peek(0);
    CHECK((!front || front->generation == 1));
    // The thread wakes without a pop, discards the frame it held and delivers frames from the target (frame 20).
    REQUIRE(waitUntil([&] { return !decoder->frames().empty(); }));
    const auto next = collectFrames(*decoder, 3);
    REQUIRE(next.size() == 3);
    for (std::size_t index = 0; index < next.size(); ++index) {
        INFO("frame " << index);
        CHECK(next[index].timing.generation == 1);
        CHECK(next[index].counter == 20u + index);
    }
    CHECK(decoder->stats().seeks == 1);
    CHECK(decoder->stats().framesQueued > queuedBefore);
}

TEST_CASE("media/video_decoder: a seek while paused delivers the frame at the target, then holds",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    auto spec = smallClip(160, 120, 10, 20);
    spec.keyFrameInterval = 5;
    const auto path = writeClip(dir, "paused_seek.mp4", spec);
    auto decoder = startDecoder(path, noLoop());
    REQUIRE(waitUntil([&] { return decoder->frames().size() == decoder->frames().capacity(); }));
    decoder->pause();
    CHECK(decoder->state() == DecoderState::Paused); // reported at once, also while the thread waits for room
    decoder->seek(1.25);
    CHECK(decoder->state() == DecoderState::Paused);
    const auto target = collectFrames(*decoder, 1);
    REQUIRE(target.size() == 1);
    CHECK(target[0].timing.generation == 1);
    CHECK(target[0].counter == 12u);
    // Paused: nothing more is decoded until resume().
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(decoder->frames().empty());
    decoder->resume();
    CHECK(decoder->state() == DecoderState::Playing);
    const auto rest = collectFrames(*decoder, 7);
    REQUIRE(rest.size() == 7);
    CHECK(rest.front().counter == 13u);
    CHECK(rest.back().counter == 19u);
}

TEST_CASE("media/video_decoder: a seek past the end of a looping clip continues from its start",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "past_end.mp4", smallClip(160, 120, 10, 10));
    auto decoder = startDecoder(path); // loops
    REQUIRE(collectFrames(*decoder, 1).size() == 1);
    decoder->seek(5.0); // the clip lasts 1 s
    std::optional<rv::test::DecodedFrame> first;
    REQUIRE(waitUntil([&] {
        for (const auto& frame : collectFrames(*decoder, 1, std::chrono::milliseconds(1))) {
            if (frame.timing.generation == 1) {
                first = frame;
            }
        }
        return first.has_value();
    }));
    CHECK(first->counter == 0u);
    const auto next = collectFrames(*decoder, 3);
    REQUIRE(next.size() == 3);
    CHECK(next[2].counter == 3u);
    CHECK(next[2].timing.ptsSeconds > first->timing.ptsSeconds);
}

TEST_CASE("media/video_decoder: a paused decoder decodes nothing until resumed", "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "paused.mp4", smallClip(160, 120, 10, 5));
    DecoderSettings settings = noLoop();
    settings.startPaused = true;
    auto decoder = startDecoder(path, settings);
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Paused; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(decoder->stats().framesDecoded == 0);
    CHECK(decoder->streamInfo()); // opened all the same
    decoder->resume();
    const auto frames = collectFrames(*decoder, 5);
    CHECK(frames.size() == 5);
}

TEST_CASE("media/video_decoder: stop returns without waiting and the thread ends on its own",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "full.mp4", smallClip(160, 120, 10, 10));
    auto decoder = startDecoder(path); // loops forever, nobody pops
    REQUIRE(waitUntil([&] { return decoder->frames().size() == decoder->frames().capacity(); }));
    CHECK(decoder->threadRunning());
    decoder->stop();
    CHECK(decoder->state() == DecoderState::Stopped);
    // The thread waiting for room wakes on the stop and ends promptly, without anyone joining it.
    REQUIRE(waitUntil([&] { return !decoder->threadRunning(); }, std::chrono::seconds(5)));
    CHECK(decoder->state() == DecoderState::Stopped);
    CHECK(decoder->frames().size() == decoder->frames().capacity()); // what was queued can still be shown
    CHECK(decoder->frames().pop());
    decoder->stop();                                // idempotent
    const double cpu = decoder->threadCpuSeconds(); // readable after the thread ended
    CHECK(cpu >= 0.0);
    rv::media::waitForStoppedDecoders();
    CHECK(decoder->threadCpuSeconds() == cpu);
}

TEST_CASE("media/video_decoder: decoders destroyed while they open or decode never wait for their thread",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto path = writeClip(dir, "short.mp4", smallClip(160, 120, 10, 10));
    for (int round = 0; round < 8; ++round) {
        auto decoder = startDecoder(path);
        if (round % 2 == 1) {
            REQUIRE(waitUntil([&] { return !decoder->frames().empty(); }));
        }
        decoder.reset(); // stop() without waiting; the thread keeps the shared state alive until it ends
    }
    const std::uint64_t waits = rv::media::stoppedDecoderWaits();
    rv::media::waitForStoppedDecoders(); // every thread has ended
    rv::media::waitForStoppedDecoders(); // nothing left: returns at once
    CHECK(rv::media::stoppedDecoderWaits() == waits + 2); // counted (I4 step 6b: the hero CRT's report)
}

TEST_CASE("media/video_decoder: unusable files fail with their reason and never crash", "[media][video_decoder]") {
    const MediaTestDir dir;
    const auto clip = writeClip(dir, "source.avi", smallClip(160, 120, 10, 10));

    CHECK(failureOf(dir.file("missing.mp4")) == DecodeFailure::FileNotFound);
    CHECK(failureOf(dir.path()) == DecodeFailure::FileNotFound); // a directory

    REQUIRE(rv::media::writeRawFile(dir.file("empty.mp4"), {}));
    CHECK(failureOf(dir.file("empty.mp4")) == DecodeFailure::InvalidFile);

    REQUIRE(rv::media::writeRawFile(dir.file("text.mp4"), "This is not a video, only some text in a file.\n"));
    CHECK(failureOf(dir.file("text.mp4")) == DecodeFailure::InvalidFile);

    REQUIRE(rv::media::writeSilentWav(dir.file("audio.wav"), 0.5));
    CHECK(failureOf(dir.file("audio.wav")) == DecodeFailure::NoVideoStream);

    REQUIRE(rv::media::writeUnknownCodecCopy(clip, dir.file("unknown.avi")));
    CHECK(failureOf(dir.file("unknown.avi")) == DecodeFailure::UnsupportedCodec);

    const auto probed = rv::media::probeVideo(dir.file("audio.wav"));
    REQUIRE_FALSE(probed.has_value());
    CHECK(probed.error().code == rv::ErrorCode::NotFound);
}

TEST_CASE("media/video_decoder: a file cut short shows its frames, then fails as truncated", "[media][video_decoder]") {
    const MediaTestDir dir;
    auto spec = smallClip(320, 240, 10, 40);
    spec.quantizer = 2; // larger frames: the cut falls well inside the stream
    const auto clip = writeClip(dir, "long.mp4", spec);
    REQUIRE(rv::media::writeTruncatedCopy(clip, dir.file("truncated.mp4"), 0.5));
    auto decoder = startDecoder(dir.file("truncated.mp4"), noLoop());
    const auto frames = collectFrames(*decoder, 40);
    CHECK(frames.size() > 5);
    CHECK(frames.size() < 40);
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Failed; }));
    CHECK(decoder->failure() == DecodeFailure::Truncated);
}

TEST_CASE("media/video_decoder: a Matroska clip whose audio outlasts its video plays and loops",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    auto spec = smallClip(160, 120, 10, 10); // 1 s of video
    spec.audioSeconds = 4.0;                 // the container lasts 4 s
    const auto path = writeClip(dir, "longer_audio.mkv", spec);
    auto decoder = startDecoder(path); // loops
    const auto frames = collectFrames(*decoder, 25);
    REQUIRE(frames.size() == 25);
    CHECK(frames[24].counter == 4u);
    CHECK(frames[24].loop == 2u);
    CHECK(frames[24].timing.ptsSeconds == Approx(2.4).margin(0.001));
    CHECK(decoder->failure() == DecodeFailure::None);
    CHECK(decoder->state() == DecoderState::Playing);
}

TEST_CASE("media/video_decoder: a Matroska file cut short shows its frames, then fails as truncated",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    auto spec = smallClip(320, 240, 10, 40);
    spec.quantizer = 2; // larger frames: the cut falls well inside the stream
    const auto clip = writeClip(dir, "long.mkv", spec);
    REQUIRE(rv::media::writeTruncatedCopy(clip, dir.file("truncated.mkv"), 0.5));
    auto decoder = startDecoder(dir.file("truncated.mkv"), noLoop());
    const auto frames = collectFrames(*decoder, 40);
    CHECK(frames.size() > 5);
    CHECK(frames.size() < 40);
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Failed; }));
    CHECK(decoder->failure() == DecodeFailure::Truncated);
}

TEST_CASE("media/video_decoder: corrupt data mid-stream ends in the fallback, never in a crash or a hang",
          "[media][video_decoder]") {
    const MediaTestDir dir;
    auto spec = smallClip(320, 240, 10, 30);
    spec.quantizer = 2;
    const auto clip = writeClip(dir, "clean.mp4", spec);
    REQUIRE(rv::media::writeCorruptedCopy(clip, dir.file("corrupt.mp4"), 7));
    auto decoder = startDecoder(dir.file("corrupt.mp4"), noLoop());
    const auto frames = collectFrames(*decoder, 30);
    // FFmpeg conceals the damage, but the damaged frames and errors it reports declare the stream corrupt.
    REQUIRE(waitUntil([&] { return decoder->state() == DecoderState::Failed; }));
    CHECK(decoder->failure() == DecodeFailure::CorruptData);
    const auto stats = decoder->stats();
    CHECK(stats.decodeErrors + stats.damagedFrames >= 3);
    CHECK(frames.size() > 0); // the frames before the damage were shown
    CHECK(frames.size() < 30);
    decoder->stop();

    // The same clip without damage plays to its end.
    auto clean = startDecoder(clip, noLoop());
    CHECK(collectFrames(*clean, 30).size() == 30);
    CHECK(clean->stats().decodeErrors == 0);
    CHECK(clean->stats().damagedFrames == 0);
}

TEST_CASE("media/video_decoder: settings out of range are refused", "[media][video_decoder]") {
    CHECK_FALSE(rv::media::VideoDecoder::start({}, {}).has_value());
    DecoderSettings settings;
    settings.queueCapacity = 0;
    CHECK(rv::media::VideoDecoder::start("x.mp4", settings).error().code == rv::ErrorCode::InvalidArgument);
    settings = {};
    settings.threadCount = 0;
    CHECK_FALSE(rv::media::VideoDecoder::start("x.mp4", settings).has_value());
    settings = {};
    settings.maxHeight = -1;
    CHECK_FALSE(rv::media::VideoDecoder::start("x.mp4", settings).has_value());
    settings = {};
    settings.maxConsecutiveErrors = 0;
    CHECK_FALSE(rv::media::VideoDecoder::start("x.mp4", settings).has_value());
    settings = {};
    settings.corruptErrorWindow = 65;
    CHECK_FALSE(rv::media::VideoDecoder::start("x.mp4", settings).has_value());
    settings = {};
    settings.corruptErrorThreshold = 0;
    CHECK_FALSE(rv::media::VideoDecoder::start("x.mp4", settings).has_value());
}

TEST_CASE("media/video_decoder: names of states and failures", "[media][video_decoder]") {
    CHECK(rv::media::toString(DecoderState::Playing) == "playing");
    CHECK(rv::media::toString(DecodeFailure::Truncated) == "truncated");
    CHECK(rv::media::errorCodeFor(DecodeFailure::UnsupportedCodec) == rv::ErrorCode::Unsupported);
    CHECK(rv::media::errorCodeFor(DecodeFailure::FileNotFound) == rv::ErrorCode::NotFound);
    CHECK(rv::media::ffmpegVersionInfo().find("libavcodec") != std::string::npos);
}
