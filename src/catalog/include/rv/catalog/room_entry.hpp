#pragma once

// The room's bulk snapshot (ADR-0010 §2, §12): one system's games as the world generator reads them, in one read on
// the catalog worker (ICatalogQuery::roomEntries, filled by rv_catalog_db in I3 step 2), never a page per game. It
// holds what the room's selection and order need (identity, keys, title, sort key, the priority's data, state, media)
// and whether the system's media are known at all (ADR-0008 §5: unknown media are never "no media"). Text is used to
// decide and never copied into a plan (ADR-0010 §8). Header only.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/metadata.hpp>

namespace rv::catalog {

/// One game of a system as the room's snapshot gives it.
struct RoomEntry {
    GameId id;
    /// Its lookup key and exact relative path (the file name is the last element): the title-key rule reads the region
    /// and status tags from the file name, the order of ties ends with the key.
    LookupKey key;
    /// A game that is a folder (FileFacts::directory): its media are named after the whole name (mediaStem()).
    bool directory = false;
    /// CatalogEntry::title: the gamelist's <name>, else the title of the file name.
    std::string title;
    /// sortKeyOf(entry), version RoomSnapshot::sortKeyVersion.
    std::string sortKey;
    /// The release date's year, when the gamelist gives a date.
    std::optional<int> year;
    std::optional<Rating> rating;
    /// ES-DE's <favorite>, <hidden> and <playcount> (EsdeFlags).
    bool favorite = false;
    bool hidden = false;
    std::uint64_t esdePlayCount = 0;
    /// Retroverse's play sessions of the game that started it (PlaySession; ES-DE's count is apart, since ES-DE does
    /// not see launches from Retroverse), and when the last one started.
    std::uint64_t sessionCount = 0;
    std::optional<std::chrono::system_clock::time_point> lastSession;
    /// Game for what ES-DE shows; DiscMember, MissingFile and the other states as the catalog keeps them.
    EntryState state = EntryState::Game;
    Availability availability = Availability::Available;
    /// The media files the catalog found for it (presence and extension per kind); meaningful only while
    /// RoomSnapshot::mediaKnown.
    EntryMedia media;

    bool operator==(const RoomEntry&) const = default;
};

/// One system's games for its room.
struct RoomSnapshot {
    SystemName system;
    /// The version of the sort keys (kSortKeyVersion of the build that computed them).
    std::uint32_t sortKeyVersion = kSortKeyVersion;
    /// False when the system's media cannot be known: the media root is missing, unreachable or unresolved (ADR-0008
    /// §5). Then no entry's media say anything, and the world decides nothing from them (ADR-0010 §3).
    bool mediaKnown = false;
    /// Why the media are not known, for the notice and the log (empty while mediaKnown).
    std::string mediaUnknownReason;
    /// Every entry of the system the catalog keeps, games or not, by GameId.
    std::vector<RoomEntry> entries;

    bool operator==(const RoomSnapshot&) const = default;
};

} // namespace rv::catalog
