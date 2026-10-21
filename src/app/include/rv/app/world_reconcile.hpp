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
// Since I4 step 5a the room is the start floor (ShownFloor, world_live_plan.hpp). Since I4 step 6b the decision is the
// live update's, room by room (planLiveUpdate(): each room by its content digest, its items and its covers): a floor
// whose collections all stand as they show is Textures (only the rooms whose covers changed, or whose sources can be
// checked now, served again; the others' instances and pictures untouched), one whose static part stands but some
// room's items changed is Rebuild with its live plan's Items (those rooms' items applied in place), anything else
// Rebuild of the whole floor, the walker kept in the frame of the area that holds it.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <rv/app/layout_difference.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_live_plan.hpp>
#include <rv/world/collection_layout.hpp>

namespace rv::app {

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
    /// The live update's decision it was made from (I4 step 6b: room by room; Items: applied in place).
    LiveUpdatePlan live;
};

/// What to do with `build` while `shown` (the preview: a plan, its layout and cover requests) shows (see the file
/// comment).
[[nodiscard]] ReconcilePlan planReconciliation(const WorldBuild& shown, const WorldBuild& build);

} // namespace rv::app
