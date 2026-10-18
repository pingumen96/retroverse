// The WorldPlan's validation (rv/world/validation.hpp, ADR-0010 §11): a hand-built plan of the shipped template that
// passes every rule, and for every rule a plan changed from it that breaks that rule and no other hard one; the
// findings' order, the LayoutScore and the prefab slots read from the asset root's manifests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/text.hpp>
#include <rv/world/validation.hpp>

#include "world_test_plans.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::test::fixtureOf;
using rv::test::snesFixtureId;
using rv::test::snesRoomId;
using rv::test::snesSlotId;
using rv::test::testGameId;
using rv::world::Fixture;
using rv::world::FixtureRole;
using rv::world::Section;
using rv::world::Severity;
using rv::world::ValidationContext;
using rv::world::ValidationFinding;
using rv::world::ValidationReport;
using rv::world::WorldPlan;
namespace rule = rv::world::rule;

namespace {

std::set<std::string> rulesOf(const ValidationReport& report, Severity severity) {
    std::set<std::string> rules;
    for (const ValidationFinding& finding : report.findings) {
        if (finding.severity == severity) {
            rules.insert(finding.rule);
        }
    }
    return rules;
}

std::set<std::string> hardRules(const ValidationReport& report) {
    return rulesOf(report, Severity::Hard);
}

std::set<std::string> softRules(const ValidationReport& report) {
    return rulesOf(report, Severity::Soft);
}

std::vector<ValidationFinding> findingsOf(const ValidationReport& report, std::string_view name) {
    std::vector<ValidationFinding> findings;
    std::copy_if(report.findings.begin(), report.findings.end(), std::back_inserter(findings),
                 [&](const ValidationFinding& finding) { return finding.rule == name; });
    return findings;
}

std::string describe(const ValidationReport& report) {
    std::string text;
    for (const ValidationFinding& finding : report.findings) {
        text += finding.rule + " " + finding.subject + ": " + finding.message + "\n";
    }
    return text;
}

/// The plan validated with the context that validates the unchanged plan.
ValidationReport validate(const WorldPlan& plan) {
    return rv::world::validatePlan(plan, rv::test::validContextFor(rv::test::validRoomPlan()));
}

/// The plan breaks `name` and no other hard rule; returns the rule's findings.
std::vector<ValidationFinding> requireOnlyHard(const ValidationReport& report, std::string_view name) {
    INFO(describe(report));
    CHECK_FALSE(report.passed());
    CHECK(hardRules(report) == std::set<std::string>{std::string(name)});
    return findingsOf(report, name);
}

} // namespace

TEST_CASE("world/validation: the rules are named once each, hard ones first, every name a rule name",
          "[world][validation]") {
    const auto rules = rv::world::validationRules();
    REQUIRE(rules.size() == 29);
    std::set<std::string_view> names;
    bool soft = false;
    for (const rv::world::ValidationRule& entry : rules) {
        INFO(entry.name);
        CHECK(names.insert(entry.name).second);
        CHECK_FALSE(entry.summary.empty());
        CHECK(rv::world::findValidationRule(entry.name) == &entry);
        // Lowercase words joined by '-'.
        CHECK(std::all_of(entry.name.begin(), entry.name.end(),
                          [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; }));
        if (entry.severity == Severity::Soft) {
            soft = true;
        } else {
            CHECK_FALSE(soft);
        }
    }
    CHECK(rv::world::findValidationRule("no-such-rule") == nullptr);
    CHECK(rv::world::findValidationRule(rule::kUnreachable)->severity == Severity::Hard);
    CHECK(rv::world::findValidationRule(rule::kTvLineOfSight)->severity == Severity::Soft);
}

TEST_CASE("world/validation: the hand-built plan of console_room_9x10 passes every rule, the soft ones included",
          "[world][validation]") {
    const WorldPlan plan = rv::test::validRoomPlan();
    const ValidationReport report = validate(plan);
    INFO(describe(report));
    CHECK(report.passed());
    CHECK(report.findings.empty());
    // Style §13.5's weights: every term full but the decoration's variation (3 prefabs among 10 decorations: 300).
    REQUIRE(report.layoutScorePermille.has_value());
    CHECK(*report.layoutScorePermille == 951);

    // The order of the lists in memory does not matter, and the report is the same on every run.
    WorldPlan reversed = plan;
    std::reverse(reversed.fixtures.begin(), reversed.fixtures.end());
    std::reverse(reversed.placements.begin(), reversed.placements.end());
    std::reverse(reversed.dressing.begin(), reversed.dressing.end());
    CHECK(validate(reversed) == report);

    // Without the optional inputs (no selection, prefab slots, lights or pins) nothing more is found.
    ValidationContext bare;
    bare.templates.push_back(rv::test::shippedRoomTemplate());
    bare.config = rv::test::validContextFor(plan).config;
    CHECK(rv::world::validatePlan(plan, bare).findings.empty());

    // The 16 m room with a module in each of its 26 side-wall sockets passes too.
    const ValidationReport grown =
        rv::world::validatePlan(rv::test::grownRoomPlan(3), rv::test::validContextFor(rv::test::grownRoomPlan(3)));
    INFO(describe(grown));
    CHECK(grown.passed());
    CHECK(hardRules(grown).empty());
}

TEST_CASE("world/validation: rooms, templates and sizes", "[world][validation]") {
    SECTION("room-unknown: a fixture or an unplaced entry of a room the plan does not list") {
        WorldPlan plan = rv::test::validRoomPlan();
        const auto nes = *rv::world::RoomId::forSystem(*rv::catalog::SystemName::parse("nes"));
        Fixture stray = fixtureOf(plan, "w07");
        stray.id = *rv::world::FixtureId::make(nes, "w07");
        plan.fixtures.push_back(stray);
        plan.unplaced.push_back({testGameId(103), nes, rv::world::UnplacedReason::NoCover, {}});
        const auto findings = requireOnlyHard(validate(plan), rule::kRoomUnknown);
        REQUIRE(findings.size() == 2);
        CHECK(findings[0].subject == "g0000000000000067");
        CHECK(findings[1].subject == "room:nes/fx:w07");
        CHECK_THAT(findings[1].message, ContainsSubstring("room:nes, which the plan does not list"));
    }
    SECTION("room-template: an unknown template, or another version of it") {
        WorldPlan plan = rv::test::validRoomPlan();
        plan.rooms[0].templateId = "lounge_room_8x8";
        auto findings = requireOnlyHard(validate(plan), rule::kRoomTemplate);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes");
        CHECK_THAT(findings[0].message, ContainsSubstring("'lounge_room_8x8' is not known"));

        plan = rv::test::validRoomPlan();
        plan.rooms[0].templateVersion = 2;
        findings = requireOnlyHard(validate(plan), rule::kRoomTemplate);
        REQUIRE(findings.size() == 1);
        CHECK_THAT(findings[0].message, ContainsSubstring("version 2 of the template 'console_room_9x10', which is now "
                                                          "version 1"));
    }
    SECTION("room-size: bounds or height other than the template's, or more growth than it allows") {
        WorldPlan plan = rv::test::validRoomPlan();
        plan.rooms[0].maxZMm = 5500;
        auto findings = requireOnlyHard(validate(plan), rule::kRoomSize);
        REQUIRE(findings.size() == 1);
        CHECK_THAT(findings[0].message, ContainsSubstring("x -4500 to 4500, z -5000 to 5500"));

        plan = rv::test::validRoomPlan();
        plan.rooms[0].heightMm = 3300;
        requireOnlyHard(validate(plan), rule::kRoomSize);

        plan = rv::test::validRoomPlan();
        plan.rooms[0].growthSteps = 4;
        plan.rooms[0].maxZMm = 13'000;
        findings = requireOnlyHard(validate(plan), rule::kRoomSize);
        CHECK_THAT(findings[0].message, ContainsSubstring("took 4 growth steps; its template allows 3"));
    }
}

TEST_CASE("world/validation: sockets, levels and prefab slots", "[world][validation]") {
    SECTION("fixture-socket: a socket the template does not have, or one that does not take the role") {
        WorldPlan plan = rv::test::validRoomPlan();
        fixtureOf(plan, "w04").id = snesFixtureId("w99");
        auto report = validate(plan);
        auto findings = requireOnlyHard(report, rule::kFixtureSocket);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:w99");
        CHECK_THAT(findings[0].message, ContainsSubstring("has no socket 'w99'"));
        // w04 is now bare.
        CHECK(softRules(report) == std::set<std::string>{std::string(rule::kSocketBare)});

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "dec-sw").role = FixtureRole::Display;
        report = validate(plan);
        findings = requireOnlyHard(report, rule::kFixtureSocket);
        CHECK_THAT(findings[0].message, ContainsSubstring("a display fixture cannot stand in the decoration socket"));

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "hero", "armchair").id = snesFixtureId("hero", "sofa");
        findings = requireOnlyHard(validate(plan), rule::kFixtureSocket);
        CHECK_THAT(findings[0].message, ContainsSubstring("the hero corner 'hero' has no part 'sofa'"));

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "w02").id = snesFixtureId("w02", "left");
        findings = requireOnlyHard(validate(plan), rule::kFixtureSocket);
        CHECK_THAT(findings[0].message, ContainsSubstring("names no part"));

        // A socket of a growth step the room has not taken (it also stands outside the room).
        plan = rv::test::validRoomPlan();
        Fixture grown = fixtureOf(plan, "w07");
        grown.id = snesFixtureId("w08");
        grown.zMm += 1000;
        plan.fixtures.push_back(grown);
        report = validate(plan);
        INFO(describe(report));
        CHECK(hardRules(report) ==
              std::set<std::string>{std::string(rule::kFixtureSocket), std::string(rule::kFixtureOutsideRoom)});
        CHECK_THAT(findingsOf(report, rule::kFixtureSocket)[0].message,
                   ContainsSubstring("comes with growth step 1; the room took 0"));
    }
    SECTION("fixture-levels: sections a role does not hold, a level twice or empty, levels on a decoration") {
        WorldPlan plan = rv::test::validRoomPlan();
        fixtureOf(plan, "w01").levels[3].section = Section::Band;
        auto findings = requireOnlyHard(validate(plan), rule::kFixtureLevels);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:w01");
        CHECK_THAT(findings[0].message, ContainsSubstring("level 3 is a band level, which a side-module fixture does"));

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "dc01").levels[0].section = Section::Spine;
        requireOnlyHard(validate(plan), rule::kFixtureLevels);

        // A hero-wall module holds band and spine levels, never featured ones.
        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "hw-e").levels[0].section = Section::Featured;
        requireOnlyHard(validate(plan), rule::kFixtureLevels);

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "w04").levels = {{0, 4, Section::Spine}};
        findings = requireOnlyHard(validate(plan), rule::kFixtureLevels);
        CHECK_THAT(findings[0].message, ContainsSubstring("a decoration fixture holds no slots"));

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "e02").levels[6].slots = 0;
        ValidationContext context = rv::test::validContextFor(rv::test::validRoomPlan());
        context.prefabSlots = nullptr; // the prefab's 33 slots on that level would differ too
        findings = requireOnlyHard(rv::world::validatePlan(plan, context), rule::kFixtureLevels);
        CHECK_THAT(findings[0].message, ContainsSubstring("level 6 has no slots"));
    }
    SECTION("prefab-slots and prefab-unresolved: the levels against the prefab's slots") {
        const WorldPlan plan = rv::test::validRoomPlan();
        ValidationContext context = rv::test::validContextFor(plan);
        WorldPlan changed = plan;
        fixtureOf(changed, "e03").levels[2].slots = 32;
        auto findings = requireOnlyHard(rv::world::validatePlan(changed, context), rule::kPrefabSlots);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:e03");
        CHECK_THAT(findings[0].message, ContainsSubstring("2: 32"));
        CHECK_THAT(findings[0].message, ContainsSubstring("the prefab furniture.shelf.side_module.generic has"));

        changed = plan;
        fixtureOf(changed, "e03").levels.pop_back();
        requireOnlyHard(rv::world::validatePlan(changed, context), rule::kPrefabSlots);

        // A prefab whose slots cannot be read is reported, soft, for every fixture of it, and the plan is kept.
        context.prefabSlots =
            [](const rv::world::PrefabRef& prefab) -> rv::Expected<std::vector<rv::world::PrefabSlotLevel>> {
            return rv::makeError(rv::ErrorCode::NotFound, "missing manifest", prefab.manifest);
        };
        const ValidationReport report = rv::world::validatePlan(plan, context);
        INFO(describe(report));
        CHECK(report.passed());
        CHECK(softRules(report) == std::set<std::string>{std::string(rule::kPrefabUnresolved)});
        // The collection fixtures only: 2 hero-wall modules, 6 side-wall modules, 3 displays.
        CHECK(findingsOf(report, rule::kPrefabUnresolved).size() == 11);
    }
}

TEST_CASE("world/validation: footprints, door-reserved sockets and the doorway", "[world][validation]") {
    SECTION("fixture-outside-room: through a wall or the ceiling") {
        WorldPlan plan = rv::test::validRoomPlan();
        fixtureOf(plan, "w01").xMm -= 100;
        auto findings = requireOnlyHard(validate(plan), rule::kFixtureOutsideRoom);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:w01");
        CHECK_THAT(findings[0].message, ContainsSubstring("x -4570 to -4220"));

        plan = rv::test::validRoomPlan();
        // The sign hangs from the middle of its back: 700 mm high, centred 2900 mm above the floor.
        fixtureOf(plan, "hero", "sign").yMm = 2900;
        findings = requireOnlyHard(validate(plan), rule::kFixtureOutsideRoom);
        CHECK_THAT(findings[0].message, ContainsSubstring("from y 2550 to 3250"));
        fixtureOf(plan, "hero", "sign").yMm = 2850;
        CHECK(validate(plan).passed());
    }
    SECTION("door-reserved: a shelf in a door-reserved socket, or a fixture entering one") {
        const rv::world::RoomTemplate roomTemplate = rv::test::shippedRoomTemplate();
        WorldPlan plan = rv::test::validRoomPlan();
        Fixture shelf = fixtureOf(plan, "w01");
        shelf.id = snesFixtureId("door-w");
        shelf.zMm = roomTemplate.socket("door-w")->zMm;
        plan.fixtures.push_back(shelf);
        ValidationContext context = rv::test::validContextFor(plan);
        auto findings = requireOnlyHard(rv::world::validatePlan(plan, context), rule::kDoorReserved);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:door-w");
        CHECK_THAT(findings[0].message, ContainsSubstring("stands in the door-reserved socket 'door-w'"));

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "w05").zMm = 1300;
        findings = requireOnlyHard(validate(plan), rule::kDoorReserved);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:w05");
        CHECK_THAT(findings[0].message, ContainsSubstring("entering the door-reserved socket 'door-w' (x -4470 to "
                                                          "-4070, z -400 to 1400)"));
    }
    SECTION("doorway-clearance and doorway-blocked: a fixture in the doorway's clear area") {
        WorldPlan plan = rv::test::validRoomPlan();
        fixtureOf(plan, "dec-se").xMm = 0;
        const ValidationReport report = validate(plan);
        INFO(describe(report));
        CHECK(hardRules(report) ==
              std::set<std::string>{std::string(rule::kDoorwayClearance), std::string(rule::kDoorwayBlocked)});
        CHECK_THAT(findingsOf(report, rule::kDoorwayClearance)[0].message,
                   ContainsSubstring("entering the doorway's clear area (x -800 to 800, z 3500 to 5000)"));
        CHECK(findingsOf(report, rule::kDoorwayBlocked)[0].subject == "room:snes");
        // A blocked doorway reaches nothing; the targets are not listed one by one.
        CHECK(findingsOf(report, rule::kUnreachable).empty());

        // A rug is walked over, but it may not lie in the doorway's clear area either.
        plan = rv::test::validRoomPlan();
        Fixture& rug = fixtureOf(plan, "hero", "rug");
        rug.xMm = 0;
        rug.zMm = 4200;
        const auto findings = requireOnlyHard(validate(plan), rule::kDoorwayClearance);
        CHECK(findings[0].subject == "room:snes/fx:hero.rug");
    }
    SECTION("doorway-blocked alone: the clear area leaves no room for the clear width") {
        // A 1.6 m minimum clear width (and main passage) and a lamp 790 mm from the clear area's centre, outside it.
        WorldPlan plan = rv::test::validRoomPlan();
        Fixture& lamp = fixtureOf(plan, "w05");
        lamp.xMm = 0;
        lamp.zMm = 3240;
        lamp.id = snesFixtureId("dec-se", "lamp");
        ValidationContext context = rv::test::validContextFor(rv::test::validRoomPlan());
        context.config.minClearWidthMm = 1600;
        context.config.mainPassageWidthMm = 1600;
        const ValidationReport report = rv::world::validatePlan(plan, context);
        INFO(describe(report));
        CHECK(hardRules(report) == std::set<std::string>{std::string(rule::kDoorwayBlocked)});
        CHECK_THAT(findingsOf(report, rule::kDoorwayBlocked)[0].message,
                   ContainsSubstring("no body 1600 mm wide can stand at the doorway's clear area's centre (x 0, z "
                                     "4250)"));
    }
    SECTION("footprints-overlap: two fixtures' boxes, heights included; touching boxes and rugs do not overlap") {
        WorldPlan plan = rv::test::validRoomPlan();
        fixtureOf(plan, "hero", "console").yMm = 500;
        auto findings = requireOnlyHard(validate(plan), rule::kFootprintsOverlap);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:hero.console");
        CHECK_THAT(findings[0].message, ContainsSubstring("overlaps room:snes/fx:hero.tv-cabinet"));

        plan = rv::test::validRoomPlan();
        fixtureOf(plan, "dc02").xMm = -900;
        const ValidationReport report = validate(plan);
        INFO(describe(report));
        CHECK(hardRules(report).contains(std::string(rule::kFootprintsOverlap)));
        CHECK(findingsOf(report, rule::kFootprintsOverlap)[0].subject == "room:snes/fx:dc01");
        // Overlaps cost the LayoutScore.
        REQUIRE(report.layoutScorePermille.has_value());
        CHECK(*report.layoutScorePermille < 951);
    }
    SECTION("a turned fixture's box: the armchair at 210 degrees, moved into the TV cabinet") {
        WorldPlan plan = rv::test::validRoomPlan();
        Fixture& armchair = fixtureOf(plan, "hero", "armchair");
        // Its box reaches 559 mm either side in x and 568.5 mm in z (800 x 850 mm turned 30 degrees past a half turn:
        // 800 sin 30 + 850 cos 30 = 1136.1 mm across, rounded up to 1137).
        armchair.zMm = -4470 + 568;
        CHECK(findingsOf(validate(plan), rule::kFootprintsOverlap).size() == 1);
        armchair.zMm = -4470 + 569;
        CHECK(findingsOf(validate(plan), rule::kFootprintsOverlap).empty());
    }
}

TEST_CASE("world/validation: lights", "[world][validation]") {
    const WorldPlan plan = rv::test::validRoomPlan();
    ValidationContext context = rv::test::validContextFor(plan);
    context.lights = {{snesRoomId(), 16}};
    CHECK(rv::world::validatePlan(plan, context).passed());
    context.lights = {{snesRoomId(), 17}};
    const auto findings = requireOnlyHard(rv::world::validatePlan(plan, context), rule::kLightsOverLimit);
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].subject == "room:snes");
    CHECK(findings[0].message == "the room holds 17 point and spot lights; at most 16 are allowed");
}

TEST_CASE("world/validation: slots and dressing", "[world][validation]") {
    SECTION("slot-unknown: a fixture the plan does not hold, a level it does not list, an index past the level") {
        WorldPlan plan = rv::test::validRoomPlan();
        plan.placements[0].slot = snesSlotId("w09", 0, 0);
        plan.placements[1].slot = snesSlotId("w01", 9, 0);
        plan.placements[2].slot = snesSlotId("w01", 0, 33);
        const auto findings = requireOnlyHard(validate(plan), rule::kSlotUnknown);
        REQUIRE(findings.size() == 3);
        CHECK(findings[0].subject == "room:snes/fx:w01/s:0.33");
        CHECK_THAT(findings[0].message, ContainsSubstring("slot 33 of level 0, which has 33 slots"));
        CHECK(findings[1].subject == "room:snes/fx:w01/s:9.0");
        CHECK_THAT(findings[1].message, ContainsSubstring("level 9, which room:snes/fx:w01 does not list"));
        CHECK(findings[2].subject == "room:snes/fx:w09/s:0.0");
        CHECK_THAT(findings[2].message, ContainsSubstring("which the plan does not hold"));
    }
    SECTION("slot-shared: two games in a slot") {
        WorldPlan plan = rv::test::validRoomPlan();
        plan.placements[1].slot = plan.placements[0].slot;
        const auto findings = requireOnlyHard(validate(plan), rule::kSlotShared);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:hw-w/s:0.0");
        CHECK(findings[0].message == "the slot holds 2 games: g0000000000000001, g0000000000000002");
    }
    SECTION("dressing-unknown, dressing-overlap and slot-dressed") {
        WorldPlan plan = rv::test::validRoomPlan();
        plan.dressing.push_back({*rv::world::LevelId::make(snesFixtureId("w09"), 0), 0, 1, plan.dressing[0].prop});
        plan.dressing.push_back({*rv::world::LevelId::make(snesFixtureId("w02"), 9), 0, 1, plan.dressing[0].prop});
        plan.dressing.push_back({*rv::world::LevelId::make(snesFixtureId("w02"), 6), 30, 4, plan.dressing[0].prop});
        plan.dressing.push_back({*rv::world::LevelId::make(snesFixtureId("w02"), 5), 3, 0, plan.dressing[0].prop});
        auto findings = requireOnlyHard(validate(plan), rule::kDressingUnknown);
        REQUIRE(findings.size() == 4);
        CHECK_THAT(findings[0].message, ContainsSubstring("covers 0 slots from slot 3; the level has 33 slots"));
        CHECK_THAT(findings[1].message, ContainsSubstring("covers 4 slots from slot 30; the level has 33 slots"));
        CHECK_THAT(findings[2].message, ContainsSubstring("does not list level 9"));
        CHECK_THAT(findings[3].message, ContainsSubstring("fixture room:snes/fx:w09 is not in the plan"));

        plan = rv::test::validRoomPlan();
        plan.dressing.push_back({*rv::world::LevelId::make(snesFixtureId("hw-w"), 0), 30, 2, plan.dressing[0].prop});
        findings = requireOnlyHard(validate(plan), rule::kDressingOverlap);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:hw-w/l:0");
        CHECK(findings[0].message == "the span of 13 slots from slot 20 overlaps the span from slot 30");

        plan = rv::test::validRoomPlan();
        plan.placements[9].slot = snesSlotId("hw-w", 0, 20);
        findings = requireOnlyHard(validate(plan), rule::kSlotDressed);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:hw-w/s:0.20");
        CHECK(findings[0].message == "g000000000000000a stands in the dressed span of 13 slots from slot 20");
        // The span's last slot is dressed, the next one free.
        plan.placements[9].slot = snesSlotId("hw-w", 0, 32);
        CHECK(findingsOf(validate(plan), rule::kSlotDressed).size() == 1);
        plan.placements[9].slot = snesSlotId("hw-w", 0, 19);
        CHECK(validate(plan).passed());
    }
}

TEST_CASE("world/validation: one entry per game", "[world][validation]") {
    SECTION("game-twice: placed twice, or placed and unplaced") {
        WorldPlan plan = rv::test::validRoomPlan();
        plan.placements.push_back({snesSlotId("w01", 0, 10), testGameId(5), 0, 0});
        plan.unplaced.push_back({testGameId(6), snesRoomId(), rv::world::UnplacedReason::Capacity, {}});
        const auto findings = requireOnlyHard(validate(plan), rule::kGameTwice);
        REQUIRE(findings.size() == 2);
        CHECK(findings[0].subject == "g0000000000000005");
        CHECK(findings[0].message == "the game has 2 placements and 0 unplaced entries");
        CHECK(findings[1].message == "the game has 1 placements and 1 unplaced entries");
    }
    SECTION("game-missing: a game of the selection neither placed nor unplaced") {
        WorldPlan plan = rv::test::validRoomPlan();
        ValidationContext context = rv::test::validContextFor(plan);
        plan.placements.pop_back(); // game 30, a title of the selection
        plan.unplaced.pop_back();   // game 102, unplaced in the selection
        const auto findings = requireOnlyHard(rv::world::validatePlan(plan, context), rule::kGameMissing);
        REQUIRE(findings.size() == 2);
        CHECK(findings[0].subject == "g000000000000001e");
        CHECK(findings[1].subject == "g0000000000000066");
        CHECK_THAT(findings[0].message, ContainsSubstring("selection of room:snes has neither a placement nor"));
        // Without a selection, nothing is missing.
        context.selections.clear();
        CHECK(rv::world::validatePlan(plan, context).passed());
    }
    SECTION("representative-unknown: a duplicate title whose representative is not placed") {
        WorldPlan plan = rv::test::validRoomPlan();
        plan.unplaced[0].representative = testGameId(77);
        auto findings = requireOnlyHard(validate(plan), rule::kRepresentativeUnknown);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "g0000000000000065");
        CHECK(findings[0].message ==
              "the representative g000000000000004d is neither placed nor listed for another reason");

        plan.unplaced[0].representative = testGameId(101);
        findings = requireOnlyHard(validate(plan), rule::kRepresentativeUnknown);
        CHECK_THAT(findings[0].message, ContainsSubstring("names itself"));

        // A representative unplaced for its capacity is a valid one (the title's other versions follow it).
        plan = rv::test::validRoomPlan();
        plan.unplaced.push_back({testGameId(103), snesRoomId(), rv::world::UnplacedReason::Capacity, {}});
        plan.unplaced[0].representative = testGameId(103);
        CHECK(rv::world::validatePlan(plan, rv::test::validContextFor(plan)).passed());
    }
}

TEST_CASE("world/validation: pins", "[world][validation]") {
    const WorldPlan plan = rv::test::validRoomPlan();
    ValidationContext context = rv::test::validContextFor(plan);
    // Honoured: a game in its slot, a fixture in its socket with its prefab.
    context.gamePins = {{testGameId(1), snesSlotId("hw-w", 0, 0)}};
    context.fixturePins = {{snesFixtureId("w04"), rv::world::PrefabRef{"decor.poster_frame.generic", "poster_frame"}},
                           {snesFixtureId("dc01"), std::nullopt}};
    CHECK(rv::world::validatePlan(plan, context).findings.empty());

    SECTION("pin-broken: a game elsewhere, a fixture missing or of another prefab") {
        context.gamePins = {{testGameId(1), snesSlotId("hw-w", 0, 5)}};
        context.fixturePins = {{snesFixtureId("dec-nw"), std::nullopt},
                               {snesFixtureId("w04"), rv::world::PrefabRef{"decor.lamp.floor.generic", "floor_lamp"}}};
        const auto findings = requireOnlyHard(rv::world::validatePlan(plan, context), rule::kPinBroken);
        REQUIRE(findings.size() == 3);
        CHECK(findings[0].subject == "g0000000000000001");
        CHECK(findings[0].message == "the game is pinned to room:snes/fx:hw-w/s:0.5 but does not stand there");
        CHECK(findings[1].subject == "room:snes/fx:dec-nw");
        CHECK(findings[2].subject == "room:snes/fx:w04");
    }
    SECTION("pin-orphaned: a slot, a game or a socket that no longer exists") {
        context.gamePins = {{testGameId(1), snesSlotId("w09", 0, 0)}, {testGameId(500), snesSlotId("hw-w", 0, 1)}};
        context.fixturePins = {{snesFixtureId("w99"), std::nullopt}};
        const ValidationReport report = rv::world::validatePlan(plan, context);
        INFO(describe(report));
        CHECK(report.passed());
        CHECK(softRules(report) == std::set<std::string>{std::string(rule::kPinOrphaned)});
        CHECK(findingsOf(report, rule::kPinOrphaned).size() == 3);
    }
}

TEST_CASE("world/validation: the hero TV, reachability and passages", "[world][validation]") {
    SECTION("hero-tv-missing: no crt in the hero corner") {
        WorldPlan plan = rv::test::validRoomPlan();
        std::erase_if(plan.fixtures, [](const Fixture& fixture) { return fixture.id.part() == "crt"; });
        const auto findings = requireOnlyHard(validate(plan), rule::kHeroTvMissing);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].message == "the room has no hero TV (the hero corner's crt)");
    }
    SECTION("unreachable: a shelf closed off by a display case standing in front of it") {
        WorldPlan plan = rv::test::validRoomPlan();
        Fixture& display = fixtureOf(plan, "dc01");
        display.xMm = -3000;
        display.zMm = -4000;
        const ValidationReport report = validate(plan);
        const auto findings = requireOnlyHard(report, rule::kUnreachable);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:w01");
        CHECK_THAT(findings[0].message, ContainsSubstring("no place 1200 mm wide within 400 mm of its interaction "
                                                          "point (x -3320, z -4000)"));
    }
    SECTION("unreachable: the hero TV walled in by its own corner") {
        WorldPlan plan = rv::test::validRoomPlan();
        // The armchair moved in front of the CRT, 300 mm from the cabinet: no 1.2 m body fits between them.
        Fixture& armchair = fixtureOf(plan, "hero", "armchair");
        armchair.xMm = -805;
        armchair.zMm = -4470 + 300 + 568;
        const ValidationReport report = validate(plan);
        INFO(describe(report));
        const auto findings = findingsOf(report, rule::kUnreachable);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:hero.crt");
        CHECK(hardRules(report) == std::set<std::string>{std::string(rule::kUnreachable)});
    }
    SECTION("passage-narrow: reachable through the minimum clear width, not through the main passage") {
        const WorldPlan plan = rv::test::validRoomPlan();
        ValidationContext context = rv::test::validContextFor(plan);
        context.config.mainPassageWidthMm = 1500;
        const ValidationReport report = rv::world::validatePlan(plan, context);
        INFO(describe(report));
        CHECK(report.passed());
        CHECK(softRules(report) == std::set<std::string>{std::string(rule::kPassageNarrow)});
        // The display cases stand 1.5 m apart: their interaction points lie in the band between them, which a 1.5 m
        // body does not enter.
        const auto findings = findingsOf(report, rule::kPassageNarrow);
        CHECK(std::any_of(findings.begin(), findings.end(),
                          [](const ValidationFinding& finding) { return finding.subject == "room:snes/fx:dc01"; }));
        REQUIRE(report.layoutScorePermille.has_value());
        CHECK(*report.layoutScorePermille < 951);
    }
    SECTION("tv-line-of-sight: a tall armchair between the doorway and the TV") {
        WorldPlan plan = rv::test::validRoomPlan();
        Fixture& armchair = fixtureOf(plan, "hero", "armchair");
        armchair.xMm = -600;
        armchair.heightMm = 1200;
        const ValidationReport report = validate(plan);
        INFO(describe(report));
        CHECK(report.passed());
        const auto findings = findingsOf(report, rule::kTvLineOfSight);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:hero.crt");
        CHECK(findings[0].message == "the hero TV is hidden from the doorway by room:snes/fx:hero.armchair");
        // The same armchair at its usual height leaves the view free over it.
        armchair.heightMm = 900;
        CHECK(findingsOf(validate(plan), rule::kTvLineOfSight).empty());
    }
}

TEST_CASE("world/validation: sockets left bare and the decoration density", "[world][validation]") {
    SECTION("socket-bare: a side-wall socket with neither a module nor decoration") {
        WorldPlan plan = rv::test::validRoomPlan();
        std::erase_if(plan.fixtures, [](const Fixture& fixture) { return fixture.id.socket() == "w07"; });
        const ValidationReport report = validate(plan);
        CHECK(report.passed());
        const auto findings = findingsOf(report, rule::kSocketBare);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].subject == "room:snes/fx:w07");
        CHECK(findings[0].message == "the side-wall socket holds neither a module nor decoration");
    }
    SECTION("decoration-density: the decoration sockets in use against style §6.3's bands") {
        WorldPlan plan = rv::test::validRoomPlan();
        for (std::string_view socket : {"dec-nw", "dec-ne"}) {
            Fixture poster = fixtureOf(plan, "w04");
            poster.id = snesFixtureId(socket);
            poster.xMm = socket == "dec-nw" ? -3650 : 3650;
            poster.zMm = -4970;
            poster.yawCdeg = 0;
            plan.fixtures.push_back(poster);
        }
        ValidationReport report = validate(plan);
        INFO(describe(report));
        CHECK(report.passed());
        auto findings = findingsOf(report, rule::kDecorationDensity);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].message == "4 of the 4 decoration sockets hold decoration (1000 permille), outside the "
                                     "band of the decoration density 400 permille (250 to 599)");
        // A rich density takes them all.
        ValidationContext context = rv::test::validContextFor(rv::test::validRoomPlan());
        context.config.decorationDensityPermille = 900;
        CHECK(findingsOf(rv::world::validatePlan(plan, context), rule::kDecorationDensity).empty());
        CHECK(
            findingsOf(rv::world::validatePlan(rv::test::validRoomPlan(), context), rule::kDecorationDensity).size() ==
            1);
    }
}

TEST_CASE("world/validation: findings come rule by rule, each rule's by subject", "[world][validation]") {
    WorldPlan plan = rv::test::validRoomPlan();
    plan.placements[5].slot = snesSlotId("w09", 0, 0);
    plan.placements[2].slot = snesSlotId("w08", 0, 0);
    plan.placements.push_back({snesSlotId("w01", 0, 10), testGameId(1), 0, 0});
    ValidationContext context = rv::test::validContextFor(rv::test::validRoomPlan());
    context.lights = {{snesRoomId(), 20}};
    const ValidationReport report = rv::world::validatePlan(plan, context);
    REQUIRE(report.findings.size() == 4);
    CHECK(report.findings[0].rule == rule::kLightsOverLimit);
    CHECK(report.findings[1].rule == rule::kSlotUnknown);
    CHECK(report.findings[1].subject == "room:snes/fx:w08/s:0.0");
    CHECK(report.findings[2].rule == rule::kSlotUnknown);
    CHECK(report.findings[2].subject == "room:snes/fx:w09/s:0.0");
    CHECK(report.findings[3].rule == rule::kGameTwice);
}

TEST_CASE("world/validation: the prefab slots of the shipped manifests", "[world][validation]") {
    const auto slots = rv::world::manifestPrefabSlots(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR));
    // The test room's shelf: 5 levels of slots for the face-out cartridge, as its prefab's sockets name them.
    const auto shelf = slots({"furniture.shelf_modular.generic", "shelf"});
    REQUIRE(shelf.has_value());
    REQUIRE_FALSE(shelf->empty());
    for (std::size_t i = 0; i < shelf->size(); ++i) {
        CHECK((*shelf)[i].level == i);
        CHECK((*shelf)[i].slots > 0);
    }
    // A prefab without slots has none.
    const auto cabinet = slots({"furniture.tv_cabinet.generic", "tv_cabinet"});
    REQUIRE(cabinet.has_value());
    CHECK(cabinet->empty());
    // A manifest that builds another kind, a missing manifest and a malformed id are errors.
    CHECK_FALSE(slots({"furniture.tv_cabinet.generic", "shelf"}).has_value());
    CHECK_FALSE(slots({"furniture.no_such_thing.generic", "shelf"}).has_value());
    CHECK_FALSE(slots({"Not An Id", "shelf"}).has_value());
}
