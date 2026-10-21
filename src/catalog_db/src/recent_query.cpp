// SqliteCatalogQuery::favoritesAndRecent (I4 step 7a; ADR-0013 §10): the directory's favourites and recently played
// games, one statement on the caller's connection (one snapshot), every library the catalog holds. The candidates are
// read whole (idx_games has no index on the favourite flag or <lastplayed>, so the statement visits every row once;
// the sessions' last starts come from rv_play_sessions_game, one group per played game) and ranked in memory by
// catalog::rankRecentGames(), which converts ES-DE's local <lastplayed> to UTC. The statement is prepared per call, as
// the room snapshot's are: microseconds, against the read's milliseconds.

#include <rv/catalog_db/catalog_query.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/time.hpp>

namespace rv::catalog_db {
namespace {

using catalog::GameId;
using catalog::RecentGame;

tl::unexpected<Error> unreadable(std::string_view what, std::string context) {
    return makeError(ErrorCode::ParseError, fmt::format("the catalog database holds {}", what), std::move(context));
}

/// The candidates, `?1` 1 to include entries that are not Available. The sessions are the room snapshot's: those that
/// started the game (every outcome but a failure to start and a launch blocked before it), launch tests left out.
std::string candidatesSql() {
    return fmt::format(
        "SELECT g.id, g.system, coalesce(s.full_name, ''), g.title, g.sort_key, g.favorite, g.last_played, p.last "
        "FROM idx_games AS g "
        "LEFT JOIN (SELECT game, max(started_at) AS last FROM rv_play_sessions "
        "WHERE test = 0 AND outcome NOT IN ('failed-to-start', 'blocked') GROUP BY game) AS p ON p.game = g.id "
        "LEFT JOIN idx_systems AS s ON s.library = g.library AND s.name = g.system "
        "WHERE g.state = '{}' AND g.hidden = 0 AND (?1 = 1 OR g.availability = '{}') "
        "AND (g.favorite = 1 OR g.last_played IS NOT NULL OR p.last IS NOT NULL)",
        catalog::toString(catalog::EntryState::Game), catalog::toString(catalog::Availability::Available));
}

Expected<RecentGame> readCandidate(const Statement& row) {
    const std::int64_t value = row.columnInt(0);
    const auto id = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
    if (!id) {
        return unreadable(fmt::format("a row whose GameId {} is none", value), "idx_games");
    }
    const std::string context = fmt::format("idx_games row {}", id->value());
    RecentGame game;
    game.id = *id;
    auto system = catalog::SystemName::parse(row.columnText(1));
    if (!system) {
        return unreadable(fmt::format("a system name that is none ({})", system.error().message), context);
    }
    game.system = std::move(*system);
    game.systemFullName = std::string(row.columnText(2));
    game.title = std::string(row.columnText(3));
    game.sortKey = std::string(row.columnText(4));
    game.favorite = row.columnInt(5) != 0;
    if (!row.isNull(6)) {
        const auto lastPlayed = catalog::parseEsdeDateTime(row.columnText(6));
        if (!lastPlayed) {
            return unreadable(fmt::format("a last played time that is none ({})", lastPlayed.error().message), context);
        }
        game.esdeLastPlayed = *lastPlayed;
    }
    if (!row.isNull(7)) {
        const auto lastSession = core::parseUtcIso8601(row.columnText(7));
        if (!lastSession) {
            return unreadable(fmt::format("a session start that is no time ({})", lastSession.error().message),
                              fmt::format("rv_play_sessions of {}", id->value()));
        }
        game.lastSession = *lastSession;
    }
    return game;
}

} // namespace

Expected<std::vector<RecentGame>> SqliteCatalogQuery::favoritesAndRecent(const catalog::RecentGamesQuery& query) {
    if (!query.localToUtc) {
        return makeError(ErrorCode::InvalidArgument, "the query needs the conversion of ES-DE's local times to UTC");
    }
    std::vector<RecentGame> candidates;
    if (query.limit == 0) {
        return candidates;
    }
    auto statement = connection().prepare(candidatesSql());
    if (!statement) {
        return propagate(statement.error());
    }
    statement->bindBool(1, query.includeUnavailable);
    {
        const ScopedReset reset(*statement);
        while (true) {
            const auto row = statement->step();
            if (!row) {
                return propagate(row.error());
            }
            if (!*row) {
                break;
            }
            auto game = readCandidate(*statement);
            if (!game) {
                return propagate(game.error());
            }
            candidates.push_back(std::move(*game));
        }
    }
    catalog::rankRecentGames(candidates, query.localToUtc, query.limit);
    return candidates;
}

} // namespace rv::catalog_db
