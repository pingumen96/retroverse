// retroverse-cli sessions (I2 step 12): the play sessions the catalog database records (ADR-0008 §9, FR-026), newest
// first: every launch attempt with its game, the choice it launched with, its UTC start and end, how long the game ran,
// its outcome (returned, a quick exit, failed to start, refused by the automation guard, crashed, blocked before the
// launch, left running, interrupted) and its owner. Like every command that records sessions, it first closes as
// interrupted the open sessions whose owner is gone (a retroverse-cli or app that ended while a game ran); a session
// another live process follows stays open (running).
//
//   sessions [--game <id>] [--limit <n>] [--json] [--output <file>]
//
// --game lists one game's sessions (an alias stands for the game it names: the output names that game, and the alias as
// the requested game, as `launch` does); --limit caps the list (50 by default, 0 for every session). Exit codes: 0; 1 a
// --game the catalog does not know (neither an identity nor an alias); 2 invalid options; 3 when the catalog cannot be
// read (no catalog yet: an import makes it) or the results written.

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/play_session.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/time.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "session_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli sessions";

/// The default of --limit.
constexpr int kDefaultLimit = 50;
/// The largest --limit.
constexpr int kMaxLimit = 1'000'000;

Json listedJson(const catalog_db::ListedSession& listed) {
    Json json = sessionJson(listed.session);
    json["library"] = listed.library.str();
    json["system"] = listed.system.str();
    json["path"] = listed.relativePath;
    json["title"] = listed.title ? Json(*listed.title) : Json(nullptr);
    return json;
}

/// `game`: the identity --game stands for; `requested`: the GameId --game gave (an alias, when it differs).
void writeText(const std::vector<catalog_db::ListedSession>& sessions,
               const std::vector<catalog::PlaySession>& interrupted, const std::optional<catalog::GameId>& game,
               const std::optional<catalog::GameId>& requested, Output& out) {
    for (const catalog::PlaySession& session : interrupted) {
        out.writeLine(fmt::format("Session {} of {} closed as interrupted now: its owner, process {}, is gone.",
                                  session.id, session.game.toString(), session.owner.processId));
    }
    if (game && requested && *requested != *game) {
        out.writeLine(fmt::format("Sessions of {} (for {}, an alias).", game->toString(), requested->toString()));
    }
    if (sessions.empty()) {
        out.writeLine(game ? fmt::format("No play session of {}.", game->toString()) : std::string("No play session."));
        return;
    }
    out.writeLine(fmt::format("{:>6}  {:<24}  {:>10}  {}", "Id", "Started (UTC)", "Ran for", "Game, choice, outcome"));
    for (const catalog_db::ListedSession& listed : sessions) {
        const catalog::PlaySession& session = listed.session;
        out.writeLine(fmt::format("{:>6}  {:<24}  {:>10}  {} {} \"{}\" [{}]", session.id,
                                  core::formatUtcIso8601(session.started),
                                  session.runTime ? durationText(*session.runTime) : std::string("-"),
                                  listed.system.str(), listed.title.value_or(listed.relativePath),
                                  listed.relativePath, session.game.toString()));
        out.writeLine(fmt::format("{:>6}  {:<24}  {:>10}  {}{}: {}", "", "", "", choiceText(session.choice),
                                  session.test ? ", launch test" : "", outcomeText(session)));
        if (!session.message.empty() && session.outcome != catalog::PlaySessionOutcome::Returned) {
            out.writeLine(fmt::format("{:>6}  {:<24}  {:>10}  {}", "", "", "", session.message));
        }
    }
    out.writeLine(fmt::format("{} session{}.", sessions.size(), sessions.size() == 1 ? "" : "s"));
}

} // namespace

ExitCode runSessions(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Lists the play sessions, newest first: each launch attempt with its game, choice, times, "
                           "duration and outcome. Closes first the sessions left open by processes that are gone.");
    parser.option("game", "id", "Only this game's sessions (its GameId).")
        .option("limit", "n", fmt::format("At most this many sessions (default {}; 0: all).", kDefaultLimit))
        .flag("json", "Print the results as JSON.");
    addOutputOption(parser);
    const auto args = parser.parse(context.arguments);
    if (!args) {
        context.err.writeLine(fmt::format("{}: {}", kName, args.error().toString()));
        return ExitCode::UsageError;
    }
    if (args->has("help")) {
        context.out.write(parser.helpText());
        return ExitCode::Ok;
    }
    auto output = outputFileOption(*args);
    if (!output) {
        return usageError(context, output.error().toString());
    }
    // --game as given (`requested`), and the identity it stands for (`game`: an alias resolved).
    std::optional<catalog::GameId> requested;
    if (args->has("game")) {
        const auto id = catalog::GameId::parse(args->value("game").value_or(""));
        if (!id) {
            return usageError(context, fmt::format("--game: {}", id.error().toString()));
        }
        requested = *id;
    }
    const auto limit = args->intValue("limit", kDefaultLimit);
    if (!limit || *limit < 0 || *limit > kMaxLimit) {
        return usageError(context, fmt::format("--limit takes a number from 0 to {}", kMaxLimit));
    }

    auto catalog = openLaunchCatalog(context.paths);
    if (!catalog) {
        return catalogFailure(context, catalog.error());
    }
    catalog_db::SessionStore& store = *catalog->store;
    const std::vector<catalog::PlaySession> interrupted = recoverInterruptedSessions(store);
    std::optional<catalog::GameId> game;
    if (requested) {
        auto resolved = store.resolveGame(*requested);
        if (!resolved) {
            return catalogFailure(context, resolved.error());
        }
        if (!*resolved) {
            context.err.writeLine(fmt::format("{}: the catalog has no game {}", kName, requested->toString()));
            return ExitCode::ProblemsFound;
        }
        game = **resolved;
    }
    catalog_db::SessionFilter filter;
    filter.game = game;
    filter.limit = static_cast<std::size_t>(*limit);
    auto sessions = store.sessions(filter);
    if (!sessions) {
        return catalogFailure(context, sessions.error());
    }
    spdlog::info("sessions: {} listed{}", sessions->size(),
                 interrupted.empty() ? std::string() : fmt::format(", {} closed as interrupted", interrupted.size()));

    std::string text;
    if (args->has("json")) {
        Json json;
        json["game"] = game ? Json(game->toString()) : Json(nullptr);
        json["requestedGame"] = requested ? Json(requested->toString()) : Json(nullptr);
        json["limit"] = *limit;
        Json list = Json::array();
        for (const catalog_db::ListedSession& listed : *sessions) {
            list.push_back(listedJson(listed));
        }
        json["sessions"] = std::move(list);
        Json closed = Json::array();
        for (const catalog::PlaySession& session : interrupted) {
            closed.push_back(sessionJson(session));
        }
        json["interrupted"] = std::move(closed);
        text = jsonText(json);
    } else {
        Output buffer = Output::memory();
        writeText(*sessions, interrupted, game, requested, buffer);
        text = buffer.text();
    }
    return writeResults(context, *output, text, ExitCode::Ok);
}

} // namespace rv::cli
