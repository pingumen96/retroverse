// The directory's room chains (I4b step 8; ADR-0015 §7 as amended on 2026-10-29; rv/app/directory_data.hpp), on a
// small plan made here: a chain's rooms listed together under the head (its expansions by number, wherever they stand
// along the hall, the numbers leaving a gap), a room alone as before; each room of a chain with its titles' ranges (the
// initials of its first and last title in the chain's alphabetical order), section by section in a shared room, the
// ranges overlapping after an update's additions said so ("scattered"); the initials; the systems whose items files
// the ranges read, and the titles read from those files.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rv/app/directory_data.hpp>
#include <rv/app/room_items.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_plan.hpp>

#include "support/temp_dir.hpp"

using rv::app::ChainRoomRanges;
using rv::app::ChainTitle;
using rv::catalog::GameId;
using rv::catalog::SystemName;
using rv::ui::DirectoryRange;
using rv::ui::DirectorySystem;
using rv::world::RoomId;

namespace {

RoomId roomId(std::string_view text) {
    return RoomId::parse(text).value();
}

SystemName systemNamed(std::string_view name) {
    return SystemName::parse(name).value();
}

GameId game(std::uint64_t value) {
    return GameId::fromValue(value).value();
}

/// A room of the computer floor, 4 m wide, standing at `xMm` in the north row (yaw 0) or the south row.
rv::world::Room room(std::string_view id, rv::world::RoomKind kind, std::vector<std::string_view> sections,
                     std::int64_t xMm, bool south = false) {
    rv::world::Room made;
    made.id = roomId(id);
    made.kind = kind;
    made.floor = "computer";
    for (const std::string_view section : sections) {
        made.sections.push_back(systemNamed(section));
    }
    made.placement = rv::world::RoomPlacement{xMm, south ? 9000 : 0, south ? 1u : 0u};
    if (made.id.isExpansion()) {
        made.expands = made.id.head();
    }
    made.minXMm = -2000;
    made.maxXMm = 2000;
    made.minZMm = 0;
    made.maxZMm = 4000;
    made.heightMm = 3000;
    return made;
}

/// Game `value` placed in `room`'s shelf, slot `index`.
rv::world::Placement placed(std::string_view room, std::uint32_t index, std::uint64_t value) {
    const auto fixture = rv::world::FixtureId::make(roomId(room), "w01").value();
    return rv::world::Placement{rv::world::SlotId::make(fixture, 0, index).value(), game(value), 0, 0};
}

/// The plan: on the computer floor, room:amiga at x 0, its expansion :3 at 4 m, room:snes (alone) at 8 m and :2 at
/// 16 m (south row); on the handheld floor, the shared room and its expansion. Amiga's titles: the head alpha, bravo,
/// charlie; :2 delta, echo; :3 foxtrot, zulu (a fresh split's order). The shared room's: gb's a.. m in the head, gb's
/// n.. z and pokemini's in :2.
struct ChainPlan {
    rv::world::WorldPlan plan;
    std::map<GameId, ChainTitle> titles;

    ChainPlan() {
        plan.floors = {{"ground", 0}, {"handheld", 7}, {"computer", 8}};
        using rv::world::RoomKind;
        plan.rooms.push_back(room("room:amiga", RoomKind::Console, {}, 0));
        plan.rooms.push_back(room("room:amiga:3", RoomKind::Console, {}, 4000));
        plan.rooms.push_back(room("room:snes", RoomKind::Console, {}, 8000));
        plan.rooms.push_back(room("room:amiga:2", RoomKind::Console, {}, 16000, true));
        rv::world::Room shared = room("area:shared:handheld", RoomKind::Shared, {"gb", "pokemini"}, 0);
        shared.floor = "handheld";
        plan.rooms.push_back(shared);
        rv::world::Room sharedTwo = room("area:shared:handheld:2", RoomKind::Shared, {"gb", "pokemini"}, 0, true);
        sharedTwo.floor = "handheld";
        plan.rooms.push_back(sharedTwo);
        add("room:amiga", "amiga", {{1, "alpha"}, {2, "bravo"}, {3, "charlie"}});
        add("room:amiga:2", "amiga", {{4, "delta"}, {5, "echo"}});
        add("room:amiga:3", "amiga", {{6, "foxtrot"}, {7, "zulu"}});
        add("room:snes", "snes", {{20, "mode seven"}});
        add("area:shared:handheld", "gb", {{30, "apple"}, {31, "mango"}});
        add("area:shared:handheld:2", "gb", {{32, "nectarine"}, {33, "zucchini"}});
        add("area:shared:handheld:2", "pokemini", {{40, "acorn"}, {41, "yew"}});
    }

    void add(std::string_view roomText, std::string_view systemName,
             std::vector<std::pair<std::uint64_t, std::string>> games) {
        std::uint32_t slot = 0;
        for (const rv::world::Placement& existing : plan.placements) {
            if (existing.slot.fixture().room() == roomId(roomText)) {
                ++slot;
            }
        }
        for (auto& [value, key] : games) {
            plan.placements.push_back(placed(roomText, slot++, value));
            titles.emplace(game(value), ChainTitle{systemNamed(systemName), key});
        }
    }

    [[nodiscard]] std::map<RoomId, ChainRoomRanges> ranges() const {
        return rv::app::chainRangesOf(plan, [this](const GameId& id) -> std::optional<ChainTitle> {
            const auto found = titles.find(id);
            return found != titles.end() ? std::optional<ChainTitle>(found->second) : std::nullopt;
        });
    }
};

DirectoryRange range(std::string_view name, std::string first, std::string last, std::string fullName = {}) {
    return DirectoryRange{DirectorySystem{std::string(name), std::move(fullName)}, std::move(first), std::move(last)};
}

/// `system`'s items file as snapshotRoomItems() writes a shared or arcade room's: its own games with their titles, every
/// placed game of the room's other sections listed without text.
void writeSectionItems(const std::filesystem::path& cacheDir, const rv::catalog::LibraryId& library,
                       std::string_view system, const std::vector<std::pair<std::uint64_t, std::string>>& own,
                       const std::vector<std::uint64_t>& others) {
    rv::app::RoomItems items;
    items.systemName = std::string(system);
    for (const auto& [value, title] : own) {
        rv::app::RoomItemText text;
        text.game = game(value);
        text.title = title;
        items.items.push_back(std::move(text));
    }
    for (const std::uint64_t value : others) {
        rv::app::RoomItemText text;
        text.game = game(value);
        items.items.push_back(std::move(text));
    }
    std::sort(items.items.begin(), items.items.end(),
              [](const rv::app::RoomItemText& a, const rv::app::RoomItemText& b) { return a.game < b.game; });
    REQUIRE(rv::app::writeRoomItems(rv::app::roomItemsFile(cacheDir, library, systemNamed(system)), items).has_value());
}

} // namespace

TEST_CASE("app/directory_chains: a chain's rooms stand together under the head, its expansions by number",
          "[app][directory_data][chains]") {
    const ChainPlan chain;
    const rv::ui::DirectoryBuilding building = rv::app::directoryBuildingOf(chain.plan, {});
    const auto computer = std::find_if(building.floors.begin(), building.floors.end(),
                                       [](const rv::ui::DirectoryFloor& floor) { return floor.id == "computer"; });
    REQUIRE(computer != building.floors.end());
    // Along the hall the rooms stand amiga, :3, snes, :2: the chain comes together at its head's place.
    std::vector<std::string> ids;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> sizes;
    for (const rv::ui::DirectoryRoom& shown : computer->rooms) {
        ids.push_back(shown.id);
        indices.push_back(shown.chainIndex);
        sizes.push_back(shown.chainRooms);
    }
    CHECK(ids == std::vector<std::string>{"room:amiga", "room:amiga:2", "room:amiga:3", "room:snes"});
    CHECK(indices == std::vector<std::uint32_t>{1, 2, 3, 1});
    CHECK(sizes == std::vector<std::uint32_t>{3, 3, 3, 1});
    // Every room of the chain names its system; its games are its own.
    CHECK(computer->rooms[1].systems == std::vector<DirectorySystem>{{"amiga", ""}});
    CHECK(computer->rooms[1].games == 2);
    CHECK(computer->rooms[2].games == 2);
    // Without the ranges (their titles not read yet) no room shows one.
    for (const rv::ui::DirectoryRoom& shown : computer->rooms) {
        CHECK(shown.ranges.empty());
        CHECK_FALSE(shown.scattered);
    }

    // A gap in the numbers (:3 standing without :2 after a shrink, ADR-0015 §7 as amended): :3 under the head.
    ChainPlan gap;
    gap.plan.rooms.erase(std::find_if(gap.plan.rooms.begin(), gap.plan.rooms.end(),
                                      [](const rv::world::Room& r) { return r.id.toString() == "room:amiga:2"; }));
    const rv::ui::DirectoryBuilding gapped = rv::app::directoryBuildingOf(gap.plan, {});
    const auto floor = std::find_if(gapped.floors.begin(), gapped.floors.end(),
                                    [](const rv::ui::DirectoryFloor& f) { return f.id == "computer"; });
    REQUIRE(floor != gapped.floors.end());
    REQUIRE(floor->rooms.size() == 3);
    CHECK(floor->rooms[0].id == "room:amiga");
    CHECK(floor->rooms[1].id == "room:amiga:3");
    CHECK(floor->rooms[1].chainIndex == 3);
    CHECK(floor->rooms[1].chainRooms == 2);
    CHECK(floor->rooms[2].id == "room:snes");
}

TEST_CASE("app/directory_chains: each room of a chain shows its titles' range, section by section",
          "[app][directory_data][chains]") {
    const ChainPlan chain;
    const std::map<RoomId, ChainRoomRanges> ranges = chain.ranges();
    // A room alone has none; every room of a chain has one entry.
    CHECK_FALSE(ranges.contains(roomId("room:snes")));
    REQUIRE(ranges.size() == 5);
    CHECK(ranges.at(roomId("room:amiga")) == ChainRoomRanges{{range("amiga", "A", "C")}, false});
    CHECK(ranges.at(roomId("room:amiga:2")) == ChainRoomRanges{{range("amiga", "D", "E")}, false});
    CHECK(ranges.at(roomId("room:amiga:3")) == ChainRoomRanges{{range("amiga", "F", "Z")}, false});
    // A shared room's chain: the sections in the head's order, each room's range per section it holds titles of.
    CHECK(ranges.at(roomId("area:shared:handheld")) == ChainRoomRanges{{range("gb", "A", "M")}, false});
    CHECK(ranges.at(roomId("area:shared:handheld:2")) ==
          ChainRoomRanges{{range("gb", "N", "Z"), range("pokemini", "A", "Y")}, false});

    // In the directory, the systems named by the catalog's full names.
    const rv::ui::DirectoryBuilding building =
        rv::app::directoryBuildingOf(chain.plan,
                                     [](const SystemName& name) {
                                         return name.str() == "gb" ? std::string("Pocket Lumen") : std::string();
                                     },
                                     &ranges);
    const auto handheld = std::find_if(building.floors.begin(), building.floors.end(),
                                       [](const rv::ui::DirectoryFloor& floor) { return floor.id == "handheld"; });
    REQUIRE(handheld != building.floors.end());
    REQUIRE(handheld->rooms.size() == 2);
    CHECK(handheld->rooms[0].id == "area:shared:handheld");
    CHECK(handheld->rooms[1].id == "area:shared:handheld:2");
    CHECK(handheld->rooms[1].chainIndex == 2);
    CHECK(handheld->rooms[1].ranges ==
          std::vector<DirectoryRange>{range("gb", "N", "Z", "Pocket Lumen"), range("pokemini", "A", "Y")});
    const auto computer = std::find_if(building.floors.begin(), building.floors.end(),
                                       [](const rv::ui::DirectoryFloor& floor) { return floor.id == "computer"; });
    REQUIRE(computer != building.floors.end());
    CHECK(computer->rooms[0].ranges == std::vector<DirectoryRange>{range("amiga", "A", "C")});
    CHECK(computer->rooms[3].ranges.empty()); // room:snes, alone

    // A title not known (its items file not written yet) is left out; a chain none of whose titles is known has an
    // empty entry for each room.
    const auto partial = rv::app::chainRangesOf(chain.plan, [&](const GameId& id) -> std::optional<ChainTitle> {
        if (id == game(1)) {
            return std::nullopt;
        }
        return chain.titles.at(id);
    });
    CHECK(partial.at(roomId("room:amiga")) == ChainRoomRanges{{range("amiga", "B", "C")}, false});
    const auto none = rv::app::chainRangesOf(chain.plan, {});
    CHECK(none.at(roomId("room:amiga")) == ChainRoomRanges{});
    CHECK(none.at(roomId("area:shared:handheld:2")) == ChainRoomRanges{});
}

TEST_CASE("app/directory_chains: an update's additions out of order make the ranges overlap, and say so",
          "[app][directory_data][chains]") {
    ChainPlan chain;
    // golf goes after its predecessor foxtrot, whose room (:3) is full: the chain's next free slot, wrapping to the
    // head (ADR-0015 §7's fill order).
    chain.add("room:amiga", "amiga", {{8, "golf"}});
    const std::map<RoomId, ChainRoomRanges> ranges = chain.ranges();
    // Never a title of a room outside its range: the head now runs A – G, over :2's and part of :3's.
    CHECK(ranges.at(roomId("room:amiga")) == ChainRoomRanges{{range("amiga", "A", "G")}, true});
    CHECK(ranges.at(roomId("room:amiga:2")) == ChainRoomRanges{{range("amiga", "D", "E")}, false});
    CHECK(ranges.at(roomId("room:amiga:3")) == ChainRoomRanges{{range("amiga", "F", "Z")}, true});
    // A title equal in its initial to the next room's first is no overlap: the boundary of a fresh split, where both
    // ends show two letters.
    ChainPlan boundary;
    boundary.add("room:amiga", "amiga", {{9, "charlotte"}});
    boundary.add("room:amiga:2", "amiga", {{10, "cobra"}});
    const std::map<RoomId, ChainRoomRanges> shared = boundary.ranges();
    CHECK(shared.at(roomId("room:amiga")) == ChainRoomRanges{{range("amiga", "A", "Ch")}, false});
    CHECK(shared.at(roomId("room:amiga:2")) == ChainRoomRanges{{range("amiga", "Co", "E")}, false});
    CHECK(shared.at(roomId("room:amiga:3")) == ChainRoomRanges{{range("amiga", "F", "Z")}, false});
}

TEST_CASE("app/directory_chains: the initials, the systems read, the titles from the items files",
          "[app][directory_data][chains]") {
    CHECK(rv::app::rangeInitial("alpha") == "A");
    CHECK(rv::app::rangeInitial("Zed") == "Z");
    CHECK(rv::app::rangeInitial("1942 strike") == "1");
    CHECK(rv::app::rangeInitial("!bang") == "#");
    CHECK(rv::app::rangeInitial("") == "#");
    CHECK(rv::app::rangeInitial("\xC3\xA9t\xC3\xA9") == "#"); // a letter beyond ASCII
    CHECK(rv::app::rangePrefix("mossy bellows") == "Mo");
    CHECK(rv::app::rangePrefix("MOSSY") == "Mo");
    CHECK(rv::app::rangePrefix("m-1") == "M");
    CHECK(rv::app::rangePrefix("1942") == "19");
    CHECK(rv::app::rangePrefix("m") == "M");
    CHECK(rv::app::rangePrefix("") == "#");

    const ChainPlan chain;
    CHECK(rv::app::chainSystemsOf(chain.plan) ==
          std::vector<SystemName>{systemNamed("amiga"), systemNamed("gb"), systemNamed("pokemini")});
    rv::world::WorldPlan alone = chain.plan;
    alone.rooms.erase(std::remove_if(alone.rooms.begin(), alone.rooms.end(),
                                     [](const rv::world::Room& r) { return r.id.isExpansion(); }),
                      alone.rooms.end());
    CHECK(rv::app::chainSystemsOf(alone).empty());

    // amiga's items file: a game with the CRT's data (its sort key), one without (its title's key), one of no title
    // (its file name's); gb's file missing.
    const rv::test::TempDir dir("rv-directory-chains");
    const rv::catalog::LibraryId library;
    rv::app::RoomItems items;
    items.systemName = "Lumen Desk Computer";
    rv::app::RoomItemText first;
    first.game = game(1);
    first.title = "Alpha";
    first.screen = rv::app::RoomItemScreen{};
    first.screen->sortKey = "alpha, the";
    rv::app::RoomItemText second;
    second.game = game(2);
    second.title = "The Bravo";
    rv::app::RoomItemText third;
    third.game = game(3);
    third.fallbackTitle = "charlie (europe)";
    items.items = {first, second, third};
    REQUIRE(rv::app::writeRoomItems(rv::app::roomItemsFile(dir.path(), library, systemNamed("amiga")), items).has_value());
    const std::map<GameId, ChainTitle> titles =
        rv::app::chainTitlesOf(dir.path(), library, {systemNamed("amiga"), systemNamed("gb")});
    REQUIRE(titles.size() == 3);
    CHECK(titles.at(game(1)) == ChainTitle{systemNamed("amiga"), "alpha, the"});
    CHECK(titles.at(game(2)) == ChainTitle{systemNamed("amiga"), "the bravo"});
    CHECK(titles.at(game(3)) == ChainTitle{systemNamed("amiga"), "charlie (europe)"});
}

TEST_CASE("app/directory_chains: a section's items file never takes the titles of the room's other sections",
          "[app][directory_data][chains]") {
    // The shared room's chain of ChainPlan: gb's file lists pokemini's games without text, pokemini's lists gb's; read
    // in either order, each game keeps its own section's title.
    const rv::test::TempDir dir("rv-directory-chains-sections");
    const rv::catalog::LibraryId library;
    writeSectionItems(dir.path(), library, "gb", {{30, "Apple"}, {31, "Mango"}, {32, "Nectarine"}, {33, "Zucchini"}},
                      {40, 41});
    writeSectionItems(dir.path(), library, "pokemini", {{40, "Acorn"}, {41, "Yew"}}, {30, 31, 32, 33});
    for (const std::vector<SystemName>& order : {std::vector<SystemName>{systemNamed("gb"), systemNamed("pokemini")},
                                                 std::vector<SystemName>{systemNamed("pokemini"), systemNamed("gb")}}) {
        const std::map<GameId, ChainTitle> titles = rv::app::chainTitlesOf(dir.path(), library, order);
        REQUIRE(titles.size() == 6);
        CHECK(titles.at(game(30)) == ChainTitle{systemNamed("gb"), "apple"});
        CHECK(titles.at(game(33)) == ChainTitle{systemNamed("gb"), "zucchini"});
        CHECK(titles.at(game(40)) == ChainTitle{systemNamed("pokemini"), "acorn"});
        CHECK(titles.at(game(41)) == ChainTitle{systemNamed("pokemini"), "yew"});
        // The ranges from them: each section's own, nothing scattered.
        const ChainPlan chain;
        const auto ranges = rv::app::chainRangesOf(chain.plan, [&](const GameId& id) -> std::optional<ChainTitle> {
            const auto found = titles.find(id);
            return found != titles.end() ? std::optional<ChainTitle>(found->second) : std::nullopt;
        });
        CHECK(ranges.at(roomId("area:shared:handheld")) == ChainRoomRanges{{range("gb", "A", "M")}, false});
        CHECK(ranges.at(roomId("area:shared:handheld:2")) ==
              ChainRoomRanges{{range("gb", "N", "Z"), range("pokemini", "A", "Y")}, false});
    }

    // An arcade chain of three systems: each file lists the two others' games without text.
    writeSectionItems(dir.path(), library, "fbneo", {{50, "Aqua Bolt"}, {51, "Zinc Ring"}}, {60, 61, 70});
    writeSectionItems(dir.path(), library, "mame", {{60, "Brass Comet"}, {61, "Yarrow"}}, {50, 51, 70});
    writeSectionItems(dir.path(), library, "naomi", {{70, "Cobalt Sky"}}, {50, 51, 60, 61});
    const std::map<GameId, ChainTitle> arcade = rv::app::chainTitlesOf(
        dir.path(), library, {systemNamed("fbneo"), systemNamed("mame"), systemNamed("naomi")});
    REQUIRE(arcade.size() == 5);
    CHECK(arcade.at(game(50)) == ChainTitle{systemNamed("fbneo"), "aqua bolt"});
    CHECK(arcade.at(game(51)) == ChainTitle{systemNamed("fbneo"), "zinc ring"});
    CHECK(arcade.at(game(60)) == ChainTitle{systemNamed("mame"), "brass comet"});
    CHECK(arcade.at(game(61)) == ChainTitle{systemNamed("mame"), "yarrow"});
    CHECK(arcade.at(game(70)) == ChainTitle{systemNamed("naomi"), "cobalt sky"});
}

TEST_CASE("app/directory_chains: a teleport, \"Mostra nella stanza\" and where-is lead into an expansion",
          "[app][directory_data][chains]") {
    const ChainPlan chain;
    const rv::world::PlanIndex index(chain.plan);
    rv::app::WorldMoveConditions ready;
    ready.planReady = true;
    ready.state = rv::app::WorldState::FreeRoam;
    // Where-is: the expansion's game stands in the expansion, on its floor.
    const std::optional<rv::world::GameLocation> location = index.find(game(4));
    REQUIRE(location.has_value());
    CHECK(location->room == roomId("room:amiga:2"));
    CHECK(location->floor == "computer");
    // The directory's row of the expansion: a teleport on its floor, a ride from another.
    rv::app::WorldMove move = rv::app::routeDirectoryCommand(
        rv::ui::DirectoryCommand{rv::ui::DirectoryCommandKind::ChooseRoom, "room:amiga:2"}, ready, &chain.plan, &index,
        "computer", "room:amiga");
    CHECK(move.kind == rv::app::WorldMoveKind::Teleport);
    CHECK(move.room == "room:amiga:2");
    move = rv::app::routeDirectoryCommand(
        rv::ui::DirectoryCommand{rv::ui::DirectoryCommandKind::ChooseRoom, "room:amiga:3"}, ready, &chain.plan, &index,
        "handheld", "");
    CHECK(move.kind == rv::app::WorldMoveKind::Ride);
    CHECK(move.floor == "computer");
    CHECK(move.room == "room:amiga:3");
    // "Mostra nella stanza" of a game of the expansion from its head: a teleport into the expansion, then its pose.
    move = rv::app::routeShowInRoom(game(4), ready, &index, "computer", "room:amiga");
    CHECK(move.kind == rv::app::WorldMoveKind::Teleport);
    CHECK(move.reason == rv::app::RideReason::ShowInRoom);
    CHECK(move.room == "room:amiga:2");
    CHECK(move.game == game(4).toString());
    // In the expansion itself: the Focus pose there; from another floor: a ride.
    CHECK(rv::app::routeShowInRoom(game(4), ready, &index, "computer", "room:amiga:2").kind ==
          rv::app::WorldMoveKind::ShowHere);
    move = rv::app::routeShowInRoom(game(7), ready, &index, "ground", "");
    CHECK(move.kind == rv::app::WorldMoveKind::Ride);
    CHECK(move.room == "room:amiga:3");
}
