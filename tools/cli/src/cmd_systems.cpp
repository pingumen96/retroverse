// retroverse-cli systems: lists the systems of the catalog with their game counts (I2 step 9), in the order of the
// catalog's System sort (the full name folded as sort keys are, then the name): for each, the games a list shows by
// default (available games), the unavailable ones (missing files, games of a folder that could not be listed), every
// indexed entry, and whether its ROM folder could be listed and its gamelist read. Read through SqliteCatalogQuery on a
// read-only connection, in one read.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/utf8.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] systems [--json] [--output <file>]\n"
    "\n"
    "Lists the systems of the catalog (<user data>/database/retroverse.sqlite, made by an import) with their game\n"
    "counts: the games a list shows (available games), the unavailable ones (missing files, games of a folder that\n"
    "could not be listed) and every indexed entry (files that are no games, disc tracks and folders included).\n"
    "\n"
    "Options:\n"
    "  --json           Write the results as JSON.\n"
    "  --output <file>  Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 2 usage error; 3 the catalog database could not be read (missing: an import makes it).\n";

/// The code points of UTF-8 text (its columns, for the alignment of full names outside ASCII).
std::size_t codePoints(std::string_view utf8) noexcept {
    return static_cast<std::size_t>(
        std::count_if(utf8.begin(), utf8.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

} // namespace

ExitCode runSystems(CommandContext& context) {
    core::ArgParser parser{"retroverse-cli systems"};
    parser.flag("json", "Write the results as JSON.");
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
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        return catalogFailure(context, reader.error());
    }
    const auto systems = reader->query->systems();
    if (!systems) {
        return catalogFailure(context, systems.error());
    }

    std::size_t games = 0;
    std::size_t unavailable = 0;
    std::size_t entries = 0;
    bool severalLibraries = false;
    for (const catalog_db::SystemSummary& summary : *systems) {
        games += summary.games;
        unavailable += summary.unavailable;
        entries += summary.entries;
        severalLibraries = severalLibraries || summary.library != systems->front().library;
    }
    std::string text;
    if (args->has("json")) {
        CatalogJson list = CatalogJson::array();
        for (const catalog_db::SystemSummary& summary : *systems) {
            const catalog::SystemInfo& system = summary.system;
            CatalogJson platforms = CatalogJson::array();
            for (const std::string& platform : system.platforms) {
                platforms.push_back(platform);
            }
            CatalogJson json;
            json["library"] = summary.library.str();
            json["name"] = system.name.str();
            json["fullName"] = system.fullName;
            json["platforms"] = std::move(platforms);
            json["theme"] = system.theme;
            json["romFolderConfigured"] = system.romFolderConfigured;
            json["romFolder"] = core::pathToUtf8Lossy(system.romFolder);
            json["availability"] = catalog::toString(system.availability);
            json["unavailableReason"] = catalog::toString(system.unavailableReason);
            json["gamelist"] = catalog::toString(system.gamelist);
            json["alternativeEmulator"] = system.alternativeEmulator;
            json["games"] = summary.games;
            json["unavailable"] = summary.unavailable;
            json["entries"] = summary.entries;
            list.push_back(std::move(json));
        }
        CatalogJson json;
        json["systems"] = std::move(list);
        json["count"] = systems->size();
        json["games"] = games;
        json["unavailable"] = unavailable;
        json["entries"] = entries;
        text = jsonText(json);
    } else {
        // name  full name  games  unavailable  entries  [notes]
        const auto label = [&](const catalog_db::SystemSummary& summary) {
            return severalLibraries ? fmt::format("{}:{}", summary.library.str(), summary.system.name.str())
                                    : summary.system.name.str();
        };
        std::size_t nameWidth = 0;
        std::size_t fullNameWidth = 0;
        for (const catalog_db::SystemSummary& summary : *systems) {
            nameWidth = std::max(nameWidth, label(summary).size());
            fullNameWidth = std::max(fullNameWidth, codePoints(summary.system.fullName));
        }
        for (const catalog_db::SystemSummary& summary : *systems) {
            const catalog::SystemInfo& system = summary.system;
            const std::size_t padding = fullNameWidth - codePoints(system.fullName);
            text += fmt::format("{:<{}}  {}{}  {:>6} games  {:>6} unavailable  {:>6} entries", label(summary),
                                nameWidth, system.fullName, std::string(padding, ' '), summary.games,
                                summary.unavailable, summary.entries);
            if (system.availability != catalog::Availability::Available) {
                text += fmt::format("  [{}: {}]", catalog::toString(system.availability),
                                    catalog::toString(system.unavailableReason));
            }
            if (system.gamelist != catalog::GamelistState::Read) {
                text += fmt::format("  [gamelist {}]", catalog::toString(system.gamelist));
            }
            text += '\n';
        }
        text += fmt::format("{} system{}: {} game{}, {} unavailable, {} entr{}\n", systems->size(),
                            systems->size() == 1 ? "" : "s", games, games == 1 ? "" : "s", unavailable, entries,
                            entries == 1 ? "y" : "ies");
    }
    return writeResults(context, *file, text, ExitCode::Ok);
}

} // namespace rv::cli
