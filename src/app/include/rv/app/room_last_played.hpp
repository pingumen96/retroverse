#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or RmlUi header is included.
//
// The game a console room played last, standing in its console (I12 step 3, ADR-0021 §1): a runtime proxy, never a
// plan placement. Its game is the room's placed game Retroverse started last (its items' Retroverse sessions,
// RoomItemScreen::lastPlayed, or the catalog's sessions now where the floor's build read them: FloorSources::
// lastPlayed); ES-DE's <lastplayed> is not part of the room's snapshot. The proxy is the room's own item of that game
// (its family's prefab, its cover), standing where Avvia's insertion leaves the item it carries: half down in the
// console's media socket, upright, its front the console's front, shrunk into the opening (world_inspect.hpp's
// insertedItemTransform() at its end). The app (WorldSession, world_last_played.cpp) draws the floor's proxies as
// collections of their own (registry keys under kLastPlayedKeyPrefix), hides them while a game is inspected, carried
// or launched, and moves a room's to the game a launch in it returned from.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>

#include <glm/glm.hpp>

#include <rv/app/collection_layers.hpp>
#include <rv/app/room_items.hpp>
#include <rv/app/world_inspect.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/world/collection_layout.hpp>

namespace rv::app {

/// The proxies' registry key prefix: "collection/lastplayed:<room id>".
inline constexpr std::string_view kLastPlayedKeyPrefix = "collection/lastplayed:";

/// The game of `layout`'s items Retroverse played last: the greatest last play, from `overrides` (the catalog's
/// sessions now, in place of the items') or else `textOf`'s (its items' RoomItemScreen::lastPlayed), the smaller GameId
/// on a tie; none when no placed game was ever played. `textOf` gives a game's text, or nullptr.
[[nodiscard]] std::optional<catalog::GameId>
lastPlayedGame(const world::CollectionLayout& layout,
               const std::function<const RoomItemText*(const catalog::GameId&)>& textOf,
               const std::map<catalog::GameId, std::int64_t>* overrides);

/// The proxy's pose: `item` at its end of the insertion into `socket` (insertedItemTransform() at progress 1, its scale
/// insertedScale() of its extents in the opening), from the item prefab's space to `socket`'s frame.
[[nodiscard]] glm::mat4 lastPlayedTransform(const world::CollectionItem& item, const ConsoleSocket& socket);

/// The proxy's collection: `room`'s item of `game` alone (none when the room places no such game), standing in
/// `socket` (lastPlayedTransform()), face-out, part of its fixture (never pulled), pick id 1, its layer keys and title
/// the room's, one cluster, the item's group of the room. Its room id is `room`'s.
[[nodiscard]] std::optional<world::CollectionLayout>
lastPlayedLayout(const world::CollectionLayout& room, const catalog::GameId& game, const ConsoleSocket& socket);

/// The proxy's cover: the room's cover item of `game` (its picture, label and spine, the room's keys), or none when
/// its text is unknown (the proxy then shows its pending picture).
[[nodiscard]] std::optional<CoverItem> lastPlayedCover(std::span<const CoverItem> covers, const catalog::GameId& game);

} // namespace rv::app
