#pragma once

// What a person played (ADR-0018 §9, specification §13.4): the sessions Retroverse recorded, its estimated hours, the most
// played and most recent games, per system and per status. Pure: SqliteCatalogQuery::playStatistics() reads the rows in
// one statement (stats_query.cpp), buildPlayStatistics() adds them up.
//
// What counts:
//  - Sessions and hours: `Returned` sessions, by their own start and end: estimated, from the launch to the detected
//    return (a game left in a menu counts). A clock moved back counts for nothing, never for less.
//  - Launch tests never count.
//  - `Crashed` and `LeftRunning` sessions are attempts, listed apart and not added to hours: their end is not the game's.
//    `Interrupted`, `Blocked` and `FailedToStart` are neither.
//  - Everything comes from rv_play_sessions alone: ES-DE's <playcount> and <playtime> are another program's count of other
//    launches. ES-DE's <lastplayed> stays in the recent games as it is (recent_games.hpp's rule, favourites not first).
//  - A game the library lost keeps its hours (under its system, its path for a title), and leaves the statuses and the
//    recent games, which are about the games the library has. A volume that went away changes nothing: availability is
//    not looked at.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rv/catalog/game_status.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/recent_games.hpp>

namespace rv::catalog {

/// How many games each list holds by default.
inline constexpr std::size_t kDefaultStatisticsGames = 10;

/// What the screen and `retroverse-cli stats` ask for.
struct PlayStatisticsQuery {
    /// The most played and the most recent, at most this many each.
    std::size_t limit = kDefaultStatisticsGames;
    /// How ES-DE's <lastplayed> becomes UTC (machineLocalTimeToUtc() in the app); required.
    LocalTimeToUtc localToUtc;
};

/// One game as the statement reads it.
struct PlayStatisticsRow {
    GameId id;
    SystemName system;
    /// The system's <fullname> ("" when the catalog has no row for it).
    std::string systemFullName;
    /// Its title, its relative path when the index holds no row for it.
    std::string title;
    std::string sortKey;
    /// A game of the library: state Game, not hidden. Only such rows count in the statuses and the recent games.
    bool listed = false;
    bool esdeCompleted = false;
    std::optional<GameStatus> stored;
    std::optional<LocalDateTime> esdeLastPlayed;
    /// `Returned`, not tests.
    std::uint64_t sessions = 0;
    /// `Crashed` and `LeftRunning`, not tests.
    std::uint64_t crashed = 0;
    std::uint64_t leftRunning = 0;
    std::chrono::milliseconds played{0};
    /// The start of its last session that started the game (recent_games.hpp's rule).
    std::optional<UtcTime> lastSession;
};

/// A game of a list.
struct PlayedGame {
    GameId id;
    SystemName system;
    std::string systemFullName;
    std::string title;
    std::uint64_t sessions = 0;
    std::chrono::milliseconds played{0};
    /// When it was last played: the later of its last session and ES-DE's <lastplayed> in the recent games; its last
    /// session in the most played.
    std::optional<UtcTime> lastPlayed;
    GameStatus status = GameStatus::NotStarted;

    bool operator==(const PlayedGame&) const = default;
};

struct SystemStatistics {
    SystemName system;
    std::string fullName;
    std::uint64_t sessions = 0;
    std::uint64_t attempts = 0;
    std::chrono::milliseconds played{0};
    /// Games with at least one Returned session.
    std::uint64_t games = 0;

    bool operator==(const SystemStatistics&) const = default;
};

struct StatusStatistics {
    GameStatus status = GameStatus::NotStarted;
    /// The library's games in this status.
    std::uint64_t games = 0;
    std::uint64_t sessions = 0;
    std::chrono::milliseconds played{0};

    bool operator==(const StatusStatistics&) const = default;
};

struct PlayStatistics {
    std::uint64_t sessions = 0;
    std::uint64_t crashed = 0;
    std::uint64_t leftRunning = 0;
    /// Estimated: the sum of the Returned sessions.
    std::chrono::milliseconds played{0};
    /// Games with at least one Returned session.
    std::uint64_t gamesPlayed = 0;
    /// The library's games, the statuses' total.
    std::uint64_t games = 0;
    /// By time played, then sessions, the sort key, the GameId.
    std::vector<PlayedGame> mostPlayed;
    /// The latest first, as recent_games.hpp orders the others.
    std::vector<PlayedGame> mostRecent;
    /// Those with a session or an attempt: by time played, then sessions, the name.
    std::vector<SystemStatistics> systems;
    /// One per status, in GameStatus's order.
    std::array<StatusStatistics, kGameStatuses> statuses{};

    [[nodiscard]] std::uint64_t attempts() const noexcept { return crashed + leftRunning; }
    /// Nothing was played yet: the screen says so.
    [[nodiscard]] bool empty() const noexcept { return sessions == 0 && attempts() == 0 && mostRecent.empty(); }

    bool operator==(const PlayStatistics&) const = default;
};

/// `rows` added up, `query`'s lists kept.
[[nodiscard]] PlayStatistics buildPlayStatistics(const std::vector<PlayStatisticsRow>& rows,
                                                 const PlayStatisticsQuery& query);

} // namespace rv::catalog
