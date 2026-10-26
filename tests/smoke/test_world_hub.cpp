// The hub in the app (I6 step 5; ADR-0014 §3, ADR-0013 §10): the building fixture (tests/fixtures/esde/building)
// recorded as the library of the case's data directory, imported and planned under the shipped threshold
// (building_support.hpp), then retroverse.exe lockstep and hidden at the ground floor's arrival point; poses and paths
// derived from the plan and the hub's layout (floor_layout.hpp), never written here:
//   - the directory's totem: the walker walks to its zone and turns to its screen, the prompt ("Indice") shows, interact
//     opens the directory at its floors (`via` "totem");
//   - the favourites' showcase: its proxies are the catalog's favourite and recent games in the directory's order (the
//     first twelve of its list), their fronts resident; interact on the first one opens the directory at its card;
//     its Avvia launches it into the fake emulator (RV_LAUNCH_ALLOWLIST), and on its return the showcase is made again;
//     a refresh that keeps the floor makes it again in place (a favourite marked since), the registries as they were;
//   - the arcade room reached from the elevator past the landmark (tests/input_scripts/hub_walk_into_arcade.json): the
//     eye in the arcade room at the end.
// The screenshots stay in build/<preset>/smoke/world_hub for a look.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>

#include "building_support.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr double kStepMeters = 2.5 * 0.016667;
constexpr double kRadiansPerPixel = 0.0022;

std::vector<std::string> hubArguments(std::uint32_t frames, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--exit-after-frames", "--frames", std::to_string(frames), "--fixed-dt",
                                       "0.016667"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

rv::world::FloorSceneBuild groundOf(const rv::world::WorldPlan& plan, const fs::path& assetRoot) {
    auto data = rv::world::loadFloorSceneData(assetRoot, plan, rv::world::kGroundFloor);
    REQUIRE(data.has_value());
    auto build = rv::world::toFloorSceneFile(plan, rv::world::kGroundFloor, *data);
    REQUIRE(build.has_value());
    REQUIRE(build->hall.hub.has_value());
    return std::move(*build);
}

float meters(std::int64_t millimetres) {
    return static_cast<float>(millimetres) / 1000.0f;
}

/// An input script's turn of `degrees` (positive: right) over `frames` frames from `from`.
Json turn(std::uint64_t from, std::uint64_t frames, double degrees) {
    const double pixels = rv::core::degreesToRadians(static_cast<float>(degrees)) / kRadiansPerPixel /
                          static_cast<double>(frames);
    return Json{{"fromFrame", from}, {"toFrame", from + frames - 1}, {"actions", {{"look_delta_x", pixels}}}};
}

std::uint64_t walkFrames(double distance) {
    return static_cast<std::uint64_t>(std::llround(std::abs(distance) / kStepMeters));
}

Json walk(std::uint64_t from, std::uint64_t frames) {
    return Json{{"fromFrame", from}, {"toFrame", from + frames - 1}, {"actions", {{"move_forward", 1.0}}}};
}

Json press(std::uint64_t frame, std::string_view action) {
    return Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}};
}

/// emulators.json's "fake-return" profile (the fake emulator of this build, 2.5 s, background) assigned to every system of
/// the building fixture that has a room.
void assignFakeEmulator(const PlannedBuilding& building) {
    const std::vector<std::string> systems{"mame", "snes", "psx", "gb", "pokemini", "amiga", "pocketlab"};
    Json served = Json::object();
    for (const std::string& system : systems) {
        served[system] = Json::object();
    }
    const Json profile{{"id", "fake-return"},
                       {"executable", arg(fakeEmulatorExe())},
                       {"arguments", {"--sleep-ms", "2500", "--exit-timeout-ms", "30000", "--", "{rom}"}},
                       {"windowTimeoutSeconds", 0},
                       {"hideConsoleWindow", true},
                       {"systems", served}};
    const Json document{{"schemaVersion", 1}, {"profiles", Json::array({profile})}};
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "config" / "emulators.json", document.dump(2) + "\n")
                .has_value());
    for (const std::string& system : systems) {
        building.cli("assign_" + system, {"assign", system, "--profile", "fake-return"});
    }
}

/// The pose before the showcase's slot `index` with its face's middle in the reticle.
rv::app::SpawnPoint poseBefore(const rv::world::HubShowcaseSlots& slots, std::size_t index) {
    const glm::vec3 slot(slots.frames[index][3]);
    const glm::vec3 face = slot + glm::vec3(slots.frames[index][1]) * (slots.size.y / 2.0f);
    const float standZ = face.z - 1.1f;
    const float eye = 1.65f;
    rv::app::SpawnPoint pose;
    pose.feet = glm::vec3(face.x, 0.0f, standZ);
    pose.yawDeg = 180.0f;
    pose.pitchDeg = -rv::core::radiansToDegrees(std::atan2(eye - face.y, face.z - standZ));
    return pose;
}

/// The heading (yaw, degrees: 0 looks at -Z, positive turns towards +X) from `from` to `to` on the floor.
double headingOf(const glm::vec2& from, const glm::vec2& to) {
    return rv::core::radiansToDegrees(std::atan2(to.x - from.x, -(to.y - from.y)));
}

double wrapped(double degrees) {
    while (degrees > 180.0) {
        degrees -= 360.0;
    }
    while (degrees <= -180.0) {
        degrees += 360.0;
    }
    return degrees;
}

} // namespace

TEST_CASE("world hub: the directory's totem: its prompt shows before it, interact opens the directory at its floors",
          "[gpu][world][world_hub]") {
    const PlannedBuilding building("world_hub/totem", false);
    const rv::world::StoredPlan stored = building.plan();
    const AppRun first = runApp(retroverseExe(), building.dir, "first", hubArguments(2));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const rv::world::FloorSceneBuild ground = groundOf(stored.plan, first.report->assetRoot);
    const rv::world::Hub& hub = *ground.hall.hub;
    // From the arrival point (facing east) to the middle of the totem's zone, then turned to its screen (east): the
    // prompt shows; interact opens the directory.
    const glm::vec2 arrival(meters(ground.hall.arrival.xMm), meters(ground.hall.arrival.zMm));
    const glm::vec2 zone(meters(hub.totemZone.minXMm + hub.totemZone.maxXMm) / 2.0f,
                         meters(hub.totemZone.minZMm + hub.totemZone.maxZMm) / 2.0f);
    const glm::vec2 screen(meters(hub.totemFace.xMm), meters(hub.totemFace.zMm));
    Json script = Json::array();
    std::uint64_t frame = 1;
    double yaw = 90.0;
    const double toZone = headingOf(arrival, zone);
    script.push_back(turn(frame, 10, wrapped(toZone - yaw)));
    frame += 10;
    yaw = toZone;
    const std::uint64_t along = walkFrames(glm::length(zone - arrival));
    script.push_back(walk(frame, along));
    frame += along;
    const double toScreen = headingOf(zone, screen);
    script.push_back(turn(frame, 10, wrapped(toScreen - yaw)));
    frame += 10;
    const std::uint64_t interactAt = frame + 6;
    script.push_back(press(interactAt, "interact"));
    const std::uint64_t frames = interactAt + 10;
    const fs::path scriptFile = building.dir / "totem.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path shot = building.dir / "totem_directory.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "totem",
                              hubArguments(static_cast<std::uint32_t>(frames),
                                           {"--input-script", arg(scriptFile), "--screenshot", arg(shot)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    CHECK(report.world.floor == rv::world::kGroundFloor);
    CHECK(report.world.totemPromptFrames >= 5);
    CHECK(report.world.elevatorPromptFrames == 0);
    REQUIRE(report.directory.bound);
    CHECK(report.directory.opened == 1);
    CHECK(report.directory.open);
    CHECK(report.directory.tab == "floors");
    const auto opened = std::find_if(report.directory.events.begin(), report.directory.events.end(),
                                     [](const rv::app::DirectoryEventRecord& event) { return event.event == "open"; });
    REQUIRE(opened != report.directory.events.end());
    CHECK(opened->id == "floors");
    CHECK(opened->detail == "totem");
    CHECK(opened->frame == interactAt);
}

TEST_CASE("world hub: the favourites' showcase holds the catalog's favourite and recent games; a proxy opens its card",
          "[gpu][world][world_hub]") {
    const PlannedBuilding building("world_hub/showcase", false);
    const rv::world::StoredPlan stored = building.plan();
    const AppRun first = runApp(retroverseExe(), building.dir, "first", hubArguments(2));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const rv::world::FloorSceneBuild ground = groundOf(stored.plan, first.report->assetRoot);
    const rv::world::HubShowcaseSlots slots = rv::world::hubShowcaseSlots(ground.hall);
    REQUIRE(slots.frames.size() == rv::world::kHubShowcaseSlots);
    // Its proxies: the first twelve of the directory's list, every front resident (lockstep: the frame waits for them).
    REQUIRE(first.report->world.showcase.bound);
    CHECK(first.report->world.showcase.builds == 1);
    const std::vector<std::string> games = first.report->world.showcase.games;
    REQUIRE_FALSE(games.empty());
    CHECK(games.size() <= rv::world::kHubShowcaseSlots);
    CHECK(first.report->world.showcase.frontsResident == games.size());

    // Before the showcase, the first proxy in the reticle (its face's middle), interact: the directory at its card.
    const rv::app::SpawnPoint pose = poseBefore(slots, 0);
    Json script = Json::array();
    script.push_back(press(8, "interact"));
    const fs::path scriptFile = building.dir / "showcase.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path hovered = building.dir / "showcase_hover.png";
    const AppRun hover = runApp(retroverseExe(), building.dir, "hover",
                                hubArguments(6, {"--spawn", spawnArgument(pose), "--screenshot", arg(hovered)}));
    requireCleanRun(hover);
    CHECK(hover.report->world.showcase.hovered == games.front());
    const fs::path shot = building.dir / "showcase_card.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "card",
                              hubArguments(40, {"--spawn", spawnArgument(pose), "--input-script", arg(scriptFile),
                                                "--screenshot", arg(shot)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    CHECK(report.world.showcase.cardsOpened == 1);
    CHECK(report.world.showcase.lastCard == games.front());
    REQUIRE(report.directory.bound);
    CHECK(report.directory.open);
    CHECK(report.directory.view == "card");
    CHECK(report.directory.cardGame == games.front());
    // The list behind the card is the catalog's favourite and recent games: the showcase's are its first.
    REQUIRE(report.directory.games.size() >= games.size());
    for (std::size_t i = 0; i < games.size(); ++i) {
        CHECK(report.directory.games[i].gameId == games[i]);
    }
}

TEST_CASE("world hub: the arcade room reached from the elevator past the landmark", "[gpu][world][world_hub]") {
    const PlannedBuilding building("world_hub/arcade", false);
    const fs::path script = sourceDir() / "tests" / "input_scripts" / "hub_walk_into_arcade.json";
    const fs::path shot = building.dir / "in_arcade.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "into_arcade",
                              hubArguments(345, {"--input-script", arg(script), "--screenshot", arg(shot)}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.sceneAreas.has_value());
    CHECK(report.sceneAreas->area == building.roomOf("mame"));
    CHECK(report.world.room == building.roomOf("mame"));
}

TEST_CASE("world hub: a game launched from a proxy's card returns and the showcase is made again",
          "[gpu][world][world_hub]") {
    ensureLaunchAllowList();
    const PlannedBuilding building("world_hub/showcase_launch", false);
    assignFakeEmulator(building);
    const rv::world::StoredPlan stored = building.plan();
    const AppRun first = runApp(retroverseExe(), building.dir, "first", hubArguments(2));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    REQUIRE(first.report->world.showcase.bound);
    const std::vector<std::string> games = first.report->world.showcase.games;
    REQUIRE_FALSE(games.empty());
    const rv::world::FloorSceneBuild ground = groundOf(stored.plan, first.report->assetRoot);
    const rv::world::HubShowcaseSlots slots = rv::world::hubShowcaseSlots(ground.hall);
    REQUIRE(slots.frames.size() == rv::world::kHubShowcaseSlots);

    // The first proxy's card, Avvia (no frame while the fake runs); on its return the showcase is made again from the
    // catalog: the game just played still first (a favourite or not, the most recently played of its group).
    Json script = Json::array();
    script.push_back(press(8, "interact"));
    script.push_back(press(12, "ui_activate")); // Avvia
    const fs::path scriptFile = building.dir / "launch_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const AppRun run = runApp(retroverseExe(), building.dir, "launch",
                              hubArguments(60, {"--spawn", spawnArgument(poseBefore(slots, 0)), "--input-script",
                                                arg(scriptFile)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    CHECK(report.world.showcase.cardsOpened == 1);
    REQUIRE(report.launches.size() == 1);
    CHECK(report.launches.front().gameId == games.front());
    CHECK(report.launches.front().outcome == "returned");
    CHECK(report.world.showcase.builds == 2);
    REQUIRE_FALSE(report.world.showcase.games.empty());
    CHECK(report.world.showcase.games.front() == games.front());
    CHECK(report.world.showcase.frontsResident == report.world.showcase.games.size());
    // I6 step 5's review: drawn once per content that differs from the one shown, in place after the first; the
    // registries never grow with a drawing.
    REQUIRE_FALSE(report.world.showcase.draws.empty());
    CHECK(report.world.showcase.draws.size() + report.world.showcase.unchanged == report.world.showcase.builds);
    CHECK_FALSE(report.world.showcase.draws.front().inPlace);
    for (const rv::app::ShowcaseDrawRecord& draw : report.world.showcase.draws) {
        CHECK(draw.meshes == report.world.showcase.draws.front().meshes);
        CHECK(draw.materials == report.world.showcase.draws.front().materials);
    }
}

TEST_CASE("world hub: a favourite marked since and Aggiorna libreria pressed: the showcase is made again in place, the "
          "registries as they were",
          "[gpu][world][world_hub]") {
    const PlannedBuilding building("world_hub/showcase_refresh", false);
    const AppRun first = runApp(retroverseExe(), building.dir, "first", hubArguments(2));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    REQUIRE(first.report->world.showcase.bound);
    const std::vector<std::string> before = first.report->world.showcase.games;
    REQUIRE_FALSE(before.empty());

    // The arcade's last game marked a favourite in its gamelist (the fixture's copy): the refresh imports it, and the
    // favourites come first in the showcase.
    const fs::path gamelist = building.fixture.path("ES-DE/gamelists/mame/gamelist.xml");
    auto text = rv::core::readTextFile(gamelist);
    REQUIRE(text.has_value());
    const std::size_t last = text->rfind("\t\t<players>");
    REQUIRE(last != std::string::npos);
    const std::size_t lineEnd = text->find('\n', last);
    REQUIRE(lineEnd != std::string::npos);
    const std::size_t gameEnd = text->find("</game>", lineEnd);
    REQUIRE(gameEnd != std::string::npos);
    REQUIRE(text->substr(lineEnd, gameEnd - lineEnd).find("<favorite>") == std::string::npos);
    text->insert(lineEnd + 1, "\t\t<favorite>true</favorite>\n");
    REQUIRE(rv::core::writeTextFileAtomic(gamelist, *text).has_value());

    // The quick catalog's "Aggiorna libreria" (tests/smoke/test_world_live_update.cpp's path) on frame 20, the floor
    // shown and its showcase drawn by then (lockstep).
    const Json script = Json::array({press(2, "quick_catalog"), press(4, "ui_home"), press(6, "ui_up"),
                                     press(8, "ui_up"), press(10, "ui_up"), press(12, "ui_up"),
                                     press(20, "ui_activate"), press(24, "quick_catalog")});
    const fs::path scriptFile = building.dir / "aggiorna.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const AppRun run =
        runApp(retroverseExe(), building.dir, "refresh", hubArguments(240, {"--input-script", arg(scriptFile)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.catalog.has_value());
    REQUIRE(report.catalog->refreshes.size() == 1);
    CHECK(report.catalog->refreshes.front().outcome == "done");
    // The floor kept (no rebuild): the showcase alone is made again.
    REQUIRE(report.world.liveUpdates.size() == 1);
    CHECK(report.world.liveUpdates.front().kind != "rebuild");
    const rv::app::ShowcaseRecord& showcase = report.world.showcase;
    CHECK(showcase.builds == 2);
    CHECK(showcase.unchanged == 0);
    CHECK(showcase.games != before);
    CHECK(showcase.frontsResident == showcase.games.size());
    REQUIRE(showcase.draws.size() == 2);
    CHECK_FALSE(showcase.draws[0].inPlace);
    CHECK(showcase.draws[1].inPlace);
    CHECK(showcase.draws[1].meshes == showcase.draws[0].meshes);
    CHECK(showcase.draws[1].materials == showcase.draws[0].materials);
}
