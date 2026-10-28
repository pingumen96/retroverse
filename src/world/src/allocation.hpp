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
//   - generator 1.4, for a family whose items stand on their side (I6 step 4's review, cases and boxes): the
//     hero-wall modules take their levels in turns rather than one module after the other, a round being the next
//     level in band order of each hero-wall module (the first of each, then the second of each, ...), and the titles
//     a round cannot all fill are shared out among its levels one at a time in the modules' order (so a room of a few
//     dozen cases shows them on both hero-wall modules at the eye, neither module left to dressing); the side-wall
//     modules follow, one after the other as before. A family on its end (the cartridges) fills as before, each level
//     a round of its own (spineFillRounds());
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
    /// A regeneration's pinned games, by slot (I4b step 5, ADR-0015 §3): each stands in its slot (a slot of the
    /// layout's, of a level of the family's items) before the titles are allocated, and the titles (which never hold a
    /// pinned game) skip those slots: a face-out level fills its other slots from the left, its free ones dressed; a
    /// spine-out level's quota counts its pinned games, and a level holding any game is never dressed. Null: none.
    const std::map<SlotId, catalog::GameId>* reserved = nullptr;
};

struct Allocation {
    std::vector<Placement> placements;
    std::vector<DressedLevel> dressing;
    /// The titles no slot was left for, in their order; the caller unplaces them for capacity. Only a regeneration's
    /// pins leave any (the counts fit the titles otherwise): the slots reserved for pinned games, and the side-wall
    /// sockets its pinned fixtures take or block once the room stands at its template's growth limit.
    std::vector<catalog::GameId> overflow;
};

[[nodiscard]] Allocation allocate(const AllocationInput& input);

// The rules the incremental update (incremental.cpp, I3 step 9) shares with the allocation, so that what it adds
// stands as a fresh plan would place it.

/// The spine-out levels of a module (indices into `levels`, whose facings are `facings`) in band order: the level whose
/// slots' centre is nearest `eyeHeightMm` first (a tie keeps the lower), then one level at a time alternately above and
/// below, above first, one side's levels in order once the other's are taken.
[[nodiscard]] std::vector<std::size_t> bandOrder(std::span<const SlotLevelInfo> levels,
                                                 std::span<const LevelFacing> facings, std::int64_t eyeHeightMm);
/// A level of the collection's modules: the index of its module (into the list given) and of the level.
struct ModuleLevel {
    std::size_t module = 0;
    std::size_t level = 0;

    bool operator==(const ModuleLevel&) const = default;
};

/// The collection's spine-out levels in fill order, in rounds (the rules in the file comment): `bandLevels` are the
/// spine-out levels of each module in band order (bandOrder()), the modules in the collection's order, `heroWall`
/// whether each stands on the hero wall. Each level is a round of its own, module after module; for a family standing
/// on its side (`facing` Side) the hero-wall modules' levels come first in turns, a round holding the next level of
/// each of them.
[[nodiscard]] std::vector<std::vector<ModuleLevel>> spineFillRounds(std::span<const std::vector<std::size_t>> bandLevels,
                                                                    const std::vector<bool>& heroWall,
                                                                    SpineFacing facing);

/// The dressing prop of a level: one of `shelfDressing` drawn from the level's prop seed (none when the list is empty).
[[nodiscard]] PrefabRef dressingProp(std::uint64_t roomSeed, const LevelId& level,
                                     std::span<const PrefabRef> shelfDressing);
/// A placement's offset from its seed (placementSeed of its game and the media family): a spine-out item pushed in by
/// 0 to kMaxInsetMm, a face-out one turned by at most kMaxFaceOutYawCdeg either way.
void setPlacementOffset(Placement& placement, std::uint64_t roomSeed, std::string_view mediaFamily, bool faceOut);

} // namespace rv::world::detail
