// I3 step 7: the D-pad steps of Focus (world::SlotNavigation, ADR-0012 §1) on hand-built shelves of step 1b's figures:
// empty and dressed slots skipped, modules crossed along a wall, levels above and below within a module.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <rv/world/slot_navigation.hpp>

#include "synthetic_shelves.hpp"

using rv::test::ShelfModuleSpec;
using rv::test::SyntheticShelves;
using rv::world::kNoPick;
using rv::world::NavFixture;
using rv::world::NavLevel;
using rv::world::NavRun;
using rv::world::NavSlot;
using rv::world::PickId;
using rv::world::SlotNavigation;
using rv::world::StepDirection;

namespace {

SlotNavigation navigationOf(const SyntheticShelves& shelves) {
    auto navigation = SlotNavigation::build({shelves.run});
    REQUIRE(navigation.has_value());
    return std::move(*navigation);
}

PickId idAt(const SyntheticShelves& shelves, int module, int level, int slot) {
    const rv::test::SyntheticItem* item = shelves.at(module, level, slot);
    REQUIRE(item != nullptr);
    return item->id;
}

/// Three modules: a hero-wall module (level 5 the face-out band, a few empty slots), a side-wall module whose levels 3
/// and 4 hold dressing, and one whose every level holds dressing but level 0.
SyntheticShelves wall() {
    std::vector<ShelfModuleSpec> modules(3);
    modules[0].faceOutLevel = 5;
    modules[0].levels = 7;
    modules[0].emptySlots = {{4, 5}, {4, 6}, {4, 7}, {4, 8}, {4, 32}, {2, 0}};
    modules[1].dressedLevels = {3, 4};
    modules[2].dressedLevels = {1, 2, 3, 4, 5, 6};
    return rv::test::buildShelves(modules);
}

} // namespace

TEST_CASE("world/slot_navigation: left and right step along a level and skip empty slots", "[world][slot_navigation]") {
    const SyntheticShelves shelves = wall();
    const SlotNavigation navigation = navigationOf(shelves);
    CHECK(navigation.step(idAt(shelves, 0, 4, 3), StepDirection::Right) == idAt(shelves, 0, 4, 4));
    // Slots 5 to 8 are empty.
    CHECK(navigation.step(idAt(shelves, 0, 4, 4), StepDirection::Right) == idAt(shelves, 0, 4, 9));
    CHECK(navigation.step(idAt(shelves, 0, 4, 9), StepDirection::Left) == idAt(shelves, 0, 4, 4));
    CHECK(navigation.step(idAt(shelves, 0, 4, 1), StepDirection::Left) == idAt(shelves, 0, 4, 0));
    // Level 2 starts at slot 1: left of it lies the wall's end.
    CHECK(navigation.step(idAt(shelves, 0, 2, 1), StepDirection::Left) == std::nullopt);
    CHECK(navigation.step(idAt(shelves, 0, 0, 0), StepDirection::Left) == std::nullopt);
}

TEST_CASE("world/slot_navigation: past a level's end the step crosses into the next module", "[world][slot_navigation]") {
    const SyntheticShelves shelves = wall();
    const SlotNavigation navigation = navigationOf(shelves);
    // Module 0's level 6 ends at slot 32; module 1's level 6 begins at slot 0.
    CHECK(navigation.step(idAt(shelves, 0, 6, 32), StepDirection::Right) == idAt(shelves, 1, 6, 0));
    CHECK(navigation.step(idAt(shelves, 1, 6, 0), StepDirection::Left) == idAt(shelves, 0, 6, 32));
    // Level 4 of module 0 ends at slot 31 (32 is empty); module 1's levels 3 and 4 hold dressing: from level 4's 1.24 m
    // the nearest level in height holding items is 5 (1.44 m), not 2 (0.84 m).
    CHECK(navigation.step(idAt(shelves, 0, 4, 31), StepDirection::Right) == idAt(shelves, 1, 5, 0));
    // From module 1's level 2, right: module 2 holds items on level 0 only.
    CHECK(navigation.step(idAt(shelves, 1, 2, 32), StepDirection::Right) == idAt(shelves, 2, 0, 0));
    CHECK(navigation.step(idAt(shelves, 2, 0, 32), StepDirection::Right) == std::nullopt);
    // Back left from module 2's level 0 into module 1's level 0, at its right end.
    CHECK(navigation.step(idAt(shelves, 2, 0, 0), StepDirection::Left) == idAt(shelves, 1, 0, 32));
    // The face-out band (module 0, level 5: 6 slots) from module 1's level 5.
    CHECK(navigation.step(idAt(shelves, 1, 5, 0), StepDirection::Left) == idAt(shelves, 0, 5, 5));
}

TEST_CASE("world/slot_navigation: a module without items is crossed whole", "[world][slot_navigation]") {
    std::vector<ShelfModuleSpec> modules(3);
    modules[1].dressedLevels = {0, 1, 2, 3, 4, 5, 6};
    const SyntheticShelves shelves = rv::test::buildShelves(modules);
    const SlotNavigation navigation = navigationOf(shelves);
    CHECK(navigation.step(idAt(shelves, 0, 3, 32), StepDirection::Right) == idAt(shelves, 2, 3, 0));
    CHECK(navigation.step(idAt(shelves, 2, 3, 0), StepDirection::Left) == idAt(shelves, 0, 3, 32));
    // Stepping right from the wall's first item visits every item of that height, left to right, then stops.
    PickId at = idAt(shelves, 0, 3, 0);
    int steps = 0;
    while (const auto next = navigation.step(at, StepDirection::Right)) {
        CHECK(shelves.item(*next).level == 3);
        CHECK((shelves.item(*next).module > shelves.item(at).module ||
               shelves.item(*next).slot == shelves.item(at).slot + 1));
        at = *next;
        ++steps;
    }
    CHECK(steps == 2 * 33 - 1);
    CHECK(at == idAt(shelves, 2, 3, 32));
}

TEST_CASE("world/slot_navigation: up and down find the nearest item on the next level holding one",
          "[world][slot_navigation]") {
    const SyntheticShelves shelves = wall();
    const SlotNavigation navigation = navigationOf(shelves);
    CHECK(navigation.step(idAt(shelves, 1, 1, 10), StepDirection::Up) == idAt(shelves, 1, 2, 10));
    // Levels 3 and 4 of module 1 hold dressing: up from 2 is 5, down from 5 is 2.
    CHECK(navigation.step(idAt(shelves, 1, 2, 10), StepDirection::Up) == idAt(shelves, 1, 5, 10));
    CHECK(navigation.step(idAt(shelves, 1, 5, 10), StepDirection::Down) == idAt(shelves, 1, 2, 10));
    // The top and the bottom: up and down never leave the module.
    CHECK(navigation.step(idAt(shelves, 1, 6, 10), StepDirection::Up) == std::nullopt);
    CHECK(navigation.step(idAt(shelves, 1, 0, 10), StepDirection::Down) == std::nullopt);
    CHECK(navigation.step(idAt(shelves, 2, 0, 10), StepDirection::Up) == std::nullopt);
    // Over empty slots: level 4's slots 5 to 8 are empty, so up from level 3's slot 6 is level 4's slot 4 (two pitches
    // away; slot 9 is three).
    CHECK(navigation.step(idAt(shelves, 0, 3, 6), StepDirection::Up) == idAt(shelves, 0, 4, 4));
    // Into the face-out band from level 4: the band's slot nearest along (12.5 cm wide slots over 2 cm spines).
    const rv::test::SyntheticItem& spine = shelves.item(idAt(shelves, 0, 4, 20));
    const auto band = navigation.step(spine.id, StepDirection::Up);
    REQUIRE(band.has_value());
    const rv::test::SyntheticItem& face = shelves.item(*band);
    CHECK(face.level == 5);
    CHECK(face.faceOut);
    CHECK(std::fabs(face.faceCenter.x - spine.faceCenter.x) <= 0.5f * (rv::test::shelf::kFaceWidth +
                                                                         rv::test::shelf::kFaceGap));
    // And from the band down, the spine nearest below it.
    const auto below = navigation.step(face.id, StepDirection::Down);
    REQUIRE(below.has_value());
    CHECK(shelves.item(*below).level == 4);
    CHECK(std::fabs(shelves.item(*below).faceCenter.x - face.faceCenter.x) <= 0.5f * rv::test::shelf::kSpinePitch);
}

TEST_CASE("world/slot_navigation: ties go to the left slot and the lower level", "[world][slot_navigation]") {
    // Positions and heights exactly representable, so that the distances tie exactly.
    const NavFixture left{{NavLevel{0.25f, {NavSlot{0.0f, 1}}}, NavLevel{0.75f, {NavSlot{0.0f, 2}}}}};
    const NavFixture middle{{NavLevel{0.5f, {NavSlot{0.0f, 10}, NavSlot{0.25f, 11}, NavSlot{0.5f, 12}}},
                             NavLevel{1.0f, {NavSlot{0.0f, 13}, NavSlot{0.25f, kNoPick}, NavSlot{0.5f, 14}}}}};
    const auto navigation = SlotNavigation::build({NavRun{{left, middle}}});
    REQUIRE(navigation.has_value());
    // Up from 0.25 along: 13 at 0.0 and 14 at 0.5 tie; the left one.
    CHECK(navigation->step(11, StepDirection::Up) == PickId{13});
    // Left out of `middle` from height 0.5: `left`'s levels at 0.25 and 0.75 tie; the lower one.
    CHECK(navigation->step(10, StepDirection::Left) == PickId{1});
}

TEST_CASE("world/slot_navigation: unknown items step nowhere and bad runs are refused", "[world][slot_navigation]") {
    const SyntheticShelves shelves = wall();
    const SlotNavigation navigation = navigationOf(shelves);
    CHECK(navigation.step(kNoPick, StepDirection::Right) == std::nullopt);
    CHECK(navigation.step(999999, StepDirection::Up) == std::nullopt);
    CHECK(navigation.contains(idAt(shelves, 0, 0, 0)));
    CHECK_FALSE(navigation.contains(999999));
    CHECK(SlotNavigation{}.step(1, StepDirection::Left) == std::nullopt);
    CHECK(navigation.runs().size() == 1);

    const auto refused = [](std::vector<NavRun> runs) {
        const auto built = SlotNavigation::build(std::move(runs));
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error().code == rv::ErrorCode::InvalidArgument);
        return built.error();
    };
    const NavLevel low{0.5f, {NavSlot{0.0f, 1}, NavSlot{0.1f, 2}}};
    CHECK(refused({NavRun{{NavFixture{{low}}, NavFixture{{low}}}}}).context == "item 1");
    refused({NavRun{{NavFixture{{NavLevel{0.5f, {NavSlot{0.1f, 1}, NavSlot{0.1f, 2}}}}}}}});
    refused({NavRun{{NavFixture{{NavLevel{0.5f, {NavSlot{0.2f, 1}, NavSlot{0.1f, 2}}}}}}}});
    refused({NavRun{{NavFixture{{low, NavLevel{0.5f, {NavSlot{0.0f, 3}}}}}}}});
    refused({NavRun{{NavFixture{{NavLevel{std::numeric_limits<float>::quiet_NaN(), {NavSlot{0.0f, 1}}}}}}}});
    refused({NavRun{{NavFixture{{NavLevel{0.5f, {NavSlot{std::numeric_limits<float>::infinity(), 1}}}}}}}});
    // Empty runs, fixtures and levels are fine.
    CHECK(SlotNavigation::build({NavRun{}, NavRun{{NavFixture{}, NavFixture{{NavLevel{0.5f, {}}}}}}}).has_value());
}
