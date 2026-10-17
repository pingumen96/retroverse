// retroverse-cli import (ADR-0008 §3, §4): imports the ES-DE collection into the catalog. I2 step 7 implements the dry
// run, the import itself is step 11 (without --dry-run the command answers "not implemented yet (I2 step 11)", exit 2).
//  - import --dry-run [--system <name>] [--json] [--output <file>]: reads the library <user data>/config/library.json
//    records (rv::esde::EsdeLibrarySource: ES-DE's configuration, every system's ROM folder and gamelist, the disc
//    groups) and prints, per system, its folder and gamelist and the counts of its entries by state (games, missing
//    files, not games with their reasons, disc members, folders, entries outside the system folder, entries whose
//    folder cannot be listed), its disc groups and issues; then the totals, the folders that could not be listed, the
//    issues and the time the read took. The text lists the systems that have entries or whose folder cannot be read;
//    --json lists every system of the configuration. Every cue sheet and playlist that is a game is read (there is no
//    database to take the groups read before from). It writes nothing but its log file.
// Exit codes: 0; 1 when an error issue was met (a folder that cannot be listed, a gamelist that cannot be read); 2 for
// invalid options, without --dry-run, without a library.json, and for a system the configuration does not define; 3
// when library.json cannot be read, the library cannot be read at all, or --output cannot be written.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/esde/library_source.hpp>

#include "commands.hpp"
#include "esde_text.hpp"
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

} // namespace

ExitCode runImport(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Imports the ES-DE collection into the catalog. --dry-run reads it and prints what the "
                           "import would take, writing nothing.");
    parser.flag("dry-run", "Read the library and print its entries by system and state; write nothing.")
        .option("system", "name", "Only this system (an ES-DE system name).")
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
    if (!args->has("dry-run")) {
        context.err.writeLine(fmt::format("{}: not implemented yet (I2 step 11)", kName));
        return ExitCode::UsageError;
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
    spdlog::info("import --dry-run: {} system(s), {} entr(ies), {} issue(s), read in {:.2f} s",
                 snapshot.systems.size(), snapshot.entries.size(), snapshot.issues.size(),
                 secondsOf(statistics.totalTime));

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
