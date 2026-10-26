#pragma once

// Capacity from the prefabs' slot sockets, never from constants of the generator (ADR-0010 §5; I3 step 3): what each
// prefab the room uses measures and holds (PrefabInfo, read once from the built prefab: its bounds, its levels of
// "Slot_<level>_<index>" sockets, a lamp's light source), the room's display slots D, hero band B and spine-out slots
// (Sh per hero-wall module, S per side-wall module) as the template's sockets give them (CollectionCapacity), and the
// display rule's counts for T titles (ADR-0010 §3): the face-out titles, the reserve, the side-wall modules, the growth
// steps and the titles beyond the template's limit (countCollection()).
//
// Everything here is integer millimetres: a prefab's metres are rounded to whole millimetres once, when it is read
// (loadPrefabCatalog()), so that the generator's arithmetic is integer and Debug and Release agree (ADR-0010 §8).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/room_template.hpp>

namespace rv::assets {
struct PrefabFamilies;
} // namespace rv::assets

namespace rv::world {

/// How the items of a level stand, judged from its slot volume against the media family's item (I3 step 1b: a level is
/// face-out when its slot is the item as it is, spine-out when it is the item on its end).
enum class LevelFacing : std::uint8_t {
    FaceOut,
    SpineOut,
    Other, ///< neither: another item's slots; the level holds no title of this family and is dressed
};

/// One level of a prefab's slots, in the prefab's frame.
struct SlotLevelInfo {
    std::uint32_t level = 0;
    std::uint32_t slots = 0;
    /// The slot volume (width along the level, height, depth) of its first slot.
    std::int64_t slotWidthMm = 0;
    std::int64_t slotHeightMm = 0;
    std::int64_t slotDepthMm = 0;
    /// The height of the first slot's origin (its bottom centre) above the prefab's origin.
    std::int64_t bottomMm = 0;

    /// The height of the slots' centres: bottomMm + slotHeightMm / 2 (rounded down).
    [[nodiscard]] std::int64_t centreMm() const noexcept { return bottomMm + slotHeightMm / 2; }
    [[nodiscard]] LevelFacing facing(const MediaFamily& family) const noexcept;
    /// Its facing in a fixture of `role` (I6 step 6): facing(family), but that a family facing front (its items in a
    /// module as face-out: the arcade cabinets, SpineFacing::Front) stands a hero-wall or side-wall module's face-out
    /// level as the spine-out section's (every title in a cabinet of the fill order, the reserve kept, no hero band).
    [[nodiscard]] LevelFacing facing(const MediaFamily& family, FixtureRole role) const noexcept;

    auto operator<=>(const SlotLevelInfo&) const = default;
};

/// A point in a prefab's frame.
struct PrefabPointMm {
    std::int64_t xMm = 0;
    std::int64_t yMm = 0;
    std::int64_t zMm = 0;

    auto operator<=>(const PrefabPointMm&) const = default;
};

/// What the generator reads of a prefab: its bounds in its own frame (the front facing +Z), its slot levels by level,
/// and the light source of a lamp ("LightSource" anchor).
struct PrefabInfo {
    /// The prefab as the data names it: the plan records this, so that the room falls back to the family's generic
    /// prefab when its manifest is missing (assets::resolvePrefab), as it does here.
    PrefabRef ref;
    /// The manifest that was used ("" for the family's procedural prefab, tier C), for the reports.
    std::string resolvedManifest;
    std::int64_t minXMm = 0;
    std::int64_t minYMm = 0;
    std::int64_t minZMm = 0;
    std::int64_t maxXMm = 0;
    std::int64_t maxYMm = 0;
    std::int64_t maxZMm = 0;
    std::vector<SlotLevelInfo> levels;
    std::optional<PrefabPointMm> lightSource;

    [[nodiscard]] std::int64_t widthMm() const noexcept { return maxXMm - minXMm; }
    [[nodiscard]] std::int64_t heightMm() const noexcept { return maxYMm - minYMm; }
    [[nodiscard]] std::int64_t depthMm() const noexcept { return maxZMm - minZMm; }
    /// The slots of the levels whose items stand with `facing`.
    [[nodiscard]] std::uint64_t slots(const MediaFamily& family, LevelFacing facing) const noexcept;
    /// The slots of the levels whose items stand with `facing` in a fixture of `role` (SlotLevelInfo::facing()).
    [[nodiscard]] std::uint64_t slots(const MediaFamily& family, LevelFacing facing, FixtureRole role) const noexcept;

    auto operator<=>(const PrefabInfo&) const = default;
};

/// The prefabs a room may use, sorted by ref, one each.
struct PrefabCatalog {
    std::vector<PrefabInfo> prefabs;

    /// The prefab of `ref`, or nullptr.
    [[nodiscard]] const PrefabInfo* find(const PrefabRef& ref) const noexcept;
    /// Adds or replaces `info` (by its ref), keeping the order.
    void put(PrefabInfo info);
};

/// The prefabs a generation reads: the template's modules, display fixtures, hero corner parts, decoration and shelf
/// dressing, the platform's console (when it has one) and the media family's item.
[[nodiscard]] std::vector<PrefabRef> prefabsToRead(const RoomTemplate& roomTemplate,
                                                   const std::optional<PrefabRef>& console, const MediaFamily& family);

/// Reads one prefab through assets::resolvePrefab (the requested manifest, else its family's generic manifest, else
/// the family's procedural prefab, each fallback logged once) and measures it: a procedural prefab is built and read
/// from its bounds and sockets (geometry::slotLevels, the "LightSource" anchor); a model manifest from its
/// runtime.scaleMeters (the pivot on the floor at the centre of its footprint) without slots. Errors: those of
/// resolvePrefab; ValidationFailed for a prefab whose kind is not the one named, or whose level slot indices do not run
/// from 0.
[[nodiscard]] Expected<PrefabInfo> readPrefabInfo(const std::filesystem::path& assetRoot,
                                                  const assets::PrefabFamilies& families, const PrefabRef& ref);
/// readPrefabInfo() for each of `refs`. Errors: the first prefab's that fails, naming it.
[[nodiscard]] Expected<PrefabCatalog> loadPrefabCatalog(const std::filesystem::path& assetRoot,
                                                        const assets::PrefabFamilies& families,
                                                        std::span<const PrefabRef> refs);

/// The catalog's canonical document (part of the plan's inputDigest): every prefab with its bounds and levels.
[[nodiscard]] nlohmann::json prefabCatalogToJson(const PrefabCatalog& catalog);

/// The room's slots as the template's sockets and the prefabs give them (ADR-0010 §3, §5), for one media family.
struct CollectionCapacity {
    /// D: the face-out slots of the display fixtures (displayOrder).
    std::uint64_t displaySlots = 0;
    /// B: the face-out slots of the hero band's modules (heroBand).
    std::uint64_t bandSlots = 0;
    /// 2 Sh: the spine-out slots of every hero-wall module.
    std::uint64_t heroSpineSlots = 0;
    /// S: the spine-out slots of one side-wall module.
    std::uint64_t sideModuleSlots = 0;
    /// The side-wall sockets of the room after each growth step: index 0 the base room, the last the template's limit.
    std::vector<std::uint32_t> sideSockets;

    /// The side-wall sockets at the template's limit.
    [[nodiscard]] std::uint32_t maxSideSockets() const noexcept { return sideSockets.empty() ? 0 : sideSockets.back(); }

    bool operator==(const CollectionCapacity&) const = default;
};

/// Errors: NotFound for a prefab the template names that the catalog lacks.
[[nodiscard]] Expected<CollectionCapacity> collectionCapacity(const RoomTemplate& roomTemplate,
                                                              const PrefabCatalog& prefabs, const MediaFamily& family);

/// The reserve the spine-out section keeps for T titles: ceil(T * reservePermille / 1000) (ADR-0010 §5).
[[nodiscard]] std::uint64_t reserveFor(std::uint64_t titles, std::int64_t reservePermille) noexcept;

/// The side-wall modules T titles need with their reserve: max(0, ceil((T - min(T, D + B) + R - 2 Sh) / S))
/// (ADR-0010 §3); nothing when they need one and a side-wall module holds no spine-out slot.
[[nodiscard]] std::optional<std::uint64_t> sideModulesFor(const CollectionCapacity& capacity, std::uint64_t titles,
                                                          std::int64_t reservePermille) noexcept;

/// The display rule's counts for T titles in priority order (ADR-0010 §3).
struct CollectionCounts {
    /// The titles the room places: T, or fewer at the template's limit.
    std::uint64_t placed = 0;
    /// The face-out titles, min(placed, D + B): the first `featured` on the display fixtures, the next `band` in the
    /// hero band; the others stand spine-out.
    std::uint64_t featured = 0;
    std::uint64_t band = 0;
    std::uint64_t spine = 0;
    /// The reserve of the placed titles, kept in the spine-out section.
    std::uint64_t reserve = 0;
    /// The side-wall modules (e) and the growth steps the room takes to hold them.
    std::uint32_t sideModules = 0;
    std::uint32_t growthSteps = 0;
    /// The titles last in priority order that the room at the template's limit does not hold with the reserve kept:
    /// unplaced with reason capacity.
    std::uint64_t overCapacity = 0;

    bool operator==(const CollectionCounts&) const = default;
};

/// The counts for `titles` titles: e side-wall modules in the smallest room whose sockets hold them; when even the
/// template's limit does not, the most titles it holds (the largest T' with e(T') at most its sockets) and the rest
/// over capacity.
[[nodiscard]] CollectionCounts countCollection(const CollectionCapacity& capacity, std::uint64_t titles,
                                               std::int64_t reservePermille) noexcept;

} // namespace rv::world
