// The explicit regeneration (rv/world/regeneration.hpp, ADR-0015 §3, §5; I4b step 5): a world without overrides
// regenerated (a room, a floor, the world) equals its fresh plan byte for byte (the golden plans and buildings, never
// rewritten here); the face-out set chosen again by priority; a room the update keeps (an older template) regenerated to
// the current one; a floor's sharing decided again, its systems on it; the overrides honoured (pinned fixtures first,
// pinned games reserved, the pin winning the representative), never dropped without the consent, every honoured pin
// standing, the update after the regeneration writing nothing; the conflicts with their reasons, kept in the
// overrides; the impact equal to the plans' difference; two runs one SHA-256; the refusals.

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/capacity.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/regeneration.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_edit.hpp>

#include "world_building_support.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

namespace {

using rv::catalog::GameId;
using rv::world::EditData;
using rv::world::EditedWorld;
using rv::world::EditOperation;
using rv::world::Fixture;
using rv::world::FixtureId;
using rv::world::FixtureRole;
using rv::world::GenerationResult;
using rv::world::Placement;
using rv::world::Regeneration;
using rv::world::RegenerationScope;
using rv::world::RoomId;
using rv::world::RoomSelection;
using rv::world::SlotId;
using rv::world::WorldOverrides;
using rv::world::WorldPlan;
using rv::test::world::generateSelection;
using rv::test::world::makeSelection;
using rv::test::world::planText;
using rv::test::world::snesData;
using rv::test::world::updateRoom;

constexpr std::uint64_t kWorldSeed = 102938;
constexpr std::uint64_t kSelectionSeed = 20261018;
const rv::world::PinProvenance kProvenance{rv::world::PinOrigin::Editor, "2026-10-28T10:00:00.000Z"};

const std::filesystem::path kGoldenDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "golden";

/// The shipped asset root's rooms' data, each request read once per run.
const rv::world::RoomDataLoader& loader() {
    static const rv::world::RoomDataLoader load = rv::world::assetRootLoader(rv::test::world::assetRoot());
    return load;
}

/// What a regeneration of a test reads besides the plan.
struct Inputs {
    std::vector<RoomSelection> selections;
    rv::world::WorldConfig config = rv::test::world::roomConfig(kWorldSeed);
    std::map<rv::catalog::SystemName, std::vector<std::string>> tags;
    rv::world::BuildingData building = snesData().building;
    rv::world::SystemsData systems = snesData().systems;
};

Regeneration regenerateOf(const WorldPlan& previous, const Inputs& inputs, const RegenerationScope& scope,
                          const WorldOverrides& overrides = {}, bool dropOverrides = false) {
    rv::world::RegenerationInput input;
    input.scope = scope;
    input.dropOverrides = dropOverrides;
    input.library = rv::test::world::library();
    input.selections = inputs.selections;
    input.platformTags = inputs.tags;
    input.config = inputs.config;
    input.overrides = overrides;
    input.previous = previous;
    auto result = rv::world::regenerate(input, inputs.building, inputs.systems, loader());
    INFO(scope.toString() << ": " << (result ? std::string("regenerated") : result.error().toString()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

std::string failedFindings(const rv::world::ValidationReport& report) {
    std::string text;
    for (const rv::world::ValidationFinding& finding : report.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            text += finding.rule + " " + finding.subject + ": " + finding.message + "\n";
        }
    }
    return text;
}

/// The plans' difference computed apart from planDifference(): sets of (game, slot, face-out), fixtures by id.
void requireImpactIsDifference(const WorldPlan& before, const Regeneration& regeneration) {
    const WorldPlan& after = regeneration.result.plan;
    const rv::world::RegenerationImpact& impact = regeneration.impact;
    const auto sectionsBefore = rv::test::world::levelSections(before);
    const auto sectionsAfter = rv::test::world::levelSections(after);
    const auto faceOut = [](const auto& sections, const Placement& placement) {
        const auto found = sections.find({placement.slot.fixture(), placement.slot.level()});
        return found != sections.end() && rv::world::isFaceOut(found->second);
    };
    std::map<GameId, std::pair<SlotId, bool>> was;
    std::map<GameId, std::pair<SlotId, bool>> is;
    for (const Placement& placement : before.placements) {
        was[placement.game] = {placement.slot, faceOut(sectionsBefore, placement)};
    }
    for (const Placement& placement : after.placements) {
        is[placement.game] = {placement.slot, faceOut(sectionsAfter, placement)};
    }
    std::vector<rv::world::GameMove> moved;
    std::vector<GameId> placed;
    std::vector<GameId> unplaced;
    std::vector<GameId> entering;
    std::vector<GameId> leaving;
    for (const auto& [game, now] : is) {
        const auto then = was.find(game);
        if (then == was.end()) {
            placed.push_back(game);
        } else if (then->second.first != now.first) {
            moved.push_back({game, then->second.first, now.first});
        }
        if (now.second && (then == was.end() || !then->second.second)) {
            entering.push_back(game);
        }
    }
    for (const auto& [game, then] : was) {
        const auto now = is.find(game);
        if (now == is.end()) {
            unplaced.push_back(game);
        }
        if (then.second && (now == is.end() || !now->second.second)) {
            leaving.push_back(game);
        }
    }
    CHECK(impact.gamesMoved == moved);
    CHECK(impact.newlyPlaced == placed);
    CHECK(impact.newlyUnplaced == unplaced);
    CHECK(impact.faceOutEntering == entering);
    CHECK(impact.faceOutLeaving == leaving);
    std::map<FixtureId, Fixture> fixturesBefore;
    for (const Fixture& fixture : before.fixtures) {
        fixturesBefore.emplace(fixture.id, fixture);
    }
    std::set<FixtureId> added;
    std::set<FixtureId> removed;
    std::vector<FixtureId> movedFixtures;
    for (const Fixture& fixture : after.fixtures) {
        const auto found = fixturesBefore.find(fixture.id);
        if (found == fixturesBefore.end()) {
            added.insert(fixture.id);
            continue;
        }
        if (found->second.role != fixture.role || found->second.prefab != fixture.prefab) {
            added.insert(fixture.id);
            removed.insert(fixture.id);
        } else if (!(found->second == fixture)) {
            movedFixtures.push_back(fixture.id);
        }
        fixturesBefore.erase(found);
    }
    for (const auto& [id, fixture] : fixturesBefore) {
        removed.insert(id);
    }
    CHECK(impact.fixturesAdded == std::vector<FixtureId>(added.begin(), added.end()));
    CHECK(impact.fixturesRemoved == std::vector<FixtureId>(removed.begin(), removed.end()));
    CHECK(impact.fixturesMoved == movedFixtures);
    std::set<RoomId> roomsBefore;
    std::map<RoomId, const rv::world::Room*> roomRecords;
    for (const rv::world::Room& room : before.rooms) {
        roomsBefore.insert(room.id);
        roomRecords.emplace(room.id, &room);
    }
    std::vector<RoomId> roomsAdded;
    std::vector<RoomId> roomsMoved;
    for (const rv::world::Room& room : after.rooms) {
        if (!roomsBefore.erase(room.id)) {
            roomsAdded.push_back(room.id);
        } else if (roomRecords.at(room.id)->placement != room.placement ||
                   roomRecords.at(room.id)->floor != room.floor) {
            roomsMoved.push_back(room.id);
        }
    }
    CHECK(impact.roomsAdded == roomsAdded);
    CHECK(impact.roomsRemoved == std::vector<RoomId>(roomsBefore.begin(), roomsBefore.end()));
    CHECK(impact.roomsMoved == roomsMoved);
}

/// The templates of the plan's rooms (the shipped one).
std::vector<rv::world::RoomTemplate> templates() {
    return {snesData().roomTemplate};
}

/// Every pin of `overrides` honoured by `plan` or a conflict of `regeneration` (each once), the honoured ones its pins
/// kept.
void requirePinsAccounted(const Regeneration& regeneration, const WorldOverrides& overrides) {
    const rv::world::PinCheck check = rv::world::checkPins(regeneration.result.plan, templates(), overrides);
    std::set<std::string> honoured;
    for (const auto& pin : check.honoured.gamePins) {
        honoured.insert(rv::world::pinText(pin));
    }
    for (const auto& pin : check.honoured.fixturePins) {
        honoured.insert(rv::world::pinText(pin));
    }
    std::set<std::string> conflicting;
    for (const rv::world::OverrideConflict& conflict : regeneration.impact.conflicts) {
        INFO(conflict.pin << ": " << rv::world::toString(conflict.reason) << ": " << conflict.message);
        CHECK(conflicting.insert(conflict.pin).second);
        CHECK_FALSE(honoured.contains(conflict.pin));
    }
    CHECK(regeneration.impact.pinsKept == std::vector<std::string>(honoured.begin(), honoured.end()));
    CHECK(honoured.size() + conflicting.size() == overrides.gamePins.size() + overrides.fixturePins.size());
}

const Placement* placementOf(const WorldPlan& plan, const GameId& game) {
    for (const Placement& placement : plan.placements) {
        if (placement.game == game) {
            return &placement;
        }
    }
    return nullptr;
}

const Fixture* fixtureOf(const WorldPlan& plan, std::string_view id) {
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.toString() == id) {
            return &fixture;
        }
    }
    return nullptr;
}

std::shared_ptr<const rv::world::WorldData> unowned(const rv::world::WorldData& data) {
    return std::shared_ptr<const rv::world::WorldData>(std::shared_ptr<const rv::world::WorldData>(), &data);
}

EditData snesEditData() {
    EditData data;
    data.config = rv::test::world::roomConfig(kWorldSeed);
    data.building = snesData().building;
    data.rooms.emplace(*RoomId::forSystem(rv::test::world::snes()), unowned(snesData()));
    return data;
}

void edit(EditedWorld& world, const EditOperation& operation, const EditData& data = snesEditData()) {
    auto result = rv::world::applyEdit(world, operation, data, kProvenance);
    REQUIRE(result.has_value());
    INFO(rv::world::editText(operation) << ": "
                                        << (result->refused ? result->refused->rule + " " + result->refused->message
                                                            : std::string("applied")));
    REQUIRE_FALSE(result->refused.has_value());
}

/// The free slots of `plan`'s `section` (dressed or not).
std::vector<SlotId> freeSlots(const WorldPlan& plan, rv::world::Section section) {
    std::set<SlotId> taken;
    for (const Placement& placement : plan.placements) {
        taken.insert(placement.slot);
    }
    std::vector<SlotId> slots;
    for (const Fixture& fixture : plan.fixtures) {
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            if (level.section != section) {
                continue;
            }
            for (std::uint32_t index = 0; index < level.slots; ++index) {
                const SlotId slot = *SlotId::make(fixture.id, level.level, index);
                if (!taken.contains(slot)) {
                    slots.push_back(slot);
                }
            }
        }
    }
    return slots;
}

/// `overrides` without the conflicts a regeneration records on their pins.
WorldOverrides withoutConflicts(WorldOverrides overrides) {
    for (rv::world::GamePin& pin : overrides.gamePins) {
        pin.conflict.reset();
    }
    for (rv::world::FixturePin& pin : overrides.fixturePins) {
        pin.conflict.reset();
    }
    return overrides;
}

/// Each conflict of `regeneration` recorded on its pin of the overrides it returns, no other pin recording one.
void requireConflictsRecorded(const Regeneration& regeneration) {
    std::map<std::string, rv::world::ConflictReason> expected;
    for (const rv::world::OverrideConflict& conflict : regeneration.impact.conflicts) {
        expected.emplace(conflict.fixture.empty() ? conflict.game.toString() : conflict.fixture.toString(),
                         conflict.reason);
    }
    std::map<std::string, rv::world::ConflictReason> recorded;
    for (const rv::world::GamePin& pin : regeneration.overrides.gamePins) {
        if (pin.conflict) {
            recorded.emplace(pin.game.toString(), *pin.conflict);
        }
    }
    for (const rv::world::FixturePin& pin : regeneration.overrides.fixturePins) {
        if (pin.conflict) {
            recorded.emplace(pin.fixture.toString(), *pin.conflict);
        }
    }
    CHECK(recorded == expected);
}

std::string sha256(const std::string& text) {
    return rv::core::sha256Hex(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

/// The SNES room's world of `titles` titles of `selectionSeed` (a console room whatever its size).
struct SnesWorld {
    Inputs inputs;
    WorldPlan plan;
};
SnesWorld snesWorld(std::size_t titles, std::uint64_t selectionSeed, std::uint64_t worldSeed = kWorldSeed) {
    SnesWorld world;
    world.inputs.selections = {makeSelection(titles, selectionSeed)};
    world.inputs.config = rv::test::world::roomConfig(worldSeed);
    world.plan = generateSelection(world.inputs.selections.front(), worldSeed).plan;
    return world;
}

/// The building fixture's systems (as test_floor_scene.cpp generates building-esde-seed-102938.json).
struct SystemCase {
    std::string_view system;
    std::size_t titles = 0;
    std::vector<std::string> tags;
    std::size_t distinctTitles = 0;
};

Inputs goldenBuildingInputs() {
    static const std::vector<SystemCase> systems{{"amiga", 20, {}},        {"gb", 30, {}},       {"mame", 15, {}, 25},
                                                {"pocketlab", 6, {"gb"}}, {"pokemini", 8, {}}, {"psx", 40, {}},
                                                {"snes", 40, {}}};
    Inputs inputs;
    inputs.config = rv::world::WorldConfig{};
    inputs.config.seed = kWorldSeed;
    std::uint64_t seed = 4000;
    for (const SystemCase& entry : systems) {
        RoomSelection selection = makeSelection(entry.titles, seed++);
        selection.distinctTitles = std::max(entry.distinctTitles, entry.titles);
        selection.room = *RoomId::forSystem(*rv::catalog::SystemName::parse(entry.system));
        for (rv::world::UnplacedGame& game : selection.unplaced) {
            game.room = selection.room;
        }
        inputs.selections.push_back(std::move(selection));
        if (!entry.tags.empty()) {
            inputs.tags[*rv::catalog::SystemName::parse(entry.system)] = entry.tags;
        }
    }
    return inputs;
}

/// A regeneration that changed nothing: the plan's text `expected`, nothing in its impact but the rooms regenerated.
void requireUnchanged(const Regeneration& regeneration, const std::string& expected) {
    INFO(failedFindings(regeneration.result.validation));
    CHECK(regeneration.result.validation.passed());
    CHECK(planText(regeneration.result.plan) == expected);
    CHECK_FALSE(regeneration.changed);
    const rv::world::RegenerationImpact& impact = regeneration.impact;
    CHECK(impact.gamesMoved.empty());
    CHECK(impact.newlyPlaced.empty());
    CHECK(impact.newlyUnplaced.empty());
    CHECK(impact.fixturesAdded.empty());
    CHECK(impact.fixturesRemoved.empty());
    CHECK(impact.fixturesMoved.empty());
    CHECK(impact.roomsAdded.empty());
    CHECK(impact.roomsRemoved.empty());
    CHECK(impact.roomsMoved.empty());
    CHECK_FALSE(impact.roomsRegenerated.empty());
}

} // namespace

TEST_CASE("world/regeneration: a world without overrides regenerated equals its fresh plan, the goldens unchanged",
          "[world][regeneration]") {
    SECTION("the golden plans of the SNES room: the room, its floor and the world") {
        struct Case {
            std::size_t titles;
            std::uint64_t seed;
        };
        for (const Case test : {Case{0, 102938}, Case{15, 102938}, Case{57, 7}, Case{890, 102938},
                                Case{890, 0xFFFFFFFFFFFFFFFF}, Case{2000, 102938}}) {
            const std::string name = fmt::format("plan-{}-seed-{}.json", test.titles, test.seed);
            INFO(name);
            auto golden = rv::core::readTextFile(kGoldenDir / name);
            REQUIRE(golden.has_value());
            auto plan = rv::world::readPlan(*golden, name);
            REQUIRE(plan.has_value());
            Inputs inputs;
            inputs.selections = {makeSelection(test.titles, kSelectionSeed)};
            inputs.config = rv::test::world::roomConfig(test.seed);
            for (const RegenerationScope& scope :
                 {RegenerationScope::ofRoom(*RoomId::parse("room:snes")), RegenerationScope::ofFloor("gen4"),
                  RegenerationScope::ofWorld()}) {
                INFO(scope.toString());
                const Regeneration regeneration = regenerateOf(*plan, inputs, scope);
                requireUnchanged(regeneration, *golden);
                CHECK(regeneration.impact.roomsRegenerated == std::vector<RoomId>{*RoomId::parse("room:snes")});
            }
        }
    }
    SECTION("the golden building of the building fixture: every room, every floor, the world") {
        const std::string name = "building-esde-seed-102938.json";
        auto golden = rv::core::readTextFile(kGoldenDir / name);
        REQUIRE(golden.has_value());
        auto plan = rv::world::readPlan(*golden, name);
        REQUIRE(plan.has_value());
        const Inputs inputs = goldenBuildingInputs();
        std::vector<RegenerationScope> scopes{RegenerationScope::ofWorld()};
        for (const rv::world::Room& room : plan->rooms) {
            scopes.push_back(RegenerationScope::ofRoom(room.id));
        }
        for (const rv::world::Floor& floor : plan->floors) {
            scopes.push_back(RegenerationScope::ofFloor(floor.id));
        }
        for (const RegenerationScope& scope : scopes) {
            INFO(scope.toString());
            const Regeneration regeneration = regenerateOf(*plan, inputs, scope);
            requireUnchanged(regeneration, *golden);
        }
    }
}

TEST_CASE("world/regeneration: the face-out set chosen again by priority, the room as a fresh one plans it",
          "[world][regeneration]") {
    SnesWorld world = snesWorld(300, 11);
    // Twenty favourites played often arrive: the update stands them spine-out (the face-out set kept).
    RoomSelection more = world.inputs.selections.front();
    rv::core::Random random(rv::core::deriveSeed("regeneration-favourites", 1));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(more);
    std::vector<GameId> favourites;
    for (int i = 0; i < 20; ++i) {
        favourites.push_back(rv::test::world::addTitle(more, random, taken, true, 500).game);
    }
    const GenerationResult updated = updateRoom(world.plan, more, kWorldSeed);
    const auto faceOutOf = [](const WorldPlan& plan) {
        std::set<GameId> games;
        for (const Placement& placement : rv::test::world::faceOutPlacements(plan)) {
            games.insert(placement.game);
        }
        return games;
    };
    for (const GameId& game : favourites) {
        CHECK_FALSE(faceOutOf(updated.plan).contains(game));
    }
    Inputs inputs = world.inputs;
    inputs.selections = {more};
    const std::string fresh = planText(generateSelection(more, kWorldSeed).plan);
    for (const RegenerationScope& scope : {RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                           RegenerationScope::ofFloor("gen4"), RegenerationScope::ofWorld()}) {
        INFO(scope.toString());
        const Regeneration regeneration = regenerateOf(updated.plan, inputs, scope);
        INFO(failedFindings(regeneration.result.validation));
        CHECK(regeneration.result.validation.passed());
        // The room as a fresh plan of the selection plans it: the favourites face-out.
        CHECK(planText(regeneration.result.plan) == fresh);
        const std::set<GameId> faceOut = faceOutOf(regeneration.result.plan);
        for (const GameId& game : favourites) {
            CHECK(faceOut.contains(game));
            CHECK(std::find(regeneration.impact.faceOutEntering.begin(), regeneration.impact.faceOutEntering.end(),
                            game) != regeneration.impact.faceOutEntering.end());
        }
        CHECK(regeneration.impact.faceOutLeaving.size() == regeneration.impact.faceOutEntering.size());
        CHECK(regeneration.changed);
        CHECK(regeneration.result.changes.outcome == rv::world::GenerationOutcome::Generated);
        requireImpactIsDifference(updated.plan, regeneration);
        // An update of the same selection after it writes nothing.
        CHECK(updateRoom(regeneration.result.plan, more, kWorldSeed).plan == regeneration.result.plan);
    }
}

TEST_CASE("world/regeneration: a room the update keeps (an older template) regenerated to the current template",
          "[world][regeneration]") {
    SnesWorld world = snesWorld(200, 12);
    WorldPlan kept = world.plan;
    const std::uint32_t current = snesData().roomTemplate.version;
    kept.rooms.front().templateVersion = current + 1;
    RoomSelection more = world.inputs.selections.front();
    rv::core::Random random(rv::core::deriveSeed("regeneration-kept", 1));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(more);
    for (int i = 0; i < 5; ++i) {
        rv::test::world::addTitle(more, random, taken);
    }
    const GenerationResult updated = updateRoom(kept, more, kWorldSeed);
    REQUIRE(updated.changes.keptRooms.size() == 1);
    CHECK(std::count_if(updated.plan.unplaced.begin(), updated.plan.unplaced.end(), [](const auto& game) {
              return game.reason == rv::world::UnplacedReason::RoomKept;
          }) == 5);
    Inputs inputs = world.inputs;
    inputs.selections = {more};
    const Regeneration regeneration = regenerateOf(updated.plan, inputs,
                                                   RegenerationScope::ofRoom(*RoomId::parse("room:snes")));
    INFO(failedFindings(regeneration.result.validation));
    CHECK(regeneration.result.validation.passed());
    CHECK(regeneration.result.plan.rooms.front().templateVersion == current);
    CHECK(planText(regeneration.result.plan) == planText(generateSelection(more, kWorldSeed).plan));
    CHECK(regeneration.impact.newlyPlaced.size() == 5);
    requireImpactIsDifference(updated.plan, regeneration);
    // The update no longer keeps it.
    CHECK(updateRoom(regeneration.result.plan, more, kWorldSeed).changes.keptRooms.empty());
}

TEST_CASE("world/regeneration: a floor's sharing decided again, its systems staying on it", "[world][regeneration]") {
    rv::test::world::BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    const auto selection = [](std::string_view system, std::size_t titles, std::uint64_t seed) {
        return rv::test::world::systemSelection(system, titles, seed);
    };
    // megadrive under the threshold takes a section of gen4's shared room; grown past it, the update keeps it there.
    const std::vector<RoomSelection> before{selection("snes", 40, 1), selection("megadrive", 12, 2)};
    const GenerationResult generated = rv::test::world::generateBuildingOf(world, before);
    REQUIRE(rv::test::world::findRoom(generated.plan, "area:shared:gen4") != nullptr);
    const std::vector<RoomSelection> after{selection("snes", 40, 1), selection("megadrive", 45, 2)};
    const GenerationResult updated = rv::test::world::updateBuildingOf(world, generated.plan, after);
    REQUIRE(rv::test::world::findRoom(updated.plan, "room:megadrive") == nullptr);
    Inputs inputs;
    inputs.selections = after;
    inputs.config = world.config;
    inputs.building = world.building;
    inputs.systems = world.systems;

    // The shared room regenerated alone keeps its sections; the floor decides them again.
    const Regeneration room =
        regenerateOf(updated.plan, inputs, RegenerationScope::ofRoom(*RoomId::parse("area:shared:gen4")));
    CHECK(room.result.validation.passed());
    CHECK(rv::test::world::findRoom(room.result.plan, "room:megadrive") == nullptr);
    const Regeneration floor = regenerateOf(updated.plan, inputs, RegenerationScope::ofFloor("gen4"));
    INFO(failedFindings(floor.result.validation));
    CHECK(floor.result.validation.passed());
    CHECK(rv::test::world::findRoom(floor.result.plan, "room:megadrive") != nullptr);
    CHECK(rv::test::world::findRoom(floor.result.plan, "area:shared:gen4") == nullptr);
    CHECK(floor.impact.roomsAdded == std::vector<RoomId>{*RoomId::parse("room:megadrive")});
    CHECK(floor.impact.roomsRemoved == std::vector<RoomId>{*RoomId::parse("area:shared:gen4")});
    requireImpactIsDifference(updated.plan, floor);
    // As a fresh building of the selections plans it, and so does the world.
    const std::string fresh = planText(rv::test::world::generateBuildingOf(world, after).plan);
    CHECK(planText(floor.result.plan) == fresh);
    CHECK(planText(regenerateOf(updated.plan, inputs, RegenerationScope::ofWorld()).result.plan) == fresh);
    // The update after it writes nothing.
    CHECK(rv::test::world::updateBuildingOf(world, floor.result.plan, after).plan == floor.result.plan);

    // snes shrunk under the threshold: the floor puts it into the shared room, room:snes removed.
    const std::vector<RoomSelection> shrunk{selection("snes", 10, 1), selection("megadrive", 45, 2)};
    const GenerationResult smaller = rv::test::world::updateBuildingOf(world, floor.result.plan, shrunk);
    inputs.selections = shrunk;
    const Regeneration again = regenerateOf(smaller.plan, inputs, RegenerationScope::ofFloor("gen4"));
    CHECK(again.result.validation.passed());
    CHECK(rv::test::world::findRoom(again.result.plan, "room:snes") == nullptr);
    REQUIRE(rv::test::world::findRoom(again.result.plan, "area:shared:gen4") != nullptr);
    CHECK(planText(again.result.plan) == planText(rv::test::world::generateBuildingOf(world, shrunk).plan));

    // A system its classification now puts on another floor stays on its floor; only the world moves it.
    Inputs moved = inputs;
    moved.selections = after;
    for (rv::world::SystemEntry& entry : moved.systems.systems) {
        if (entry.system.str() == "megadrive") {
            entry.generation = 3;
        }
    }
    const Regeneration stays = regenerateOf(floor.result.plan, moved, RegenerationScope::ofFloor("gen4"));
    REQUIRE(rv::test::world::findRoom(stays.result.plan, "room:megadrive") != nullptr);
    CHECK(rv::test::world::findRoom(stays.result.plan, "room:megadrive")->floor == "gen4");
    const Regeneration goes = regenerateOf(floor.result.plan, moved, RegenerationScope::ofWorld());
    REQUIRE(rv::test::world::findRoom(goes.result.plan, "room:megadrive") != nullptr);
    CHECK(rv::test::world::findRoom(goes.result.plan, "room:megadrive")->floor == "gen3");
}

TEST_CASE("world/regeneration: the overrides honoured, never dropped without the consent", "[world][regeneration]") {
    SnesWorld world = snesWorld(300, 21);
    const std::string freshText = planText(world.plan);
    EditedWorld edited{world.plan, {}};
    // A game moved into a free slot, one swapped onto a display fixture, two swapped, one pinned in place, a display
    // case moved, a decoration pinned where it stands.
    std::vector<Placement> spine;
    std::vector<Placement> faceOut = rv::test::world::faceOutPlacements(edited.plan);
    for (const Placement& placement : edited.plan.placements) {
        if (std::find(faceOut.begin(), faceOut.end(), placement) == faceOut.end()) {
            spine.push_back(placement);
        }
    }
    REQUIRE(spine.size() > 10);
    REQUIRE(faceOut.size() > 2);
    const std::vector<SlotId> free = freeSlots(edited.plan, rv::world::Section::Spine);
    REQUIRE_FALSE(free.empty());
    edit(edited, EditOperation::move(spine[3].game, free.back()));
    edit(edited, EditOperation::swap(spine[4].game, faceOut[1].game));
    edit(edited, EditOperation::swap(spine[5].game, spine[9].game));
    edit(edited, EditOperation::pin(faceOut.front().game));
    const Fixture display = *fixtureOf(edited.plan, "room:snes/fx:dc01");
    bool moved = false;
    for (const std::int64_t dz : {-300, 300, -600, 600, -900}) {
        EditedWorld trial = edited;
        auto result = rv::world::applyEdit(trial, EditOperation::moveFixture(display.id, display.xMm, display.zMm + dz),
                                           snesEditData(), kProvenance);
        REQUIRE(result.has_value());
        if (!result->refused) {
            edited = trial;
            moved = true;
            break;
        }
    }
    REQUIRE(moved);
    const Fixture* decoration = nullptr;
    for (const Fixture& fixture : edited.plan.fixtures) {
        if (fixture.role == FixtureRole::Decoration && decoration == nullptr) {
            decoration = &fixture;
        }
    }
    REQUIRE(decoration != nullptr);
    edit(edited, EditOperation::pinFixture(decoration->id));
    const WorldOverrides overrides = edited.overrides;
    REQUIRE(overrides.gamePins.size() == 6);
    REQUIRE(overrides.fixturePins.size() == 2);

    for (const RegenerationScope& scope : {RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                           RegenerationScope::ofFloor("gen4"), RegenerationScope::ofWorld()}) {
        INFO(scope.toString());
        const Regeneration regeneration = regenerateOf(edited.plan, world.inputs, scope, overrides);
        INFO(failedFindings(regeneration.result.validation));
        CHECK(regeneration.result.validation.passed());
        // Kept: the overrides as they were, every pin honoured.
        CHECK(regeneration.overrides == overrides);
        requirePinsAccounted(regeneration, overrides);
        CHECK(regeneration.impact.conflicts.empty());
        for (const rv::world::GamePin& pin : overrides.gamePins) {
            const Placement* placement = placementOf(regeneration.result.plan, pin.game);
            REQUIRE(placement != nullptr);
            CHECK(placement->slot == pin.slot);
        }
        const Fixture* movedDisplay = fixtureOf(regeneration.result.plan, "room:snes/fx:dc01");
        REQUIRE(movedDisplay != nullptr);
        CHECK(*movedDisplay == *fixtureOf(edited.plan, "room:snes/fx:dc01"));
        requireImpactIsDifference(edited.plan, regeneration);
        // The face-out titles chosen again around the pins: the edited plan's face-out set but the pinned games.
        // Twice one SHA-256.
        const Regeneration twice = regenerateOf(edited.plan, world.inputs, scope, overrides);
        CHECK(sha256(planText(twice.result.plan)) == sha256(planText(regeneration.result.plan)));
        CHECK(twice.overrides == regeneration.overrides);
        // The update of the same selection after it writes nothing.
        const GenerationResult update =
            updateRoom(regeneration.result.plan, world.inputs.selections.front(), kWorldSeed, regeneration.overrides);
        CHECK(update.plan == regeneration.result.plan);
        CHECK(update.validation.passed());
        // Regenerated again from its own result: nothing changes.
        CHECK_FALSE(regenerateOf(regeneration.result.plan, world.inputs, scope, regeneration.overrides).changed);

        // Dropped with the consent: no pin left, the plan the fresh one.
        const Regeneration dropped = regenerateOf(edited.plan, world.inputs, scope, overrides, true);
        CHECK(dropped.overrides.empty());
        CHECK(dropped.impact.overridesDropped.size() == 8);
        CHECK(dropped.impact.pinsKept.empty());
        CHECK(planText(dropped.result.plan) == freshText);
    }
}

TEST_CASE("world/regeneration: the pin wins the representative", "[world][regeneration]") {
    SnesWorld world = snesWorld(120, 31);
    // A spine-out title with no other version: its game pinned in place.
    std::set<GameId> duplicated;
    for (const rv::world::UnplacedGame& game : world.inputs.selections.front().unplaced) {
        duplicated.insert(game.representative);
    }
    std::vector<Placement> faceOut = rv::test::world::faceOutPlacements(world.plan);
    const Placement* chosen = nullptr;
    for (const Placement& placement : world.plan.placements) {
        if (chosen == nullptr && !duplicated.contains(placement.game) &&
            std::find(faceOut.begin(), faceOut.end(), placement) == faceOut.end()) {
            chosen = &placement;
        }
    }
    REQUIRE(chosen != nullptr);
    const Placement pinned = *chosen;
    EditedWorld edited{world.plan, {}};
    edit(edited, EditOperation::pin(pinned.game));
    // A better version arrives: the selection's representative of its title is now another game.
    RoomSelection selection = world.inputs.selections.front();
    rv::core::Random random(rv::core::deriveSeed("regeneration-representative", 1));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(selection);
    const GameId better = rv::test::world::freshGameId(random, taken);
    for (rv::world::RoomTitle& title : selection.titles) {
        if (title.game == pinned.game) {
            title.game = better;
        }
    }
    std::sort(selection.titles.begin(), selection.titles.end(), rv::world::priorityLess);
    selection.unplaced.push_back({pinned.game, selection.room, rv::world::UnplacedReason::DuplicateTitle, better});
    std::sort(selection.unplaced.begin(), selection.unplaced.end());
    Inputs inputs = world.inputs;
    inputs.selections = {selection};

    const Regeneration regeneration =
        regenerateOf(edited.plan, inputs, RegenerationScope::ofRoom(*RoomId::parse("room:snes")), edited.overrides);
    INFO(failedFindings(regeneration.result.validation));
    CHECK(regeneration.result.validation.passed());
    const Placement* stands = placementOf(regeneration.result.plan, pinned.game);
    REQUIRE(stands != nullptr);
    CHECK(stands->slot == pinned.slot);
    CHECK(placementOf(regeneration.result.plan, better) == nullptr);
    const auto entry = std::find_if(regeneration.result.plan.unplaced.begin(), regeneration.result.plan.unplaced.end(),
                                    [&](const rv::world::UnplacedGame& game) { return game.game == better; });
    REQUIRE(entry != regeneration.result.plan.unplaced.end());
    CHECK(entry->reason == rv::world::UnplacedReason::DuplicateTitle);
    CHECK(entry->representative == pinned.game);
    CHECK(regeneration.impact.conflicts.empty());
    // The update keeps the pinned version as the representative: it writes nothing.
    CHECK(updateRoom(regeneration.result.plan, selection, kWorldSeed, regeneration.overrides).plan ==
          regeneration.result.plan);
    // Without the pin, the better version represents the title.
    const Regeneration dropped = regenerateOf(edited.plan, inputs,
                                              RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                              edited.overrides, true);
    CHECK(placementOf(dropped.result.plan, better) != nullptr);
    CHECK(placementOf(dropped.result.plan, pinned.game) == nullptr);
}

TEST_CASE("world/regeneration: conflicts with their reasons, kept in the overrides", "[world][regeneration]") {
    SECTION("game-gone and slot-gone") {
        SnesWorld world = snesWorld(150, 41);
        std::set<GameId> duplicated;
        for (const rv::world::UnplacedGame& game : world.inputs.selections.front().unplaced) {
            duplicated.insert(game.representative);
        }
        std::vector<GameId> games;
        for (const Placement& placement : world.plan.placements) {
            if (!duplicated.contains(placement.game)) {
                games.push_back(placement.game);
            }
        }
        REQUIRE(games.size() > 4);
        EditedWorld edited{world.plan, {}};
        edit(edited, EditOperation::pin(games[0]));
        WorldOverrides overrides = edited.overrides;
        // By hand: a pin of a slot no socket holds and of a level no fixture has.
        overrides.gamePins.push_back({games[1], *SlotId::parse("room:snes/fx:nowhere/s:0.0"), {}});
        overrides.gamePins.push_back({games[2], *SlotId::parse("room:snes/fx:hw-w/s:99.0"), {}});
        std::sort(overrides.gamePins.begin(), overrides.gamePins.end(),
                  [](const auto& a, const auto& b) { return a.game < b.game; });
        // The first pinned game leaves the catalog.
        Inputs inputs = world.inputs;
        RoomSelection& selection = inputs.selections.front();
        std::erase_if(selection.titles, [&](const rv::world::RoomTitle& title) { return title.game == games[0]; });
        selection.distinctTitles = selection.titles.size();
        const Regeneration regeneration =
            regenerateOf(edited.plan, inputs, RegenerationScope::ofRoom(*RoomId::parse("room:snes")), overrides);
        INFO(failedFindings(regeneration.result.validation));
        CHECK(regeneration.result.validation.passed());
        std::map<GameId, rv::world::ConflictReason> reasons;
        for (const rv::world::OverrideConflict& conflict : regeneration.impact.conflicts) {
            reasons[conflict.game] = conflict.reason;
        }
        CHECK(reasons == std::map<GameId, rv::world::ConflictReason>{{games[0], rv::world::ConflictReason::GameGone},
                                                                      {games[1], rv::world::ConflictReason::SlotGone},
                                                                      {games[2], rv::world::ConflictReason::SlotGone}});
        // Never dropped silently: kept, each conflict recorded on its pin.
        CHECK(withoutConflicts(regeneration.overrides) == overrides);
        requireConflictsRecorded(regeneration);
        requirePinsAccounted(regeneration, overrides);
        requireImpactIsDifference(edited.plan, regeneration);
    }
    SECTION("room-gone: a system moved into its floor's shared room") {
        rv::test::world::BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
        const std::vector<RoomSelection> before{rv::test::world::systemSelection("snes", 40, 1),
                                                rv::test::world::systemSelection("megadrive", 45, 2)};
        const GenerationResult generated = rv::test::world::generateBuildingOf(world, before);
        const Placement* megadrive = nullptr;
        for (const Placement& placement : generated.plan.placements) {
            if (megadrive == nullptr && placement.slot.fixture().room().toString() == "room:megadrive") {
                megadrive = &placement;
            }
        }
        REQUIRE(megadrive != nullptr);
        WorldOverrides overrides;
        overrides.gamePins.push_back({megadrive->game, megadrive->slot, kProvenance});
        const std::vector<RoomSelection> after{rv::test::world::systemSelection("snes", 40, 1),
                                               rv::test::world::systemSelection("megadrive", 12, 2)};
        Inputs inputs;
        inputs.selections = after;
        inputs.config = world.config;
        inputs.building = world.building;
        inputs.systems = world.systems;
        const Regeneration regeneration =
            regenerateOf(generated.plan, inputs, RegenerationScope::ofFloor("gen4"), overrides);
        CHECK(regeneration.result.validation.passed());
        REQUIRE(regeneration.impact.conflicts.size() == 1);
        CHECK(regeneration.impact.conflicts.front().reason == rv::world::ConflictReason::RoomGone);
        CHECK(withoutConflicts(regeneration.overrides) == overrides);
        requireConflictsRecorded(regeneration);
    }
    SECTION("invalid: a display case moved where the smaller room has no floor") {
        // A room of one growth step: the display case moved into its grown part.
        SnesWorld world = snesWorld(3400, 42);
        const rv::world::Room grown = world.plan.rooms.front();
        REQUIRE(grown.growthSteps >= 1);
        EditedWorld edited{world.plan, {}};
        const Fixture display = *fixtureOf(edited.plan, "room:snes/fx:dc01");
        const std::int64_t baseMaxZ = rv::world::interiorFootprint(snesData().roomTemplate, 0).maxZMm;
        // West of the doorway's clear area, clear of the west wall's modules, on the case's grid.
        bool moved = false;
        std::string refusals;
        for (const std::int64_t dx : {-800, -700, -900, -600, -1000}) {
            for (std::int64_t z = display.zMm + 100; !moved && z + 600 <= grown.maxZMm; z += 100) {
                if (z < baseMaxZ) {
                    continue;
                }
                EditedWorld trial = edited;
                auto result = rv::world::applyEdit(trial, EditOperation::moveFixture(display.id, display.xMm + dx, z),
                                                   snesEditData(), kProvenance);
                REQUIRE(result.has_value());
                if (!result->refused) {
                    edited = trial;
                    moved = true;
                } else if (refusals.size() < 2000) {
                    refusals += result->refused->rule + " " + result->refused->message + "\n";
                }
            }
        }
        INFO(refusals);
        REQUIRE(moved);
        // The collection shrinks: the base room, the case's pinned place outside it.
        Inputs inputs = world.inputs;
        inputs.selections = {makeSelection(400, 42)};
        const Regeneration regeneration = regenerateOf(edited.plan, inputs,
                                                       RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                                       edited.overrides);
        INFO(failedFindings(regeneration.result.validation));
        CHECK(regeneration.result.validation.passed());
        CHECK(regeneration.result.plan.rooms.front().growthSteps == 0);
        REQUIRE(regeneration.impact.conflicts.size() == 1);
        const rv::world::OverrideConflict& conflict = regeneration.impact.conflicts.front();
        CHECK(conflict.reason == rv::world::ConflictReason::Invalid);
        CHECK(conflict.fixture == display.id);
        INFO(conflict.message);
        CHECK(conflict.message.find(rv::world::rule::kFixtureOutsideRoom) != std::string::npos);
        // The case stands in its socket; the pin kept.
        CHECK(fixtureOf(regeneration.result.plan, "room:snes/fx:dc01")->zMm == display.zMm);
        CHECK(withoutConflicts(regeneration.overrides) == edited.overrides);
        requireConflictsRecorded(regeneration);
    }
}

TEST_CASE("world/regeneration: a conflict recorded on its pin, reported as orphaned at every update, never as "
          "needing a confirmation",
          "[world][regeneration]") {
    SnesWorld world = snesWorld(150, 44);
    std::set<GameId> duplicated;
    for (const rv::world::UnplacedGame& game : world.inputs.selections.front().unplaced) {
        duplicated.insert(game.representative);
    }
    const Placement* chosen = nullptr;
    for (const Placement& placement : world.plan.placements) {
        if (chosen == nullptr && !duplicated.contains(placement.game)) {
            chosen = &placement;
        }
    }
    REQUIRE(chosen != nullptr);
    const GameId game = chosen->game;
    EditedWorld edited{world.plan, {}};
    edit(edited, EditOperation::pin(game));
    // The pinned game loses its cover: unplaced (no-cover), no longer a title of the room.
    Inputs inputs = world.inputs;
    RoomSelection& selection = inputs.selections.front();
    std::erase_if(selection.titles, [&](const rv::world::RoomTitle& title) { return title.game == game; });
    selection.unplaced.push_back({game, selection.room, rv::world::UnplacedReason::NoCover, {}});
    std::sort(selection.unplaced.begin(), selection.unplaced.end());
    selection.distinctTitles = selection.titles.size();
    const RegenerationScope scope = RegenerationScope::ofRoom(*RoomId::parse("room:snes"));
    const Regeneration regeneration = regenerateOf(edited.plan, inputs, scope, edited.overrides);
    INFO(failedFindings(regeneration.result.validation));
    CHECK(regeneration.result.validation.passed());
    REQUIRE(regeneration.impact.conflicts.size() == 1);
    CHECK(regeneration.impact.conflicts.front().reason == rv::world::ConflictReason::GameGone);
    REQUIRE(regeneration.overrides.gamePins.size() == 1);
    CHECK(regeneration.overrides.gamePins.front().conflict == rv::world::ConflictReason::GameGone);
    const std::string pin = rv::world::pinText(regeneration.overrides.gamePins.front());
    CHECK(regeneration.result.changes.needsConfirmation.empty());
    CHECK(regeneration.result.changes.orphanedPins == std::vector<std::string>{pin});
    // overrides.json keeps it: written and read back equal.
    const auto text = rv::world::writeOverrides(regeneration.overrides);
    REQUIRE(text.has_value());
    CHECK(text->find("\"conflict\": \"game-gone\"") != std::string::npos);
    CHECK(rv::world::parseOverrides(*text, "overrides.json") == regeneration.overrides);
    // The update of the unchanged catalog: nothing written, the conflict orphaned, nothing to confirm.
    const GenerationResult update = updateRoom(regeneration.result.plan, selection, kWorldSeed, regeneration.overrides);
    CHECK(update.plan == regeneration.result.plan);
    CHECK(update.validation.passed());
    CHECK(update.changes.needsConfirmation.empty());
    CHECK(update.changes.orphanedPins == std::vector<std::string>{pin});
    // A pin without the record, as it was before the regeneration, would need a confirmation.
    CHECK(updateRoom(regeneration.result.plan, selection, kWorldSeed, withoutConflicts(regeneration.overrides))
              .changes.needsConfirmation == std::vector<std::string>{pin});
    // Regenerated again: the same conflict, nothing changes.
    CHECK_FALSE(regenerateOf(regeneration.result.plan, inputs, scope, regeneration.overrides).changed);
    // The cover comes back: the pin honoured again, its record cleared.
    const Regeneration back = regenerateOf(regeneration.result.plan, world.inputs, scope, regeneration.overrides);
    CHECK(back.impact.conflicts.empty());
    CHECK(back.overrides == edited.overrides);
    const Placement* stands = placementOf(back.result.plan, game);
    REQUIRE(stands != nullptr);
    CHECK(stands->slot == chosen->slot);
}

TEST_CASE("world/regeneration: a hung decoration on the south wall follows it to the room's new depth",
          "[world][regeneration]") {
    // A room grown by a step, a poster of the south wall slid along it (pinned at its transform), then the collection
    // shrinks: the regenerated room's south wall is a step north, and the poster with it.
    SnesWorld world;
    const Fixture* poster = nullptr;
    for (std::uint64_t seed = kWorldSeed; poster == nullptr && seed < kWorldSeed + 60; ++seed) {
        world = snesWorld(3400, 43, seed);
        for (const Fixture& fixture : world.plan.fixtures) {
            if (fixture.role == FixtureRole::Decoration && fixture.yMm > 0 &&
                (fixture.id.socket() == "dec-sw" || fixture.id.socket() == "dec-se")) {
                poster = &fixture;
            }
        }
    }
    REQUIRE(poster != nullptr);
    REQUIRE(world.plan.rooms.front().growthSteps >= 1);
    EditedWorld edited{world.plan, {}};
    const Fixture before = *poster;
    bool slid = false;
    for (const std::int64_t step : {100, -100, 200, -200, 300, -300}) {
        EditedWorld trial = edited;
        auto result = rv::world::applyEdit(trial, EditOperation::moveFixture(before.id, before.xMm + step, before.zMm),
                                           snesEditData(), kProvenance);
        REQUIRE(result.has_value());
        if (!result->refused) {
            edited = trial;
            slid = true;
            break;
        }
    }
    REQUIRE(slid);
    const Fixture placed = *fixtureOf(edited.plan, before.id.toString());
    Inputs inputs = world.inputs;
    inputs.selections = {makeSelection(400, 43)};
    const Regeneration regeneration = regenerateOf(edited.plan, inputs,
                                                   RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                                   edited.overrides);
    INFO(failedFindings(regeneration.result.validation));
    CHECK(regeneration.result.validation.passed());
    const std::int64_t shift = regeneration.result.plan.rooms.front().maxZMm - edited.plan.rooms.front().maxZMm;
    REQUIRE(shift < 0);
    const Fixture* after = fixtureOf(regeneration.result.plan, before.id.toString());
    REQUIRE(after != nullptr);
    CHECK(after->xMm == placed.xMm);
    CHECK(after->zMm == placed.zMm + shift);
    REQUIRE(regeneration.overrides.fixturePins.size() == 1);
    CHECK(regeneration.overrides.fixturePins.front().transform ==
          rv::world::FixtureTransform{placed.xMm, placed.zMm + shift, placed.yawCdeg});
    CHECK(regeneration.impact.conflicts.empty());
    CHECK(regeneration.impact.pinsKept.size() == 1);
}

namespace {

/// The side-wall modules `titles` titles need in the SNES room (capacity.hpp's count).
std::uint32_t sideModulesOf(std::size_t titles) {
    static const rv::world::CollectionCapacity capacity = [] {
        auto measured = rv::world::collectionCapacity(snesData().roomTemplate, snesData().prefabs,
                                                      snesData().mediaFamily);
        REQUIRE(measured.has_value());
        return *measured;
    }();
    return rv::world::countCollection(capacity, titles, rv::test::world::roomConfig(kWorldSeed).growthReservePermille)
        .sideModules;
}

/// The most titles whose count needs `modules` side-wall modules.
std::size_t mostTitlesFor(std::uint32_t modules) {
    std::size_t titles = 1;
    while (sideModulesOf(titles + 1) <= modules) {
        ++titles;
    }
    REQUIRE(sideModulesOf(titles) == modules);
    return titles;
}

std::size_t capacityUnplaced(const WorldPlan& plan) {
    return static_cast<std::size_t>(
        std::count_if(plan.unplaced.begin(), plan.unplaced.end(), [](const rv::world::UnplacedGame& game) {
            return game.reason == rv::world::UnplacedReason::Capacity;
        }));
}

} // namespace

TEST_CASE("world/regeneration: a pinned decoration on a growth step's socket judged against the growth every pin "
          "asks for",
          "[world][regeneration]") {
    // Fifteen side-wall modules: the room grown a step, w08 a module and e08 (after it in the collection's order)
    // decoration. Both pinned; the collection shrinks to a room of no growth, but the pinned module keeps the step:
    // the decoration of e08, which sorts before w08, stands.
    SnesWorld world = snesWorld(mostTitlesFor(15), 62);
    REQUIRE(world.plan.rooms.front().growthSteps == 1);
    const Fixture* module = fixtureOf(world.plan, "room:snes/fx:w08");
    const Fixture* decoration = fixtureOf(world.plan, "room:snes/fx:e08");
    REQUIRE(module != nullptr);
    REQUIRE(decoration != nullptr);
    REQUIRE(module->role == FixtureRole::SideModule);
    REQUIRE(decoration->role == FixtureRole::Decoration);
    EditedWorld edited{world.plan, {}};
    // The decoration slid along its wall (pinned at its transform: a fresh draw never stands it there).
    bool slid = false;
    for (const std::int64_t dz : {100, -100, 200, -200}) {
        EditedWorld trial = edited;
        auto result = rv::world::applyEdit(
            trial, EditOperation::moveFixture(decoration->id, decoration->xMm, decoration->zMm + dz,
                                              decoration->yawCdeg),
            snesEditData(), kProvenance);
        REQUIRE(result.has_value());
        if (!result->refused) {
            edited = trial;
            slid = true;
            break;
        }
    }
    REQUIRE(slid);
    edit(edited, EditOperation::pinFixture(module->id));
    REQUIRE(edited.overrides.fixturePins.size() == 2);
    REQUIRE(edited.overrides.fixturePins.front().fixture == decoration->id);
    REQUIRE(edited.overrides.fixturePins.front().transform.has_value());
    const Fixture moved = *fixtureOf(edited.plan, "room:snes/fx:e08");
    Inputs inputs = world.inputs;
    inputs.selections = {makeSelection(mostTitlesFor(10), 62)};
    REQUIRE(generateSelection(inputs.selections.front(), kWorldSeed).plan.rooms.front().growthSteps == 0);
    const Regeneration regeneration = regenerateOf(edited.plan, inputs,
                                                   RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                                   edited.overrides);
    INFO(failedFindings(regeneration.result.validation));
    CHECK(regeneration.result.validation.passed());
    for (const rv::world::OverrideConflict& conflict : regeneration.impact.conflicts) {
        INFO(conflict.pin << ": " << rv::world::toString(conflict.reason) << ": " << conflict.message);
        CHECK(false);
    }
    CHECK(regeneration.result.plan.rooms.front().growthSteps == 1);
    const Fixture* standing = fixtureOf(regeneration.result.plan, "room:snes/fx:e08");
    REQUIRE(standing != nullptr);
    CHECK(*standing == moved);
    REQUIRE(fixtureOf(regeneration.result.plan, "room:snes/fx:w08") != nullptr);
    CHECK(fixtureOf(regeneration.result.plan, "room:snes/fx:w08")->role == FixtureRole::SideModule);
    requirePinsAccounted(regeneration, edited.overrides);
}

TEST_CASE("world/regeneration: the modules a pinned fixture displaces grow the room", "[world][regeneration]") {
    // Thirteen side-wall modules: e07, the last socket of the room without growth, holds decoration; pinned. The
    // collection grows to exactly the fourteen modules the room without growth holds: with e07 pinned, the room
    // takes a step and every title stands.
    SnesWorld world = snesWorld(mostTitlesFor(13), 63);
    REQUIRE(world.plan.rooms.front().growthSteps == 0);
    const Fixture* decoration = fixtureOf(world.plan, "room:snes/fx:e07");
    REQUIRE(decoration != nullptr);
    REQUIRE(decoration->role == FixtureRole::Decoration);
    EditedWorld edited{world.plan, {}};
    edit(edited, EditOperation::pinFixture(decoration->id));
    Inputs inputs = world.inputs;
    inputs.selections = {makeSelection(mostTitlesFor(14), 63)};
    const GenerationResult fresh = generateSelection(inputs.selections.front(), kWorldSeed);
    REQUIRE(fresh.plan.rooms.front().growthSteps == 0);
    REQUIRE(capacityUnplaced(fresh.plan) == 0);
    const Regeneration regeneration = regenerateOf(edited.plan, inputs,
                                                   RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                                   edited.overrides);
    INFO(failedFindings(regeneration.result.validation));
    CHECK(regeneration.result.validation.passed());
    CHECK(regeneration.impact.conflicts.empty());
    CHECK(regeneration.result.plan.rooms.front().growthSteps == 1);
    CHECK(capacityUnplaced(regeneration.result.plan) == 0);
    const Fixture* standing = fixtureOf(regeneration.result.plan, "room:snes/fx:e07");
    REQUIRE(standing != nullptr);
    CHECK(*standing == *decoration);
    REQUIRE(fixtureOf(regeneration.result.plan, "room:snes/fx:w08") != nullptr);
    CHECK(fixtureOf(regeneration.result.plan, "room:snes/fx:w08")->role == FixtureRole::SideModule);
    requirePinsAccounted(regeneration, edited.overrides);
    // The update of the same selection after it writes nothing.
    CHECK(updateRoom(regeneration.result.plan, inputs.selections.front(), kWorldSeed, regeneration.overrides).plan ==
          regeneration.result.plan);
}

TEST_CASE("world/regeneration: random edit sessions then a changed catalog: no override dropped, every pin honoured "
          "or a conflict, the update after writing nothing",
          "[world][regeneration][property]") {
    const EditData data = snesEditData();
    for (const std::uint64_t seed : {7ull, 8ull, 9ull}) {
        SnesWorld world = snesWorld(70, seed);
        EditedWorld edited{world.plan, {}};
        rv::core::Random random(rv::core::deriveSeed("regeneration-property", seed));
        std::vector<Fixture> movable;
        std::vector<SlotId> slots;
        for (const Fixture& fixture : edited.plan.fixtures) {
            if (fixture.role == FixtureRole::SideModule || fixture.role == FixtureRole::Display ||
                fixture.role == FixtureRole::Decoration) {
                movable.push_back(fixture);
            }
            for (const rv::world::FixtureLevel& level : fixture.levels) {
                for (std::uint32_t index = 0; index < level.slots; ++index) {
                    slots.push_back(*SlotId::make(fixture.id, level.level, index));
                }
            }
        }
        for (int step = 0; step < 40; ++step) {
            const std::vector<Placement>& placements = edited.plan.placements;
            const Placement game = placements[random.below(placements.size())];
            const Placement other = placements[random.below(placements.size())];
            EditOperation operation;
            switch (random.below(6)) {
            case 0:
            case 1:
                operation = EditOperation::move(game.game, slots[random.below(slots.size())]);
                break;
            case 2:
                operation = EditOperation::swap(game.game, other.game);
                break;
            case 3:
                operation = EditOperation::pin(game.game);
                break;
            default: {
                const Fixture& base = movable[random.below(movable.size())];
                const Fixture* now = fixtureOf(edited.plan, base.id.toString());
                const auto offset = [&] { return (static_cast<std::int64_t>(random.below(9)) - 4) * 100; };
                const std::int32_t turn = base.role == FixtureRole::Decoration ? 1500 : 9000;
                const auto yaw = (now->yawCdeg + turn * static_cast<std::int32_t>(random.below(4))) % 36000;
                operation = EditOperation::moveFixture(base.id, now->xMm + offset(), now->zMm + offset(), yaw);
                break;
            }
            }
            auto result = rv::world::applyEdit(edited, operation, data, kProvenance);
            REQUIRE(result.has_value());
        }
        REQUIRE_FALSE(edited.overrides.empty());
        // The catalog changes: titles arrive and some leave (a pinned one among them, perhaps).
        RoomSelection selection = world.inputs.selections.front();
        std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(selection);
        const std::size_t arriving = random.below(30);
        for (std::size_t i = 0; i < arriving; ++i) {
            rv::test::world::addTitle(selection, random, taken, random.below(4) == 0, random.below(50));
        }
        std::set<GameId> represented;
        for (const rv::world::UnplacedGame& game : selection.unplaced) {
            represented.insert(game.representative);
        }
        const std::size_t leaving = random.below(8);
        for (std::size_t i = 0; i < leaving && !selection.titles.empty(); ++i) {
            const std::size_t at = random.below(selection.titles.size());
            if (!represented.contains(selection.titles[at].game)) {
                selection.titles.erase(selection.titles.begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
        selection.distinctTitles = selection.titles.size();
        Inputs inputs = world.inputs;
        inputs.selections = {selection};
        const std::string fresh = planText(generateSelection(selection, kWorldSeed).plan);
        for (const RegenerationScope& scope : {RegenerationScope::ofRoom(*RoomId::parse("room:snes")),
                                               RegenerationScope::ofFloor("gen4"), RegenerationScope::ofWorld()}) {
            INFO("seed " << seed << ", " << scope.toString());
            const Regeneration regeneration = regenerateOf(edited.plan, inputs, scope, edited.overrides);
            INFO(failedFindings(regeneration.result.validation));
            REQUIRE(regeneration.result.validation.passed());
            // No override dropped: the game pins as they were, the fixture pins too (a south-wall poster's transform
            // following its wall).
            CHECK(withoutConflicts(regeneration.overrides).gamePins == edited.overrides.gamePins);
            requireConflictsRecorded(regeneration);
            REQUIRE(regeneration.overrides.fixturePins.size() == edited.overrides.fixturePins.size());
            for (std::size_t i = 0; i < edited.overrides.fixturePins.size(); ++i) {
                CHECK(regeneration.overrides.fixturePins[i].fixture == edited.overrides.fixturePins[i].fixture);
                CHECK(regeneration.overrides.fixturePins[i].prefab == edited.overrides.fixturePins[i].prefab);
            }
            requirePinsAccounted(regeneration, regeneration.overrides);
            requireImpactIsDifference(edited.plan, regeneration);
            const Regeneration twice = regenerateOf(edited.plan, inputs, scope, edited.overrides);
            CHECK(sha256(planText(twice.result.plan)) == sha256(planText(regeneration.result.plan)));
            const GenerationResult update =
                updateRoom(regeneration.result.plan, selection, kWorldSeed, regeneration.overrides);
            CHECK(update.plan == regeneration.result.plan);
            CHECK_FALSE(update.overrides.has_value());
            // With the consent: no pin left, the fresh plan of the selection.
            const Regeneration dropped = regenerateOf(edited.plan, inputs, scope, edited.overrides, true);
            CHECK(dropped.overrides.empty());
            CHECK(planText(dropped.result.plan) == fresh);
        }
    }
}

TEST_CASE("world/regeneration: the plans' difference lists its rooms sorted, whatever order the plans hold them in",
          "[world][regeneration]") {
    const auto room = [](std::string_view id, std::string floor) {
        rv::world::Room made;
        made.id = *RoomId::parse(id);
        made.floor = std::move(floor);
        return made;
    };
    WorldPlan before;
    before.rooms = {room("room:snes", "gen4"), room("room:gb", "handheld"), room("room:amiga", "computers")};
    WorldPlan after;
    after.rooms = {room("room:snes", "gen3"), room("room:psx", "gen5"), room("room:gb", "gen4"),
                   room("room:amiga", "computers"), room("room:mame", "ground")};
    const rv::world::RegenerationImpact impact = rv::world::planDifference(before, after);
    CHECK(impact.roomsAdded == std::vector<RoomId>{*RoomId::parse("room:mame"), *RoomId::parse("room:psx")});
    CHECK(impact.roomsMoved == std::vector<RoomId>{*RoomId::parse("room:gb"), *RoomId::parse("room:snes")});
    CHECK(impact.roomsRemoved.empty());
    CHECK(rv::world::planDifference(after, before).roomsRemoved ==
          std::vector<RoomId>{*RoomId::parse("room:mame"), *RoomId::parse("room:psx")});
}

TEST_CASE("world/regeneration: the pins an invalid layout blames: those named, else those applied in the finding's "
          "room, with that room's finding",
          "[world][regeneration]") {
    const auto pin = [](std::string_view fixture) {
        rv::world::FixturePin made;
        made.fixture = *FixtureId::parse(fixture);
        made.transform = rv::world::FixtureTransform{100, 200, 0};
        return made;
    };
    const auto hard = [](std::string rule, std::string subject, std::string message) {
        return rv::world::ValidationFinding{std::move(rule), rv::world::Severity::Hard, std::move(subject),
                                            std::move(message)};
    };
    const std::vector<rv::world::FixturePin> candidates{pin("room:gb/fx:dc01"), pin("room:snes/fx:dc01"),
                                                        pin("room:snes/fx:dc02"), pin("room:snes/fx:w03")};
    const std::set<FixtureId> applied{*FixtureId::parse("room:gb/fx:dc01"), *FixtureId::parse("room:snes/fx:dc02"),
                                      *FixtureId::parse("room:snes/fx:w03")};
    rv::world::ValidationReport report;
    // A finding naming a pin blames it alone (dc01 named, not dc010).
    report.findings = {hard("footprints-overlap", "room:snes/fx:dc010", "it overlaps room:snes/fx:dc01"),
                       hard("unreachable", "room:snes/fx:w05/s:1.2", "no path")};
    auto blamed = rv::world::detail::breakingPins(report, candidates, applied);
    REQUIRE(blamed.size() == 1);
    CHECK(blamed.begin()->first == *FixtureId::parse("room:snes/fx:dc01"));
    CHECK(blamed.begin()->second == "it breaks footprints-overlap: it overlaps room:snes/fx:dc01");
    // None named: the applied pins of the rooms the findings are about, each with its own room's finding; a pin not
    // applied (room:snes/fx:dc01) and a room no finding is about (room:gb) are not blamed; a soft finding, or one
    // whose subject is no room's, blames nothing.
    report.findings = {
        rv::world::ValidationFinding{"socket-bare", rv::world::Severity::Soft, "room:gb/fx:w01", "bare"},
        hard("game-twice", "g0123456789abcdef", "placed twice"),
        hard("unreachable", "room:snes/fx:w05/s:1.2", "no path to the slot"),
        hard("doorway-blocked", "room:psx", "the doorway is blocked")};
    blamed = rv::world::detail::breakingPins(report, candidates, applied);
    CHECK(blamed == std::map<FixtureId, std::string>{
                        {*FixtureId::parse("room:snes/fx:dc02"),
                         "its room breaks unreachable (room:snes/fx:w05/s:1.2): no path to the slot"},
                        {*FixtureId::parse("room:snes/fx:w03"),
                         "its room breaks unreachable (room:snes/fx:w05/s:1.2): no path to the slot"}});
    // Only rooms without applied pins: nothing blamed (the plan is refused as any).
    report.findings = {hard("doorway-blocked", "room:psx", "the doorway is blocked")};
    CHECK(rv::world::detail::breakingPins(report, candidates, applied).empty());
}

TEST_CASE("world/regeneration: refusals", "[world][regeneration]") {
    SnesWorld world = snesWorld(60, 51);
    rv::world::RegenerationInput input;
    input.library = rv::test::world::library();
    input.selections = world.inputs.selections;
    input.config = world.inputs.config;
    input.previous = world.plan;
    const auto refusal = [&](rv::world::RegenerationInput given) {
        auto result = rv::world::regenerate(given, snesData().building, snesData().systems, loader());
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    rv::world::RegenerationInput unknownRoom = input;
    unknownRoom.scope = RegenerationScope::ofRoom(*RoomId::parse("room:nes"));
    CHECK(refusal(unknownRoom) == rv::ErrorCode::InvalidArgument);
    rv::world::RegenerationInput unknownFloor = input;
    unknownFloor.scope = RegenerationScope::ofFloor("attic");
    CHECK(refusal(unknownFloor) == rv::ErrorCode::InvalidArgument);
    rv::world::RegenerationInput noSelection = input;
    noSelection.scope = RegenerationScope::ofRoom(*RoomId::parse("room:snes"));
    noSelection.selections.clear();
    CHECK(refusal(noSelection) == rv::ErrorCode::NotFound);
    rv::world::RegenerationInput otherSeed = input;
    otherSeed.scope = RegenerationScope::ofFloor("gen4");
    otherSeed.config.seed = 7;
    CHECK(refusal(otherSeed) == rv::ErrorCode::Unsupported);
    // The world regenerates a plan of another seed.
    rv::world::RegenerationInput world7 = otherSeed;
    world7.scope = RegenerationScope::ofWorld();
    auto regenerated = rv::world::regenerate(world7, snesData().building, snesData().systems, loader());
    REQUIRE(regenerated.has_value());
    CHECK(regenerated->result.plan.seed == 7);
    rv::world::RegenerationInput otherLibrary = input;
    otherLibrary.library = *rv::catalog::LibraryId::parse("other");
    CHECK(refusal(otherLibrary) == rv::ErrorCode::InvalidArgument);
    CHECK(RegenerationScope::ofFloor("gen4").toString() == "floor:gen4");
    CHECK(RegenerationScope::ofWorld().toString() == "world");
    CHECK(RegenerationScope::ofRoom(*RoomId::parse("area:arcade")).toString() == "area:arcade");
}
