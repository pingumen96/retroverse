// The room's capacity from the prefabs' slot sockets (rv/world/capacity.hpp, ADR-0010 §3, §5; I3 step 3): D, B, Sh and
// S read from the shipped manifests built with their parameters (step 1b's figures: 44, 12, 198, 231), the side-wall
// sockets per growth step, the display rule's counts at the reference sizes and at the template's limit, and the local
// seeds against the oracle's chain (scripts/world_seed_oracle.py).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rv/core/seed.hpp>
#include <rv/world/capacity.hpp>
#include <rv/world/seeds.hpp>

#include "world_generation_support.hpp"

using rv::world::CollectionCapacity;
using rv::world::CollectionCounts;
using rv::world::LevelFacing;

namespace {

const CollectionCapacity& snesCapacity() {
    static const CollectionCapacity capacity = [] {
        const rv::world::WorldData& data = rv::test::world::snesData();
        auto computed = rv::world::collectionCapacity(data.roomTemplate, data.prefabs, data.mediaFamily);
        REQUIRE(computed.has_value());
        return *computed;
    }();
    return capacity;
}

} // namespace

TEST_CASE("world/capacity: D, B, Sh and S come from the prefabs' slot sockets", "[world][capacity]") {
    const CollectionCapacity& capacity = snesCapacity();
    CHECK(capacity.displaySlots == 44);   // D: the featured wall shelf's 12 and two display cases' 16
    CHECK(capacity.bandSlots == 12);      // B: one face-out level of 6 on each hero-wall module
    CHECK(capacity.heroSpineSlots == 396); // 2 Sh: 6 spine-out levels of 33 on each hero-wall module
    CHECK(capacity.sideModuleSlots == 231); // S: 7 spine-out levels of 33
    // 14 side-wall sockets in the base room, 4 more per 2 m step up to 16 m.
    CHECK(capacity.sideSockets == std::vector<std::uint32_t>{14, 18, 22, 26});
    CHECK(capacity.maxSideSockets() == 26);

    // The levels as the prefabs give them, facing judged against the media family's item.
    const rv::world::WorldData& data = rv::test::world::snesData();
    const rv::world::PrefabInfo* hero = data.prefabs.find(data.roomTemplate.heroModule);
    REQUIRE(hero != nullptr);
    REQUIRE(hero->levels.size() == 7);
    for (const rv::world::SlotLevelInfo& level : hero->levels) {
        INFO(level.level);
        CHECK(level.facing(data.mediaFamily) == (level.level == 5 ? LevelFacing::FaceOut : LevelFacing::SpineOut));
        CHECK(level.slots == (level.level == 5 ? 6u : 33u));
    }
    CHECK(hero->widthMm() == 900);
    CHECK(hero->heightMm() == 1775);
    CHECK(hero->depthMm() == 240);
    // The spine-out slots' centres: boards from 0.375 m in 0.2 m steps, 12.5 cm cartridges on their ends.
    CHECK(hero->levels[4].centreMm() == 1237);
    // The floor lamp's light source, read from its LightSource anchor.
    bool lamp = false;
    for (const rv::world::PrefabInfo& info : data.prefabs.prefabs) {
        if (info.ref.kind == "floor_lamp") {
            lamp = true;
            REQUIRE(info.lightSource.has_value());
            CHECK(info.lightSource->yMm > 1000);
        } else {
            CHECK_FALSE(info.lightSource.has_value());
        }
    }
    CHECK(lamp);
}

TEST_CASE("world/capacity: the display rule's counts at the reference sizes", "[world][capacity]") {
    const CollectionCapacity& capacity = snesCapacity();
    struct Expected {
        std::uint64_t titles;
        CollectionCounts counts;
    };
    // placed, featured, band, spine, reserve, side-wall modules, growth steps, over capacity
    const Expected expected[] = {
        {0, {0, 0, 0, 0, 0, 0, 0, 0}},
        {1, {1, 1, 0, 0, 1, 0, 0, 0}},
        {15, {15, 15, 0, 0, 3, 0, 0, 0}},
        {44, {44, 44, 0, 0, 7, 0, 0, 0}},
        {50, {50, 44, 6, 0, 8, 0, 0, 0}},     // G02: all face-out, no side-wall module
        {56, {56, 44, 12, 0, 9, 0, 0, 0}},    // D + B: all face-out
        {57, {57, 44, 12, 1, 9, 0, 0, 0}},    // the first spine-out title
        {890, {890, 44, 12, 834, 134, 3, 0, 0}},   // the reference library: three side-wall modules
        {2000, {2000, 44, 12, 1944, 300, 8, 0, 0}}, // the stress fixture: eight, exactly full with the reserve
        {3205, {3205, 44, 12, 3149, 481, 14, 0, 0}}, // the 9 x 10 m room's capacity
        {3206, {3206, 44, 12, 3150, 481, 15, 1, 0}}, // one more grows the room by a step
        {5615, {5615, 44, 12, 5559, 843, 26, 3, 0}}, // the 16 m room's capacity
        {8000, {5615, 44, 12, 5559, 843, 26, 3, 2385}},
    };
    for (const Expected& row : expected) {
        INFO(row.titles);
        CHECK(rv::world::countCollection(capacity, row.titles, 150) == row.counts);
    }
    CHECK(rv::world::reserveFor(890, 150) == 134);
    CHECK(rv::world::reserveFor(0, 150) == 0);
    CHECK(rv::world::reserveFor(1, 0) == 0);
    CHECK(*rv::world::sideModulesFor(capacity, 890, 150) == 3);
    CHECK(*rv::world::sideModulesFor(capacity, 8000, 150) == 38);

    // The fewest modules: one fewer would not hold the spine-out titles with the reserve, for every size up to the
    // template's limit.
    for (std::uint64_t titles = 0; titles <= 5615; titles += 7) {
        const CollectionCounts counts = rv::world::countCollection(capacity, titles, 150);
        const std::uint64_t needed = counts.spine + counts.reserve;
        CHECK(needed <= capacity.heroSpineSlots + counts.sideModules * capacity.sideModuleSlots);
        if (counts.sideModules > 0) {
            CHECK(needed > capacity.heroSpineSlots + (counts.sideModules - 1) * capacity.sideModuleSlots);
        }
        CHECK(counts.overCapacity == 0);
        CHECK(capacity.sideSockets[counts.growthSteps] >= counts.sideModules);
        if (counts.growthSteps > 0) {
            CHECK(capacity.sideSockets[counts.growthSteps - 1] < counts.sideModules);
        }
    }
}

TEST_CASE("world/seeds: the local seeds are the oracle's chain", "[world][seeds]") {
    // tests/unit/core/test_seed_random.cpp's chain, from scripts/world_seed_oracle.py.
    const std::uint64_t world = rv::core::deriveSeed("world", "esde");
    CHECK(world == 0xa1fac9cfdf9c5474ull);
    const auto room = *rv::world::RoomId::forSystem(rv::test::world::snes());
    const std::uint64_t seed = rv::world::roomSeed(world, room);
    CHECK(seed == 0x1ab6a19d258c94f6ull);
    const auto w03 = *rv::world::FixtureId::make(room, "w03");
    CHECK(rv::world::fixtureSeed(seed, w03) == 0x9ad2fe9e05606ab3ull);
    CHECK(rv::world::propSeed(seed, *rv::world::LevelId::make(w03, 2)) == 0xa7191a90ef280353ull);
    CHECK(rv::world::placementSeed(seed, *rv::catalog::GameId::parse("g3c374d5e06145c9b"), "cartridge_home_16bit") ==
          0xe33bedc98f6b310cull);
    // A socket's decoration: "prop" and the socket within its room.
    CHECK(rv::world::propSeed(seed, *rv::world::FixtureId::make(room, "dec-sw")) ==
          rv::core::deriveSeed(seed, "prop", "fx:dec-sw"));
}
