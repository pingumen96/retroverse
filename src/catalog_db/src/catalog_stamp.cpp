#include <rv/catalog_db/catalog_stamp.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

#include <fmt/format.h>

#include <rv/core/time.hpp>

namespace rv::catalog_db {
namespace {

/// Retroverse's play sessions that count for a game (room_query.cpp's kStartedSessions): no launch test, every outcome
/// but a failure to start or a launch blocked.
constexpr std::string_view kCountedSessions = "test = 0 AND outcome NOT IN ('failed-to-start', 'blocked')";

/// The first row of `sql` (its parameter ?1 bound to `parameter` when given) as "a|b|...", every column as text
/// ("null" for NULL).
Expected<std::string> rowText(Connection& connection, std::string_view sql, int columns,
                              std::optional<std::string_view> parameter = std::nullopt) {
    auto statement = connection.prepare(sql);
    if (!statement) {
        return propagate(statement.error());
    }
    const ScopedReset reset(*statement);
    if (parameter) {
        statement->bindText(1, *parameter);
    }
    auto row = statement->step();
    if (!row) {
        return propagate(row.error());
    }
    std::string text;
    for (int column = 0; column < columns; ++column) {
        if (column > 0) {
            text += '|';
        }
        if (!*row || statement->isNull(column)) {
            text += "null";
        } else {
            text += statement->columnText(column);
        }
    }
    return text;
}

} // namespace

Expected<std::string> catalogStamp(Connection& connection, const catalog::LibraryId& library) {
    auto imports = rowText(connection,
                           "SELECT id, finished_at FROM rv_imports WHERE library = ?1 ORDER BY id DESC LIMIT 1", 2,
                           library.str());
    if (!imports) {
        return propagate(imports.error());
    }
    // roomEntries' started sessions (room_query.cpp's kStartedSessions).
    auto sessions =
        rowText(connection, fmt::format("SELECT count(*), max(started_at) FROM rv_play_sessions WHERE {}", kCountedSessions),
                2);
    if (!sessions) {
        return propagate(sessions.error());
    }
    auto aliases = rowText(connection, "SELECT count(*), max(rowid) FROM rv_identity_aliases", 2);
    if (!aliases) {
        return propagate(aliases.error());
    }
    auto history = rowText(connection, "SELECT count(*), max(rowid) FROM rv_identity_history", 2);
    if (!history) {
        return propagate(history.error());
    }
    auto sortKeys = rowText(connection, "SELECT value FROM rv_meta WHERE key = 'sort_key_version'", 1);
    if (!sortKeys) {
        return propagate(sortKeys.error());
    }
    auto schema = connection.userVersion();
    if (!schema) {
        return propagate(schema.error());
    }
    return fmt::format("catalog-stamp/1;schema {};sort keys {};import {};sessions {};aliases {};history {}", *schema,
                       *sortKeys, *imports, *sessions, *aliases, *history);
}

Expected<std::map<catalog::GameId, std::chrono::system_clock::time_point>> lastPlaySessions(Connection& connection) {
    auto statement = connection.prepare(
        fmt::format("SELECT game, max(started_at) FROM rv_play_sessions WHERE {} GROUP BY game", kCountedSessions));
    if (!statement) {
        return propagate(statement.error());
    }
    const ScopedReset reset(*statement);
    std::map<catalog::GameId, std::chrono::system_clock::time_point> sessions;
    while (true) {
        auto row = statement->step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            return sessions;
        }
        const std::int64_t value = statement->columnInt(0);
        const auto game = catalog::GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
        if (!game) {
            return makeError(ErrorCode::ParseError,
                             fmt::format("the catalog database holds a play session whose GameId {} is none", value),
                             "rv_play_sessions");
        }
        auto started = core::parseUtcIso8601(statement->columnText(1));
        if (!started) {
            return makeError(ErrorCode::ParseError,
                             fmt::format("the catalog database holds a session start that is no time ({})",
                                         started.error().message),
                             fmt::format("rv_play_sessions of {}", game->toString()));
        }
        sessions.emplace(*game, *started);
    }
}

} // namespace rv::catalog_db
