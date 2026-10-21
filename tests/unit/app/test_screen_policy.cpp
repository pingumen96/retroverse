// The hero CRT's decisions without a GPU or a decoder (I3 step 11, ADR-0012 §7): the start choice, the dwell, the
// fallback chain and the decoder's life around launches.

#include <rv/app/screen_policy.hpp>
#include <rv/esde/media_resolver.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using rv::app::ScreenGame;
using rv::app::ScreenMedia;
using rv::app::ScreenPhase;
using rv::app::ScreenPicture;
using rv::app::ScreenPolicy;

namespace {

/// The run's step under --fixed-dt 0.016667.
constexpr double kFrame = 0.016667;

ScreenGame game(std::string id, std::string video = {}, std::optional<std::int64_t> lastPlayed = std::nullopt) {
    ScreenGame g;
    g.id = std::move(id);
    if (!video.empty()) {
        g.media.video = std::filesystem::path("C:/media/videos") / video;
    }
    g.lastPlayed = lastPlayed;
    return g;
}

ScreenGame withStills(ScreenGame g, bool screenshot, bool titleScreen, bool cover) {
    if (screenshot) {
        g.media.screenshot = "C:/media/screenshots/" + g.id + ".png";
    }
    if (titleScreen) {
        g.media.titleScreen = "C:/media/titlescreens/" + g.id + ".png";
    }
    if (cover) {
        g.media.cover = "C:/media/covers/" + g.id + ".png";
    }
    return g;
}

/// Hovers `id` for `frames` frames from `start` (simulated seconds), the policy's step under --fixed-dt; returns the
/// time after them.
double hoverFor(ScreenPolicy& policy, std::optional<std::string> id, int frames, double start) {
    double now = start;
    for (int frame = 0; frame < frames; ++frame) {
        policy.hover(id ? std::optional<std::string_view>(*id) : std::nullopt, now);
        now += kFrame;
    }
    return now;
}

} // namespace

TEST_CASE("app/screen_policy: the video files ES-DE reads are played, others never opened", "[app][screen]") {
    for (const char* name : {"a.mp4", "a.MKV", "a.avi", "a.wmv", "a.Mov", "a.webm"}) {
        INFO(name);
        CHECK(rv::app::isPlayableVideoFile(name));
    }
    for (const char* name : {"a.flv", "a.mp4.txt", "a", "", "a.mpg", "a.gif"}) {
        INFO(name);
        CHECK_FALSE(rv::app::isPlayableVideoFile(name));
    }
    // The list is ES-DE's own (rv_app_cli does not link rv_esde: kept equal here).
    CHECK(std::equal(std::begin(rv::app::kScreenVideoExtensions), std::end(rv::app::kScreenVideoExtensions),
                     rv::esde::kVideoExtensions.begin(), rv::esde::kVideoExtensions.end()));
}

TEST_CASE("app/screen_policy: the start choice is the most recently played game with a video, else the first",
          "[app][screen]") {
    using rv::app::chooseStartGame;
    CHECK_FALSE(chooseStartGame({}).has_value());
    // No game with a playable video: the idle pattern.
    const std::vector<ScreenGame> none{game("a"), game("b", "b.flv", 100)};
    CHECK_FALSE(chooseStartGame(none).has_value());
    // Never played: the first with a video, in the games' (featured) order.
    const std::vector<ScreenGame> unplayed{game("a"), game("b", "b.mp4"), game("c", "c.mkv")};
    CHECK(chooseStartGame(unplayed) == 1u);
    // The most recently played one with a video wins; one played later without a playable video does not count.
    const std::vector<ScreenGame> played{game("a", "a.mp4", 10), game("b", "b.mp4", 30), game("c", "c.flv", 50),
                                         game("d", "d.mp4"), game("e", "e.mp4", 20)};
    CHECK(chooseStartGame(played) == 1u);
    // Equal times keep the earlier game.
    const std::vector<ScreenGame> tie{game("a", "a.mp4", 5), game("b", "b.mp4", 5)};
    CHECK(chooseStartGame(tie) == 0u);
    // I3 step 10c: a game that is no start candidate (one the room does not place) is never the start choice, however
    // recently played; it plays when selected.
    std::vector<ScreenGame> unplaced{game("a", "a.mp4", 90), game("b", "b.mp4", 10), game("c", "c.mp4")};
    unplaced[0].startCandidate = false;
    CHECK(chooseStartGame(unplaced) == 1u);
    unplaced[1].startCandidate = false;
    CHECK(chooseStartGame(unplaced) == 2u);
    unplaced[2].startCandidate = false;
    CHECK_FALSE(chooseStartGame(unplaced).has_value());
    ScreenPolicy selecting(unplaced);
    CHECK(selecting.targetId().empty());
    static_cast<void>(hoverFor(selecting, std::string("a"), 19, 0.0));
    CHECK(selecting.targetId() == "a");
    CHECK(selecting.wantsDecoder());

    const ScreenPolicy policy(played);
    CHECK(policy.targetId() == "b");
    CHECK(policy.targetGeneration() == 0);
    CHECK_FALSE(policy.selected());
    CHECK(policy.wantsDecoder());
    CHECK(policy.picture() == ScreenPicture::IdlePattern); // until a frame or a still shows

    const ScreenPolicy empty(none);
    CHECK(empty.targetId().empty());
    CHECK_FALSE(empty.wantsDecoder());
    CHECK(empty.picture() == ScreenPicture::IdlePattern);
}

TEST_CASE("app/screen_policy: a selection becomes the target after 300 ms, and stepping faster changes nothing",
          "[app][screen]") {
    ScreenPolicy policy({game("a", "a.mp4", 10), game("b", "b.mp4"), game("c", "c.mp4")});
    REQUIRE(policy.targetId() == "a");

    // The 18th frame after the first is 0.300 s later: the 19th frame of the hover selects, the 18th does not.
    double now = hoverFor(policy, "b", 19, 0.0);
    CHECK(policy.targetId() == "b");
    CHECK(policy.selected());
    CHECK(policy.selections() == 1);
    CHECK(policy.targetGeneration() == 1);

    ScreenPolicy early({game("a", "a.mp4", 10), game("b", "b.mp4")});
    static_cast<void>(hoverFor(early, "b", 18, 0.0));
    CHECK(early.targetId() == "a");
    CHECK(early.selections() == 0);

    // Stepping across items every 5 frames (83 ms) never restarts the decoder.
    for (const char* id : {"c", "a", "c", "a"}) {
        now = hoverFor(policy, std::string(id), 5, now);
    }
    CHECK(policy.targetId() == "b");
    CHECK(policy.targetGeneration() == 1);

    // Nothing hovered: the selection keeps playing.
    now = hoverFor(policy, std::nullopt, 120, now);
    CHECK(policy.targetId() == "b");
    // Hovering the target again is no new selection.
    now = hoverFor(policy, "b", 40, now);
    CHECK(policy.selections() == 1);
    CHECK(policy.targetGeneration() == 1);

    // A hover that leaves and comes back starts its dwell again.
    now = hoverFor(policy, "c", 10, now);
    now = hoverFor(policy, std::nullopt, 1, now);
    now = hoverFor(policy, "c", 10, now);
    CHECK(policy.targetId() == "b");
    static_cast<void>(hoverFor(policy, "c", 9, now));
    CHECK(policy.targetId() == "c");
    CHECK(policy.selections() == 2);

    // The start choice selected: counted, not restarted.
    ScreenPolicy same({game("a", "a.mp4")});
    static_cast<void>(hoverFor(same, "a", 30, 0.0));
    CHECK(same.selections() == 1);
    CHECK(same.selected());
    CHECK(same.targetGeneration() == 0);

    // An item the games do not list is a game without media: the idle pattern.
    ScreenPolicy unknown({game("a", "a.mp4")});
    static_cast<void>(hoverFor(unknown, "zzz", 30, 0.0));
    CHECK(unknown.targetId() == "zzz");
    CHECK_FALSE(unknown.hasVideo());
    CHECK_FALSE(unknown.wantedStill().has_value());
    CHECK(unknown.picture() == ScreenPicture::IdlePattern);
}

TEST_CASE("app/screen_policy: the fallback chain goes screenshot, title screen, cover, then the idle pattern",
          "[app][screen]") {
    CHECK(rv::app::stillChain(withStills(game("a"), true, true, true).media) ==
          std::vector<ScreenPicture>{ScreenPicture::Screenshot, ScreenPicture::TitleScreen, ScreenPicture::Cover});
    CHECK(rv::app::stillChain(withStills(game("a"), false, true, true).media) ==
          std::vector<ScreenPicture>{ScreenPicture::TitleScreen, ScreenPicture::Cover});
    CHECK(rv::app::stillChain(withStills(game("a"), false, false, false).media).empty());
    const ScreenMedia media = withStills(game("a", "a.mp4"), true, false, true).media;
    CHECK(*rv::app::mediaPath(media, ScreenPicture::Video) == media.video);
    CHECK(*rv::app::mediaPath(media, ScreenPicture::Cover) == media.cover);
    CHECK(rv::app::mediaPath(media, ScreenPicture::TitleScreen)->empty());
    CHECK(rv::app::mediaPath(media, ScreenPicture::IdlePattern) == nullptr);

    SECTION("the still shows while the video opens, the video once a frame of it shows") {
        ScreenPolicy policy({withStills(game("a", "a.mp4"), true, true, true)});
        CHECK(policy.wantsDecoder());
        CHECK(policy.wantedStill() == ScreenPicture::Screenshot);
        CHECK(policy.picture() == ScreenPicture::IdlePattern);
        policy.stillReady();
        CHECK(policy.picture() == ScreenPicture::Screenshot);
        CHECK_FALSE(policy.wantedStill().has_value());
        policy.videoShowing();
        CHECK(policy.picture() == ScreenPicture::Video);
        // The decoder fails later (the frames it decoded have been shown): back to the still, never asked again.
        policy.videoFailed();
        CHECK(policy.picture() == ScreenPicture::Screenshot);
        CHECK_FALSE(policy.wantsDecoder());
    }
    SECTION("each still that fails gives the next, the last the idle pattern") {
        ScreenPolicy policy({withStills(game("a", "a.mp4"), true, true, true)});
        policy.videoFailed();
        CHECK_FALSE(policy.wantsDecoder());
        policy.stillFailed();
        CHECK(policy.wantedStill() == ScreenPicture::TitleScreen);
        policy.stillFailed();
        CHECK(policy.wantedStill() == ScreenPicture::Cover);
        CHECK(policy.picture() == ScreenPicture::IdlePattern);
        policy.stillReady();
        CHECK(policy.picture() == ScreenPicture::Cover);

        ScreenPolicy bare({withStills(game("a", "a.mp4"), false, false, true)});
        bare.videoFailed();
        bare.stillFailed();
        CHECK_FALSE(bare.wantedStill().has_value());
        CHECK(bare.picture() == ScreenPicture::IdlePattern);
        bare.stillFailed(); // nothing left: harmless
        bare.stillReady();
        CHECK(bare.picture() == ScreenPicture::IdlePattern);
    }
    SECTION("a file ES-DE would not read is never asked for: its stills show") {
        ScreenPolicy policy(
            {withStills(game("a", "a.mp4"), false, false, true), withStills(game("b", "b.flv"), false, true, false)});
        static_cast<void>(hoverFor(policy, "b", 20, 0.0));
        REQUIRE(policy.targetId() == "b");
        CHECK_FALSE(policy.hasVideo());
        CHECK(policy.wantedStill() == ScreenPicture::TitleScreen);
    }
    SECTION("a new target starts its own chain afresh") {
        ScreenPolicy policy(
            {withStills(game("a", "a.mp4"), true, false, false), withStills(game("b", "b.mp4"), false, false, true)});
        policy.stillReady();
        policy.videoShowing();
        static_cast<void>(hoverFor(policy, "b", 20, 0.0));
        REQUIRE(policy.targetId() == "b");
        CHECK(policy.picture() == ScreenPicture::IdlePattern);
        CHECK(policy.wantedStill() == ScreenPicture::Cover);
        CHECK(policy.wantsDecoder());
    }
}

TEST_CASE("app/screen_policy: the decoder stops for a launch and reopens at its position after it", "[app][screen]") {
    SECTION("the return") {
        ScreenPolicy policy({game("a", "a.mp4")});
        policy.videoShowing();
        CHECK(policy.phase() == ScreenPhase::Running);
        policy.launchStarting(2.25);
        CHECK(policy.phase() == ScreenPhase::Launched);
        CHECK_FALSE(policy.wantsDecoder());
        // The picture stays (the texture keeps the last frame) while the game runs.
        CHECK(policy.picture() == ScreenPicture::Video);
        CHECK_FALSE(policy.takeResumePosition().has_value());
        policy.launchEnded();
        CHECK(policy.phase() == ScreenPhase::Running);
        CHECK(policy.wantsDecoder());
        CHECK(policy.takeResumePosition() == 2.25);
        CHECK_FALSE(policy.takeResumePosition().has_value()); // once
        policy.launchEnded();                                 // no launch: nothing
        CHECK_FALSE(policy.takeResumePosition().has_value());
    }
    SECTION("a refused or failed launch restarts it at once, from where it was") {
        ScreenPolicy policy({game("a", "a.mp4")});
        policy.launchStarting(std::nullopt); // no frame showed yet: from the start
        policy.launchEnded();
        CHECK(policy.wantsDecoder());
        CHECK_FALSE(policy.takeResumePosition().has_value());
    }
    SECTION("quitting during a game keeps it stopped") {
        ScreenPolicy policy({game("a", "a.mp4")});
        policy.launchStarting(1.0);
        policy.quit();
        CHECK(policy.phase() == ScreenPhase::Quit);
        CHECK_FALSE(policy.wantsDecoder());
        policy.launchEnded();
        CHECK(policy.phase() == ScreenPhase::Quit);
        CHECK_FALSE(policy.wantsDecoder());
        policy.launchStarting(2.0);
        CHECK(policy.phase() == ScreenPhase::Quit);
    }
    SECTION("out of view the decoder pauses, the picture kept") {
        ScreenPolicy policy({game("a", "a.mp4")});
        policy.videoShowing();
        CHECK_FALSE(policy.decoderPaused());
        policy.setVisible(false);
        CHECK(policy.decoderPaused());
        CHECK(policy.wantsDecoder());
        CHECK(policy.picture() == ScreenPicture::Video);
        policy.setVisible(true);
        CHECK_FALSE(policy.decoderPaused());
    }
}

TEST_CASE("app/screen_policy: new games replace the start choice until something is selected", "[app][screen]") {
    ScreenPolicy policy;
    CHECK(policy.targetId().empty());
    policy.setGames({game("a", "a.mp4")});
    CHECK(policy.targetId() == "a");
    CHECK(policy.targetGeneration() == 1);
    policy.setGames({game("a", "a.mp4"), game("b", "b.mp4", 40)});
    CHECK(policy.targetId() == "b");
    // The same games again: nothing restarts.
    policy.setGames({game("a", "a.mp4"), game("b", "b.mp4", 40)});
    CHECK(policy.targetGeneration() == 2);
    // A selection stays whatever the start choice becomes; its media changing shows it afresh.
    static_cast<void>(hoverFor(policy, "a", 20, 0.0));
    REQUIRE(policy.targetId() == "a");
    const std::uint64_t generation = policy.targetGeneration();
    policy.setGames({game("a", "a.mp4"), game("b", "b.mp4", 40), game("c", "c.mp4", 90)});
    CHECK(policy.targetId() == "a");
    CHECK(policy.targetGeneration() == generation);
    policy.setGames({game("a", "a2.mp4"), game("c", "c.mp4", 90)});
    CHECK(policy.targetId() == "a");
    CHECK(policy.targetGeneration() == generation + 1);
    CHECK(policy.targetMedia().video.filename() == "a2.mp4");
    CHECK(rv::app::toString(ScreenPicture::TitleScreen) == "title-screen");
    CHECK(rv::app::toString(ScreenPhase::Launched) == "launched");
}

TEST_CASE("app/screen_policy: another screen's games restart the choice, a selection forgotten, the phase kept",
          "[app][screen]") {
    ScreenPolicy policy({game("a", "a.mp4"), game("b", "b.mp4", 40)});
    REQUIRE(policy.targetId() == "b");
    static_cast<void>(hoverFor(policy, "a", 20, 0.0));
    REQUIRE(policy.selected());
    const std::uint64_t generation = policy.targetGeneration();
    // I4 step 6b: the CRT of another room, its own games: its start choice, nothing selected.
    policy.restart({game("c", "c.mp4"), game("d", "d.mp4", 90)});
    CHECK(policy.targetId() == "d");
    CHECK_FALSE(policy.selected());
    CHECK(policy.targetGeneration() == generation + 1);
    CHECK(policy.picture() == ScreenPicture::IdlePattern); // its picture afresh
    // A room without games: the idle pattern.
    policy.restart({});
    CHECK(policy.targetId().empty());
    CHECK(policy.picture() == ScreenPicture::IdlePattern);
    // Quitting stays quit.
    policy.quit();
    policy.restart({game("c", "c.mp4")});
    CHECK(policy.phase() == ScreenPhase::Quit);
    CHECK_FALSE(policy.wantsDecoder());
}
