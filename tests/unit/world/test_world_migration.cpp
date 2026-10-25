// The one-time migration of I6 step 4 (rv/world/incremental.hpp's updateBuilding(); ADR-0014 §2), with the shipped
// data: a stored building of the old families (generator 1.3's: jewel cases, big boxes and generic cases standing on
// their end) updated by this generator regenerates each room of a family succeeded in rooms of its kind once with its
// successor (standing on its side), keeping its id, floor, x, yaw, sections and seed, its face-out titles chosen again
// by the same priority; every other room (the cartridges', and the arcade room, whose generic case keeps until I6 step
// 6's cabinets) stays byte for byte in its elements; the report lists the migrated rooms; a second update writes
// nothing; a pinned room or one without every section's selection is not migrated; a migrated room may grow beyond
// its successor's capacity (its titles unplaced for capacity, counted in the report) or shrink, its z following its
// depth either way; main's generator-1.3 golden building (tests/fixtures/world/legacy/) migrated once.

#include "world_building_support.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/validation.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using rv::world::GenerationResult;
using rv::world::MigratedRoom;
using rv::world::Room;
using rv::world::RoomSelection;
using rv::world::WorldData;
using rv::world::WorldPlan;

namespace {

using namespace rv::test::world;

constexpr std::uint64_t kWorldSeed = 102938;

/// A building of every kind of room I6 step 4 touches: the snes and gb cartridge rooms (no successor), the psx jewel
/// cases and the amiga big boxes in rooms of their own, ps2's 3 titles in the shared room of gen6 and mame in the
/// arcade room (both generic cases).
std::vector<RoomSelection> selections() {
    return {systemSelection("snes", 40, 11),  systemSelection("gb", 45, 12), systemSelection("psx", 160, 13),
            systemSelection("amiga", 60, 14), systemSelection("ps2", 3, 15), systemSelection("mame", 12, 16)};
}

/// The building generator 1.3 made of `selections`: the old families (legacyEnteringData()).
WorldPlan legacyBuilding(const BuildingWorld& world, const std::vector<RoomSelection>& given) {
    std::vector<WorldData> rooms;
    for (const RoomSelection& selection : given) {
        rooms.push_back(legacyEnteringData(world.root, selection.room.system().str()));
    }
    rv::world::BuildingInput input;
    input.library = library();
    input.selections = given;
    input.config = world.config;
    auto result = rv::world::generateBuilding(input, world.building, world.systems, rooms);
    INFO((result ? std::string("generated") : result.error().toString()));
    REQUIRE(result.has_value());
    REQUIRE(result->validation.passed());
    return result->plan;
}

/// The room `id` and its elements, as a plan of that one room (for byte comparisons).
WorldPlan roomPart(const WorldPlan& plan, std::string_view id) {
    WorldPlan part;
    for (const Room& room : plan.rooms) {
        if (room.id.toString() == id) {
            part.rooms.push_back(room);
        }
    }
    const auto ofRoom = [&](const rv::world::RoomId& room) {
        return room.toString() == id;
    };
    for (const auto& fixture : plan.fixtures) {
        if (ofRoom(fixture.id.room())) {
            part.fixtures.push_back(fixture);
        }
    }
    for (const auto& placement : plan.placements) {
        if (ofRoom(placement.slot.fixture().room())) {
            part.placements.push_back(placement);
        }
    }
    for (const auto& dressed : plan.dressing) {
        if (ofRoom(dressed.level.fixture().room())) {
            part.dressing.push_back(dressed);
        }
    }
    for (const auto& light : plan.lights) {
        if (ofRoom(light.id.room())) {
            part.lights.push_back(light);
        }
    }
    for (const auto& game : plan.unplaced) {
        if (ofRoom(game.room)) {
            part.unplaced.push_back(game);
        }
    }
    return part;
}

} // namespace

TEST_CASE("world/migration: rooms of succeeded families are migrated once, every other room stays byte for byte",
          "[world][migration]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> given = selections();
    const WorldPlan legacy = legacyBuilding(world, given);
    // The old building: its cases and boxes of the old families.
    REQUIRE(findRoom(legacy, "room:psx") != nullptr);
    CHECK(findRoom(legacy, "room:psx")->mediaFamily == "disc_case_cd");
    CHECK(findRoom(legacy, "room:amiga")->mediaFamily == "big_box");
    CHECK(findRoom(legacy, "area:shared:gen6")->mediaFamily == "generic_case");
    CHECK(findRoom(legacy, "area:arcade")->mediaFamily == "generic_case");
    CHECK(findRoom(legacy, "room:snes")->mediaFamily == "cartridge_home_16bit");

    const GenerationResult migrated = updateBuildingOf(world, legacy, given);
    for (const rv::world::ValidationFinding& finding : migrated.validation.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            UNSCOPED_INFO(finding.rule << " " << finding.subject << ": " << finding.message);
        }
    }
    CHECK(migrated.validation.passed());
    CHECK(migrated.changes.outcome == rv::world::GenerationOutcome::Updated);
    CHECK(migrated.changes.rebuild == rv::world::RebuildLevel::Full);
    CHECK(migrated.changes.keptRooms.empty());
    const std::vector<MigratedRoom> expected{
        {*rv::world::RoomId::parse("area:shared:gen6"), "generic_case", "generic_case_v2"},
        {*rv::world::RoomId::parse("room:amiga"), "big_box", "big_box_v2"},
        {*rv::world::RoomId::parse("room:psx"), "disc_case_cd", "disc_case_cd_v2"},
    };
    CHECK(migrated.changes.migratedRooms == expected);
    const WorldPlan& plan = migrated.plan;
    CHECK(plan.generatorVersion == rv::world::kGeneratorVersion);

    // The cartridges' rooms and the arcade room stand byte for byte in their elements.
    CHECK(findRoom(plan, "area:arcade")->mediaFamily == "generic_case");
    for (const std::string_view id : {"room:snes", "room:gb", "area:arcade"}) {
        INFO(id);
        auto before = rv::world::roomContentDigest(legacy, *rv::world::RoomId::parse(id));
        auto after = rv::world::roomContentDigest(plan, *rv::world::RoomId::parse(id));
        REQUIRE(before.has_value());
        REQUIRE(after.has_value());
        CHECK(*before == *after);
    }
    // Each migrated room: its id, kind, floor, x, yaw, sections and seed kept, its family the successor, its theme pack
    // and template as recorded, its z following its depth; every title of its sections placed (none over capacity).
    for (const MigratedRoom& entry : expected) {
        const std::string id = entry.room.toString();
        INFO(id);
        const Room* then = findRoom(legacy, id);
        const Room* now = findRoom(plan, id);
        REQUIRE(then != nullptr);
        REQUIRE(now != nullptr);
        CHECK(now->kind == then->kind);
        CHECK(now->floor == then->floor);
        CHECK(now->sections == then->sections);
        CHECK(now->seed == then->seed);
        CHECK(now->placement.xMm == then->placement.xMm);
        CHECK(now->placement.yawHalfTurns == then->placement.yawHalfTurns);
        CHECK(now->placement == rv::world::followDepth(*now, world.building.layout));
        CHECK(now->mediaFamily == entry.to);
        CHECK(now->themePack == then->themePack);
        CHECK(now->templateId == then->templateId);
        CHECK(now->templateVersion == then->templateVersion);
        CHECK(now->generatorVersion == rv::world::kGeneratorVersion);
        // Its fixtures are its successor's modules.
        const WorldPlan part = roomPart(plan, id);
        bool successorModules = true;
        for (const auto& fixture : part.fixtures) {
            if (fixture.role == rv::world::FixtureRole::SideModule ||
                fixture.role == rv::world::FixtureRole::HeroModule) {
                successorModules = successorModules && fixture.prefab.manifest.find("_side.") != std::string::npos;
            }
        }
        CHECK(successorModules);
        for (const auto& game : part.unplaced) {
            CHECK(game.reason != rv::world::UnplacedReason::Capacity);
        }
    }
    // Every game of the selections placed or unplaced once; the face-out titles of psx chosen again by priority: the
    // first D + B of its selection.
    std::size_t titles = 0;
    for (const RoomSelection& selection : given) {
        titles += selection.titles.size();
    }
    CHECK(plan.placements.size() == titles);
    {
        const WorldPlan psx = roomPart(plan, "room:psx");
        const auto faceOut = faceOutPlacements(psx);
        const RoomSelection& selection = given[2];
        std::set<rv::catalog::GameId> first;
        for (std::size_t i = 0; i < faceOut.size(); ++i) {
            first.insert(selection.titles[i].game);
        }
        std::set<rv::catalog::GameId> shown;
        for (const auto& placement : faceOut) {
            shown.insert(placement.game);
        }
        CHECK(faceOut.size() == 48); // D 38 + B 10
        CHECK(shown == first);
    }
    // The plan reads back as written, its digest the report's.
    auto digest = rv::world::planDigest(plan);
    REQUIRE(digest.has_value());
    CHECK(*digest == migrated.changes.digestAfter);

    // A second update of the same catalog migrates nothing and writes nothing.
    const GenerationResult again = updateBuildingOf(world, plan, given);
    CHECK(again.changes.migratedRooms.empty());
    CHECK(again.changes.digestAfter == again.changes.digestBefore);
    CHECK(again.plan == plan);
    CHECK(again.changes.rebuild == rv::world::RebuildLevel::NoRebuild);
}

TEST_CASE("world/migration: a room without every section's selection or with a pin is not migrated",
          "[world][migration]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> given = selections();
    const WorldPlan legacy = legacyBuilding(world, given);

    // Only snes and psx given: psx is migrated, the others (amiga, the shared and arcade rooms) stand as they were.
    {
        const GenerationResult partial = updateBuildingOf(world, legacy, {given[0], given[2]});
        REQUIRE(partial.changes.migratedRooms.size() == 1);
        CHECK(partial.changes.migratedRooms.front().room.toString() == "room:psx");
        for (const std::string_view id : {"room:amiga", "area:shared:gen6", "area:arcade", "room:gb"}) {
            INFO(id);
            auto before = rv::world::roomContentDigest(legacy, *rv::world::RoomId::parse(id));
            auto after = rv::world::roomContentDigest(partial.plan, *rv::world::RoomId::parse(id));
            REQUIRE(before.has_value());
            REQUIRE(after.has_value());
            CHECK(*before == *after);
        }
        // A later update given the rest migrates the rest, once (the arcade room stays).
        const GenerationResult rest = updateBuildingOf(world, partial.plan, given);
        CHECK(rest.changes.migratedRooms.size() == 2);
        CHECK(findRoom(rest.plan, "room:psx")->mediaFamily == "disc_case_cd_v2");
        auto psxBefore = rv::world::roomContentDigest(partial.plan, *rv::world::RoomId::parse("room:psx"));
        auto psxAfter = rv::world::roomContentDigest(rest.plan, *rv::world::RoomId::parse("room:psx"));
        CHECK(*psxBefore == *psxAfter);
    }

    // A pin names the psx room: it is updated as it stands, and the refusal is reported.
    {
        const auto placement = std::find_if(legacy.placements.begin(), legacy.placements.end(), [](const auto& p) {
            return p.slot.fixture().room().toString() == "room:psx";
        });
        REQUIRE(placement != legacy.placements.end());
        std::vector<rv::world::WorldData> rooms;
        for (const Room& room : legacy.rooms) {
            rooms.push_back(storedData(world.root, room));
        }
        rv::world::BuildingUpdateInput input;
        input.library = library();
        input.selections = given;
        input.config = world.config;
        input.previous = legacy;
        input.overrides.gamePins.push_back({placement->game, placement->slot});
        auto pinned = rv::world::updateBuilding(input, world.building, world.systems, rooms);
        REQUIRE(pinned.has_value());
        CHECK(pinned->validation.passed());
        CHECK(findRoom(pinned->plan, "room:psx")->mediaFamily == "disc_case_cd");
        CHECK(pinned->changes.migratedRooms.size() == 2);
        CHECK(std::any_of(pinned->changes.needsConfirmation.begin(), pinned->changes.needsConfirmation.end(),
                          [](const std::string& text) { return text.find("migrate room:psx") != std::string::npos; }));
    }
}

TEST_CASE("world/migration: a new room of a succeeded family takes its successor", "[world][migration]") {
    // A fresh building of the shipped data plans the successors; its update migrates nothing.
    BuildingWorld world = shippedWorld(kWorldSeed);
    const std::vector<RoomSelection> given = selections();
    const GenerationResult fresh = generateBuildingOf(world, given);
    CHECK(findRoom(fresh.plan, "room:psx")->mediaFamily == "disc_case_cd_v2");
    CHECK(findRoom(fresh.plan, "room:amiga")->mediaFamily == "big_box_v2");
    CHECK(findRoom(fresh.plan, "area:shared:gen6")->mediaFamily == "generic_case_v2");
    CHECK(findRoom(fresh.plan, "area:arcade")->mediaFamily == "generic_case");
    const GenerationResult updated = updateBuildingOf(world, fresh.plan, given);
    CHECK(updated.changes.migratedRooms.empty());
    CHECK(updated.plan == fresh.plan);

    // A system entering an old building gets a room of the successor family, beside the old rooms.
    const WorldPlan legacy = legacyBuilding(world, {given[0], given[2]});
    std::vector<RoomSelection> more{given[0], given[2], systemSelection("saturn", 50, 17)};
    const GenerationResult entered = updateBuildingOf(world, legacy, more);
    REQUIRE(findRoom(entered.plan, "room:saturn") != nullptr);
    CHECK(findRoom(entered.plan, "room:saturn")->mediaFamily == "disc_case_cd_v2");
    CHECK(entered.changes.migratedRooms.size() == 1);
}

TEST_CASE("world/migration: a room grows past its successor's capacity or shrinks, its z following its depth",
          "[world][migration]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    // amiga's 1,700 big boxes fill the old room grown to 16 m (1,923 of big_box), not the new one (1,561 of big_box_v2:
    // standing on their side they take 4 levels of 16 rather than 5); psx's 5,600 jewel cases need the old room grown
    // (5,398 of disc_case_cd in 9 x 10 m) and fit the new one as it is (6,177 of disc_case_cd_v2: a level more).
    const std::vector<RoomSelection> given{systemSelection("amiga", 1700, 21), systemSelection("psx", 5600, 22)};
    const WorldPlan legacy = legacyBuilding(world, given);
    const GenerationResult migrated = updateBuildingOf(world, legacy, given);
    CHECK(migrated.validation.passed());
    REQUIRE(migrated.changes.migratedRooms.size() == 2);
    const auto capacityIn = [](const WorldPlan& plan, std::string_view id) {
        return static_cast<std::uint64_t>(
            std::count_if(plan.unplaced.begin(), plan.unplaced.end(), [&](const rv::world::UnplacedGame& game) {
                return game.room.toString() == id && game.reason == rv::world::UnplacedReason::Capacity;
            }));
    };
    for (const MigratedRoom& entry : migrated.changes.migratedRooms) {
        const std::string id = entry.room.toString();
        INFO(id);
        const Room* then = findRoom(legacy, id);
        const Room* now = findRoom(migrated.plan, id);
        REQUIRE(then != nullptr);
        REQUIRE(now != nullptr);
        CHECK(capacityIn(legacy, id) == 0);
        CHECK(entry.unplacedForCapacity == capacityIn(migrated.plan, id));
        CHECK(now->placement == rv::world::followDepth(*now, world.building.layout));
        CHECK(now->placement.xMm == then->placement.xMm);
        if (id == "room:amiga") {
            // Grown to its largest and still short: the rest unplaced for capacity, counted in the report.
            CHECK(entry.unplacedForCapacity == 1700 - 1561);
            CHECK(now->growthSteps >= then->growthSteps);
        } else {
            // Shallower than before: its z follows its depth back towards the hall.
            CHECK(entry.unplacedForCapacity == 0);
            CHECK(then->growthSteps > 0);
            CHECK(now->growthSteps < then->growthSteps);
            CHECK(now->placement.zMm != then->placement.zMm);
        }
    }
    // The next update moves nothing and writes nothing.
    const GenerationResult again = updateBuildingOf(world, migrated.plan, given);
    CHECK(again.changes.migratedRooms.empty());
    CHECK(again.plan == migrated.plan);
}

TEST_CASE("world/migration: main's generator-1.3 building of the building fixture is migrated once",
          "[world][migration]") {
    // tests/fixtures/world/legacy/README.md: the golden building as generator 1.3 wrote it, updated with the selections
    // that made it (test_world_building_update.cpp's).
    BuildingWorld world = shippedWorld(kWorldSeed);
    world.tags[systemName("pocketlab")] = {"gb"};
    struct Entry {
        std::string_view system;
        std::size_t titles;
        std::size_t distinct;
    };
    const Entry systems[] = {{"amiga", 20, 0}, {"gb", 30, 0},       {"mame", 15, 25}, {"pocketlab", 6, 0},
                             {"pokemini", 8, 0}, {"psx", 40, 0}, {"snes", 40, 0}};
    std::vector<RoomSelection> given;
    std::uint64_t seed = 4000;
    for (const Entry& entry : systems) {
        given.push_back(systemSelection(entry.system, entry.titles, seed++, entry.distinct));
    }
    const std::filesystem::path file = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" /
                                       "legacy" / "building-esde-seed-102938-generator-1.3.json";
    auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    auto legacy = rv::world::readPlan(*text, "legacy");
    REQUIRE(legacy.has_value());
    REQUIRE(legacy->generatorVersion == rv::world::GeneratorVersion{1, 3});

    const GenerationResult migrated = updateBuildingOf(world, *legacy, given);
    CHECK(migrated.validation.passed());
    CHECK(migrated.changes.outcome == rv::world::GenerationOutcome::Updated);
    CHECK(migrated.changes.rebuild == rv::world::RebuildLevel::Full);
    const std::vector<MigratedRoom> expected{
        {*rv::world::RoomId::parse("area:shared:computer"), "generic_case", "generic_case_v2"},
        {*rv::world::RoomId::parse("area:shared:handheld"), "generic_case", "generic_case_v2"},
        {*rv::world::RoomId::parse("room:psx"), "disc_case_cd", "disc_case_cd_v2"},
    };
    CHECK(migrated.changes.migratedRooms == expected);
    CHECK(migrated.changes.needsConfirmation.empty());
    CHECK(migrated.plan.generatorVersion == rv::world::kGeneratorVersion);

    // Every room records the pack its floor resolves now (I6 step 3, ADR-0014 §1: a look, written by every update),
    // whether migrated or not; the legacy plan recorded the kinds' defaults of generator 1.3's time.
    const std::map<std::string, std::string> packs = {
        {"area:arcade", "arcade_after_dark"},        {"area:shared:computer", "computer_studio"},
        {"area:shared:handheld", "handheld_archive"}, {"room:gb", "handheld_archive"},
        {"room:psx", "console_90s_collector"},       {"room:snes", "classic_16bit_warm"}};
    REQUIRE(migrated.plan.rooms.size() == packs.size());
    for (const Room& room : migrated.plan.rooms) {
        INFO(room.id.toString());
        CHECK(room.themePack == packs.at(room.id.toString()));
    }

    // Every other room byte for byte, and the whole plan outside the migrated rooms but for the generator's stamps,
    // the inputs' digest and the rewritten packs: the floors, the hall's lights, every element of gb, snes and the
    // arcade room.
    const auto withoutMigrated = [&](WorldPlan plan) {
        for (const MigratedRoom& entry : expected) {
            std::erase_if(plan.rooms, [&](const Room& room) { return room.id == entry.room; });
            std::erase_if(plan.fixtures, [&](const auto& fixture) { return fixture.id.room() == entry.room; });
            std::erase_if(plan.placements,
                          [&](const auto& placement) { return placement.slot.fixture().room() == entry.room; });
            std::erase_if(plan.dressing,
                          [&](const auto& dressed) { return dressed.level.fixture().room() == entry.room; });
            std::erase_if(plan.lights, [&](const auto& light) { return light.id.room() == entry.room; });
            std::erase_if(plan.unplaced, [&](const auto& game) { return game.room == entry.room; });
        }
        plan.generatorVersion = rv::world::GeneratorVersion{1, 3};
        plan.inputDigest = legacy->inputDigest;
        for (Room& room : plan.rooms) {
            room.generatorVersion = rv::world::GeneratorVersion{1, 3};
            room.themePack = findRoom(*legacy, room.id.toString())->themePack;
        }
        auto written = rv::world::writePlan(plan);
        REQUIRE(written.has_value());
        return *written;
    };
    CHECK(withoutMigrated(migrated.plan) == withoutMigrated(*legacy));
    CHECK(findRoom(migrated.plan, "area:arcade")->mediaFamily == "generic_case");
    // The migrated rooms: their successor's, every title of theirs placed as before.
    for (const MigratedRoom& entry : expected) {
        INFO(entry.room.toString());
        const Room* now = findRoom(migrated.plan, entry.room.toString());
        REQUIRE(now != nullptr);
        CHECK(now->mediaFamily == entry.to);
        CHECK(now->generatorVersion == rv::world::kGeneratorVersion);
        CHECK(roomPart(migrated.plan, entry.room.toString()).placements.size() ==
              roomPart(*legacy, entry.room.toString()).placements.size());
    }
    // Once.
    const GenerationResult again = updateBuildingOf(world, migrated.plan, given);
    CHECK(again.changes.migratedRooms.empty());
    CHECK(again.plan == migrated.plan);
    CHECK(again.changes.digestAfter == again.changes.digestBefore);
}
