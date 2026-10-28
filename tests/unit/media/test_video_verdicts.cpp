// The clips' verdicts (I5 step 7, ADR-0016 §8: <user data>/cache/media/videos.json): keyed by the path under the media
// root (folded as the file system compares names) and the file's size and time, a change of either checked again, the
// canonical text read back byte for byte, two writers merging into one file without dropping each other's verdicts.

#include "support/temp_dir.hpp"

#include <rv/core/file_io.hpp>
#include <rv/media/video_verdicts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using rv::media::DecodeFailure;
using rv::media::VerdictSource;
using rv::media::VideoFileFacts;
using rv::media::VideoVerdict;
using rv::media::VideoVerdicts;

namespace {

VideoVerdict verdict(std::string path, DecodeFailure failure = DecodeFailure::None, std::uint64_t bytes = 1000,
                     std::int64_t modified = 133000000000000000) {
    VideoVerdict made;
    made.path = std::move(path);
    made.file = VideoFileFacts{bytes, modified};
    made.failure = failure;
    if (failure == DecodeFailure::None) {
        made.durationMs = 38000;
        made.width = 640;
        made.height = 480;
        made.audioSampleRate = 96000;
        made.codec = "h264";
    }
    return made;
}

/// The canonical text of two verdicts: a playable clip and a truncated one (members sorted, one verdict per line).
constexpr std::string_view kTwoVerdicts =
    "{\"format\":\"retroverse-video-verdicts\",\"version\":1,\"videos\":[\n"
    "{\"audioSampleRate\":96000,\"bytes\":1000,\"codec\":\"h264\",\"damagedFrames\":0,\"decodeErrors\":0,"
    "\"durationMs\":38000,\"height\":480,\"modified\":133000000000000000,\"path\":\"snes/videos/Amber Tide (USA).mp4\","
    "\"source\":\"check\",\"verdict\":\"playable\",\"width\":640},\n"
    "{\"audioSampleRate\":0,\"bytes\":2000,\"codec\":\"\",\"damagedFrames\":0,\"decodeErrors\":0,\"durationMs\":0,"
    "\"height\":0,\"modified\":-5,\"path\":\"snes/videos/Brass Kettle (USA).mp4\",\"source\":\"play\","
    "\"verdict\":\"truncated\",\"width\":0}\n"
    "]}\n";

} // namespace

TEST_CASE("media/video_verdicts: verdict names", "[media][video_verdicts]") {
    CHECK(rv::media::verdictName(DecodeFailure::None) == "playable");
    CHECK(rv::media::verdictName(DecodeFailure::InvalidFile) == "invalid-file");
    CHECK(rv::media::verdictName(DecodeFailure::NoVideoStream) == "no-video-stream");
    CHECK(rv::media::verdictName(DecodeFailure::UnsupportedCodec) == "unsupported-codec");
    CHECK(rv::media::verdictName(DecodeFailure::CorruptData) == "corrupt-data");
    CHECK(rv::media::verdictName(DecodeFailure::Truncated) == "truncated");
    CHECK(rv::media::verdictName(DecodeFailure::DecoderError) == "decoder-error");
    for (const DecodeFailure failure : {DecodeFailure::None, DecodeFailure::InvalidFile, DecodeFailure::NoVideoStream,
                                        DecodeFailure::UnsupportedCodec, DecodeFailure::CorruptData,
                                        DecodeFailure::Truncated, DecodeFailure::DecoderError}) {
        CHECK(rv::media::parseVerdictName(rv::media::verdictName(failure)) == failure);
    }
    CHECK_FALSE(rv::media::parseVerdictName("file-not-found")); // a missing file has no verdict
    CHECK_FALSE(rv::media::parseVerdictName("read-error"));     // nor one the system could not read
    CHECK_FALSE(rv::media::parseVerdictName("Playable"));
    CHECK(rv::media::toString(VerdictSource::Check) == "check");
    CHECK(rv::media::toString(VerdictSource::Play) == "play");
}

TEST_CASE("media/video_verdicts: keyed by the path as the file system compares it, and the file's size and time",
          "[media][video_verdicts]") {
    VideoVerdicts verdicts;
    REQUIRE(verdicts.set(verdict("snes/videos/Amber Tide (USA).mp4")));
    REQUIRE(verdicts.set(verdict("snes/videos/Brass Kettle (USA).mp4", DecodeFailure::Truncated, 2000)));
    CHECK(verdicts.size() == 2);

    // The path folded: another case finds the same clip, another name none.
    const VideoVerdict* found = verdicts.find("SNES/Videos/AMBER TIDE (usa).MP4");
    REQUIRE(found != nullptr);
    CHECK(found->path == "snes/videos/Amber Tide (USA).mp4");
    CHECK(verdicts.find("snes/videos/Amber Tide (Europe).mp4") == nullptr);
    CHECK(verdicts.find("snes/videos") == nullptr);

    // The file's facts: the same size and time keep the verdict; another size or another time is checked again.
    const VideoFileFacts facts{2000, 133000000000000000};
    CHECK(verdicts.findCurrent("snes/videos/Brass Kettle (USA).mp4", facts) != nullptr);
    CHECK(verdicts.findCurrent("snes/videos/Brass Kettle (USA).mp4", VideoFileFacts{2001, facts.lastWriteTime}) ==
          nullptr);
    CHECK(verdicts.findCurrent("snes/videos/Brass Kettle (USA).mp4",
                               VideoFileFacts{facts.size, facts.lastWriteTime + 1}) == nullptr);

    // A new verdict of the path (spelled otherwise) replaces the old one.
    REQUIRE(verdicts.set(verdict("SNES/videos/brass kettle (usa).mp4", DecodeFailure::None, 2001)));
    CHECK(verdicts.size() == 2);
    CHECK(verdicts.find("snes/videos/Brass Kettle (USA).mp4")->playable());
    CHECK(verdicts.find("snes/videos/Brass Kettle (USA).mp4")->path == "SNES/videos/brass kettle (usa).mp4");

    CHECK(verdicts.erase("snes/videos/BRASS KETTLE (USA).mp4"));
    CHECK_FALSE(verdicts.erase("snes/videos/Brass Kettle (USA).mp4"));
    CHECK(verdicts.size() == 1);

    // Malformed paths and a missing file's failure are refused.
    for (const std::string& path : {std::string(), std::string("snes\\videos\\a.mp4"), std::string("snes//a.mp4"),
                                    std::string("../a.mp4"), std::string("snes/./a.mp4"), std::string("a\xff.mp4")}) {
        INFO(path);
        CHECK_FALSE(verdicts.set(verdict(path)));
        CHECK(verdicts.find(path) == nullptr);
    }
    CHECK_FALSE(verdicts.set(verdict("snes/videos/c.mp4", DecodeFailure::FileNotFound)));
    CHECK_FALSE(verdicts.set(verdict("snes/videos/c.mp4", DecodeFailure::ReadError)));
}

TEST_CASE("media/video_verdicts: the canonical text reads back byte for byte", "[media][video_verdicts]") {
    VideoVerdicts verdicts;
    REQUIRE(verdicts.set(verdict("snes/videos/Brass Kettle (USA).mp4", DecodeFailure::Truncated, 2000, -5)));
    VideoVerdict played = verdict("snes/videos/Amber Tide (USA).mp4");
    REQUIRE(verdicts.set(played));
    // The truncated one came from the app.
    VideoVerdict fromApp = *verdicts.find("snes/videos/Brass Kettle (USA).mp4");
    fromApp.source = VerdictSource::Play;
    REQUIRE(verdicts.set(fromApp));

    const auto text = verdicts.format();
    REQUIRE(text.has_value());
    CHECK(*text == kTwoVerdicts);

    const auto parsed = VideoVerdicts::parse(*text, "videos.json");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == verdicts);
    const auto again = parsed->format();
    REQUIRE(again.has_value());
    CHECK(*again == *text); // byte-identical

    const auto empty = VideoVerdicts{}.format();
    REQUIRE(empty.has_value());
    CHECK(*empty == "{\"format\":\"retroverse-video-verdicts\",\"version\":1,\"videos\":[]}\n");
    REQUIRE(VideoVerdicts::parse(*empty, "empty").has_value());
    CHECK(VideoVerdicts::parse(*empty, "empty")->empty());
}

TEST_CASE("media/video_verdicts: texts other than the canonical form are refused", "[media][video_verdicts]") {
    const std::string text(kTwoVerdicts);
    const auto refused = [](const std::string& changed) {
        INFO(changed);
        CHECK_FALSE(VideoVerdicts::parse(changed, "videos.json").has_value());
    };
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string changed = text;
        const std::size_t at = changed.find(from);
        REQUIRE(at != std::string::npos);
        changed.replace(at, from.size(), to);
        return changed;
    };
    refused(replaced("\"version\":1", "\"version\":2"));
    refused(replaced("retroverse-video-verdicts", "retroverse-videos"));
    refused(replaced("\"verdict\":\"truncated\"", "\"verdict\":\"broken\""));
    refused(replaced("\"verdict\":\"truncated\"", "\"verdict\":\"file-not-found\""));
    refused(replaced("\"source\":\"play\"", "\"source\":\"user\""));
    refused(replaced("\"bytes\":2000", "\"bytes\":-2000"));
    refused(replaced("\"durationMs\":38000", "\"durationMs\":38000.5"));
    refused(replaced("\"width\":640}", "\"width\":640,\"zoom\":1}"));
    refused(replaced("Brass Kettle (USA).mp4", "Amber Tide (USA).MP4"));            // two verdicts of one path
    refused(replaced("{\"audioSampleRate\":96000", "{ \"audioSampleRate\":96000")); // white space: edited by hand
    refused("{}\n");
    refused("not json");
}

TEST_CASE("media/video_verdicts: the file, merged by two writers", "[media][video_verdicts]") {
    const rv::test::TempDir dir("video-verdicts");
    const std::filesystem::path file = rv::media::videoVerdictsFile(dir.file("cache"));
    CHECK(file == dir.file("cache") / "media" / "videos.json");

    // No file: no verdict.
    const auto none = rv::media::readVideoVerdicts(file);
    REQUIRE(none.has_value());
    CHECK(none->empty());

    // The CLI writes two, then the app one: the file holds the three.
    const std::vector<VideoVerdict> checked{verdict("snes/videos/a.mp4"),
                                            verdict("snes/videos/b.mp4", DecodeFailure::CorruptData)};
    REQUIRE(rv::media::updateVideoVerdicts(file, checked).has_value());
    VideoVerdict failed = verdict("snes/videos/c.mp4", DecodeFailure::UnsupportedCodec);
    failed.source = VerdictSource::Play;
    const auto merged = rv::media::updateVideoVerdicts(file, std::vector<VideoVerdict>{failed});
    REQUIRE(merged.has_value());
    CHECK(merged->size() == 3);
    const auto read = rv::media::readVideoVerdicts(file);
    REQUIRE(read.has_value());
    CHECK(*read == *merged);
    CHECK(read->find("snes/videos/b.mp4")->failure == DecodeFailure::CorruptData);
    CHECK(read->find("snes/videos/c.mp4")->source == VerdictSource::Play);

    // A clip gone is removed; its file's bytes are those of format().
    const auto removed = rv::media::updateVideoVerdicts(file, std::vector<VideoVerdict>{},
                                                        std::vector<std::string>{"SNES/VIDEOS/A.MP4"});
    REQUIRE(removed.has_value());
    CHECK(removed->size() == 2);
    const auto bytes = rv::core::readTextFile(file);
    REQUIRE(bytes.has_value());
    CHECK(*bytes == removed->format().value());

    // A file that cannot be read as verdicts is replaced by the next write.
    REQUIRE(rv::core::writeTextFileAtomic(file, "{\"edited\": true}\n"));
    CHECK_FALSE(rv::media::readVideoVerdicts(file).has_value());
    const auto replaced = rv::media::updateVideoVerdicts(file, std::vector<VideoVerdict>{verdict("snes/videos/d.mp4")});
    REQUIRE(replaced.has_value());
    CHECK(replaced->size() == 1);
    CHECK(rv::media::readVideoVerdicts(file).value().size() == 1);

    // A malformed change writes nothing.
    CHECK_FALSE(rv::media::updateVideoVerdicts(file, std::vector<VideoVerdict>{verdict("../e.mp4")}).has_value());
    CHECK(rv::media::readVideoVerdicts(file).value().size() == 1);
}

TEST_CASE("media/video_verdicts: what the decoder found describes the verdict", "[media][video_verdicts]") {
    rv::media::StreamInfo info;
    info.codec = "h264";
    info.codedSize = rv::media::FrameSize{640, 480};
    info.durationSeconds = 37.9996;
    info.audioSampleRate = 96000;
    VideoVerdict described;
    rv::media::describeStream(described, info);
    CHECK(described.codec == "h264");
    CHECK(described.width == 640);
    CHECK(described.height == 480);
    CHECK(described.durationMs == 38000);
    CHECK(described.audioSampleRate == 96000);
}
