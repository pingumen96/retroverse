// The elevator and the directory in the app (I4 step 7b; ADR-0013 §10, §13): the building fixture
// (tests/fixtures/esde/building) recorded as the library of the case's data directory, imported and planned
// (building_support.hpp), then retroverse.exe lockstep and hidden, its rooms, floors and poses derived from the plan and
// the floors' layout (floor_layout.hpp), never written here:
//   - a ride from the ground floor to gen4 and back: the walker turned to the elevator (the prompt shows only facing
//     its doors), interact opening the directory on its floors, the hall row of the other floor chosen (ui actions),
//     the view faded out, the floor swapped in the next frame, faded in, the walker at the destination's arrival point
//     facing the hall (not as it stood); world.rides with both rides, the phase "elevator"; after each ride the GPU
//     memory the floor holds (its arrays, buffers, scene textures, procedural surfaces) the destination's (a direct run
//     of that floor's), at the end the residency and the GPU memory the ground floor's; in Release the rides within the
//     acceptable row (the first cold within 10 s, the second warm within 3 s);
//   - a room of another floor chosen in the directory (ChooseRoom): the ride, the walker at that room's doorway camera
//     in the destination's frame; the floor's other room teleported to, then the walker's own floor's hall: back at its
//     elevator's arrival point;
//   - "Mostra nella stanza" of the quick catalog's card for a game placed on another floor: the ride, then the ease to
//     the item's Focus pose (the item focused);
//   - the directory's favourites and recent games: the fixture's favourites first, then its played games, each the
//     catalog's (the gamelists' <favorite> and <lastplayed>);
//   - Avvia on the directory's card: the fake emulator launched and back (RV_LAUNCH_ALLOWLIST, RV_LAUNCH_BACKGROUND),
//     the card's outcome none; and a launch that cannot start, its outcome on the card;
//   - the directory opens in free roam only: interact on a face-out item and Map in one frame inspect it and open
//     nothing, Map in Inspect nothing, Map back in free roam the directory;
//   - the ride's fade dims every pixel alike: the view halfway through the fade-out is the view before it times the
//     fade's share, its lights too;
//   - a game played before a ride (the quick catalog's Avvia into the fake): its room's CRT on the destination starts
//     on it (the catalog's sessions now, not the items files').
// The screenshots stay in build/<preset>/smoke/world_elevator for a look.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/room_items.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/world/building.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/where_is.hpp>

#include "building_support.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The acceptable ride (docs/roadmap.md, "Acceptable performance"; ADR-0013 §13): the view back within 3 s warm and
/// 10 s cold.
constexpr double kRideWarmMs = 3000.0;
constexpr double kRideColdMs = 10000.0;
/// --fixed-dt of the runs, and the ride's fade (ADR-0013 §10: 250 ms).
constexpr double kFixedDt = 0.016667;
constexpr double kFadeSeconds = 0.25;

#if defined(NDEBUG)
constexpr bool kRelease = true;
#else
constexpr bool kRelease = false;
#endif

/// A lockstep run of `frames` frames: in the lobby, or in `startRoom`.
std::vector<std::string> elevatorArguments(std::uint32_t frames, std::string_view startRoom = {},
                                           std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--exit-after-frames", "--frames", std::to_string(frames), "--fixed-dt",
                                       "0.016667"};
    if (!startRoom.empty()) {
        arguments.insert(arguments.begin(), {"--start-room", std::string(startRoom)});
    }
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// One press of `action` on `frame`.
Json press(std::uint64_t frame, std::string_view action) {
    return Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}};
}

/// `count` presses of `action` from `frame`, three frames apart; the frame after the last.
std::uint64_t presses(Json& script, std::uint64_t frame, std::string_view action, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        script.push_back(press(frame, action));
        frame += 3;
    }
    return frame;
}

/// Half a turn of the view on frames `frame` to `frame` + 9 (10 * 142.8 mouse pixels at 0.0022 rad a pixel: 180
/// degrees, tests/input_scripts/turn_around.json); the frame after them.
std::uint64_t turnAround(Json& script, std::uint64_t frame) {
    script.push_back(Json{{"fromFrame", frame}, {"toFrame", frame + 9}, {"actions", {{"look_delta_x", 142.8}}}});
    return frame + 10;
}

/// The plan's floors in the elevator's order (the directory's rows).
std::vector<std::string> floorsInOrder(const rv::world::WorldPlan& plan) {
    std::vector<rv::world::Floor> floors = plan.floors;
    std::sort(floors.begin(), floors.end(),
              [](const rv::world::Floor& a, const rv::world::Floor& b) { return a.order < b.order; });
    std::vector<std::string> ids;
    for (const rv::world::Floor& floor : floors) {
        ids.push_back(floor.id);
    }
    return ids;
}

std::size_t indexOf(const std::vector<std::string>& ids, std::string_view id) {
    const auto found = std::find(ids.begin(), ids.end(), id);
    INFO(id);
    REQUIRE(found != ids.end());
    return static_cast<std::size_t>(found - ids.begin());
}

/// The rooms of `floor` along its hall from the elevator (the directory's rows after the hall's: by the middle of their
/// footprint on the floor, the north row first on a tie).
std::vector<std::string> roomsAlongHall(const rv::world::WorldPlan& plan, std::string_view floor) {
    std::vector<std::pair<std::int64_t, const rv::world::Room*>> rooms;
    for (const rv::world::Room& room : plan.rooms) {
        if (room.floor == floor) {
            const rv::world::Footprint footprint = rv::world::floorFootprint(room, rv::world::BuildingLayout{});
            rooms.emplace_back(footprint.minXMm + footprint.maxXMm, &room);
        }
    }
    std::sort(rooms.begin(), rooms.end(), [](const auto& a, const auto& b) {
        return std::tie(a.first, a.second->placement.yawHalfTurns, a.second->id) <
               std::tie(b.first, b.second->placement.yawHalfTurns, b.second->id);
    });
    std::vector<std::string> ids;
    for (const auto& [middle, room] : rooms) {
        ids.push_back(room->id.toString());
    }
    return ids;
}

/// Script presses that move the directory's selection from row `from` to row `to` of a list; the frame after them.
std::uint64_t moveSelection(Json& script, std::uint64_t frame, std::size_t from, std::size_t to) {
    if (to > from) {
        return presses(script, frame, "ui_down", to - from);
    }
    return presses(script, frame, "ui_up", from - to);
}

/// The frames of one ride under lockstep at --fixed-dt 0.016667: 15 to fade out, the swap in the next, the eye's area
/// checked in the one after, 15 to fade in, and a few to spare.
constexpr std::uint64_t kRideFrames = 40;

/// The floor scene of `floor` as the app makes it (its hall's arrival point, its camera).
rv::world::FloorSceneBuild floorSceneOf(const rv::world::WorldPlan& plan, std::string_view floor,
                                        const fs::path& assetRoot) {
    auto data = rv::world::loadFloorSceneData(assetRoot, plan, floor);
    REQUIRE(data.has_value());
    auto build = rv::world::toFloorSceneFile(plan, floor, *data);
    REQUIRE(build.has_value());
    return std::move(*build);
}

float meters(std::int64_t millimetres) {
    return static_cast<float>(millimetres) / 1000.0f;
}

/// The walker stands at `floor`'s arrival point in front of its elevator, facing the hall (the floor's frame).
void requireAtArrival(const rv::app::SpawnPoint& walker, const rv::world::FloorSceneBuild& floor) {
    INFO("walker at (" << walker.feet.x << ", " << walker.feet.y << ", " << walker.feet.z << ") yaw "
                       << walker.yawDeg.value_or(-999.0f));
    CHECK(std::abs(walker.feet.x - meters(floor.hall.arrival.xMm)) < 1e-3f);
    CHECK(std::abs(walker.feet.z - meters(floor.hall.arrival.zMm)) < 1e-3f);
    CHECK(std::abs(walker.feet.y) < 1e-3f);
    const float yaw = floor.sceneFile.at("camera").value("yawDeg", 0.0f);
    CHECK(std::abs(std::remainder(walker.yawDeg.value_or(0.0f) - yaw, 360.0f)) < 1e-2f);
}

/// The collection arrays' bytes at the end of a run.
std::uint64_t arrayBytesOf(const rv::app::RunReport& report) {
    return report.gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).bytes;
}

/// The GPU memory a ride left when its view came back is the destination floor's: every kind a floor holds (its
/// collection arrays, buffers, scene textures and procedural surfaces) as `direct`, a run started on that floor, holds
/// at its end; the resident floor's given back. The hero textures, the CRT's video and the render targets are the
/// moment's (an item hovered, a CRT bound), not the floor's.
void requireFloorMemory(const rv::app::RideRecord& ride, const rv::app::RunReport& direct) {
    for (const rv::gl::GpuMemoryKind kind :
         {rv::gl::GpuMemoryKind::ProceduralSurfaces, rv::gl::GpuMemoryKind::CollectionArrays,
          rv::gl::GpuMemoryKind::SceneTextures, rv::gl::GpuMemoryKind::Buffers}) {
        INFO(ride.from << " -> " << ride.to << ": " << rv::gl::toString(kind));
        CHECK(ride.bytesOf(kind) == direct.gpuMemory.usage(kind).bytes);
    }
    CHECK(ride.arrayBytes == ride.bytesOf(rv::gl::GpuMemoryKind::CollectionArrays));
}

/// The rooms with a collection of `floor` (those that place a game), sorted.
std::vector<std::string> collectionRooms(const rv::world::WorldPlan& plan, std::string_view floor) {
    std::vector<std::string> rooms;
    for (const rv::world::Room* room : rv::world::roomsOnFloor(plan, floor)) {
        if (placementsIn(plan, room->id.toString()) > 0) {
            rooms.push_back(room->id.toString());
        }
    }
    std::sort(rooms.begin(), rooms.end());
    return rooms;
}

std::vector<std::string> residentRooms(const rv::app::RunReport& report) {
    std::vector<std::string> rooms;
    for (const rv::app::RoomResidencyRecord& room : report.residency.rooms) {
        rooms.push_back(room.room);
    }
    std::sort(rooms.begin(), rooms.end());
    return rooms;
}

/// A ride's record checked: its floors and reason, its frames (lockstep: the same on every run), the view back, the
/// walker recorded.
void requireRide(const rv::app::RideRecord& ride, std::string_view from, std::string_view to, std::string_view reason) {
    INFO("ride " << ride.from << " -> " << ride.to << " (" << ride.reason << "): " << ride.outcome);
    CHECK(ride.from == from);
    CHECK(ride.to == to);
    CHECK(ride.reason == reason);
    CHECK(ride.outcome == "arrived");
    REQUIRE(ride.fadedOutFrame.has_value());
    REQUIRE(ride.swappedFrame.has_value());
    REQUIRE(ride.fadedInFrame.has_value());
    // 250 ms of fade at 60 frames a second: 15 frames each way. The floor swaps in the frame after the fade ended,
    // whenever its job answered; the eye's area is checked in the next (the frames waiting for it), then the fade in.
    CHECK(*ride.fadedOutFrame == ride.requestedFrame + 14);
    CHECK(*ride.swappedFrame == *ride.fadedOutFrame + 1);
    CHECK(*ride.fadedInFrame == *ride.swappedFrame + 16);
    CHECK(ride.builtMs.has_value());
    REQUIRE(ride.residentMs.has_value());
    REQUIRE(ride.totalMs.has_value());
    CHECK(*ride.totalMs >= *ride.builtMs);
    CHECK(ride.walker.has_value());
    CHECK(ride.arrayBytes > 0);
    CHECK(ride.gpuBytes >= ride.arrayBytes);
    // I4 step 8: the view came back once the covers in view of the room that holds the eye were resident (lockstep: the
    // frames waited for them, none pending), and the room was whole later, still on its floor at the run's end (its
    // last frame waits for the whole floor); a floor's elevator stands in its hall, no room's covers to wait for.
    if (reason == "floor") {
        CHECK_FALSE(ride.viewItems.has_value());
        CHECK_FALSE(ride.roomResidentMs.has_value());
    } else {
        REQUIRE(ride.viewItems.has_value());
        CHECK(*ride.viewItems > 0);
        CHECK(ride.viewPending == std::optional<std::uint64_t>(0));
        REQUIRE(ride.roomResidentMs.has_value());
        CHECK(*ride.roomResidentMs >= *ride.residentMs);
    }
}

/// The walker standing at `room`'s doorway camera (world::doorwayCamera(), moved onto its floor by its placement), its
/// feet on the floor.
rv::app::SpawnPoint doorwayWalker(const rv::world::WorldPlan& plan, std::string_view roomId, const fs::path& assetRoot) {
    const rv::world::Room& room = roomIn(plan, roomId);
    auto data = rv::world::loadSceneData(assetRoot, plan, room.id);
    REQUIRE(data.has_value());
    const rv::world::DoorwayCamera camera = rv::world::doorwayCamera(room, data->roomTemplate);
    const glm::vec4 eye = rv::world::roomToFloorMatrix(room.placement) *
                          glm::vec4(static_cast<float>(camera.x), static_cast<float>(camera.eyeY),
                                    static_cast<float>(camera.z), 1.0f);
    double yaw = camera.yawDeg + (room.placement.yawHalfTurns % 2 != 0 ? 180.0 : 0.0);
    while (yaw > 180.0) {
        yaw -= 360.0;
    }
    rv::app::SpawnPoint walker;
    walker.feet = glm::vec3(eye.x, eye.y - 1.65f, eye.z);
    walker.yawDeg = static_cast<float>(yaw);
    walker.pitchDeg = static_cast<float>(camera.pitchDeg);
    return walker;
}

void requireAt(const rv::app::SpawnPoint& walker, const rv::app::SpawnPoint& expected) {
    INFO("walker at (" << walker.feet.x << ", " << walker.feet.y << ", " << walker.feet.z << ") yaw "
                       << walker.yawDeg.value_or(-999.0f) << ", expected (" << expected.feet.x << ", "
                       << expected.feet.y << ", " << expected.feet.z << ") yaw " << expected.yawDeg.value_or(-999.0f));
    CHECK(glm::length(walker.feet - expected.feet) < 2e-3f);
    const float yaw = walker.yawDeg.value_or(0.0f) - expected.yawDeg.value_or(0.0f);
    CHECK(std::abs(std::remainder(yaw, 360.0f)) < 1e-2f);
}

/// A game of the fixture's gamelists: its title, a favourite, the date it was last played ("" never).
struct ListedGame {
    std::string title;
    bool favorite = false;
    std::string played;
};

std::string xmlText(std::string text) {
    for (const auto& [entity, character] : std::vector<std::pair<std::string, std::string>>{
             {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}}) {
        for (std::size_t at = text.find(entity); at != std::string::npos; at = text.find(entity, at + 1)) {
            text.replace(at, entity.size(), character);
        }
    }
    return text;
}

/// Every favourite or played game of the materialized building fixture's gamelists (ES-DE/gamelists/*/gamelist.xml).
std::vector<ListedGame> favoriteOrPlayed(const fs::path& fixtureRoot) {
    std::vector<ListedGame> games;
    const std::regex block("<game>([\\s\\S]*?)</game>");
    const std::regex name("<name>([^<]*)</name>");
    const std::regex favorite("<favorite>true</favorite>");
    const std::regex played("<lastplayed>(\\d{4})(\\d{2})(\\d{2})T\\d{6}</lastplayed>");
    for (const fs::directory_entry& system : fs::directory_iterator(fixtureRoot / "ES-DE" / "gamelists")) {
        const auto text = rv::core::readTextFile(system.path() / "gamelist.xml");
        REQUIRE(text.has_value());
        for (std::sregex_iterator it(text->begin(), text->end(), block), end; it != end; ++it) {
            const std::string entry = (*it)[1];
            std::smatch match;
            ListedGame game;
            if (std::regex_search(entry, match, name)) {
                game.title = xmlText(match[1]);
            }
            game.favorite = std::regex_search(entry, favorite);
            if (std::regex_search(entry, match, played)) {
                game.played = std::string(match[1]) + "-" + std::string(match[2]) + "-" + std::string(match[3]);
            }
            if (game.favorite || !game.played.empty()) {
                games.push_back(std::move(game));
            }
        }
    }
    return games;
}

/// The <name> of every game of the materialized building fixture's gamelists.
std::vector<std::string> gamelistNames(const fs::path& fixtureRoot) {
    std::vector<std::string> names;
    const std::regex name("<name>([^<]*)</name>");
    for (const fs::directory_entry& system : fs::directory_iterator(fixtureRoot / "ES-DE" / "gamelists")) {
        const auto text = rv::core::readTextFile(system.path() / "gamelist.xml");
        REQUIRE(text.has_value());
        for (std::sregex_iterator it(text->begin(), text->end(), name), end; it != end; ++it) {
            names.push_back(xmlText((*it)[1]));
        }
    }
    return names;
}

/// emulators.json's "fake-return" profile (the fake emulator of this build, 2.5 s, background) assigned to every
/// system of the building fixture that has a room.
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

/// The title of `game` in the room items of `room` (its first section's, or its system's), lower case: what the quick
/// catalog's search finds it by (its title unique in the fixture).
std::string searchOf(const PlannedBuilding& building, const rv::world::Room& room, const rv::catalog::GameId& game) {
    const auto items = rv::app::readRoomItems(
        rv::app::roomItemsFile(building.data / "cache", rv::catalog::LibraryId::parse("esde").value(),
                               room.sections.empty() ? room.id.system() : room.sections.front()));
    REQUIRE(items.has_value());
    const rv::app::RoomItemText* text = items->find(game);
    REQUIRE(text != nullptr);
    REQUIRE_FALSE(text->title.empty());
    std::string search = text->title;
    std::transform(search.begin(), search.end(), search.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return search;
}

/// The quick catalog's way to a game's card (world_show_in_room.json's path) from `open`: the catalog opened, the search
/// field, `search` typed, the list, the card's actions (Avvia focused); the frame of the last press.
std::uint64_t catalogCard(Json& script, std::uint64_t open, const std::string& search) {
    script.push_back(press(open, "quick_catalog"));
    script.push_back(press(open + 3, "ui_up"));
    script.push_back(press(open + 6, "ui_up"));
    script.push_back(Json{{"fromFrame", open + 9}, {"toFrame", open + 9}, {"text", search}});
    script.push_back(press(open + 12, "ui_activate"));
    script.push_back(press(open + 15, "ui_right"));
    return open + 15;
}

} // namespace

TEST_CASE("world elevator: a ride from the ground floor to gen4 and back through the elevator's prompt and the "
          "directory",
          "[gpu][world][world_elevator]") {
    ensureLaunchAllowList();
    const PlannedBuilding building("world_elevator/ride", false);
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    const std::string ground(rv::world::kGroundFloor);
    const std::string destination = "gen4";
    const std::vector<std::string> floors = floorsInOrder(plan);
    const std::size_t groundRow = indexOf(floors, ground);
    const std::size_t destinationRow = indexOf(floors, destination);
    REQUIRE_FALSE(roomsAlongHall(plan, destination).empty());
    REQUIRE_FALSE(roomsAlongHall(plan, ground).empty());

    // The script (the run starts at the ground floor's arrival point facing the hall, the elevator behind): the view
    // turned to the elevator's doors (the prompt shows), interact opens the directory on the floors, the walker's
    // floor selected; down to gen4, its rooms (the first room selected), up to the hall's row, chosen: the ride. At
    // gen4's arrival point, facing its hall again, the same back to the ground floor.
    Json script = Json::array();
    std::uint64_t frame = turnAround(script, 1);
    const std::uint64_t firstOpen = frame + 3;
    script.push_back(press(firstOpen, "interact"));
    frame = moveSelection(script, firstOpen + 3, groundRow, destinationRow);
    script.push_back(press(frame, "ui_activate"));
    script.push_back(press(frame + 3, "ui_up"));
    script.push_back(press(frame + 6, "ui_activate"));
    const std::uint64_t firstChoice = frame + 6;
    frame = turnAround(script, firstChoice + kRideFrames + 5);
    const std::uint64_t secondOpen = frame + 3;
    script.push_back(press(secondOpen, "interact"));
    frame = moveSelection(script, secondOpen + 3, destinationRow, groundRow);
    script.push_back(press(frame, "ui_activate"));
    script.push_back(press(frame + 3, "ui_up"));
    script.push_back(press(frame + 6, "ui_activate"));
    const std::uint64_t secondChoice = frame + 6;
    const std::uint64_t frames = secondChoice + kRideFrames + 10;
    const fs::path scriptFile = building.dir / "ride_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());

    const fs::path shot = building.dir / "back_in_lobby.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "ride",
                              elevatorArguments(static_cast<std::uint32_t>(frames), {},
                                                {"--input-script", arg(scriptFile), "--screenshot", arg(shot)}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.state == "room");

    // The plan's floors in the elevator's order.
    REQUIRE(world.floors.size() == floors.size());
    for (std::size_t index = 0; index < floors.size(); ++index) {
        CHECK(world.floors[index].id == floors[index]);
    }
    // The prompt showed once the walker faced the elevator, the directory opened there twice (interact), its two
    // choices the floors.
    CHECK(world.elevatorPromptFrames > 0);
    REQUIRE(report.directory.bound);
    CHECK(report.directory.opened == 2);
    std::vector<std::string> commands;
    std::vector<std::uint64_t> opened;
    for (const rv::app::DirectoryEventRecord& event : report.directory.events) {
        if (event.event == "open") {
            CHECK(event.detail == "elevator");
            CHECK(event.id == "floors");
            opened.push_back(event.frame);
        }
        if (event.event == "command") {
            commands.push_back(event.id + ":" + event.detail);
        }
    }
    CHECK(opened == std::vector<std::uint64_t>{firstOpen, secondOpen});
    CHECK(commands == std::vector<std::string>{"choose-floor:" + destination, "choose-floor:" + ground});
    CHECK_FALSE(report.directory.open);

    // Two rides, each to its floor's arrival point facing the hall (the walker faced the elevator when it asked).
    REQUIRE(world.rides.size() == 2);
    requireRide(world.rides[0], ground, destination, "floor");
    requireRide(world.rides[1], destination, ground, "floor");
    CHECK(world.rides[0].requestedFrame == firstChoice + 1);
    CHECK(world.rides[1].requestedFrame == secondChoice + 1);
    const rv::world::FloorSceneBuild destinationScene = floorSceneOf(plan, destination, report.assetRoot);
    const rv::world::FloorSceneBuild groundScene = floorSceneOf(plan, ground, report.assetRoot);
    requireAtArrival(*world.rides[0].walker, destinationScene);
    requireAtArrival(*world.rides[1].walker, groundScene);
    REQUIRE(report.finalCamera.has_value());
    requireAtArrival(*report.finalCamera, groundScene);
    CHECK(world.teleports.empty());
    // The ride's phase, once a ride.
    const auto elevator = std::find_if(report.phases.begin(), report.phases.end(),
                                       [](const rv::app::PhaseRecord& phase) { return phase.name == "elevator"; });
    REQUIRE(elevator != report.phases.end());
    CHECK(elevator->opened == 2);
    CHECK_FALSE(elevator->open);
    // The run ends on the ground floor: its rooms resident, nothing of gen4's.
    CHECK(world.floor == ground);
    CHECK(residentRooms(report) == collectionRooms(plan, ground));
    CHECK(report.residency.pending == 0);
    (void)requireScreenshot(run, shot, kShotSize);

    // The GPU memory after each ride is the destination's: what the floor holds as a run started on that floor holds
    // it (the resident floor's given back), and so at the end.
    const std::vector<std::string> destinationRooms = roomsAlongHall(plan, destination);
    const AppRun direct =
        runApp(retroverseExe(), building.dir, "direct_gen4", elevatorArguments(5, destinationRooms.front()));
    requireCleanRun(direct);
    const AppRun lobby = runApp(retroverseExe(), building.dir, "direct_ground", elevatorArguments(5));
    requireCleanRun(lobby);
    CHECK(direct.report->world.floor == destination);
    CHECK(lobby.report->world.floor == ground);
    INFO("arrays: gen4 " << arrayBytesOf(*direct.report) << ", ground " << arrayBytesOf(*lobby.report));
    requireFloorMemory(world.rides[0], *direct.report);
    requireFloorMemory(world.rides[1], *lobby.report);
    CHECK(arrayBytesOf(report) == arrayBytesOf(*lobby.report));
    CHECK(report.residency.arrayBytes == lobby.report->residency.arrayBytes);

    // Release: the first ride cold (gen4's covers never derived in this data directory) within 10 s, the second warm
    // (the ground floor's covers in the cache since the start) within 3 s.
    if (kRelease) {
        INFO("rides: " << world.rides[0].totalMs.value_or(-1.0) << " ms, " << world.rides[1].totalMs.value_or(-1.0)
                       << " ms");
        CHECK(world.rides[0].totalMs.value_or(kRideColdMs + 1.0) <= kRideColdMs);
        CHECK(world.rides[1].totalMs.value_or(kRideWarmMs + 1.0) <= kRideWarmMs);
    }
}

TEST_CASE("world elevator: a room of another floor chosen with Map rides there and stands the walker at its doorway; "
          "a room of the same floor and its hall teleport",
          "[gpu][world][world_elevator]") {
    const PlannedBuilding building("world_elevator/room", false);
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    const std::string ground(rv::world::kGroundFloor);
    // The handheld floor: two collection rooms (gb's own, the shared room), so its arrays are not the lobby's.
    const std::string destination = "handheld";
    const std::vector<std::string> floors = floorsInOrder(plan);
    const std::vector<std::string> rooms = roomsAlongHall(plan, destination);
    REQUIRE(rooms.size() >= 2);
    const std::string target = building.roomOf("gb");
    const std::string other = rooms.front() == target ? rooms[1] : rooms.front();
    const std::size_t targetRow = 1 + indexOf(rooms, target);
    const std::size_t otherRow = 1 + indexOf(rooms, other);

    // Map in the lobby: the floors, down to the handheld floor, its rooms (the first room selected: row 1), the target
    // room chosen. There, Map again: the floors (the walker's selected), its rooms (the walker's room selected), the
    // other room of the floor chosen: a teleport. In that room, Map again: up to the hall's row of the walker's own
    // floor: a teleport back to its elevator.
    Json script = Json::array();
    std::uint64_t frame = 5;
    script.push_back(press(frame, "map"));
    frame = moveSelection(script, frame + 3, indexOf(floors, ground), indexOf(floors, destination));
    script.push_back(press(frame, "ui_activate"));
    frame = moveSelection(script, frame + 3, 1, targetRow);
    script.push_back(press(frame, "ui_activate"));
    const std::uint64_t choice = frame;
    frame = choice + kRideFrames + 5;
    script.push_back(press(frame, "map"));
    script.push_back(press(frame + 3, "ui_activate"));
    frame = moveSelection(script, frame + 6, targetRow, otherRow);
    script.push_back(press(frame, "ui_activate"));
    const std::uint64_t teleport = frame;
    frame = teleport + 5;
    script.push_back(press(frame, "map"));
    script.push_back(press(frame + 3, "ui_activate"));
    frame = moveSelection(script, frame + 6, otherRow, 0);
    script.push_back(press(frame, "ui_activate"));
    const std::uint64_t hall = frame;
    const std::uint64_t frames = hall + 10;
    const fs::path scriptFile = building.dir / "room_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());

    const AppRun run = runApp(retroverseExe(), building.dir, "room",
                              elevatorArguments(static_cast<std::uint32_t>(frames), {},
                                                {"--input-script", arg(scriptFile), "--screenshot",
                                                 arg(building.dir / "elevator.png")}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.state == "room");
    REQUIRE(report.directory.bound);
    CHECK(report.directory.opened == 3);
    for (const rv::app::DirectoryEventRecord& event : report.directory.events) {
        if (event.event == "open") {
            CHECK(event.detail == "map");
        }
    }

    // The ride to the room's floor, the walker at its doorway camera (the floor's frame: no start room).
    REQUIRE(world.rides.size() == 1);
    const rv::app::RideRecord& ride = world.rides.front();
    requireRide(ride, ground, destination, "room");
    CHECK(ride.room == target);
    CHECK(ride.requestedFrame == choice + 1);
    requireAt(*ride.walker, doorwayWalker(plan, target, report.assetRoot));
    // The teleport to the floor's other room (its doorway), then to the floor's own elevator: its arrival point facing
    // the hall, where the walker stood in the other room before.
    REQUIRE(world.teleports.size() == 2);
    const rv::app::TeleportRecord& moved = world.teleports.front();
    CHECK(moved.frame == teleport + 1);
    CHECK(moved.to == other);
    CHECK(moved.reason == "room");
    requireAt(moved.walker, doorwayWalker(plan, other, report.assetRoot));
    const rv::app::TeleportRecord& back = world.teleports.back();
    CHECK(back.frame == hall + 1);
    CHECK(back.to == "elevator");
    CHECK(back.reason == "floor");
    const rv::world::FloorSceneBuild destinationScene = floorSceneOf(plan, destination, report.assetRoot);
    requireAtArrival(back.walker, destinationScene);
    REQUIRE(report.finalCamera.has_value());
    requireAtArrival(*report.finalCamera, destinationScene);
    CHECK(world.floor == destination);
    CHECK(world.room.empty()); // the hall
    std::vector<std::string> commands;
    for (const rv::app::DirectoryEventRecord& event : report.directory.events) {
        if (event.event == "command") {
            commands.push_back(event.id + ":" + event.detail);
        }
    }
    CHECK(commands == std::vector<std::string>{"choose-room:" + target, "choose-room:" + other,
                                               "choose-floor:" + destination});

    // The GPU memory after the ride: the destination's, as a run started in the room has it; not the lobby's.
    const AppRun direct = runApp(retroverseExe(), building.dir, "direct", elevatorArguments(5, target));
    requireCleanRun(direct);
    const AppRun lobby = runApp(retroverseExe(), building.dir, "lobby", elevatorArguments(5));
    requireCleanRun(lobby);
    INFO("arrays: handheld " << arrayBytesOf(*direct.report) << ", ground " << arrayBytesOf(*lobby.report));
    requireFloorMemory(ride, *direct.report);
    // The rooms' arrays differ (the ground floor's total, with the hub showcase's own arrays, may round to the same).
    CHECK(direct.report->residency.arrayBytes != lobby.report->residency.arrayBytes);
    CHECK(arrayBytesOf(report) == arrayBytesOf(*direct.report));
    CHECK(residentRooms(report) == collectionRooms(plan, destination));
}

TEST_CASE("world elevator: Mostra nella stanza of a game on another floor rides there and eases to its Focus pose",
          "[gpu][world][world_elevator]") {
    const PlannedBuilding building("world_elevator/show_in_room", false);
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    const std::string ground(rv::world::kGroundFloor);
    // A first run writes the rooms' items (the title the search below types).
    const AppRun first = runApp(retroverseExe(), building.dir, "first", elevatorArguments(2));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const fs::path assetRoot = first.report->assetRoot;

    // The game: the first face-out item of snes's room (on gen4), its title unique in the fixture.
    const std::string roomId = building.roomOf("snes");
    const rv::world::Room& room = roomIn(plan, roomId);
    REQUIRE(room.floor != ground);
    auto data = rv::world::loadSceneData(assetRoot, plan, room.id);
    REQUIRE(data.has_value());
    auto layout = rv::world::collectionLayout(plan, room.id, *data, {});
    REQUIRE(layout.has_value());
    const rv::world::CollectionLayout floorLayout =
        rv::world::floorCollectionLayout(std::move(*layout), room.placement);
    const auto faceOut = std::find_if(floorLayout.items.begin(), floorLayout.items.end(),
                                      [](const rv::world::CollectionItem& item) {
                                          return item.stance == rv::world::ItemStance::FaceOut;
                                      });
    REQUIRE(faceOut != floorLayout.items.end());
    const rv::world::CollectionItem& target = *faceOut;
    const std::string search = searchOf(building, room, target.game);

    // The quick catalog (world_show_in_room.json's path): the search field, the title typed, the list, the card's
    // actions, "Mostra nella stanza"; the catalog closes, the ride, then the walker eased to the item's Focus pose in
    // 600 ms.
    Json script = Json::array();
    const std::uint64_t card = catalogCard(script, 5, search);
    script.push_back(press(card + 3, "ui_down"));
    script.push_back(press(card + 6, "ui_activate"));
    const std::uint64_t asked = card + 6;
    const std::uint64_t frames = asked + kRideFrames + 36 + 15;
    const fs::path scriptFile = building.dir / "show_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path shot = building.dir / "focus.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "show",
                              elevatorArguments(static_cast<std::uint32_t>(frames), {},
                                                {"--input-script", arg(scriptFile), "--screenshot", arg(shot)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(report.catalog.has_value());
    CHECK(report.catalog->selectedId == target.game.toString());

    // The ride to the game's floor, to its room's doorway.
    REQUIRE(world.rides.size() == 1);
    const rv::app::RideRecord& ride = world.rides.front();
    requireRide(ride, ground, room.floor, "showInRoom");
    CHECK(ride.room == roomId);
    CHECK(ride.game == target.game.toString());
    CHECK(ride.requestedFrame == asked + 1);
    requireAt(*ride.walker, doorwayWalker(plan, roomId, assetRoot));
    // Then the Focus pose of its item, the item focused (pulled out), the room holding the eye; no notice.
    CHECK(world.launch.showInRoom == 1);
    CHECK(world.launch.showInRoomFailed == 0);
    CHECK(world.interaction.state == "focus");
    CHECK(world.interaction.focusedGame == target.game.toString());
    CHECK(world.room == roomId);
    CHECK(world.floor == room.floor);
    CHECK(world.notice != "room-unreachable");
    rv::app::FocusViewer viewer;
    auto roomScene = rv::world::toSceneDescription(plan, room.id, *data);
    REQUIRE(roomScene.has_value());
    viewer.lens = roomScene->camera.lens;
    viewer.aspect = 1280.0f / 720.0f;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(target, viewer);
    rv::app::SpawnPoint focus;
    focus.feet = pose.feet;
    focus.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    REQUIRE(report.finalCamera.has_value());
    requireAt(*report.finalCamera, focus);
    (void)requireScreenshot(run, shot, kShotSize);
}

TEST_CASE("world elevator: the directory's favourites and recent games are the catalog's, a card says where its game "
          "stands",
          "[gpu][world][world_elevator]") {
    const PlannedBuilding building("world_elevator/favourites", false);
    const rv::world::StoredPlan stored = building.plan();
    // Map, the right shoulder to the games tab (asked of the catalog worker: a lockstep frame waits for it), the first
    // game's card.
    Json script = Json::array();
    script.push_back(press(5, "map"));
    script.push_back(press(8, "ui_filter_next"));
    script.push_back(press(14, "ui_activate"));
    const fs::path scriptFile = building.dir / "games_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path shot = building.dir / "card.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "games",
                              elevatorArguments(24, {}, {"--input-script", arg(scriptFile), "--screenshot", arg(shot)}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::DirectoryRecord& directory = run.report->directory;
    REQUIRE(directory.bound);
    CHECK(directory.open);
    CHECK(directory.tab == "games");
    CHECK(directory.view == "card");
    CHECK(directory.gamesState == "ready");
    CHECK(directory.gamesRequests == 1);

    // The fixture's favourites first (every one of them), then its played games, the most recent first, at most 30.
    const std::vector<ListedGame> expected = favoriteOrPlayed(building.fixture.root());
    const auto favourites = static_cast<std::size_t>(
        std::count_if(expected.begin(), expected.end(), [](const ListedGame& game) { return game.favorite; }));
    REQUIRE(favourites > 0);
    REQUIRE(expected.size() > favourites);
    const std::vector<rv::app::DirectoryGameRecord>& games = directory.games;
    REQUIRE(games.size() == std::min<std::size_t>(30, expected.size()));
    std::set<std::string> favouriteTitles;
    for (const ListedGame& game : expected) {
        if (game.favorite) {
            favouriteTitles.insert(game.title);
        }
    }
    std::string lastPlayed = "9999-99-99";
    for (std::size_t index = 0; index < games.size(); ++index) {
        const rv::app::DirectoryGameRecord& game = games[index];
        INFO(index << ": " << game.title << " (" << game.system << ") " << game.played);
        CHECK(game.favorite == (index < favourites));
        const auto listed = std::find_if(expected.begin(), expected.end(), [&](const ListedGame& candidate) {
            return candidate.title == game.title && candidate.favorite == game.favorite;
        });
        REQUIRE(listed != expected.end());
        CHECK(game.played == listed->played);
        if (game.favorite) {
            favouriteTitles.erase(game.title);
        } else {
            CHECK_FALSE(game.played.empty());
            CHECK(game.played <= lastPlayed);
            lastPlayed = game.played;
        }
    }
    CHECK(favouriteTitles.empty());
    // A played game left out was played no later than the last one listed.
    for (const ListedGame& game : expected) {
        if (!game.favorite &&
            std::none_of(games.begin(), games.end(),
                         [&](const rv::app::DirectoryGameRecord& listed) { return listed.title == game.title; })) {
            CHECK(game.played <= lastPlayed);
        }
    }

    // The first game's card: where it stands in the building (its room key, as the quick catalog's card says it).
    REQUIRE_FALSE(games.empty());
    CHECK(directory.cardGame == games.front().gameId);
    const rv::world::PlanIndex index(stored.plan);
    const auto location = index.find(rv::catalog::GameId::parse(games.front().gameId).value());
    const std::string room = !location                ? std::string("not-in-room")
                             : location->placed()     ? std::string("placed")
                             : location->reason       ? std::string(rv::world::toString(*location->reason))
                                                      : std::string("not-in-room");
    CHECK(directory.cardRoom == room);
    CHECK(directory.cardOutcome.empty());
    (void)requireScreenshot(run, shot, kShotSize);
}

TEST_CASE("world elevator: Avvia on the directory's card launches the game into the fake emulator and back",
          "[gpu][world][world_elevator]") {
    ensureLaunchAllowList();
    const PlannedBuilding building("world_elevator/launch", false);
    assignFakeEmulator(building);
    Json script = Json::array();
    script.push_back(press(5, "map"));
    script.push_back(press(8, "ui_filter_next"));
    script.push_back(press(14, "ui_activate"));  // the first game's card, Avvia focused
    script.push_back(press(20, "ui_activate"));  // Avvia
    const fs::path scriptFile = building.dir / "launch_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const AppRun run =
        runApp(retroverseExe(), building.dir, "launch", elevatorArguments(40, {}, {"--input-script", arg(scriptFile)}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::DirectoryRecord& directory = report.directory;
    REQUIRE_FALSE(directory.games.empty());
    const std::string game = directory.games.front().gameId;
    // The quick catalog's launch flow, by GameId: resolved on the catalog worker, the fake run and returned.
    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.gameId == game);
    CHECK(launch.profile == "fake-return");
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "returned");
    CHECK(launch.framesWhileRunning == 0);
    // The directory: the command, the launch's end on its card (a normal return tells nothing), still open on it.
    std::vector<std::string> events;
    for (const rv::app::DirectoryEventRecord& event : directory.events) {
        if (event.event == "command" || event.event == "launch-ended") {
            events.push_back(event.event + ":" + event.id + ":" + event.detail);
        }
    }
    CHECK(events == std::vector<std::string>{"command:launch:" + game, "launch-ended:" + game + ":returned"});
    CHECK(directory.open);
    CHECK(directory.view == "card");
    CHECK(directory.cardGame == game);
    CHECK(directory.cardOutcome.empty());
    // No ride, the walker where it started.
    CHECK(report.world.rides.empty());
}

TEST_CASE("world elevator: Avvia on the directory's card that cannot start tells why on the card",
          "[gpu][world][world_elevator]") {
    // No emulator profile assigned: the fixture's ES-DE command names its own copy of the fake, which the automation
    // guard does not allow (or no emulator at all): the launch ends at once, its outcome on the card.
    ensureLaunchAllowList();
    const PlannedBuilding building("world_elevator/launch_refused", false);
    Json script = Json::array();
    script.push_back(press(5, "map"));
    script.push_back(press(8, "ui_filter_next"));
    script.push_back(press(14, "ui_activate"));
    script.push_back(press(20, "ui_activate"));
    const fs::path scriptFile = building.dir / "launch_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path shot = building.dir / "outcome.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "launch",
                              elevatorArguments(40, {}, {"--input-script", arg(scriptFile), "--screenshot", arg(shot)}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::DirectoryRecord& directory = report.directory;
    REQUIRE_FALSE(directory.games.empty());
    const std::string game = directory.games.front().gameId;
    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    INFO("launch: " << launch.state << ", " << launch.outcome << " (" << launch.reason << ") " << launch.detail);
    CHECK(launch.gameId == game);
    CHECK_FALSE(launch.outcome.empty());
    CHECK(launch.outcome != "returned");
    // The card tells the launch's outcome (setCard), the directory still open on it.
    std::vector<std::string> events;
    for (const rv::app::DirectoryEventRecord& event : directory.events) {
        if (event.event == "command" || event.event == "launch-ended") {
            events.push_back(event.event + ":" + event.id + ":" + event.detail);
        }
    }
    CHECK(events == std::vector<std::string>{"command:launch:" + game, "launch-ended:" + game + ":" + launch.outcome});
    CHECK(directory.open);
    CHECK(directory.view == "card");
    CHECK(directory.cardGame == game);
    CHECK(directory.cardOutcome == launch.outcome);
    (void)requireScreenshot(run, shot, kShotSize);
}

TEST_CASE("world elevator: the directory opens in free roam only, never over an item taken in the same frame",
          "[gpu][world][world_elevator]") {
    const PlannedBuilding building("world_elevator/free_roam", false);
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    const std::string roomId = building.roomOf("snes");
    const rv::world::Room& room = roomIn(plan, roomId);
    const AppRun first = runApp(retroverseExe(), building.dir, "first", elevatorArguments(2, roomId));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    // The walker at the Focus pose of the room's first face-out item (the start room's frame), the reticle on it.
    auto data = rv::world::loadSceneData(first.report->assetRoot, plan, room.id);
    REQUIRE(data.has_value());
    auto layout = rv::world::collectionLayout(plan, room.id, *data, {});
    REQUIRE(layout.has_value());
    const auto faceOut = std::find_if(layout->items.begin(), layout->items.end(), [](const rv::world::CollectionItem& item) {
        return item.stance == rv::world::ItemStance::FaceOut;
    });
    REQUIRE(faceOut != layout->items.end());
    auto roomScene = rv::world::toSceneDescription(plan, room.id, *data);
    REQUIRE(roomScene.has_value());
    rv::app::FocusViewer viewer;
    viewer.lens = roomScene->camera.lens;
    viewer.aspect = 1280.0f / 720.0f;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(*faceOut, viewer);
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);

    // Interact (the face-out item hovered: Inspect) and Map in one frame: the item into the hand, no directory. Map in
    // Inspect: nothing. Indietro (ui_back: Inspect to Focus), Back (Focus to free roam), then Map: the directory.
    Json script = Json::array();
    const std::uint64_t both = 10;
    script.push_back(Json{{"fromFrame", both}, {"toFrame", both}, {"actions", {{"interact", 1}, {"map", 1}}}});
    script.push_back(press(both + 30, "map"));
    script.push_back(press(both + 40, "ui_back"));
    script.push_back(press(both + 90, "back"));
    const std::uint64_t opened = both + 150;
    script.push_back(press(opened, "map"));
    const fs::path scriptFile = building.dir / "free_roam_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path shot = building.dir / "free_roam.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "free_roam",
               elevatorArguments(static_cast<std::uint32_t>(opened + 10), roomId,
                                 {"--spawn", spawnArgument(spawn), "--input-script", arg(scriptFile), "--screenshot",
                                  arg(shot)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    // The item went into the hand in the frame of the double press, and the interaction came back to free roam.
    std::vector<std::string> states;
    for (const rv::app::InteractionEventRecord& event : report.world.interaction.events) {
        if (event.event == "state") {
            states.push_back(std::to_string(event.frame) + ":" + event.state);
        }
    }
    INFO("states: " << Json(states).dump());
    REQUIRE_FALSE(states.empty());
    CHECK(states.front() == std::to_string(both) + ":inspect");
    CHECK(report.world.interaction.state == "free roam");
    // The directory opened once: by the last Map, in free roam.
    const rv::app::DirectoryRecord& directory = report.directory;
    CHECK(directory.opened == 1);
    std::vector<std::string> events;
    for (const rv::app::DirectoryEventRecord& event : directory.events) {
        if (event.event == "open") {
            events.push_back(std::to_string(event.frame) + ":" + event.detail);
        }
    }
    CHECK(events == std::vector<std::string>{std::to_string(opened) + ":map"});
    CHECK(directory.open);
    CHECK(report.world.rides.empty());
    (void)requireScreenshot(run, shot, kShotSize);
}

TEST_CASE("world elevator: the ride's fade dims every pixel of the view alike, its lights too",
          "[gpu][world][world_elevator]") {
    // The view halfway through the fade-out is the view before it, each pixel's sRGB value times the fade's share
    // (ADR-0013 §10's fade; I4 step 7b's review: a fade of the exposure kept the lights bright until its last frame).
    const PlannedBuilding building("world_elevator/fade", false);
    const rv::world::StoredPlan stored = building.plan();
    const std::vector<std::string> floors = floorsInOrder(stored.plan);
    const fs::path before = building.dir / "before.png";
    const AppRun still = runApp(retroverseExe(), building.dir, "still",
                                elevatorArguments(5, {}, {"--screenshot", arg(before)}));
    skipWithoutOpenGl(still);
    requireCleanRun(still);
    // Map, gen4's hall chosen (the walker frozen meanwhile: the same view); the run ends in the ride's 7th frame.
    Json script = Json::array();
    script.push_back(press(5, "map"));
    std::uint64_t frame = moveSelection(script, 8, indexOf(floors, std::string(rv::world::kGroundFloor)),
                                        indexOf(floors, "gen4"));
    script.push_back(press(frame, "ui_activate"));
    script.push_back(press(frame + 3, "ui_up"));
    script.push_back(press(frame + 6, "ui_activate"));
    const std::uint64_t requested = frame + 7;
    constexpr std::uint64_t kFadeFrames = 7;
    const fs::path scriptFile = building.dir / "fade_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path fading = building.dir / "fading.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "fading",
                              elevatorArguments(static_cast<std::uint32_t>(requested + kFadeFrames), {},
                                                {"--input-script", arg(scriptFile), "--screenshot", arg(fading)}));
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.rides.size() == 1);
    CHECK(run.report->world.rides.front().requestedFrame == requested);
    CHECK(run.report->world.rides.front().outcome == "running");
    const rv::assets::ImageData a = requireScreenshot(still, before, kShotSize);
    const rv::assets::ImageData b = requireScreenshot(run, fading, kShotSize);
    REQUIRE(a.pixels.size() == b.pixels.size());
    const double share = 1.0 - static_cast<double>(kFadeFrames) * kFixedDt / kFadeSeconds;
    // The whole view and its brightest value (the lights) dimmed by the share. Then every 4 x 4 block's mean of each
    // colour channel against the share of the one before: the fade dims each block alike wherever it stands, whatever
    // the view holds; a block's mean is blind to the anti-aliasing of the edges inside it (a thin frame or a rug's
    // pattern finds its edges a pixel apart in the darker view, which moved single values by up to about 26).
    double sumA = 0.0;
    double sumB = 0.0;
    std::uint8_t brightestA = 0;
    std::uint8_t brightestB = 0;
    for (std::size_t index = 0; index < a.pixels.size(); ++index) {
        sumA += a.pixels[index];
        sumB += b.pixels[index];
        brightestA = std::max(brightestA, a.pixels[index]);
        brightestB = std::max(brightestB, b.pixels[index]);
    }
    constexpr int kBlock = 4;
    const int colours = std::min(a.channels, 3);
    double worst = 0.0;
    std::size_t off = 0;
    std::size_t blocks = 0;
    for (int by = 0; by + kBlock <= a.height; by += kBlock) {
        for (int bx = 0; bx + kBlock <= a.width; bx += kBlock) {
            for (int channel = 0; channel < colours; ++channel) {
                double meanA = 0.0;
                double meanB = 0.0;
                for (int y = by; y < by + kBlock; ++y) {
                    for (int x = bx; x < bx + kBlock; ++x) {
                        const std::size_t index =
                            (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) +
                             static_cast<std::size_t>(x)) * static_cast<std::size_t>(a.channels) +
                            static_cast<std::size_t>(channel);
                        meanA += a.pixels[index];
                        meanB += b.pixels[index];
                    }
                }
                meanA /= kBlock * kBlock;
                meanB /= kBlock * kBlock;
                const double excess = std::abs(meanB - share * meanA);
                worst = std::max(worst, excess);
                off += excess > 2.0 ? 1 : 0;
                ++blocks;
            }
        }
    }
    const double offShare = static_cast<double>(off) / static_cast<double>(blocks);
    INFO("share " << share << ": means " << sumA / static_cast<double>(a.pixels.size()) << " and "
                  << sumB / static_cast<double>(b.pixels.size()) << ", brightest " << int(brightestA) << " and "
                  << int(brightestB) << ", worst block excess " << worst << ", " << offShare * 100.0
                  << "% of the blocks' values off by more than 2");
    CHECK(brightestA >= 200); // the view has bright lights to dim
    CHECK(std::abs(sumB / sumA - share) < 0.01);
    CHECK(static_cast<double>(brightestB) <= share * brightestA + 3.0);
    // Measured in I6 step 2's dressed lobby: a worst block 4.4 off, 0.012% of the blocks' values off by more than 2;
    // in I6 step 5's hub (the balcony's balusters, the showcase's rails, the rugs' borders: edges anti-aliased a pixel
    // apart in the darker view, the whole view's mean excess about 0): a worst block 5.7 off, 0.074%.
    CHECK(worst <= 8.0);
    CHECK(offShare < 0.002);
}

TEST_CASE("world elevator: a game played before a ride is the start choice of its room's CRT on the destination",
          "[gpu][world][world_elevator]") {
    // ADR-0012 §7: a room's CRT starts on its most recently played placed game with a clip. A game launched from the
    // lobby (the quick catalog's Avvia into the fake) is played after the rooms' items files were written: the ride's
    // floor-build job takes the catalog's sessions now, so the CRT of its room, on another floor, starts on it.
    ensureLaunchAllowList();
    const PlannedBuilding building("world_elevator/played", false);
    assignFakeEmulator(building);
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    const rv::world::PlanIndex index(plan);
    // The game: in a console room off the ground floor, a placed game with a clip that is not the start choice of the
    // room's CRT now (a start there: the items files written), whose title no other game of the catalog holds (a
    // second release has its first's, another system's word lists may give it too: the quick catalog's search finds it
    // alone).
    const std::vector<std::string> names = gamelistNames(building.fixture.root());
    const auto titleShared = [&](const std::string& title) {
        return std::count_if(names.begin(), names.end(),
                             [&](const std::string& name) { return name.find(title) != std::string::npos; }) != 1;
    };
    std::string roomId;
    std::string choice;
    std::optional<rv::catalog::GameId> played;
    for (const std::string system : {"snes", "psx", "gb"}) {
        const std::string candidate = building.roomOf(system);
        if (roomIn(plan, candidate).floor == rv::world::kGroundFloor) {
            continue;
        }
        const AppRun start = runApp(retroverseExe(), building.dir, "start_" + system, elevatorArguments(10, candidate));
        skipWithoutOpenGl(start);
        requireCleanRun(start);
        REQUIRE(start.report->video.bound);
        const auto items = rv::app::readRoomItems(rv::app::roomItemsFile(
            building.data / "cache", rv::catalog::LibraryId::parse("esde").value(), roomIn(plan, candidate).id.system()));
        REQUIRE(items.has_value());
        for (const rv::app::RoomItemText& item : items->items) {
            const auto location = index.find(item.game);
            if (item.screen && !item.screen->video.empty() && item.game.toString() != start.report->video.gameId &&
                location && location->placed() && location->room.toString() == candidate && !item.title.empty() &&
                !titleShared(item.title)) {
                roomId = candidate;
                choice = start.report->video.gameId;
                played = item.game;
                break;
            }
        }
        if (played) {
            break;
        }
    }
    REQUIRE(played.has_value());
    const rv::world::Room& room = roomIn(plan, roomId);
    const std::string search = searchOf(building, room, *played);

    // The quick catalog in the lobby: the game's card, Avvia (no frame while the fake runs), the catalog closed; Map,
    // the room's floor, the room chosen: the ride, the walker at its doorway, the CRT bound after the eye's dwell.
    const std::vector<std::string> floors = floorsInOrder(plan);
    const std::vector<std::string> rooms = roomsAlongHall(plan, room.floor);
    Json script = Json::array();
    const std::uint64_t card = catalogCard(script, 5, search);
    script.push_back(press(card + 3, "ui_activate")); // Avvia
    script.push_back(press(card + 10, "quick_catalog"));
    std::uint64_t frame = card + 15;
    script.push_back(press(frame, "map"));
    frame = moveSelection(script, frame + 3, indexOf(floors, std::string(rv::world::kGroundFloor)),
                          indexOf(floors, room.floor));
    script.push_back(press(frame, "ui_activate"));
    frame = moveSelection(script, frame + 3, 1, 1 + indexOf(rooms, roomId));
    script.push_back(press(frame, "ui_activate"));
    const std::uint64_t choose = frame;
    const fs::path scriptFile = building.dir / "played_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const AppRun run = runApp(retroverseExe(), building.dir, "played",
                              elevatorArguments(static_cast<std::uint32_t>(choose + kRideFrames + 40), {},
                                                {"--input-script", arg(scriptFile)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.launches.size() == 1);
    REQUIRE(report.launches.front().gameId == played->toString());
    CHECK(report.launches.front().outcome == "returned");
    REQUIRE(report.world.rides.size() == 1);
    requireRide(report.world.rides.front(), rv::world::kGroundFloor, room.floor, "room");
    CHECK(report.world.room == roomId);
    CHECK(report.world.screenRoom == roomId);
    // The CRT of the room plays the game just played: the start choice of the catalog's sessions, not the items'.
    const rv::app::VideoRecord& video = report.video;
    REQUIRE(video.bound);
    INFO("the start's choice " << choice << ", played " << played->toString());
    CHECK(video.gameId == played->toString());
}
