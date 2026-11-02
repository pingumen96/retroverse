// SqliteCatalogQuery::playStatistics and ::gameStatus (I7 step 6; ADR-0018 §9): one statement on the caller's connection
// (one snapshot). `played` groups rv_play_sessions per game on rv_play_sessions_game; every game of the library is then
// read once, for the statuses, with the identities the sessions name that the library lost. catalog::buildPlayStatistics()
// adds the rows up, so the rules live in one place. Estimated time is ended_at less started_at, both UTC ISO-8601 text as
// julianday() reads it, rounded to the millisecond and never below zero (a clock moved back). The statement is prepared
// per call, as the room snapshot's are.

#include <rv/catalog_db/catalog_query.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog/statistics.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/time.hpp>

namespace rv::catalog_db {
namespace {

using catalog::GameId;
using catalog::PlaySessionOutcome;

constexpr std::string_view kReturned = catalog::toString(PlaySessionOutcome::Returned);

std::string statisticsSql() {
    return fmt::format(
        "WITH played AS (SELECT game, "
        "sum(outcome = '{returned}') AS sessions, "
        "sum(outcome = '{crashed}') AS crashed, "
        "sum(outcome = '{left}') AS left_running, "
        "sum(CASE WHEN outcome = '{returned}' THEN max(0, CAST(round((julianday(ended_at) - julianday(started_at)) "
        "* 86400000.0) AS INTEGER)) ELSE 0 END) AS played_ms, "
        "max(CASE WHEN outcome NOT IN ('{failed}', '{blocked}') THEN started_at END) AS last_session "
        "FROM rv_play_sessions WHERE test = 0 GROUP BY game) "
        "SELECT i.id, i.system, coalesce(s.full_name, ''), coalesce(g.title, i.relative_path), "
        "coalesce(g.sort_key, ''), coalesce(g.state = '{game}' AND g.hidden = 0, 0), coalesce(g.completed, 0), "
        "g.last_played, st.status, coalesce(p.sessions, 0), coalesce(p.crashed, 0), coalesce(p.left_running, 0), "
        "coalesce(p.played_ms, 0), p.last_session "
        "FROM rv_game_identities AS i "
        "LEFT JOIN idx_games AS g ON g.id = i.id "
        "LEFT JOIN idx_systems AS s ON s.library = i.library AND s.name = i.system "
        "LEFT JOIN rv_game_status AS st ON st.game = i.id "
        "LEFT JOIN played AS p ON p.game = i.id "
        "WHERE (g.state = '{game}' AND g.hidden = 0) OR p.game IS NOT NULL",
        fmt::arg("returned", kReturned), fmt::arg("crashed", catalog::toString(PlaySessionOutcome::Crashed)),
        fmt::arg("left", catalog::toString(PlaySessionOutcome::LeftRunning)),
        fmt::arg("failed", catalog::toString(PlaySessionOutcome::FailedToStart)),
        fmt::arg("blocked", catalog::toString(PlaySessionOutcome::Blocked)),
        fmt::arg("game", catalog::toString(catalog::EntryState::Game)));
}

Expected<catalog::PlayStatisticsRow> readRow(const Statement& row) {
    const std::int64_t value = row.columnInt(0);
    const auto id = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
    if (!id) {
        return makeError(ErrorCode::ParseError, fmt::format("the catalog database holds a GameId that is none: {}", value),
                         "rv_game_identities");
    }
    auto system = catalog::SystemName::parse(row.columnText(1));
    if (!system) {
        return makeError(ErrorCode::ParseError, "the catalog database holds a system name that is none",
                         fmt::format("rv_game_identities row {}", id->value()));
    }
    catalog::PlayStatisticsRow read;
    read.id = *id;
    read.system = std::move(*system);
    read.systemFullName = std::string(row.columnText(2));
    read.title = std::string(row.columnText(3));
    read.sortKey = std::string(row.columnText(4));
    read.listed = row.columnInt(5) != 0;
    read.esdeCompleted = row.columnInt(6) != 0;
    // What cannot be read back counts as absent here: a statistic is no reason to refuse a whole catalog.
    if (!row.isNull(7)) {
        if (const auto lastPlayed = catalog::parseEsdeDateTime(row.columnText(7))) {
            read.esdeLastPlayed = *lastPlayed;
        }
    }
    if (!row.isNull(8)) {
        read.stored = catalog::parseGameStatus(row.columnText(8));
    }
    read.sessions = static_cast<std::uint64_t>(row.columnInt(9));
    read.crashed = static_cast<std::uint64_t>(row.columnInt(10));
    read.leftRunning = static_cast<std::uint64_t>(row.columnInt(11));
    read.played = std::chrono::milliseconds(row.columnInt(12));
    if (!row.isNull(13)) {
        if (const auto lastSession = core::parseUtcIso8601(row.columnText(13))) {
            read.lastSession = *lastSession;
        }
    }
    return read;
}

} // namespace

Expected<catalog::PlayStatistics> SqliteCatalogQuery::playStatistics(const catalog::PlayStatisticsQuery& query) {
    if (!query.localToUtc) {
        return makeError(ErrorCode::InvalidArgument, "the query needs the conversion of ES-DE's local times to UTC");
    }
    auto statement = connection().prepare(statisticsSql());
    if (!statement) {
        return propagate(statement.error());
    }
    std::vector<catalog::PlayStatisticsRow> rows;
    {
        const ScopedReset reset(*statement);
        while (true) {
            const auto stepped = statement->step();
            if (!stepped) {
                return propagate(stepped.error());
            }
            if (!*stepped) {
                break;
            }
            auto row = readRow(*statement);
            if (!row) {
                return propagate(row.error());
            }
            rows.push_back(std::move(*row));
        }
    }
    return catalog::buildPlayStatistics(rows, query);
}

Expected<std::optional<catalog::GameStatusState>> SqliteCatalogQuery::gameStatus(catalog::GameId game) {
    auto statement = connection().prepare(fmt::format(
        "SELECT st.status, coalesce(g.completed, 0), EXISTS (SELECT 1 FROM rv_play_sessions AS p WHERE p.game = i.id "
        "AND p.test = 0 AND p.outcome = '{}') "
        "FROM rv_game_identities AS i "
        "LEFT JOIN idx_games AS g ON g.id = i.id "
        "LEFT JOIN rv_game_status AS st ON st.game = i.id "
        "WHERE i.id = coalesce((SELECT a.game FROM rv_identity_aliases AS a WHERE a.alias = ?1), ?1)",
        kReturned));
    if (!statement) {
        return propagate(statement.error());
    }
    statement->bindInt(1, static_cast<std::int64_t>(game.value()));
    const ScopedReset reset(*statement);
    const auto stepped = statement->step();
    if (!stepped) {
        return propagate(stepped.error());
    }
    if (!*stepped) {
        return std::optional<catalog::GameStatusState>();
    }
    const std::optional<catalog::GameStatus> stored =
        statement->isNull(0) ? std::nullopt : catalog::parseGameStatus(statement->columnText(0));
    return std::optional<catalog::GameStatusState>(catalog::GameStatusState{
        catalog::gameStatusOf(stored, statement->columnInt(1) != 0, statement->columnInt(2) != 0), stored.has_value()});
}

} // namespace rv::catalog_db
