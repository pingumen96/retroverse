// The hero CRT's decisions without a GPU or a decoder (I3 step 11, ADR-0012 §7): the start choice, the dwell, the
// fallback chain and the decoder's life around launches; since I5 step 1 (ADR-0016 §3, §8) the playlist after the start
// choice, the selection's hold, the title card at the chain's end.

#include <rv/app/screen_policy.hpp>
#include <rv/esde/media_resolver.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
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

// --- I5 step 1: the playlist, the selection's hold, the title card (ADR-0016 §3, §8) ---------------------------------

namespace {

using rv::app::ScreenOrigin;
using rv::app::ScreenPlaySettings;

/// The GPU case's room (tests/smoke/test_video.cpp, "the hero screen plays its playlist"): the test room's three games
/// with clips, in the mock catalog's order, and a fourth without media; 2 s a game.
std::vector<ScreenGame> playlistRoom() {
    std::vector<ScreenGame> games{game("g000000000000e001", "e001.mp4"), game("g000000000000e002", "e002.mp4"),
                                  game("g000000000000e003", "e003.mp4"), game("g000000000000e009")};
    for (ScreenGame& g : games) {
        g.card.title = "Title of " + g.id;
    }
    return games;
}

ScreenPlaySettings playlistSettings(double previewSeconds = 2.0) {
    ScreenPlaySettings settings;
    settings.previewSeconds = previewSeconds;
    settings.playlist.roomId = "m0_testroom";
    return settings;
}

using Targets = std::vector<std::pair<int, std::string>>;

/// Runs `frames` frames of free roam from frame `first` (simulated time first * kFrame), nothing hovered; returns the
/// targets as they changed: (frame, id).
Targets run(ScreenPolicy& policy, int first, int frames) {
    Targets targets;
    std::uint64_t generation = policy.targetGeneration();
    for (int frame = first; frame < first + frames; ++frame) {
        const double now = frame * kFrame;
        policy.hover(std::nullopt, now);
        policy.tick(now);
        if (policy.targetGeneration() != generation) {
            generation = policy.targetGeneration();
            targets.emplace_back(frame, policy.targetId());
        }
    }
    return targets;
}

} // namespace

TEST_CASE("app/screen_policy: the start choice, then the playlist's games, each for previewDuration",
          "[app][screen][playlist]") {
    ScreenPolicy policy(playlistRoom(), playlistSettings());
    // Nothing played yet: the first game with a clip; then the shuffle of the test room (the oracle's, as in
    // test_playlist.cpp): e002, e003, e001, then the next cycle's e002, e001, e003 (e001 just shown: never twice).
    CHECK(policy.targetId() == "g000000000000e001");
    CHECK(policy.origin() == ScreenOrigin::Start);
    // Its card is never drawn over the wait for its clip's first frame: only once the clip cannot play.
    CHECK_FALSE(policy.wantedStill().has_value());
    CHECK(policy.playlist().order() ==
          std::vector<std::string>{"g000000000000e002", "g000000000000e003", "g000000000000e001"});
    const Targets targets = run(policy, 0, 721);
    // 2 s at 1/60 s: the 120th frame after a game began (its first frame counts no time). Cycle 0 ends with e001,
    // which follows e003; cycle 1 (e002, e001, e003) begins with e002.
    const Targets expected{{120, "g000000000000e002"}, {240, "g000000000000e003"}, {360, "g000000000000e001"},
                           {480, "g000000000000e002"}, {600, "g000000000000e001"}, {720, "g000000000000e003"}};
    CHECK(targets == expected);
    CHECK(policy.origin() == ScreenOrigin::Playlist);
    CHECK(policy.selections() == 0);
}

TEST_CASE("app/screen_policy: a clip shorter than previewDuration plays to its end", "[app][screen][playlist]") {
    ScreenPolicy policy(playlistRoom(), playlistSettings(30.0));
    policy.tick(0.0);
    policy.videoShowing();
    policy.clipDuration(1.0);
    const Targets targets = run(policy, 1, 70);
    REQUIRE_FALSE(targets.empty());
    CHECK(targets.front() == std::pair<int, std::string>{60, "g000000000000e002"});
    // Its time counts from its first frame: one that took half a second to come still plays its whole second.
    ScreenPolicy late(playlistRoom(), playlistSettings(30.0));
    REQUIRE(run(late, 0, 31).empty());
    late.videoShowing();
    late.clipDuration(1.0);
    const Targets lateTargets = run(late, 31, 70);
    REQUIRE_FALSE(lateTargets.empty());
    CHECK(lateTargets.front() == std::pair<int, std::string>{90, "g000000000000e002"});
    // A room with one clip only keeps playing it (its clip loops), never restarted.
    ScreenPolicy single({game("g000000000000e001", "e001.mp4")}, playlistSettings());
    CHECK(run(single, 0, 400).empty());
    CHECK(single.targetId() == "g000000000000e001");
}

TEST_CASE("app/screen_policy: a selection interrupts the playlist and gives it back after its clip",
          "[app][screen][playlist]") {
    ScreenPolicy policy(playlistRoom(), playlistSettings());
    run(policy, 0, 150); // e002 from frame 120
    REQUIRE(policy.targetId() == "g000000000000e002");
    // e003 focused from frame 150: the selection after the dwell (frame 168), held while focused, beyond
    // previewDuration. Its clip of 1.5 s shows its first frame on that frame.
    for (int frame = 150; frame < 450; ++frame) {
        policy.hover(std::string_view("g000000000000e003"), frame * kFrame);
        policy.tick(frame * kFrame);
        if (frame == 168) {
            policy.videoShowing();
            policy.clipDuration(1.5);
        }
    }
    CHECK(policy.targetId() == "g000000000000e003");
    CHECK(policy.origin() == ScreenOrigin::Selection);
    CHECK(policy.selections() == 1);
    CHECK_FALSE(policy.holdLeft().has_value());
    // Let go 282 frames (4.70 s) after its first frame, 0.20 s into the looping clip's fourth pass, which plays to its
    // end (1.30 s, within previewDuration); then the playlist's e002 again from its start.
    const Targets back = run(policy, 450, 200);
    CHECK_FALSE(policy.holdLeft().has_value());
    REQUIRE(back.size() >= 2);
    CHECK(back.front().second == "g000000000000e002");
    CHECK(back.front().first == 450 + 78);
    // Then the playlist goes on after it: e003, 2 s later.
    CHECK(back[1].second == "g000000000000e003");
    CHECK(back[1].first - back[0].first == 120);
    CHECK(policy.origin() == ScreenOrigin::Playlist);
    CHECK_FALSE(policy.selected());
}

TEST_CASE("app/screen_policy: a selection let go holds for previewDuration without a clip, again when hovered",
          "[app][screen][playlist]") {
    ScreenPolicy policy(playlistRoom(), playlistSettings());
    // The game without media selected at frame 18 (the start choice interrupted): its card, the chain's end.
    hoverFor(policy, std::string("g000000000000e009"), 19, 0.0);
    REQUIRE(policy.targetId() == "g000000000000e009");
    CHECK(policy.wantedStill() == ScreenPicture::TitleCard);
    // Let go for 60 frames, focused again (the hold forgotten), let go again: 2 s from then, back to the start choice.
    int frame = 19;
    for (; frame < 80; ++frame) {
        policy.hover(std::nullopt, frame * kFrame);
        policy.tick(frame * kFrame);
    }
    REQUIRE(policy.holdLeft().has_value());
    CHECK(policy.targetId() == "g000000000000e009");
    policy.hover(std::string_view("g000000000000e009"), frame * kFrame);
    policy.tick(frame * kFrame);
    CHECK_FALSE(policy.holdLeft().has_value());
    ++frame;
    const Targets back = run(policy, frame, 200);
    REQUIRE_FALSE(back.empty());
    // The first frame let go sets the hold, the 120 after it count it down.
    CHECK(back.front() == std::pair<int, std::string>{frame + 120, "g000000000000e001"});
    CHECK(policy.origin() == ScreenOrigin::Start);
}

TEST_CASE("app/screen_policy: frozen runs hold the start choice, autoplay off plays selections only",
          "[app][screen][playlist]") {
    ScreenPlaySettings frozen = playlistSettings();
    frozen.frozen = true;
    ScreenPolicy held(playlistRoom(), frozen);
    CHECK(run(held, 0, 600).empty());
    CHECK(held.targetId() == "g000000000000e001");

    ScreenPlaySettings selectionsOnly = playlistSettings();
    selectionsOnly.autoplay = false;
    ScreenPolicy policy(playlistRoom(), selectionsOnly);
    CHECK(policy.targetId().empty());
    CHECK(policy.picture() == ScreenPicture::IdlePattern);
    CHECK(run(policy, 0, 300).empty());
    hoverFor(policy, std::string("g000000000000e003"), 19, 300 * kFrame);
    CHECK(policy.targetId() == "g000000000000e003");
    const Targets back = run(policy, 319, 200);
    REQUIRE(back.size() == 1);
    CHECK(back.front().second.empty()); // after its turn, no game again
    CHECK(policy.origin() == ScreenOrigin::None);

    ScreenPlaySettings noFocus = playlistSettings();
    noFocus.showOnFocus = false;
    ScreenPolicy ignoring(playlistRoom(), noFocus);
    hoverFor(ignoring, std::string("g000000000000e003"), 60, 0.0);
    CHECK(ignoring.targetId() == "g000000000000e001");
    CHECK(ignoring.selections() == 0);
}

TEST_CASE("app/screen_policy: out of view or during a game the item's clock stops", "[app][screen][playlist]") {
    ScreenPolicy policy(playlistRoom(), playlistSettings());
    run(policy, 0, 60);
    policy.setVisible(false);
    CHECK(run(policy, 60, 300).empty());
    policy.setVisible(true);
    policy.launchStarting(std::nullopt);
    CHECK(run(policy, 360, 300).empty());
    policy.launchEnded();
    const Targets targets = run(policy, 660, 100);
    REQUIRE_FALSE(targets.empty());
    // 59 frames counted before (frame 0 counts none), 61 after.
    CHECK(targets.front() == std::pair<int, std::string>{720, "g000000000000e002"});
}

TEST_CASE("app/screen_policy: a clip that failed leaves the playlist for the run, its card at the chain's end",
          "[app][screen][playlist]") {
    std::vector<ScreenGame> games = playlistRoom();
    games[1] = withStills(games[1], false, false, true);
    ScreenPolicy policy(games, playlistSettings());
    run(policy, 0, 121);
    REQUIRE(policy.targetId() == "g000000000000e002");
    CHECK(policy.wantedStill() == ScreenPicture::Cover);
    policy.videoFailed();
    CHECK_FALSE(policy.hasVideo());
    CHECK(policy.playlist().excluded().contains("g000000000000e002"));
    policy.stillFailed(); // the cover cannot be read: the card
    CHECK(policy.wantedStill() == ScreenPicture::TitleCard);
    CHECK(rv::app::mediaPath(policy.targetMedia(), ScreenPicture::TitleCard) == nullptr);
    policy.stillReady();
    CHECK(policy.picture() == ScreenPicture::TitleCard);
    // Never again in the run: not in the next cycles, nor after the screen is bound again.
    for (const auto& [frame, id] : run(policy, 121, 1200)) {
        CHECK(id != "g000000000000e002");
    }
    policy.restart(games);
    CHECK(policy.playlist().excluded().contains("g000000000000e002"));
    CHECK_FALSE(policy.playlist().order().empty());
    // Clips off (video.json's "enabled": false): the playlist goes on with the games' stills and cards.
    ScreenPlaySettings stills = playlistSettings();
    stills.clips = false;
    ScreenPolicy noClips(games, stills);
    CHECK_FALSE(noClips.hasVideo());
    CHECK(noClips.wantedStill() == ScreenPicture::TitleCard);
    CHECK(run(noClips, 0, 121).size() == 1);
}

TEST_CASE("app/screen_policy: unknown explicit ids are skipped", "[app][screen][playlist]") {
    ScreenPlaySettings settings = playlistSettings();
    settings.playlist.mode = rv::app::PlaylistMode::Explicit;
    settings.playlist.explicitGames = {"g000000000000e003", "g00000000000fffff", "g000000000000e009",
                                       "g000000000000e001"};
    ScreenPolicy policy(playlistRoom(), settings);
    CHECK(policy.playlist().mode() == rv::app::PlaylistMode::Explicit);
    CHECK(policy.playlist().order() == std::vector<std::string>{"g000000000000e003", "g000000000000e001"});
    CHECK(policy.playlist().unknownIds() == std::vector<std::string>{"g00000000000fffff"});
    CHECK(policy.playlist().unplayableIds() == std::vector<std::string>{"g000000000000e009"});
    // The start choice (e001), then the list: e003, e001, e003, ...
    const Targets expected{{120, "g000000000000e003"}, {240, "g000000000000e001"}, {360, "g000000000000e003"}};
    CHECK(run(policy, 0, 361) == expected);
}

TEST_CASE("app/screen_policy: a room without a clip shows its first game's stills, its card last",
          "[app][screen][playlist]") {
    using rv::app::chooseStillGame;
    // The first start candidate with a still or a card, in the room's order; none without either.
    ScreenGame bare = game("g000000000000f001");
    ScreenGame carded = game("g000000000000f002");
    carded.card.title = "Lanterna";
    ScreenGame covered = withStills(game("g000000000000f003"), false, false, true);
    ScreenGame unplaced = withStills(game("g000000000000f004"), true, false, false);
    unplaced.startCandidate = false;
    CHECK_FALSE(chooseStillGame({}).has_value());
    CHECK_FALSE(chooseStillGame(std::vector<ScreenGame>{bare}).has_value());
    CHECK(chooseStillGame(std::vector<ScreenGame>{bare, unplaced, covered, carded}) == 2u);
    CHECK(chooseStillGame(std::vector<ScreenGame>{carded, covered}) == 0u);

    ScreenPolicy policy({bare, covered, carded}, playlistSettings());
    CHECK(policy.targetId() == "g000000000000f003");
    CHECK(policy.origin() == ScreenOrigin::Start);
    CHECK_FALSE(policy.hasVideo());
    CHECK(policy.wantedStill() == ScreenPicture::Cover);
    CHECK(policy.playlist().empty());
    // No playlist to go on with: the still stays.
    CHECK(run(policy, 0, 600).empty());
    // A clip in the room wins over any still.
    ScreenPolicy withClip({covered, game("g000000000000f005", "f005.mp4")}, playlistSettings());
    CHECK(withClip.targetId() == "g000000000000f005");
    // Nothing to show at all: the idle pattern.
    ScreenPolicy nothing({bare}, playlistSettings());
    CHECK(nothing.targetId().empty());
    CHECK(nothing.picture() == ScreenPicture::IdlePattern);
}

TEST_CASE("app/screen_policy: a selection let go gives the screen back without any clip to play",
          "[app][screen][playlist]") {
    ScreenGame covered = withStills(game("g000000000000f003"), false, false, true);
    ScreenGame carded = game("g000000000000f002");
    carded.card.title = "Lanterna";
    // Selects `id` from frame `first` (the dwell's 19 frames, the policy's clock running), lets go, and runs 200
    // frames more; the targets as they changed after the selection.
    const auto selectAndLetGo = [](ScreenPolicy& policy, const std::string& id, int first) {
        for (int frame = first; frame < first + 19; ++frame) {
            policy.hover(std::string_view(id), frame * kFrame);
            policy.tick(frame * kFrame);
        }
        REQUIRE(policy.targetId() == id);
        REQUIRE(policy.origin() == ScreenOrigin::Selection);
        return run(policy, first + 19, 200);
    };

    SECTION("a room without a clip: back to its start choice after the hold") {
        ScreenPolicy policy({covered, carded}, playlistSettings());
        REQUIRE(policy.targetId() == "g000000000000f003");
        REQUIRE(policy.playlist().empty());
        const Targets back = selectAndLetGo(policy, "g000000000000f002", 0);
        // The first frame let go sets the hold (2 s, no clip), the 120 after it count it down.
        CHECK(back == Targets{{19 + 120, "g000000000000f003"}});
        CHECK(policy.origin() == ScreenOrigin::Start);
        CHECK_FALSE(policy.selected());
        CHECK(policy.wantedStill() == ScreenPicture::Cover);
    }
    SECTION("a room without a clip, its start choice selected: it plays on as the start choice") {
        ScreenPolicy policy({covered, carded}, playlistSettings());
        const std::uint64_t generation = policy.targetGeneration();
        const Targets back = selectAndLetGo(policy, "g000000000000f003", 0);
        CHECK(back.empty());
        CHECK(policy.targetGeneration() == generation);
        CHECK(policy.targetId() == "g000000000000f003");
        CHECK(policy.origin() == ScreenOrigin::Start);
        CHECK_FALSE(policy.selected());
    }
    SECTION("every clip failed: back to the game interrupted, its stills") {
        ScreenPolicy policy({withStills(game("g000000000000e001", "e001.mp4"), false, true, false), carded},
                            playlistSettings());
        REQUIRE(policy.targetId() == "g000000000000e001");
        policy.videoFailed(); // the room's only clip: the playlist has nothing to give from now on
        REQUIRE(policy.playlist().excluded().contains("g000000000000e001"));
        const Targets back = selectAndLetGo(policy, "g000000000000f002", 0);
        CHECK(back == Targets{{19 + 120, "g000000000000e001"}});
        CHECK(policy.origin() == ScreenOrigin::Start);
        CHECK_FALSE(policy.hasVideo());
        CHECK(policy.wantedStill() == ScreenPicture::TitleScreen);
        // The failed start choice selected and let go: it stays, as the start choice.
        const Targets again = selectAndLetGo(policy, "g000000000000e001", 300);
        CHECK(again.empty());
        CHECK(policy.origin() == ScreenOrigin::Start);
        CHECK_FALSE(policy.selected());
    }
}
