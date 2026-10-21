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
//
// Since I4 step 5a the room shown is a floor (world_build.hpp): its collections are compared room by room (matched by
// room id, the items by game within them), the static part is the floor's scene file, and Items applies each room's
// collection in place. The items are compared in their room's frame, so that a room moved as a whole (its growth)
// counts none of its items as moved.
//
// Since I4 step 6b (ADR-0013 §5, §7, §9) the decision is made room by room (LiveUpdatePlan::rooms), first by each
// room's content digest: a room whose digest, covers and place are as they show is Unchanged (its collection's
// instances and uploads stay as they are, nothing re-uploaded); one whose covers alone changed (or whose covers'
// sources can be checked now) is Textures; one whose collection changed within the same static part is Items (applied
// in place, the others untouched). Any change of the floor's static part (a room added, a room grown, a new section's
// fixtures) commits the whole floor again, the walker (and a Focus or Inspect pose) kept in the frame of the area that
// holds it, moved with it (walkerMove()); each room's pictures pass to its new collection. The persisted start's
// reconciliation (world_reconcile.hpp) takes the same decision.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include <rv/app/collection_layers.hpp>
#include <rv/app/layout_difference.hpp>
#include <rv/app/world_build.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/world/collection_layout.hpp>

namespace rv::app {

/// A collection room of the floor shown: its layout (in the scene's frame, its room standing at `scenePlacement` there:
/// RoomCollection's), its cover requests, its content digest and its CRT's games.
struct ShownCollection {
    world::RoomId room;
    world::CollectionLayout layout;
    std::vector<CoverItem> covers;
    world::RoomPlacement scenePlacement;
    std::string contentDigest;
    std::vector<ScreenGame> screenGames;
};

/// What the session keeps of the floor it shows, to compare the next build with (a WorldBuild without its scene, scene
/// data or plan).
struct ShownFloor {
    std::string planDigest;
    std::string floor;
    /// The SHA-256 of the static part's scene file (WorldBuild::sceneDigest); empty when unknown (never equal).
    std::string sceneDigest;
    std::vector<ShownCollection> rooms;
    /// The covers were read under a media root (none: the cache alone served them).
    bool mediaKnown = false;
    std::vector<ScreenGame> screenGames;
    /// The floor's frame in the scene's frame (WorldBuild::floorPlacement): the hall's frame.
    world::RoomPlacement floorPlacement;

    /// The room `room`, or nullptr.
    [[nodiscard]] const ShownCollection* find(const world::RoomId& room) const noexcept;
};

/// What `build` (with a plan; its scene may have been given away) shows; nullopt for a build without a plan.
[[nodiscard]] std::optional<ShownFloor> shownFloorOf(const WorldBuild& build);

/// The floor shown against `build`'s collections, by room and by game (a room in one only: its items added or removed),
/// each room's items in its own frame (compareLayouts() with its scene placements: a room that grew, or the others in
/// the frame of a start room that grew, moved none of its items).
[[nodiscard]] LayoutDifference compareFloors(const ShownFloor& shown, const WorldBuild& build);

enum class LiveUpdateKind : std::uint8_t {
    Unchanged, ///< nothing to do
    Textures,  ///< the covers served again (only the changed pictures uploaded)
    Items,     ///< the collection's items applied in place (the scene untouched)
    Rebuild,   ///< the room committed again, the walker where it stands
    Kept,      ///< the build has nothing to show: the room shown stays
};

/// "unchanged", "textures", "items", "rebuild", "kept".
[[nodiscard]] std::string_view toString(LiveUpdateKind kind) noexcept;

/// What a build does to one collection room of the floor shown (I4 step 6b).
enum class RoomUpdateKind : std::uint8_t {
    Unchanged, ///< its digest, covers and place as they show: its collection and its pictures stay as they are
    Textures,  ///< its covers served again (a cover added, replaced or gone, a title changed, the media root known now)
    Items,     ///< its collection's items applied in place
    Added,     ///< a room the floor shown lacks (the floor committed again)
    Removed,   ///< a room the build lacks (the floor committed again)
};

/// "unchanged", "textures", "items", "added", "removed".
[[nodiscard]] std::string_view toString(RoomUpdateKind kind) noexcept;

struct RoomUpdate {
    world::RoomId room;
    RoomUpdateKind kind = RoomUpdateKind::Unchanged;
    /// Its items, compared in its own frame (zero for Added and Removed: their items are the floor's added, removed).
    LayoutDifference difference;
    /// Its items whose cover request changed.
    std::size_t coversChanged = 0;
    /// Its CRT's games differ.
    bool screenGamesChanged = false;
    /// Its content digest (or its place) differs: the plan changed something of it (its items may show the same: a game
    /// unplaced, say).
    bool planChanged = false;
};

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
    /// The hero CRT's games differ (the start room's, or any room's).
    bool screenGamesChanged = false;
    /// Why it is a Rebuild or Kept, in English (the log).
    std::string reason;
    /// Each collection room (I4 step 6b): the build's in its order, then those it lacks; empty for Kept.
    std::vector<RoomUpdate> rooms;

    /// The room `room`'s part, or nullptr.
    [[nodiscard]] const RoomUpdate* find(const world::RoomId& room) const noexcept;
};

/// What to do with `build` while `shown` shows (see the file comment).
[[nodiscard]] LiveUpdatePlan planLiveUpdate(const ShownFloor& shown, const WorldBuild& build);

/// How a floor committed again moves what stands in an area (ADR-0013 §7): from the area's frame in the floor shown to
/// its frame in the build, in the scene's frame (a translation, and a half turn when the frames are turned apart).
struct FrameMove {
    glm::mat4 matrix{1.0f};
    bool halfTurn = false;
};

/// The move of the walker (and of a Focus or Inspect pose) standing in `area` (the area that holds the eye: a
/// collection room's id, else the hall) when `build` replaces the floor shown: that room's scene placement before and
/// after, or the floor's frame for the hall; none when the frame stays (no move) or the room is not in both.
[[nodiscard]] std::optional<FrameMove> walkerMove(const ShownFloor& shown, const WorldBuild& build,
                                                  std::string_view area);

} // namespace rv::app
