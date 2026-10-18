#pragma once

// What the incremental update's tests share (I3 step 9): selections that grow and shrink (a generated selection's
// subsets, new invented games that are titles, versions of placed titles or games without a cover), the update of a
// plan to a selection, and what a plan's spine-out section holds (its free slots, dressed slots, the face-out set).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/random.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>

#include "world_generation_support.hpp"

namespace rv::test::world {

/// `full` with only the titles `keep` accepts (in their priority order) and the unplaced games whose representative it
/// keeps (games without a cover are kept unless `keepUnplaced` refuses them).
template <class KeepTitle, class KeepUnplaced>
rv::world::RoomSelection subsetSelection(const rv::world::RoomSelection& full, KeepTitle&& keep,
                                         KeepUnplaced&& keepUnplaced) {
    rv::world::RoomSelection subset = full;
    subset.titles.clear();
    std::set<rv::catalog::GameId> titles;
    for (const rv::world::RoomTitle& title : full.titles) {
        if (keep(title.game)) {
            subset.titles.push_back(title);
            titles.insert(title.game);
        }
    }
    subset.unplaced.clear();
    for (const rv::world::UnplacedGame& game : full.unplaced) {
        const bool represented =
            game.reason != rv::world::UnplacedReason::DuplicateTitle || titles.contains(game.representative);
        if (represented && keepUnplaced(game.game)) {
            subset.unplaced.push_back(game);
        }
    }
    return subset;
}

template <class KeepTitle>
rv::world::RoomSelection subsetSelection(const rv::world::RoomSelection& full, KeepTitle&& keep) {
    return subsetSelection(full, std::forward<KeepTitle>(keep), [](const rv::catalog::GameId&) { return true; });
}

/// Every GameId a selection names.
inline std::set<std::uint64_t> gameValuesOf(const rv::world::RoomSelection& selection) {
    std::set<std::uint64_t> values;
    for (const rv::world::RoomTitle& title : selection.titles) {
        values.insert(title.game.value());
    }
    for (const rv::world::UnplacedGame& game : selection.unplaced) {
        values.insert(game.game.value());
    }
    return values;
}

/// A GameId none of `taken` holds (added to it).
inline rv::catalog::GameId freshGameId(rv::core::Random& random, std::set<std::uint64_t>& taken) {
    for (;;) {
        const std::uint64_t value = random.next() & rv::catalog::GameId::kMaxValue;
        if (value != 0 && taken.insert(value).second) {
            return *rv::catalog::GameId::fromValue(value);
        }
    }
}

/// A new invented title (syllables, so that its place in the alphabet varies, then its GameId, so that its title key
/// is no other title's), inserted into `selection` in priority order.
inline rv::world::RoomTitle addTitle(rv::world::RoomSelection& selection, rv::core::Random& random,
                                     std::set<std::uint64_t>& taken, bool favorite = false,
                                     std::uint64_t playCount = 0) {
    rv::world::RoomTitle title;
    title.game = freshGameId(random, taken);
    const std::string key = inventedTitle(random) + " " + title.game.toString();
    title.titleKey = key;
    title.sortKey = key;
    title.favorite = favorite;
    title.playCount = playCount;
    const auto at = std::upper_bound(selection.titles.begin(), selection.titles.end(), title, rv::world::priorityLess);
    selection.titles.insert(at, title);
    return title;
}

/// A game unplaced for `reason` (a version of `representative` for DuplicateTitle), inserted into `selection`.
inline rv::world::UnplacedGame addUnplaced(rv::world::RoomSelection& selection, rv::core::Random& random,
                                           std::set<std::uint64_t>& taken, rv::world::UnplacedReason reason,
                                           rv::catalog::GameId representative = {}) {
    rv::world::UnplacedGame game;
    game.game = freshGameId(random, taken);
    game.room = selection.room;
    game.reason = reason;
    game.representative = representative;
    selection.unplaced.insert(std::upper_bound(selection.unplaced.begin(), selection.unplaced.end(), game), game);
    return game;
}

/// The update of `previous` to `selection` with the world seed `worldSeed` (the previous plan's digest computed).
inline rv::world::GenerationResult updateRoom(const rv::world::WorldPlan& previous,
                                              const rv::world::RoomSelection& selection, std::uint64_t worldSeed,
                                              rv::world::WorldOverrides overrides = {},
                                              std::vector<rv::world::GameAlias> aliases = {}) {
    rv::world::UpdateInput input;
    input.library = library();
    input.selection = selection;
    input.config.seed = worldSeed;
    input.overrides = std::move(overrides);
    input.previous = previous;
    input.aliases = std::move(aliases);
    auto result = rv::world::update(input, snesData());
    if (!result) {
        UNSCOPED_INFO(result.error().toString());
    }
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// A fresh plan of `selection` with the world seed `worldSeed`.
inline rv::world::GenerationResult generateSelection(const rv::world::RoomSelection& selection,
                                                     std::uint64_t worldSeed) {
    rv::world::GenerationInput input;
    input.library = library();
    input.selection = selection;
    input.config.seed = worldSeed;
    auto result = rv::world::generate(input, snesData());
    REQUIRE(result.has_value());
    return std::move(*result);
}

using LevelKey = std::pair<rv::world::FixtureId, std::uint32_t>;

/// The section of every level of the plan's fixtures.
inline std::map<LevelKey, rv::world::Section> levelSections(const rv::world::WorldPlan& plan) {
    std::map<LevelKey, rv::world::Section> sections;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            sections[{fixture.id, level.level}] = level.section;
        }
    }
    return sections;
}

/// The face-out placements (featured and band), as they stand.
inline std::vector<rv::world::Placement> faceOutPlacements(const rv::world::WorldPlan& plan) {
    const auto sections = levelSections(plan);
    std::vector<rv::world::Placement> faceOut;
    for (const rv::world::Placement& placement : plan.placements) {
        const auto found = sections.find({placement.slot.fixture(), placement.slot.level()});
        if (found != sections.end() && rv::world::isFaceOut(found->second)) {
            faceOut.push_back(placement);
        }
    }
    return faceOut;
}

/// What the spine-out section of a plan holds: its slots that no title takes and no dressing covers (the reserve),
/// and those dressing covers.
struct SpineCounts {
    std::uint64_t slots = 0;
    std::uint64_t titles = 0;
    std::uint64_t freeSlots = 0;
    std::uint64_t dressedSlots = 0;
};

inline SpineCounts countSpine(const rv::world::WorldPlan& plan) {
    SpineCounts counts;
    std::map<LevelKey, std::uint32_t> spineLevels;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            if (level.section == rv::world::Section::Spine) {
                spineLevels[{fixture.id, level.level}] = level.slots;
                counts.slots += level.slots;
            }
        }
    }
    for (const rv::world::Placement& placement : plan.placements) {
        counts.titles += spineLevels.contains({placement.slot.fixture(), placement.slot.level()}) ? 1 : 0;
    }
    for (const rv::world::DressedLevel& dressed : plan.dressing) {
        if (spineLevels.contains({dressed.level.fixture(), dressed.level.level()})) {
            counts.dressedSlots += dressed.count;
        }
    }
    counts.freeSlots = counts.slots - counts.titles - counts.dressedSlots;
    return counts;
}

inline std::size_t countFixtures(const rv::world::WorldPlan& plan, rv::world::FixtureRole role) {
    return static_cast<std::size_t>(std::count_if(plan.fixtures.begin(), plan.fixtures.end(),
                                                  [&](const rv::world::Fixture& f) { return f.role == role; }));
}

inline std::string planText(const rv::world::WorldPlan& plan) {
    auto text = rv::world::writePlan(plan);
    REQUIRE(text.has_value());
    return std::move(*text);
}

} // namespace rv::test::world
