#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// How two collections of a room differ, by game (I3 step 10c's reconciliation, I3 step 15b's live update; implemented
// in world_reconcile.cpp): the items compared by their pick ids, transforms (in the room's own frame, I4 step 5a) and
// flags.

#include <cstddef>

#include <rv/world/collection_layout.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

/// How two collections of a room differ, by game.
struct LayoutDifference {
    /// In both, at the same pick id and transform with the same flags.
    std::size_t kept = 0;
    /// In both, at another pick id or transform.
    std::size_t moved = 0;
    /// In both at the same place, with other flags (scene::instance_flags: its file gone or back).
    std::size_t flagsChanged = 0;
    /// In `after` only, in `before` only.
    std::size_t added = 0;
    std::size_t removed = 0;

    [[nodiscard]] bool same() const noexcept { return moved == 0 && flagsChanged == 0 && added == 0 && removed == 0; }
    bool operator==(const LayoutDifference&) const = default;
};

/// Compares the items of `before` and `after` by game (their GameIds; a game is in a layout once).
[[nodiscard]] LayoutDifference compareLayouts(const world::CollectionLayout& before,
                                              const world::CollectionLayout& after);
/// The same for two layouts of one room in the scene's frame, its room standing at `beforeAt` and at `afterAt` there
/// (world::floorCollectionLayout): the items compared in the room's own frame, so that a room moved as a whole (its
/// growth, ADR-0013 §7: followDepth; the start room's growth moving the others in its frame) moves none of its items.
/// The same placement compares exactly, as compareLayouts(); another one within 0.1 mm and 1e-4 of a rotation (a
/// slot's place is whole millimetres: the frames' float arithmetic is the only difference).
[[nodiscard]] LayoutDifference compareLayouts(const world::CollectionLayout& before,
                                              const world::RoomPlacement& beforeAt,
                                              const world::CollectionLayout& after,
                                              const world::RoomPlacement& afterAt);

} // namespace rv::app
