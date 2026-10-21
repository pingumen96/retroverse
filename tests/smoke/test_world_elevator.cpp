// The elevator and the directory in the app (I4 step 7b; ADR-0013 §10, §13): the building fixture
// (tests/fixtures/esde/building) recorded as the library of the case's data directory, imported and planned
// (building_support.hpp), then retroverse.exe lockstep and hidden, its rooms, floors and poses derived from the plan and
// the floors' layout (floor_layout.hpp), never written here:
//   - a ride from the ground floor to gen4 and back: the elevator's prompt at the arrival point, interact opening the
//     directory on its floors, the hall row of the other floor chosen (ui actions), the view faded out, the floor
//     swapped, faded in, the walker at the destination's arrival point facing the hall; world.rides with both rides, the
//     phase "elevator"; after each ride the collection arrays the destination's (a direct run of that floor's), at the
//     end the residency and the GPU memory the ground floor's; in Release the rides within the acceptable row (the
//     first cold within 10 s, the second warm within 3 s);
//   - a room of another floor chosen in the directory (ChooseRoom): the ride, the walker at that room's doorway camera
//     in the destination's frame, the room holding the eye;
//   - "Mostra nella stanza" of the quick catalog's card for a game placed on another floor: the ride, then the ease to
//     the item's Focus pose (the item focused);
//   - the directory's favourites and recent games: the fixture's favourites first, then its played games, each the
//     catalog's (the gamelists' <favorite> and <lastplayed>);
//   - Avvia on the directory's card: the fake emulator launched and back (RV_LAUNCH_ALLOWLIST, RV_LAUNCH_BACKGROUND),
//     the card's outcome none;
//   - a ride refused in Inspect: the directory opened in the frame interact inspected a face-out item, its floor chosen
//     while the item is in the hand: the panel back with its notice, no ride, the walker where it was.
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

/// The frames of one ride under lockstep at --fixed-dt 0.016667: 15 to fade out, the swap, the eye's area checked, 15
/// to fade in, and a few to spare.
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
    CHECK(std::abs(walker.yawDeg.value_or(0.0f) - yaw) < 1e-2f);
}

/// The collection arrays' bytes at the end of a run.
std::uint64_t arrayBytesOf(const rv::app::RunReport& report) {
    return report.gpuMemory.usage(rv::gl::GpuMemoryKind::CollectionArrays).bytes;
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

/// A ride's record checked: its floors and reason, its frames in order, the view back, the walker recorded.
void requireRide(const rv::app::RideRecord& ride, std::string_view from, std::string_view to, std::string_view reason) {
    INFO("ride " << ride.from << " -> " << ride.to << " (" << ride.reason << "): " << ride.outcome);
    CHECK(ride.from == from);
    CHECK(ride.to == to);
    CHECK(ride.reason == reason);
    CHECK(ride.outcome == "arrived");
    REQUIRE(ride.fadedOutFrame.has_value());
    REQUIRE(ride.swappedFrame.has_value());
    REQUIRE(ride.fadedInFrame.has_value());
    // 250 ms of fade at 60 frames a second: 15 frames each way.
    CHECK(*ride.fadedOutFrame == ride.requestedFrame + 14);
    CHECK(*ride.swappedFrame >= *ride.fadedOutFrame);
    CHECK(*ride.fadedInFrame >= *ride.swappedFrame + 15);
    CHECK(ride.builtMs.has_value());
    CHECK(ride.residentMs.has_value());
    REQUIRE(ride.totalMs.has_value());
    CHECK(*ride.totalMs >= *ride.builtMs);
    CHECK(ride.walker.has_value());
    CHECK(ride.arrayBytes > 0);
    CHECK(ride.gpuBytes >= ride.arrayBytes);
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

    // The script (the run starts at the ground floor's arrival point, in front of its elevator): interact opens the
    // directory on the floors, the walker's floor selected; down to gen4, its rooms (the first room selected), up to the
    // hall's row, chosen: the ride. At gen4's elevator the same back to the ground floor.
    Json script = Json::array();
    std::uint64_t frame = 5;
    script.push_back(press(frame, "interact"));
    frame = moveSelection(script, frame + 3, groundRow, destinationRow);
    script.push_back(press(frame, "ui_activate"));
    script.push_back(press(frame + 3, "ui_up"));
    script.push_back(press(frame + 6, "ui_activate"));
    const std::uint64_t firstChoice = frame + 6;
    frame = firstChoice + kRideFrames + 5;
    script.push_back(press(frame, "interact"));
    frame = moveSelection(script, frame + 3, destinationRow, groundRow);
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
    // The prompt showed at the elevator, the directory opened there twice (interact), its two choices the floors.
    CHECK(world.elevatorPromptFrames > 0);
    REQUIRE(report.directory.bound);
    CHECK(report.directory.opened == 2);
    std::vector<std::string> commands;
    for (const rv::app::DirectoryEventRecord& event : report.directory.events) {
        if (event.event == "open") {
            CHECK(event.detail == "elevator");
            CHECK(event.id == "floors");
        }
        if (event.event == "command") {
            commands.push_back(event.id + ":" + event.detail);
        }
    }
    CHECK(commands == std::vector<std::string>{"choose-floor:" + destination, "choose-floor:" + ground});
    CHECK_FALSE(report.directory.open);

    // Two rides, each to its floor's arrival point facing the hall.
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

    // The GPU memory after each ride is the destination's: its collection arrays those of a run started on that floor
    // (the resident floor's given back), and so at the end.
    const std::vector<std::string> destinationRooms = roomsAlongHall(plan, destination);
    const AppRun direct =
        runApp(retroverseExe(), building.dir, "direct_gen4", elevatorArguments(5, destinationRooms.front()));
    requireCleanRun(direct);
    const AppRun lobby = runApp(retroverseExe(), building.dir, "direct_ground", elevatorArguments(5));
    requireCleanRun(lobby);
    CHECK(direct.report->world.floor == destination);
    CHECK(lobby.report->world.floor == ground);
    INFO("arrays: gen4 " << arrayBytesOf(*direct.report) << ", ground " << arrayBytesOf(*lobby.report));
    CHECK(world.rides[0].arrayBytes == arrayBytesOf(*direct.report));
    CHECK(world.rides[1].arrayBytes == arrayBytesOf(*lobby.report));
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
          "a room of the same floor teleports",
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
    // other room of the floor chosen: a teleport.
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
    const std::uint64_t frames = teleport + 10;
    const fs::path scriptFile = building.dir / "room_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());

    const AppRun run = runApp(retroverseExe(), building.dir, "room",
                              elevatorArguments(static_cast<std::uint32_t>(frames), {},
                                                {"--input-script", arg(scriptFile), "--screenshot",
                                                 arg(building.dir / "other_room.png")}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.state == "room");
    REQUIRE(report.directory.bound);
    CHECK(report.directory.opened == 2);
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
    // The teleport to the floor's other room: its doorway, the room holding the eye at the end.
    REQUIRE(world.teleports.size() == 1);
    const rv::app::TeleportRecord& moved = world.teleports.front();
    CHECK(moved.frame == teleport + 1);
    CHECK(moved.to == other);
    CHECK(moved.reason == "room");
    requireAt(moved.walker, doorwayWalker(plan, other, report.assetRoot));
    REQUIRE(report.finalCamera.has_value());
    requireAt(*report.finalCamera, doorwayWalker(plan, other, report.assetRoot));
    CHECK(world.floor == destination);
    CHECK(world.room == other);

    // The arrays after the ride: the destination's, as a run started in the room has them; not the lobby's.
    const AppRun direct = runApp(retroverseExe(), building.dir, "direct", elevatorArguments(5, target));
    requireCleanRun(direct);
    const AppRun lobby = runApp(retroverseExe(), building.dir, "lobby", elevatorArguments(5));
    requireCleanRun(lobby);
    INFO("arrays: handheld " << arrayBytesOf(*direct.report) << ", ground " << arrayBytesOf(*lobby.report));
    CHECK(ride.arrayBytes == arrayBytesOf(*direct.report));
    CHECK(arrayBytesOf(*direct.report) != arrayBytesOf(*lobby.report));
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
    const auto items = rv::app::readRoomItems(
        rv::app::roomItemsFile(building.data / "cache", rv::catalog::LibraryId::parse("esde").value(),
                               room.sections.empty() ? room.id.system() : room.sections.front()));
    REQUIRE(items.has_value());
    const rv::app::RoomItemText* text = items->find(target.game);
    REQUIRE(text != nullptr);
    REQUIRE_FALSE(text->title.empty());
    std::string search = text->title;
    std::transform(search.begin(), search.end(), search.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    // The quick catalog (world_show_in_room.json's path): the search field, the title typed, the list, the card's
    // actions, "Mostra nella stanza"; the catalog closes, the ride, then the walker eased to the item's Focus pose in
    // 600 ms.
    Json script = Json::array();
    const std::uint64_t open = 5;
    script.push_back(press(open, "quick_catalog"));
    script.push_back(press(open + 3, "ui_up"));
    script.push_back(press(open + 6, "ui_up"));
    script.push_back(Json{{"fromFrame", open + 9}, {"toFrame", open + 9}, {"text", search}});
    script.push_back(press(open + 12, "ui_activate"));
    script.push_back(press(open + 15, "ui_right"));
    script.push_back(press(open + 18, "ui_down"));
    script.push_back(press(open + 21, "ui_activate"));
    const std::uint64_t asked = open + 21;
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
    // Then the Focus pose of its item, the item focused (pulled out), the room holding the eye.
    CHECK(world.launch.showInRoom == 1);
    CHECK(world.launch.showInRoomFailed == 0);
    CHECK(world.interaction.state == "focus");
    CHECK(world.interaction.focusedGame == target.game.toString());
    CHECK(world.room == roomId);
    CHECK(world.floor == room.floor);
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

TEST_CASE("world elevator: a ride asked while an item is inspected is refused, the directory back with its notice",
          "[gpu][world][world_elevator]") {
    const PlannedBuilding building("world_elevator/refused", false);
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    const std::string roomId = building.roomOf("snes");
    const rv::world::Room& room = roomIn(plan, roomId);
    const std::vector<std::string> floors = floorsInOrder(plan);
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

    // Interact (the face-out item hovered: Inspect) and Map in one frame: the directory opens over the item lifting into
    // the hand. The ground floor's hall row chosen: refused, the directory back on that floor's rooms with its notice.
    Json script = Json::array();
    const std::uint64_t both = 10;
    script.push_back(Json{{"fromFrame", both}, {"toFrame", both}, {"actions", {{"interact", 1}, {"map", 1}}}});
    std::uint64_t frame = moveSelection(script, both + 3, indexOf(floors, room.floor), indexOf(floors, "ground"));
    script.push_back(press(frame, "ui_activate"));
    script.push_back(press(frame + 3, "ui_up"));
    script.push_back(press(frame + 6, "ui_activate"));
    const std::uint64_t choice = frame + 6;
    const fs::path scriptFile = building.dir / "refused_script.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());
    const fs::path shot = building.dir / "refused.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "refused",
               elevatorArguments(static_cast<std::uint32_t>(choice + 10), roomId,
                                 {"--spawn", spawnArgument(spawn), "--input-script", arg(scriptFile), "--screenshot",
                                  arg(shot)}));
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    CHECK(report.world.rides.empty());
    CHECK(report.world.teleports.empty());
    CHECK(report.world.floor == room.floor);
    CHECK(report.world.interaction.state == "inspect");
    CHECK(report.world.interaction.focusedGame == faceOut->game.toString());
    const rv::app::DirectoryRecord& directory = report.directory;
    std::vector<std::string> events;
    for (const rv::app::DirectoryEventRecord& event : directory.events) {
        if (event.event == "command" || event.event == "refused") {
            events.push_back(std::to_string(event.frame) + ":" + event.event + ":" + event.id + ":" + event.detail);
        }
    }
    CHECK(events == std::vector<std::string>{std::to_string(choice) + ":command:choose-floor:ground",
                                             std::to_string(choice + 1) + ":refused:ride-unavailable:choose-floor"});
    CHECK(directory.open);
    CHECK(directory.view == "rooms");
    CHECK(directory.notice == "ride-unavailable");
    REQUIRE(report.finalCamera.has_value());
    CHECK(glm::length(report.finalCamera->feet - spawn.feet) < 1e-3f);
    (void)requireScreenshot(run, shot, kShotSize);
}
