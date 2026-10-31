// The screens' verdicts (I5 step 7) asked for one clip (I5 step 8's review: the media viewer's, whose game the world may
// never have screened): answered from the failure verdicts and the file's facts, without swallowing the world's changes.

#include "support/temp_dir.hpp"

#include <rv/app/screen_verdicts.hpp>
#include <rv/core/file_io.hpp>
#include <rv/media/video_verdicts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using rv::app::ScreenGame;
using rv::app::ScreenMedia;
using rv::app::ScreenVerdicts;
using rv::media::DecodeFailure;
using rv::media::VideoVerdict;
using State = ScreenVerdicts::ClipScreening::State;

constexpr std::chrono::seconds kBound{5};

struct Library {
    rv::test::TempDir dir{"screen-verdicts"};
    std::filesystem::path verdictsFile = rv::media::videoVerdictsFile(dir.file("cache"));

    /// A clip file below the media root, `key` its path there.
    [[nodiscard]] ScreenMedia clip(const std::string& key) const {
        ScreenMedia media;
        media.video = dir.file("media") / std::filesystem::path(key);
        media.videoKey = key;
        return media;
    }

    [[nodiscard]] ScreenMedia write(const std::string& key) const {
        ScreenMedia media = clip(key);
        std::filesystem::create_directories(media.video.parent_path());
        REQUIRE(rv::core::writeTextFileAtomic(media.video, "not a clip, but a file with a size and a time\n"));
        return media;
    }

    /// A failure verdict for the file of `media` as it is now (`sizeOffset` != 0: as it was before it changed).
    void fail(const ScreenMedia& media, DecodeFailure failure, std::uint64_t sizeOffset = 0) const {
        const auto facts = rv::core::listedFileFacts(media.video);
        REQUIRE(facts.has_value());
        VideoVerdict verdict;
        verdict.path = media.videoKey;
        verdict.file = *facts;
        verdict.file.size += sizeOffset;
        verdict.failure = failure;
        REQUIRE(rv::media::updateVideoVerdicts(verdictsFile, std::vector<VideoVerdict>{verdict}).has_value());
    }
};

} // namespace

TEST_CASE("app/screen_verdicts: one clip answered from the failures and its file's facts", "[app][screen_verdicts]") {
    const Library library;
    const ScreenMedia known = library.write("snes/videos/Known Bad.mp4");
    const ScreenMedia changed = library.write("snes/videos/Changed.mp4");
    const ScreenMedia clean = library.write("snes/videos/Clean.mp4");
    library.fail(known, DecodeFailure::UnsupportedCodec);
    library.fail(changed, DecodeFailure::CorruptData, 1);

    ScreenVerdicts verdicts(library.verdictsFile, true, kBound);
    verdicts.waitLoaded(kBound);

    // A clip the world never screened: its failure verdict holds for its file as it is (looked at once, then known).
    const ScreenVerdicts::ClipScreening knownAnswer = verdicts.screenClip(known, "game-known");
    CHECK(knownAnswer.state == State::LeftOut);
    CHECK(knownAnswer.failure == "unsupported-codec");
    CHECK(verdicts.screenClip(known, "game-known").state == State::LeftOut);
    // Its file changed since its verdict: given (played and judged again).
    const ScreenVerdicts::ClipScreening changedAnswer = verdicts.screenClip(changed, "game-changed");
    CHECK(changedAnswer.state == State::Given);
    CHECK(changedAnswer.failure.empty());
    // No verdict, no key: given.
    CHECK(verdicts.screenClip(clean, "game-clean").state == State::Given);
    ScreenMedia unkeyed = known;
    unkeyed.videoKey.clear();
    CHECK(verdicts.screenClip(unkeyed, "game-unkeyed").state == State::Given);

    // The world's pending change (the file read, the changed clip's look) is still there for the world to take.
    CHECK(verdicts.takeChanged());
    CHECK_FALSE(verdicts.takeChanged());

    // The world's games agree: the known clip left out, the changed one given; the known clip recorded once.
    std::vector<ScreenGame> games(2);
    games[0].id = "game-known";
    games[0].media = known;
    games[1].id = "game-changed";
    games[1].media = changed;
    const std::vector<ScreenGame> screened = verdicts.screened(games);
    CHECK(screened[0].media.video.empty());
    CHECK(screened[1].media.video == changed.video);
    rv::app::VideoVerdictsRecord record;
    verdicts.record(record);
    REQUIRE(record.knownBad.size() == 1);
    CHECK(record.knownBad[0].gameId == "game-known");
    CHECK(record.knownBad[0].verdict == "unsupported-codec");
}

TEST_CASE("app/screen_verdicts: one clip's question keeps the file's first change for the world",
          "[app][screen_verdicts]") {
    const Library library;
    const ScreenMedia known = library.write("snes/videos/Known Bad.mp4");
    library.fail(known, DecodeFailure::Truncated);

    ScreenVerdicts verdicts(library.verdictsFile, true, kBound);
    verdicts.waitLoaded(kBound);
    CHECK(verdicts.screenClip(known, "game-known").state == State::LeftOut);
    // screened() resets the flag, screenClip() never does: the world still screens its games again.
    CHECK(verdicts.takeChanged());
}

TEST_CASE("app/screen_verdicts: without settling, one clip waits while its file is looked at",
          "[app][screen_verdicts]") {
    const Library library;
    const ScreenMedia known = library.write("snes/videos/Known Bad.mp4");
    library.fail(known, DecodeFailure::CorruptData);

    ScreenVerdicts verdicts(library.verdictsFile, false, kBound);
    verdicts.waitLoaded(kBound);
    // Looking until the thread has looked at the file (never opened meanwhile), then left out.
    ScreenVerdicts::ClipScreening answer = verdicts.screenClip(known, "game-known");
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (answer.state == State::Looking && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        answer = verdicts.screenClip(known, "game-known");
    }
    CHECK(answer.state == State::LeftOut);
    CHECK(answer.failure == "corrupt-data");
}

TEST_CASE("app/screen_verdicts: no verdicts file gives every clip", "[app][screen_verdicts]") {
    const Library library;
    const ScreenMedia known = library.write("snes/videos/Known Bad.mp4");
    ScreenVerdicts verdicts(std::filesystem::path(), true, kBound);
    CHECK(verdicts.screenClip(known, "game-known").state == State::Given);
    ScreenVerdicts missing(library.verdictsFile, true, kBound);
    missing.waitLoaded(kBound);
    CHECK(missing.screenClip(known, "game-known").state == State::Given);
}
