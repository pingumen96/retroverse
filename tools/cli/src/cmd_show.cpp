// retroverse-cli show: shows a game: metadata, files, media and launch readiness. `show <gameId>` prints everything the
// catalog holds about the game (I2 step 9, SqliteCatalogQuery::details(), one read on a read-only connection): its
// system and library, state and availability with their reasons, its file as listed and its path, the gamelist entry
// and its metadata, ES-DE's flags, the media kinds present, the raw ES-DE values kept. Launch readiness comes with the
// launch translation (I2 step 10) and media paths with the media resolver (step 8): neither is shown yet.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] show <gameId> [--json] [--output <file>]\n"
    "\n"
    "Shows everything the catalog (<user data>/database/retroverse.sqlite) holds about a game: its system, state and\n"
    "availability, its file and gamelist entry, metadata, ES-DE's flags, media kinds and the raw ES-DE values kept.\n"
    "<gameId> is the GameId as the other commands print it: 'g' and 16 hexadecimal digits.\n"
    "\n"
    "Options:\n"
    "  --json           Write the results as JSON.\n"
    "  --output <file>  Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 the catalog has no such game; 2 usage error; 3 the catalog database could not be read.\n";

/// 1970-01-01 in FILETIME ticks (100 ns since 1601-01-01 UTC).
constexpr std::int64_t kUnixEpochTicks = 116'444'736'000'000'000;

/// A file time (FILETIME ticks) as UTC ISO-8601 text.
std::string fileTimeText(std::int64_t ticks) {
    const catalog::FileTimeTicks sinceUnixEpoch(ticks - kUnixEpochTicks);
    return core::formatUtcIso8601(std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(sinceUnixEpoch)));
}

/// The ROM's path as ES-DE builds it: the system's ROM folder and the relative path.
std::string romPathText(const catalog_db::GameDetails& details) {
    if (!details.system) {
        return {};
    }
    std::filesystem::path path = details.system->romFolder / core::pathFromUtf8(details.indexed.entry.key.relativePath());
    path.make_preferred();
    return core::pathToUtf8Lossy(path);
}

CatalogJson rawJson(const std::vector<catalog::RawElement>& elements) {
    CatalogJson list = CatalogJson::array();
    for (const catalog::RawElement& element : elements) {
        list.push_back(CatalogJson::array({element.name, element.text}));
    }
    return list;
}

std::string detailsJson(const catalog_db::GameDetails& details) {
    const catalog::IndexedEntry& indexed = details.indexed;
    const catalog::CatalogEntry& entry = indexed.entry;
    const catalog::GameMetadata& metadata = entry.metadata;
    CatalogJson json;
    json["id"] = indexed.id.toString();
    json["library"] = details.library.str();
    json["system"] = entry.system.str();
    if (details.system) {
        json["systemFullName"] = details.system->fullName;
        json["romFolder"] = core::pathToUtf8Lossy(details.system->romFolder);
    } else {
        json["systemFullName"] = nullptr;
        json["romFolder"] = nullptr;
    }
    json["relativePath"] = entry.key.relativePath();
    json["path"] = details.system ? CatalogJson(romPathText(details)) : CatalogJson(nullptr);
    json["title"] = entry.title;
    json["sortKey"] = indexed.sortKey;
    json["state"] = catalog::toString(entry.state);
    json["notAGameReason"] = catalog::toString(entry.notAGameReason);
    json["excludingSetting"] = entry.excludingSetting;
    json["availability"] = catalog::toString(entry.availability);
    json["unavailableReason"] = catalog::toString(entry.unavailableReason);
    if (entry.file) {
        json["file"] = CatalogJson{{"directory", entry.file->directory},
                                   {"size", entry.file->size},
                                   {"lastWriteTime", fileTimeText(entry.file->lastWriteTime)},
                                   {"hidden", entry.file->hidden}};
    } else {
        json["file"] = nullptr;
    }
    json["gamelistPath"] = entry.gamelistPath;
    CatalogJson meta;
    meta["name"] = metadata.name;
    meta["sortName"] = metadata.sortName;
    meta["description"] = metadata.description;
    meta["developer"] = metadata.developer;
    meta["publisher"] = metadata.publisher;
    meta["genre"] = metadata.genre;
    meta["releaseDate"] = metadata.releaseDate ? CatalogJson(dateText(*metadata.releaseDate)) : CatalogJson(nullptr);
    meta["rating"] = metadata.rating ? CatalogJson(metadata.rating->tenths / 10.0) : CatalogJson(nullptr);
    meta["players"] = metadata.players ? CatalogJson(catalog::formatPlayers(*metadata.players)) : CatalogJson(nullptr);
    json["metadata"] = std::move(meta);
    json["discHead"] = entry.discHead.empty() ? CatalogJson(nullptr) : CatalogJson(entry.discHead.relativePath());
    json["altEmulator"] = entry.altEmulator;
    const catalog::EsdeFlags& flags = entry.flags;
    json["flags"] = CatalogJson{
        {"favorite", flags.favorite},
        {"hidden", flags.hidden},
        {"kidGame", flags.kidGame},
        {"completed", flags.completed},
        {"broken", flags.broken},
        {"playCount", flags.playCount},
        {"playTime", flags.playTimeSeconds},
        {"lastPlayed", flags.lastPlayed ? CatalogJson(catalog::formatEsdeDateTime(*flags.lastPlayed))
                                        : CatalogJson(nullptr)},
    };
    CatalogJson media = CatalogJson::array();
    for (const catalog::MediaKind kind : catalog::kMediaKinds) {
        if (indexed.media[kind].present) {
            media.push_back(CatalogJson{{"kind", catalog::toString(kind)}, {"extension", indexed.media[kind].extension}});
        }
    }
    json["media"] = std::move(media);
    json["raw"] = CatalogJson{{"unknown", rawJson(entry.raw.unknownElements)},
                              {"original", rawJson(entry.raw.originalForms)}};
    return jsonText(json);
}

std::string detailsText(const catalog_db::GameDetails& details) {
    const catalog::IndexedEntry& indexed = details.indexed;
    const catalog::CatalogEntry& entry = indexed.entry;
    const catalog::GameMetadata& metadata = entry.metadata;
    std::string text;
    const auto line = [&](std::string_view label, std::string_view value) {
        text += fmt::format("{:<22}{}\n", fmt::format("{}:", label), value);
    };
    line("Game", indexed.id.toString());
    line("Title", entry.title);
    line("System", details.system ? fmt::format("{} ({}), library {}", entry.system.str(), details.system->fullName,
                                                details.library.str())
                                  : fmt::format("{} (not in the catalog's systems), library {}", entry.system.str(),
                                                details.library.str()));
    std::string state(catalog::toString(entry.state));
    if (entry.state == catalog::EntryState::NotAGame) {
        state += fmt::format(" ({}{})", catalog::toString(entry.notAGameReason),
                             entry.excludingSetting.empty() ? "" : ": " + entry.excludingSetting);
    }
    line("State", state);
    std::string availability(catalog::toString(entry.availability));
    if (entry.availability == catalog::Availability::SourceUnavailable) {
        availability += fmt::format(" ({})", catalog::toString(entry.unavailableReason));
    }
    line("Availability", availability);
    line("Relative path", entry.key.relativePath());
    if (details.system) {
        line("Path", romPathText(details));
    }
    if (entry.file) {
        line("File", fmt::format("{}{} bytes, written {}{}", entry.file->directory ? "folder, " : "", entry.file->size,
                                 fileTimeText(entry.file->lastWriteTime), entry.file->hidden ? ", hidden" : ""));
    } else {
        line("File", "not seen by the last import");
    }
    if (!entry.discHead.empty()) {
        line("Disc of", entry.discHead.relativePath());
    }
    line("Gamelist entry", entry.gamelistPath.empty() ? "none (unscraped: titled from its file name)"
                                                      : entry.gamelistPath);
    line("Sort key", indexed.sortKey);
    if (const std::string sortName = catalog::displayText(metadata.sortName); !sortName.empty()) {
        line("Sort title", sortName);
    }
    if (metadata.releaseDate) {
        line("Release date", dateText(*metadata.releaseDate));
    }
    if (const std::string developer = catalog::displayText(metadata.developer); !developer.empty()) {
        line("Developer", developer);
    }
    if (const std::string publisher = catalog::displayText(metadata.publisher); !publisher.empty()) {
        line("Publisher", publisher);
    }
    if (const std::string genre = catalog::displayText(metadata.genre); !genre.empty()) {
        line("Genre", genre);
    }
    if (metadata.players) {
        line("Players", catalog::formatPlayers(*metadata.players));
    }
    if (metadata.rating) {
        line("Rating", catalog::formatRating(*metadata.rating));
    }
    const catalog::EsdeFlags& flags = entry.flags;
    std::vector<std::string_view> set;
    for (const auto& [on, name] : {std::pair{flags.favorite, "favorite"}, std::pair{flags.hidden, "hidden"},
                                   std::pair{flags.kidGame, "kid game"}, std::pair{flags.completed, "completed"},
                                   std::pair{flags.broken, "broken"}}) {
        if (on) {
            set.push_back(name);
        }
    }
    line("Flags", set.empty() ? std::string("none") : fmt::format("{}", fmt::join(set, ", ")));
    line("Played", fmt::format("{} time{}, {} s{}", flags.playCount, flags.playCount == 1 ? "" : "s",
                               flags.playTimeSeconds,
                               flags.lastPlayed ? ", last " + catalog::formatEsdeDateTime(*flags.lastPlayed) + " (local)"
                                                : std::string()));
    if (!entry.altEmulator.empty()) {
        line("Alternative emulator", entry.altEmulator);
    }
    std::vector<std::string> media;
    for (const catalog::MediaKind kind : catalog::kMediaKinds) {
        if (indexed.media[kind].present) {
            media.push_back(fmt::format("{} ({})", catalog::toString(kind), indexed.media[kind].extension));
        }
    }
    line("Media", media.empty() ? std::string("none") : fmt::format("{}", fmt::join(media, ", ")));
    for (const catalog::RawElement& element : entry.raw.unknownElements) {
        line("Gamelist element", fmt::format("<{}> {}", element.name, catalog::displayText(element.text)));
    }
    if (!metadata.description.empty()) {
        text += "Description:\n";
        std::string_view description = metadata.description;
        while (!description.empty()) {
            const std::size_t end = std::min(description.find('\n'), description.size());
            text += fmt::format("  {}\n", description.substr(0, end));
            description.remove_prefix(std::min(end + 1, description.size()));
        }
    }
    return text;
}

} // namespace

ExitCode runShow(CommandContext& context) {
    core::ArgParser parser{"retroverse-cli show"};
    parser.positional("gameId", "The GameId: 'g' and 16 hexadecimal digits.", true);
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
    const auto id = catalog::GameId::parse(args->positional("gameId").value_or(""));
    if (!id) {
        return usageError(context, id.error().toString());
    }
    auto file = outputFileOption(*args);
    if (!file) {
        return usageError(context, file.error().toString());
    }
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        return catalogFailure(context, reader.error());
    }
    const auto details = reader->query->details(*id);
    if (!details) {
        return catalogFailure(context, details.error());
    }
    if (!*details) {
        context.err.writeLine(fmt::format("retroverse-cli show: the catalog has no game {}", id->toString()));
        return ExitCode::ProblemsFound;
    }
    const std::string text = args->has("json") ? detailsJson(**details) : detailsText(**details);
    return writeResults(context, *file, text, ExitCode::Ok);
}

} // namespace rv::cli
