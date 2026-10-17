// retroverse-cli games: lists games, filtered and sorted, a page at a time (I2 step 9). retroverse-cli search shares the
// list (runGameList()) and adds a search text. Both read the catalog database of the user data directory through
// SqliteCatalogQuery on a read-only connection: the list is materialized in one read (ADR-0008 §5), the page shown
// fetched by ids. Also the helpers of catalog_text.hpp, shared with systems and show.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kListOptions =
    "  --system <name>        Only the games of this system (its ES-DE name: gb, snes, psx...).\n"
    "  --state <states>       Entry states, comma-separated: game, missing-file, not-a-game, disc-member,\n"
    "                         folder (default: game and missing-file).\n"
    "  --availability <list>  Availabilities, comma-separated: available, source-unavailable, missing; an\n"
    "                         unavailable one implies --include-unavailable.\n"
    "  --include-unavailable  Also missing files and the games of folders that could not be listed (hidden by\n"
    "                         default).\n"
    "  --sort <order>         title (default), system or release-date: the sort key, then the GameId, breaks\n"
    "                         every tie.\n"
    "  --offset <n>           Skip the first n games of the list (default 0).\n"
    "  --limit <n>            Show at most n games (default: all).\n"
    "  --json                 Write the results as JSON.\n"
    "  --output <file>        Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help             Show this help and exit.\n";

constexpr std::string_view kGamesUsage =
    "Usage: retroverse-cli [global options] games [--system <name>] [--state <states>] [--availability <list>]\n"
    "           [--include-unavailable] [--sort <order>] [--offset <n>] [--limit <n>] [--json] [--output <file>]\n"
    "\n"
    "Lists the games of the catalog (<user data>/database/retroverse.sqlite, made by an import): one line each,\n"
    "its GameId, system, title, year and, when it is not an available game, its state; then the count.\n"
    "\n"
    "Options:\n";

constexpr std::string_view kSearchUsage =
    "Usage: retroverse-cli [global options] search <text> [--system <name>] [--state <states>]\n"
    "           [--availability <list>] [--include-unavailable] [--sort <order>] [--offset <n>] [--limit <n>]\n"
    "           [--json] [--output <file>]\n"
    "\n"
    "Searches the games of the catalog by title, sort title, developer, publisher, genre, year and system name:\n"
    "every word of <text> must match a whole word, in any order, the last one the start of a word; case and\n"
    "diacritics are ignored ('eclair' finds 'Eclair'). A single character finds the titles that start with it.\n"
    "Text is never search syntax; put '--' before a text that starts with '-'.\n"
    "\n"
    "Options:\n";

constexpr std::string_view kExitCodes =
    "\nExit codes: 0 ok (also when nothing matches); 2 usage error, or a system the catalog does not have; 3 the\n"
    "catalog database could not be read (missing: an import makes it).\n";

/// The items of a comma-separated option, trimmed, the empty ones dropped.
std::vector<std::string> listItems(std::string_view text) {
    std::vector<std::string> items;
    for (const std::string_view item : core::split(text, ',')) {
        if (const std::string_view trimmed = core::trim(item); !trimmed.empty()) {
            items.emplace_back(trimmed);
        }
    }
    return items;
}

/// A count option: a whole number from 0; nothing when not given.
Expected<std::optional<std::size_t>> countOption(const core::ParsedArgs& args, std::string_view name) {
    if (!args.has(name)) {
        return std::optional<std::size_t>{};
    }
    const auto value = args.intValue(name, 0);
    if (!value || *value < 0) {
        return makeError(ErrorCode::InvalidArgument, "expected a whole number from 0", "--" + std::string(name));
    }
    return std::optional<std::size_t>(static_cast<std::size_t>(*value));
}

/// "1 game", "2 games"; "entry", "entries" for lists of other states.
std::string countText(std::size_t count, bool games) {
    if (games) {
        return fmt::format("{} game{}", count, count == 1 ? "" : "s");
    }
    return fmt::format("{} entr{}", count, count == 1 ? "y" : "ies");
}

} // namespace

// --- catalog_text.hpp ------------------------------------------------------------------------------------------------

Expected<CatalogReader> openCatalogReader(const core::Paths& paths) {
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.switchToWal = false; // reading writes nothing into the file
    auto database = catalog_db::Database::open(catalog_db::databaseFile(paths), options);
    if (!database) {
        return propagate(database.error());
    }
    auto connection = database->openReader();
    if (!connection) {
        return propagate(connection.error());
    }
    auto query = catalog_db::SqliteCatalogQuery::open(std::move(*connection));
    if (!query) {
        return propagate(query.error());
    }
    CatalogReader reader;
    reader.database = std::make_unique<catalog_db::Database>(std::move(*database));
    reader.query = std::move(*query);
    return reader;
}

ExitCode catalogFailure(CommandContext& context, const Error& error) {
    context.err.writeLine(
        fmt::format("retroverse-cli {}: cannot read the catalog: {}", context.command.name, error.toString()));
    if (error.code == ErrorCode::NotFound) {
        context.err.writeLine("There is no catalog database yet: 'retroverse-cli import' makes it.");
    } else if (error.code == ErrorCode::Conflict && error.message.find("migrate it first") != std::string::npos) {
        context.err.writeLine("Run 'retroverse-cli db migrate' first.");
    }
    return ExitCode::RuntimeFailure;
}

ExitCode writeResults(CommandContext& context, std::optional<Output>& file, const std::string& text, ExitCode code) {
    if (!file) {
        context.out.write(text);
        return code;
    }
    file->write(text);
    if (auto finished = file->finish(); !finished) {
        context.err.writeLine(fmt::format("retroverse-cli {}: cannot write the results: {}", context.command.name,
                                          finished.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    return code;
}

ExitCode usageError(CommandContext& context, std::string_view message) {
    context.err.writeLine(fmt::format("retroverse-cli {}: {}", context.command.name, message));
    context.err.writeLine(fmt::format("Run 'retroverse-cli {} --help' for its options.", context.command.name));
    return ExitCode::UsageError;
}

std::string jsonText(const CatalogJson& json) {
    return json.dump(2, ' ', false, CatalogJson::error_handler_t::replace) + "\n";
}

std::string dateText(const catalog::ReleaseDate& date) {
    return fmt::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

CatalogJson rowJson(const catalog::CatalogRow& row) {
    CatalogJson json;
    json["id"] = row.id.toString();
    json["system"] = row.system.str();
    json["systemFullName"] = row.systemFullName;
    json["title"] = row.title;
    json["state"] = catalog::toString(row.state);
    json["availability"] = catalog::toString(row.availability);
    json["releaseDate"] = row.releaseDate ? CatalogJson(dateText(*row.releaseDate)) : CatalogJson(nullptr);
    json["year"] = row.releaseDate ? CatalogJson(row.releaseDate->year) : CatalogJson(nullptr);
    json["developer"] = row.developer;
    json["publisher"] = row.publisher;
    json["genre"] = row.genre;
    json["players"] = row.players ? CatalogJson(catalog::formatPlayers(*row.players)) : CatalogJson(nullptr);
    json["rating"] = row.rating ? CatalogJson(row.rating->tenths / 10.0) : CatalogJson(nullptr);
    json["favorite"] = row.favorite;
    json["description"] = row.description;
    return json;
}

// --- games and search ------------------------------------------------------------------------------------------------

ExitCode runGameList(CommandContext& context, bool search) {
    const std::string_view name = context.command.name;
    core::ArgParser parser{fmt::format("retroverse-cli {}", name)};
    if (search) {
        parser.positional("text", "What to search.", true);
    }
    parser.option("system", "name", "Only the games of this system.")
        .option("state", "states", "Entry states, comma-separated.")
        .option("availability", "list", "Availabilities, comma-separated.")
        .flag("include-unavailable", "Also the unavailable entries.")
        .option("sort", "order", "title, system or release-date.", "title")
        .option("offset", "n", "Skip the first n games.")
        .option("limit", "n", "Show at most n games.")
        .flag("json", "Write the results as JSON.");
    addOutputOption(parser);
    auto args = parser.parse(context.arguments);
    if (!args) {
        return usageError(context, args.error().toString());
    }
    if (args->has("help")) {
        context.out.write(std::string(search ? kSearchUsage : kGamesUsage) + std::string(kListOptions) +
                          std::string(kExitCodes));
        return ExitCode::Ok;
    }

    catalog::CatalogQuery query;
    if (search) {
        query.text = args->positional("text").value_or("");
    }
    if (args->has("system")) {
        auto system = catalog::SystemName::parse(args->value("system").value_or(""));
        if (!system) {
            return usageError(context, fmt::format("--system: {}", system.error().message));
        }
        query.systems.push_back(std::move(*system));
    }
    for (const std::string& item : listItems(args->value("state").value_or(""))) {
        const auto state = catalog::parseEntryState(item);
        if (!state) {
            return usageError(context, fmt::format("--state: unknown state '{}' (game, missing-file, not-a-game, "
                                                   "disc-member, folder)",
                                                   item));
        }
        query.states.push_back(*state);
    }
    for (const std::string& item : listItems(args->value("availability").value_or(""))) {
        const auto availability = catalog::parseAvailability(item);
        if (!availability) {
            return usageError(context, fmt::format("--availability: unknown availability '{}' (available, "
                                                   "source-unavailable, missing)",
                                                   item));
        }
        query.availabilities.push_back(*availability);
        // Naming an unavailable availability asks for it (the query lets such entries through only when asked).
        query.includeUnavailable = query.includeUnavailable || *availability != catalog::Availability::Available;
    }
    query.includeUnavailable = query.includeUnavailable || args->has("include-unavailable");
    const std::string sortText = args->value("sort").value_or("title");
    const auto sort = catalog::parseSortOrder(sortText);
    if (!sort) {
        return usageError(context, fmt::format("--sort: unknown order '{}' (title, system, release-date)", sortText));
    }
    query.sort = *sort;
    const auto offset = countOption(*args, "offset");
    const auto limit = countOption(*args, "limit");
    if (!offset || !limit) {
        return usageError(context, (!offset ? offset.error() : limit.error()).toString());
    }
    auto file = outputFileOption(*args);
    if (!file) {
        return usageError(context, file.error().toString());
    }

    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        return catalogFailure(context, reader.error());
    }
    catalog_db::SqliteCatalogQuery& catalog = *reader->query;
    if (!query.systems.empty()) {
        // A system the catalog does not have is a mistake worth naming, not an empty list.
        const auto systems = catalog.systems();
        if (!systems) {
            return catalogFailure(context, systems.error());
        }
        const bool known = std::any_of(systems->begin(), systems->end(), [&](const catalog_db::SystemSummary& s) {
            return s.system.name == query.systems.front();
        });
        if (!known) {
            context.err.writeLine(fmt::format("retroverse-cli {}: the catalog has no system '{}' ('retroverse-cli "
                                              "systems' lists them)",
                                              name, query.systems.front().str()));
            return ExitCode::UsageError;
        }
    }
    const auto plan = catalog.searchPlan(query.text);
    if (!plan) {
        return catalogFailure(context, plan.error());
    }
    const auto ids = catalog.run(query);
    if (!ids) {
        return catalogFailure(context, ids.error());
    }
    const std::size_t first = offset->value_or(0);
    const std::span<const catalog::GameId> page = catalog::pageOf(*ids, first, limit->value_or(ids->size()));
    const auto rows = catalog.fetch(page);
    if (!rows) {
        return catalogFailure(context, rows.error());
    }

    const bool games = query.states.empty();
    std::string text;
    if (args->has("json")) {
        CatalogJson request;
        request["text"] = query.text;
        CatalogJson words = CatalogJson::array();
        for (const std::string& word : plan->words) {
            words.push_back(word);
        }
        request["search"] = plan->kind == catalog_db::SearchPlan::Kind::Words            ? "words"
                            : plan->kind == catalog_db::SearchPlan::Kind::FirstCharacter ? "first-character"
                                                                                         : "everything";
        request["words"] = words;
        request["systems"] = CatalogJson::array();
        for (const catalog::SystemName& system : query.systems) {
            request["systems"].push_back(system.str());
        }
        request["states"] = CatalogJson::array();
        for (const catalog::EntryState state : query.states) {
            request["states"].push_back(catalog::toString(state));
        }
        request["availabilities"] = CatalogJson::array();
        for (const catalog::Availability availability : query.availabilities) {
            request["availabilities"].push_back(catalog::toString(availability));
        }
        request["includeUnavailable"] = query.includeUnavailable;
        request["sort"] = catalog::toString(query.sort);
        CatalogJson list = CatalogJson::array();
        for (const catalog::CatalogRow& row : *rows) {
            list.push_back(rowJson(row));
        }
        CatalogJson json;
        json["query"] = std::move(request);
        json["count"] = ids->size();
        json["offset"] = first;
        json["limit"] = limit->has_value() ? CatalogJson(**limit) : CatalogJson(nullptr);
        json["games"] = std::move(list);
        text = jsonText(json);
    } else {
        std::size_t systemWidth = 0;
        for (const catalog::CatalogRow& row : *rows) {
            systemWidth = std::max(systemWidth, row.system.str().size());
        }
        for (const catalog::CatalogRow& row : *rows) {
            text += fmt::format("{}  {:<{}}  {}", row.id.toString(), row.system.str(), systemWidth, row.title);
            if (row.releaseDate) {
                text += fmt::format(" ({})", row.releaseDate->year);
            }
            if (row.state != catalog::EntryState::Game) {
                text += fmt::format(" [{}]", catalog::toString(row.state));
            }
            if (row.availability != catalog::Availability::Available) {
                text += fmt::format(" [{}]", catalog::toString(row.availability));
            }
            text += '\n';
        }
        if (first == 0 && page.size() == ids->size()) {
            text += countText(ids->size(), games) + "\n";
        } else if (page.empty()) {
            text += fmt::format("None from position {}: the list holds {}\n", first + 1, countText(ids->size(), games));
        } else {
            text += fmt::format("{} to {} of {}\n", first + 1, first + page.size(), countText(ids->size(), games));
        }
    }
    return writeResults(context, *file, text, ExitCode::Ok);
}

ExitCode runGames(CommandContext& context) {
    return runGameList(context, false);
}

} // namespace rv::cli
