#pragma once

// RoomSelection (ADR-0010 §3): the generator's input for one room, which world::selectRoom (I3 step 2) makes from a
// RoomInput: the eligible titles, one representative each, in priority order, and every other game of the system with
// the reason it is not placed. The generator places the titles and copies the unplaced games into the plan.
//
// Its canonical JSON (core::writeCanonicalJson) is what the plan's inputDigest hashes (with the configuration and the
// data files, I3 step 3) and what tests compare; it holds the catalog's sort keys and the title keys, so it is never
// written into a plan or under the user's data:
//   {"catalogSortKeyVersion": 1, "eligibility": "covered", "room": "room:snes", "titleKeyRule": "title-key/1",
//    "titles": [{"game": "g...", "titleKey": "...", "sortKey": "...", "favorite": true, "playCount": 12,
//                "rating": 8}, ...],
//    "unplaced": [{"game": "g...", "room": "room:snes", "reason": "duplicate-title", "representative": "g..."}]}
// "rating" is null for an unrated game. The titles are in priority order (priorityLess()), no game twice; the unplaced
// games as in a plan (sorted, "representative" exactly for duplicate-title), all of the selection's room.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
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
    /// The priority's data (ADR-0010 §3): ES-DE's favourite, the play count (ES-DE's plus Retroverse's sessions) and
    /// the rating in tenths (0 to 10), nothing for an unrated game.
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

    bool operator==(const RoomSelection&) const = default;
};

/// The canonical document of `selection` (titles kept in their order, unplaced games sorted). Errors: InvalidArgument
/// when it holds what readRoomSelection() would refuse, naming the field.
[[nodiscard]] Expected<nlohmann::json> roomSelectionToJson(const RoomSelection& selection);
/// Its canonical text. Errors: those of roomSelectionToJson().
[[nodiscard]] Expected<std::string> writeRoomSelection(const RoomSelection& selection);
/// Reads a canonical text back. Errors (context and line): ParseError, ValidationFailed (not canonical, a field
/// missing, unknown, of another type or range, the titles out of priority order or a game twice, an unplaced game of
/// another room, out of order or twice).
[[nodiscard]] Expected<RoomSelection> readRoomSelection(std::string_view text, std::string_view context);

} // namespace rv::world
