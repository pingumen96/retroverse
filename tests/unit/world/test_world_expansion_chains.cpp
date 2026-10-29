// Expansion rooms after I4b step 7's review (ADR-0015 §7 as amended on 2026-10-29), with the shipped data: a pinned
// game counts in its room's equal share (no title unplaced for capacity because of a pin while the floor has room);
// expansion numbers never reused while the overrides name them (a chain shrunk by a regeneration, its pins room-gone,
// then grown by an update and regenerated again: the numbers after them, a gap the validation accepts); stale pins
// (room-gone) neither refusing growth nor pulling titles; a room scope the plan lacks and an expansion without its
// head refused; a pin refusing a growth in the head or in an expansion (no expansion then); a stored chain's next room
// and a new system's room competing for a floor's last place; a shared room's chain and a new section entering it;
// generator 1.4's capacity entries on a full floor migrated in part, a chain held at its first update and migrated
// later; an expansion whose titles all left the catalog standing as it was.

#include "world_expansion_support.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/regeneration.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/validation.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::world::ConflictReason;
using rv::world::Fixture;
using rv::world::FixturePin;
using rv::world::GamePin;
using rv::world::GenerationResult;
using rv::world::Placement;
using rv::world::Regeneration;
using rv::world::RegenerationScope;
using rv::world::Room;
using rv::world::RoomId;
using rv::world::RoomSelection;
using rv::world::RoomTitle;
using rv::world::SlotId;
using rv::world::UnplacedReason;
using rv::world::WorldOverrides;
using rv::world::WorldPlan;

namespace {

using namespace rv::test::world;

constexpr std::uint64_t kWorldSeed = 102938;

const rv::world::PinProvenance kProvenance{rv::world::PinOrigin::Cli, "2026-10-29T10:00:00.000Z"};

/// The free spine-out slots of `room` in `plan`, in id order.
std::vector<SlotId> freeSpineSlots(const WorldPlan& plan, const RoomId& room) {
    std::set<SlotId> used;
    for (const Placement& placement : plan.placements) {
        used.insert(placement.slot);
    }
    std::vector<SlotId> free;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != room) {
            continue;
        }
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            for (std::uint32_t index = 0; index < level.slots; ++index) {
                const SlotId slot = *SlotId::make(fixture.id, level.level, index);
                if (level.section == rv::world::Section::Spine && !used.contains(slot)) {
                    free.push_back(slot);
                }
            }
        }
    }
    std::sort(free.begin(), free.end());
    return free;
}

/// The placements of `room` in `plan`, by slot.
std::vector<Placement> placementsOf(const WorldPlan& plan, const RoomId& room) {
    std::vector<Placement> placements;
    for (const Placement& placement : plan.placements) {
        if (placement.slot.fixture().room() == room) {
            placements.push_back(placement);
        }
    }
    return placements;
}

/// Where `game` stands in `plan`, or null.
const Placement* placementOf(const WorldPlan& plan, const GameId& game) {
    const auto found = std::find_if(plan.placements.begin(), plan.placements.end(),
                                    [&](const Placement& placement) { return placement.game == game; });
    return found != plan.placements.end() ? &*found : nullptr;
}

/// The fixture of `room` standing in a south-wall socket of `roomTemplate` (a decoration there), REQUIREd.
const Fixture& southWallFixture(const WorldPlan& plan, const RoomId& room,
                                const rv::world::RoomTemplate& roomTemplate) {
    const auto found = std::find_if(plan.fixtures.begin(), plan.fixtures.end(), [&](const Fixture& fixture) {
        const rv::world::TemplateSocket* socket = roomTemplate.socket(fixture.id.socket());
        return fixture.id.room() == room && fixture.id.part().empty() && socket != nullptr &&
               socket->wall == rv::world::Wall::South;
    });
    INFO("a fixture of " << room.toString() << " in a south-wall socket");
    REQUIRE(found != plan.fixtures.end());
    return *found;
}

/// `selection` with `count` new invented titles (random of `seed`, GameIds none of `taken` holds).
RoomSelection withTitles(RoomSelection selection, std::size_t count, std::uint64_t seed,
                         std::set<std::uint64_t>& taken) {
    rv::core::Random random(seed);
    for (std::size_t i = 0; i < count; ++i) {
        addTitle(selection, random, taken);
    }
    return selection;
}

std::set<std::uint64_t> takenBy(const std::vector<RoomSelection>& selections) {
    std::set<std::uint64_t> taken;
    for (const RoomSelection& selection : selections) {
        const std::set<std::uint64_t> values = gameValuesOf(selection);
        taken.insert(values.begin(), values.end());
    }
    return taken;
}

/// `plan` without the room `id` and its elements.
WorldPlan withoutRoom(WorldPlan plan, const RoomId& id) {
    std::erase_if(plan.rooms, [&](const Room& room) { return room.id == id; });
    std::erase_if(plan.fixtures, [&](const Fixture& fixture) { return fixture.id.room() == id; });
    std::erase_if(plan.placements, [&](const Placement& placement) { return placement.slot.fixture().room() == id; });
    std::erase_if(plan.dressing, [&](const auto& dressed) { return dressed.level.fixture().room() == id; });
    std::erase_if(plan.lights, [&](const auto& light) { return light.id.room() == id; });
    std::erase_if(plan.unplaced, [&](const auto& game) { return game.room == id; });
    return plan;
}

/// `plan` as generator 1.4 recorded it: the same rooms, every stamp 1.4 (I4b step 7's chains never stood in such a
/// plan: the callers give a plan of no expansion).
WorldPlan stampedOneFour(WorldPlan plan) {
    constexpr rv::world::GeneratorVersion kOneFour{1, 4};
    plan.generatorVersion = kOneFour;
    for (Room& room : plan.rooms) {
        REQUIRE_FALSE(room.id.isExpansion());
        room.generatorVersion = kOneFour;
    }
    return plan;
}

bool anyContains(const std::vector<std::string>& lines, std::string_view part) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& line) { return line.find(part) != std::string::npos; });
}

} // namespace

TEST_CASE("world/expansion chains: a pinned game counts in its room's equal share", "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const RoomId head = roomId("area:arcade");
    const RoomId expansion = roomId("area:arcade:2");

    SECTION("an arcade of 106 titles, one game of the expansion pinned into the head: 53 and 53, none for capacity") {
        const std::vector<RoomSelection> given{systemSelection("mame", 106, 5100)};
        const WorldPlan plan = generateBuildingOf(world, given).plan;
        REQUIRE(chainIds(plan, head) == std::vector{head, expansion});
        REQUIRE(placedIn(plan, head) == 53);
        REQUIRE(placedIn(plan, expansion) == 53);
        const Placement moved = placementsOf(plan, expansion).front();
        const std::vector<SlotId> free = freeSpineSlots(plan, head);
        REQUIRE_FALSE(free.empty());
        WorldOverrides overrides;
        overrides.gamePins.push_back({moved.game, free.front(), kProvenance, std::nullopt});

        const Regeneration regenerated =
            regenerateOf(world, plan, given, RegenerationScope::ofRoom(head), overrides);
        const WorldPlan& after = regenerated.result.plan;
        CHECK(regenerated.impact.conflicts.empty());
        CHECK(regenerated.impact.pinsKept.size() == 1);
        CHECK(capacityEntries(after) == 0);
        CHECK(placedIn(after, head) == 53);
        CHECK(placedIn(after, expansion) == 53);
        REQUIRE(placementOf(after, moved.game) != nullptr);
        CHECK(placementOf(after, moved.game)->slot == free.front());
        CHECK(regenerated.impact.roomsAdded.empty());
        // The update after it writes nothing.
        const GenerationResult updated =
            updateBuildingOf(world, after, given, StoredRoomData::Recorded, regenerated.overrides);
        CHECK(updated.plan == after);
    }

    SECTION("pins outnumbering the head's share: the head takes them all, the expansion the rest") {
        // 60 titles, 30 and 30: the head's 30 games pinned where they stand and one of the expansion's into the head.
        const std::vector<RoomSelection> given{systemSelection("mame", 60, 5100)};
        const WorldPlan plan = generateBuildingOf(world, given).plan;
        REQUIRE(placedIn(plan, head) == 30);
        WorldOverrides overrides;
        for (const Placement& placement : placementsOf(plan, head)) {
            overrides.gamePins.push_back({placement.game, placement.slot, kProvenance, std::nullopt});
        }
        const Placement moved = placementsOf(plan, expansion).back();
        const std::vector<SlotId> free = freeSpineSlots(plan, head);
        REQUIRE_FALSE(free.empty());
        overrides.gamePins.push_back({moved.game, free.front(), kProvenance, std::nullopt});
        const Regeneration regenerated =
            regenerateOf(world, plan, given, RegenerationScope::ofRoom(head), overrides);
        const WorldPlan& after = regenerated.result.plan;
        CHECK(regenerated.impact.conflicts.empty());
        CHECK(regenerated.impact.pinsKept.size() == 31);
        CHECK(capacityEntries(after) == 0);
        CHECK(placedIn(after, head) == 31);
        CHECK(placedIn(after, expansion) == 29);
        CHECK(chainIds(after, head) == std::vector{head, expansion});
    }

    SECTION("a pinned game without a cover in the expansion: its own unplaced reason") {
        const std::vector<RoomSelection> given{systemSelection("mame", 60, 5100)};
        const auto noCover = std::find_if(given[0].unplaced.begin(), given[0].unplaced.end(), [](const auto& game) {
            return game.reason == UnplacedReason::NoCover;
        });
        REQUIRE(noCover != given[0].unplaced.end());
        const WorldPlan plan = generateBuildingOf(world, given).plan;
        const std::vector<SlotId> free = freeSpineSlots(plan, expansion);
        REQUIRE_FALSE(free.empty());
        WorldOverrides overrides;
        overrides.gamePins.push_back({noCover->game, free.front(), kProvenance, std::nullopt});
        const Regeneration regenerated =
            regenerateOf(world, plan, given, RegenerationScope::ofRoom(head), overrides);
        REQUIRE(regenerated.impact.conflicts.size() == 1);
        CHECK(regenerated.impact.conflicts[0].reason == ConflictReason::GameGone);
        CHECK(regenerated.impact.conflicts[0].message == "the game is unplaced (no-cover)");
    }
}

TEST_CASE("world/expansion chains: pins alone past a room's limit take no further room", "[world][expansion]") {
    // The head's 53 games pinned where they stand and one of the expansion's pinned into the head: 54 pinned games,
    // past the 53 a room holds with the reserve. The head's share is its pinned games, which its reserve's slots hold
    // (the pins win over the reserve); the expansion holds every other title, its share only shrank. A further room
    // would hold nothing a pin allows to move: none is taken, and no title is unplaced for capacity.
    BuildingWorld world = shippedWorld(kWorldSeed);
    const RoomId head = roomId("area:arcade");
    const RoomId expansion = roomId("area:arcade:2");
    const std::vector<RoomSelection> given{systemSelection("mame", 106, 5100)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    WorldOverrides overrides;
    for (const Placement& placement : placementsOf(plan, head)) {
        overrides.gamePins.push_back({placement.game, placement.slot, kProvenance, std::nullopt});
    }
    const Placement moved = placementsOf(plan, expansion).front();
    const std::vector<SlotId> free = freeSpineSlots(plan, head);
    REQUIRE_FALSE(free.empty());
    overrides.gamePins.push_back({moved.game, free.front(), kProvenance, std::nullopt});
    const Regeneration regenerated = regenerateOf(world, plan, given, RegenerationScope::ofRoom(head), overrides);
    const WorldPlan& after = regenerated.result.plan;
    CHECK(chainIds(after, head) == std::vector{head, expansion});
    CHECK(regenerated.impact.roomsAdded.empty());
    CHECK(regenerated.impact.conflicts.empty());
    CHECK(regenerated.impact.pinsKept.size() == 54);
    CHECK(placedIn(after, head) == 54);
    CHECK(placedIn(after, expansion) == 52);
    CHECK(capacityEntries(after) == 0);
}

TEST_CASE("world/expansion chains: a number the overrides name is never given again", "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const RoomId head = roomId("area:arcade");
    const RoomId second = roomId("area:arcade:2");
    const RoomId third = roomId("area:arcade:3");
    const RoomId fourth = roomId("area:arcade:4");
    const RoomSelection full = systemSelection("mame", 150, 5100);
    const WorldPlan plan = generateBuildingOf(world, {full}).plan;
    REQUIRE(chainIds(plan, head) == std::vector{head, second, third});
    // A game of area:arcade:3 pinned where it stands.
    const Placement pinnedThere = placementsOf(plan, third).front();
    WorldOverrides overrides;
    overrides.gamePins.push_back({pinnedThere.game, pinnedThere.slot, kProvenance, std::nullopt});

    // The catalog shrinks to 100 titles (the pinned game kept): "Rigenera stanza" drops the highest number, its pin
    // recorded room-gone.
    std::size_t kept = 0;
    const RoomSelection fewer = subsetSelection(full, [&](const GameId& game) {
        return game == pinnedThere.game || (kept < 99 && ++kept > 0);
    });
    REQUIRE(fewer.titles.size() == 100);
    const Regeneration shrunk = regenerateOf(world, plan, {fewer}, RegenerationScope::ofRoom(head), overrides);
    CHECK(chainIds(shrunk.result.plan, head) == std::vector{head, second});
    CHECK(shrunk.impact.roomsRemoved == std::vector{third});
    REQUIRE(shrunk.impact.conflicts.size() == 1);
    CHECK(shrunk.impact.conflicts[0].reason == ConflictReason::RoomGone);
    REQUIRE(shrunk.overrides.gamePins.size() == 1);
    CHECK(shrunk.overrides.gamePins[0].conflict == ConflictReason::RoomGone);
    // The game stands in its share all the same.
    CHECK(placementOf(shrunk.result.plan, pinnedThere.game) != nullptr);

    // An update whose titles the two rooms no longer hold: the chain's next room is area:arcade:4, area:arcade:3 being
    // named by the overrides; the chain's numbers leave a gap the validation accepts.
    std::set<std::uint64_t> taken = takenBy({full});
    const RoomSelection more = withTitles(fewer, 40, 31, taken);
    const GenerationResult grown =
        updateBuildingOf(world, shrunk.result.plan, {more}, StoredRoomData::Recorded, shrunk.overrides);
    INFO(hardFindings(grown.validation));
    CHECK(grown.validation.passed());
    CHECK(grown.changes.roomsAdded == std::vector{fourth});
    CHECK(chainIds(grown.plan, head) == std::vector{head, second, fourth});
    CHECK(capacityEntries(grown.plan) == 0);
    CHECK(findRoom(grown.plan, "area:arcade:4")->expands == head);
    // The stale pin is reported as orphaned (its room is gone), never as needing a confirmation.
    CHECK(grown.changes.needsConfirmation.empty());
    CHECK_FALSE(grown.changes.orphanedPins.empty());

    // "Rigenera stanza" of the grown chain: its standing rooms kept by number, area:arcade:3 still not given.
    const Regeneration again =
        regenerateOf(world, grown.plan, {more}, RegenerationScope::ofRoom(fourth), shrunk.overrides);
    CHECK(chainIds(again.result.plan, head) == std::vector{head, second, fourth});
    CHECK(again.impact.roomsAdded.empty());
    REQUIRE(again.overrides.gamePins.size() == 1);
    CHECK(again.overrides.gamePins[0].conflict == ConflictReason::RoomGone);
    // A regeneration of the first plan with the overrides that name area:arcade:3 and a catalog that needs a fourth
    // room: its standing area:arcade:3 is kept by number, its new one numbered after it.
    const RoomSelection most = withTitles(full, 60, 32, taken);
    const Regeneration widened = regenerateOf(world, plan, {most}, RegenerationScope::ofRoom(head), overrides);
    CHECK(chainIds(widened.result.plan, head) == std::vector{head, second, third, fourth});

    // Once the overrides are dropped, nothing names area:arcade:3: a later chain may take it again (the documented
    // caveat: video.json's rooms.<id> settings would then apply to the new room).
    const GenerationResult unnamed = updateBuildingOf(world, shrunk.result.plan, {more});
    CHECK(unnamed.changes.roomsAdded == std::vector{third});
}

TEST_CASE("world/expansion chains: a stale pin neither refuses growth nor pulls titles", "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const RoomId head = roomId("area:arcade");
    const RoomId expansion = roomId("area:arcade:2");
    const rv::world::RoomTemplate& roomTemplate = enteringData(world.root, "mame").roomTemplate;
    const std::vector<RoomSelection> given{systemSelection("mame", 60, 5100)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    REQUIRE(chainIds(plan, head) == std::vector{head, expansion});
    std::set<std::uint64_t> taken = takenBy(given);
    const std::vector<RoomSelection> more{withTitles(given[0], 60, 41, taken)};
    const Fixture& south = southWallFixture(plan, head, roomTemplate);

    SECTION("a fixture pinned in the head's south-wall socket refuses the head's growth: no expansion") {
        WorldOverrides overrides;
        overrides.fixturePins.push_back({south.id, std::nullopt, std::nullopt, kProvenance, std::nullopt});
        const GenerationResult updated =
            updateBuildingOf(world, plan, more, StoredRoomData::Recorded, overrides);
        INFO(hardFindings(updated.validation));
        CHECK(updated.validation.passed());
        CHECK(updated.changes.roomsAdded.empty());
        CHECK(findRoom(updated.plan, "area:arcade:3") == nullptr);
        CHECK(findRoom(updated.plan, "area:arcade")->growthSteps == 0);
        CHECK(findRoom(updated.plan, "area:arcade:2")->growthSteps == roomTemplate.maxGrowthSteps());
        CHECK(capacityEntries(updated.plan) > 0);
        CHECK(anyContains(updated.changes.needsConfirmation,
                          "growth: the room would grow and move the pinned fixture of socket " + south.id.socket()));
    }
    SECTION("the same pin stale (room-gone): the head grows, nothing refused") {
        WorldOverrides overrides;
        overrides.fixturePins.push_back(
            {south.id, std::nullopt, std::nullopt, kProvenance, ConflictReason::RoomGone});
        const GenerationResult updated =
            updateBuildingOf(world, plan, more, StoredRoomData::Recorded, overrides);
        INFO(hardFindings(updated.validation));
        CHECK(updated.validation.passed());
        CHECK(findRoom(updated.plan, "area:arcade")->growthSteps > 0);
        CHECK(capacityEntries(updated.plan) == 0);
        CHECK(updated.changes.needsConfirmation.empty());
    }
    SECTION("a stale game pin naming a head's slot: the regeneration leaves the game in its share") {
        const Placement game = placementsOf(plan, expansion).front();
        const std::vector<SlotId> free = freeSpineSlots(plan, head);
        REQUIRE_FALSE(free.empty());
        WorldOverrides overrides;
        overrides.gamePins.push_back({game.game, free.front(), kProvenance, ConflictReason::RoomGone});
        const Regeneration regenerated =
            regenerateOf(world, plan, given, RegenerationScope::ofRoom(head), overrides);
        CHECK(regenerated.result.plan == plan);
        CHECK(placementOf(regenerated.result.plan, game.game)->slot.fixture().room() == expansion);
        REQUIRE(regenerated.impact.conflicts.size() == 1);
        CHECK(regenerated.impact.conflicts[0].reason == ConflictReason::RoomGone);
        CHECK(regenerated.overrides.gamePins[0].conflict == ConflictReason::RoomGone);
    }
}

TEST_CASE("world/expansion chains: a fixture pinned in an expansion during an update", "[world][expansion]") {
    // The expansion's south-wall fixture pinned in its socket: the head grows to its limit, the expansion's growth is
    // refused (named with its room), so no room of the chain past the head is at its limit: no expansion, the titles
    // over capacity naming the head.
    BuildingWorld world = shippedWorld(kWorldSeed);
    const RoomId head = roomId("area:arcade");
    const RoomId expansion = roomId("area:arcade:2");
    const rv::world::RoomTemplate& roomTemplate = enteringData(world.root, "mame").roomTemplate;
    const std::vector<RoomSelection> given{systemSelection("mame", 60, 5100)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    std::set<std::uint64_t> taken = takenBy(given);
    const std::vector<RoomSelection> more{withTitles(given[0], 60, 43, taken)};
    const Fixture south = southWallFixture(plan, expansion, roomTemplate);
    WorldOverrides overrides;
    overrides.fixturePins.push_back({south.id, std::nullopt, std::nullopt, kProvenance, std::nullopt});
    const GenerationResult updated = updateBuildingOf(world, plan, more, StoredRoomData::Recorded, overrides);
    INFO(hardFindings(updated.validation));
    CHECK(updated.validation.passed());
    CHECK(updated.changes.roomsAdded.empty());
    CHECK(findRoom(updated.plan, "area:arcade")->growthSteps == roomTemplate.maxGrowthSteps());
    CHECK(findRoom(updated.plan, "area:arcade:2")->growthSteps == 0);
    CHECK(anyContains(updated.changes.needsConfirmation,
                      "of socket " + south.id.socket() + " of area:arcade:2; the titles that need it"));
    // The pinned fixture stands as it stood; every capacity entry names the head.
    const auto standing = std::find_if(updated.plan.fixtures.begin(), updated.plan.fixtures.end(),
                                       [&](const Fixture& fixture) { return fixture.id == south.id; });
    REQUIRE(standing != updated.plan.fixtures.end());
    CHECK(*standing == south);
    CHECK(capacityEntries(updated.plan) > 0);
    for (const rv::world::UnplacedGame& game : updated.plan.unplaced) {
        CHECK(game.room == head);
    }
}

TEST_CASE("world/expansion chains: a room scope the plan lacks, an expansion without its head", "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> given{systemSelection("mame", 60, 5100), systemSelection("snes", 40, 5300)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    REQUIRE(findRoom(plan, "area:arcade:2") != nullptr);
    // An expansion's number the plan lacks: refused, not taken for its head's chain.
    auto missing = rv::world::regenerate(
        regenerationInput(world, plan, given, RegenerationScope::ofRoom(roomId("area:arcade:3"))), world.building,
        world.systems, expansionLoader());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_THAT(missing.error().message, ContainsSubstring("the plan holds no room area:arcade:3"));

    // A plan holding area:arcade:2 without area:arcade (edited by hand): the update and every regeneration refuse it.
    const WorldPlan headless = withoutRoom(plan, roomId("area:arcade"));
    rv::world::BuildingUpdateInput input;
    input.library = library();
    input.selections = given;
    input.platformTags = world.tags;
    input.config = world.config;
    input.previous = headless;
    std::vector<rv::world::WorldData> rooms;
    for (const Room& room : headless.rooms) {
        rooms.push_back(storedData(world.root, room));
    }
    auto updated = rv::world::updateBuilding(input, world.building, world.systems, rooms);
    REQUIRE_FALSE(updated.has_value());
    CHECK(updated.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_THAT(updated.error().message,
               ContainsSubstring("the plan holds the expansion area:arcade:2 without its head area:arcade"));
    for (const RegenerationScope& scope :
         {RegenerationScope::ofRoom(roomId("room:snes")), RegenerationScope::ofFloor("ground"),
          RegenerationScope::ofWorld()}) {
        INFO(scope.toString());
        auto regenerated = rv::world::regenerate(regenerationInput(world, headless, given, scope), world.building,
                                                 world.systems, expansionLoader());
        REQUIRE_FALSE(regenerated.has_value());
        CHECK(regenerated.error().code == rv::ErrorCode::InvalidArgument);
        CHECK_THAT(regenerated.error().message, ContainsSubstring("without its head area:arcade"));
    }
}

TEST_CASE("world/expansion chains: a stored chain's next room and a new system's room compete for the last place",
          "[world][expansion]") {
    // The computer floor holding four rooms (two a row): amiga's chain at its limit (two rooms of 1,561 titles) and
    // dos's room stand, one place left. An update brings amiga titles past the chain's reserve and a new system, pc:
    // the new system enters first (ADR-0013 §7) and takes the place; amiga's titles past its rooms are unplaced for
    // capacity, naming the head, until a floor has room.
    BuildingWorld world = shippedWorld(kWorldSeed);
    world.building.layout.maxRoomsPerRow = 2;
    const std::vector<RoomSelection> given{systemSelection("amiga", 3122, 5200), systemSelection("dos", 40, 5600)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    REQUIRE(chainIds(plan, roomId("room:amiga")) == std::vector{roomId("room:amiga"), roomId("room:amiga:2")});
    REQUIRE(capacityEntries(plan) == 0);
    std::set<std::uint64_t> taken = takenBy(given);
    std::vector<RoomSelection> more{withTitles(given[0], 600, 51, taken), given[1], systemSelection("pc", 40, 5700)};
    const GenerationResult updated = updateBuildingOf(world, plan, more);
    INFO(hardFindings(updated.validation));
    CHECK(updated.validation.passed());
    CHECK(updated.changes.roomsAdded == std::vector{roomId("room:pc")});
    CHECK(findRoom(updated.plan, "room:amiga:3") == nullptr);
    CHECK(findRoom(updated.plan, "room:pc")->floor == "computer");
    const std::size_t capacity = capacityEntries(updated.plan);
    CHECK(capacity > 0);
    CHECK(capacity < 600);
    for (const rv::world::UnplacedGame& game : updated.plan.unplaced) {
        if (game.reason == UnplacedReason::Capacity) {
            CHECK(game.room == roomId("room:amiga"));
        }
    }
    // The next update of the same catalog writes nothing.
    CHECK(updateBuildingOf(world, updated.plan, more).plan == updated.plan);
}

TEST_CASE("world/expansion chains: a shared room's chain, a new section entering it", "[world][expansion]") {
    // Every handheld system in the floor's shared room (sharedRoomMinTitles past their counts): 7,000 titles of
    // generic cases are two rooms, area:shared:handheld and area:shared:handheld:2, each of both sections.
    BuildingWorld world = shippedWorld(kWorldSeed);
    world.config.sharedRoomMinTitles = 100000;
    const RoomId head = roomId("area:shared:handheld");
    const RoomId expansion = roomId("area:shared:handheld:2");
    const std::vector<RoomSelection> given{systemSelection("gb", 3500, 5400), systemSelection("gbc", 3500, 5401)};
    const GenerationResult generated = generateBuildingOf(world, given);
    INFO(hardFindings(generated.validation));
    CHECK(generated.validation.passed());
    const WorldPlan& plan = generated.plan;
    checkFreshChain(plan, head, {&given[0], &given[1]}, 2);
    CHECK(findRoom(plan, "area:shared:handheld:2")->sections == std::vector{systemName("gb"), systemName("gbc")});
    CHECK(capacityEntries(plan) == 0);

    // gba enters: a section appended to every room of the stored chain, its titles after gbc's in the chain's fill
    // order, the chain's records otherwise as they stood.
    std::vector<RoomSelection> more = given;
    more.push_back(systemSelection("gba", 500, 5402));
    const GenerationResult updated = updateBuildingOf(world, plan, more);
    INFO(hardFindings(updated.validation));
    CHECK(updated.validation.passed());
    std::vector<std::string> failures;
    checkBuildingUpdate(plan, updated, more, world, enteringData(world.root, "gb").roomTemplate, failures,
                        "gba entering");
    reportFailures(failures);
    CHECK(updated.changes.systemsEntered == std::vector{rv::world::SystemRoom{systemName("gba"), head}});
    CHECK(updated.changes.roomsAdded.empty());
    for (const RoomId& room : {head, expansion}) {
        CHECK(findRoom(updated.plan, room.toString())->sections ==
              std::vector{systemName("gb"), systemName("gbc"), systemName("gba")});
    }
    CHECK(capacityEntries(updated.plan) == 0);
    std::map<std::string, std::size_t> gbaRooms;
    for (const RoomTitle& title : more[2].titles) {
        const Placement* placement = placementOf(updated.plan, title.game);
        REQUIRE(placement != nullptr);
        ++gbaRooms[placement->slot.fixture().room().toString()];
    }
    // gba's first titles follow gbc's last in the expansion; the chain's one fill order then wraps to the head's free
    // slots before a module or a growth step: every one of them stands in the chain, the head's placements kept.
    CHECK(gbaRooms.size() == 2);
    CHECK(gbaRooms["area:shared:handheld"] + gbaRooms["area:shared:handheld:2"] == 500);
    const std::vector<Placement> headBefore = placementsOf(plan, head);
    const std::vector<Placement> headAfter = placementsOf(updated.plan, head);
    CHECK(std::includes(headAfter.begin(), headAfter.end(), headBefore.begin(), headBefore.end()));
    CHECK(updateBuildingOf(world, updated.plan, more).plan == updated.plan);
}

TEST_CASE("world/expansion chains: generator 1.4's capacity entries on a full floor migrate in part",
          "[world][expansion]") {
    // Two rooms a floor: amiga's room of 2,000 titles at its limit beside dos's, 439 titles unplaced for capacity, as
    // generator 1.4 left them. The first update by 1.5 gives them the head's free slots (its reserve); the floor has no
    // place for an expansion, so the rest stay unplaced for capacity, naming the head; the next update writes nothing.
    BuildingWorld world = shippedWorld(kWorldSeed);
    world.building.layout.maxRoomsPerRow = 1;
    const std::vector<RoomSelection> given{systemSelection("amiga", 2000, 5200), systemSelection("dos", 40, 5600)};
    const WorldPlan legacy = stampedOneFour(generateBuildingOf(world, given).plan);
    REQUIRE(capacityEntries(legacy) == 2000 - 1561);
    const std::vector<SlotId> free = freeSpineSlots(legacy, roomId("room:amiga"));
    REQUIRE(free.size() < 2000 - 1561);

    const GenerationResult migrated = updateBuildingOf(world, legacy, given);
    INFO(hardFindings(migrated.validation));
    CHECK(migrated.validation.passed());
    CHECK(migrated.changes.added.size() == free.size());
    CHECK(capacityEntries(migrated.plan) == 2000 - 1561 - free.size());
    CHECK(migrated.changes.roomsAdded.empty());
    for (const rv::world::UnplacedGame& game : migrated.plan.unplaced) {
        if (game.reason == UnplacedReason::Capacity) {
            CHECK(game.room == roomId("room:amiga"));
        }
    }
    CHECK(roomPart(migrated.plan, roomId("room:dos")) == roomPart(legacy, roomId("room:dos")));
    // Since then a title unplaced for capacity stays so until a regeneration.
    const GenerationResult again = updateBuildingOf(world, migrated.plan, given);
    CHECK(again.plan == migrated.plan);
    CHECK(again.changes.digestAfter == again.changes.digestBefore);
}

TEST_CASE("world/expansion chains: a chain held at its first update by 1.5 is migrated later", "[world][expansion]") {
    // Generator 1.4's overflow building (tests/fixtures/world/legacy/) with its arcade room holding a second section,
    // fbneo (as a 1.4 plan of both arcade systems would): the first update without fbneo's selection holds the room as
    // it stands, its capacity entries and its 1.4 stamp kept; the update with it migrates the chain then.
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::filesystem::path file = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" /
                                       "legacy" / "building-overflow-seed-102938-generator-1.4.json";
    auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    auto read = rv::world::readPlan(*text, "legacy");
    REQUIRE(read.has_value());
    WorldPlan legacy = std::move(*read);
    const RoomId arcade = roomId("area:arcade");
    for (Room& room : legacy.rooms) {
        if (room.id == arcade) {
            room.sections.push_back(systemName("fbneo"));
        }
    }
    const WorldPlan arcadeBefore = roomPart(legacy, arcade);
    REQUIRE(capacityEntries(arcadeBefore) == 7);

    const std::vector<RoomSelection> given = overflowSelections();
    const GenerationResult held = updateBuildingOf(world, legacy, given);
    INFO(hardFindings(held.validation));
    CHECK(held.validation.passed());
    REQUIRE(held.changes.heldRooms.size() == 1);
    CHECK(held.changes.heldRooms[0].room == arcade);
    CHECK(roomPart(held.plan, arcade) == arcadeBefore);
    CHECK(findRoom(held.plan, "area:arcade")->generatorVersion == rv::world::GeneratorVersion{1, 4});
    CHECK(held.changes.roomsAdded.empty());

    // fbneo's selection given: the chain is updated for the first time by 1.5, its capacity entries enter as additions
    // with fbneo's titles (the head's 8 free slots, then its next room).
    std::vector<RoomSelection> both = given;
    both.push_back(systemSelection("fbneo", 5, 5150));
    const GenerationResult migrated = updateBuildingOf(world, held.plan, both);
    INFO(hardFindings(migrated.validation));
    CHECK(migrated.validation.passed());
    CHECK(capacityEntries(migrated.plan) == 0);
    CHECK(migrated.changes.added.size() == 7 + 5);
    CHECK(migrated.changes.roomsAdded == std::vector{roomId("area:arcade:2")});
    CHECK(findRoom(migrated.plan, "area:arcade")->generatorVersion == rv::world::kGeneratorVersion);
    CHECK(updateBuildingOf(world, migrated.plan, both).plan == migrated.plan);
}

TEST_CASE("world/expansion chains: an expansion whose titles all left the catalog stands as it was",
          "[world][expansion]") {
    // Titles that leave the catalog keep their slots (ADR-0010 §10): the expansion's every title gone, the update
    // moves nothing, removes no room; only a regeneration drops it.
    BuildingWorld world = shippedWorld(kWorldSeed);
    const RoomId head = roomId("area:arcade");
    const RoomId expansion = roomId("area:arcade:2");
    const std::vector<RoomSelection> given{systemSelection("mame", 60, 5100)};
    const WorldPlan plan = generateBuildingOf(world, given).plan;
    std::set<GameId> inExpansion;
    for (const Placement& placement : placementsOf(plan, expansion)) {
        inExpansion.insert(placement.game);
    }
    const RoomSelection left =
        subsetSelection(given[0], [&](const GameId& game) { return !inExpansion.contains(game); });
    REQUIRE(left.titles.size() == 30);
    const GenerationResult updated = updateBuildingOf(world, plan, {left});
    INFO(hardFindings(updated.validation));
    CHECK(updated.validation.passed());
    CHECK(updated.plan.rooms == plan.rooms);
    CHECK(updated.plan.placements == plan.placements);
    CHECK(updated.plan.fixtures == plan.fixtures);
    CHECK(updated.changes.roomsAdded.empty());
    CHECK(updated.changes.added.empty());
    CHECK(updated.changes.rebuild == rv::world::RebuildLevel::NoRebuild);
    // "Rigenera stanza" then fits the 30 titles left in one room: the expansion is removed.
    const Regeneration regenerated = regenerateOf(world, updated.plan, {left}, RegenerationScope::ofRoom(head));
    CHECK(chainIds(regenerated.result.plan, head) == std::vector{head});
    CHECK(regenerated.impact.roomsRemoved == std::vector{expansion});
}
