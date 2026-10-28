// Expansion rooms (I4b step 7, ADR-0015 §7; generator 1.5), with the shipped data: a room past its template's capacity
// is a chain, its head and its expansions (room:<system>:<n>, area:arcade:<n>, each `expands` naming the head, on its
// floor, of its kind, sections, family and template). A fresh building splits a chain's titles into the fewest rooms
// that hold them with the reserve, in equal alphabetical shares, each expansion entering its floor at the east end of
// the shorter row once every system entered; an update fills one fill order per chain (the head's freed slots used
// first) and creates the chain's next room, with no face-out title, once every room stands at its limit; generator
// 1.4's building of the overflow fixture's counts (tests/fixtures/world/legacy/) migrated: its titles unplaced for
// capacity enter their chains (the heads' reserve holds them), every other room byte for byte in its elements, then
// more titles open an expansion; no title unplaced for capacity while the floor has room (a property over random
// arcade sequences), a full floor's titles unplaced for capacity naming the head until "Rigenera stanza"; chains
// regenerated (a room's chain split again, the world equal to the fresh building), a pin in an expansion honoured;
// two runs one SHA-256.

#include "world_building_support.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/regeneration.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_edit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using rv::catalog::GameId;
using rv::world::FixtureRole;
using rv::world::GenerationResult;
using rv::world::Placement;
using rv::world::Regeneration;
using rv::world::RegenerationScope;
using rv::world::Room;
using rv::world::RoomId;
using rv::world::RoomSelection;
using rv::world::RoomTitle;
using rv::world::UnplacedReason;
using rv::world::WorldPlan;

namespace {

using namespace rv::test::world;

constexpr std::uint64_t kWorldSeed = 102938;

/// The selections of tests/fixtures/world/legacy/building-overflow-seed-102938-generator-1.4.json: the esde/overflow
/// fixture's counts (a big-box system of 1,700 titles, an arcade of 60) and three rooms beside them.
std::vector<RoomSelection> overflowSelections() {
    return {systemSelection("amiga", 1700, 5200), systemSelection("mame", 60, 5100), systemSelection("snes", 40, 5300),
            systemSelection("gb", 30, 5400), systemSelection("pokemini", 8, 5500)};
}

RoomId roomId(std::string_view text) {
    return *RoomId::parse(text);
}

/// The placements of `plan` by room.
std::map<RoomId, std::vector<Placement>> placementsByRoom(const WorldPlan& plan) {
    std::map<RoomId, std::vector<Placement>> rooms;
    for (const Placement& placement : plan.placements) {
        rooms[placement.slot.fixture().room()].push_back(placement);
    }
    return rooms;
}

std::size_t capacityEntries(const WorldPlan& plan) {
    return static_cast<std::size_t>(std::count_if(plan.unplaced.begin(), plan.unplaced.end(), [](const auto& game) {
        return game.reason == UnplacedReason::Capacity;
    }));
}

/// The room `id` with its elements, as a plan of that room, its record's generator and theme pack left out (an update
/// stamps them), so that rooms compare by their elements.
WorldPlan roomPart(const WorldPlan& plan, const RoomId& id) {
    WorldPlan part;
    for (const Room& room : plan.rooms) {
        if (room.id == id) {
            part.rooms.push_back(room);
            part.rooms.back().generatorVersion = {};
            part.rooms.back().themePack.clear();
        }
    }
    std::copy_if(plan.fixtures.begin(), plan.fixtures.end(), std::back_inserter(part.fixtures),
                 [&](const auto& fixture) { return fixture.id.room() == id; });
    std::copy_if(plan.placements.begin(), plan.placements.end(), std::back_inserter(part.placements),
                 [&](const auto& placement) { return placement.slot.fixture().room() == id; });
    std::copy_if(plan.dressing.begin(), plan.dressing.end(), std::back_inserter(part.dressing),
                 [&](const auto& dressed) { return dressed.level.fixture().room() == id; });
    std::copy_if(plan.lights.begin(), plan.lights.end(), std::back_inserter(part.lights),
                 [&](const auto& light) { return light.id.room() == id; });
    std::copy_if(plan.unplaced.begin(), plan.unplaced.end(), std::back_inserter(part.unplaced),
                 [&](const auto& game) { return game.room == id; });
    return part;
}

/// The face-out placements of `room` in `plan`.
std::size_t faceOutIn(const WorldPlan& plan, const RoomId& room) {
    const auto sections = levelSections(plan);
    return static_cast<std::size_t>(std::count_if(plan.placements.begin(), plan.placements.end(), [&](const auto& p) {
        return p.slot.fixture().room() == room &&
               rv::world::isFaceOut(sections.at({p.slot.fixture(), p.slot.level()}));
    }));
}

std::string hardFindings(const rv::world::ValidationReport& report) {
    std::string text;
    for (const rv::world::ValidationFinding& finding : report.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            text += finding.rule + " " + finding.subject + ": " + finding.message + "\n";
        }
    }
    return text;
}

const rv::world::RoomDataLoader& loader() {
    static const rv::world::RoomDataLoader load = rv::world::assetRootLoader(assetRoot());
    return load;
}

Regeneration regenerateOf(const BuildingWorld& world, const WorldPlan& previous,
                          const std::vector<RoomSelection>& selections, const RegenerationScope& scope,
                          const rv::world::WorldOverrides& overrides = {}) {
    rv::world::RegenerationInput input;
    input.scope = scope;
    input.library = library();
    input.selections = selections;
    input.platformTags = world.tags;
    input.config = world.config;
    input.overrides = overrides;
    input.previous = previous;
    auto result = rv::world::regenerate(input, world.building, world.systems, loader());
    INFO(scope.toString() << ": " << (result ? std::string("regenerated") : result.error().toString()));
    REQUIRE(result.has_value());
    INFO(hardFindings(result->result.validation));
    CHECK(result->result.validation.passed());
    return std::move(*result);
}

/// The chain whose head is `head` in `plan`: its rooms by number.
std::vector<const Room*> chainOf(const WorldPlan& plan, const RoomId& head) {
    std::vector<const Room*> chain;
    for (const Room& room : plan.rooms) {
        if (room.id.head() == head) {
            chain.push_back(&room);
        }
    }
    std::sort(chain.begin(), chain.end(), [](const Room* a, const Room* b) { return a->id < b->id; });
    return chain;
}

/// A chain's rooms each of the head's record but for their places (ADR-0015 §7), numbered from 2 without gaps
/// (a fresh chain's), the titles of `selections` in equal alphabetical shares (the collection's order: section, sort
/// key, title key, GameId), each placed once.
void checkFreshChain(const WorldPlan& plan, const RoomId& head, const std::vector<const RoomSelection*>& selections,
                     std::size_t rooms) {
    INFO(head.toString());
    const std::vector<const Room*> chain = chainOf(plan, head);
    REQUIRE(chain.size() == rooms);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const Room& room = *chain[i];
        CHECK(room.id == *head.inChain(static_cast<std::uint32_t>(i + 1)));
        CHECK(room.expands == (i == 0 ? RoomId{} : head));
        CHECK(room.floor == chain.front()->floor);
        CHECK(room.kind == chain.front()->kind);
        CHECK(room.sections == chain.front()->sections);
        CHECK(room.mediaFamily == chain.front()->mediaFamily);
        CHECK(room.templateId == chain.front()->templateId);
        CHECK(room.seed == rv::world::roomSeed(plan.seed, room.id));
    }
    std::vector<std::tuple<std::size_t, std::string, std::string, GameId>> alphabetical;
    for (std::size_t section = 0; section < selections.size(); ++section) {
        for (const RoomTitle& title : selections[section]->titles) {
            alphabetical.emplace_back(section, title.sortKey, title.titleKey, title.game);
        }
    }
    std::sort(alphabetical.begin(), alphabetical.end());
    std::map<GameId, RoomId> roomOfGame;
    for (const Placement& placement : plan.placements) {
        roomOfGame.emplace(placement.game, placement.slot.fixture().room());
    }
    std::size_t next = 0;
    for (std::size_t i = 0; i < rooms; ++i) {
        const std::size_t share = alphabetical.size() / rooms + (i < alphabetical.size() % rooms ? 1 : 0);
        for (std::size_t taken = 0; taken < share; ++taken, ++next) {
            const auto found = roomOfGame.find(std::get<3>(alphabetical[next]));
            REQUIRE(found != roomOfGame.end());
            CHECK(found->second == chain[i]->id);
        }
    }
}

} // namespace

TEST_CASE("world/expansion: a fresh building splits each room past its capacity into a chain of equal shares",
          "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> given = overflowSelections();
    const GenerationResult result = generateBuildingOf(world, given);
    INFO(hardFindings(result.validation));
    REQUIRE(result.validation.passed());
    const WorldPlan& plan = result.plan;
    // No title unplaced for capacity while the floor has room: amiga's 1,700 in two rooms of 850 (1,561 hold one),
    // mame's 60 in two of 30 (53 hold the arcade room).
    CHECK(capacityEntries(plan) == 0);
    checkFreshChain(plan, roomId("room:amiga"), {&given[0]}, 2);
    checkFreshChain(plan, roomId("area:arcade"), {&given[1]}, 2);
    const auto placed = placementsByRoom(plan);
    CHECK(placed.at(roomId("room:amiga")).size() == 850);
    CHECK(placed.at(roomId("room:amiga:2")).size() == 850);
    CHECK(placed.at(roomId("area:arcade")).size() == 30);
    CHECK(placed.at(roomId("area:arcade:2")).size() == 30);
    // A fresh expansion chooses its own face-out titles from its share.
    CHECK(faceOutIn(plan, roomId("room:amiga:2")) > 0);
    CHECK(faceOutIn(plan, roomId("area:arcade:2")) > 0);
    // Each expansion entered its floor once every system had: the shorter row's east end, here the south row's start.
    for (const std::string_view id : {"room:amiga:2", "area:arcade:2"}) {
        const Room* expansion = findRoom(plan, id);
        const Room* head = findRoom(plan, expansion->expands.toString());
        REQUIRE(head != nullptr);
        CHECK(expansion->placement.yawHalfTurns == 1);
        CHECK(expansion->placement.xMm == head->placement.xMm);
        CHECK(expansion->placement == rv::world::followDepth(*expansion, world.building.layout));
    }
    // The report names the new rooms, the systems the heads.
    CHECK(std::find(result.changes.roomsAdded.begin(), result.changes.roomsAdded.end(), roomId("room:amiga:2")) !=
          result.changes.roomsAdded.end());
    CHECK(std::find(result.changes.roomsAdded.begin(), result.changes.roomsAdded.end(), roomId("area:arcade:2")) !=
          result.changes.roomsAdded.end());
    CHECK(std::find(result.changes.systemsEntered.begin(), result.changes.systemsEntered.end(),
                    rv::world::SystemRoom{systemName("amiga"), roomId("room:amiga")}) !=
          result.changes.systemsEntered.end());
    // Two runs, one SHA-256; the update of the same selections writes nothing; the world regenerated is the same.
    const GenerationResult again = generateBuildingOf(world, given);
    CHECK(again.changes.digestAfter == result.changes.digestAfter);
    CHECK(planText(again.plan) == planText(plan));
    const GenerationResult updated = updateBuildingOf(world, plan, given);
    CHECK(updated.plan == plan);
    CHECK(updated.changes.digestAfter == updated.changes.digestBefore);
    const Regeneration regenerated = regenerateOf(world, plan, given, RegenerationScope::ofWorld());
    CHECK(planText(regenerated.result.plan) == planText(plan));
    CHECK_FALSE(regenerated.changed);
    // The expansion's chain regenerated from any of its rooms: the same chain.
    const Regeneration chain = regenerateOf(world, plan, given, RegenerationScope::ofRoom(roomId("room:amiga:2")));
    CHECK(planText(chain.result.plan) == planText(plan));
    CHECK(chain.impact.roomsRegenerated == std::vector{roomId("room:amiga"), roomId("room:amiga:2")});
    const Regeneration floor = regenerateOf(world, plan, given, RegenerationScope::ofFloor("ground"));
    CHECK(planText(floor.result.plan) == planText(plan));
}

TEST_CASE("world/expansion: generator 1.4's capacity entries enter their chains, every other room as it stood",
          "[world][expansion]") {
    // tests/fixtures/world/legacy/README.md: main's generator 1.4 wrote the building of overflowSelections(), its
    // big-box room (1,561 of 1,700) and arcade room (53 of 60) at their template's limit.
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> given = overflowSelections();
    const std::filesystem::path file = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" /
                                       "legacy" / "building-overflow-seed-102938-generator-1.4.json";
    auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    auto legacy = rv::world::readPlan(*text, "legacy");
    REQUIRE(legacy.has_value());
    REQUIRE(legacy->generatorVersion == rv::world::GeneratorVersion{1, 4});
    std::vector<GameId> overCapacity;
    for (const rv::world::UnplacedGame& game : legacy->unplaced) {
        if (game.reason == UnplacedReason::Capacity) {
            overCapacity.push_back(game.game);
        }
    }
    std::sort(overCapacity.begin(), overCapacity.end());
    REQUIRE(overCapacity.size() == 139 + 7);

    const GenerationResult migrated = updateBuildingOf(world, *legacy, given);
    INFO(hardFindings(migrated.validation));
    CHECK(migrated.validation.passed());
    std::vector<std::string> failures;
    checkBuildingUpdate(*legacy, migrated, given, world, enteringData(world.root, "snes").roomTemplate, failures,
                        "the migration");
    reportFailures(failures);
    // The titles unplaced for capacity are placed as additions of their chains: the heads' free slots (their reserve)
    // hold them all here (amiga's 139 of 235, mame's 7 of 8), so no expansion is needed yet.
    CHECK(capacityEntries(migrated.plan) == 0);
    CHECK(migrated.changes.added == overCapacity);
    CHECK(migrated.changes.roomsAdded.empty());
    CHECK(migrated.changes.migratedRooms.empty());
    CHECK(migrated.changes.needsConfirmation.empty());
    CHECK(migrated.changes.rebuild == rv::world::RebuildLevel::LocalReflow);
    // The heads keep every element (their reserve slots took titles), every other room byte for byte in its elements.
    for (const std::string_view id : {"area:arcade", "room:amiga"}) {
        INFO(id);
        const WorldPlan before = roomPart(*legacy, roomId(id));
        const WorldPlan after = roomPart(migrated.plan, roomId(id));
        CHECK(after.rooms == before.rooms);
        CHECK(after.fixtures == before.fixtures);
        CHECK(after.lights == before.lights);
        CHECK(std::includes(after.placements.begin(), after.placements.end(), before.placements.begin(),
                            before.placements.end()));
        CHECK(after.placements.size() > before.placements.size());
    }
    for (const std::string_view id : {"area:shared:handheld", "room:gb", "room:snes"}) {
        INFO(id);
        CHECK(roomPart(migrated.plan, roomId(id)) == roomPart(*legacy, roomId(id)));
    }
    // Twice one SHA-256; the next update writes nothing (once migrated, a chain's capacity entries would stay).
    CHECK(updateBuildingOf(world, *legacy, given).changes.digestAfter == migrated.changes.digestAfter);
    const GenerationResult again = updateBuildingOf(world, migrated.plan, given);
    CHECK(again.plan == migrated.plan);
    CHECK(again.changes.roomsAdded.empty());

    // More titles than the full head holds: the chain's next room, room:amiga:2, enters the computer floor with no
    // face-out title; every other room as it stood.
    std::vector<RoomSelection> more = given;
    std::set<std::uint64_t> taken;
    for (const RoomSelection& selection : more) {
        const std::set<std::uint64_t> values = gameValuesOf(selection);
        taken.insert(values.begin(), values.end());
    }
    rv::core::Random random(5201);
    for (int i = 0; i < 300; ++i) {
        addTitle(more[0], random, taken);
    }
    const GenerationResult expanded = updateBuildingOf(world, migrated.plan, more);
    INFO(hardFindings(expanded.validation));
    CHECK(expanded.validation.passed());
    failures.clear();
    checkBuildingUpdate(migrated.plan, expanded, more, world, enteringData(world.root, "snes").roomTemplate, failures,
                        "300 titles more");
    reportFailures(failures);
    CHECK(expanded.changes.roomsAdded == std::vector{roomId("room:amiga:2")});
    CHECK(expanded.changes.added.size() == 300);
    CHECK(capacityEntries(expanded.plan) == 0);
    const Room* expansion = findRoom(expanded.plan, "room:amiga:2");
    REQUIRE(expansion != nullptr);
    CHECK(expansion->expands == roomId("room:amiga"));
    CHECK(expansion->generatorVersion == rv::world::kGeneratorVersion);
    CHECK(expansion->floor == "computer");
    CHECK(faceOutIn(expanded.plan, expansion->id) == 0);
    CHECK_FALSE(placementsByRoom(expanded.plan).at(expansion->id).empty());
    for (const std::string_view id : {"area:arcade", "area:shared:handheld", "room:gb", "room:snes"}) {
        INFO(id);
        CHECK(roomPart(expanded.plan, roomId(id)) == roomPart(migrated.plan, roomId(id)));
    }
    CHECK(updateBuildingOf(world, expanded.plan, more).plan == expanded.plan);

    // "Rigenera stanza" of the chain: a fresh split into equal alphabetical shares, the expansion's face-out titles
    // chosen; the update after it writes nothing.
    const Regeneration regenerated =
        regenerateOf(world, expanded.plan, more, RegenerationScope::ofRoom(roomId("room:amiga:2")));
    checkFreshChain(regenerated.result.plan, roomId("room:amiga"), {&more[0]}, 2);
    CHECK(faceOutIn(regenerated.result.plan, roomId("room:amiga:2")) > 0);
    CHECK(regenerated.impact.roomsRegenerated == std::vector{roomId("room:amiga"), roomId("room:amiga:2")});
    CHECK(regenerated.impact.roomsAdded.empty());
    CHECK(regenerated.impact.roomsRemoved.empty());
    const GenerationResult after = updateBuildingOf(world, regenerated.result.plan, more);
    CHECK(after.plan == regenerated.result.plan);
    CHECK(regenerateOf(world, expanded.plan, more, RegenerationScope::ofRoom(roomId("room:amiga")))
              .result.changes.digestAfter == regenerated.result.changes.digestAfter);
    // The head's chain of the migrated building ("Rigenera stanza" of room:amiga): split anew, its expansion added.
    const Regeneration split =
        regenerateOf(world, migrated.plan, given, RegenerationScope::ofRoom(roomId("room:amiga")));
    checkFreshChain(split.result.plan, roomId("room:amiga"), {&given[0]}, 2);
    CHECK(split.impact.roomsAdded == std::vector{roomId("room:amiga:2")});
}

TEST_CASE("world/expansion: one fill order per chain, the head's freed slots used first", "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    std::vector<RoomSelection> given{systemSelection("mame", 60, 5100)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    const RoomId head = roomId("area:arcade");
    const RoomId expansion = roomId("area:arcade:2");
    REQUIRE(chainOf(plan, head).size() == 2);
    // A spine-out game of the head that is not its level's first: the title before it stands just before its slot.
    const auto sections = levelSections(plan);
    const Placement* freed = nullptr;
    for (const Placement& placement : plan.placements) {
        if (placement.slot.fixture().room() == head && placement.slot.index() > 0 &&
            sections.at({placement.slot.fixture(), placement.slot.level()}) == rv::world::Section::Spine) {
            freed = &placement;
            break;
        }
    }
    REQUIRE(freed != nullptr);
    RoomSelection& mame = given.front();
    const auto title = std::find_if(mame.titles.begin(), mame.titles.end(),
                                    [&](const RoomTitle& candidate) { return candidate.game == freed->game; });
    REQUIRE(title != mame.titles.end());
    const RoomTitle gone = *title;
    // The game becomes an alias of another title of the head (the catalog's identities): its slot is freed.
    const auto target = std::find_if(plan.placements.begin(), plan.placements.end(), [&](const Placement& placement) {
        return placement.slot.fixture().room() == head && placement.game != freed->game;
    });
    REQUIRE(target != plan.placements.end());
    mame.titles.erase(title);
    // And a new title takes its place in the alphabet: after the freed game's predecessor, in the head's freed slot,
    // though the expansion's slots are free too.
    std::set<std::uint64_t> taken = gameValuesOf(mame);
    taken.insert(freed->game.value());
    rv::core::Random random(17);
    RoomTitle fresh = gone;
    fresh.game = freshGameId(random, taken);
    fresh.favorite = false;
    fresh.playCount = 0;
    mame.titles.insert(std::upper_bound(mame.titles.begin(), mame.titles.end(), fresh, rv::world::priorityLess),
                       fresh);
    rv::world::BuildingUpdateInput input;
    input.library = library();
    input.selections = given;
    input.platformTags = world.tags;
    input.config = world.config;
    input.previous = plan;
    input.aliases = {{freed->game, target->game}};
    const std::vector<rv::world::WorldData> rooms{enteringData(world.root, "mame")};
    auto updated = rv::world::updateBuilding(input, world.building, world.systems, rooms);
    REQUIRE(updated.has_value());
    INFO(hardFindings(updated->validation));
    CHECK(updated->validation.passed());
    CHECK(updated->changes.slotsFreed == std::vector{freed->slot});
    CHECK(updated->changes.added == std::vector{fresh.game});
    const auto now = std::find_if(updated->plan.placements.begin(), updated->plan.placements.end(),
                                  [&](const Placement& placement) { return placement.game == fresh.game; });
    REQUIRE(now != updated->plan.placements.end());
    CHECK(now->slot == freed->slot);
    CHECK(placementsByRoom(updated->plan).at(expansion) == placementsByRoom(plan).at(expansion));

    // A title past every other in the alphabet follows the chain's last title, in the expansion.
    RoomSelection more = given.front();
    RoomTitle last = gone;
    last.game = freshGameId(random, taken);
    last.sortKey = "~~~ last";
    last.titleKey = "~~~ last";
    more.titles.insert(std::upper_bound(more.titles.begin(), more.titles.end(), last, rv::world::priorityLess), last);
    ++more.distinctTitles;
    input.selections = {more};
    input.previous = updated->plan;
    input.aliases.clear();
    auto after = rv::world::updateBuilding(input, world.building, world.systems, rooms);
    REQUIRE(after.has_value());
    const auto lastPlaced = std::find_if(after->plan.placements.begin(), after->plan.placements.end(),
                                         [&](const Placement& placement) { return placement.game == last.game; });
    REQUIRE(lastPlaced != after->plan.placements.end());
    CHECK(lastPlaced->slot.fixture().room() == expansion);
}

TEST_CASE("world/expansion: a full floor keeps the titles unplaced for capacity, naming the head",
          "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    world.building.layout.maxRoomsPerRow = 1; // two rooms a floor
    const std::vector<RoomSelection> given{systemSelection("amiga", 1700, 5200), systemSelection("dos", 40, 5600)};
    const GenerationResult full = generateBuildingOf(world, given);
    INFO(hardFindings(full.validation));
    CHECK(full.validation.passed());
    CHECK(findRoom(full.plan, "room:amiga:2") == nullptr);
    std::size_t capacity = 0;
    for (const rv::world::UnplacedGame& game : full.plan.unplaced) {
        if (game.reason == UnplacedReason::Capacity) {
            ++capacity;
            CHECK(game.room == roomId("room:amiga"));
        }
    }
    CHECK(capacity == 1700 - 1561);
    // The update changes nothing: a title unplaced for capacity stays so until a regeneration.
    const GenerationResult same = updateBuildingOf(world, full.plan, given);
    CHECK(same.plan == full.plan);
    // A floor with room: "Rigenera stanza" splits the chain, its expansion entering the floor.
    world.building.layout.maxRoomsPerRow = 12;
    const Regeneration room = regenerateOf(world, full.plan, given, RegenerationScope::ofRoom(roomId("room:amiga")));
    CHECK(capacityEntries(room.result.plan) == 0);
    CHECK(room.impact.roomsAdded == std::vector{roomId("room:amiga:2")});
    checkFreshChain(room.result.plan, roomId("room:amiga"), {&given[0]}, 2);
}

TEST_CASE("world/expansion: no title unplaced for capacity while the floor has room (random arcade sequences)",
          "[world][expansion]") {
    // Two arcade systems in the arcade room (53 titles at its limit), titles added by bursts; each update keeps the
    // building's invariants and places every title; then the chain regenerated in equal shares, the update after it
    // writing nothing, twice one SHA-256.
    BuildingWorld world = shippedWorld(kWorldSeed);
    const RoomId head = roomId("area:arcade");
    const std::uint64_t limit = 53;
    for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}}) {
        INFO("run " << seed);
        rv::core::Random random(seed);
        std::vector<RoomSelection> given{systemSelection("mame", 20 + random.below(60), 5100 + seed),
                                         systemSelection("fbneo", random.below(40), 5150 + seed)};
        std::set<std::uint64_t> taken;
        for (const RoomSelection& selection : given) {
            const std::set<std::uint64_t> values = gameValuesOf(selection);
            taken.insert(values.begin(), values.end());
        }
        WorldPlan plan = generateBuildingOf(world, given).plan;
        std::vector<std::string> failures;
        for (int step = 0; step < 6; ++step) {
            for (RoomSelection& selection : given) {
                const std::uint64_t count = random.below(40);
                for (std::uint64_t i = 0; i < count; ++i) {
                    addTitle(selection, random, taken, random.below(10) == 0, random.below(5));
                }
            }
            const GenerationResult updated = updateBuildingOf(world, plan, given);
            checkBuildingUpdate(plan, updated, given, world, enteringData(world.root, "mame").roomTemplate, failures,
                                fmt::format("run {} step {}", seed, step));
            if (capacityEntries(updated.plan) != 0) {
                failures.push_back(fmt::format("run {} step {}: titles unplaced for capacity", seed, step));
            }
            plan = updated.plan;
        }
        reportFailures(failures);
        // "Rigenera stanza": the fewest rooms, equal alphabetical shares; the update after writes nothing.
        const std::uint64_t titles = given[0].titles.size() + given[1].titles.size();
        const Regeneration regenerated = regenerateOf(world, plan, given, RegenerationScope::ofRoom(head));
        checkFreshChain(regenerated.result.plan, head, {&given[0], &given[1]},
                        static_cast<std::size_t>((titles + limit - 1) / limit));
        CHECK(capacityEntries(regenerated.result.plan) == 0);
        CHECK(updateBuildingOf(world, regenerated.result.plan, given).plan == regenerated.result.plan);
        CHECK(regenerateOf(world, plan, given, RegenerationScope::ofRoom(head)).result.changes.digestAfter ==
              regenerated.result.changes.digestAfter);
    }
}

TEST_CASE("world/expansion: a game moved into the expansion is pinned there through the chain's regeneration",
          "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> given{systemSelection("mame", 60, 5100)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    const RoomId head = roomId("area:arcade");
    const RoomId expansion = roomId("area:arcade:2");
    // A game of the head moved into a free slot of the expansion (within its chain, ADR-0015 §4).
    std::set<rv::world::SlotId> used;
    for (const Placement& placement : plan.placements) {
        used.insert(placement.slot);
    }
    const auto sections = levelSections(plan);
    rv::world::SlotId target;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            for (std::uint32_t index = 0; index < level.slots && target.empty(); ++index) {
                const auto slot = *rv::world::SlotId::make(fixture.id, level.level, index);
                if (fixture.id.room() == expansion && !used.contains(slot) &&
                    level.section == rv::world::Section::Spine) {
                    target = slot;
                }
            }
        }
    }
    REQUIRE_FALSE(target.empty());
    const auto moved = std::find_if(plan.placements.begin(), plan.placements.end(), [&](const Placement& placement) {
        return placement.slot.fixture().room() == head &&
               sections.at({placement.slot.fixture(), placement.slot.level()}) == rv::world::Section::Spine;
    });
    REQUIRE(moved != plan.placements.end());
    rv::world::EditedWorld edited{plan, {}};
    rv::world::EditData data;
    data.config = world.config;
    data.building = world.building;
    const Room* headRoom = findRoom(plan, "area:arcade");
    REQUIRE(headRoom != nullptr);
    const auto shared = std::make_shared<const rv::world::WorldData>(storedData(world.root, *headRoom));
    data.rooms.emplace(head, shared);
    data.rooms.emplace(expansion, shared);
    const rv::world::PinProvenance provenance{rv::world::PinOrigin::Editor, "2026-10-28T10:00:00.000Z"};
    auto result =
        rv::world::applyEdit(edited, rv::world::EditOperation::move(moved->game, target), data, provenance);
    REQUIRE(result.has_value());
    INFO((result->refused ? result->refused->rule + ": " + result->refused->message : std::string("applied")));
    REQUIRE_FALSE(result->refused.has_value());
    REQUIRE(edited.overrides.gamePins.size() == 1);

    // "Rigenera stanza" of the chain honours the pin: the game stands in the expansion's slot.
    const Regeneration regenerated =
        regenerateOf(world, edited.plan, given, RegenerationScope::ofRoom(head), edited.overrides);
    CHECK(regenerated.impact.conflicts.empty());
    CHECK(regenerated.impact.pinsKept.size() == 1);
    const auto standing =
        std::find_if(regenerated.result.plan.placements.begin(), regenerated.result.plan.placements.end(),
                     [&](const Placement& placement) { return placement.game == moved->game; });
    REQUIRE(standing != regenerated.result.plan.placements.end());
    CHECK(standing->slot == target);
    CHECK(capacityEntries(regenerated.result.plan) == 0);
    // The update after it writes nothing.
    rv::world::BuildingUpdateInput input;
    input.library = library();
    input.selections = given;
    input.platformTags = world.tags;
    input.config = world.config;
    input.overrides = regenerated.overrides;
    input.previous = regenerated.result.plan;
    const std::vector<rv::world::WorldData> rooms{enteringData(world.root, "mame")};
    auto updated = rv::world::updateBuilding(input, world.building, world.systems, rooms);
    REQUIRE(updated.has_value());
    CHECK(updated->plan == regenerated.result.plan);
}
