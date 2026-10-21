#pragma once

// RoomSelection (ADR-0010 §3): the generator's input for one room, which selectRoom() (I3 step 2) makes from a
// RoomInput: the eligible titles, one representative each, in priority order, and every other game of the system with
// the reason it is not placed. The generator places the titles and copies the unplaced games into the plan.
//
// Its canonical JSON (core::writeCanonicalJson) is what the plan's inputDigest hashes (with the configuration and the
// data files, I3 step 3) and what tests compare; it holds the catalog's sort keys and the title keys, so it is never
// written into a plan or under the user's data:
//   {"catalogSortKeyVersion": 1, "distinctTitles": 1, "eligibility": "covered", "room": "room:snes",
//    "titleKeyRule": "title-key/1",
//    "titles": [{"game": "g...", "titleKey": "...", "sortKey": "...", "favorite": true, "playCount": 12,
//                "rating": 8}, ...],
//    "unplaced": [{"game": "g...", "room": "room:snes", "reason": "duplicate-title", "representative": "g..."}]}
// "rating" is null for an unrated game. The titles are in priority order (priorityLess()), no game twice; the unplaced
// games as in a plan (sorted, "representative" exactly for duplicate-title), all of the selection's room;
// "distinctTitles" (I4 step 4) at least the titles' count.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// Which games of the system were eligible (ADR-0010 §3).
enum class Eligibility : std::uint8_t {
    Covered,         ///< the games with a cover (WorldConfig's include "covered", the default)
    All,             ///< every game (include "all")
    NoCoverInSystem, ///< every game, because no game of the system has a cover (known media only: style §21's P0 room)
};

/// "covered", "all", "no-cover-in-system".
[[nodiscard]] constexpr std::string_view toString(Eligibility eligibility) noexcept {
    switch (eligibility) {
    case Eligibility::Covered:
        return "covered";
    case Eligibility::All:
        return "all";
    case Eligibility::NoCoverInSystem:
        return "no-cover-in-system";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<Eligibility> parseEligibility(std::string_view text) noexcept {
    return detail::enumFromString(text, Eligibility::NoCoverInSystem);
}

/// A title the room shows, by its representative, with what its priority and its place in the alphabet follow from.
struct RoomTitle {
    catalog::GameId game;
    /// The title key that grouped its versions (title_key.hpp).
    std::string titleKey;
    /// The representative's catalog sort key (catalog::sortKeyOf, RoomSelection::catalogSortKeyVersion): the order of
    /// titles within a section (ADR-0010 §6).
    std::string sortKey;
    /// The priority's data (ADR-0010 §3), of the title's eligible versions (selectRoom()): ES-DE's favourite, the play
    /// count (ES-DE's plus Retroverse's sessions) and the rating in tenths (0 to 10), nothing for an unrated title.
    bool favorite = false;
    std::uint64_t playCount = 0;
    std::optional<int> ratingTenths;

    bool operator==(const RoomTitle&) const = default;
};

/// The priority order of ADR-0010 §3: favourites first, then the higher play count, then the higher rating (a rated
/// title before an unrated one), then the catalog sort key, then the GameId. A strict total order on distinct games.
[[nodiscard]] bool priorityLess(const RoomTitle& a, const RoomTitle& b) noexcept;

/// One room's selection.
struct RoomSelection {
    RoomId room;
    Eligibility eligibility = Eligibility::Covered;
    std::string titleKeyRule;
    std::uint32_t catalogSortKeyVersion = 0;
    /// The titles in priority order (priorityLess()): the first ones stand face-out (ADR-0010 §3).
    std::vector<RoomTitle> titles;
    /// The other games of the system, sorted as a plan sorts them.
    std::vector<UnplacedGame> unplaced;
    /// The distinct titles over every game of the system the room could place, with or without a cover: the titles
    /// selectRoom() gives under include "all" (ADR-0013 §3, I4 step 4). It decides whether a system entering the
    /// building gets a room of its own (WorldConfig::sharedRoomMinTitles), so that it measures the collection, not the
    /// scraping; at least titles.size().
    std::uint64_t distinctTitles = 0;

    bool operator==(const RoomSelection&) const = default;
};

/// The room's selection from the catalog's snapshot of its system (ADR-0010 §3, I3 step 2):
///  - the games: the entries in state Game or MissingFile that ES-DE does not hide (EsdeFlags::hidden); NotAGame,
///    DiscMember and Folder entries are no games, a hidden game is left out as ES-DE leaves it out, and none of them is
///    listed;
///  - a MissingFile entry (its file gone) is unplaced, reason Unavailable. A game whose folder could not be listed
///    (Availability::SourceUnavailable) is not known to be gone and is selected like the others;
///  - eligibility: with config.include All every other game is eligible (Eligibility::All); else the games with a cover
///    are (Covered) and the others unplaced, reason NoCover; when no game has a cover (MissingFile ones included),
///    every game is eligible (NoCoverInSystem: style §21's P0 room, golden G06's single ROM without media);
///  - one title per title key (title_key.hpp) of the eligible games' titles; its representative is the first of its
///    versions by their file names' tags (versionTagsLess()), then the lookup key and the GameId; the other versions
///    are unplaced, reason DuplicateTitle, with the representative;
///  - each title's priority data is its versions': a favourite when one of them is, the play counts (ES-DE's plus
///    Retroverse's sessions) summed, the highest rating; its sort key is the representative's. The titles are in
///    priority order (priorityLess()), the unplaced games sorted as a plan sorts them;
///  - distinctTitles: the title keys of every game that is not MissingFile, with a cover or not (the titles under
///    include "all"; a game without a cover whose title has no title key counts as a title of its own).
/// Nothing is decided from unknown media: while the snapshot's mediaKnown is false there is no selection. Errors:
/// Unavailable when the media are not known, its message holding RoomSnapshot::mediaUnknownReason; InvalidArgument
/// for a snapshot without a system or with a GameId twice; those of titleKey() (a title that is not UTF-8), naming the
/// game.
[[nodiscard]] Expected<RoomSelection> selectRoom(const RoomInput& input, const WorldConfig& config);

/// The canonical document of `selection` (titles kept in their order, unplaced games sorted). Errors: InvalidArgument
/// when it holds what readRoomSelection() would refuse, naming the field.
[[nodiscard]] Expected<nlohmann::json> roomSelectionToJson(const RoomSelection& selection);
/// Its canonical text. Errors: those of roomSelectionToJson().
[[nodiscard]] Expected<std::string> writeRoomSelection(const RoomSelection& selection);
/// Reads a canonical text back. Errors (context and line): ParseError, ValidationFailed (not canonical, a field
/// missing, unknown, of another type or range, the titles out of priority order or a game twice, an unplaced game of
/// another room, out of order or twice, fewer distinct titles than titles).
[[nodiscard]] Expected<RoomSelection> readRoomSelection(std::string_view text, std::string_view context);

} // namespace rv::world
