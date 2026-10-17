// retroverse-cli identities (ADR-0008 §4; I2 step 11b): the catalog's identities and their reconciliation.
//  - identities list --system <name>: the identities of a system of the library library.json records, each with its
//    state (present, missing: its file gone since an import saw it, unseen: no import saw its file), the quick hash's
//    state (current, stale, none), its size and its last path.
//  - identities show <gameId>: one identity: its state, last path and key, file facts, quick hash, the aliases that
//    stand for it, its history (moves with their reason, aliases, keys computed again, reviews resolved) and its review
//    rows; a GameId that is an alias names the identity it stands for, shown below it.
//  - identities reviews [--system <name>] [--all]: the open review rows (with --all, the resolved ones too), each with
//    its vanished identity, the candidates (with the GameId each has now) and why no pairing was safe.
//  - identities resolve <review> (--with <gameId> | --keep-separate): resolves a review row (catalog::resolveReview(),
//    under the import lock): paired, the vanished identity takes the chosen identity's file and the chosen one becomes
//    its alias, as an import applies a pairing; kept separate, nothing changes but the row.
//  - identities hash [--system <name>] [--limit <files>]: the progressive quick-hash pass (catalog::runQuickHashPass(),
//    under the import lock): the present files without a current quick hash, at most 128 KiB read each and never
//    written, committed in batches; resumable (a later run hashes what is left). Progress goes to the log (stderr at
//    --log-level info or below, and the log file).
// Each takes --json and --output <file>. list, show and reviews only read the catalog (nothing is written into it);
// resolve and hash write state rows and refuse while an import (of this or another process) holds the import lock.
// Exit codes: 0; 1 show: the catalog has no such identity or alias; resolve: no such review row, resolved already, or
// its vanished identity's file is present again; hash: files that could not be read or reached; 2 usage errors (no
// library.json, a library.json naming another library than the catalog's, a system without identities, an identity to
// pair with that cannot be: an alias, of another system, its file not present); 3 the catalog cannot be read or
// written, another import runs, the results cannot be written.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/fingerprint.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

#include "commands.hpp"
#include "library_snapshot.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli identities";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] identities <subcommand> [options] [--json] [--output <file>]\n"
    "\n"
    "The catalog's identities (<user data>/database/retroverse.sqlite) and their reconciliation: files moved or\n"
    "renamed keep their GameId, by their quick hash or a unique size, time and name; what no rule pairs safely waits\n"
    "in a review row for the user.\n"
    "\n"
    "Subcommands:\n"
    "  list --system <name>               The identities of a system: state, quick hash, size and last path.\n"
    "  show <gameId>                      One identity: state, last path, file, quick hash, aliases, history and\n"
    "                                     review rows (an alias: the identity it stands for).\n"
    "  reviews [--system <name>] [--all]  The open review rows with their candidates (--all: resolved ones too).\n"
    "  resolve <review> --with <gameId>   Pair the review's vanished identity with that identity, which becomes its\n"
    "                                     alias (the move applied as an import applies it).\n"
    "  resolve <review> --keep-separate   Keep them apart: the row is resolved, nothing else changes.\n"
    "  hash [--system <name>] [--limit <files>]\n"
    "                                     Compute the quick hashes the catalog lacks (128 KiB read per file, never\n"
    "                                     written), committed in batches; a later run goes on with what is left.\n"
    "\n"
    "Options:\n"
    "  --json           Write the results as JSON.\n"
    "  --output <file>  Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 1 show: no such identity; resolve: no such review row, resolved already, or its file is\n"
    "present again; hash: files that could not be read; 2 usage error (no library.json, another library, a system\n"
    "without identities, an identity that cannot be paired with); 3 the catalog could not be read or written, another\n"
    "import runs, or the results could not be written.\n";

/// A subcommand's parsed arguments.
struct Arguments {
    std::string subcommand;
    core::ParsedArgs args;
    bool json = false;
    std::optional<Output> file;
};

/// The catalog opened with its store: for reading only (nothing written into the file, never made) or for writing.
struct Catalog {
    std::unique_ptr<catalog_db::Database> database;
    std::unique_ptr<catalog_db::SqliteCatalogStore> store;
};

std::string subcommandName(std::string_view subcommand) {
    return fmt::format("{} {}", kName, subcommand);
}

ExitCode failure(CommandContext& context, std::string_view subcommand, ExitCode code, std::string_view message) {
    context.err.writeLine(fmt::format("{}: {}", subcommandName(subcommand), message));
    return code;
}

Expected<Catalog> openCatalog(const core::Paths& paths, bool write) {
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.switchToWal = write; // looking at the catalog writes nothing into the file
    auto database = catalog_db::Database::open(catalog_db::databaseFile(paths), options);
    if (!database) {
        return propagate(database.error());
    }
    Catalog catalog;
    catalog.database = std::make_unique<catalog_db::Database>(std::move(*database));
    auto store = catalog_db::SqliteCatalogStore::open(*catalog.database);
    if (!store) {
        return propagate(store.error());
    }
    catalog.store = std::move(*store);
    return catalog;
}

ExitCode catalogError(CommandContext& context, std::string_view subcommand, const Error& error) {
    context.err.writeLine(fmt::format("{}: cannot use the catalog: {}", subcommandName(subcommand), error.toString()));
    if (error.code == ErrorCode::NotFound) {
        context.err.writeLine("There is no catalog database yet: 'retroverse-cli import' makes it.");
    } else if (error.code == ErrorCode::Conflict && error.message.find("migrate it first") != std::string::npos) {
        context.err.writeLine("Run 'retroverse-cli db migrate' first.");
    }
    return ExitCode::RuntimeFailure;
}

/// The library library.json records, which the catalog must hold (or hold nothing). On failure the message is on
/// stderr and `code` says how to exit.
std::optional<catalog::LibraryId> recordedLibrary(CommandContext& context, std::string_view subcommand,
                                                  catalog::ICatalogStore& store, ExitCode& code) {
    const std::string name = subcommandName(subcommand);
    const auto profile = readRecordedLibrary(context, name, code);
    if (!profile) {
        return std::nullopt;
    }
    const auto libraries = store.libraries();
    if (!libraries) {
        code = catalogError(context, subcommand, libraries.error());
        return std::nullopt;
    }
    if (!libraries->empty() &&
        std::none_of(libraries->begin(), libraries->end(),
                     [&](const catalog::LibraryProfile& library) { return library.id == profile->id; })) {
        code = failure(context, subcommand, ExitCode::UsageError,
                       fmt::format("library.json names the library '{}', but the catalog holds '{}': name it in "
                                   "library.json again (retroverse-cli library create --id {} --force)",
                                   profile->id.str(), libraries->front().id.str(), libraries->front().id.str()));
        return std::nullopt;
    }
    return profile->id;
}

ExitCode deliver(CommandContext& context, Arguments& arguments, const std::string& text, ExitCode code) {
    if (!arguments.file) {
        context.out.write(text);
        return code;
    }
    arguments.file->write(text);
    if (auto finished = arguments.file->finish(); !finished) {
        return failure(context, arguments.subcommand, ExitCode::RuntimeFailure,
                       fmt::format("cannot write the results: {}", finished.error().toString()));
    }
    return code;
}

std::string jsonText(const Json& json) {
    return json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

/// 1970-01-01 in FILETIME ticks (100 ns since 1601-01-01 UTC).
constexpr std::int64_t kUnixEpochTicks = 116'444'736'000'000'000;

/// A file time (FILETIME ticks) as UTC ISO-8601 text.
std::string fileTimeText(std::int64_t ticks) {
    const catalog::FileTimeTicks sinceUnixEpoch(ticks - kUnixEpochTicks);
    return core::formatUtcIso8601(std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(sinceUnixEpoch)));
}

/// "present", "missing" (its file gone since an import saw it), "unseen" (no import saw its file).
std::string_view stateOf(const catalog::StoredIdentity& identity) {
    return identity.present ? "present" : identity.file ? "missing" : "unseen";
}

/// "current", "stale", "none", "folder".
std::string_view hashStateOf(const catalog::StoredIdentity& identity) {
    if (identity.file && identity.file->directory) {
        return "folder";
    }
    if (!identity.quickHash) {
        return "none";
    }
    return catalog::currentQuickHash(identity) ? "current" : "stale";
}

Json identityJson(const catalog::StoredIdentity& identity) {
    Json json{
        {"id", identity.id.toString()},
        {"library", identity.library.str()},
        {"system", identity.system.str()},
        {"relativePath", identity.key.relativePath()},
        {"key", identity.key.key()},
        {"keyVersion", Json{{"rule", identity.keyVersion.rule}, {"casingTable", identity.keyVersion.casingTable}}},
        {"state", stateOf(identity)},
        {"present", identity.present}};
    json["file"] = identity.file ? Json{{"directory", identity.file->directory},
                                        {"size", identity.file->size},
                                        {"lastWriteTime", fileTimeText(identity.file->lastWriteTime)},
                                        {"hidden", identity.file->hidden}}
                                 : Json(nullptr);
    json["quickHash"] = identity.quickHash ? Json{{"digest", catalog::toHex(identity.quickHash->digest)},
                                                  {"size", identity.quickHash->size},
                                                  {"lastWriteTime", fileTimeText(identity.quickHash->lastWriteTime)},
                                                  {"current", catalog::currentQuickHash(identity).has_value()}}
                                           : Json(nullptr);
    json["quickHashState"] = hashStateOf(identity);
    return json;
}

Json aliasJson(const catalog::StoredAlias& alias) {
    return Json{{"alias", alias.alias.toString()},
                {"game", alias.game.toString()},
                {"relativePath", alias.key.relativePath()},
                {"at", alias.at},
                {"reason", catalog::toString(alias.reason)}};
}

Json eventJson(const catalog::IdentityEvent& event) {
    const auto text = [](const std::string& value) { return value.empty() ? Json(nullptr) : Json(value); };
    return Json{{"id", event.id},
                {"game", event.game.toString()},
                {"at", event.at},
                {"event", catalog::toString(event.kind)},
                {"reason", event.reason ? Json(catalog::toString(*event.reason)) : Json(nullptr)},
                {"fromKey", text(event.fromKey)},
                {"fromPath", text(event.fromPath)},
                {"toKey", text(event.toKey)},
                {"toPath", text(event.toPath)},
                {"alias", event.alias ? Json(event.alias->toString()) : Json(nullptr)},
                {"review", event.review ? Json(*event.review) : Json(nullptr)},
                {"detail", event.detail}};
}

std::string eventText(const catalog::IdentityEvent& event) {
    std::string text = fmt::format("{} {}", event.at, catalog::toString(event.kind));
    if (event.reason) {
        text += fmt::format(" ({})", catalog::toString(*event.reason));
    }
    switch (event.kind) {
    case catalog::IdentityEventKind::Moved:
        text += fmt::format(": {} -> {}", event.fromPath, event.toPath);
        break;
    case catalog::IdentityEventKind::Alias:
        text += fmt::format(": {}, which was {}", event.alias ? event.alias->toString() : std::string("?"),
                            event.fromPath);
        break;
    case catalog::IdentityEventKind::Rekeyed:
        text += fmt::format(": {}", event.toPath);
        break;
    case catalog::IdentityEventKind::ReviewResolved:
        break;
    }
    if (!event.detail.empty()) {
        text += fmt::format("; {}", event.detail);
    }
    return text;
}

/// The GameIds the identities of a system have now, by exact relative path (a review's candidates).
class IdentitiesByPath {
public:
    IdentitiesByPath(catalog::ICatalogStore& store, catalog::LibraryId library)
        : m_store(store), m_library(std::move(library)) {}

    Expected<std::optional<catalog::GameId>> find(const catalog::SystemName& system, const catalog::LookupKey& key) {
        auto known = m_systems.find(system);
        if (known == m_systems.end()) {
            auto identities = m_store.identities(m_library, system);
            if (!identities) {
                return propagate(identities.error());
            }
            std::map<std::string, catalog::GameId> byPath;
            for (const catalog::StoredIdentity& identity : *identities) {
                byPath.emplace(identity.key.relativePath(), identity.id);
            }
            known = m_systems.emplace(system, std::move(byPath)).first;
        }
        const auto found = known->second.find(key.relativePath());
        return found == known->second.end() ? std::nullopt : std::optional<catalog::GameId>(found->second);
    }

private:
    catalog::ICatalogStore& m_store;
    catalog::LibraryId m_library;
    std::map<catalog::SystemName, std::map<std::string, catalog::GameId>> m_systems;
};

Expected<Json> reviewJson(const catalog::StoredReview& stored, IdentitiesByPath& identities) {
    const catalog::ReviewRow& review = stored.review;
    Json candidates = Json::array();
    for (const catalog::LookupKey& candidate : review.candidates) {
        auto id = identities.find(review.system, candidate);
        if (!id) {
            return propagate(id.error());
        }
        candidates.push_back(Json{{"relativePath", candidate.relativePath()},
                                  {"game", *id ? Json((*id)->toString()) : Json(nullptr)}});
    }
    return Json{{"review", review.id},
                {"game", review.game.toString()},
                {"system", review.system.str()},
                {"vanished", review.vanished.relativePath()},
                {"candidates", std::move(candidates)},
                {"reason", review.reason},
                {"createdAt", stored.createdAt},
                {"open", stored.open()},
                {"resolvedAt", stored.resolvedAt ? Json(*stored.resolvedAt) : Json(nullptr)},
                {"resolution", stored.resolution ? Json(catalog::toString(*stored.resolution)) : Json(nullptr)},
                {"resolvedWith", stored.resolvedWith ? Json(stored.resolvedWith->toString()) : Json(nullptr)}};
}

Expected<std::string> reviewText(const catalog::StoredReview& stored, IdentitiesByPath& identities) {
    const catalog::ReviewRow& review = stored.review;
    std::string text = fmt::format("Review {} ({}, made {}): {} {}: {} vanished\n", review.id,
                                   stored.open() ? std::string("open")
                                                 : fmt::format("{} {}{}", catalog::toString(*stored.resolution),
                                                               stored.resolvedAt.value_or(std::string()),
                                                               stored.resolvedWith
                                                                   ? " with " + stored.resolvedWith->toString()
                                                                   : std::string()),
                                   stored.createdAt, review.game.toString(), review.system.str(),
                                   review.vanished.relativePath());
    text += fmt::format("  why: {}\n", review.reason);
    for (const catalog::LookupKey& candidate : review.candidates) {
        auto id = identities.find(review.system, candidate);
        if (!id) {
            return propagate(id.error());
        }
        text += fmt::format("  candidate {}: {}\n", *id ? (*id)->toString() : std::string("(no identity)"),
                            candidate.relativePath());
    }
    return text;
}

// --- list ------------------------------------------------------------------------------------------------------------

ExitCode listIdentities(CommandContext& context, Arguments& arguments) {
    const std::optional<std::string> systemText = arguments.args.value("system");
    if (!systemText) {
        return failure(context, arguments.subcommand, ExitCode::UsageError, "--system <name> is required");
    }
    const auto system = catalog::SystemName::parse(*systemText);
    if (!system) {
        return failure(context, arguments.subcommand, ExitCode::UsageError,
                       fmt::format("--system: {}", system.error().toString()));
    }
    auto catalog = openCatalog(context.paths, false);
    if (!catalog) {
        return catalogError(context, arguments.subcommand, catalog.error());
    }
    ExitCode code = ExitCode::Ok;
    const auto library = recordedLibrary(context, arguments.subcommand, *catalog->store, code);
    if (!library) {
        return code;
    }
    const auto identities = catalog->store->identities(*library, *system);
    if (!identities) {
        return catalogError(context, arguments.subcommand, identities.error());
    }
    if (identities->empty()) {
        return failure(context, arguments.subcommand, ExitCode::UsageError,
                       fmt::format("the library '{}' has no identity of the system '{}'", library->str(),
                                   system->str()));
    }
    std::map<std::string_view, std::size_t> states;
    std::map<std::string_view, std::size_t> hashes;
    for (const catalog::StoredIdentity& identity : *identities) {
        ++states[stateOf(identity)];
        ++hashes[hashStateOf(identity)];
    }
    if (arguments.json) {
        Json list = Json::array();
        for (const catalog::StoredIdentity& identity : *identities) {
            list.push_back(identityJson(identity));
        }
        Json counts = Json::object();
        for (const auto& [state, count] : states) {
            counts[std::string(state)] = count;
        }
        Json hashCounts = Json::object();
        for (const auto& [state, count] : hashes) {
            hashCounts[std::string(state)] = count;
        }
        return deliver(context, arguments,
                       jsonText(Json{{"library", library->str()},
                                     {"system", system->str()},
                                     {"count", identities->size()},
                                     {"states", std::move(counts)},
                                     {"quickHashes", std::move(hashCounts)},
                                     {"identities", std::move(list)}}),
                       ExitCode::Ok);
    }
    std::string text = fmt::format("Identities of {} in library {}: {} ({} present, {} missing, {} unseen; quick "
                                   "hashes: {} current, {} stale, {} none)\n\n",
                                   system->str(), library->str(), identities->size(), states["present"],
                                   states["missing"], states["unseen"], hashes["current"], hashes["stale"],
                                   hashes["none"]);
    text += fmt::format("{:<19}{:<9}{:<9}{:>12}  {}\n", "GameId", "State", "Hash", "Size", "Last path");
    for (const catalog::StoredIdentity& identity : *identities) {
        text += fmt::format("{:<19}{:<9}{:<9}{:>12}  {}\n", identity.id.toString(), stateOf(identity),
                            hashStateOf(identity),
                            identity.file && !identity.file->directory ? std::to_string(identity.file->size)
                                                                       : std::string("-"),
                            identity.key.relativePath());
    }
    return deliver(context, arguments, text, ExitCode::Ok);
}

// --- show ------------------------------------------------------------------------------------------------------------

ExitCode showIdentity(CommandContext& context, Arguments& arguments) {
    const std::optional<std::string> idText = arguments.args.positional("gameId");
    if (!idText) {
        return failure(context, arguments.subcommand, ExitCode::UsageError, "a GameId is required");
    }
    const auto id = catalog::GameId::parse(*idText);
    if (!id) {
        return failure(context, arguments.subcommand, ExitCode::UsageError, id.error().toString());
    }
    auto catalog = openCatalog(context.paths, false);
    if (!catalog) {
        return catalogError(context, arguments.subcommand, catalog.error());
    }
    catalog::ICatalogStore& store = *catalog->store;
    // A GameId that is an alias names the identity it stands for.
    std::optional<catalog::StoredAlias> asAlias;
    auto identity = store.identity(*id);
    if (!identity) {
        return catalogError(context, arguments.subcommand, identity.error());
    }
    if (!*identity) {
        auto alias = store.alias(*id);
        if (!alias) {
            return catalogError(context, arguments.subcommand, alias.error());
        }
        if (!*alias) {
            return failure(context, arguments.subcommand, ExitCode::ProblemsFound,
                           fmt::format("the catalog has no identity or alias {}", id->toString()));
        }
        asAlias = **alias;
        identity = store.identity(asAlias->game);
        if (!identity) {
            return catalogError(context, arguments.subcommand, identity.error());
        }
        if (!*identity) {
            return failure(context, arguments.subcommand, ExitCode::ProblemsFound,
                           fmt::format("{} is an alias of {}, which the catalog does not hold", id->toString(),
                                       asAlias->game.toString()));
        }
    }
    const catalog::StoredIdentity& shown = **identity;
    const auto aliases = store.aliasesOf(shown.id);
    const auto history = store.history(shown.id);
    const auto reviews = store.reviews(shown.library, false);
    if (!aliases || !history || !reviews) {
        return catalogError(context, arguments.subcommand,
                            !aliases ? aliases.error() : !history ? history.error() : reviews.error());
    }
    std::vector<catalog::StoredReview> own;
    std::copy_if(reviews->begin(), reviews->end(), std::back_inserter(own),
                 [&](const catalog::StoredReview& review) { return review.review.game == shown.id; });
    IdentitiesByPath byPath(store, shown.library);

    if (arguments.json) {
        Json aliasList = Json::array();
        for (const catalog::StoredAlias& alias : *aliases) {
            aliasList.push_back(aliasJson(alias));
        }
        Json events = Json::array();
        for (const catalog::IdentityEvent& event : *history) {
            events.push_back(eventJson(event));
        }
        Json reviewList = Json::array();
        for (const catalog::StoredReview& review : own) {
            auto json = reviewJson(review, byPath);
            if (!json) {
                return catalogError(context, arguments.subcommand, json.error());
            }
            reviewList.push_back(std::move(*json));
        }
        return deliver(context, arguments,
                       jsonText(Json{{"asked", id->toString()},
                                     {"alias", asAlias ? aliasJson(*asAlias) : Json(nullptr)},
                                     {"identity", identityJson(shown)},
                                     {"aliases", std::move(aliasList)},
                                     {"history", std::move(events)},
                                     {"reviews", std::move(reviewList)}}),
                       ExitCode::Ok);
    }
    std::string text;
    const auto line = [&](std::string_view label, std::string_view value) {
        text += fmt::format("{:<14}{}\n", fmt::format("{}:", label), value);
    };
    if (asAlias) {
        text += fmt::format("{} is an alias of {} (since {}, {}): it was {}\n\n", asAlias->alias.toString(),
                            asAlias->game.toString(), asAlias->at, catalog::toString(asAlias->reason),
                            asAlias->key.relativePath());
    }
    line("Identity", fmt::format("{}: {}, library {}", shown.id.toString(), shown.system.str(), shown.library.str()));
    line("State", stateOf(shown));
    line("Last path", shown.key.relativePath());
    line("Lookup key", fmt::format("{} (key rule {}, casing table {})", shown.key.key(), shown.keyVersion.rule,
                                   shown.keyVersion.casingTable));
    if (shown.file) {
        line("File", fmt::format("{}{} bytes, written {}{}", shown.file->directory ? "folder, " : "",
                                 shown.file->size, fileTimeText(shown.file->lastWriteTime),
                                 shown.file->hidden ? ", hidden" : ""));
    } else {
        line("File", "never seen by an import");
    }
    if (shown.quickHash) {
        line("Quick hash", fmt::format("{} ({}; of {} bytes written {})", catalog::toHex(shown.quickHash->digest),
                                       hashStateOf(shown), shown.quickHash->size,
                                       fileTimeText(shown.quickHash->lastWriteTime)));
    } else {
        line("Quick hash", hashStateOf(shown) == "folder" ? "none (a folder)" : "none yet ('identities hash')");
    }
    if (aliases->empty()) {
        line("Aliases", "none");
    }
    for (const catalog::StoredAlias& alias : *aliases) {
        line("Alias", fmt::format("{}, which was {} ({}, {})", alias.alias.toString(), alias.key.relativePath(),
                                  catalog::toString(alias.reason), alias.at));
    }
    text += fmt::format("History:      {}\n", history->empty() ? "nothing yet" : "");
    for (const catalog::IdentityEvent& event : *history) {
        text += fmt::format("  {}\n", eventText(event));
    }
    for (const catalog::StoredReview& review : own) {
        auto reviewed = reviewText(review, byPath);
        if (!reviewed) {
            return catalogError(context, arguments.subcommand, reviewed.error());
        }
        text += *reviewed;
    }
    return deliver(context, arguments, text, ExitCode::Ok);
}

// --- reviews ---------------------------------------------------------------------------------------------------------

ExitCode listReviews(CommandContext& context, Arguments& arguments) {
    std::optional<catalog::SystemName> system;
    if (const std::optional<std::string> systemText = arguments.args.value("system")) {
        auto parsed = catalog::SystemName::parse(*systemText);
        if (!parsed) {
            return failure(context, arguments.subcommand, ExitCode::UsageError,
                           fmt::format("--system: {}", parsed.error().toString()));
        }
        system = std::move(*parsed);
    }
    const bool all = arguments.args.has("all");
    auto catalog = openCatalog(context.paths, false);
    if (!catalog) {
        return catalogError(context, arguments.subcommand, catalog.error());
    }
    ExitCode code = ExitCode::Ok;
    const auto library = recordedLibrary(context, arguments.subcommand, *catalog->store, code);
    if (!library) {
        return code;
    }
    auto reviews = catalog->store->reviews(*library, !all);
    if (!reviews) {
        return catalogError(context, arguments.subcommand, reviews.error());
    }
    if (system) {
        std::erase_if(*reviews, [&](const catalog::StoredReview& review) { return review.review.system != *system; });
    }
    const std::size_t open = static_cast<std::size_t>(std::count_if(
        reviews->begin(), reviews->end(), [](const catalog::StoredReview& review) { return review.open(); }));
    IdentitiesByPath byPath(*catalog->store, *library);
    if (arguments.json) {
        Json list = Json::array();
        for (const catalog::StoredReview& review : *reviews) {
            auto json = reviewJson(review, byPath);
            if (!json) {
                return catalogError(context, arguments.subcommand, json.error());
            }
            list.push_back(std::move(*json));
        }
        return deliver(context, arguments,
                       jsonText(Json{{"library", library->str()},
                                     {"system", system ? Json(system->str()) : Json(nullptr)},
                                     {"all", all},
                                     {"open", open},
                                     {"reviews", std::move(list)}}),
                       ExitCode::Ok);
    }
    std::string text = fmt::format("Review rows of library {}{}: {} open{}\n", library->str(),
                                   system ? " (" + system->str() + ")" : std::string(), open,
                                   all ? fmt::format(", {} in all", reviews->size()) : std::string());
    for (const catalog::StoredReview& review : *reviews) {
        auto reviewed = reviewText(review, byPath);
        if (!reviewed) {
            return catalogError(context, arguments.subcommand, reviewed.error());
        }
        text += "\n" + *reviewed;
    }
    if (open > 0) {
        text += "\nResolve one with 'retroverse-cli identities resolve <review> --with <gameId>' (the candidate's "
                "GameId: it becomes the vanished identity's alias) or '--keep-separate'.\n";
    }
    return deliver(context, arguments, text, ExitCode::Ok);
}

// --- resolve ---------------------------------------------------------------------------------------------------------

ExitCode resolveReviewRow(CommandContext& context, Arguments& arguments) {
    const std::optional<std::string> reviewText = arguments.args.positional("review");
    if (!reviewText) {
        return failure(context, arguments.subcommand, ExitCode::UsageError, "a review row's id is required");
    }
    std::int64_t review = 0;
    const auto [end, parsed] = std::from_chars(reviewText->data(), reviewText->data() + reviewText->size(), review);
    if (parsed != std::errc() || end != reviewText->data() + reviewText->size() || review <= 0) {
        return failure(context, arguments.subcommand, ExitCode::UsageError,
                       fmt::format("'{}' is no review row id (a positive number)", *reviewText));
    }
    const std::optional<std::string> with = arguments.args.value("with");
    const bool separate = arguments.args.has("keep-separate");
    if (with.has_value() == separate) {
        return failure(context, arguments.subcommand, ExitCode::UsageError,
                       "give either --with <gameId> or --keep-separate");
    }
    catalog::ReviewDecision decision;
    decision.review = review;
    if (with) {
        const auto id = catalog::GameId::parse(*with);
        if (!id) {
            return failure(context, arguments.subcommand, ExitCode::UsageError,
                           fmt::format("--with: {}", id.error().toString()));
        }
        decision.pairWith = *id;
    }
    auto catalog = openCatalog(context.paths, true);
    if (!catalog) {
        return catalogError(context, arguments.subcommand, catalog.error());
    }
    ExitCode code = ExitCode::Ok;
    const auto library = recordedLibrary(context, arguments.subcommand, *catalog->store, code);
    if (!library) {
        return code;
    }
    const auto result = catalog::resolveReview(*catalog->store, *library, decision);
    if (!result) {
        const Error& error = result.error();
        if (error.code == ErrorCode::Conflict && error.message.find("import lock") != std::string::npos) {
            return failure(context, arguments.subcommand, ExitCode::RuntimeFailure,
                           fmt::format("another import runs (in this or another process): {}", error.toString()));
        }
        if (error.code == ErrorCode::NotFound || error.code == ErrorCode::Conflict) {
            return failure(context, arguments.subcommand, ExitCode::ProblemsFound, error.toString());
        }
        if (error.code == ErrorCode::InvalidArgument) {
            return failure(context, arguments.subcommand, ExitCode::UsageError, error.toString());
        }
        return catalogError(context, arguments.subcommand, error);
    }
    const catalog::StoredReview& resolved = result->review;
    if (arguments.json) {
        IdentitiesByPath byPath(*catalog->store, *library);
        auto json = reviewJson(resolved, byPath);
        if (!json) {
            return catalogError(context, arguments.subcommand, json.error());
        }
        Json moved = nullptr;
        if (result->moved) {
            moved = Json{{"id", result->moved->id.toString()},
                         {"system", result->moved->system.str()},
                         {"from", result->moved->from.relativePath()},
                         {"to", result->moved->to.relativePath()},
                         {"reason", catalog::toString(result->moved->reason)},
                         {"alias", result->moved->alias ? Json(result->moved->alias->toString()) : Json(nullptr)}};
        }
        return deliver(context, arguments,
                       jsonText(Json{{"review", std::move(*json)},
                                     {"moved", std::move(moved)},
                                     {"leftAtOldPath", result->leftAtOldPath ? Json(result->leftAtOldPath->toString())
                                                                             : Json(nullptr)}}),
                       ExitCode::Ok);
    }
    std::string text;
    if (result->moved) {
        const catalog::MovedGame& move = *result->moved;
        text += fmt::format("Review {} resolved: {} paired with the file of {}, which is its alias now: {} {} -> {}\n",
                            review, move.id.toString(), move.alias ? move.alias->toString() : std::string("?"),
                            move.system.str(), move.from.relativePath(), move.to.relativePath());
        if (result->leftAtOldPath) {
            text += fmt::format("The gamelist entry left at {} has a new identity: {}\n", move.from.relativePath(),
                                result->leftAtOldPath->toString());
        }
    } else {
        text += fmt::format("Review {} resolved: {} kept separate from its candidates.\n", review,
                            resolved.review.game.toString());
    }
    return deliver(context, arguments, text, ExitCode::Ok);
}

// --- hash ------------------------------------------------------------------------------------------------------------

ExitCode hashFiles(CommandContext& context, Arguments& arguments) {
    catalog::QuickHashPassOptions options;
    if (const std::optional<std::string> systemText = arguments.args.value("system")) {
        auto system = catalog::SystemName::parse(*systemText);
        if (!system) {
            return failure(context, arguments.subcommand, ExitCode::UsageError,
                           fmt::format("--system: {}", system.error().toString()));
        }
        options.system = std::move(*system);
    }
    if (const std::optional<std::string> limitText = arguments.args.value("limit")) {
        std::size_t limit = 0;
        const auto [end, parsed] = std::from_chars(limitText->data(), limitText->data() + limitText->size(), limit);
        if (parsed != std::errc() || end != limitText->data() + limitText->size() || limit == 0) {
            return failure(context, arguments.subcommand, ExitCode::UsageError,
                           fmt::format("--limit: '{}' is no positive number of files", *limitText));
        }
        options.limit = limit;
    }
    auto catalog = openCatalog(context.paths, true);
    if (!catalog) {
        return catalogError(context, arguments.subcommand, catalog.error());
    }
    ExitCode code = ExitCode::Ok;
    const auto library = recordedLibrary(context, arguments.subcommand, *catalog->store, code);
    if (!library) {
        return code;
    }
    // Progress in the log, every two seconds.
    auto lastReport = std::chrono::steady_clock::now();
    options.progress = [&](const catalog::QuickHashProgress& progress) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport < std::chrono::seconds(2)) {
            return;
        }
        lastReport = now;
        spdlog::info("Quick hashes: {} of {} file(s) looked at, {} hashed, {:.1f} MiB read", progress.done,
                     progress.total, progress.hashed, static_cast<double>(progress.bytesRead) / (1024.0 * 1024.0));
    };
    const auto result = catalog::runQuickHashPass(*catalog->store, *library, options);
    if (!result) {
        const Error& error = result.error();
        if (error.code == ErrorCode::Conflict) {
            return failure(context, arguments.subcommand, ExitCode::RuntimeFailure,
                           fmt::format("another import runs (in this or another process): {}", error.toString()));
        }
        if (error.code == ErrorCode::InvalidArgument) {
            return failure(context, arguments.subcommand, ExitCode::UsageError, error.toString());
        }
        return catalogError(context, arguments.subcommand, error);
    }
    const catalog::QuickHashPassResult& pass = *result;
    const double seconds = std::chrono::duration<double>(pass.time).count();
    const double mebibytes = static_cast<double>(pass.bytesRead) / (1024.0 * 1024.0);
    const ExitCode exit = pass.unreadable > 0 || pass.unavailable > 0 ? ExitCode::ProblemsFound : ExitCode::Ok;
    if (arguments.json) {
        Json issues = Json::array();
        for (const catalog::Issue& issue : pass.issues) {
            issues.push_back(issueJson(issue));
        }
        return deliver(context, arguments,
                       jsonText(Json{{"library", library->str()},
                                     {"system", options.system ? Json(options.system->str()) : Json(nullptr)},
                                     {"limit", options.limit == 0 ? Json(nullptr) : Json(options.limit)},
                                     {"identities", pass.identities},
                                     {"notPresent", pass.notPresent},
                                     {"folders", pass.folders},
                                     {"current", pass.current},
                                     {"candidates", pass.candidates},
                                     {"hashed", pass.hashed},
                                     {"changed", pass.changed},
                                     {"gone", pass.gone},
                                     {"unreadable", pass.unreadable},
                                     {"unavailable", pass.unavailable},
                                     {"remaining", pass.remaining},
                                     {"batches", pass.batches},
                                     {"bytesRead", pass.bytesRead},
                                     {"seconds", seconds},
                                     {"cancelled", pass.cancelled},
                                     {"issues", std::move(issues)}}),
                       exit);
    }
    std::string text =
        fmt::format("Quick hashes of library {}{}: {} identities: {} not present, {} folder(s), {} with "
                    "a current quick hash, {} to hash{}\n",
                    library->str(), options.system ? " (" + options.system->str() + ")" : std::string(),
                    pass.identities, pass.notPresent, pass.folders, pass.current, pass.candidates,
                    options.limit == 0 ? std::string() : fmt::format(" (at most {} now)", options.limit));
    text += fmt::format("Hashed {} file(s) in {} batch(es): {} changed since the last import, {} gone, {} unreadable, "
                        "{} out of reach, {} left; {:.1f} MiB read in {:.2f} s\n",
                        pass.hashed, pass.batches, pass.changed, pass.gone, pass.unreadable, pass.unavailable,
                        pass.remaining, mebibytes, seconds);
    if (pass.changed > 0 || pass.gone > 0) {
        text +=
            "Files changed or gone since the last import are hashed after the next one ('retroverse-cli import').\n";
    }
    if (pass.remaining > 0) {
        text += "Run 'retroverse-cli identities hash' again to go on with what is left.\n";
    }
    text += fmt::format("Issues: {}\n", pass.issues.size());
    for (const catalog::Issue& issue : pass.issues) {
        text += fmt::format("  [{}] {} {}: {}{}\n", catalog::toString(issue.severity), issue.code, issue.system,
                            issue.reason, issue.file.empty() ? std::string() : " (" + issue.file + ")");
    }
    return deliver(context, arguments, text, exit);
}

} // namespace

ExitCode runIdentities(CommandContext& context) {
    if (context.arguments.empty()) {
        context.err.write(kUsage);
        return ExitCode::UsageError;
    }
    const std::string& subcommand = context.arguments.front();
    if (subcommand == "--help" || subcommand == "-h") {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    using Runner = ExitCode (*)(CommandContext&, Arguments&);
    Runner runner = nullptr;
    core::ArgParser parser{subcommandName(subcommand)};
    if (subcommand == "list") {
        runner = listIdentities;
        parser.option("system", "name", "The ES-DE system whose identities to list.");
    } else if (subcommand == "show") {
        runner = showIdentity;
        parser.positional("gameId", "The GameId: 'g' and 16 hexadecimal digits.");
    } else if (subcommand == "reviews") {
        runner = listReviews;
        parser.option("system", "name", "Only this system's review rows.").flag("all", "The resolved rows too.");
    } else if (subcommand == "resolve") {
        runner = resolveReviewRow;
        parser.positional("review", "The review row's id.")
            .option("with", "gameId", "Pair the vanished identity with this identity, which becomes its alias.")
            .flag("keep-separate", "Keep them apart.");
    } else if (subcommand == "hash") {
        runner = hashFiles;
        parser.option("system", "name", "Only this system's files.")
            .option("limit", "files", "Read at most this many files now.");
    } else {
        context.err.writeLine(fmt::format("{}: unknown {} '{}'", kName,
                                          subcommand.starts_with('-') ? "option" : "subcommand", subcommand));
        context.err.writeLine("Run 'retroverse-cli identities --help' for the subcommands and their options.");
        return ExitCode::UsageError;
    }
    parser.flag("json", "Write the results as JSON.");
    addOutputOption(parser);
    auto args = parser.parse(context.arguments.subspan(1));
    if (!args) {
        context.err.writeLine(fmt::format("{}: {}", subcommandName(subcommand), args.error().toString()));
        context.err.writeLine("Run 'retroverse-cli identities --help' for the subcommands and their options.");
        return ExitCode::UsageError;
    }
    if (args->has("help")) {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    auto file = outputFileOption(*args);
    if (!file) {
        context.err.writeLine(fmt::format("{}: {}", subcommandName(subcommand), file.error().toString()));
        return ExitCode::UsageError;
    }
    const bool json = args->has("json");
    Arguments arguments{subcommand, std::move(*args), json, std::move(*file)};
    return runner(context, arguments);
}

} // namespace rv::cli
