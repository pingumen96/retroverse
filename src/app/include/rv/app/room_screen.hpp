#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The games the generated room's hero CRT may play (I3 step 10c; ADR-0012 §7): the ES-DE catalog's games of the room's
// system with a video, a screenshot, a title screen or a cover, their media under the media root (the paths the
// room's snapshot derived from the catalog's stored presence: esde::roomMediaPaths, no folder listed), and when
// Retroverse last played them (RoomEntry::lastSession: ES-DE's own <lastplayed> is not in the snapshot). The placed
// games come first, in the featured order: the display fixtures' titles, then the hero band's, then the spine-out
// ones, each by the room's priority (world::priorityLess: favourites, play count, rating, sort key; a title's data
// gathered from its versions, as the selection gathers them); they are the start candidates (the most recently played
// placed game with a video, else the first featured one with one). The other games with media follow by GameId, played
// only when selected (the quick catalog's selection until I3 steps 13a and 13b). Nothing while the media are unknown
// (ADR-0010 §3: an unreachable media root is never read for them) or without a media root. A shared or arcade room's
// CRT plays the games of all its sections (I4 step 4, ADR-0013 §9): their snapshots taken together, its placed games in
// the room's featured order whatever their section.

#include <span>
#include <vector>

#include <rv/app/room_snapshot.hpp>
#include <rv/app/screen_policy.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

/// See the file comment. `layout` is `plan`'s collection (its items' sections), `snapshot` the snapshot the room was
/// reconciled with.
[[nodiscard]] std::vector<ScreenGame> roomScreenGames(const world::WorldPlan& plan,
                                                      const world::CollectionLayout& layout,
                                                      const RoomSnapshotResult& snapshot);
/// The same over the snapshots of the room's sections (a shared or arcade room's), each section's media under its
/// snapshot's media root; a section whose media are unknown, or whose snapshot failed, gives none.
[[nodiscard]] std::vector<ScreenGame> roomScreenGames(const world::WorldPlan& plan,
                                                      const world::CollectionLayout& layout,
                                                      std::span<const RoomSnapshotResult* const> snapshots);

} // namespace rv::app
