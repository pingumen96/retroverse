// The building update (rv/world/incremental.hpp's updateBuilding(); ADR-0013 §3, §5, §7; I4 step 6a), with the shipped
// data: a system no room holds enters the building as it enters a fresh one, nothing that stands moving: its own room
// at the east end of the shorter row of its floor (a floor that held no room appears), a section appended to its
// floor's shared room or to the arcade room (the room opened when absent), a system waiting while its shared room is
// held, a section of a kept room room-kept, a full floor's console room joining the shared room or its games unplaced
// for capacity; an update of the building of no room equal to the fresh building; the golden update sequence of the
// building fixture's systems (tests/fixtures/world/golden/building-update-esde-seed-102938.json, compared byte for byte
// in Debug and Release, rewritten only with RV_UPDATE_GOLDEN=1).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>

#include "world_building_support.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::world::GenerationResult;
using rv::world::Room;
using rv::world::RoomId;
using rv::world::RoomSelection;
using rv::world::SystemRoom;
using rv::world::WorldPlan;
using rv::test::world::BuildingWorld;
using rv::test::world::findRoom;
using rv::test::world::sectionNames;
using rv::test::world::systemName;
using rv::test::world::systemSelection;

namespace {

constexpr std::uint64_t kWorldSeed = 102938;

const std::filesystem::path kGoldenDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "golden";

bool updatingGoldens() {
    const char* value = std::getenv("RV_UPDATE_GOLDEN");
    return value != nullptr && std::string_view(value) == "1";
}

const rv::world::RoomTemplate& shippedTemplate() {
    return rv::test::world::enteringData(rv::test::world::assetRoot(), "snes").roomTemplate;
}

/// The update of `before` to `selections`, checked against the building's invariants.
GenerationResult update(const BuildingWorld& world, const WorldPlan& before, const std::vector<RoomSelection>& selections,
                        std::string_view where) {
    GenerationResult result = rv::test::world::updateBuildingOf(world, before, selections);
    std::vector<std::string> failures;
    rv::test::world::checkBuildingUpdate(before, result, selections, world, shippedTemplate(), failures,
                                         std::string(where));
    rv::test::world::reportFailures(failures);
    return result;
}

const Room& roomOf(const WorldPlan& plan, std::string_view id) {
    const Room* room = findRoom(plan, id);
    INFO(id);
    REQUIRE(room != nullptr);
    return *room;
}

std::string contentDigest(const WorldPlan& plan, std::string_view room) {
    auto digest = rv::world::roomContentDigest(plan, *RoomId::parse(room));
    REQUIRE(digest.has_value());
    return *digest;
}

/// The placements of `plan` in `room`.
std::size_t placedIn(const WorldPlan& plan, std::string_view room) {
    const RoomId id = *RoomId::parse(room);
    return static_cast<std::size_t>(std::count_if(plan.placements.begin(), plan.placements.end(),
                                                  [&](const auto& placement) {
                                                      return placement.slot.fixture().room() == id;
                                                  }));
}

std::string planText(const WorldPlan& plan) {
    auto text = rv::world::writePlan(plan);
    REQUIRE(text.has_value());
    return *text;
}

/// The GameIds every selection of `selections` names.
std::set<std::uint64_t> takenGames(const std::vector<RoomSelection>& selections) {
    std::set<std::uint64_t> taken;
    for (const RoomSelection& selection : selections) {
        const std::set<std::uint64_t> values = rv::test::world::gameValuesOf(selection);
        taken.insert(values.begin(), values.end());
    }
    return taken;
}

/// `count` new titles in `selection`, and their GameIds.
std::vector<GameId> addTitles(RoomSelection& selection, std::size_t count, rv::core::Random& random,
                              std::set<std::uint64_t>& taken) {
    std::vector<GameId> added;
    for (std::size_t i = 0; i < count; ++i) {
        added.push_back(rv::test::world::addTitle(selection, random, taken).game);
    }
    return added;
}

} // namespace

TEST_CASE("world/building update: a new system takes its own room at the east end of the shorter row of its floor",
          "[world][building_update]") {
    const BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> first{systemSelection("snes", 40, 6000), systemSelection("gb", 30, 6001)};
    const WorldPlan plan = rv::test::world::generateBuildingOf(world, first).plan;

    // megadrive takes gen4's south row (snes stands in the north row's first place); psx opens gen5.
    std::vector<RoomSelection> grown = first;
    grown.push_back(systemSelection("megadrive", 40, 6002));
    grown.push_back(systemSelection("psx", 40, 6003));
    const GenerationResult updated = update(world, plan, grown, "megadrive and psx enter");
    CHECK(updated.changes.roomsAdded == std::vector<RoomId>{*RoomId::parse("room:megadrive"),
                                                            *RoomId::parse("room:psx")});
    CHECK(updated.changes.systemsEntered ==
          std::vector<SystemRoom>{{systemName("megadrive"), *RoomId::parse("room:megadrive")},
                                  {systemName("psx"), *RoomId::parse("room:psx")}});
    CHECK(updated.changes.systemsWaiting.empty());
    CHECK(roomOf(updated.plan, "room:megadrive").placement == rv::world::RoomPlacement{7650, 6650, 1});
    CHECK(roomOf(updated.plan, "room:psx").placement == rv::world::RoomPlacement{7650, -6650, 0});
    CHECK(roomOf(updated.plan, "room:psx").floor == "gen5");
    CHECK(updated.plan.floors ==
          std::vector<rv::world::Floor>{{"ground", 0}, {"gen4", 3}, {"gen5", 4}, {"handheld", 7}});
    CHECK(contentDigest(updated.plan, "room:snes") == contentDigest(plan, "room:snes"));
    CHECK(contentDigest(updated.plan, "room:gb") == contentDigest(plan, "room:gb"));
    CHECK(updated.changes.added.size() == 80);
    CHECK(updated.changes.rebuild == rv::world::RebuildLevel::LocalReflow);
    // New rooms that take places of their own are the fresh building's rooms (the same entry order): the update equals
    // the fresh building of all four systems, inputDigest included.
    CHECK(planText(updated.plan) == planText(rv::test::world::generateBuildingOf(world, grown).plan));

    // Again: nothing to change, nothing written.
    const GenerationResult again = update(world, updated.plan, grown, "again");
    CHECK(again.plan == updated.plan);
    CHECK(again.changes.digestAfter == again.changes.digestBefore);
    CHECK(again.changes.roomsAdded.empty());
    CHECK(again.changes.systemsEntered.empty());

    // A system entering alone (the others' selections unknown: their rooms stand) takes the same place.
    const GenerationResult alone = update(world, plan, {systemSelection("megadrive", 40, 6002)}, "megadrive alone");
    CHECK(roomOf(alone.plan, "room:megadrive") == roomOf(updated.plan, "room:megadrive"));
    CHECK(contentDigest(alone.plan, "room:megadrive") == contentDigest(updated.plan, "room:megadrive"));
}

TEST_CASE("world/building update: a small or arcade system appends a section, a new shared room opens its floor's",
          "[world][building_update]") {
    BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    world.tags[systemName("pocketlab")] = {"gb"};
    const std::vector<RoomSelection> first{systemSelection("gb", 30, 6100), systemSelection("pokemini", 8, 6101),
                                           systemSelection("mame", 15, 6102, 25)};
    const WorldPlan plan = rv::test::world::generateBuildingOf(world, first).plan;
    REQUIRE(sectionNames(roomOf(plan, "area:shared:handheld")) == std::vector<std::string>{"pokemini"});
    REQUIRE(sectionNames(roomOf(plan, "area:arcade")) == std::vector<std::string>{"mame"});

    // pocketlab (gb's row by its tag, 6 titles) after pokemini; fbneo after mame; neogeo opens gen4's shared room.
    std::vector<RoomSelection> grown = first;
    grown.push_back(systemSelection("pocketlab", 6, 6103));
    grown.push_back(systemSelection("fbneo", 10, 6104));
    grown.push_back(systemSelection("neogeo", 5, 6105));
    const GenerationResult updated = update(world, plan, grown, "three systems enter");
    CHECK(sectionNames(roomOf(updated.plan, "area:shared:handheld")) ==
          std::vector<std::string>{"pokemini", "pocketlab"});
    CHECK(sectionNames(roomOf(updated.plan, "area:arcade")) == std::vector<std::string>{"mame", "fbneo"});
    const Room& gen4 = roomOf(updated.plan, "area:shared:gen4");
    CHECK(sectionNames(gen4) == std::vector<std::string>{"neogeo"});
    CHECK(gen4.kind == rv::world::RoomKind::Shared);
    CHECK(gen4.placement == rv::world::RoomPlacement{7650, -6650, 0});
    CHECK(updated.changes.roomsAdded == std::vector<RoomId>{*RoomId::parse("area:shared:gen4")});
    CHECK(updated.changes.systemsEntered ==
          std::vector<SystemRoom>{{systemName("fbneo"), *RoomId::parse("area:arcade")},
                                  {systemName("neogeo"), *RoomId::parse("area:shared:gen4")},
                                  {systemName("pocketlab"), *RoomId::parse("area:shared:handheld")}});
    CHECK(placedIn(updated.plan, "area:shared:handheld") == 8 + 6);
    CHECK(placedIn(updated.plan, "area:arcade") == 15 + 10);
    CHECK(placedIn(updated.plan, "area:shared:gen4") == 5);
    // The appended sections' titles stand spine-out (the face-out set changes only with a regeneration), in the room
    // whose section they are; gb's room as it stood.
    const auto faceOut = [](const WorldPlan& of) { return rv::test::world::faceOutPlacements(of); };
    CHECK(faceOut(updated.plan).size() == faceOut(plan).size() + 5); // the new room's: its 5 titles all face-out
    for (const rv::world::Placement& placement : updated.plan.placements) {
        const std::string room = placement.slot.fixture().room().toString();
        if ((room == "area:shared:handheld" || room == "area:arcade") &&
            std::find(plan.placements.begin(), plan.placements.end(), placement) == plan.placements.end()) {
            const auto sections = rv::test::world::levelSections(updated.plan);
            CHECK(sections.at({placement.slot.fixture(), placement.slot.level()}) == rv::world::Section::Spine);
        }
    }
    CHECK(contentDigest(updated.plan, "room:gb") == contentDigest(plan, "room:gb"));
    const GenerationResult again = update(world, updated.plan, grown, "again");
    CHECK(again.changes.digestAfter == again.changes.digestBefore);
}

TEST_CASE("world/building update: a system waits while its shared room is held, then enters",
          "[world][building_update]") {
    BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    world.tags[systemName("pocketlab")] = {"gb"};
    const std::vector<RoomSelection> first{systemSelection("gb", 30, 6200), systemSelection("pokemini", 8, 6201),
                                           systemSelection("pocketlab", 6, 6202)};
    const WorldPlan plan = rv::test::world::generateBuildingOf(world, first).plan;
    REQUIRE(sectionNames(roomOf(plan, "area:shared:handheld")) == std::vector<std::string>{"pocketlab", "pokemini"});

    // pocketlab's selection unknown: the shared room is held, atarilynx (5 titles, its section's) waits, nothing moves.
    const RoomSelection atarilynx = systemSelection("atarilynx", 5, 6203);
    const GenerationResult held =
        update(world, plan, {first[0], first[1], atarilynx}, "atarilynx waits for the held shared room");
    CHECK(held.plan == plan);
    CHECK(held.changes.digestAfter == held.changes.digestBefore);
    CHECK(held.changes.systemsWaiting ==
          std::vector<SystemRoom>{{systemName("atarilynx"), *RoomId::parse("area:shared:handheld")}});
    REQUIRE(held.changes.heldRooms.size() == 1);
    CHECK(held.changes.heldRooms.front().room.toString() == "area:shared:handheld");
    // Given no selection of the room at all, alike.
    const GenerationResult unknown = update(world, plan, {first[0], atarilynx}, "the shared room not updated");
    CHECK(unknown.plan == plan);
    CHECK(unknown.changes.systemsWaiting == held.changes.systemsWaiting);

    // Every section given: atarilynx enters as the last section.
    const GenerationResult entered = update(world, plan, {first[0], first[1], first[2], atarilynx}, "every section");
    CHECK(sectionNames(roomOf(entered.plan, "area:shared:handheld")) ==
          std::vector<std::string>{"pocketlab", "pokemini", "atarilynx"});
    CHECK(entered.changes.systemsWaiting.empty());
    CHECK(entered.changes.added.size() == 5);
}

TEST_CASE("world/building update: a kept shared room takes the new section, its games room-kept",
          "[world][building_update]") {
    const BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> first{systemSelection("gb", 30, 6300), systemSelection("pokemini", 8, 6301)};
    WorldPlan plan = rv::test::world::generateBuildingOf(world, first).plan;
    for (Room& room : plan.rooms) {
        if (room.id.toString() == "area:shared:handheld") {
            room.generatorVersion = {0, 9}; // another generator major: kept as it is (ADR-0013 §5)
        }
    }
    std::vector<RoomSelection> grown = first;
    grown.push_back(systemSelection("atarilynx", 5, 6302));
    const GenerationResult updated = update(world, plan, grown, "a kept room's new section");
    REQUIRE(updated.changes.keptRooms.size() == 1);
    CHECK(updated.changes.keptRooms.front().room.toString() == "area:shared:handheld");
    CHECK(sectionNames(roomOf(updated.plan, "area:shared:handheld")) ==
          std::vector<std::string>{"pokemini", "atarilynx"});
    std::size_t roomKept = 0;
    for (const rv::world::UnplacedGame& game : updated.plan.unplaced) {
        roomKept += game.reason == rv::world::UnplacedReason::RoomKept ? 1 : 0;
    }
    CHECK(roomKept == 5);
    CHECK(placedIn(updated.plan, "area:shared:handheld") == placedIn(plan, "area:shared:handheld"));
}

TEST_CASE("world/building update: on a full floor a new console room joins the shared room, else waits as capacity",
          "[world][building_update]") {
    BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    world.building.layout.maxRoomsPerRow = 1;
    // gen4 full: snes north, its shared room (sfc, 5 titles) south. megadrive (40 titles) joins the shared room.
    const std::vector<RoomSelection> first{systemSelection("snes", 40, 6400), systemSelection("sfc", 5, 6401)};
    const WorldPlan plan = rv::test::world::generateBuildingOf(world, first).plan;
    std::vector<RoomSelection> grown = first;
    grown.push_back(systemSelection("megadrive", 40, 6402));
    const GenerationResult joined = update(world, plan, grown, "megadrive joins the shared room");
    CHECK(sectionNames(roomOf(joined.plan, "area:shared:gen4")) == std::vector<std::string>{"sfc", "megadrive"});
    CHECK(joined.changes.roomsAdded.empty());
    CHECK(placedIn(joined.plan, "area:shared:gen4") == 45);

    // Without a shared room on the full floor: genesis's games are unplaced for capacity, naming its room; again with
    // the same selections nothing changes (its entries give way to the same ones).
    const std::vector<RoomSelection> consoles{systemSelection("snes", 40, 6400), systemSelection("megadrive", 40, 6402)};
    const WorldPlan full = rv::test::world::generateBuildingOf(world, consoles).plan;
    std::vector<RoomSelection> more = consoles;
    more.push_back(systemSelection("genesis", 40, 6403));
    const GenerationResult capacity = update(world, full, more, "genesis finds no place");
    CHECK(capacity.plan.rooms == full.rooms);
    std::size_t unplaced = 0;
    for (const rv::world::UnplacedGame& game : capacity.plan.unplaced) {
        if (game.room.toString() == "room:genesis") {
            CHECK(game.reason == rv::world::UnplacedReason::Capacity);
            ++unplaced;
        }
    }
    CHECK(unplaced == 42); // its 40 titles and its two games of the selection's unplaced
    CHECK(capacity.changes.unplaced.size() == 42);
    const GenerationResult same = update(world, capacity.plan, more, "genesis again");
    CHECK(same.plan == capacity.plan);
    CHECK(same.changes.digestAfter == same.changes.digestBefore);
    // A title that leaves genesis meanwhile leaves no entry behind: the update is the fresh building of the same
    // selections.
    std::vector<RoomSelection> fewer = more;
    RoomSelection& genesis = fewer.back();
    const GameId gone = genesis.titles.front().game;
    genesis.titles.erase(genesis.titles.begin());
    std::erase_if(genesis.unplaced, [&](const rv::world::UnplacedGame& game) {
        return game.reason == rv::world::UnplacedReason::DuplicateTitle && game.representative == gone;
    });
    const GenerationResult left = update(world, capacity.plan, fewer, "a title leaves genesis");
    CHECK(std::none_of(left.plan.unplaced.begin(), left.plan.unplaced.end(),
                       [&](const rv::world::UnplacedGame& game) { return game.game == gone; }));
    CHECK(planText(left.plan) == planText(rv::test::world::generateBuildingOf(world, fewer).plan));
    // A small system there has no shared room to open: its games are unplaced naming the shared room.
    more.push_back(systemSelection("neogeo", 5, 6404));
    const GenerationResult small = update(world, capacity.plan, more, "neogeo finds no place");
    CHECK_FALSE(findRoom(small.plan, "area:shared:gen4"));
    CHECK(std::any_of(small.plan.unplaced.begin(), small.plan.unplaced.end(), [](const rv::world::UnplacedGame& game) {
        return game.room.toString() == "area:shared:gen4" && game.reason == rv::world::UnplacedReason::Capacity;
    }));
}

TEST_CASE("world/building update: the update of the building of no room is the fresh building",
          "[world][building_update]") {
    // The building fixture's systems (the golden building of test_floor_scene.cpp: the same selections) entering an
    // empty building at once: the fresh building's plan, byte for byte, its golden file.
    BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    world.tags[systemName("pocketlab")] = {"gb"};
    const WorldPlan empty = rv::test::world::generateBuildingOf(world, {}).plan;
    REQUIRE(empty.rooms.empty());
    struct Entry {
        std::string_view system;
        std::size_t titles;
        std::size_t distinct;
    };
    const Entry systems[] = {{"amiga", 20, 0}, {"gb", 30, 0},       {"mame", 15, 25}, {"pocketlab", 6, 0},
                             {"pokemini", 8, 0}, {"psx", 40, 0}, {"snes", 40, 0}};
    std::vector<RoomSelection> selections;
    std::uint64_t seed = 4000;
    for (const Entry& entry : systems) {
        selections.push_back(systemSelection(entry.system, entry.titles, seed++, entry.distinct));
    }
    const GenerationResult updated = update(world, empty, selections, "every system enters the empty building");
    auto golden = rv::core::readTextFile(kGoldenDir / "building-esde-seed-102938.json");
    REQUIRE(golden.has_value());
    CHECK(planText(updated.plan) == *golden);
    CHECK(updated.changes.roomsAdded.size() == 6);
    CHECK(updated.changes.systemsEntered.size() == 7);
}

TEST_CASE("world/building update: the golden update sequence of the building fixture", "[world][building_update]") {
    // The golden building of the building fixture's systems, then additions/ (tests/fixtures/esde/building/README.md:
    // megadrive's 12 titles, 5 titles more for snes and psx, 24 for pokemini), then three systems more: neogeo (small, a
    // section after megadrive's), fbneo (arcade, after mame) and nes (its own room on gen3, a new floor). Every step
    // keeps the building's invariants; the last plan byte for byte (Debug equal to Release).
    BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    world.tags[systemName("pocketlab")] = {"gb"};
    const std::vector<std::pair<std::string_view, std::size_t>> fixture = {
        {"amiga", 20}, {"gb", 30}, {"mame", 15}, {"pocketlab", 6}, {"pokemini", 8}, {"psx", 40}, {"snes", 40}};
    std::vector<RoomSelection> selections;
    std::uint64_t seed = 4000;
    for (const auto& [system, titles] : fixture) {
        selections.push_back(systemSelection(system, titles, seed++, system == "mame" ? 25 : 0));
    }
    const WorldPlan building = rv::test::world::generateBuildingOf(world, selections).plan;
    auto golden = rv::core::readTextFile(kGoldenDir / "building-esde-seed-102938.json");
    REQUIRE(golden.has_value());
    REQUIRE(planText(building) == *golden);

    std::set<std::uint64_t> taken = takenGames(selections);
    rv::core::Random random(3800);
    const auto selectionOf = [&](std::string_view system) -> RoomSelection& {
        const auto found = std::find_if(selections.begin(), selections.end(), [&](const RoomSelection& selection) {
            return selection.room.system() == systemName(system);
        });
        REQUIRE(found != selections.end());
        return *found;
    };
    // additions/.
    RoomSelection megadrive = systemSelection("megadrive", 12, 3800);
    for (const std::uint64_t value : rv::test::world::gameValuesOf(megadrive)) {
        REQUIRE(taken.insert(value).second);
    }
    selections.push_back(megadrive);
    addTitles(selectionOf("snes"), 5, random, taken);
    addTitles(selectionOf("psx"), 5, random, taken);
    addTitles(selectionOf("pokemini"), 24, random, taken);
    const GenerationResult additions = update(world, building, selections, "additions/");
    CHECK(additions.changes.added.size() == 12 + 5 + 5 + 24);
    CHECK(additions.changes.roomsAdded == std::vector<RoomId>{*RoomId::parse("area:shared:gen4")});
    CHECK(sectionNames(roomOf(additions.plan, "area:shared:handheld")) ==
          std::vector<std::string>{"pocketlab", "pokemini"}); // pokemini past 30 stays in its section
    for (const std::string_view room : {"area:arcade", "area:shared:computer", "room:gb"}) {
        CHECK(contentDigest(additions.plan, room) == contentDigest(building, room));
    }

    // Three systems more.
    for (const auto& [system, titles, selectionSeed] :
         std::vector<std::tuple<std::string_view, std::size_t, std::uint64_t>>{
             {"neogeo", 6, 3900}, {"fbneo", 10, 3901}, {"nes", 40, 3902}}) {
        RoomSelection selection = systemSelection(system, titles, selectionSeed);
        for (const std::uint64_t value : rv::test::world::gameValuesOf(selection)) {
            REQUIRE(taken.insert(value).second);
        }
        selections.push_back(std::move(selection));
    }
    const GenerationResult more = update(world, additions.plan, selections, "neogeo, fbneo, nes");
    CHECK(sectionNames(roomOf(more.plan, "area:shared:gen4")) == std::vector<std::string>{"megadrive", "neogeo"});
    CHECK(sectionNames(roomOf(more.plan, "area:arcade")) == std::vector<std::string>{"mame", "fbneo"});
    CHECK(roomOf(more.plan, "room:nes").floor == "gen3");
    CHECK(more.changes.roomsAdded == std::vector<RoomId>{*RoomId::parse("room:nes")});
    CHECK(std::find(more.plan.floors.begin(), more.plan.floors.end(), rv::world::Floor{"gen3", 2}) !=
          more.plan.floors.end());

    const std::string text = planText(more.plan);
    const std::filesystem::path path = kGoldenDir / "building-update-esde-seed-102938.json";
    if (updatingGoldens()) {
        REQUIRE(rv::core::writeTextFileAtomic(path, text).has_value());
        WARN("rewrote " << rv::core::pathToUtf8(path));
        return;
    }
    auto expected = rv::core::readTextFile(path);
    INFO(rv::core::pathToUtf8(path) << " (RV_UPDATE_GOLDEN=1 rewrites it)");
    REQUIRE(expected.has_value());
    CHECK(text == *expected);
}
