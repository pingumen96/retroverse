#pragma once

// Private to rv_world (I3 step 3): the allocation of a fresh room's titles to slots (ADR-0010 §5, §6), on the fixtures
// fixture_layout placed:
//   - the featured titles fill the display fixtures in their order and the band's titles the hero band's levels, each
//     level whole before the next, no reserve; the slots they leave hold shelf dressing;
//   - the spine-out titles fill the modules one after the other in the collection's order: in each module the
//     spine-out levels in band order (the level whose slots' centres are nearest the eye height first, then one level
//     at a time alternately above and below, above first, one side's levels in order once the other's are taken),
//     each to its quota of floor(slots / (1 + reserve)) titles, its last slots kept free; when the quotas cannot hold
//     the section's titles, every quota rises by one until they do (never past a level's slots);
//   - within each section the titles stand in the order (catalog sort key, title key, GameId), as one reads a page: a
//     fixture's top used level first, left to right (slot 0 first), then the level below, fixture after fixture;
//   - every level no title needs holds one dressing prop, chosen from the level's prop seed (no bare board).
// A placement's offset comes from its own seed (seeds.hpp): spine-out pushed in by 0 to kMaxInsetMm, face-out turned by
// at most kMaxFaceOutYawCdeg either way.

#include <cstdint>
#include <span>
#include <vector>

#include <rv/world/capacity.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_plan.hpp>

#include "fixture_layout.hpp"

namespace rv::world::detail {

inline constexpr std::int64_t kMaxInsetMm = 4;
inline constexpr std::int64_t kMaxFaceOutYawCdeg = 150;

struct AllocationInput {
    const RoomLayout& layout;
    /// The placed titles in priority order: the first counts.featured are featured, the next counts.band stand in the
    /// band, the rest spine-out.
    std::span<const RoomTitle> titles;
    const CollectionCounts& counts;
    std::int64_t reservePermille = 0;
    std::int64_t eyeHeightMm = 0;
    const MediaFamily& family;
    std::uint64_t roomSeed = 0;
    std::span<const PrefabRef> shelfDressing;
};

struct Allocation {
    std::vector<Placement> placements;
    std::vector<DressedLevel> dressing;
};

[[nodiscard]] Allocation allocate(const AllocationInput& input);

} // namespace rv::world::detail
