// The clips' verdicts in the app (I5 step 7, ADR-0016 §8): tests/fixtures/esde/video-verdicts recorded as the case's
// library with one clip kept (the others removed before the import), its snes room's hero CRT started with
// --start-room. A clip that fails is remembered in <data>/cache/media/videos.json; the next run never opens it (0
// decoders) and its screen shows its game's still from its first picture; a file changed since is opened again. A clip
// played through once clean is remembered playable. The runs are lockstep (--fixed-dt), so the verdicts are read before
// the first frame.

#include "building_support.hpp"
#include "smoke_support.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/media/video_verdicts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1280, 720};

/// The fixture without every clip but `kept` (a file name under media/snes/videos).
auto keepOnlyClip(std::string_view kept) {
    return [kept](const rv::test::Fixture& fixture) {
        for (const auto& entry : fs::directory_iterator(fixture.path("media/snes/videos"))) {
            if (rv::core::pathToUtf8(entry.path().filename()) != kept) {
                fs::remove(entry.path());
            }
        }
    };
}

/// A lockstep run of `frames` frames in the snes room, its hero CRT the room's; a screenshot at the end.
AppRun runRoom(const PlannedBuilding& building, std::string_view name, int frames) {
    AppRun run = runApp(retroverseExe(), building.dir, name,
                        {"--start-room", building.roomOf("snes"), "--exit-after-frames", "--frames",
                         std::to_string(frames), "--fixed-dt", "0.016667", "--screenshot",
                         arg(building.dir / (std::string(name) + ".png")), "--screenshot-size", "1280x720"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    return run;
}

rv::media::VideoVerdicts verdictsOf(const PlannedBuilding& building) {
    auto verdicts = rv::media::readVideoVerdicts(rv::media::videoVerdictsFile(building.data / "cache"));
    INFO((verdicts ? std::string("read") : verdicts.error().toString()));
    REQUIRE(verdicts.has_value());
    return std::move(*verdicts);
}

bool showsVideo(const rv::app::VideoRecord& video) {
    return std::any_of(video.shown.begin(), video.shown.end(),
                       [](const rv::app::VideoShownRecord& shown) { return shown.picture == "video"; });
}

} // namespace

TEST_CASE("video verdicts: a clip that failed is never opened again, its screen on its still from the start",
          "[video_verdicts]") {
    constexpr std::string_view kClip = "Ember Quill (USA).avi"; // an unsupported codec: it fails as it opens
    const PlannedBuilding building("video_verdicts/known_bad", true, "esde/video-verdicts", keepOnlyClip(kClip));
    const fs::path clip = building.fixture.path("media/snes/videos/Ember Quill (USA).avi");
    const std::string key = "snes/videos/Ember Quill (USA).avi";

    // The first run opens the clip, which fails: the screen falls back to the game's screenshot, and the verdict is
    // kept in the user data's cache.
    const AppRun first = runRoom(building, "first", 60);
    const rv::app::VideoRecord& opened = first.report->video;
    INFO(first.describe());
    REQUIRE(opened.bound);
    const std::string game = opened.gameId;
    CHECK(opened.decodersStarted == 1);
    REQUIRE(opened.fallbacks.size() >= 1);
    CHECK(opened.fallbacks.front().gameId == game);
    CHECK(opened.fallbacks.front().picture == "video");
    CHECK(opened.fallbacks.front().reason == "unsupported-codec");
    CHECK(opened.verdicts.loaded); // no file yet: no verdict
    CHECK(opened.verdicts.entries == 0);
    CHECK(opened.verdicts.knownBad.empty());
    REQUIRE(opened.verdicts.given.size() == 1);
    CHECK(opened.verdicts.given.front().gameId == game);
    CHECK(opened.verdicts.given.front().verdict == "unsupported-codec");
    CHECK(opened.verdicts.written == 1);
    CHECK(opened.verdicts.writeFailures == 0);
    const rv::media::VideoVerdicts kept = verdictsOf(building);
    REQUIRE(kept.size() == 1);
    const rv::media::VideoVerdict* verdict = kept.find(key);
    REQUIRE(verdict != nullptr);
    CHECK(verdict->failure == rv::media::DecodeFailure::UnsupportedCodec);
    CHECK(verdict->source == rv::media::VerdictSource::Play);
    CHECK(verdict->file == rv::core::listedFileFacts(clip).value());

    // The second run never opens it: no decoder, no video fallback; the game is the screen's (its still: the room has
    // no clip left to play), its screenshot the first picture shown, never the screen's own pattern.
    const AppRun second = runRoom(building, "second", 60);
    const rv::app::VideoRecord& known = second.report->video;
    INFO(second.describe());
    REQUIRE(known.bound);
    CHECK(known.decodersStarted == 0);
    CHECK(known.maxDecoderThreads == 0);
    CHECK(known.fallbacks.empty());
    CHECK(known.verdicts.loaded);
    CHECK(known.verdicts.entries == 1);
    REQUIRE(known.verdicts.knownBad.size() == 1);
    CHECK(known.verdicts.knownBad.front().gameId == game);
    CHECK(known.verdicts.knownBad.front().verdict == "unsupported-codec");
    CHECK(known.verdicts.given.empty());
    CHECK(known.gameId == game);
    CHECK(known.picture == "screenshot");
    REQUIRE_FALSE(known.shown.empty());
    CHECK(known.shown.front().gameId == game);
    CHECK(known.shown.front().picture == "screenshot");
    CHECK_FALSE(showsVideo(known));
    CHECK(known.idleFramesWithGame == 0);
    static_cast<void>(requireScreenshot(second, building.dir / "second.png", kShotSize));

    // The file changed (its time): the clip is opened again, and fails again.
    fs::last_write_time(clip, fs::last_write_time(clip) + std::chrono::hours(1));
    const AppRun third = runRoom(building, "third", 60);
    const rv::app::VideoRecord& changed = third.report->video;
    INFO(third.describe());
    CHECK(changed.decodersStarted == 1);
    CHECK(changed.verdicts.knownBad.empty());
    REQUIRE(changed.verdicts.given.size() == 1);
    CHECK(changed.verdicts.given.front().verdict == "unsupported-codec");
    CHECK(verdictsOf(building).find(key)->file == rv::core::listedFileFacts(clip).value());
}

TEST_CASE("video verdicts: a clip played through once clean is remembered playable", "[video_verdicts]") {
    constexpr std::string_view kClip = "Amber Tide (USA).mp4"; // 3 s
    const PlannedBuilding building("video_verdicts/played", true, "esde/video-verdicts", keepOnlyClip(kClip));

    // 4 s: the clip's first pass ends, and the next shows.
    const AppRun first = runRoom(building, "first", 240);
    const rv::app::VideoRecord& played = first.report->video;
    INFO(first.describe());
    CHECK(played.decodersStarted == 1);
    CHECK(played.fallbacks.empty());
    REQUIRE(played.verdicts.given.size() == 1);
    CHECK(played.verdicts.given.front().verdict == "playable");
    CHECK(played.verdicts.written == 1);
    const rv::media::VideoVerdicts kept = verdictsOf(building);
    const rv::media::VideoVerdict* verdict = kept.find("snes/videos/Amber Tide (USA).mp4");
    REQUIRE(verdict != nullptr);
    CHECK(verdict->playable());
    CHECK(verdict->source == rv::media::VerdictSource::Play);
    CHECK(verdict->durationMs == 3000);
    CHECK(verdict->codec == "mpeg4");

    // Played again: nothing new to say, nothing written.
    const AppRun second = runRoom(building, "second", 240);
    CHECK(second.report->video.verdicts.loaded);
    CHECK(second.report->video.verdicts.knownBad.empty());
    CHECK(second.report->video.verdicts.given.empty());
    CHECK(second.report->video.decodersStarted == 1);
}
