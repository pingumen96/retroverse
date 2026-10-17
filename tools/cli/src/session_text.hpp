#pragma once

// Private to rv_cli_core (I2 step 12): what `launch --game`, `assign` and `sessions` share: the catalog database opened
// for the launch state (its writer, the session store and the queries on a reader), the owner of this process's
// sessions and the recovery of the sessions left open by owners that are gone, and play sessions and launch choices
// as text and as JSON. No source list names it.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/play_session.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/time.hpp>
#include <rv/launcher/exit_status.hpp>
#include <rv/platform_win/win32_process_runner.hpp>

#include "catalog_text.hpp"
#include "esde_text.hpp"
#include "launch_text.hpp"

namespace rv::cli {

/// The writer's busy timeout of the commands that record launches: a launch waits at most this long for another
/// process's write transaction before it starts anyway and keeps its session's first write for later
/// (catalog_db::SessionRecorder); later attempts retry as BusyRetry says.
inline constexpr std::chrono::milliseconds kLaunchBusyTimeout{1000};

/// The catalog database opened to record launches: its writer with the session store, and the catalog's queries on a
/// reader (which never waits for another process's write transaction).
struct LaunchCatalog {
    std::unique_ptr<catalog_db::Database> database;
    std::unique_ptr<catalog_db::SessionStore> store;
    std::unique_ptr<catalog_db::SqliteCatalogQuery> query;
};

/// Opens <user data>/database/retroverse.sqlite to record launches: never made (an import makes it; NotFound
/// otherwise), migrated when an older schema allows it (openCatalogDatabase()), the writer's busy timeout
/// `busyTimeout`. Errors: those of openCatalogDatabase(), SessionStore::open() and SqliteCatalogQuery::open().
inline Expected<LaunchCatalog> openLaunchCatalog(const core::Paths& paths,
                                                 std::chrono::milliseconds busyTimeout = kLaunchBusyTimeout) {
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.busyTimeout = busyTimeout;
    auto database = catalog_db::openCatalogDatabase(catalog_db::databaseFile(paths), options);
    if (!database) {
        return propagate(database.error());
    }
    LaunchCatalog catalog;
    catalog.database = std::make_unique<catalog_db::Database>(std::move(*database));
    auto store = catalog_db::SessionStore::open(catalog.database->writer());
    if (!store) {
        return propagate(store.error());
    }
    catalog.store = std::move(*store);
    auto reader = catalog.database->openReader();
    if (!reader) {
        return propagate(reader.error());
    }
    auto query = catalog_db::SqliteCatalogQuery::open(std::move(*reader));
    if (!query) {
        return propagate(query.error());
    }
    catalog.query = std::move(*query);
    return catalog;
}

/// This process as the owner of the sessions it records.
inline catalog::SessionOwner currentSessionOwner() noexcept {
    const platform_win::ProcessIdentity process = platform_win::currentProcessIdentity();
    return catalog::SessionOwner{process.processId, process.creationTime};
}

/// Whether a session's owner may still run: false only when Windows says it is gone.
inline bool sessionOwnerAlive(const catalog::SessionOwner& owner) noexcept {
    return platform_win::processLiveness(platform_win::ProcessIdentity{owner.processId, owner.creationTime}) !=
           platform_win::ProcessLiveness::Gone;
}

/// At the start of a command that records sessions (ADR-0008 §9): closes as interrupted the open sessions whose owner
/// is gone, and returns them. A database that stays busy for `retry` leaves them to the next start (a warning in the
/// log); so does any other error, logged.
inline std::vector<catalog::PlaySession> recoverInterruptedSessions(catalog_db::SessionStore& store,
                                                                     const catalog_db::BusyRetry& retry = {}) {
    auto closed = store.closeInterrupted(&sessionOwnerAlive, std::chrono::system_clock::now(), retry);
    if (!closed) {
        spdlog::warn("The play sessions left open by processes that are gone could not be closed now: {}",
                     closed.error().toString());
        return {};
    }
    return std::move(*closed);
}

/// A game of the catalog that a --game names (launch, assign).
struct CatalogGame {
    /// The identity: the GameId given, or the game it is an alias of.
    catalog::GameId id;
    catalog::GameId requested;
    catalog_db::GameDetails details;
};

/// Reads the game `requested` from the catalog (`query`, an alias resolved through `store`) and checks that it is a
/// game of `library` that can be launched: a game, or a missing file (its launch is blocked as "ROM missing" until the
/// file is back). On failure the message goes to stderr and `failure` says how the command exits: ProblemsFound for a
/// game the catalog does not have, UsageError for one of another library or that is no game (a disc member, a folder,
/// not a game for ES-DE), RuntimeFailure when the catalog cannot be read.
inline std::optional<CatalogGame> readCatalogGame(CommandContext& context, catalog_db::SqliteCatalogQuery& query,
                                                  catalog_db::SessionStore& store, catalog::GameId requested,
                                                  const catalog::LibraryId& library, ExitCode& failure) {
    const std::string name = fmt::format("retroverse-cli {}", context.command.name);
    CatalogGame game;
    game.requested = requested;
    game.id = requested;
    auto details = query.details(requested);
    if (!details) {
        failure = catalogFailure(context, details.error());
        return std::nullopt;
    }
    if (!*details) {
        auto resolved = store.resolveGame(requested);
        if (!resolved) {
            failure = catalogFailure(context, resolved.error());
            return std::nullopt;
        }
        if (*resolved && **resolved != requested) {
            game.id = **resolved;
            details = query.details(game.id);
            if (!details) {
                failure = catalogFailure(context, details.error());
                return std::nullopt;
            }
        }
    }
    if (!*details) {
        context.err.writeLine(fmt::format("{}: the catalog has no game {} (run 'retroverse-cli import' to bring the "
                                          "catalog up to date)",
                                          name, requested.toString()));
        failure = ExitCode::ProblemsFound;
        return std::nullopt;
    }
    game.details = std::move(**details);
    if (game.details.library != library) {
        context.err.writeLine(fmt::format("{}: the game {} is of the library '{}', library.json records '{}'", name,
                                          game.id.toString(), game.details.library.str(), library.str()));
        failure = ExitCode::UsageError;
        return std::nullopt;
    }
    const catalog::CatalogEntry& entry = game.details.indexed.entry;
    switch (entry.state) {
    case catalog::EntryState::Game:
    case catalog::EntryState::MissingFile:
        return game;
    case catalog::EntryState::DiscMember:
        context.err.writeLine(fmt::format("{}: {} is a disc member of \"{}\" (ADR-0008 §3): name that game instead",
                                          name, game.id.toString(), entry.discHead.relativePath()));
        break;
    case catalog::EntryState::Folder:
        context.err.writeLine(fmt::format("{}: {} is a folder, not a game", name, game.id.toString()));
        break;
    default:
        context.err.writeLine(fmt::format("{}: {} is not a game for ES-DE ({}{})", name, game.id.toString(),
                                          catalog::toString(entry.state),
                                          entry.notAGameReason == catalog::NotAGameReason::None
                                              ? std::string()
                                              : fmt::format(": {}", catalog::toString(entry.notAGameReason))));
        break;
    }
    failure = ExitCode::UsageError;
    return std::nullopt;
}

/// "2.6 s", "1 min 05 s", "2 h 03 min".
inline std::string durationText(std::chrono::milliseconds duration) {
    const auto ms = duration.count();
    if (ms < 60'000) {
        return fmt::format("{:.1f} s", static_cast<double>(ms) / 1000.0);
    }
    const auto seconds = ms / 1000;
    if (seconds < 3600) {
        return fmt::format("{} min {:02} s", seconds / 60, seconds % 60);
    }
    return fmt::format("{} h {:02} min", seconds / 3600, seconds / 60 % 60);
}

/// What a session's outcome says, in a few words: "returned (exit 0)", "failed to start: refused by the automation
/// guard", "left running (not waited for)".
inline std::string outcomeText(const catalog::PlaySession& session) {
    using catalog::PlaySessionOutcome;
    const auto exitText = [&]() {
        return session.exitCode ? launcher::describeExitCode(*session.exitCode) : std::string("no exit code");
    };
    switch (session.outcome) {
    case PlaySessionOutcome::Running:
        return session.processId != 0 ? fmt::format("running (process {}, owner process {})", session.processId,
                                                    session.owner.processId)
                                      : fmt::format("running (owner process {})", session.owner.processId);
    case PlaySessionOutcome::Returned:
        if (session.quickExit && !session.confirmedByUser) {
            return fmt::format("returned at once: quick exit, {} (the profile may need \"wait\": \"tree\")",
                               exitText());
        }
        if (session.processesRemained) {
            return fmt::format("returned, {}; processes of it still run", exitText());
        }
        return session.confirmedByUser ? fmt::format("returned (the user confirmed; {})", exitText())
                                       : fmt::format("returned, {}", exitText());
    case PlaySessionOutcome::FailedToStart:
        if (session.refusedByGuard) {
            return "failed to start: refused by the automation guard (RV_LAUNCH_ALLOWLIST)";
        }
        if (session.exitCode) {
            return fmt::format("failed to start ({}), {}", launcher::toString(session.failure), exitText());
        }
        return fmt::format("failed to start ({})", launcher::toString(session.failure));
    case PlaySessionOutcome::Crashed:
        return fmt::format("crashed, {}", exitText());
    case PlaySessionOutcome::Interrupted:
        return fmt::format("interrupted (its owner, process {}, ended first)", session.owner.processId);
    case PlaySessionOutcome::Blocked:
        return fmt::format("blocked before the launch: {}",
                           session.readiness ? catalog::describe(*session.readiness) : std::string_view("not ready"));
    case PlaySessionOutcome::LeftRunning:
        switch (session.leftRunning) {
        case catalog::LeftRunningReason::NotWaited:
            return "left running (not waited for)";
        case catalog::LeftRunningReason::WaitLimit:
            return "left running (the wait limit passed)";
        case catalog::LeftRunningReason::Quit:
            return "left running (stopped waiting)";
        case catalog::LeftRunningReason::Confirmed:
            return "left running (the user returned)";
        case catalog::LeftRunningReason::None:
            break;
        }
        return "left running";
    }
    return std::string(catalog::toString(session.outcome));
}

inline Json optionalTimeJson(const std::optional<std::chrono::system_clock::time_point>& time) {
    return time ? Json(core::formatUtcIso8601(*time)) : Json(nullptr);
}

/// A session as JSON: id, game, choice {kind, value, source}, test, startedAt, endedAt, closedAt, durationMs, outcome,
/// outcomeText, readiness, leftRunning, failure, exitCode, exitKind, exitDescription, quickExit, confirmedByUser,
/// processesRemained, refusedByGuard, message, processId, owner {processId, creationTime}.
inline Json sessionJson(const catalog::PlaySession& session) {
    Json json;
    json["id"] = session.id;
    json["game"] = session.game.toString();
    json["choice"] = Json{{"kind", std::string(launcher::toString(session.choice.kind))},
                          {"value", session.choice.value},
                          {"source", std::string(catalog::toString(session.choiceSource))}};
    json["test"] = session.test;
    json["startedAt"] = core::formatUtcIso8601(session.started);
    json["endedAt"] = optionalTimeJson(session.ended);
    json["closedAt"] = optionalTimeJson(session.closed);
    json["durationMs"] = session.runTime ? Json(session.runTime->count()) : Json(nullptr);
    json["outcome"] = std::string(catalog::toString(session.outcome));
    json["outcomeText"] = outcomeText(session);
    json["readiness"] = session.readiness ? Json(std::string(catalog::toString(*session.readiness))) : Json(nullptr);
    json["leftRunning"] = std::string(catalog::toString(session.leftRunning));
    json["failure"] = std::string(launcher::toString(session.failure));
    json["exitCode"] = session.exitCode ? Json(*session.exitCode) : Json(nullptr);
    json["exitKind"] = std::string(launcher::toString(session.exitKind));
    json["exitDescription"] =
        session.exitCode ? Json(launcher::describeExitCode(*session.exitCode)) : Json(nullptr);
    json["quickExit"] = session.quickExit;
    json["confirmedByUser"] = session.confirmedByUser;
    json["processesRemained"] = session.processesRemained;
    json["refusedByGuard"] = session.refusedByGuard;
    json["message"] = session.message;
    json["processId"] = session.processId;
    json["owner"] = Json{{"processId", session.owner.processId}, {"creationTime", session.owner.creationTime}};
    return json;
}

/// A stored choice as JSON: kind, value, chosenAt.
inline Json storedChoiceJson(const std::optional<catalog_db::StoredChoice>& stored) {
    if (!stored) {
        return nullptr;
    }
    return Json{{"kind", std::string(launcher::toString(stored->choice.kind))},
                {"value", stored->choice.value},
                {"chosenAt", core::formatUtcIso8601(stored->chosenAt)}};
}

} // namespace rv::cli
