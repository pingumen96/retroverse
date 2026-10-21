// The directory in the app, its GPU-free part (I4 step 7b; ADR-0013 §10; docs/i4/step-7a.md's contract;
// rv/app/directory_data.hpp), on the golden building of the building fixture
// (tests/fixtures/world/golden/building-esde-seed-102938.json: five floors, a shared room in the south row beside a
// console room in the north row at the same x): the floors in the elevator's order with their rooms along the hall,
// kinds, systems and placed games; the floors' record; a favourite or recent game as the games tab shows it; where each
// command of the directory and "Mostra nella stanza" take the walker, and when they are refused.

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rv/app/directory_data.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/where_is.hpp>

using rv::app::RideReason;
using rv::app::WorldMove;
using rv::app::WorldMoveConditions;
using rv::app::WorldMoveKind;
using rv::app::WorldState;
using rv::ui::DirectoryCommand;
using rv::ui::DirectoryCommandKind;

namespace {

rv::world::WorldPlan goldenBuilding() {
    const std::filesystem::path file = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" /
                                       "golden" / "building-esde-seed-102938.json";
    auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "golden building");
    REQUIRE(plan.has_value());
    return std::move(*plan);
}

std::size_t placedIn(const rv::world::WorldPlan& plan, std::string_view room) {
    return static_cast<std::size_t>(
        std::count_if(plan.placements.begin(), plan.placements.end(), [&](const rv::world::Placement& placement) {
            return placement.slot.fixture().room().toString() == room;
        }));
}

/// A placed game of `room`.
rv::catalog::GameId gameIn(const rv::world::WorldPlan& plan, std::string_view room) {
    const auto found = std::find_if(plan.placements.begin(), plan.placements.end(), [&](const rv::world::Placement& p) {
        return p.slot.fixture().room().toString() == room;
    });
    REQUIRE(found != plan.placements.end());
    return found->game;
}

WorldMoveConditions ready(WorldState state = WorldState::FreeRoam) {
    WorldMoveConditions conditions;
    conditions.planReady = true;
    conditions.state = state;
    return conditions;
}

} // namespace

TEST_CASE("app/directory_data: the plan's floors in the elevator's order, their rooms along the hall",
          "[app][directory_data]") {
    const rv::world::WorldPlan plan = goldenBuilding();
    const std::map<std::string, std::string> names{{"gb", "Pocket Lumen"}, {"snes", "Lumen 16 Home System"}};
    const rv::ui::DirectoryBuilding building =
        rv::app::directoryBuildingOf(plan, [&](const rv::catalog::SystemName& system) {
            const auto found = names.find(system.str());
            return found != names.end() ? found->second : std::string();
        });
    CHECK(building.currentFloor.empty());
    CHECK(building.currentRoom.empty());
    std::vector<std::string> floors;
    std::vector<std::uint32_t> orders;
    for (const rv::ui::DirectoryFloor& floor : building.floors) {
        floors.push_back(floor.id);
        orders.push_back(floor.order);
    }
    CHECK(floors == std::vector<std::string>{"ground", "gen4", "gen5", "handheld", "computer"});
    CHECK(orders == std::vector<std::uint32_t>{0, 3, 4, 7, 8});

    // The handheld floor: gb's room in the north row and the shared room across the hall at the same x, the north row
    // first; the shared room's sections in its order.
    const rv::ui::DirectoryFloor& handheld = building.floors[3];
    REQUIRE(handheld.rooms.size() == 2);
    CHECK(handheld.rooms[0].id == "room:gb");
    CHECK(handheld.rooms[0].kind == "console");
    REQUIRE(handheld.rooms[0].systems.size() == 1);
    CHECK(handheld.rooms[0].systems[0] == rv::ui::DirectorySystem{"gb", "Pocket Lumen"});
    CHECK(handheld.rooms[0].games == placedIn(plan, "room:gb"));
    CHECK(handheld.rooms[1].id == "area:shared:handheld");
    CHECK(handheld.rooms[1].kind == "shared");
    REQUIRE(handheld.rooms[1].systems.size() == 2);
    CHECK(handheld.rooms[1].systems[0] == rv::ui::DirectorySystem{"pocketlab", ""});
    CHECK(handheld.rooms[1].systems[1] == rv::ui::DirectorySystem{"pokemini", ""});
    CHECK(handheld.rooms[1].games == placedIn(plan, "area:shared:handheld"));
    // The ground floor's arcade room.
    REQUIRE(building.floors[0].rooms.size() == 1);
    CHECK(building.floors[0].rooms[0].id == "area:arcade");
    CHECK(building.floors[0].rooms[0].kind == "arcade");
    CHECK(building.floors[0].rooms[0].games == 15);

    // The floors' record: the same order, their rooms and placed games.
    const std::vector<rv::app::WorldFloorRecord> records = rv::app::floorRecordsOf(plan);
    REQUIRE(records.size() == 5);
    std::uint64_t placed = 0;
    for (std::size_t index = 0; index < records.size(); ++index) {
        CHECK(records[index].id == floors[index]);
        CHECK(records[index].order == orders[index]);
        placed += records[index].placed;
    }
    CHECK(records[3].rooms == 2);
    CHECK(records[3].placed == placedIn(plan, "room:gb") + placedIn(plan, "area:shared:handheld"));
    CHECK(placed == plan.placements.size());
}

TEST_CASE("app/directory_data: a favourite or recent game as the games tab shows it", "[app][directory_data]") {
    rv::catalog::RecentGame recent;
    recent.id = rv::catalog::GameId::fromValue(0x1234).value();
    recent.system = rv::catalog::SystemName::parse("snes").value();
    recent.systemFullName = "Lumen 16 Home System";
    recent.title = "Amber Lantern";
    recent.favorite = true;
    // A fixed date for the time given.
    const rv::app::LocalDate localDate = [](rv::catalog::UtcTime) {
        return std::optional<rv::ui::DirectoryDate>(rv::ui::DirectoryDate{2026, 3, 9});
    };
    rv::ui::DirectoryGame game = rv::app::directoryGameOf(recent, localDate);
    CHECK(game.gameId == recent.id.toString());
    CHECK(game.title == "Amber Lantern");
    CHECK(game.system == "snes");
    CHECK(game.systemName == "Lumen 16 Home System");
    CHECK(game.favorite);
    CHECK_FALSE(game.played.has_value()); // a favourite never played
    recent.lastPlayed = rv::catalog::UtcTime{};
    game = rv::app::directoryGameOf(recent, localDate);
    CHECK(game.played == std::optional<rv::ui::DirectoryDate>(rv::ui::DirectoryDate{2026, 3, 9}));
    CHECK(rv::app::dateText(game.played) == "2026-03-09");
    CHECK(rv::app::dateText(std::nullopt).empty());
    // The machine's date of a time: a real date (the zone's, or UTC's).
    const std::optional<rv::ui::DirectoryDate> today = rv::app::machineLocalDate(std::chrono::system_clock::now());
    REQUIRE(today.has_value());
    CHECK(today->year >= 2026);
    CHECK(today->month >= 1);
    CHECK(today->month <= 12);
}

TEST_CASE("app/directory_data: where the directory's floors and rooms take the walker", "[app][directory_data]") {
    const rv::world::WorldPlan plan = goldenBuilding();
    const rv::world::PlanIndex index(plan);
    const auto route = [&](DirectoryCommandKind kind, std::string id, const WorldMoveConditions& conditions,
                           std::string_view floor = "ground") {
        return rv::app::routeDirectoryCommand(DirectoryCommand{kind, std::move(id)}, conditions, &plan, &index, floor,
                                              "");
    };
    // Another floor: a ride to its elevator; the walker's own floor: back to its elevator, no ride.
    WorldMove move = route(DirectoryCommandKind::ChooseFloor, "gen4", ready());
    CHECK(move.kind == WorldMoveKind::Ride);
    CHECK(move.reason == RideReason::Floor);
    CHECK(move.floor == "gen4");
    CHECK(move.room.empty());
    move = route(DirectoryCommandKind::ChooseFloor, "ground", ready());
    CHECK(move.kind == WorldMoveKind::Teleport);
    CHECK(move.floor == "ground");
    CHECK(move.room.empty());
    // A room of another floor: a ride to its floor, then its doorway; of the walker's floor: a teleport to its doorway.
    move = route(DirectoryCommandKind::ChooseRoom, "room:gb", ready());
    CHECK(move.kind == WorldMoveKind::Ride);
    CHECK(move.reason == RideReason::Room);
    CHECK(move.floor == "handheld");
    CHECK(move.room == "room:gb");
    move = route(DirectoryCommandKind::ChooseRoom, "area:shared:handheld", ready(), "handheld");
    CHECK(move.kind == WorldMoveKind::Teleport);
    CHECK(move.reason == RideReason::Room);
    CHECK(move.room == "area:shared:handheld");
    // Targets the plan lacks, and the product UI's own commands.
    CHECK(route(DirectoryCommandKind::ChooseFloor, "gen7", ready()).kind == WorldMoveKind::Ignore);
    CHECK(route(DirectoryCommandKind::ChooseRoom, "room:nes", ready()).kind == WorldMoveKind::Ignore);
    CHECK(route(DirectoryCommandKind::ChooseRoom, "not a room", ready()).kind == WorldMoveKind::Ignore);
    CHECK(route(DirectoryCommandKind::Launch, gameIn(plan, "room:snes").toString(), ready()).kind ==
          WorldMoveKind::Ignore);
    CHECK(route(DirectoryCommandKind::Close, "", ready()).kind == WorldMoveKind::Ignore);
}

TEST_CASE("app/directory_data: a ride or a teleport refused for now, with the panel's notice", "[app][directory_data]") {
    const rv::world::WorldPlan plan = goldenBuilding();
    const rv::world::PlanIndex index(plan);
    const DirectoryCommand gen4{DirectoryCommandKind::ChooseFloor, "gen4"};
    const DirectoryCommand ground{DirectoryCommandKind::ChooseFloor, "ground"};
    // No plan yet (the shell, the building being made): building-pending.
    WorldMoveConditions conditions = ready();
    conditions.planReady = false;
    WorldMove move = rv::app::routeDirectoryCommand(gen4, conditions, &plan, &index, "ground", "");
    CHECK(move.kind == WorldMoveKind::Refuse);
    CHECK(move.notice == rv::app::kNoticeBuildingPending);
    CHECK(rv::app::routeDirectoryCommand(gen4, ready(), nullptr, nullptr, "ground", "").notice ==
          rv::app::kNoticeBuildingPending);
    // A floor applying, a ride running, a launch active, Focus, Inspect, a launch's states: ride-unavailable, for a
    // teleport too.
    std::vector<WorldMoveConditions> refused;
    conditions = ready();
    conditions.applying = true;
    refused.push_back(conditions);
    conditions = ready();
    conditions.riding = true;
    refused.push_back(conditions);
    conditions = ready();
    conditions.launchActive = true;
    refused.push_back(conditions);
    for (const WorldState state :
         {WorldState::Focus, WorldState::Inspect, WorldState::Launching, WorldState::Returning}) {
        refused.push_back(ready(state));
    }
    for (const WorldMoveConditions& now : refused) {
        CAPTURE(rv::app::toString(now.state), now.applying, now.riding, now.launchActive);
        for (const DirectoryCommand& command : {gen4, ground}) {
            move = rv::app::routeDirectoryCommand(command, now, &plan, &index, "ground", "");
            CHECK(move.kind == WorldMoveKind::Refuse);
            CHECK(move.notice == rv::app::kNoticeRideUnavailable);
        }
    }
    CHECK(rv::app::toString(WorldMoveKind::Refuse) == "refuse");
}

TEST_CASE("app/directory_data: Mostra nella stanza across rooms and floors", "[app][directory_data]") {
    const rv::world::WorldPlan plan = goldenBuilding();
    const rv::world::PlanIndex index(plan);
    const rv::catalog::GameId snes = gameIn(plan, "room:snes");
    const rv::catalog::GameId gb = gameIn(plan, "room:gb");
    const rv::catalog::GameId pokemini = gameIn(plan, "area:shared:handheld");
    // In the room the interaction is in: I3's Focus pose there.
    WorldMove move = rv::app::routeShowInRoom(snes, ready(), &index, "gen4", "room:snes");
    CHECK(move.kind == WorldMoveKind::ShowHere);
    CHECK(move.room == "room:snes");
    CHECK(move.game == snes.toString());
    // In another room of the floor (or from its hall): a teleport to that room's doorway, then the Focus pose.
    move = rv::app::routeShowInRoom(pokemini, ready(), &index, "handheld", "room:gb");
    CHECK(move.kind == WorldMoveKind::Teleport);
    CHECK(move.reason == RideReason::ShowInRoom);
    CHECK(move.room == "area:shared:handheld");
    CHECK(rv::app::routeShowInRoom(gb, ready(), &index, "handheld", "").kind == WorldMoveKind::Teleport);
    // On another floor: a ride.
    move = rv::app::routeShowInRoom(gb, ready(), &index, "ground", "");
    CHECK(move.kind == WorldMoveKind::Ride);
    CHECK(move.floor == "handheld");
    CHECK(move.room == "room:gb");
    CHECK(move.game == gb.toString());
    // Focus is left first; Inspect and a launch's states refuse it.
    CHECK(rv::app::routeShowInRoom(gb, ready(WorldState::Focus), &index, "ground", "").kind == WorldMoveKind::Ride);
    for (const WorldState state : {WorldState::Inspect, WorldState::Launching, WorldState::Returning}) {
        move = rv::app::routeShowInRoom(gb, ready(state), &index, "ground", "");
        CHECK(move.kind == WorldMoveKind::Refuse);
        CHECK(move.notice == rv::app::kNoticeRideUnavailable);
    }
    // The directory's ShowInRoom routes the same.
    move = rv::app::routeDirectoryCommand(DirectoryCommand{DirectoryCommandKind::ShowInRoom, gb.toString()},
                                          ready(WorldState::Focus), &plan, &index, "ground", "");
    CHECK(move.kind == WorldMoveKind::Ride);
    CHECK(move.reason == RideReason::ShowInRoom);
    // A game the plan leaves out, or does not hold: nothing to go to.
    REQUIRE_FALSE(plan.unplaced.empty());
    CHECK(rv::app::routeShowInRoom(plan.unplaced.front().game, ready(), &index, "ground", "").kind ==
          WorldMoveKind::Ignore);
    CHECK(rv::app::routeShowInRoom(rv::catalog::GameId::fromValue(0x7777).value(), ready(), &index, "ground", "").kind ==
          WorldMoveKind::Ignore);
}
