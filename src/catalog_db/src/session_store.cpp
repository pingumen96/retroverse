#include <rv/catalog_db/session_store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

namespace rv::catalog_db {

using catalog::GameId;
using catalog::LeftRunningReason;
using catalog::LibraryId;
using catalog::PlaySession;
using catalog::PlaySessionOutcome;
using catalog::SystemName;

namespace {

// --- Statements ------------------------------------------------------------------------------------------------------

/// The identity a GameId parameter stands for, inside a statement: the game an alias names, else the id itself.
#define RV_RESOLVED_GAME(parameter)                                                                                    \
    "coalesce((SELECT game FROM rv_identity_aliases WHERE alias = " parameter "), " parameter ")"

/// The columns of a session, in the order readSession() reads them.
#define RV_SESSION_COLUMNS                                                                                             \
    "s.id, s.game, s.choice_kind, s.choice, s.choice_source, s.test, s.started_at, s.ended_at, s.closed_at, s.run_ms, " \
    "s.outcome, s.readiness, s.left_running, s.failure, s.exit_code, s.exit_kind, s.quick_exit, s.confirmed_by_user, " \
    "s.processes_remained, s.refused_by_guard, s.message, s.process_id, s.owner_pid, s.owner_created"

/// How many columns RV_SESSION_COLUMNS names.
constexpr int kSessionColumns = 24;

enum class Sql : std::size_t {
    InsertSession,
    UpdateSession,
    SelectSession,
    SelectOpenSessions,
    CloseInterrupted,
    SelectSessions,
    SelectGameSessions,
    SelectGameChoice,
    UpsertGameChoice,
    DeleteGameChoice,
    SelectGameStatus,
    UpsertGameStatus,
    SelectSystemChoices,
    SelectSystemChoice,
    UpsertSystemChoice,
    DeleteSystemChoice,
    SelectProgramChoices,
    UpsertProgramChoice,
    DeleteProgramChoice,
    UpsertVerification,
    SelectVerifications,
    ResolveGame,
    Count,
};

std::string_view sqlOf(Sql which) noexcept {
    switch (which) {
    case Sql::InsertSession:
        return "INSERT INTO rv_play_sessions (game, choice_kind, choice, choice_source, test, started_at, ended_at, "
               "closed_at, run_ms, outcome, readiness, left_running, failure, exit_code, exit_kind, quick_exit, "
               "confirmed_by_user, processes_remained, refused_by_guard, message, process_id, owner_pid, owner_created) "
               "VALUES (" RV_RESOLVED_GAME("?1") ", ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, "
               "?17, ?18, ?19, ?20, ?21, ?22, ?23)";
    case Sql::UpdateSession:
        return "UPDATE rv_play_sessions SET choice_kind = ?2, choice = ?3, choice_source = ?4, test = ?5, "
               "started_at = ?6, ended_at = ?7, closed_at = ?8, run_ms = ?9, outcome = ?10, readiness = ?11, "
               "left_running = ?12, failure = ?13, exit_code = ?14, exit_kind = ?15, quick_exit = ?16, "
               "confirmed_by_user = ?17, processes_remained = ?18, refused_by_guard = ?19, message = ?20, "
               "process_id = ?21 WHERE id = ?1";
    case Sql::SelectSession:
        return "SELECT " RV_SESSION_COLUMNS " FROM rv_play_sessions AS s WHERE s.id = ?1";
    case Sql::SelectOpenSessions:
        return "SELECT " RV_SESSION_COLUMNS " FROM rv_play_sessions AS s WHERE s.outcome = 'running' ORDER BY s.id";
    case Sql::CloseInterrupted:
        return "UPDATE rv_play_sessions SET outcome = 'interrupted', closed_at = ?2, message = ?3 "
               "WHERE id = ?1 AND outcome = 'running' AND owner_pid = ?4 AND owner_created = ?5";
    case Sql::SelectSessions:
        return "SELECT " RV_SESSION_COLUMNS ", i.library, i.system, i.relative_path, g.title "
               "FROM rv_play_sessions AS s JOIN rv_game_identities AS i ON i.id = s.game "
               "LEFT JOIN idx_games AS g ON g.id = s.game ORDER BY s.id DESC LIMIT ?1";
    case Sql::SelectGameSessions:
        return "SELECT " RV_SESSION_COLUMNS ", i.library, i.system, i.relative_path, g.title "
               "FROM rv_play_sessions AS s JOIN rv_game_identities AS i ON i.id = s.game "
               "LEFT JOIN idx_games AS g ON g.id = s.game WHERE s.game = " RV_RESOLVED_GAME("?1") " "
               "ORDER BY s.id DESC LIMIT ?2";
    case Sql::SelectGameChoice:
        return "SELECT kind, value, chosen_at FROM rv_launch_game_choices WHERE game = " RV_RESOLVED_GAME("?1");
    case Sql::UpsertGameChoice:
        return "INSERT INTO rv_launch_game_choices (game, kind, value, chosen_at) VALUES (" RV_RESOLVED_GAME("?1")
               ", ?2, ?3, ?4) ON CONFLICT (game) DO UPDATE SET kind = excluded.kind, value = excluded.value, "
               "chosen_at = excluded.chosen_at";
    case Sql::SelectGameStatus:
        return "SELECT status FROM rv_game_status WHERE game = " RV_RESOLVED_GAME("?1");
    case Sql::UpsertGameStatus:
        return "INSERT INTO rv_game_status (game, status, set_at) VALUES (" RV_RESOLVED_GAME("?1")
               ", ?2, ?3) ON CONFLICT (game) DO UPDATE SET status = excluded.status, set_at = excluded.set_at";
    case Sql::DeleteGameChoice:
        return "DELETE FROM rv_launch_game_choices WHERE game = " RV_RESOLVED_GAME("?1");
    case Sql::SelectSystemChoices:
        return "SELECT system, kind, value, chosen_at FROM rv_launch_system_choices WHERE library = ?1 ORDER BY system";
    case Sql::SelectSystemChoice:
        return "SELECT system, kind, value, chosen_at FROM rv_launch_system_choices WHERE library = ?1 AND system = ?2";
    case Sql::UpsertSystemChoice:
        return "INSERT INTO rv_launch_system_choices (library, system, kind, value, chosen_at) VALUES (?1, ?2, ?3, ?4, "
               "?5) ON CONFLICT (library, system) DO UPDATE SET kind = excluded.kind, value = excluded.value, "
               "chosen_at = excluded.chosen_at";
    case Sql::DeleteSystemChoice:
        return "DELETE FROM rv_launch_system_choices WHERE library = ?1 AND system = ?2";
    case Sql::SelectProgramChoices:
        return "SELECT emulator, program, chosen_at FROM rv_launch_program_choices WHERE library = ?1 ORDER BY emulator";
    case Sql::UpsertProgramChoice:
        return "INSERT INTO rv_launch_program_choices (library, emulator, program, chosen_at) VALUES (?1, ?2, ?3, ?4) "
               "ON CONFLICT (library, emulator) DO UPDATE SET program = excluded.program, chosen_at = excluded.chosen_at";
    case Sql::DeleteProgramChoice:
        return "DELETE FROM rv_launch_program_choices WHERE library = ?1 AND emulator = ?2";
    case Sql::UpsertVerification:
        return "INSERT INTO rv_launch_verifications (library, system, kind, value, game, session, test, program, "
               "verified_at) VALUES (?1, ?2, ?3, ?4, " RV_RESOLVED_GAME("?5") ", ?6, ?7, ?8, ?9) "
               "ON CONFLICT (library, system, kind, value) DO UPDATE SET game = excluded.game, "
               "session = excluded.session, test = excluded.test, program = excluded.program, "
               "verified_at = excluded.verified_at RETURNING id";
    case Sql::SelectVerifications:
        return "SELECT id, library, system, kind, value, game, session, test, program, verified_at "
               "FROM rv_launch_verifications WHERE library = ?1 ORDER BY system, kind, value";
    case Sql::ResolveGame:
        return "SELECT id FROM rv_game_identities WHERE id = ?1 "
               "UNION ALL SELECT game FROM rv_identity_aliases WHERE alias = ?1 LIMIT 1";
    case Sql::Count:
        break;
    }
    return {};
}

#undef RV_SESSION_COLUMNS
#undef RV_RESOLVED_GAME

// --- Values ----------------------------------------------------------------------------------------------------------

/// A value the database holds that the store cannot read back: a damaged or foreign row.
tl::unexpected<Error> unreadable(std::string_view what, std::string context = {}) {
    return makeError(ErrorCode::ParseError, fmt::format("the catalog database holds {}", what), std::move(context));
}

/// True when `error` is SQLite's report of a FOREIGN KEY constraint that failed.
bool isForeignKeyFailure(const Error& error) {
    return error.code == ErrorCode::InvalidArgument &&
           error.message.find("FOREIGN KEY constraint failed") != std::string::npos;
}

std::string timeText(std::chrono::system_clock::time_point time) {
    return core::formatUtcIso8601(time);
}

Expected<std::chrono::system_clock::time_point> readTime(std::string_view text, std::string_view column) {
    auto time = core::parseUtcIso8601(text);
    if (!time) {
        return unreadable(fmt::format("a time that is none in {}", column), std::string(text));
    }
    return time;
}

Expected<std::optional<std::chrono::system_clock::time_point>> readOptionalTime(const Statement& row, int column,
                                                                                std::string_view name) {
    if (row.isNull(column)) {
        return std::optional<std::chrono::system_clock::time_point>();
    }
    auto time = readTime(row.columnText(column), name);
    if (!time) {
        return propagate(time.error());
    }
    return std::optional<std::chrono::system_clock::time_point>(*time);
}

/// launcher::FailureReason by its toString() name.
std::optional<launcher::FailureReason> failureFrom(std::string_view text) noexcept {
    for (int value = 0; value <= static_cast<int>(launcher::FailureReason::EarlyExit); ++value) {
        const auto reason = static_cast<launcher::FailureReason>(value);
        if (launcher::toString(reason) == text) {
            return reason;
        }
    }
    return std::nullopt;
}

/// launcher::ExitKind by its toString() name.
std::optional<launcher::ExitKind> exitKindFrom(std::string_view text) noexcept {
    for (int value = 0; value <= static_cast<int>(launcher::ExitKind::Interrupted); ++value) {
        const auto kind = static_cast<launcher::ExitKind>(value);
        if (launcher::toString(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

/// launcher::LaunchChoiceKind by its toString() name ("command", "profile").
std::optional<launcher::LaunchChoiceKind> choiceKindFrom(std::string_view text) noexcept {
    if (text == launcher::toString(launcher::LaunchChoiceKind::EsdeCommand)) {
        return launcher::LaunchChoiceKind::EsdeCommand;
    }
    if (text == launcher::toString(launcher::LaunchChoiceKind::Profile)) {
        return launcher::LaunchChoiceKind::Profile;
    }
    return std::nullopt;
}

Expected<launcher::LaunchChoice> readChoice(const Statement& row, int kindColumn, int valueColumn) {
    const auto kind = choiceKindFrom(row.columnText(kindColumn));
    if (!kind) {
        return unreadable("a launch choice of an unknown kind", std::string(row.columnText(kindColumn)));
    }
    return launcher::LaunchChoice{*kind, std::string(row.columnText(valueColumn))};
}

Expected<GameId> readGameId(std::int64_t value) {
    const auto id = value > 0 ? GameId::fromValue(static_cast<std::uint64_t>(value)) : std::nullopt;
    if (!id) {
        return unreadable(fmt::format("a GameId that is none ({})", value));
    }
    return *id;
}

/// A choice's value: not empty, UTF-8 without NUL. Errors: InvalidArgument.
Status checkChoice(const launcher::LaunchChoice& choice) {
    if (choice.value.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a launch choice names no command label or profile id");
    }
    if (!core::isValidUtf8(choice.value) || choice.value.find('\0') != std::string::npos) {
        return makeError(ErrorCode::InvalidArgument, "a launch choice that is not UTF-8 text");
    }
    return ok();
}

/// What the schema's checks ask of a session, said before SQLite says it (InvalidArgument naming the field).
Status checkSession(const PlaySession& session) {
    const auto refuse = [&](std::string_view why) -> Status {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("a play session {}: {}", catalog::toString(session.outcome), why));
    };
    if (!session.game.isValid()) {
        return refuse("names no game");
    }
    const bool open =
        session.outcome == PlaySessionOutcome::Running || session.outcome == PlaySessionOutcome::Interrupted;
    if (open && session.ended) {
        return refuse("has an end time");
    }
    if (!open && !session.ended) {
        return refuse("has no end time");
    }
    if ((session.outcome == PlaySessionOutcome::Interrupted) != session.closed.has_value()) {
        return refuse(session.closed ? "has a time it was closed as interrupted" : "has no time it was closed");
    }
    if (session.outcome == PlaySessionOutcome::Blocked) {
        if (!session.readiness || *session.readiness == catalog::LaunchReadiness::Ready) {
            return refuse("has no readiness that blocked it");
        }
    } else if (session.readiness) {
        return refuse("has a readiness, which only a blocked attempt has");
    }
    if ((session.outcome == PlaySessionOutcome::LeftRunning) != (session.leftRunning != LeftRunningReason::None)) {
        return refuse(session.leftRunning == LeftRunningReason::None ? "says no reason it was left running"
                                                                     : "has a reason it was left running");
    }
    if (session.exitCode.has_value() != session.exitKind.has_value()) {
        return refuse(session.exitCode ? "has an exit code without its kind" : "has an exit kind without an exit code");
    }
    const bool neverStarted = session.outcome == PlaySessionOutcome::Blocked || session.refusedByGuard ||
                              session.failure == launcher::FailureReason::InvalidRequest ||
                              session.failure == launcher::FailureReason::StartFailed;
    if (session.exitCode && neverStarted) {
        return refuse("has an exit code, which an attempt that never started has not");
    }
    if (session.outcome == PlaySessionOutcome::Crashed && !session.exitCode) {
        return refuse("has no exit code");
    }
    if (session.runTime && session.runTime->count() < 0) {
        return refuse("ran for a negative time");
    }
    for (const std::string_view text : {std::string_view(session.choice.value), std::string_view(session.message)}) {
        if (!core::isValidUtf8(text) || text.find('\0') != std::string_view::npos) {
            return refuse("holds text that is not UTF-8");
        }
    }
    return ok();
}

/// Binds the session's fields from ?2 to ?21 (the columns UpdateSession sets, in InsertSession's order after the game).
void bindSessionFields(Statement& statement, const PlaySession& session) {
    statement.bindText(2, launcher::toString(session.choice.kind))
        .bindText(3, session.choice.value)
        .bindText(4, catalog::toString(session.choiceSource))
        .bindBool(5, session.test)
        .bindText(6, timeText(session.started))
        .bindOptionalText(7, session.ended ? std::optional<std::string>(timeText(*session.ended)) : std::nullopt)
        .bindOptionalText(8, session.closed ? std::optional<std::string>(timeText(*session.closed)) : std::nullopt)
        .bindOptionalInt(9, session.runTime ? std::optional<std::int64_t>(session.runTime->count()) : std::nullopt)
        .bindText(10, catalog::toString(session.outcome))
        .bindOptionalText(11, session.readiness
                                  ? std::optional<std::string_view>(catalog::toString(*session.readiness))
                                  : std::nullopt)
        .bindText(12, catalog::toString(session.leftRunning))
        .bindText(13, launcher::toString(session.failure))
        .bindOptionalInt(14, session.exitCode ? std::optional<std::int64_t>(*session.exitCode) : std::nullopt)
        .bindOptionalText(15, session.exitKind ? std::optional<std::string_view>(launcher::toString(*session.exitKind))
                                               : std::nullopt)
        .bindBool(16, session.quickExit)
        .bindBool(17, session.confirmedByUser)
        .bindBool(18, session.processesRemained)
        .bindBool(19, session.refusedByGuard)
        .bindText(20, session.message)
        .bindInt(21, session.processId);
}

/// A session from the current row of a statement selecting RV_SESSION_COLUMNS first.
Expected<PlaySession> readSession(const Statement& row) {
    PlaySession session;
    session.id = row.columnInt(0);
    const std::string context = fmt::format("rv_play_sessions {}", session.id);
    const auto withContext = [&](Error error) {
        error.context = context;
        return tl::unexpected<Error>(std::move(error));
    };
    auto game = readGameId(row.columnInt(1));
    if (!game) {
        return withContext(game.error());
    }
    session.game = *game;
    auto choice = readChoice(row, 2, 3);
    if (!choice) {
        return withContext(choice.error());
    }
    session.choice = std::move(*choice);
    const auto source = catalog::parseChoiceSource(row.columnText(4));
    if (!source) {
        return unreadable("a choice source it does not know", context);
    }
    session.choiceSource = *source;
    session.test = row.columnInt(5) != 0;
    auto started = readTime(row.columnText(6), "started_at");
    if (!started) {
        return withContext(started.error());
    }
    session.started = *started;
    auto ended = readOptionalTime(row, 7, "ended_at");
    if (!ended) {
        return withContext(ended.error());
    }
    session.ended = *ended;
    auto closed = readOptionalTime(row, 8, "closed_at");
    if (!closed) {
        return withContext(closed.error());
    }
    session.closed = *closed;
    if (const auto runMs = row.columnOptionalInt(9)) {
        session.runTime = std::chrono::milliseconds(*runMs);
    }
    const auto outcome = catalog::parsePlaySessionOutcome(row.columnText(10));
    if (!outcome) {
        return unreadable("a play session outcome it does not know", context);
    }
    session.outcome = *outcome;
    if (!row.isNull(11)) {
        const auto readiness = catalog::parseLaunchReadiness(row.columnText(11));
        if (!readiness) {
            return unreadable("a readiness it does not know", context);
        }
        session.readiness = *readiness;
    }
    const auto leftRunning = catalog::parseLeftRunningReason(row.columnText(12));
    const auto failure = failureFrom(row.columnText(13));
    const auto exitKind = row.isNull(15) ? std::optional<launcher::ExitKind>() : exitKindFrom(row.columnText(15));
    if (!leftRunning || !failure || (!row.isNull(15) && !exitKind)) {
        return unreadable("a play session detail it does not know", context);
    }
    session.leftRunning = *leftRunning;
    session.failure = *failure;
    session.exitKind = exitKind;
    if (const auto code = row.columnOptionalInt(14)) {
        session.exitCode = static_cast<std::uint32_t>(*code);
    }
    session.quickExit = row.columnInt(16) != 0;
    session.confirmedByUser = row.columnInt(17) != 0;
    session.processesRemained = row.columnInt(18) != 0;
    session.refusedByGuard = row.columnInt(19) != 0;
    session.message = std::string(row.columnText(20));
    session.processId = static_cast<std::uint32_t>(row.columnInt(21));
    session.owner.processId = static_cast<std::uint32_t>(row.columnInt(22));
    session.owner.creationTime = static_cast<std::uint64_t>(row.columnInt(23));
    return session;
}

/// SQLite's integers are signed: a FILETIME creation time fits (2^63 ticks are 29,000 years).
std::int64_t storedTicks(std::uint64_t ticks) noexcept {
    return static_cast<std::int64_t>(std::min<std::uint64_t>(ticks, std::numeric_limits<std::int64_t>::max()));
}

} // namespace

// --- SessionStore ----------------------------------------------------------------------------------------------------

struct SessionStore::State {
    explicit State(Connection& connection) noexcept : connection(connection) {}

    /// The statement `which`, prepared on first use and kept.
    Expected<Statement*> statement(Sql which) {
        Statement& kept = statements[static_cast<std::size_t>(which)];
        if (!kept.valid()) {
            auto prepared = connection.prepare(sqlOf(which));
            if (!prepared) {
                return propagate(prepared.error());
            }
            kept = std::move(*prepared);
        }
        return &kept;
    }

    /// Runs `write` (a function returning Expected<T> or Status) in one write transaction (BEGIN IMMEDIATE), committed
    /// when it succeeds; a busy database is tried again as `retry` says. `what` names the write in the log.
    template <class Write>
    auto inTransaction(std::string_view what, const BusyRetry& retry, Write&& write) -> decltype(write()) {
        using Result = decltype(write());
        const auto deadline = std::chrono::steady_clock::now() + retry.retryFor;
        for (int attempt = 1;; ++attempt) {
            Result result = [&]() -> Result {
                auto transaction = Transaction::begin(connection, Transaction::Mode::Immediate);
                if (!transaction) {
                    return propagate(transaction.error());
                }
                Result written = write();
                if (!written) {
                    return written; // the transaction rolls back
                }
                if (auto committed = transaction->commit(); !committed) {
                    return propagate(committed.error());
                }
                return written;
            }();
            if (result || !isBusyError(result.error())) {
                if (result && attempt > 1) {
                    spdlog::info("Session store: {} written at attempt {} (the database was busy)", what, attempt);
                }
                return result;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return result;
            }
            ++busyRetries;
            if (attempt == 1) {
                spdlog::warn("Session store: the catalog database is busy (another process writes, an import?): "
                             "{} is tried again for up to {:.1f} s",
                             what, std::chrono::duration<double>(retry.retryFor).count());
            }
            std::this_thread::sleep_for(
                std::min<std::chrono::steady_clock::duration>(retry.interval, deadline - now));
        }
    }

    Connection& connection;
    std::array<Statement, static_cast<std::size_t>(Sql::Count)> statements;
    /// Atomic: busyRetries() reads it on any thread, also while a write waits.
    std::atomic<std::size_t> busyRetries{0};
};

SessionStore::SessionStore(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
SessionStore::~SessionStore() = default;

Expected<std::unique_ptr<SessionStore>> SessionStore::open(Connection& connection) {
    const auto version = connection.userVersion();
    if (!version) {
        return propagate(version.error());
    }
    const int program = programSchemaVersion();
    if (*version < program) {
        return makeError(ErrorCode::Conflict,
                         fmt::format("the catalog database has schema version {}, this program needs {}: migrate it "
                                     "first (retroverse-cli db migrate)",
                                     *version, program),
                         core::pathToUtf8Lossy(connection.path()));
    }
    if (*version > program) {
        return makeError(ErrorCode::Conflict,
                         fmt::format("the catalog database has schema version {}, newer than this program's {}: it "
                                     "was written by a newer Retroverse",
                                     *version, program),
                         core::pathToUtf8Lossy(connection.path()));
    }
    return std::unique_ptr<SessionStore>(new SessionStore(std::make_unique<State>(connection)));
}

std::size_t SessionStore::busyRetries() const noexcept {
    return m_state->busyRetries.load();
}

Expected<std::int64_t> SessionStore::insertSession(const PlaySession& session, const BusyRetry& retry) {
    if (session.id != 0) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("the play session {} is stored already: update it", session.id));
    }
    if (auto checked = checkSession(session); !checked) {
        return propagate(checked.error());
    }
    return m_state->inTransaction("a play session", retry, [&]() -> Expected<std::int64_t> {
        auto insert = m_state->statement(Sql::InsertSession);
        if (!insert) {
            return propagate(insert.error());
        }
        Statement& statement = **insert;
        statement.bindInt(1, static_cast<std::int64_t>(session.game.value()));
        bindSessionFields(statement, session);
        statement.bindInt(22, session.owner.processId).bindInt(23, storedTicks(session.owner.creationTime));
        if (auto inserted = statement.run(); !inserted) {
            if (isForeignKeyFailure(inserted.error())) {
                return makeError(ErrorCode::NotFound,
                                 fmt::format("the catalog has no game {} (no identity, no alias)",
                                             session.game.toString()));
            }
            return propagate(inserted.error());
        }
        return m_state->connection.lastInsertRowId();
    });
}

Status SessionStore::updateSession(const PlaySession& session, const BusyRetry& retry) {
    if (session.id <= 0) {
        return makeError(ErrorCode::InvalidArgument, "the play session is not stored yet: insert it");
    }
    if (auto checked = checkSession(session); !checked) {
        return checked;
    }
    return m_state->inTransaction("the end of a play session", retry, [&]() -> Status {
        auto update = m_state->statement(Sql::UpdateSession);
        if (!update) {
            return propagate(update.error());
        }
        Statement& statement = **update;
        statement.bindInt(1, session.id);
        bindSessionFields(statement, session);
        if (auto updated = statement.run(); !updated) {
            return updated;
        }
        if (m_state->connection.changes() == 0) {
            return makeError(ErrorCode::NotFound, fmt::format("the catalog has no play session {}", session.id));
        }
        return ok();
    });
}

Expected<std::optional<PlaySession>> SessionStore::session(std::int64_t id) {
    auto select = m_state->statement(Sql::SelectSession);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindInt(1, id);
    auto stepped = row.step();
    if (!stepped) {
        return propagate(stepped.error());
    }
    if (!*stepped) {
        return std::optional<PlaySession>();
    }
    auto session = readSession(row);
    if (!session) {
        return propagate(session.error());
    }
    return std::optional<PlaySession>(std::move(*session));
}

Expected<std::vector<PlaySession>> SessionStore::closeInterrupted(const OwnerAlive& ownerAlive,
                                                                  std::chrono::system_clock::time_point now,
                                                                  const BusyRetry& retry) {
    // The open sessions and their owners, read first (the owners are asked outside any transaction).
    std::vector<PlaySession> gone;
    {
        auto select = m_state->statement(Sql::SelectOpenSessions);
        if (!select) {
            return propagate(select.error());
        }
        Statement& row = **select;
        const ScopedReset reset(row);
        for (;;) {
            auto stepped = row.step();
            if (!stepped) {
                return propagate(stepped.error());
            }
            if (!*stepped) {
                break;
            }
            auto session = readSession(row);
            if (!session) {
                return propagate(session.error());
            }
            if (!ownerAlive || !ownerAlive(session->owner)) {
                gone.push_back(std::move(*session));
            }
        }
    }
    if (gone.empty()) {
        return gone;
    }
    auto closed = m_state->inTransaction(
        "the sessions left open by owners that are gone", retry, [&]() -> Expected<std::vector<PlaySession>> {
            auto update = m_state->statement(Sql::CloseInterrupted);
            if (!update) {
                return propagate(update.error());
            }
            std::vector<PlaySession> result;
            for (const PlaySession& session : gone) {
                PlaySession interrupted = session;
                interrupted.outcome = PlaySessionOutcome::Interrupted;
                interrupted.closed = now;
                interrupted.message = fmt::format("its owner (process {}) ended before the session did; the game's end "
                                                  "is not known",
                                                  session.owner.processId);
                Statement& statement = **update;
                statement.bindInt(1, session.id)
                    .bindText(2, timeText(now))
                    .bindText(3, interrupted.message)
                    .bindInt(4, session.owner.processId)
                    .bindInt(5, storedTicks(session.owner.creationTime));
                if (auto updated = statement.run(); !updated) {
                    return propagate(updated.error());
                }
                if (m_state->connection.changes() > 0) {
                    result.push_back(std::move(interrupted));
                }
            }
            return result;
        });
    if (closed) {
        for (const PlaySession& session : *closed) {
            spdlog::info("Session store: play session {} of game {} closed as interrupted (its owner, process {}, is "
                         "gone)",
                         session.id, session.game.toString(), session.owner.processId);
        }
    }
    return closed;
}

Expected<std::vector<ListedSession>> SessionStore::sessions(const SessionFilter& filter) {
    auto select = m_state->statement(filter.game ? Sql::SelectGameSessions : Sql::SelectSessions);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    const std::int64_t limit = filter.limit == 0 ? -1 : static_cast<std::int64_t>(std::min<std::size_t>(
                                                            filter.limit, std::numeric_limits<std::int64_t>::max()));
    if (filter.game) {
        row.bindInt(1, static_cast<std::int64_t>(filter.game->value())).bindInt(2, limit);
    } else {
        row.bindInt(1, limit);
    }
    std::vector<ListedSession> listed;
    for (;;) {
        auto stepped = row.step();
        if (!stepped) {
            return propagate(stepped.error());
        }
        if (!*stepped) {
            break;
        }
        auto session = readSession(row);
        if (!session) {
            return propagate(session.error());
        }
        ListedSession entry;
        entry.session = std::move(*session);
        auto library = LibraryId::parse(row.columnText(kSessionColumns));
        auto system = SystemName::parse(row.columnText(kSessionColumns + 1));
        if (!library || !system) {
            return unreadable("an identity with a library or system that is none",
                              fmt::format("rv_game_identities {}", entry.session.game.toString()));
        }
        entry.library = std::move(*library);
        entry.system = std::move(*system);
        entry.relativePath = std::string(row.columnText(kSessionColumns + 2));
        if (!row.isNull(kSessionColumns + 3)) {
            entry.title = std::string(row.columnText(kSessionColumns + 3));
        }
        listed.push_back(std::move(entry));
    }
    return listed;
}

// --- Choices ---------------------------------------------------------------------------------------------------------

Expected<std::optional<StoredChoice>> SessionStore::gameChoice(GameId game) {
    auto select = m_state->statement(Sql::SelectGameChoice);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindInt(1, static_cast<std::int64_t>(game.value()));
    auto stepped = row.step();
    if (!stepped) {
        return propagate(stepped.error());
    }
    if (!*stepped) {
        return std::optional<StoredChoice>();
    }
    auto choice = readChoice(row, 0, 1);
    if (!choice) {
        return propagate(choice.error());
    }
    auto chosenAt = readTime(row.columnText(2), "rv_launch_game_choices.chosen_at");
    if (!chosenAt) {
        return propagate(chosenAt.error());
    }
    return std::optional<StoredChoice>(StoredChoice{std::move(*choice), *chosenAt});
}

Status SessionStore::setGameChoice(GameId game, const launcher::LaunchChoice& choice,
                                   std::chrono::system_clock::time_point chosenAt, const BusyRetry& retry) {
    if (auto checked = checkChoice(choice); !checked) {
        return checked;
    }
    return m_state->inTransaction("a game's launch choice", retry, [&]() -> Status {
        auto upsert = m_state->statement(Sql::UpsertGameChoice);
        if (!upsert) {
            return propagate(upsert.error());
        }
        Statement& statement = **upsert;
        statement.bindInt(1, static_cast<std::int64_t>(game.value()))
            .bindText(2, launcher::toString(choice.kind))
            .bindText(3, choice.value)
            .bindText(4, timeText(chosenAt));
        if (auto written = statement.run(); !written) {
            if (isForeignKeyFailure(written.error())) {
                return makeError(ErrorCode::NotFound,
                                 fmt::format("the catalog has no game {} (no identity, no alias)", game.toString()));
            }
            return written;
        }
        return ok();
    });
}

Expected<std::optional<catalog::GameStatus>> SessionStore::gameStatus(GameId game) {
    auto select = m_state->statement(Sql::SelectGameStatus);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindInt(1, static_cast<std::int64_t>(game.value()));
    auto stepped = row.step();
    if (!stepped) {
        return propagate(stepped.error());
    }
    if (!*stepped) {
        return std::optional<catalog::GameStatus>();
    }
    const std::optional<catalog::GameStatus> status = catalog::parseGameStatus(row.columnText(0));
    if (!status) {
        return unreadable("a game status that is none", std::string(row.columnText(0)));
    }
    return status;
}

Status SessionStore::setGameStatus(GameId game, catalog::GameStatus status,
                                   std::chrono::system_clock::time_point setAt, const BusyRetry& retry) {
    return m_state->inTransaction("a game's status", retry, [&]() -> Status {
        auto upsert = m_state->statement(Sql::UpsertGameStatus);
        if (!upsert) {
            return propagate(upsert.error());
        }
        Statement& statement = **upsert;
        statement.bindInt(1, static_cast<std::int64_t>(game.value()))
            .bindText(2, catalog::toString(status))
            .bindText(3, timeText(setAt));
        if (auto written = statement.run(); !written) {
            if (isForeignKeyFailure(written.error())) {
                return makeError(ErrorCode::NotFound,
                                 fmt::format("the catalog has no game {} (no identity, no alias)", game.toString()));
            }
            return written;
        }
        return ok();
    });
}

Expected<bool> SessionStore::clearGameChoice(GameId game, const BusyRetry& retry) {
    return m_state->inTransaction("a game's launch choice removed", retry, [&]() -> Expected<bool> {
        auto remove = m_state->statement(Sql::DeleteGameChoice);
        if (!remove) {
            return propagate(remove.error());
        }
        (*remove)->bindInt(1, static_cast<std::int64_t>(game.value()));
        if (auto removed = (*remove)->run(); !removed) {
            return propagate(removed.error());
        }
        return m_state->connection.changes() > 0;
    });
}

namespace {

Expected<SystemChoiceRow> readSystemChoice(const Statement& row) {
    auto system = SystemName::parse(row.columnText(0));
    if (!system) {
        return unreadable("a launch choice for a system name that is none", std::string(row.columnText(0)));
    }
    auto choice = readChoice(row, 1, 2);
    if (!choice) {
        return propagate(choice.error());
    }
    auto chosenAt = readTime(row.columnText(3), "rv_launch_system_choices.chosen_at");
    if (!chosenAt) {
        return propagate(chosenAt.error());
    }
    return SystemChoiceRow{std::move(*system), StoredChoice{std::move(*choice), *chosenAt}};
}

} // namespace

Expected<std::vector<SystemChoiceRow>> SessionStore::systemChoices(const LibraryId& library) {
    auto select = m_state->statement(Sql::SelectSystemChoices);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindText(1, library.str());
    std::vector<SystemChoiceRow> rows;
    for (;;) {
        auto stepped = row.step();
        if (!stepped) {
            return propagate(stepped.error());
        }
        if (!*stepped) {
            break;
        }
        auto choice = readSystemChoice(row);
        if (!choice) {
            return propagate(choice.error());
        }
        rows.push_back(std::move(*choice));
    }
    return rows;
}

Expected<std::optional<StoredChoice>> SessionStore::systemChoice(const LibraryId& library, const SystemName& system) {
    auto select = m_state->statement(Sql::SelectSystemChoice);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindText(1, library.str()).bindText(2, system.str());
    auto stepped = row.step();
    if (!stepped) {
        return propagate(stepped.error());
    }
    if (!*stepped) {
        return std::optional<StoredChoice>();
    }
    auto choice = readSystemChoice(row);
    if (!choice) {
        return propagate(choice.error());
    }
    return std::optional<StoredChoice>(std::move(choice->stored));
}

Status SessionStore::setSystemChoice(const LibraryId& library, const SystemName& system,
                                     const launcher::LaunchChoice& choice,
                                     std::chrono::system_clock::time_point chosenAt, const BusyRetry& retry) {
    if (system.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a system's launch choice names no system");
    }
    if (auto checked = checkChoice(choice); !checked) {
        return checked;
    }
    return m_state->inTransaction("a system's launch choice", retry, [&]() -> Status {
        auto upsert = m_state->statement(Sql::UpsertSystemChoice);
        if (!upsert) {
            return propagate(upsert.error());
        }
        Statement& statement = **upsert;
        statement.bindText(1, library.str())
            .bindText(2, system.str())
            .bindText(3, launcher::toString(choice.kind))
            .bindText(4, choice.value)
            .bindText(5, timeText(chosenAt));
        if (auto written = statement.run(); !written) {
            if (isForeignKeyFailure(written.error())) {
                return makeError(ErrorCode::InvalidArgument,
                                 fmt::format("the catalog holds no library '{}' (an import records it)", library.str()));
            }
            return written;
        }
        return ok();
    });
}

Expected<bool> SessionStore::clearSystemChoice(const LibraryId& library, const SystemName& system,
                                               const BusyRetry& retry) {
    return m_state->inTransaction("a system's launch choice removed", retry, [&]() -> Expected<bool> {
        auto remove = m_state->statement(Sql::DeleteSystemChoice);
        if (!remove) {
            return propagate(remove.error());
        }
        (*remove)->bindText(1, library.str()).bindText(2, system.str());
        if (auto removed = (*remove)->run(); !removed) {
            return propagate(removed.error());
        }
        return m_state->connection.changes() > 0;
    });
}

Expected<std::vector<ProgramChoiceRow>> SessionStore::programChoices(const LibraryId& library) {
    auto select = m_state->statement(Sql::SelectProgramChoices);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindText(1, library.str());
    std::vector<ProgramChoiceRow> rows;
    for (;;) {
        auto stepped = row.step();
        if (!stepped) {
            return propagate(stepped.error());
        }
        if (!*stepped) {
            break;
        }
        auto program = core::tryPathFromUtf8(row.columnText(1));
        auto chosenAt = readTime(row.columnText(2), "rv_launch_program_choices.chosen_at");
        if (!program || !chosenAt) {
            return unreadable("a program choice it cannot read", std::string(row.columnText(0)));
        }
        rows.push_back(ProgramChoiceRow{std::string(row.columnText(0)), std::move(*program), *chosenAt});
    }
    return rows;
}

Status SessionStore::setProgramChoice(const LibraryId& library, std::string_view emulator,
                                      const std::filesystem::path& program,
                                      std::chrono::system_clock::time_point chosenAt, const BusyRetry& retry) {
    const auto text = core::tryPathToUtf8(program);
    if (emulator.empty() || !text || text->empty()) {
        return makeError(ErrorCode::InvalidArgument, "a program choice names no emulator or no program");
    }
    if (!core::isValidUtf8(emulator) || emulator.find('\0') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "a program choice that is not UTF-8 text");
    }
    return m_state->inTransaction("an emulator's program", retry, [&]() -> Status {
        auto upsert = m_state->statement(Sql::UpsertProgramChoice);
        if (!upsert) {
            return propagate(upsert.error());
        }
        Statement& statement = **upsert;
        statement.bindText(1, library.str())
            .bindText(2, emulator)
            .bindText(3, *text)
            .bindText(4, timeText(chosenAt));
        if (auto written = statement.run(); !written) {
            if (isForeignKeyFailure(written.error())) {
                return makeError(ErrorCode::InvalidArgument,
                                 fmt::format("the catalog holds no library '{}' (an import records it)", library.str()));
            }
            return written;
        }
        return ok();
    });
}

Expected<bool> SessionStore::clearProgramChoice(const LibraryId& library, std::string_view emulator,
                                                const BusyRetry& retry) {
    return m_state->inTransaction("an emulator's program removed", retry, [&]() -> Expected<bool> {
        auto remove = m_state->statement(Sql::DeleteProgramChoice);
        if (!remove) {
            return propagate(remove.error());
        }
        (*remove)->bindText(1, library.str()).bindText(2, emulator);
        if (auto removed = (*remove)->run(); !removed) {
            return propagate(removed.error());
        }
        return m_state->connection.changes() > 0;
    });
}

// --- Verifications ---------------------------------------------------------------------------------------------------

Expected<std::int64_t> SessionStore::recordVerification(const LaunchVerification& verification,
                                                        const BusyRetry& retry) {
    if (verification.system.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a launch verification names no system");
    }
    auto program = core::tryPathToUtf8(verification.program);
    if (!program || !core::isValidUtf8(verification.choice.value)) {
        return makeError(ErrorCode::InvalidArgument, "a launch verification holds text that is not UTF-8");
    }
    return m_state->inTransaction("a launch verification", retry, [&]() -> Expected<std::int64_t> {
        auto upsert = m_state->statement(Sql::UpsertVerification);
        if (!upsert) {
            return propagate(upsert.error());
        }
        Statement& statement = **upsert;
        const ScopedReset reset(statement);
        statement.bindText(1, verification.library.str())
            .bindText(2, verification.system.str())
            .bindText(3, launcher::toString(verification.choice.kind))
            .bindText(4, verification.choice.value)
            .bindInt(5, static_cast<std::int64_t>(verification.game.value()))
            .bindInt(6, verification.session)
            .bindBool(7, verification.test)
            .bindText(8, *program)
            .bindText(9, timeText(verification.verifiedAt));
        auto stepped = statement.step();
        if (!stepped) {
            if (isForeignKeyFailure(stepped.error())) {
                return makeError(ErrorCode::InvalidArgument,
                                 fmt::format("a launch verification names a game ({}), library ('{}') or session ({}) "
                                             "the catalog does not hold",
                                             verification.game.toString(), verification.library.str(),
                                             verification.session));
            }
            return propagate(stepped.error());
        }
        if (!*stepped) {
            return unreadable("no row for the launch verification it wrote");
        }
        const std::int64_t id = statement.columnInt(0);
        // To its end, so that the statement is done before the commit.
        if (auto finished = statement.step(); !finished) {
            return propagate(finished.error());
        }
        return id;
    });
}

Expected<std::vector<LaunchVerification>> SessionStore::verifications(const LibraryId& library) {
    auto select = m_state->statement(Sql::SelectVerifications);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindText(1, library.str());
    std::vector<LaunchVerification> rows;
    for (;;) {
        auto stepped = row.step();
        if (!stepped) {
            return propagate(stepped.error());
        }
        if (!*stepped) {
            break;
        }
        LaunchVerification verification;
        verification.id = row.columnInt(0);
        const std::string context = fmt::format("rv_launch_verifications {}", verification.id);
        auto libraryId = LibraryId::parse(row.columnText(1));
        auto system = SystemName::parse(row.columnText(2));
        auto choice = readChoice(row, 3, 4);
        auto game = readGameId(row.columnInt(5));
        auto program = core::tryPathFromUtf8(row.columnText(8));
        auto verifiedAt = readTime(row.columnText(9), "rv_launch_verifications.verified_at");
        if (!libraryId || !system || !choice || !game || !program || !verifiedAt) {
            return unreadable("a launch verification it cannot read", context);
        }
        verification.library = std::move(*libraryId);
        verification.system = std::move(*system);
        verification.choice = std::move(*choice);
        verification.game = *game;
        verification.session = row.columnInt(6);
        verification.test = row.columnInt(7) != 0;
        verification.program = std::move(*program);
        verification.verifiedAt = *verifiedAt;
        rows.push_back(std::move(verification));
    }
    return rows;
}

// --- Games -----------------------------------------------------------------------------------------------------------

Expected<std::optional<GameId>> SessionStore::resolveGame(GameId id) {
    auto select = m_state->statement(Sql::ResolveGame);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindInt(1, static_cast<std::int64_t>(id.value()));
    auto stepped = row.step();
    if (!stepped) {
        return propagate(stepped.error());
    }
    if (!*stepped) {
        return std::optional<GameId>();
    }
    auto game = readGameId(row.columnInt(0));
    if (!game) {
        return propagate(game.error());
    }
    return std::optional<GameId>(*game);
}

// --- SessionRecorder -------------------------------------------------------------------------------------------------

SessionRecorder::SessionRecorder(SessionStore& store, PlaySession session) noexcept
    : m_store(&store), m_session(std::move(session)) {
    m_session.id = 0;
}

Status SessionRecorder::write(const BusyRetry& retry) {
    if (!m_pending) {
        return ok();
    }
    if (m_session.id == 0) {
        auto id = m_store->insertSession(m_session, retry);
        if (!id) {
            return propagate(id.error());
        }
        m_session.id = *id;
    } else if (auto updated = m_store->updateSession(m_session, retry); !updated) {
        return updated;
    }
    m_pending = false;
    return ok();
}

Status SessionRecorder::begin(const BusyRetry& retry) {
    return flush(retry);
}

void SessionRecorder::update(PlaySession session) {
    session.id = m_session.id;
    m_session = std::move(session);
    m_pending = true;
}

Status SessionRecorder::flush(const BusyRetry& retry) {
    auto written = write(retry);
    if (!written && isBusyError(written.error())) {
        spdlog::warn("Session store: the catalog database is busy: the play session of {} stays pending ({})",
                     m_session.game.toString(), written.error().message);
        return ok();
    }
    return written;
}

Status SessionRecorder::finish(const BusyRetry& retry) {
    return write(retry);
}

} // namespace rv::catalog_db
