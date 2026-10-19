#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The live update's decision (I3 step 15b; ADR-0010 §10, §12): after a refresh of the catalog (step 13r) the room's job
// runs again (the snapshot, world::update, the plan saved) while the room shows, and its result is applied to the room
// shown at a frame boundary with as little rebuilt as the incremental rules allow. The start's reconciliation (step
// 10c) takes the same decision when its result is another collection. Compared with the room shown:
//
//   - the same plan (its digest) and the same collection, the same cover requests: nothing to do (Unchanged);
//   - the same plan and collection, other cover requests (a cover added, replaced or gone, a title changed: the spine's
//     text) or the media root known now: the covers served again, only the changed pictures uploaded (Textures,
//     ADR-0010 §10's NoRebuild);
//   - the same static part (the room's scene file, by its SHA-256) and the same media family, other items (titles
//     placed into free slots, slots freed by aliases, availabilities changed): the collection's items applied in place
//     (Items): the items that stood keep their pick ids, transforms and resident layers, the new ones take layers of
//     the arrays (grown by a GPU copy when the headroom is gone), only the collection's static group is made again, the
//     scene, the walker and the hero CRT untouched;
//   - another static part (a dressed level taken, a side-wall module added, the room grown: LocalReflow with fixtures
//     or dressing, RoomExpansion), or a family that changed: the room committed again with the walker where it stands
//     (Rebuild);
//   - a build with nothing to show (the stored plan refused or unreadable under the world's lock, an error): the room
//     shown stays as it is (Kept; logged), since a live update never takes away a room that shows (ADR-0010 §11: the
//     app keeps the previous plan).
//
// An item never moves in Items: an item of the same game at another pick id or transform (which the incremental rules
// never give) makes it a Rebuild.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/collection_layers.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_reconcile.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/world/collection_layout.hpp>

namespace rv::app {

/// What the session keeps of the room it shows, to compare the next build with (a WorldBuild without its scene, scene
/// data or plan).
struct ShownRoom {
    std::string planDigest;
    /// The SHA-256 of the static part's scene file (WorldBuild::sceneDigest); empty when unknown (never equal).
    std::string sceneDigest;
    world::CollectionLayout layout;
    std::vector<CoverItem> covers;
    /// The covers were read under a media root (none: the cache alone served them).
    bool mediaKnown = false;
    std::vector<ScreenGame> screenGames;
};

/// What `build` (with a plan, a scene and a layout) shows; nullopt for a build with nothing to show.
[[nodiscard]] std::optional<ShownRoom> shownRoomOf(const WorldBuild& build);

enum class LiveUpdateKind : std::uint8_t {
    Unchanged, ///< nothing to do
    Textures,  ///< the covers served again (only the changed pictures uploaded)
    Items,     ///< the collection's items applied in place (the scene untouched)
    Rebuild,   ///< the room committed again, the walker where it stands
    Kept,      ///< the build has nothing to show: the room shown stays
};

/// "unchanged", "textures", "items", "rebuild", "kept".
[[nodiscard]] std::string_view toString(LiveUpdateKind kind) noexcept;

struct LiveUpdatePlan {
    LiveUpdateKind kind = LiveUpdateKind::Rebuild;
    /// The room shown against the build's collection, by game (zero for Kept).
    LayoutDifference difference;
    /// The games the build places that the room shown lacks, and those it no longer holds (in GameId order).
    std::vector<catalog::GameId> added;
    std::vector<catalog::GameId> removed;
    /// Games in both whose cover request differs (their pictures served again; their hero textures dropped).
    std::vector<catalog::GameId> coversChanged;
    /// The covers are served again (Textures, Items: the new and the changed items, the adopted ones checked).
    bool reloadCovers = false;
    /// The hero CRT's games differ.
    bool screenGamesChanged = false;
    /// Why it is a Rebuild or Kept, in English (the log).
    std::string reason;
};

/// What to do with `build` while `shown` shows (see the file comment).
[[nodiscard]] LiveUpdatePlan planLiveUpdate(const ShownRoom& shown, const WorldBuild& build);

} // namespace rv::app
