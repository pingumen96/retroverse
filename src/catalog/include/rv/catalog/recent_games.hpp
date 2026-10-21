#pragma once

// The directory's favourites and recently played games (I4 step 7a; ADR-0013 §10): one list across systems, read in
// one query on the catalog worker (ICatalogQuery::favoritesAndRecent, SqliteCatalogQuery's), ordered here.
//
// The rule:
//  - Candidates: the entries that are games (EntryState::Game: never NotAGame, MissingFile, DiscMember or Folder), not
//    marked <hidden> in their gamelist, Available unless the query includes unavailable ones (FR-033), and either
//    favourites (ES-DE's <favorite>) or played: a session of Retroverse's that started the game (every outcome but
//    failed-to-start and blocked, launch tests left out: the room snapshot's rule) or ES-DE's <lastplayed>.
//  - When a game was last played: the later of its last such session's start (UTC, as rv_play_sessions keeps it) and
//    ES-DE's <lastplayed> converted to UTC. ES-DE writes <lastplayed> as the machine's local wall time, without a zone
//    (metadata.hpp's LocalDateTime); it is read as a time of the machine's time zone now, with that zone's rules at
//    that date (machineLocalTimeToUtc(): DST included). A wall time the clocks showed twice (the hour repeated when
//    they go back) is its earlier instant; one they skipped (when they go forward) is the instant of the change. Those
//    are std::chrono::choose::earliest's answers.
//  - Order (recentGameBefore()): favourites first; within favourites and within the others, the most recently played
//    first and the never played last; then the sort key (kSortKeyVersion's), then the GameId: a total order, the same
//    on every run.
//  - The first `limit` of that order. A library with more favourites than the limit lists favourites only, the most
//    recently played of them first.

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

using UtcTime = std::chrono::system_clock::time_point;

/// ES-DE's <lastplayed> (a local wall time) as a UTC instant; nothing for a time the converter cannot place (a date
/// outside the zone database's range).
using LocalTimeToUtc = std::function<std::optional<UtcTime>(const LocalDateTime&)>;

/// The converter of `zone`, which must outlive it: the zone's rules at that date, an ambiguous wall time its earlier
/// instant, a skipped one the instant of the change (std::chrono::time_zone::to_sys with choose::earliest, to the
/// second). It keeps the zone's intervals it has read (two a year where the zone has daylight saving time), so that a
/// conversion costs a search, not a query of the zone database: one converter per thread.
[[nodiscard]] LocalTimeToUtc zoneLocalTimeToUtc(const std::chrono::time_zone& zone);
/// zoneLocalTimeToUtc() of the machine's time zone (std::chrono::current_zone()). Errors: Unavailable when the time
/// zone database cannot be read (the caller then decides: fixedOffsetLocalTimeToUtc(0) reads ES-DE's times as UTC).
[[nodiscard]] Expected<LocalTimeToUtc> machineLocalTimeToUtc();
/// A zone of a fixed offset east of UTC (UTC+01:00 is +60 minutes): no daylight saving time. For tests, and the
/// fallback above.
[[nodiscard]] LocalTimeToUtc fixedOffsetLocalTimeToUtc(std::chrono::minutes offset);

/// The directory's list asks for at most this many games by default.
inline constexpr std::size_t kDefaultRecentGames = 30;

/// What the directory's favourites and recent games tab asks for.
struct RecentGamesQuery {
    /// At most this many games (0: none).
    std::size_t limit = kDefaultRecentGames;
    /// How ES-DE's <lastplayed> becomes UTC (machineLocalTimeToUtc() in the app); required.
    LocalTimeToUtc localToUtc;
    /// Also games that are not Available (a folder that could not be listed); off as in the quick catalog's lists.
    bool includeUnavailable = false;
};

/// A game of the list.
struct RecentGame {
    GameId id;
    SystemName system;
    /// The system's <fullname> ("" when the catalog has no row for the system).
    std::string systemFullName;
    std::string title;
    /// The entry's sort key (kSortKeyVersion's).
    std::string sortKey;
    /// ES-DE's <favorite>.
    bool favorite = false;
    /// The start of Retroverse's last session that started the game (UTC).
    std::optional<UtcTime> lastSession;
    /// ES-DE's <lastplayed>, as written (local wall time).
    std::optional<LocalDateTime> esdeLastPlayed;
    /// When the game was last played: the later of lastSession and esdeLastPlayed converted to UTC; nothing for a
    /// favourite never played (or whose <lastplayed> did not convert).
    std::optional<UtcTime> lastPlayed;

    bool operator==(const RecentGame&) const = default;
};

/// The list's order (see above): favourites first, then lastPlayed descending (nothing last), the sort key, the GameId.
[[nodiscard]] bool recentGameBefore(const RecentGame& a, const RecentGame& b) noexcept;

/// Sets every candidate's lastPlayed from its lastSession and esdeLastPlayed (through `localToUtc`, which may be empty:
/// <lastplayed> then counts for nothing), orders them by recentGameBefore() and keeps the first `limit`.
void rankRecentGames(std::vector<RecentGame>& candidates, const LocalTimeToUtc& localToUtc, std::size_t limit);

} // namespace rv::catalog
