// The one-time migration of I6 step 4 (rv/world/incremental.hpp's updateBuilding(); ADR-0014 §2), with the shipped
// data: a stored building of the old families (generator 1.3's: jewel cases, big boxes and generic cases standing on
// their end) updated by this generator regenerates each room of a succeeded family once with its successor (standing
// on its side), keeping its id, floor, x, yaw, sections and seed, its face-out titles chosen again by the same
// priority; every other room (the cartridges') stays byte for byte in its elements; the report lists the migrated
// rooms; a second update writes nothing; a pinned room or one without every section's selection is not migrated.

#include "world_building_support.hpp"

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
        {*rv::world::RoomId::parse("area:arcade"), "generic_case", "generic_case_v2"},
        {*rv::world::RoomId::parse("area:shared:gen6"), "generic_case", "generic_case_v2"},
        {*rv::world::RoomId::parse("room:amiga"), "big_box", "big_box_v2"},
        {*rv::world::RoomId::parse("room:psx"), "disc_case_cd", "disc_case_cd_v2"},
    };
    CHECK(migrated.changes.migratedRooms == expected);
    const WorldPlan& plan = migrated.plan;
    CHECK(plan.generatorVersion == rv::world::kGeneratorVersion);

    // The cartridges' rooms stand byte for byte in their elements.
    for (const std::string_view id : {"room:snes", "room:gb"}) {
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
        // A later update given the rest migrates the rest, once.
        const GenerationResult rest = updateBuildingOf(world, partial.plan, given);
        CHECK(rest.changes.migratedRooms.size() == 3);
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
        CHECK(pinned->changes.migratedRooms.size() == 3);
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
    CHECK(findRoom(fresh.plan, "area:arcade")->mediaFamily == "generic_case_v2");
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
