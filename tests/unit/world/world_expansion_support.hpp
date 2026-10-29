#pragma once

// What the expansion rooms' tests share (I4b step 7, ADR-0015 §7): the overflow fixture's selections, a plan's rooms
// and chains, its capacity entries and face-out placements, the regeneration through the asset root's loader, and the
// check of a fresh chain (equal alphabetical shares).

#include "world_building_support.hpp"

#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/regeneration.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/validation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace rv::test::world {

/// The selections of tests/fixtures/world/legacy/building-overflow-seed-102938-generator-1.4.json: the esde/overflow
/// fixture's counts (a big-box system of 1,700 titles, an arcade of 60) and three rooms beside them.
inline std::vector<rv::world::RoomSelection> overflowSelections() {
    return {systemSelection("amiga", 1700, 5200), systemSelection("mame", 60, 5100), systemSelection("snes", 40, 5300),
            systemSelection("gb", 30, 5400), systemSelection("pokemini", 8, 5500)};
}

inline rv::world::RoomId roomId(std::string_view text) {
    return *rv::world::RoomId::parse(text);
}

/// The placements of `plan` by room.
inline std::map<rv::world::RoomId, std::vector<rv::world::Placement>> placementsByRoom(
    const rv::world::WorldPlan& plan) {
    std::map<rv::world::RoomId, std::vector<rv::world::Placement>> rooms;
    for (const rv::world::Placement& placement : plan.placements) {
        rooms[placement.slot.fixture().room()].push_back(placement);
    }
    return rooms;
}

/// The placements of `plan` in `room`.
inline std::size_t placedIn(const rv::world::WorldPlan& plan, const rv::world::RoomId& room) {
    return static_cast<std::size_t>(std::count_if(plan.placements.begin(), plan.placements.end(), [&](const auto& p) {
        return p.slot.fixture().room() == room;
    }));
}

inline std::size_t capacityEntries(const rv::world::WorldPlan& plan) {
    return static_cast<std::size_t>(std::count_if(plan.unplaced.begin(), plan.unplaced.end(), [](const auto& game) {
        return game.reason == rv::world::UnplacedReason::Capacity;
    }));
}

/// The room `id` with its elements, as a plan of that room, its record's generator and theme pack left out (an update
/// stamps them), so that rooms compare by their elements.
inline rv::world::WorldPlan roomPart(const rv::world::WorldPlan& plan, const rv::world::RoomId& id) {
    rv::world::WorldPlan part;
    for (const rv::world::Room& room : plan.rooms) {
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
inline std::size_t faceOutIn(const rv::world::WorldPlan& plan, const rv::world::RoomId& room) {
    const auto sections = levelSections(plan);
    return static_cast<std::size_t>(std::count_if(plan.placements.begin(), plan.placements.end(), [&](const auto& p) {
        return p.slot.fixture().room() == room &&
               rv::world::isFaceOut(sections.at({p.slot.fixture(), p.slot.level()}));
    }));
}

inline std::string hardFindings(const rv::world::ValidationReport& report) {
    std::string text;
    for (const rv::world::ValidationFinding& finding : report.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            text += finding.rule + " " + finding.subject + ": " + finding.message + "\n";
        }
    }
    return text;
}

inline const rv::world::RoomDataLoader& expansionLoader() {
    static const rv::world::RoomDataLoader load = rv::world::assetRootLoader(assetRoot());
    return load;
}

/// The regeneration input of `scope` of `previous` with `selections` and `overrides`.
inline rv::world::RegenerationInput regenerationInput(const BuildingWorld& world, const rv::world::WorldPlan& previous,
                                                      const std::vector<rv::world::RoomSelection>& selections,
                                                      const rv::world::RegenerationScope& scope,
                                                      const rv::world::WorldOverrides& overrides = {}) {
    rv::world::RegenerationInput input;
    input.scope = scope;
    input.library = library();
    input.selections = selections;
    input.platformTags = world.tags;
    input.config = world.config;
    input.overrides = overrides;
    input.previous = previous;
    return input;
}

/// The regeneration of `scope` (REQUIREd), its plan passing the validation.
inline rv::world::Regeneration regenerateOf(const BuildingWorld& world, const rv::world::WorldPlan& previous,
                                            const std::vector<rv::world::RoomSelection>& selections,
                                            const rv::world::RegenerationScope& scope,
                                            const rv::world::WorldOverrides& overrides = {}) {
    auto result = rv::world::regenerate(regenerationInput(world, previous, selections, scope, overrides),
                                        world.building, world.systems, expansionLoader());
    INFO(scope.toString() << ": " << (result ? std::string("regenerated") : result.error().toString()));
    REQUIRE(result.has_value());
    INFO(hardFindings(result->result.validation));
    CHECK(result->result.validation.passed());
    return std::move(*result);
}

/// The chain whose head is `head` in `plan`: its rooms by number.
inline std::vector<const rv::world::Room*> chainOf(const rv::world::WorldPlan& plan, const rv::world::RoomId& head) {
    std::vector<const rv::world::Room*> chain;
    for (const rv::world::Room& room : plan.rooms) {
        if (room.id.head() == head) {
            chain.push_back(&room);
        }
    }
    std::sort(chain.begin(), chain.end(),
              [](const rv::world::Room* a, const rv::world::Room* b) { return a->id < b->id; });
    return chain;
}

/// The ids of the chain whose head is `head` in `plan`, by number.
inline std::vector<rv::world::RoomId> chainIds(const rv::world::WorldPlan& plan, const rv::world::RoomId& head) {
    std::vector<rv::world::RoomId> ids;
    for (const rv::world::Room* room : chainOf(plan, head)) {
        ids.push_back(room->id);
    }
    return ids;
}

/// A chain's rooms each of the head's record but for their places (ADR-0015 §7), numbered from 2 without gaps
/// (a fresh chain's), the titles of `selections` in equal alphabetical shares (the collection's order: section, sort
/// key, title key, GameId), each placed once.
inline void checkFreshChain(const rv::world::WorldPlan& plan, const rv::world::RoomId& head,
                            const std::vector<const rv::world::RoomSelection*>& selections, std::size_t rooms) {
    INFO(head.toString());
    const std::vector<const rv::world::Room*> chain = chainOf(plan, head);
    REQUIRE(chain.size() == rooms);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const rv::world::Room& room = *chain[i];
        CHECK(room.id == *head.inChain(static_cast<std::uint32_t>(i + 1)));
        CHECK(room.expands == (i == 0 ? rv::world::RoomId{} : head));
        CHECK(room.floor == chain.front()->floor);
        CHECK(room.kind == chain.front()->kind);
        CHECK(room.sections == chain.front()->sections);
        CHECK(room.mediaFamily == chain.front()->mediaFamily);
        CHECK(room.templateId == chain.front()->templateId);
        CHECK(room.seed == rv::world::roomSeed(plan.seed, room.id));
    }
    std::vector<std::tuple<std::size_t, std::string, std::string, rv::catalog::GameId>> alphabetical;
    for (std::size_t section = 0; section < selections.size(); ++section) {
        for (const rv::world::RoomTitle& title : selections[section]->titles) {
            alphabetical.emplace_back(section, title.sortKey, title.titleKey, title.game);
        }
    }
    std::sort(alphabetical.begin(), alphabetical.end());
    std::map<rv::catalog::GameId, rv::world::RoomId> roomOfGame;
    for (const rv::world::Placement& placement : plan.placements) {
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

} // namespace rv::test::world
