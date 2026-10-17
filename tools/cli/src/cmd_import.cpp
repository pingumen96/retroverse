// retroverse-cli import (ADR-0008 §3, §4, §5, §7): imports the ES-DE collection into the catalog. I2 step 7 implements
// the dry run, step 11a the import itself.
//  - import [--system <name>] [--accept-removals] [--json] [--output <file>]: imports the library library.json records
//    into <user data>/database/retroverse.sqlite (made and migrated when needed) with catalog::ImportService
//    (runCatalogImport(), import_text.hpp, shared with `db rebuild-index`): identities given out and matched, the
//    index rows written where they changed, the disc groups with their references, the media presence, the
//    availability of every folder, the mass-disappearance guard, the gamelists that failed kept, files moved or renamed
//    paired with their identities (catalog::IdentityPairing, I2 step 11b: by quick hash, or a unique size, time and
//    name; the rest left as review rows for `retroverse-cli identities`); then prints the ChangeSet per system (added,
//    changed, removed, unchanged, unavailable, kept, issues, the outcome), the identities' changes with each move and
//    review row, the issues and the times. --system imports that system only (the others stay as they are);
//    --accept-removals applies the removals the guard holds back. The library must be the catalog's: a library.json
//    naming another library is refused until it names the catalog's again (re-association: `retroverse-cli library
//    create --id <the catalog's library> --force`).
//  - import --dry-run [--system <name>] [--json] [--output <file>]: reads the library <user data>/config/library.json
//    records (rv::esde::EsdeLibrarySource: ES-DE's configuration, every system's ROM folder and gamelist, the disc
//    groups) and prints, per system, its folder and gamelist and the counts of its entries by state (games, missing
//    files, not games with their reasons, disc members, folders, entries outside the system folder, entries whose
//    folder cannot be listed), its disc groups and issues; then the totals, the folders that could not be listed, the
//    issues and the time the read took. The text lists the systems that have entries or whose folder cannot be read;
//    --json lists every system of the configuration. Every cue sheet and playlist that is a game is read (there is no
//    database to take the groups read before from). It writes nothing but its log file.
// Exit codes: 0; 1 when an error issue was met (a folder that cannot be listed, a gamelist that cannot be read) or the
// guard kept a system as it was; 2 for invalid options, without a library.json, for a system the configuration does
// not define, and for a library.json naming another library than the catalog's; 3 when library.json cannot be read,
// the library or the catalog database cannot be read or written, another import runs, or --output cannot be written.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/change_set.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

#include "commands.hpp"
#include "esde_text.hpp"
#include "import_text.hpp"
#include "library_snapshot.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli import";

/// One system's entries counted, or all of them.
struct Counts {
    std::size_t entries = 0;
    std::size_t games = 0;
    std::size_t missing = 0;
    std::size_t notAGame = 0;
    std::size_t members = 0;
    std::size_t folders = 0;
    std::size_t outside = 0;
    std::size_t unavailable = 0; ///< entries whose folder could not be listed (any state)
    std::size_t esde = 0;        ///< games and members on disk: what ES-DE lists
    std::size_t groups = 0;
    std::size_t unavailableFolders = 0;
    std::size_t issues = 0;
    /// "extension-not-configured", "noload-txt", "bios-or-device-set", "excluded-by-setting:<setting>".
    std::map<std::string, std::size_t> reasons;

    void add(const Counts& other) {
        entries += other.entries;
        games += other.games;
        missing += other.missing;
        notAGame += other.notAGame;
        members += other.members;
        folders += other.folders;
        outside += other.outside;
        unavailable += other.unavailable;
        esde += other.esde;
        groups += other.groups;
        unavailableFolders += other.unavailableFolders;
        issues += other.issues;
        for (const auto& [reason, count] : other.reasons) {
            reasons[reason] += count;
        }
    }
};

std::string reasonOf(const catalog::CatalogEntry& entry) {
    std::string reason(catalog::toString(entry.notAGameReason));
    if (!entry.excludingSetting.empty()) {
        reason += ":" + entry.excludingSetting;
    }
    return reason;
}

/// The counts of every system of the snapshot, by name.
std::map<std::string, Counts> countsOf(const catalog::LibrarySnapshot& snapshot) {
    std::map<std::string, Counts> counts;
    for (const catalog::SystemInfo& system : snapshot.systems) {
        counts[system.name.str()];
    }
    for (const catalog::CatalogEntry& entry : snapshot.entries) {
        Counts& count = counts[entry.system.str()];
        ++count.entries;
        count.unavailable += entry.availability == catalog::Availability::SourceUnavailable ? 1 : 0;
        const bool game = entry.state == catalog::EntryState::Game || entry.state == catalog::EntryState::DiscMember;
        count.esde += game && entry.availability == catalog::Availability::Available ? 1 : 0;
        switch (entry.state) {
        case catalog::EntryState::Game:
            ++count.games;
            break;
        case catalog::EntryState::MissingFile:
            ++count.missing;
            break;
        case catalog::EntryState::NotAGame:
            ++count.notAGame;
            ++count.reasons[reasonOf(entry)];
            break;
        case catalog::EntryState::DiscMember:
            ++count.members;
            break;
        case catalog::EntryState::Folder:
            ++count.folders;
            break;
        case catalog::EntryState::OutsideSystemFolder:
            ++count.outside;
            break;
        }
    }
    for (const catalog::DiscGroup& group : snapshot.discGroups) {
        ++counts[group.system.str()].groups;
    }
    for (const catalog::UnavailableFolder& folder : snapshot.unavailableFolders) {
        ++counts[folder.system.str()].unavailableFolders;
    }
    for (const catalog::Issue& issue : snapshot.issues) {
        if (const auto found = counts.find(issue.system); found != counts.end()) {
            ++found->second.issues;
        }
    }
    return counts;
}

double secondsOf(std::chrono::nanoseconds time) {
    return std::chrono::duration<double>(time).count();
}

Json countsJson(const Counts& counts) {
    Json reasons = Json::object();
    for (const auto& [reason, count] : counts.reasons) {
        reasons[reason] = count;
    }
    return Json{{"entries", counts.entries},
                {"games", counts.games},
                {"missingFiles", counts.missing},
                {"notAGame", counts.notAGame},
                {"notAGameReasons", std::move(reasons)},
                {"discMembers", counts.members},
                {"folders", counts.folders},
                {"outsideSystemFolder", counts.outside},
                {"sourceUnavailable", counts.unavailable},
                {"esdeGames", counts.esde},
                {"discGroups", counts.groups},
                {"unavailableFolders", counts.unavailableFolders},
                {"issues", counts.issues}};
}

Json reportJson(const catalog::LibrarySnapshot& snapshot, const std::map<std::string, Counts>& counts,
                const esde::EsdeLoadStatistics& statistics) {
    Json systems = Json::array();
    Counts total;
    for (const catalog::SystemInfo& system : snapshot.systems) {
        const Counts& count = counts.at(system.name.str());
        total.add(count);
        Json row{{"name", system.name.str()},
                 {"fullName", system.fullName},
                 {"romFolder", system.romFolder.empty() ? Json(nullptr) : Json(pathText(system.romFolder))},
                 {"availability", std::string(catalog::toString(system.availability))},
                 {"unavailableReason", system.availability == catalog::Availability::Available
                                           ? Json(nullptr)
                                           : Json(std::string(catalog::toString(system.unavailableReason)))},
                 {"gamelist", std::string(catalog::toString(system.gamelist))}};
        row.update(countsJson(count));
        systems.push_back(std::move(row));
    }
    Json folders = Json::array();
    for (const catalog::UnavailableFolder& folder : snapshot.unavailableFolders) {
        folders.push_back(Json{{"system", folder.system.str()},
                               {"path", folder.relativePath},
                               {"reason", std::string(catalog::toString(folder.reason))},
                               {"detail", folder.detail}});
    }
    Json issues = Json::array();
    for (const catalog::Issue& issue : snapshot.issues) {
        issues.push_back(issueJson(issue));
    }
    return Json{{"library", snapshot.profile.id.str()},
                {"esdeHome", pathText(snapshot.profile.esdeHome)},
                {"dryRun", true},
                {"systems", std::move(systems)},
                {"totals", countsJson(total)},
                {"unavailableFolders", std::move(folders)},
                {"issues", std::move(issues)},
                {"statistics", Json{{"seconds", secondsOf(statistics.totalTime)},
                                    {"configurationSeconds", secondsOf(statistics.configurationTime)},
                                    {"scanSeconds", secondsOf(statistics.scanTime)},
                                    {"gamelistSeconds", secondsOf(statistics.gamelistTime)},
                                    {"sheetSeconds", secondsOf(statistics.sheetTime)},
                                    {"systemsScanned", statistics.systemsScanned},
                                    {"filesListed", statistics.filesListed},
                                    {"foldersListed", statistics.foldersListed},
                                    {"gamelistsRead", statistics.gamelistsRead},
                                    {"gamelistRetries", statistics.gamelistRetries},
                                    {"sheetsRead", statistics.sheetsRead},
                                    {"sheetsFromCache", statistics.sheetsFromCache},
                                    {"sheetsUnreadable", statistics.sheetsUnreadable}}}};
}

void writeText(const catalog::LibrarySnapshot& snapshot, const std::map<std::string, Counts>& counts,
               const esde::EsdeLoadStatistics& statistics, Output& out) {
    out.writeLine(fmt::format("Library {}: ES-DE home {} (dry run: nothing is written)", snapshot.profile.id.str(),
                              pathText(snapshot.profile.esdeHome)));
    out.writeLine();
    const std::array<std::string_view, 11> header{"Entries", "Games",   "Missing", "Not game", "Members", "Folders",
                                                  "Outside", "Unavail", "Groups",  "Issues",   ""};
    std::string line = fmt::format("{:<16}", "System");
    for (const std::string_view column : header) {
        line += column.empty() ? std::string() : fmt::format("{:>9}", column);
    }
    out.writeLine(line + "  Folder, gamelist");
    const auto row = [](const Counts& count) {
        return fmt::format("{:>9}{:>9}{:>9}{:>9}{:>9}{:>9}{:>9}{:>9}{:>9}{:>9}", count.entries, count.games,
                           count.missing, count.notAGame, count.members, count.folders, count.outside,
                           count.unavailable, count.groups, count.issues);
    };
    Counts total;
    std::size_t withFolder = 0;
    std::size_t listed = 0;
    for (const catalog::SystemInfo& system : snapshot.systems) {
        const Counts& count = counts.at(system.name.str());
        total.add(count);
        const bool available = system.availability == catalog::Availability::Available;
        withFolder += available ? 1 : 0;
        const bool unreadable =
            !available && system.unavailableReason != catalog::SourceUnavailableReason::FolderMissing;
        if (count.entries == 0 && count.issues == 0 && !unreadable) {
            continue;
        }
        ++listed;
        const std::string folder =
            available ? std::string("available") : std::string(catalog::toString(system.unavailableReason));
        out.writeLine(
            fmt::format("{:<16}{}  {}, {}", system.name.str(), row(count), folder, catalog::toString(system.gamelist)));
    }
    out.writeLine(fmt::format("{:<16}{}", "Total", row(total)));
    out.writeLine();
    out.writeLine(fmt::format("{} systems configured, {} with a ROM folder, {} listed above; games as ES-DE lists them "
                              "(the games and disc members on disk): {}",
                              snapshot.systems.size(), withFolder, listed, total.esde));
    if (!total.reasons.empty()) {
        out.writeLine("Not a game, by reason:");
        for (const auto& [name, count] : counts) {
            for (const auto& [reason, number] : count.reasons) {
                out.writeLine(fmt::format("  {}: {} {}", name, reason, number));
            }
        }
    }
    if (!snapshot.unavailableFolders.empty()) {
        out.writeLine("Folders that cannot be listed (their entries are kept as source unavailable):");
        for (const catalog::UnavailableFolder& folder : snapshot.unavailableFolders) {
            out.writeLine(fmt::format("  {}: {} ({}: {})", folder.system.str(), folder.relativePath,
                                      catalog::toString(folder.reason), folder.detail));
        }
    }
    out.writeLine(fmt::format("Issues: {}", snapshot.issues.size()));
    for (const catalog::Issue& issue : snapshot.issues) {
        out.writeLine(fmt::format("  [{}] {}{}: {}{}", catalog::toString(issue.severity), issue.code,
                                  issue.system.empty() ? std::string() : " " + issue.system, issue.reason,
                                  issue.file.empty() ? std::string() : " (" + issue.file + ")"));
    }
    out.writeLine();
    out.writeLine(fmt::format("Read in {:.2f} s (configuration {:.2f} s, ROM folders {:.2f} s, gamelists {:.2f} s, "
                              "cue sheets and playlists {:.2f} s): {} files and {} folders listed in {} system "
                              "folders, {} gamelists read, {} sheets read, {} unreadable",
                              secondsOf(statistics.totalTime), secondsOf(statistics.configurationTime),
                              secondsOf(statistics.scanTime), secondsOf(statistics.gamelistTime),
                              secondsOf(statistics.sheetTime), statistics.filesListed, statistics.foldersListed,
                              statistics.systemsScanned, statistics.gamelistsRead, statistics.sheetsRead,
                              statistics.sheetsUnreadable));
}

// --- The import (runCatalogImport) -----------------------------------------------------------------------------------

Json changesJson(const catalog::SystemChanges& changes) {
    Json json{{"added", changes.added},
              {"changed", changes.changed},
              {"removed", changes.removed},
              {"unchanged", changes.unchanged},
              {"unavailable", changes.unavailable},
              {"kept", changes.kept},
              {"rows", changes.rows()},
              {"newIdentities", changes.newIdentities},
              {"vanished", changes.vanished},
              {"returned", changes.returned},
              {"rekeyed", changes.rekeyed},
              {"moved", changes.moved},
              {"presentBefore", changes.presentBefore},
              {"gone", changes.gone},
              {"issues", changes.issues}};
    return json;
}

/// A system worth a line of the text: rows, changes, issues, or an outcome other than applied or source-unavailable (a
/// system whose folder is missing and that holds no rows is left to the JSON).
bool worthListing(const catalog::SystemChanges& changes) {
    const bool usual = changes.outcome == catalog::SystemOutcome::Applied ||
                       changes.outcome == catalog::SystemOutcome::SourceUnavailable;
    return !usual || changes.rows() > 0 || changes.removed > 0 || changes.issues > 0 || changes.vanished > 0;
}

/// Whether the import found what the exit code reports as problems: an error issue, a system kept by the guard.
bool foundProblems(const catalog::ChangeSet& changes) {
    return std::any_of(changes.issues.begin(), changes.issues.end(),
                       [](const catalog::Issue& issue) { return issue.severity == catalog::IssueSeverity::Error; }) ||
           std::any_of(changes.systems.begin(), changes.systems.end(), [](const catalog::SystemChanges& system) {
               return system.outcome == catalog::SystemOutcome::KeptByGuard;
           });
}

std::string importJson(const CatalogImportRequest& request, const catalog::LibraryProfile& profile,
                       const std::filesystem::path& database, const catalog::ImportResult& result,
                       const esde::EsdeLoadStatistics& load, const catalog::IdentityPairing::Statistics& pairing) {
    const catalog::ChangeSet& changes = result.changes;
    const catalog::ImportStatistics& statistics = result.statistics;
    Json systems = Json::array();
    for (const catalog::SystemChanges& system : changes.systems) {
        Json row{{"system", system.system.str()}, {"outcome", std::string(catalog::toString(system.outcome))}};
        row.update(changesJson(system));
        systems.push_back(std::move(row));
    }
    Json moved = Json::array();
    for (const catalog::MovedGame& game : changes.moved) {
        moved.push_back(Json{{"id", game.id.toString()},
                             {"system", game.system.str()},
                             {"from", game.from.relativePath()},
                             {"to", game.to.relativePath()},
                             {"reason", std::string(catalog::toString(game.reason))},
                             {"alias", game.alias ? Json(game.alias->toString()) : Json(nullptr)}});
    }
    Json reviews = Json::array();
    for (const catalog::ReviewRow& review : changes.reviews) {
        Json candidates = Json::array();
        for (const catalog::LookupKey& candidate : review.candidates) {
            candidates.push_back(candidate.relativePath());
        }
        reviews.push_back(Json{{"review", review.id},
                               {"game", review.game.toString()},
                               {"system", review.system.str()},
                               {"vanished", review.vanished.relativePath()},
                               {"candidates", std::move(candidates)},
                               {"reason", review.reason}});
    }
    Json issues = Json::array();
    for (const catalog::Issue& issue : changes.issues) {
        issues.push_back(issueJson(issue));
    }
    const Json report{
        {"library", profile.id.str()},
        {"esdeHome", pathText(profile.esdeHome)},
        {"database", pathText(database)},
        {"kind", request.rebuildIndex ? "rebuild-index" : "import"},
        {"firstImport", changes.firstImport},
        {"acceptRemovals", request.acceptRemovals},
        {"system", request.system ? Json(*request.system) : Json(nullptr)},
        {"systems", std::move(systems)},
        {"totals", changesJson(changes.total())},
        {"moved", std::move(moved)},
        {"reviews", std::move(reviews)},
        {"issues", std::move(issues)},
        {"statistics",
         Json{{"seconds", secondsOf(statistics.totalTime)},
              {"loadSeconds", secondsOf(statistics.loadTime)},
              {"mediaSeconds", secondsOf(statistics.mediaTime)},
              {"compareSeconds", secondsOf(statistics.compareTime)},
              {"writeSeconds", secondsOf(statistics.writeTime)},
              {"entries", statistics.entries},
              {"mediaUnknown", statistics.mediaUnknown},
              {"identitiesWritten", statistics.identitiesWritten},
              {"entriesWritten", statistics.entriesWritten},
              {"entriesRemoved", statistics.entriesRemoved},
              {"discGroupsWritten", statistics.discGroupsWritten},
              {"discGroupsRemoved", statistics.discGroupsRemoved},
              {"systemsWritten", statistics.systemsWritten},
              {"historyWritten", statistics.historyWritten},
              {"reviewsWritten", statistics.reviewsWritten},
              {"searchIndexRebuilt", statistics.searchIndexRebuilt},
              {"filesListed", load.filesListed},
              {"sheetsRead", load.sheetsRead},
              {"sheetsFromCache", load.sheetsFromCache},
              {"quickHashesRead", pairing.filesHashed},
              {"quickHashesUnreadable", pairing.filesUnreadable},
              {"quickHashBytesRead", pairing.bytesRead}}}};
    return report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

std::string importText(const CatalogImportRequest& request, const catalog::LibraryProfile& profile,
                       const std::filesystem::path& database, const catalog::ImportResult& result,
                       const esde::EsdeLoadStatistics& load, const catalog::IdentityPairing::Statistics& pairing) {
    const catalog::ChangeSet& changes = result.changes;
    const catalog::ImportStatistics& statistics = result.statistics;
    std::string text;
    const auto line = [&](const std::string& value) {
        text += value;
        text += '\n';
    };
    line(fmt::format("Library {}: ES-DE home {}", profile.id.str(), pathText(profile.esdeHome)));
    line(fmt::format("Catalog: {}{}", pathText(database),
                     request.rebuildIndex       ? " (the index rebuilt: every row written again)"
                     : changes.firstImport      ? " (first import)"
                     : request.system.has_value() ? fmt::format(" (only {})", *request.system)
                                                  : std::string()));
    line("");
    const auto row = [](const catalog::SystemChanges& system) {
        return fmt::format("{:>9}{:>9}{:>9}{:>10}{:>9}{:>9}{:>9}", system.added, system.changed, system.removed,
                           system.unchanged, system.unavailable, system.kept, system.issues);
    };
    line(fmt::format("{:<16}{:>9}{:>9}{:>9}{:>10}{:>9}{:>9}{:>9}  Outcome", "System", "Added", "Changed", "Removed",
                     "Unchanged", "Unavail", "Kept", "Issues"));
    std::size_t listed = 0;
    for (const catalog::SystemChanges& system : changes.systems) {
        if (!worthListing(system)) {
            continue;
        }
        ++listed;
        std::string outcome(catalog::toString(system.outcome));
        if (system.outcome == catalog::SystemOutcome::KeptByGuard) {
            outcome += fmt::format(" ({} of {} present files gone)", system.gone, system.presentBefore);
        }
        line(fmt::format("{:<16}{}  {}", system.system.str(), row(system), outcome));
    }
    const catalog::SystemChanges total = changes.total();
    line(fmt::format("{:<16}{}", "Total", row(total)));
    line("");
    line(fmt::format("{} systems looked at, {} listed above; the catalog holds {} rows of them", changes.systems.size(),
                     listed, total.rows()));
    line(fmt::format("Identities: {} new, {} vanished, {} back, {} re-keyed, {} moved, {} to review",
                     total.newIdentities, total.vanished, total.returned, total.rekeyed, total.moved,
                     changes.reviews.size()));
    for (const catalog::MovedGame& move : changes.moved) {
        line(fmt::format("  moved {} {}: {} -> {} ({}{})", move.id.toString(), move.system.str(),
                         move.from.relativePath(), move.to.relativePath(), catalog::toString(move.reason),
                         move.alias ? fmt::format("; {} is its alias now", move.alias->toString()) : std::string()));
    }
    for (const catalog::ReviewRow& review : changes.reviews) {
        line(fmt::format("  review {}: {} {}: {} vanished, {} candidate(s): {}", review.id, review.game.toString(),
                         review.system.str(), review.vanished.relativePath(), review.candidates.size(),
                         review.reason));
    }
    if (!changes.reviews.empty()) {
        line("  Resolve them with 'retroverse-cli identities resolve <review> (--with <gameId> | --keep-separate)'; "
             "'retroverse-cli identities reviews' lists them with their candidates.");
    }
    for (const catalog::SystemChanges& system : changes.systems) {
        if (system.outcome == catalog::SystemOutcome::KeptByGuard) {
            line(fmt::format("{}: nothing changed, more than half of its present files are gone ({} of {}): run the "
                             "import with --accept-removals to apply the removals",
                             system.system.str(), system.gone, system.presentBefore));
        }
    }
    line(fmt::format("Issues: {}", changes.issues.size()));
    for (const catalog::Issue& issue : changes.issues) {
        line(fmt::format("  [{}] {}{}: {}{}", catalog::toString(issue.severity), issue.code,
                         issue.system.empty() ? std::string() : " " + issue.system, issue.reason,
                         issue.file.empty() ? std::string() : " (" + issue.file + ")"));
    }
    line("");
    line(fmt::format("{} in {:.2f} s (library read {:.2f} s, media {:.2f} s, comparison {:.2f} s, writes {:.2f} s): {} "
                     "identities and {} entries written, {} entries removed, {} disc groups written; {} files listed, "
                     "{} sheets read, {} from the catalog; {} quick hash(es) read for the pairing{}",
                     request.rebuildIndex ? "Rebuilt" : "Imported", secondsOf(statistics.totalTime),
                     secondsOf(statistics.loadTime), secondsOf(statistics.mediaTime),
                     secondsOf(statistics.compareTime), secondsOf(statistics.writeTime), statistics.identitiesWritten,
                     statistics.entriesWritten, statistics.entriesRemoved, statistics.discGroupsWritten,
                     load.filesListed, load.sheetsRead, load.sheetsFromCache, pairing.filesHashed,
                     statistics.searchIndexRebuilt ? "; the search index rebuilt" : ""));
    return text;
}

} // namespace

ExitCode runCatalogImport(CommandContext& context, const CatalogImportRequest& request) {
    const auto failure = [&](ExitCode code, const std::string& message) {
        context.err.writeLine(fmt::format("{}: {}", request.name, message));
        return code;
    };
    std::optional<catalog::SystemName> system;
    if (request.system) {
        auto name = catalog::SystemName::parse(*request.system);
        if (!name) {
            return failure(ExitCode::UsageError, fmt::format("--system: {}", name.error().toString()));
        }
        system = std::move(*name);
    }
    ExitCode readFailure = ExitCode::RuntimeFailure;
    const auto profile = readRecordedLibrary(context, request.name, readFailure);
    if (!profile) {
        return readFailure;
    }
    const auto settings = esde::readSettings(profile->esdeHome);
    if (!settings) {
        return failure(ExitCode::RuntimeFailure,
                       fmt::format("the library cannot be read: {}", settings.error().toString()));
    }
    const esde::ResolvedRoot mediaRoot = esde::resolveMediaRoot(*settings, *profile);

    // The catalog database, made and migrated when needed; its library is the authority on the id.
    auto database = catalog_db::openCatalogDatabase(catalog_db::databaseFile(context.paths));
    if (!database) {
        return failure(ExitCode::RuntimeFailure,
                       fmt::format("cannot open the catalog database: {}", database.error().toString()));
    }
    auto store = catalog_db::SqliteCatalogStore::open(*database);
    if (!store) {
        return failure(ExitCode::RuntimeFailure,
                       fmt::format("cannot open the catalog database: {}", store.error().toString()));
    }
    // The import checks the library under its lock; this look first only words the refusal for the user (with the
    // re-association to run), and words it again when the import refuses after another import made the catalog.
    const auto otherLibrary = [&]() -> std::optional<ExitCode> {
        const auto libraries = (*store)->libraries();
        if (!libraries) {
            return failure(ExitCode::RuntimeFailure,
                           fmt::format("cannot read the catalog database: {}", libraries.error().toString()));
        }
        if (libraries->empty() ||
            std::any_of(libraries->begin(), libraries->end(),
                        [&](const catalog::LibraryProfile& library) { return library.id == profile->id; })) {
            return std::nullopt;
        }
        const catalog::LibraryId& held = libraries->front().id;
        return failure(
            ExitCode::UsageError,
            fmt::format("library.json names the library '{}', but the catalog holds '{}': the catalog is the authority "
                        "on the library's id, from which every GameId derives, and refuses another one. To import "
                        "into '{}' with every GameId kept, name it in library.json again (re-association): "
                        "retroverse-cli library create --id {} --force (with the folder options library.json was "
                        "created with); to start a new catalog instead, delete {}",
                        profile->id.str(), held.str(), held.str(), held.str(), pathText(database->path())));
    };
    if (const auto refused = otherLibrary()) {
        return *refused;
    }

    esde::EsdeLibrarySourceOptions sourceOptions;
    if (system) {
        sourceOptions.systems.push_back(system->str());
    }
    esde::EsdeLibrarySource source(std::move(sourceOptions));
    esde::EsdeMediaResolver resolver(mediaRoot);
    // Reconciliation (I2 step 11b): moved and renamed files keep their identity, by quick hash, or by a unique size,
    // time and name; the rest becomes review rows.
    catalog::IdentityPairing pairing;
    catalog::ImportService service(source, resolver, **store, &pairing);
    catalog::ImportOptions options;
    options.acceptRemovals = request.acceptRemovals;
    options.rebuildIndex = request.rebuildIndex;
    if (system) {
        options.systems.push_back(*system);
    }
    const auto result = service.run(*profile, options);
    if (!result) {
        const Error& error = result.error();
        if (error.code == ErrorCode::InvalidArgument && error.context == request.system.value_or(std::string())) {
            return failure(ExitCode::UsageError, error.toString()); // a system the configuration does not define
        }
        if (error.code == ErrorCode::Conflict && error.context == "library.json") {
            const auto refused = otherLibrary();
            return refused ? *refused : failure(ExitCode::UsageError, error.toString());
        }
        if (error.code == ErrorCode::Conflict) {
            return failure(ExitCode::RuntimeFailure,
                           fmt::format("another import runs (in this or another process), or the database is busy: {}",
                                       error.toString()));
        }
        return failure(ExitCode::RuntimeFailure, error.toString());
    }
    const std::string text =
        request.json
            ? importJson(request, *profile, database->path(), *result, source.lastStatistics(), pairing.statistics())
            : importText(request, *profile, database->path(), *result, source.lastStatistics(), pairing.statistics());
    Output& out = request.file != nullptr && *request.file ? **request.file : context.out;
    out.write(text);
    if (request.file != nullptr && *request.file) {
        if (const Status written = (*request.file)->finish(); !written) {
            return failure(ExitCode::RuntimeFailure, written.error().toString());
        }
    }
    return foundProblems(result->changes) ? ExitCode::ProblemsFound : ExitCode::Ok;
}

ExitCode runImport(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Imports the ES-DE collection into the catalog. --dry-run reads it and prints what the "
                           "import would take, writing nothing.");
    parser.flag("dry-run", "Read the library and print its entries by system and state; write nothing.")
        .option("system", "name", "Only this system (an ES-DE system name).")
        .flag("accept-removals",
              "Apply the removals the mass-disappearance guard holds back (more than half of a system's files gone).")
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
        context.err.writeLine(fmt::format("{}: {}", kName, output.error().toString()));
        return ExitCode::UsageError;
    }
    const std::optional<std::string> system = args->value("system");
    if (system && system->empty()) {
        context.err.writeLine(fmt::format("{}: --system: value must not be empty", kName));
        return ExitCode::UsageError;
    }
    if (!args->has("dry-run")) {
        CatalogImportRequest request;
        request.name = kName;
        request.system = system;
        request.acceptRemovals = args->has("accept-removals");
        request.json = args->has("json");
        request.file = &*output;
        return runCatalogImport(context, request);
    }
    if (args->has("accept-removals")) {
        context.err.writeLine(fmt::format("{}: --accept-removals is for the import, not the dry run", kName));
        return ExitCode::UsageError;
    }

    ExitCode failure = ExitCode::RuntimeFailure;
    const auto profile = readRecordedLibrary(context, kName, failure);
    if (!profile) {
        return failure;
    }
    const auto read = readLibrarySnapshot(*profile, system);
    if (!read) {
        context.err.writeLine(fmt::format("{}: {}", kName, read.error().toString()));
        return read.error().code == ErrorCode::InvalidArgument ? ExitCode::UsageError : ExitCode::RuntimeFailure;
    }
    const catalog::LibrarySnapshot& snapshot = read->snapshot;
    const std::map<std::string, Counts> counts = countsOf(snapshot);
    const esde::EsdeLoadStatistics& statistics = read->statistics;
    spdlog::info("import --dry-run: {} system(s), {} entr(ies), {} issue(s), read in {:.2f} s", snapshot.systems.size(),
                 snapshot.entries.size(), snapshot.issues.size(), secondsOf(statistics.totalTime));

    Output& out = *output ? **output : context.out;
    if (args->has("json")) {
        out.write(
            reportJson(snapshot, counts, statistics).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) +
            "\n");
    } else {
        writeText(snapshot, counts, statistics, out);
    }
    if (*output) {
        if (const Status written = (*output)->finish(); !written) {
            context.err.writeLine(fmt::format("{}: {}", kName, written.error().toString()));
            return ExitCode::RuntimeFailure;
        }
    }
    const bool failed = std::any_of(snapshot.issues.begin(), snapshot.issues.end(), [](const catalog::Issue& issue) {
        return issue.severity == catalog::IssueSeverity::Error;
    });
    return failed ? ExitCode::ProblemsFound : ExitCode::Ok;
}

} // namespace rv::cli
