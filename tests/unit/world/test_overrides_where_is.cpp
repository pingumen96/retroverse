// overrides.json (rv/world/overrides.hpp: pins read and checked against a plan, I3 step 9; schema 2 written, schema 1
// lifted, I4b step 1) and where-is (rv/world/where_is.hpp: the plan's index), I3 step 9.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/where_is.hpp>

#include "support/temp_dir.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::world::GamePin;
using rv::world::FixturePin;
using rv::world::Placement;
using rv::world::UnplacedGame;
using rv::world::UnplacedReason;
using rv::world::WorldOverrides;

namespace {

GameId game(std::string_view text) {
    return *GameId::parse(text);
}

rv::world::SlotId slot(std::string_view text) {
    return *rv::world::SlotId::parse(text);
}

rv::world::FixtureId fixture(std::string_view text) {
    return *rv::world::FixtureId::parse(text);
}

} // namespace

TEST_CASE("world/overrides: pins read from overrides.json", "[world][overrides]") {
    const auto read = rv::world::parseOverrides(R"({
        // A user's pins, written by hand.
        "schemaVersion": 1,
        "gamePins": [{"game": "g00000000000000b2", "slot": "room:snes/fx:w01/s:3.12"},
                     {"game": "g00000000000000a1", "slot": "room:snes/fx:dc01/s:0.0"}],
        "fixturePins": [{"fixture": "room:snes/fx:w05",
                         "prefab": {"manifest": "decor.poster.generic", "kind": "poster_frame"}},
                        {"fixture": "room:snes/fx:hero.crt"}]
    })",
                                                "overrides.json");
    REQUIRE(read.has_value());
    // Sorted by game and by fixture.
    CHECK(read->gamePins == std::vector<GamePin>{{game("g00000000000000a1"), slot("room:snes/fx:dc01/s:0.0")},
                                                 {game("g00000000000000b2"), slot("room:snes/fx:w01/s:3.12")}});
    REQUIRE(read->fixturePins.size() == 2);
    CHECK(read->fixturePins[0] == FixturePin{fixture("room:snes/fx:hero.crt"), std::nullopt});
    CHECK(read->fixturePins[1].prefab->manifest == "decor.poster.generic");
    CHECK(rv::world::parseOverrides(R"({"schemaVersion": 1})", "o")->empty());

    struct Case {
        std::string_view text;
        std::string_view message;
        std::size_t line;
    };
    const std::vector<Case> refused{
        {"{\"schemaVersion\": 1,\n \"pins\": []}", "/pins", 2},
        {"{\"schemaVersion\": 1,\n \"gamePins\": [{\"game\": \"g1\", \"slot\": \"room:snes/fx:w01/s:0.0\"}]}",
         "expected a GameId", 2},
        {"{\"schemaVersion\": 1,\n \"gamePins\": [{\"game\": \"g00000000000000a1\", \"slot\": \"w01/0\"}]}",
         "expected a slot id", 2},
        {"{\"schemaVersion\": 1, \"gamePins\": [\n{\"game\": \"g00000000000000a1\", \"slot\": "
         "\"room:snes/fx:w01/s:0.0\"},\n{\"game\": \"g00000000000000a1\", \"slot\": \"room:snes/fx:w01/s:0.1\"}]}",
         "pinned twice", 3},
        {"{\"schemaVersion\": 1, \"gamePins\": [\n{\"game\": \"g00000000000000a1\", \"slot\": "
         "\"room:snes/fx:w01/s:0.0\"},\n{\"game\": \"g00000000000000a2\", \"slot\": \"room:snes/fx:w01/s:0.0\"}]}",
         "holds two pins", 3},
        {"{\"schemaVersion\": 1, \"fixturePins\": [\n{\"fixture\": \"room:snes/fx:w05\"},\n{\"fixture\": "
         "\"room:snes/fx:w05\"}]}",
         "pinned twice", 3},
        {"{\"schemaVersion\": 1,\n \"fixturePins\": [{\"fixture\": \"room:snes/fx:w05\", \"prefab\": \"x\"}]}",
         "/fixturePins/0/prefab", 2},
        {"{\"gamePins\": []}", "schemaVersion", 1},
        {"{\"schemaVersion\": 1, \"schemaVersion\": 1}", "schemaVersion", 1},
    };
    for (const Case& test : refused) {
        INFO(test.text);
        const auto error = rv::world::parseOverrides(test.text, "overrides.json");
        REQUIRE_FALSE(error.has_value());
        CHECK_THAT(error.error().message, ContainsSubstring(std::string(test.message)));
        CHECK(error.error().context == "overrides.json:" + std::to_string(test.line));
    }
    // A newer schema: written by a newer Retroverse.
    const auto newer = rv::world::parseOverrides(R"({"schemaVersion": 3, "whatever": true})", "overrides.json");
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().code == rv::ErrorCode::Unsupported);

    // A missing file holds no pin; a broken one is an error.
    const rv::test::TempDir dir("rv-world-overrides");
    CHECK(rv::world::loadOverrides(dir.file("overrides.json"))->empty());
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("overrides.json"), "{").has_value());
    CHECK(rv::world::loadOverrides(dir.file("overrides.json")).error().code == rv::ErrorCode::ParseError);
}

TEST_CASE("world/overrides: pins measured against a plan", "[world][overrides]") {
    const rv::world::WorldPlan plan = rv::test::world::generateRoom(890, 4242, 102938).plan;
    const Placement& placed = plan.placements[10];
    const Placement& other = plan.placements[11];
    const UnplacedGame& unplaced = plan.unplaced.front();
    WorldOverrides overrides;
    overrides.gamePins = {
        {placed.game, placed.slot},                                       // honoured
        {other.game, placed.slot},                                        // not honoured: another slot
        {unplaced.game, slot("room:snes/fx:w01/s:6.32")},                 // not honoured: unplaced
        {game("g0000000000000123"), slot("room:snes/fx:w01/s:0.0")},      // orphaned: no such game
        {plan.placements[12].game, slot("room:snes/fx:w01/s:6.33")},      // orphaned: past the level
    };
    const rv::world::Fixture& poster = *std::find_if(plan.fixtures.begin(), plan.fixtures.end(), [](const auto& f) {
        return f.role == rv::world::FixtureRole::Decoration;
    });
    overrides.fixturePins = {
        {poster.id, poster.prefab},                                       // honoured
        {fixture("room:snes/fx:hero.crt"), rv::world::PrefabRef{"other.prefab.generic", "crt"}}, // another prefab
        {fixture("room:snes/fx:e13"), std::nullopt},                      // a socket of a growth step: not honoured
        {fixture("room:snes/fx:nowhere"), std::nullopt},                  // orphaned: no such socket
        {fixture("room:nes/fx:w01"), std::nullopt},                       // orphaned: another room
    };
    const rv::world::PinCheck check =
        rv::world::checkPins(plan, {rv::test::world::snesData().roomTemplate}, overrides);
    CHECK(check.orphaned.size() == 4);
    CHECK(check.unhonoured.size() == 4);
    CHECK(check.honouredOrOrphaned.gamePins.size() == 3);
    CHECK(check.honouredOrOrphaned.fixturePins.size() == 3);
    CHECK(std::is_sorted(check.orphaned.begin(), check.orphaned.end()));
    CHECK(rv::world::pinText(overrides.gamePins[0]) ==
          "game " + placed.game.toString() + " pinned to " + placed.slot.toString());
    CHECK(rv::world::pinText(overrides.fixturePins[3]) == "fixture room:snes/fx:nowhere pinned");
    // The validation agrees: the honoured and orphaned pins break no hard rule, the others are pin-broken.
    rv::world::ValidationContext context;
    context.templates = {rv::test::world::snesData().roomTemplate};
    context.gamePins = check.honouredOrOrphaned.gamePins;
    context.fixturePins = check.honouredOrOrphaned.fixturePins;
    CHECK(rv::world::validatePlan(plan, context).passed());
    context.gamePins = overrides.gamePins;
    context.fixturePins = overrides.fixturePins;
    const rv::world::ValidationReport report = rv::world::validatePlan(plan, context);
    std::size_t broken = 0;
    for (const rv::world::ValidationFinding& finding : report.findings) {
        broken += finding.rule == rv::world::rule::kPinBroken ? 1 : 0;
    }
    CHECK(broken == 4);
}

TEST_CASE("world/overrides: schema 2 written and read back, schema 1 lifted", "[world][overrides]") {
    using rv::world::FixtureTransform;
    using rv::world::PinOrigin;
    using rv::world::PinProvenance;
    WorldOverrides overrides;
    overrides.gamePins = {
        {game("g00000000000000b2"), slot("room:snes/fx:w01/s:3.12"), {PinOrigin::Editor, "2026-10-27T10:00:00.000Z"}},
        {game("g00000000000000a1"), slot("room:snes/fx:dc01/s:0.0"), {PinOrigin::File, ""}},
    };
    overrides.fixturePins = {
        {fixture("room:snes/fx:w05"), rv::world::PrefabRef{"decor.poster.generic", "poster_frame"},
         FixtureTransform{1200, -300, 9000}, {PinOrigin::Cli, "2026-10-27T11:30:00.250Z"}},
        {fixture("room:snes/fx:hero.crt"), std::nullopt, std::nullopt, {}},
    };
    const auto text = rv::world::writeOverrides(overrides);
    REQUIRE(text.has_value());
    // The canonical text: keys sorted, pins sorted by game and by fixture, a time, a prefab and a transform only when
    // set.
    CHECK(*text == R"({
  "fixturePins": [
    {
      "fixture": "room:snes/fx:hero.crt",
      "provenance": {
        "origin": "file"
      }
    },
    {
      "fixture": "room:snes/fx:w05",
      "prefab": {
        "kind": "poster_frame",
        "manifest": "decor.poster.generic"
      },
      "provenance": {
        "origin": "cli",
        "time": "2026-10-27T11:30:00.250Z"
      },
      "transform": {
        "xMm": 1200,
        "yawCdeg": 9000,
        "zMm": -300
      }
    }
  ],
  "gamePins": [
    {
      "game": "g00000000000000a1",
      "provenance": {
        "origin": "file"
      },
      "slot": "room:snes/fx:dc01/s:0.0"
    },
    {
      "game": "g00000000000000b2",
      "provenance": {
        "origin": "editor",
        "time": "2026-10-27T10:00:00.000Z"
      },
      "slot": "room:snes/fx:w01/s:3.12"
    }
  ],
  "schemaVersion": 2
}
)");
    // Round trips: the overrides back (sorted), and their text again.
    const auto read = rv::world::parseOverrides(*text, "overrides.json");
    REQUIRE(read.has_value());
    WorldOverrides sorted = overrides;
    std::swap(sorted.gamePins[0], sorted.gamePins[1]);
    std::swap(sorted.fixturePins[0], sorted.fixturePins[1]);
    CHECK(*read == sorted);
    CHECK(rv::world::writeOverrides(*read) == *text);
    CHECK(rv::world::writeOverrides(WorldOverrides{}) ==
          std::string("{\n  \"fixturePins\": [],\n  \"gamePins\": [],\n  \"schemaVersion\": 2\n}\n"));

    // Schema 1 lifted: every pin of origin file, no time, no transform; written again as schema 2.
    const auto lifted = rv::world::parseOverrides(R"({"schemaVersion": 1,
        "gamePins": [{"game": "g00000000000000a1", "slot": "room:snes/fx:dc01/s:0.0"}],
        "fixturePins": [{"fixture": "room:snes/fx:w05",
                         "prefab": {"manifest": "decor.poster.generic", "kind": "poster_frame"}}]})",
                                                  "overrides.json");
    REQUIRE(lifted.has_value());
    REQUIRE(lifted->gamePins.size() == 1);
    CHECK(lifted->gamePins[0].provenance == PinProvenance{PinOrigin::File, ""});
    REQUIRE(lifted->fixturePins.size() == 1);
    CHECK_FALSE(lifted->fixturePins[0].transform.has_value());
    CHECK(lifted->fixturePins[0].provenance == PinProvenance{});
    const auto liftedText = rv::world::writeOverrides(*lifted);
    REQUIRE(liftedText.has_value());
    CHECK(liftedText->find("\"schemaVersion\": 2") != std::string::npos);
    CHECK(rv::world::parseOverrides(*liftedText, "o") == *lifted);
    // A hand-written schema-2 pin without provenance: origin file.
    const auto hand = rv::world::parseOverrides(
        R"({"schemaVersion": 2, "gamePins": [{"game": "g00000000000000a1", "slot": "room:snes/fx:dc01/s:0.0"}]})", "o");
    REQUIRE(hand.has_value());
    CHECK(hand->gamePins[0].provenance == PinProvenance{});

    struct Case {
        std::string_view text;
        std::string_view message;
    };
    const std::vector<Case> refused{
        // Schema 2's keys in a schema-1 file.
        {R"({"schemaVersion": 1, "gamePins": [{"game": "g00000000000000a1", "slot": "room:snes/fx:w01/s:0.0",
             "provenance": {"origin": "cli"}}]})",
         "/gamePins/0/provenance"},
        {R"({"schemaVersion": 1, "fixturePins": [{"fixture": "room:snes/fx:w05",
             "transform": {"xMm": 0, "zMm": 0, "yawCdeg": 0}}]})",
         "/fixturePins/0/transform"},
        {R"({"schemaVersion": 2, "gamePins": [{"game": "g00000000000000a1", "slot": "room:snes/fx:w01/s:0.0",
             "provenance": {"origin": "someone"}}]})",
         "one of editor, cli, file"},
        {R"({"schemaVersion": 2, "gamePins": [{"game": "g00000000000000a1", "slot": "room:snes/fx:w01/s:0.0",
             "provenance": {"origin": "cli", "time": "yesterday"}}]})",
         "a UTC time"},
        {R"({"schemaVersion": 2, "fixturePins": [{"fixture": "room:snes/fx:w05",
             "transform": {"xMm": 0, "zMm": 0, "yawCdeg": 36000}}]})",
         "/fixturePins/0/transform/yawCdeg"},
        {R"({"schemaVersion": 2, "fixturePins": [{"fixture": "room:snes/fx:w05",
             "transform": {"xMm": 0, "zMm": 0}}]})",
         "yawCdeg"},
        {R"({"schemaVersion": 0})", "schemaVersion"},
    };
    for (const Case& test : refused) {
        INFO(test.text);
        const auto error = rv::world::parseOverrides(test.text, "overrides.json");
        REQUIRE_FALSE(error.has_value());
        CHECK(error.error().code == rv::ErrorCode::ValidationFailed);
        CHECK_THAT(error.error().message, ContainsSubstring(std::string(test.message)));
    }
    // Overrides the reader would refuse are never written.
    WorldOverrides twice;
    twice.gamePins = {{game("g00000000000000a1"), slot("room:snes/fx:w01/s:0.0")},
                      {game("g00000000000000a1"), slot("room:snes/fx:w01/s:0.1")}};
    CHECK(rv::world::writeOverrides(twice).error().code == rv::ErrorCode::InvalidArgument);
    WorldOverrides badTime;
    badTime.gamePins = {{game("g00000000000000a1"), slot("room:snes/fx:w01/s:0.0"), {PinOrigin::Cli, "now"}}};
    CHECK(rv::world::writeOverrides(badTime).error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("world/overrides: a fixture pinned at a transform is honoured where it stands", "[world][overrides]") {
    const rv::world::WorldPlan plan = rv::test::world::generateRoom(890, 4242, 102938).plan;
    const rv::world::Fixture& poster = *std::find_if(plan.fixtures.begin(), plan.fixtures.end(), [](const auto& f) {
        return f.role == rv::world::FixtureRole::Decoration;
    });
    WorldOverrides overrides;
    overrides.fixturePins = {
        {poster.id, std::nullopt, rv::world::FixtureTransform{poster.xMm, poster.zMm, poster.yawCdeg}}};
    const std::vector<rv::world::RoomTemplate> templates{rv::test::world::snesData().roomTemplate};
    rv::world::PinCheck check = rv::world::checkPins(plan, templates, overrides);
    CHECK(check.unhonoured.empty());
    CHECK(check.honouredOrOrphaned.fixturePins.size() == 1);
    rv::world::ValidationContext context;
    context.templates = templates;
    context.fixturePins = overrides.fixturePins;
    CHECK(rv::world::validatePlan(plan, context).passed());

    // Elsewhere: not honoured (the update never moves it there), and the validation says so.
    overrides.fixturePins[0].transform->xMm += 100;
    check = rv::world::checkPins(plan, templates, overrides);
    REQUIRE(check.unhonoured.size() == 1);
    CHECK_THAT(check.unhonoured[0], ContainsSubstring(" pinned at x " + std::to_string(poster.xMm + 100) + " z "));
    context.fixturePins = overrides.fixturePins;
    const rv::world::ValidationReport report = rv::world::validatePlan(plan, context);
    CHECK(std::any_of(report.findings.begin(), report.findings.end(), [](const rv::world::ValidationFinding& finding) {
        return finding.rule == rv::world::rule::kPinBroken;
    }));
}

TEST_CASE("world/where-is: the plan's index answers placed and unplaced games", "[world][where_is]") {
    const rv::world::WorldPlan plan = rv::test::world::generateRoom(890, 4242, 102938).plan;
    const rv::world::PlanIndex index(plan);
    CHECK(index.placedCount() == 890);
    CHECK(index.unplacedCount() == plan.unplaced.size());
    const auto sections = rv::test::world::levelSections(plan);
    for (const Placement& placement : plan.placements) {
        const auto location = index.find(placement.game);
        REQUIRE(location.has_value());
        REQUIRE(location->placed());
        CHECK(location->place->slot == placement.slot);
        CHECK(location->room == plan.rooms.front().id);
        CHECK(location->place->section == sections.at({placement.slot.fixture(), placement.slot.level()}));
        CHECK(index.gameAt(placement.slot) == placement.game);
    }
    bool sawDuplicate = false;
    for (const UnplacedGame& unplaced : plan.unplaced) {
        const auto location = index.find(unplaced.game);
        REQUIRE(location.has_value());
        CHECK_FALSE(location->placed());
        CHECK(location->reason == unplaced.reason);
        if (unplaced.reason == UnplacedReason::DuplicateTitle) {
            sawDuplicate = true;
            CHECK(location->representative == unplaced.representative);
            REQUIRE(location->representativePlace.has_value());
            CHECK(location->representativePlace->slot == index.find(unplaced.representative)->place->slot);
        }
    }
    CHECK(sawDuplicate);
    CHECK_FALSE(index.find(game("g0000000000000001")).has_value());
    CHECK_FALSE(index.gameAt(slot("room:snes/fx:w01/s:6.32")).has_value());
    // The face-out ones on their fixtures: a display case's slot is featured.
    const auto featured = std::find_if(plan.placements.begin(), plan.placements.end(), [](const Placement& p) {
        return p.slot.fixture().socket() == "dc01";
    });
    REQUIRE(featured != plan.placements.end());
    const auto location = index.find(featured->game);
    CHECK(location->place->role == rv::world::FixtureRole::Display);
    CHECK(location->place->section == rv::world::Section::Featured);
    CHECK(location->place->prefab.kind == "display_case");
    // An empty plan answers nothing.
    const rv::world::PlanIndex empty{rv::world::WorldPlan{}};
    CHECK_FALSE(empty.find(featured->game).has_value());
}
