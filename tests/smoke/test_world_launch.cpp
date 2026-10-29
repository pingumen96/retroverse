// Launching from the world and "Mostra nella stanza" (I3 step 15a; ADR-0012 §6, §8): the snes-room fixture
// (tests/fixtures/esde/snes-room: 50 invented titles, every one face-out) imported into the case's data directory with
// an emulators.json whose "fake-return" profile runs the fake emulator of this build (about 2.5 s), assigned to snes;
// retroverse.exe without --scene, lockstep (--fixed-dt), hidden, the automation guard listing the fake alone
// (RV_LAUNCH_ALLOWLIST) and every launch in the background (RV_LAUNCH_BACKGROUND, main.cpp).
//  - Avvia on the inspected item's card (world_launch.json from the Focus pose of Amber Lantern, which has a clip the
//    hero CRT plays while it is inspected; the secondary TV's autoplay off): the launch resolved on the catalog worker,
//    Returned, its play session recorded and verified; the walker's pose and Inspect of the same item restored, the
//    card shown again; the hero CRT's decoder stopped with its thread ended before the hand-over and opened again
//    after the return, at the clip's position; the cover loader paused, no frame rendered while the game ran.
//  - The same launch on the product's video settings (I5 step 3's review): the hero CRT's and the secondary TV's
//    decoders both stopped before the hand-over, and the TV's clip open again where it stopped once in view.
//  - "Mostra nella stanza" on the quick catalog's card of a placed game (world_show_in_room.json: Acorn Errands): the
//    catalog closed, Focus of the game's item, the walker at its Focus pose; and the card of a game the room leaves out
//    (world_show_unplaced.json: Thistle Trellis, no cover): its reason, no button.
// Helpers repeat test_world_card.cpp's (see docs/i3/step-15a.md, "For the merge").

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// video.json with the secondary TVs' autoplay off (I5 step 3).
constexpr std::string_view kSecondaryOff = "{\"secondary\": {\"enabled\": false}}\n";

/// The snes-room fixture recorded as the library of <caseDir>/data and imported, the fake emulator's "fake-return"
/// profile assigned to snes (as test_catalog.cpp assigns it to gb).
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    /// `secondaryOff`: video.json with the secondary TVs' autoplay off; else the product's video settings.
    explicit ImportedRoom(std::string_view name, bool secondaryOff = true)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("world_launch/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        // The cases that follow the hero CRT alone, on its own decoder: the room's secondary TV (I5 step 3) is left to
        // selections (video.json's secondary autoplay off), so that it never opens a second one.
        if (secondaryOff) {
            REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "video.json", kSecondaryOff).has_value());
        }
        const nlohmann::json profile{{"id", "fake-return"},
                                     {"executable", arg(fakeEmulatorExe())},
                                     {"arguments", {"--sleep-ms", "2500", "--exit-timeout-ms", "30000", "--", "{rom}"}},
                                     {"windowTimeoutSeconds", 0},
                                     {"hideConsoleWindow", true},
                                     {"systems", {{"snes", nlohmann::json::object()}}}};
        const nlohmann::json document{{"schemaVersion", 1}, {"profiles", nlohmann::json::array({profile})}};
        REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "emulators.json", document.dump(2) + "\n").has_value());
        cli("assign", {"assign", "snes", "--profile", "fake-return"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    /// The GameId of the snes game whose ROM is `relativePath`.
    [[nodiscard]] rv::catalog::GameId gameAt(std::string_view relativePath) const {
        auto reader = openReader();
        auto statement =
            reader.prepare("SELECT id FROM rv_game_identities WHERE system = 'snes' AND relative_path = ?1").value();
        statement.bindText(1, relativePath);
        REQUIRE(statement.step().value());
        return rv::catalog::GameId::fromValue(static_cast<std::uint64_t>(statement.columnInt(0))).value();
    }

    /// The play sessions of `game`, newest first.
    [[nodiscard]] std::vector<rv::catalog_db::ListedSession> sessionsOf(rv::catalog::GameId game) const {
        auto reader = openReader();
        auto store = rv::catalog_db::SessionStore::open(reader);
        REQUIRE(store.has_value());
        rv::catalog_db::SessionFilter filter;
        filter.game = game;
        auto sessions = (*store)->sessions(filter);
        REQUIRE(sessions.has_value());
        return std::move(*sessions);
    }

private:
    [[nodiscard]] rv::catalog_db::Connection openReader() const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(data / "database" / "retroverse.sqlite", options);
        REQUIRE(database.has_value());
        auto reader = database->openReader();
        REQUIRE(reader.has_value());
        return std::move(*reader);
    }
};

std::vector<std::string> roomArguments(std::uint32_t frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{
        "--start-room", "room:snes", "--exit-after-frames", "--frames", std::to_string(frames),
        "--fixed-dt",   "0.016667",  "--screenshot",        arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

struct ShownRoom {
    rv::scene::CameraSetup camera;
    rv::world::CollectionLayout layout;
    /// The room's secondary TV (I5 step 3): its screen's id and its bounds' middle (the screen service's distance and
    /// view test read its bounds).
    std::string secondaryScreen;
    glm::vec3 secondary{0.0f};
};

ShownRoom shownRoom(const ImportedRoom& room, const rv::app::RunReport& report) {
    const fs::path plan =
        rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan;
    auto stored = rv::world::readStoredPlan(plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    ShownRoom shown{scene->camera, std::move(*layout), {}, glm::vec3(0.0f)};
    auto tvs = rv::world::secondaryTvFixtures(stored->plan, roomId, *data);
    REQUIRE(tvs.has_value());
    if (!tvs->empty()) {
        const rv::scene::SceneObject* object = scene->findObject(tvs->front().localText());
        REQUIRE(object != nullptr);
        auto prefab = rv::geometry::buildPrefab(object->kind, object->params);
        REQUIRE(prefab.has_value());
        shown.secondaryScreen = report.world.room + "/" + tvs->front().localText();
        shown.secondary = glm::vec3(object->instanceTransforms().front() * glm::vec4(prefab->bounds.center(), 1.0f));
    }
    return shown;
}

const rv::world::CollectionItem& itemOf(const ShownRoom& room, const rv::catalog::GameId& game) {
    const rv::world::CollectionItem* item = room.layout.findGame(game);
    INFO("the item of " << game.toString());
    REQUIRE(item != nullptr);
    return *item;
}

rv::scene::WalkerPose focusPose(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = kWindowSize.x / kWindowSize.y;
    return rv::app::focusPoseFor(item, viewer);
}

rv::app::SpawnPoint spawnOf(const rv::scene::WalkerPose& pose) {
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    return spawn;
}

const rv::app::RunReport& requireRoom(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.state == "room");
    REQUIRE(run.report->world.interaction.bound);
    REQUIRE(run.report->world.launch.bound);
    CHECK(run.report->world.launch.eventsDropped == 0);
    CHECK(run.report->world.interaction.staticDrawnWhilePulledFrames == 0);
    return *run.report;
}

std::vector<std::string> launchEvents(const rv::app::RoomLaunchRecord& record) {
    std::vector<std::string> events;
    for (const rv::app::RoomLaunchEventRecord& event : record.events) {
        events.push_back(std::to_string(event.frame) + ":" + event.event);
    }
    return events;
}

/// The run's first plan (2 frames): the room shown, for the poses and items of the cases.
ShownRoom firstRun(const ImportedRoom& room) {
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    return shownRoom(room, *first.report);
}

} // namespace

TEST_CASE("world launch: Avvia on the card launches the game and the room comes back as it was, the hero CRT's "
          "decoder stopped and reopened",
          "[gpu][world][world_launch]") {
    ensureLaunchAllowList();
    ImportedRoom room("avvia");
    const ShownRoom shown = firstRun(room);
    // Amber Lantern has a clip: the hero CRT plays it while the item is inspected (I3 step 13b), so a decoder runs at
    // Avvia.
    const rv::world::CollectionItem& item = itemOf(shown, room.gameAt("Amber Lantern (USA).sfc"));
    const std::string game = item.game.toString();
    const fs::path shot = room.dir / "returned.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "avvia",
                              roomArguments(120, shot,
                                            {"--spawn", spawnArgument(spawnOf(focusPose(item, shown.camera))),
                                             "--input-script", arg(inputScript("world_launch.json"))}));
    const rv::app::RunReport& report = requireRoom(run);
    INFO(run.describe());
    requireScreenshot(run, shot, {1920, 1080});

    // The launch: the product UI's, resolved on the catalog worker (the request the frame after Avvia), Returned.
    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.gameId == game);
    CHECK(launch.profile == "fake-return");
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "returned");
    CHECK(launch.exitCode == 0u);
    CHECK(launch.requestFrame == 81);
    CHECK(launch.framesWhileRunning == 0);
    REQUIRE(launch.runMs.has_value());
    CHECK(*launch.runMs >= 2400.0);
    // The walker before and after it: the same.
    REQUIRE(launch.before.has_value());
    REQUIRE(launch.after.has_value());
    REQUIRE(launch.before->camera.has_value());
    CHECK(launch.after->camera == launch.before->camera);
    // The play session, recorded on the worker and verified.
    REQUIRE(launch.sessionId.has_value());
    CHECK(launch.sessionError.empty());
    CHECK(launch.verified);
    const auto sessions = room.sessionsOf(item.game);
    REQUIRE(sessions.size() == 1);
    const rv::catalog::PlaySession& session = sessions.front().session;
    CHECK(session.id == *launch.sessionId);
    CHECK(session.outcome == rv::catalog::PlaySessionOutcome::Returned);
    CHECK(session.choice.value == "fake-return");
    CHECK(rv::catalog::returnedNormally(session));

    // The room's side: asked at Avvia, ended returned, Inspect of the same item restored with the walker's pose.
    const rv::app::RoomLaunchRecord& world = report.world.launch;
    CHECK(world.launches == 1);
    CHECK(world.returned == 1);
    CHECK(world.notReturned == 0);
    const std::vector<std::string> events = launchEvents(world);
    REQUIRE(events.size() == 3);
    CHECK(events[0] == "80:launch");
    CHECK(events[1].ends_with(":ended"));
    CHECK(events[2].ends_with(":restored"));
    CHECK(world.events[1].frame == world.events[2].frame);
    CHECK(world.events[0].game == game);
    CHECK(world.events[1].outcome == "returned");
    CHECK(world.events[2].state == "inspect");
    CHECK(world.events[2].pickId == world.events[0].pickId);
    CHECK(world.poseRestored);
    CHECK(world.walkerAfter == world.walkerBefore);
    CHECK(world.walkerBefore == launch.before->camera);
    CHECK(report.world.interaction.state == "inspect");
    CHECK(report.world.interaction.focusedGame == game);
    CHECK(report.finalCamera == launch.before->camera);
    // The card: hidden while the game ran, shown again after the return on the same game.
    std::vector<std::uint64_t> shownFrames;
    std::vector<std::uint64_t> hiddenFrames;
    for (const rv::app::CardEventRecord& event : report.world.card.events) {
        if (event.event == "shown") {
            shownFrames.push_back(event.frame);
        } else if (event.event == "hidden") {
            hiddenFrames.push_back(event.frame);
        }
    }
    REQUIRE(shownFrames.size() == 2);
    CHECK(shownFrames[0] == 40);
    CHECK(shownFrames[1] == world.events[2].frame);
    CHECK(hiddenFrames == std::vector<std::uint64_t>{80});
    CHECK(report.world.card.game == game);

    // The hero CRT: its decoder stopped and its thread ended before the hand-over, opened again after the return at
    // the clip's position; never more than one decoder (the secondary TV's autoplay off here).
    const rv::app::VideoRecord& video = report.video;
    REQUIRE(video.bound);
    REQUIRE(video.launches.size() == 1);
    const rv::app::VideoLaunchRecord& stop = video.launches.front();
    WARN("decoder at the launch: ran " << stop.decoderRan << ", stopped in " << stop.stopMs << " ms, position "
                                       << stop.positionSeconds.value_or(-1.0));
    CHECK(video.gameId == game);
    CHECK(video.maxDecoderThreads <= 1);
    CHECK(stop.decoderRan);
    CHECK(stop.stopMs < 1000.0);
    CHECK(stop.threadsRunning == 0);
    CHECK(stop.reopened);
    if (stop.positionSeconds) {
        REQUIRE(stop.reopenedAtSeconds.has_value());
        CHECK(std::abs(*stop.reopenedAtSeconds - *stop.positionSeconds) < 1e-6);
    }
    // Nothing rendered (so nothing uploaded) while the game ran: its start and end frames are one. The cover loader's
    // rounds pause while a launch is on (WorldSession::coverRound); a lockstep run has every cover resident before
    // Avvia, so they had nothing to hold back (pausedRounds counts rounds with work held).
    CHECK(launch.startFrame == launch.endFrame);
    WARN("cover rounds paused: " << report.residency.pausedRounds);
}

TEST_CASE("world launch: on the product's video settings, the hero CRT's and the secondary TV's decoders both stop "
          "before the hand-over and play again after the return",
          "[gpu][world][world_launch]") {
    // I5 step 3's review: the console room's secondary TV playing (no video.json). From Amber Lantern's Focus pose
    // turned towards the TV, which opens its clip in view (an ambient screen opens a slot only in view); turned back
    // onto the item (the TV paused out of view, keeping its slot), inspected (the hero CRT plays the game's clip),
    // Avvia: both slots stop and their threads end before the hand-over. After the return Inspect comes back as it
    // was, the TV out of view and so without a slot; Indietro (Focus), back (free roam), turned towards the TV, which
    // opens its clip again where it stopped.
    ensureLaunchAllowList();
    ImportedRoom room("both_screens", false);
    const ShownRoom shown = firstRun(room);
    REQUIRE_FALSE(shown.secondaryScreen.empty());
    const rv::world::CollectionItem& item = itemOf(shown, room.gameAt("Amber Lantern (USA).sfc"));
    const std::string game = item.game.toString();
    const rv::scene::WalkerPose focus = focusPose(item, shown.camera);
    // The yaw and pitch that look at the TV from the Focus pose's eye, and the turns between them (mouse pixels).
    constexpr float kRadiansPerPixel = 0.0022f;
    const glm::vec3 to = shown.secondary - (focus.feet + glm::vec3(0.0f, 1.65f, 0.0f));
    const float tvYaw = std::atan2(to.x, -to.z);
    const float tvPitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
    float turn = focus.yaw - tvYaw;
    while (turn > std::numbers::pi_v<float>) {
        turn -= 2.0f * std::numbers::pi_v<float>;
    }
    while (turn <= -std::numbers::pi_v<float>) {
        turn += 2.0f * std::numbers::pi_v<float>;
    }
    rv::scene::WalkerPose start = focus;
    start.yaw = tvYaw;
    start.pitch = tvPitch;
    const nlohmann::json steps = nlohmann::json::array(
        {{{"fromFrame", 20}, {"toFrame", 20},
          {"actions",
           {{"look_delta_x", turn / kRadiansPerPixel},
            {"look_delta_y", (focus.pitch - tvPitch) / kRadiansPerPixel}}}},
         {{"fromFrame", 40}, {"toFrame", 40}, {"actions", {{"interact", 1}}}},
         {{"fromFrame", 80}, {"toFrame", 80}, {"actions", {{"confirm", 1}}}},
         {{"fromFrame", 110}, {"toFrame", 110}, {"actions", {{"ui_back", 1}}}},
         {{"fromFrame", 130}, {"toFrame", 130}, {"actions", {{"back", 1}}}},
         {{"fromFrame", 150}, {"toFrame", 150},
          {"actions",
           {{"look_delta_x", -turn / kRadiansPerPixel},
            {"look_delta_y", (tvPitch - focus.pitch) / kRadiansPerPixel}}}}});
    const fs::path script = room.dir / "both_screens.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, steps.dump(2) + "\n").has_value());
    const fs::path shot = room.dir / "both_screens.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "both_screens",
                              roomArguments(220, shot, {"--spawn", spawnArgument(spawnOf(start)), "--input-script",
                                                        arg(script)}));
    const rv::app::RunReport& report = requireRoom(run);
    INFO(run.describe());
    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.gameId == game);
    CHECK(launch.outcome == "returned");
    REQUIRE(launch.startFrame.has_value());
    REQUIRE(launch.endFrame.has_value());
    const rv::app::VideoRecord& video = report.video;
    REQUIRE(video.bound);
    CHECK(video.maxOpenDecoders == 2);
    CHECK(video.maxDecoderThreads <= 2);
    REQUIRE(video.launches.size() == 1);
    CHECK(video.launches.front().threadsRunning == 0);
    CHECK(video.launches.front().stopMs < 1000.0);
    // Both slots' "launch" stops before the launch's start frame (Retroverse renders nothing while the game runs).
    std::map<std::uint64_t, rv::app::VideoSlotEventRecord> launchStops;
    for (const rv::app::VideoSlotEventRecord& event : video.slotEvents) {
        if (event.action == "stop" && event.reason == "launch") {
            launchStops.emplace(event.slot, event);
        }
    }
    REQUIRE(launchStops.size() == 2);
    for (const auto& [slot, event] : launchStops) {
        INFO("slot " << slot);
        CHECK(event.frame <= *launch.startFrame);
    }
    // The TV's clip: opened in view at the start, stopped for the launch, and open again where it stopped once the
    // walker turned back towards it.
    const auto tvOpen = std::find_if(video.slotEvents.begin(), video.slotEvents.end(), [&](const auto& event) {
        return event.action == "open" && std::find(event.screens.begin(), event.screens.end(),
                                                   shown.secondaryScreen) != event.screens.end();
    });
    REQUIRE(tvOpen != video.slotEvents.end());
    CHECK(tvOpen->frame < 20);
    REQUIRE(launchStops.contains(tvOpen->slot));
    const rv::app::VideoSlotEventRecord& tvStop = launchStops.at(tvOpen->slot);
    CHECK(tvStop.gameId == tvOpen->gameId);
    const auto resumed = std::find_if(video.slotEvents.begin(), video.slotEvents.end(), [&](const auto& event) {
        return event.action == "open" && event.frame >= *launch.endFrame && event.gameId == tvStop.gameId &&
               std::find(event.screens.begin(), event.screens.end(), shown.secondaryScreen) != event.screens.end();
    });
    REQUIRE(resumed != video.slotEvents.end());
    CHECK(resumed->frame >= 150);
    CHECK(resumed->frame <= 154);
    REQUIRE(resumed->fromSeconds.has_value());
    CHECK(*resumed->fromSeconds > 0.0);
    // The hero CRT's clip opened again after the return.
    CHECK(std::any_of(video.slotEvents.begin(), video.slotEvents.end(), [&](const auto& event) {
        return event.action == "open" && event.frame >= *launch.endFrame && event.gameId == game;
    }));
    const auto secondary = std::find_if(video.screens.begin(), video.screens.end(),
                                        [&](const auto& screen) { return screen.screen == shown.secondaryScreen; });
    REQUIRE(secondary != video.screens.end());
    CHECK(secondary->screenClass == "ambient");
    CHECK(secondary->visible);
    CHECK(secondary->picture == "video");
}

TEST_CASE("world launch: Mostra nella stanza takes the walker to a placed game's item; an unplaced game's card says "
          "why",
          "[gpu][world][world_launch]") {
    ensureLaunchAllowList();
    ImportedRoom room("show_in_room");
    const ShownRoom shown = firstRun(room);
    const rv::catalog::GameId thistle = room.gameAt("Thistle Trellis (USA).sfc");
    const rv::world::CollectionItem& acorn = itemOf(shown, room.gameAt("Acorn Errands (USA).sfc"));
    CHECK(shown.layout.findGame(thistle) == nullptr);

    // A placed game: the button, the catalog closed, Focus of its item, the walker at its Focus pose.
    {
        const fs::path shot = room.dir / "show_in_room.png";
        const AppRun run =
            runApp(retroverseExe(), room.dir, "placed",
                   roomArguments(80, shot, {"--input-script", arg(inputScript("world_show_in_room.json"))}));
        const rv::app::RunReport& report = requireRoom(run);
        INFO(run.describe());
        requireScreenshot(run, shot, {1920, 1080});
        const rv::app::RoomLaunchRecord& world = report.world.launch;
        CHECK(world.placementQueries > 0);
        CHECK(world.showInRoom == 1);
        CHECK(world.showInRoomFailed == 0);
        REQUIRE(world.events.size() == 1);
        CHECK(world.events[0].event == "show-in-room");
        CHECK(world.events[0].frame == 24);
        CHECK(world.events[0].game == acorn.game.toString());
        REQUIRE(report.catalog.has_value());
        CHECK_FALSE(report.catalog->open);
        CHECK(report.launches.empty());
        CHECK(report.world.interaction.state == "focus");
        CHECK(report.world.interaction.focusedGame == acorn.game.toString());
        CHECK(report.world.interaction.walkerEaseFrames >= 30);
        // The walker at the item's Focus pose (the eased walker placed there exactly; degrees through the report).
        REQUIRE(report.finalCamera.has_value());
        const rv::app::SpawnPoint expected = spawnOf(focusPose(acorn, shown.camera));
        CHECK(glm::length(report.finalCamera->feet - expected.feet) < 1e-3f);
        CHECK(std::abs(*report.finalCamera->yawDeg - *expected.yawDeg) < 0.05f);
        CHECK(std::abs(*report.finalCamera->pitchDeg - *expected.pitchDeg) < 0.05f);
    }

    // A game the room leaves out: its reason on the card, no button (ui_down below Avvia finds nothing to press).
    {
        const fs::path shot = room.dir / "unplaced.png";
        const AppRun run =
            runApp(retroverseExe(), room.dir, "unplaced",
                   roomArguments(30, shot, {"--input-script", arg(inputScript("world_show_unplaced.json"))}));
        const rv::app::RunReport& report = requireRoom(run);
        INFO(run.describe());
        requireScreenshot(run, shot, {1920, 1080});
        const rv::app::RoomLaunchRecord& world = report.world.launch;
        REQUIRE(report.catalog.has_value());
        CHECK(report.catalog->open);
        CHECK(world.placement == "no-cover");
        CHECK(world.placementGame == thistle.toString());
        CHECK(report.catalog->selectedId == thistle.toString());
        CHECK(world.events.empty());
        CHECK(report.world.interaction.state == "free roam");
        CHECK(report.catalog->cardSqueezed.empty());
    }
}
