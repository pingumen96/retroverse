// The schema-1 lift (rv/world/lift.hpp, ADR-0013 §5; I4 step 1), checked against the frozen oracles of
// tests/fixtures/world/v1/ (never rewritten): every oracle lifted, then updated by this generator with the selection it
// was made from (world_generation_support.hpp's makeSelection, as tests/fixtures/world/golden/README.md describes),
// keeps its room's id, template and version, bounds, height, growth steps, seed, media family and theme pack, and every
// fixture, placement, dressing, light and unplaced element equal to the oracle's (the 8,000 titles' oracle: its every
// element kept, its titles unplaced for capacity added to its chain by generator 1.5); a fresh generation gives the
// oracle's room too, below the template's limit. The lift itself: pure, a valid building (the room on its system's
// floor at the north row's first position, the ground floor, nothing else), refused for what is not a schema-1 room; a
// lifted plan updated with nothing new is written once, in schema 2, and then left as it is.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/lift.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_store.hpp>

#include "support/temp_dir.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::world::GenerationOutcome;
using rv::world::GenerationResult;
using rv::world::RebuildLevel;
using rv::world::RoomSelection;
using rv::world::RoomTitle;
using rv::world::UnplacedGame;
using rv::world::UnplacedReason;
using rv::world::WorldPlan;
using rv::test::world::makeSelection;
using rv::test::world::snesData;

namespace {

constexpr std::uint64_t kSelectionSeed = 20261018;

const std::filesystem::path kOracleDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "v1";

/// An oracle's file: its text and its plan as read (schema 1).
struct Oracle {
    std::string text;
    WorldPlan plan;
};

Oracle readOracle(std::string_view name) {
    auto text = rv::core::readTextFile(kOracleDir / rv::core::pathFromUtf8(std::string(name)));
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, name);
    REQUIRE(plan.has_value());
    REQUIRE(plan->schemaVersion == 1);
    return {std::move(*text), std::move(*plan)};
}

/// The titles of a selection that no other version names as its representative (as test_world_update.cpp's).
std::vector<GameId> titlesWithoutVersions(const RoomSelection& selection) {
    std::set<GameId> represented;
    for (const UnplacedGame& game : selection.unplaced) {
        if (game.reason == UnplacedReason::DuplicateTitle) {
            represented.insert(game.representative);
        }
    }
    std::vector<GameId> titles;
    for (const RoomTitle& title : selection.titles) {
        if (!represented.contains(title.game)) {
            titles.push_back(title.game);
        }
    }
    return titles;
}

void removeTitle(RoomSelection& selection, const GameId& game) {
    std::erase_if(selection.titles, [&](const RoomTitle& title) { return title.game == game; });
    std::erase_if(selection.unplaced, [&](const UnplacedGame& unplaced) {
        return unplaced.reason == UnplacedReason::DuplicateTitle && unplaced.representative == game;
    });
}

/// The final selections of the golden update sequences (test_world_update.cpp's "golden update sequences" and G10),
/// made the same way.
RoomSelection g10Selection() {
    return makeSelection(60, kSelectionSeed);
}

struct Sequences {
    RoomSelection plusFiveMinusThree;
    RoomSelection fullPlusTwenty;
};

Sequences sequenceSelections() {
    const RoomSelection full = makeSelection(893, kSelectionSeed);
    std::vector<GameId> held = titlesWithoutVersions(full);
    rv::core::Random random(893);
    random.shuffle(std::span<GameId>(held));
    Sequences sequences;
    sequences.plusFiveMinusThree = full;
    const std::vector<GameId> leaving{full.titles[10].game, full.titles[400].game, full.titles[800].game};
    for (const GameId& game : leaving) {
        removeTitle(sequences.plusFiveMinusThree, game);
    }
    const RoomSelection big = makeSelection(3205, kSelectionSeed);
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(big);
    const WorldPlan plan = rv::test::world::generateSelection(big, 102938).plan;
    const rv::test::world::SpineCounts counts = rv::test::world::countSpine(plan);
    sequences.fullPlusTwenty = big;
    for (std::uint64_t i = 0; i < counts.freeSlots + counts.dressedSlots + 20; ++i) {
        rv::test::world::addTitle(sequences.fullPlusTwenty, random, taken);
    }
    return sequences;
}

/// The room's record of `plan` without schema 2's fields: what the lift must keep from the oracle.
struct RoomCore {
    rv::world::RoomId id;
    std::string templateId;
    std::uint32_t templateVersion = 0;
    std::int64_t minXMm = 0;
    std::int64_t maxXMm = 0;
    std::int64_t minZMm = 0;
    std::int64_t maxZMm = 0;
    std::int64_t heightMm = 0;
    std::uint32_t growthSteps = 0;
    std::uint64_t seed = 0;
    std::string mediaFamily;
    std::string themePack;

    bool operator==(const RoomCore&) const = default;
};

RoomCore coreOf(const rv::world::Room& room) {
    return {room.id,     room.templateId, room.templateVersion, room.minXMm,      room.maxXMm,     room.minZMm,
            room.maxZMm, room.heightMm,   room.growthSteps,     room.seed,        room.mediaFamily, room.themePack};
}

/// The oracle's room and elements equal `plan`'s (ADR-0013 §5's invariant).
void checkProjection(const WorldPlan& plan, const WorldPlan& oracle) {
    REQUIRE(plan.rooms.size() == 1);
    REQUIRE(oracle.rooms.size() == 1);
    CHECK(coreOf(plan.rooms.front()) == coreOf(oracle.rooms.front()));
    CHECK(plan.fixtures == oracle.fixtures);
    CHECK(plan.placements == oracle.placements);
    CHECK(plan.dressing == oracle.dressing);
    CHECK(plan.lights == oracle.lights);
    CHECK(plan.unplaced == oracle.unplaced);
}

GenerationResult updateLifted(const Oracle& oracle, const RoomSelection& selection, std::uint64_t worldSeed) {
    rv::world::UpdateInput input;
    input.library = rv::test::world::library();
    input.selection = selection;
    input.config.seed = worldSeed;
    input.previous = oracle.plan;
    input.previousDigest = rv::core::sha256Hex(oracle.text);
    auto result = rv::world::update(input, snesData());
    if (!result) {
        UNSCOPED_INFO(result.error().toString());
    }
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("world/lift: every v1 oracle lifted and updated by 1.3 with its selection keeps its room and elements",
          "[world][lift]") {
    struct Case {
        std::string name;
        RoomSelection selection;
        std::uint64_t worldSeed = 102938;
    };
    std::vector<Case> cases;
    for (const std::uint64_t seed : {std::uint64_t{102938}, std::uint64_t{7}, std::uint64_t{0xFFFFFFFFFFFFFFFF}}) {
        cases.push_back({fmt::format("plan-890-seed-{}.json", seed), makeSelection(890, kSelectionSeed), seed});
    }
    for (const std::size_t titles : {50, 2000, 8000}) {
        cases.push_back({fmt::format("plan-{}-seed-102938.json", titles), makeSelection(titles, kSelectionSeed)});
    }
    cases.push_back({"update-50-plus-10-seed-102938.json", g10Selection()});
    Sequences sequences = sequenceSelections();
    cases.push_back({"update-890-plus-5-minus-3-seed-102938.json", std::move(sequences.plusFiveMinusThree)});
    cases.push_back({"update-3205-full-plus-20-seed-102938.json", std::move(sequences.fullPlusTwenty)});
    // Every oracle of the folder has its case.
    std::set<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(kOracleDir)) {
        if (entry.path().extension() == ".json") {
            files.insert(rv::core::pathToUtf8(entry.path().filename()));
        }
    }
    std::set<std::string> covered;
    for (const Case& test : cases) {
        covered.insert(test.name);
    }
    CHECK(covered == files);

    for (const Case& test : cases) {
        INFO(test.name);
        const Oracle oracle = readOracle(test.name);
        // The lift alone.
        auto lifted = rv::world::lift(oracle.plan, snesData().building, snesData().systems);
        REQUIRE(lifted.has_value());
        checkProjection(*lifted, oracle.plan);
        CHECK(lifted->generatorVersion == oracle.plan.generatorVersion);
        CHECK(lifted->inputDigest == oracle.plan.inputDigest);
        // Then an update with the same selection: nothing moves, the room is now this generator's, the file schema 2.
        const GenerationResult updated = updateLifted(oracle, test.selection, test.worldSeed);
        CHECK(updated.changes.outcome == GenerationOutcome::Updated);
        CHECK(updated.changes.unplaced.empty());
        CHECK(updated.changes.kept == oracle.plan.placements.size());
        CHECK(updated.changes.keptRooms.empty());
        CHECK(updated.changes.digestBefore == rv::core::sha256Hex(oracle.text));
        CHECK(updated.changes.digestAfter != updated.changes.digestBefore);
        CHECK(updated.validation.passed());
        CHECK(updated.plan.schemaVersion == 2);
        CHECK(updated.plan.generatorVersion == rv::world::kGeneratorVersion);
        CHECK(updated.plan.rooms.front().generatorVersion == rv::world::kGeneratorVersion);
        CHECK(updated.plan.rooms.front().placement == lifted->rooms.front().placement);
        std::vector<rv::catalog::GameId> overCapacity;
        for (const rv::world::UnplacedGame& game : oracle.plan.unplaced) {
            if (game.reason == rv::world::UnplacedReason::Capacity) {
                overCapacity.push_back(game.game);
            }
        }
        std::sort(overCapacity.begin(), overCapacity.end());
        if (overCapacity.empty()) {
            CHECK(updated.changes.added.empty());
            CHECK(updated.changes.rebuild == RebuildLevel::NoRebuild);
            checkProjection(updated.plan, oracle.plan);
        } else {
            // Generator 1.5 (I4b step 7, ADR-0015 §7): the titles the oracle's room at its limit left unplaced for
            // capacity enter its chain as additions, the head's free slots first, then a new expansion; every element
            // of the oracle stands as it stood.
            CHECK(updated.changes.added == overCapacity);
            CHECK(updated.changes.roomsAdded == std::vector{*updated.plan.rooms.front().id.inChain(2)});
            REQUIRE(updated.plan.rooms.size() == 2);
            CHECK(coreOf(updated.plan.rooms.front()) == coreOf(oracle.plan.rooms.front()));
            CHECK(std::includes(updated.plan.placements.begin(), updated.plan.placements.end(),
                                oracle.plan.placements.begin(), oracle.plan.placements.end()));
            CHECK(std::none_of(updated.plan.unplaced.begin(), updated.plan.unplaced.end(),
                               [](const rv::world::UnplacedGame& game) {
                                   return game.reason == rv::world::UnplacedReason::Capacity;
                               }));
        }
        // A fresh generation of the plan-* oracles' selections the template's limit holds gives their rooms too
        // (generate() stands on I3's room-level rules; past the limit it makes a chain since generator 1.5).
        if (test.name.starts_with("plan-") && overCapacity.empty()) {
            rv::world::GenerationInput input;
            input.library = rv::test::world::library();
            input.selection = test.selection;
            input.config.seed = test.worldSeed;
            auto fresh = rv::world::generate(input, snesData());
            REQUIRE(fresh.has_value());
            checkProjection(fresh->plan, oracle.plan);
            CHECK(fresh->plan.rooms.front().placement == lifted->rooms.front().placement);
        }
    }
}

TEST_CASE("world/lift: a pure function giving a valid building", "[world][lift]") {
    const Oracle oracle = readOracle("plan-890-seed-102938.json");
    const rv::world::BuildingData& building = snesData().building;
    const rv::world::SystemsData& systems = snesData().systems;
    auto lifted = rv::world::lift(oracle.plan, building, systems);
    REQUIRE(lifted.has_value());
    CHECK(*rv::world::lift(oracle.plan, building, systems) == *lifted);
    CHECK(lifted->schemaVersion == 2);
    // The ground floor and the system's floor, nothing else.
    CHECK(lifted->floors == std::vector<rv::world::Floor>{{"ground", 0}, {"gen4", 3}});
    const rv::world::Room& room = lifted->rooms.front();
    CHECK(room.kind == rv::world::RoomKind::Console);
    CHECK(room.floor == "gen4");
    CHECK(room.sections == std::vector<rv::catalog::SystemName>{rv::test::world::snes()});
    CHECK(room.expands.empty());
    CHECK(room.generatorVersion == rv::world::GeneratorVersion{1, 2});
    // The north row's first position: the footprint's west edge on the rows' start, the doorway wall on the hall.
    CHECK(room.placement == rv::world::RoomPlacement{7650, -6650, 0});
    const rv::world::Footprint footprint = rv::world::floorFootprint(room, building.layout);
    CHECK(footprint.minXMm == building.layout.rowStartXMm);
    CHECK(footprint.maxZMm == -building.layout.hallWidthMm / 2);
    // Written in schema 2 and read back as itself.
    auto text = rv::world::writePlan(*lifted);
    REQUIRE(text.has_value());
    CHECK(*rv::world::readPlan(*text, "lifted.json") == *lifted);
    CHECK(rv::core::sha256Hex(*text) != rv::core::sha256Hex(oracle.text));
    CHECK(*rv::world::liftIfNeeded(*lifted, building, systems) == *lifted);
    CHECK(*rv::world::liftIfNeeded(oracle.plan, building, systems) == *lifted);
    // A valid building, as worldplan check judges one.
    rv::world::ValidationContext context;
    context.templates = {snesData().roomTemplate};
    context.config.seed = 102938;
    context.selections = {makeSelection(890, kSelectionSeed)};
    context.prefabSlots = rv::world::manifestPrefabSlots(rv::test::world::assetRoot());
    context.lights = {{room.id, static_cast<std::uint32_t>(lifted->lights.size())}};
    context.building = building;
    const rv::world::ValidationReport report = rv::world::validatePlan(*lifted, context);
    for (const rv::world::ValidationFinding& finding : report.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            UNSCOPED_INFO(finding.rule << " " << finding.subject << ": " << finding.message);
        }
    }
    CHECK(report.passed());
}

TEST_CASE("world/lift: what is not a schema-1 room is refused", "[world][lift]") {
    const Oracle oracle = readOracle("plan-50-seed-102938.json");
    const rv::world::BuildingData& building = snesData().building;
    const rv::world::SystemsData& systems = snesData().systems;
    auto lifted = rv::world::lift(oracle.plan, building, systems);
    REQUIRE(lifted.has_value());
    // Already a building.
    CHECK(rv::world::lift(*lifted, building, systems).error().code == rv::ErrorCode::InvalidArgument);
    // No room, or two.
    WorldPlan empty = oracle.plan;
    empty.rooms.clear();
    CHECK(rv::world::lift(empty, building, systems).error().code == rv::ErrorCode::InvalidArgument);
    WorldPlan two = oracle.plan;
    two.rooms.push_back(two.rooms.front());
    two.rooms.back().id = *rv::world::RoomId::forSystem(*rv::catalog::SystemName::parse("nes"));
    CHECK(rv::world::lift(two, building, systems).error().code == rv::ErrorCode::InvalidArgument);
    // A system that gets no room.
    rv::world::SystemsData excluded = systems;
    for (rv::world::SystemEntry& entry : excluded.systems) {
        if (entry.system == rv::test::world::snes()) {
            entry.kind = rv::world::SystemKind::Excluded;
        }
    }
    auto refused = rv::world::lift(oracle.plan, building, excluded);
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error().message, ContainsSubstring("gets no room"));
}

TEST_CASE("world/lift: a lifted plan updated with nothing new is written once, then left as it is", "[world][lift]") {
    // The world's folder holds the frozen schema-1 plan; the first update writes it in schema 2 (a backup of the v1
    // file kept), the second finds nothing to write.
    const rv::test::TempDir dir("rv-world-lift-store");
    const rv::world::WorldFiles files = rv::world::WorldFiles::in(dir.file("world"));
    const Oracle oracle = readOracle("plan-50-seed-102938.json");
    REQUIRE(rv::core::writeTextFileAtomic(files.plan, oracle.text).has_value());
    auto lock = rv::world::WorldLock::acquire(files);
    REQUIRE(lock.has_value());

    auto stored = rv::world::readStoredPlan(files.plan);
    REQUIRE(stored.has_value());
    CHECK(stored->plan.schemaVersion == 1);
    CHECK(stored->digest == rv::core::sha256Hex(oracle.text));
    rv::world::UpdateInput input;
    input.library = rv::test::world::library();
    input.selection = makeSelection(50, kSelectionSeed);
    input.config.seed = 102938;
    input.previous = stored->plan;
    input.previousDigest = stored->digest;
    auto first = rv::world::update(input, snesData());
    REQUIRE(first.has_value());
    CHECK(first->changes.digestBefore == stored->digest);
    auto saved = rv::world::savePlan(*lock, *first);
    REQUIRE(saved.has_value());
    CHECK(saved->written);
    CHECK(saved->backup.has_value());

    stored = rv::world::readStoredPlan(files.plan);
    REQUIRE(stored.has_value());
    CHECK(stored->plan.schemaVersion == 2);
    CHECK(stored->digest == first->changes.digestAfter);
    input.previous = stored->plan;
    input.previousDigest = stored->digest;
    auto second = rv::world::update(input, snesData());
    REQUIRE(second.has_value());
    CHECK(second->changes.outcome == GenerationOutcome::Updated);
    CHECK(second->changes.rebuild == RebuildLevel::NoRebuild);
    CHECK(second->changes.digestAfter == second->changes.digestBefore);
    CHECK(second->plan == stored->plan);
    saved = rv::world::savePlan(*lock, *second);
    REQUIRE(saved.has_value());
    CHECK_FALSE(saved->written);
    // A schema-1 plan's digest is its file's: the update asks for it.
    input.previous = oracle.plan;
    input.previousDigest.clear();
    CHECK(rv::world::update(input, snesData()).error().code == rv::ErrorCode::InvalidArgument);
}
