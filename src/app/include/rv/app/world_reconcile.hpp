#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The persisted start's reconciliation (I3 step 10c; ADR-0010 §12): at start the stored plan's room shows before the
// catalog's snapshot answers (WorldFlow's preview, a WorldBuild of the plan as stored), and the room's job then
// reconciles it with the snapshot (the incremental rules: unchanged, updated, kept). What the main thread does with the
// result, compared with the room it shows:
//
//   - the same plan (its digest) and the same collection (every item's game, pick id, transform and flags): nothing
//     moves, nothing is rebuilt; the covers are served again (with their sources checked under the media root) when
//     the snapshot's cover requests differ from those the room was shown with or the media root is known, and the hero
//     CRT gets its games (Textures);
//   - another plan or another collection (games added or removed, an availability changed): the room is committed
//     again, the walker where it stands (Rebuild); the items that stood keep their pick ids and transforms (the
//     incremental rules move none: LayoutDifference::moved is 0);
//   - no plan (the stored plan refused under the world's lock, the room's data unreadable): the empty room's shell
//     with its notice, as when no plan was stored (Shell).
//
// Since I4 step 5a the room is the start floor (ShownFloor, world_live_plan.hpp): the same plan and floor with every
// collection room's collection the same is Textures, anything else Rebuild of the whole floor. Reconciling room by
// room is I4 step 6b's.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <rv/app/world_build.hpp>
#include <rv/world/collection_layout.hpp>

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

enum class Reconciliation : std::uint8_t {
    Textures, ///< the room as it shows, its covers served again when they changed, the CRT's games
    Rebuild,  ///< the room committed again from the build, the walker where it stands
    Shell,    ///< the empty room's shell with the build's notice
};

/// "textures", "rebuild", "shell".
[[nodiscard]] std::string_view toString(Reconciliation reconciliation) noexcept;

struct ReconcilePlan {
    Reconciliation kind = Reconciliation::Rebuild;
    /// The shown room's collection against the build's (zero for Shell).
    LayoutDifference difference;
    /// Textures: the cover loader starts again on the room's collection with the build's cover requests.
    bool reloadCovers = false;
};

/// What to do with `build` while `shown` (the preview: a plan, its layout and cover requests) shows (see the file
/// comment).
[[nodiscard]] ReconcilePlan planReconciliation(const WorldBuild& shown, const WorldBuild& build);

} // namespace rv::app
