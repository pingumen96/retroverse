#pragma once

// Private to rv_world (I3 step 3): the allocation of a fresh room's titles to slots (ADR-0010 §5, §6), on the fixtures
// fixture_layout placed:
//   - the featured titles fill the display fixtures in their order and the band's titles the hero band's levels, each
//     level whole before the next, no reserve; the slots they leave hold shelf dressing; a level they fill only
//     partly holds them in its middle, dressed on both sides (generator 1.2);
//   - the spine-out titles fill the modules one after the other in the collection's order: in each module the
//     spine-out levels in band order (the level whose slots' centres are nearest the eye height first, then one level
//     at a time alternately above and below, above first, one side's levels in order once the other's are taken),
//     each to its quota of floor(slots / (1 + reserve)) titles, its last slots kept free; when the quotas cannot hold
//     the section's titles, every quota rises by one until they do (never past a level's slots);
//   - within each section the titles stand in the order (catalog sort key, title key, GameId), as one reads a page: a
//     fixture's top used level first, left to right (slot 0 first), then the level below, fixture after fixture; a
//     shared or arcade room's in the order (its system's section, catalog sort key, title key, GameId) (ADR-0013 §6,
//     I4 step 4), so that each system's titles follow the previous system's;
//   - every level no title needs holds one dressing prop, chosen from the level's prop seed (no bare board).
// A placement's offset comes from its own seed (seeds.hpp): spine-out pushed in by 0 to kMaxInsetMm, face-out turned by
// at most kMaxFaceOutYawCdeg either way.

#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>

#include <rv/world/capacity.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_plan.hpp>

#include "fixture_layout.hpp"

namespace rv::world::detail {

inline constexpr std::int64_t kMaxInsetMm = 4;
inline constexpr std::int64_t kMaxFaceOutYawCdeg = 150;

/// The section of each title of a shared or arcade room: its system's index in the room's sections (ADR-0013 §6). A
/// title it does not name (every title of a console room) is of section 0.
using SectionIndex = std::map<catalog::GameId, std::uint32_t>;

/// A title's section in `sections` (null: 0).
[[nodiscard]] std::uint32_t sectionOf(const SectionIndex* sections, const catalog::GameId& game) noexcept;

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
    /// A shared or arcade room's sections; null for a console room.
    const SectionIndex* sections = nullptr;
};

struct Allocation {
    std::vector<Placement> placements;
    std::vector<DressedLevel> dressing;
};

[[nodiscard]] Allocation allocate(const AllocationInput& input);

// The rules the incremental update (incremental.cpp, I3 step 9) shares with the allocation, so that what it adds
// stands as a fresh plan would place it.

/// The spine-out levels of a module (indices into `levels`, whose facings are `facings`) in band order: the level whose
/// slots' centre is nearest `eyeHeightMm` first (a tie keeps the lower), then one level at a time alternately above and
/// below, above first, one side's levels in order once the other's are taken.
[[nodiscard]] std::vector<std::size_t> bandOrder(std::span<const SlotLevelInfo> levels,
                                                 std::span<const LevelFacing> facings, std::int64_t eyeHeightMm);
/// The dressing prop of a level: one of `shelfDressing` drawn from the level's prop seed (none when the list is empty).
[[nodiscard]] PrefabRef dressingProp(std::uint64_t roomSeed, const LevelId& level,
                                     std::span<const PrefabRef> shelfDressing);
/// A placement's offset from its seed (placementSeed of its game and the media family): a spine-out item pushed in by
/// 0 to kMaxInsetMm, a face-out one turned by at most kMaxFaceOutYawCdeg either way.
void setPlacementOffset(Placement& placement, std::uint64_t roomSeed, std::string_view mediaFamily, bool faceOut);

} // namespace rv::world::detail
