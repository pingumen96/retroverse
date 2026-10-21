#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or RmlUi header is included.
//
// The room's items as it last showed them (I3 step 10c; ADR-0010 §12's persisted start): for each placed game what the
// collection's layout and the cover loader read of the catalog's snapshot (its title, the title of its file name, its
// year, its cover's media path, whether its file is there) and the system's full name, kept in <user data>/cache/world/
// <library id>/room-<system>.json, so that at the next start the stored plan's room shows with its covers and spines
// from the derived-texture cache before the catalog's snapshot answers. A cache: derived from the snapshot, written
// when what it holds changes, never needed (a missing or unreadable file leaves the stored room's items on the pending
// picture until the snapshot's reconciliation). It holds the catalog's titles and the covers' paths relative to the
// media root, never an absolute path. Its text is canonical JSON (core::writeCanonicalJson), so the same items always
// give the same bytes and an unchanged room rewrites nothing.
//
// Since I4 step 6b (ADR-0013 §9) each placed game's item also holds what the hero CRT of its room needs
// (RoomItemScreen: its clip, screenshot and title screen relative to the media root, when Retroverse last played it,
// its title's priority data), so that a floor built from the plan and the items files alone (the persisted start's
// preview, a rebuild) has its CRTs' games without the catalog (room_screen.hpp). A file written before has no such
// data: its rooms' CRTs have no games until the building job writes it again.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/collection_layers.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/collection_layout.hpp>

namespace rv::app {

inline constexpr int kRoomItemsSchemaVersion = 1;

/// What the hero CRT needs of a placed game ES-DE shows (catalog::EntryState::Game; I4 step 6b): its media relative to
/// the media root as esde::roomMediaPaths names them (RoomItemText::coverPath is its cover; empty where it has none),
/// when Retroverse last played it (seconds since 1970, catalog::RoomEntry::lastSession), and its title's priority data
/// gathered from its versions as world::selectRoom gathers them (the CRT's featured order: world::priorityLess).
struct RoomItemScreen {
    std::string video;
    std::string screenshot;
    std::string titleScreen;
    std::optional<std::int64_t> lastPlayed;
    std::string sortKey;
    bool favorite = false;
    std::uint64_t playCount = 0;
    std::optional<int> ratingTenths;

    bool operator==(const RoomItemScreen&) const = default;
};

/// One placed game's text, as the room's snapshot gave it.
struct RoomItemText {
    catalog::GameId game;
    /// CoverItem's title, fallbackTitle, year and coverPath.
    std::string title;
    std::string fallbackTitle;
    std::string year;
    std::string coverPath;
    /// world::CollectionTitle::available.
    bool available = true;
    /// The hero CRT's data (none for a game the snapshot did not list, or that ES-DE does not show).
    std::optional<RoomItemScreen> screen;

    bool operator==(const RoomItemText&) const = default;
};

struct RoomItems {
    /// The system's full name (CoverItem::systemName).
    std::string systemName;
    /// The system's ES-DE platform tags (RoomSnapshotResult::platformTags): the preview's scene classifies a system
    /// systems.json does not name by them, as the snapshot's does. A file without them (I3's) reads as none.
    std::vector<std::string> platforms;
    /// By GameId, each game once.
    std::vector<RoomItemText> items;

    /// The item of `game`, or nullptr.
    [[nodiscard]] const RoomItemText* find(const catalog::GameId& game) const noexcept;

    bool operator==(const RoomItems&) const = default;
};

/// The items of `covers` (the room's cover requests, world_build.hpp), available unless `layout`'s item of the game is
/// flagged scene::instance_flags::kUnavailable, sorted by GameId; the system's name is the first cover's.
[[nodiscard]] RoomItems roomItemsOf(std::span<const CoverItem> covers, const world::CollectionLayout& layout);

/// The titles of `plan`'s placed games as world::collectionLayout takes them (world::collectionTitles' form: by GameId,
/// with their availability): `items`' text of each, and for a game `items` does not know no title and available (so
/// that its instance flags stay those of the reconciled room until the snapshot says otherwise).
[[nodiscard]] std::vector<world::CollectionTitle> collectionTitlesOf(const RoomItems& items,
                                                                     const world::WorldPlan& plan);

/// <cacheDir>/world/<library>/room-<system>.json (cacheDir: core::Paths::cacheDir()).
[[nodiscard]] std::filesystem::path roomItemsFile(const std::filesystem::path& cacheDir,
                                                  const catalog::LibraryId& library, const catalog::SystemName& system);

/// {"schemaVersion": 1, "systemName": "...", "platforms": ["..."], "items": [{"game", "title", "fallbackTitle", "year",
/// "coverPath", "available", "screen": {"video", "screenshot", "titleScreen", "lastPlayed", "sortKey", "favorite",
/// "playCount", "rating"}}, ...]}; "platforms" only when there are any, "screen" only for an item that has it (its
/// "lastPlayed" and "rating" only when known).
[[nodiscard]] nlohmann::json roomItemsToJson(const RoomItems& items);
/// Errors (ParseError, context "<context>:<JSON pointer>"): not that shape ("platforms" and "screen" optional), a newer
/// schemaVersion, a GameId that does not parse, a game listed twice or out of order.
[[nodiscard]] Expected<RoomItems> parseRoomItems(const nlohmann::json& document, std::string_view context);

/// Reads `file`. Errors: NotFound when there is none, those of the read and of parseRoomItems().
[[nodiscard]] Expected<RoomItems> readRoomItems(const std::filesystem::path& file);
/// Writes `items` to `file` atomically (core::writeTextFileAtomic, the folders made) unless it holds exactly that text
/// already. Returns whether it wrote. Errors: those of the write.
[[nodiscard]] Expected<bool> writeRoomItems(const std::filesystem::path& file, const RoomItems& items);

} // namespace rv::app
