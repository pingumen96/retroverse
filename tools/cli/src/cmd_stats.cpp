// retroverse-cli stats (I7 step 6; ADR-0018 §9): what was played in Retroverse: sessions, estimated hours, the most played
// and most recent games, per system and per status, as the statistics screen shows them (SqliteCatalogQuery::playStatistics
// on a read-only connection, statistics.hpp's rules): it writes nothing and closes no session.

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/recent_games.hpp>
#include <rv/catalog/statistics.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/time.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] stats [--limit <n>] [--json] [--output <file>]\n"
    "\n"
    "What was played in Retroverse: sessions, estimated hours (from the launch to the detected return), the most played\n"
    "and most recent games, per system and per status. Launch tests never count; crashed and left-running sessions are\n"
    "attempts, not hours. ES-DE's play counts are not added in; its last played date stays in the recent games.\n"
    "\n"
    "Options:\n"
    "  --limit <n>      At most this many games in each list (default 10).\n"
    "  --json           Write the results as JSON.\n"
    "  --output <file>  Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 2 usage error; 3 the catalog database could not be read (missing: an import makes it).\n";

/// The largest --limit.
constexpr int kMaxLimit = 1'000'000;

[[nodiscard]] double hoursOf(std::chrono::milliseconds played) noexcept {
    return std::chrono::duration<double, std::ratio<3600>>(played).count();
}

[[nodiscard]] CatalogJson gameJson(const catalog::PlayedGame& game) {
    CatalogJson json;
    json["id"] = game.id.toString();
    json["system"] = game.system.str();
    json["title"] = game.title;
    json["sessions"] = game.sessions;
    json["playedMs"] = game.played.count();
    json["lastPlayed"] = game.lastPlayed ? CatalogJson(core::formatUtcIso8601(*game.lastPlayed)) : CatalogJson(nullptr);
    json["status"] = catalog::toString(game.status);
    return json;
}

[[nodiscard]] CatalogJson statisticsJson(const catalog::PlayStatistics& statistics) {
    CatalogJson json;
    json["sessions"] = statistics.sessions;
    json["crashed"] = statistics.crashed;
    json["leftRunning"] = statistics.leftRunning;
    json["playedMs"] = statistics.played.count();
    json["gamesPlayed"] = statistics.gamesPlayed;
    json["games"] = statistics.games;
    CatalogJson played = CatalogJson::array();
    for (const catalog::PlayedGame& game : statistics.mostPlayed) {
        played.push_back(gameJson(game));
    }
    json["mostPlayed"] = std::move(played);
    CatalogJson recent = CatalogJson::array();
    for (const catalog::PlayedGame& game : statistics.mostRecent) {
        recent.push_back(gameJson(game));
    }
    json["mostRecent"] = std::move(recent);
    CatalogJson systems = CatalogJson::array();
    for (const catalog::SystemStatistics& system : statistics.systems) {
        CatalogJson row;
        row["system"] = system.system.str();
        row["fullName"] = system.fullName;
        row["sessions"] = system.sessions;
        row["attempts"] = system.attempts;
        row["playedMs"] = system.played.count();
        row["games"] = system.games;
        systems.push_back(std::move(row));
    }
    json["systems"] = std::move(systems);
    CatalogJson statuses = CatalogJson::array();
    for (const catalog::StatusStatistics& status : statistics.statuses) {
        CatalogJson row;
        row["status"] = catalog::toString(status.status);
        row["games"] = status.games;
        row["sessions"] = status.sessions;
        row["playedMs"] = status.played.count();
        statuses.push_back(std::move(row));
    }
    json["statuses"] = std::move(statuses);
    return json;
}

[[nodiscard]] std::string statisticsText(const catalog::PlayStatistics& statistics) {
    std::string text = fmt::format("{} session{}, {:.1f} estimated hours, {} of {} games played.\n", statistics.sessions,
                                   statistics.sessions == 1 ? "" : "s", hoursOf(statistics.played),
                                   statistics.gamesPlayed, statistics.games);
    if (statistics.attempts() > 0) {
        text += fmt::format("Not counted in the hours: {} crashed, {} left running.\n", statistics.crashed,
                            statistics.leftRunning);
    }
    if (!statistics.mostPlayed.empty()) {
        text += "\nMost played:\n";
        for (const catalog::PlayedGame& game : statistics.mostPlayed) {
            text += fmt::format("{:>8.1f} h  {:>4} session{}  {} {}\n", hoursOf(game.played), game.sessions,
                                game.sessions == 1 ? " " : "s", game.system.str(), game.title);
        }
    }
    if (!statistics.mostRecent.empty()) {
        text += "\nMost recent:\n";
        for (const catalog::PlayedGame& game : statistics.mostRecent) {
            text += fmt::format("{:<24}  {} {}\n", game.lastPlayed ? core::formatUtcIso8601(*game.lastPlayed) : "-",
                                game.system.str(), game.title);
        }
    }
    if (!statistics.systems.empty()) {
        text += "\nPer system:\n";
        for (const catalog::SystemStatistics& system : statistics.systems) {
            text += fmt::format("{:>8.1f} h  {:>4} session{}  {:>3} attempt{}  {}\n", hoursOf(system.played),
                                system.sessions, system.sessions == 1 ? " " : "s", system.attempts,
                                system.attempts == 1 ? " " : "s", system.system.str());
        }
    }
    text += "\nPer status:\n";
    for (const catalog::StatusStatistics& status : statistics.statuses) {
        text += fmt::format("{:>8.1f} h  {:>4} session{}  {:>6} game{}  {}\n", hoursOf(status.played), status.sessions,
                            status.sessions == 1 ? " " : "s", status.games, status.games == 1 ? " " : "s",
                            catalog::toString(status.status));
    }
    return text;
}

} // namespace

ExitCode runStats(CommandContext& context) {
    core::ArgParser parser{"retroverse-cli stats"};
    parser.option("limit", "n", "At most this many games in each list (default 10).")
        .flag("json", "Write the results as JSON.");
    addOutputOption(parser);
    auto args = parser.parse(context.arguments);
    if (!args) {
        return usageError(context, args.error().toString());
    }
    if (args->has("help")) {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    auto file = outputFileOption(*args);
    if (!file) {
        return usageError(context, file.error().toString());
    }
    catalog::PlayStatisticsQuery query;
    const auto limit = args->intValue("limit", static_cast<int>(catalog::kDefaultStatisticsGames));
    if (!limit || *limit < 0 || *limit > kMaxLimit) {
        return usageError(context, fmt::format("--limit takes a number from 0 to {}", kMaxLimit));
    }
    query.limit = static_cast<std::size_t>(*limit);
    if (auto machine = catalog::machineLocalTimeToUtc()) {
        query.localToUtc = std::move(*machine);
    } else {
        spdlog::warn("The machine's time zone cannot be read ({}): ES-DE's times are read as UTC",
                     machine.error().toString());
        query.localToUtc = catalog::fixedOffsetLocalTimeToUtc(std::chrono::minutes(0));
    }
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        return catalogFailure(context, reader.error());
    }
    const auto statistics = reader->query->playStatistics(query);
    if (!statistics) {
        return catalogFailure(context, statistics.error());
    }
    return writeResults(context, *file, args->has("json") ? jsonText(statisticsJson(*statistics))
                                                          : statisticsText(*statistics),
                        ExitCode::Ok);
}

} // namespace rv::cli
