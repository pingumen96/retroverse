// retroverse-cli media (I2 step 8): the media of an entry and the media coverage of the library, read in place from
// ES-DE's media folders (ADR-0008 §5, ADR-0003) with esde::EsdeMediaResolver, read-only.
//  - media --system <name> --path <relative path> [--json] [--output <file>]: the media of a ROM path of a system (as
//    listed on disk, or as a gamelist writes it: "./Collection A/Pocket Puzzle (World).gb"), kind by kind: the file
//    ES-DE shows, or none. A folder (on disk) keeps its whole name, a file loses its last extension, a path that is not
//    there is named as a file is (a gamelist entry whose file is gone keeps its media).
//  - media --coverage [--system <name>] [--json] [--output <file>]: per system and kind, the share of the entries that
//    have a file of the kind, over the populations of catalog::CoveragePopulation (every gamelist entry, those whose
//    file exists, the scraped games, every game), the library's totals first.
//  - media --game <id>: the media of a game of the catalog, from I2 step 11 on (the import); until then it answers
//    "not implemented yet (I2 step 11)" and exits 2.
// The library is the one <user data>/config/library.json records (library create). Until the ROM scan (I2 step 7) is
// merged, the coverage reads its entries from the gamelists alone (GamelistSource below): the games without a gamelist
// entry are not in it yet. Exit codes: 0; 1 when something could not be read (the media root, a media folder, the
// systems, a gamelist, the ROM folder of the path); 2 for invalid options, an unknown system, a path that leads out
// of its ROM folder, no library.json; 3 when library.json is invalid or the results cannot be written.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/coverage.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

#include "commands.hpp"
#include "esde_text.hpp"

namespace rv::cli {
namespace {

namespace fs = std::filesystem;
using catalog::CoveragePopulation;
using catalog::MediaKind;

constexpr std::string_view kName = "retroverse-cli media";
/// The step that answers --game (the import gives games their ids).
constexpr int kGameStep = 11;

/// The short column titles of the coverage tables, in the order of catalog::kMediaKinds (the legend names ES-DE's
/// folders).
constexpr std::array<std::string_view, catalog::kMediaKindCount> kKindColumns{
    "cover", "back", "3dbox", "phys", "shot", "title", "marq", "mix", "fanart", "manual", "video", "custom"};

/// What each population is, for people.
std::string_view populationText(CoveragePopulation population) {
    switch (population) {
    case CoveragePopulation::GamelistEntries:
        return "every <game> entry of the gamelists, its file there or not (ADR-0003's denominator)";
    case CoveragePopulation::GamelistEntriesWithFile:
        return "the gamelist entries whose file exists";
    case CoveragePopulation::ScrapedGames:
        return "the games with a gamelist entry";
    case CoveragePopulation::Games:
        return "every game ES-DE shows, scraped or not";
    }
    return "";
}

// --- The entries: the gamelists, until the ROM scan of I2 step 7 -----------------------------------------------------

/// A file or folder of a ROM folder that a gamelist entry names, as listed.
struct ListedFile {
    std::string relativePath;
    catalog::FileFacts facts;
};

/// The extension ES-DE compares with a system's list: from the last dot of the name, "." for a name without one
/// (es_systems.xml's "." means "no extension").
std::string_view extensionOf(std::string_view relativePath) {
    const std::size_t slash = relativePath.rfind('/');
    const std::string_view name = relativePath.substr(slash == std::string_view::npos ? 0 : slash + 1);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view(".") : name.substr(dot);
}

/// The folded paths of the folders above `key` ("A/B/C.GB" gives "A" and "A/B").
std::vector<std::string> parentsOf(std::string_view key) {
    std::vector<std::string> parents;
    for (std::size_t slash = key.find('/'); slash != std::string_view::npos; slash = key.find('/', slash + 1)) {
        parents.emplace_back(key.substr(0, slash));
    }
    return parents;
}

/// The library as its gamelists tell it, each <game> entry looked up in its system's ROM folder (listed with
/// core::walkTree, only down the folders the gamelist names): the entries the coverage needs until the ROM scan of I2
/// step 7 (esde::EsdeLibrarySource) is merged, which this source then gives way to. It knows the states the gamelist
/// and the listing tell: Game, NotAGame for an extension the system does not configure (compared with its case, as
/// ES-DE compares it) and for what ShowHiddenFiles and ShowHiddenGames exclude, MissingFile, OutsideSystemFolder, and
/// SourceUnavailable under a folder that cannot be listed. It leaves to the ROM scan the files without a gamelist entry,
/// the disc groups (a track a gamelist names is a game here), noload.txt and the MAME BIOS sets; <folder> entries are
/// left out (a folder is in no coverage population). A gamelist that cannot be read fails its system at once (no
/// retry).
class GamelistSource final : public catalog::ILibrarySource {
public:
    explicit GamelistSource(std::optional<catalog::SystemName> only) : m_only(std::move(only)) {}

    Expected<catalog::LibrarySnapshot> load(const catalog::LibraryProfile& profile,
                                            const catalog::LibraryLoadOptions& options) override {
        (void)options;
        auto settings = esde::readSettings(profile.esdeHome);
        if (!settings) {
            return propagate(settings.error());
        }
        const auto systems = esde::loadSystems(profile);
        if (!systems) {
            return propagate(systems.error());
        }
        catalog::LibrarySnapshot snapshot;
        snapshot.profile = profile;
        const esde::ResolvedRoot romRoot = esde::resolveRomRoot(*settings, profile);
        snapshot.issues.insert(snapshot.issues.end(), romRoot.issues.begin(), romRoot.issues.end());
        snapshot.systems = esde::systemInfos(*systems, romRoot, profile, snapshot.issues);
        for (catalog::SystemInfo& system : snapshot.systems) {
            if (m_only && system.name != *m_only) {
                continue;
            }
            const esde::SystemDefinition* definition = systems->find(system.name.str());
            if (definition != nullptr) {
                readSystem(snapshot, system, *definition, *settings);
            }
        }
        std::sort(snapshot.entries.begin(), snapshot.entries.end(), catalog::entryOrder);
        return snapshot;
    }

private:
    void readSystem(catalog::LibrarySnapshot& snapshot, catalog::SystemInfo& system,
                    const esde::SystemDefinition& definition, const esde::EsdeSettings& settings) const {
        if (system.romFolder.empty()) {
            return; // its ROM folder could not be resolved: systemInfos() reported it
        }
        const fs::path file = esde::gamelistPath(snapshot.profile.esdeHome, system.name);
        auto gamelist = esde::readGamelist(file, system.name, system.romFolder);
        if (!gamelist) {
            if (gamelist.error().code == ErrorCode::NotFound) {
                system.gamelist = catalog::GamelistState::Absent;
                return;
            }
            system.gamelist = catalog::GamelistState::Failed;
            snapshot.issues.push_back(catalog::Issue{
                catalog::IssueSeverity::Error, "gamelist-failed", system.name.str(), core::pathToUtf8Lossy(file),
                fmt::format("the gamelist of {} cannot be read: {}", system.name.str(), gamelist.error().toString())});
            return;
        }
        system.gamelist = catalog::GamelistState::Read;
        system.alternativeEmulator = gamelist->alternativeEmulator;
        snapshot.issues.insert(snapshot.issues.end(), gamelist->issues.begin(), gamelist->issues.end());

        // The keys the <game> entries name, and the folders above them: the walk goes down those folders only.
        std::unordered_set<std::string> wanted;
        std::unordered_set<std::string> wantedFolders;
        for (const esde::GamelistEntry& entry : gamelist->entries) {
            if (entry.kind == esde::GamelistEntryKind::Game && !entry.outsideSystemFolder()) {
                wanted.insert(entry.key.key());
                for (std::string& parent : parentsOf(entry.key.key())) {
                    wantedFolders.insert(std::move(parent));
                }
            }
        }
        std::unordered_map<std::string, std::vector<ListedFile>> listed;
        std::vector<std::string> unlisted;
        std::unordered_set<std::string> hiddenFolders;
        std::string lastFolder;
        std::optional<Error> folderError;
        if (system.availability == catalog::Availability::Available) {
            const auto walked = core::walkTree(system.romFolder, [&](const core::TreeEntry& entry) {
                if (entry.kind == core::TreeEntryKind::Error) {
                    if (entry.relativePath == lastFolder) {
                        if (auto key = catalog::makeLookupKey(entry.relativePath)) {
                            unlisted.push_back(key->key());
                        }
                    }
                    return core::WalkAction::Continue;
                }
                auto key = catalog::makeLookupKey(entry.relativePath);
                if (!key) {
                    return core::WalkAction::SkipChildren;
                }
                const bool directory = entry.kind == core::TreeEntryKind::Directory;
                const std::size_t slash = entry.relativePath.rfind('/');
                bool hidden = entry.hidden || entry.relativePath[slash == std::string::npos ? 0 : slash + 1] == '.';
                for (const std::string& parent : parentsOf(key->key())) {
                    hidden = hidden || hiddenFolders.contains(parent);
                }
                if (wanted.contains(key->key())) {
                    listed[key->key()].push_back(ListedFile{
                        key->relativePath(),
                        catalog::FileFacts{directory, entry.size, catalog::fileTimeTicks(entry.lastWriteTime),
                                           hidden}});
                }
                if (!directory) {
                    return core::WalkAction::Continue;
                }
                if (!wantedFolders.contains(key->key())) {
                    return core::WalkAction::SkipChildren;
                }
                if (hidden) {
                    hiddenFolders.insert(key->key());
                }
                lastFolder = entry.relativePath;
                return core::WalkAction::Continue;
            });
            if (!walked) {
                folderError = walked.error();
            }
        }

        const std::vector<std::string>& extensions = definition.extensions;
        for (const esde::GamelistEntry& entry : gamelist->entries) {
            if (entry.kind != esde::GamelistEntryKind::Game) {
                continue;
            }
            catalog::CatalogEntry out;
            out.system = system.name;
            out.gamelistPath = entry.path;
            out.metadata = entry.metadata;
            out.raw = entry.raw;
            out.flags = entry.flags;
            out.altEmulator = entry.altEmulator;
            if (entry.outsideSystemFolder()) {
                out.state = catalog::EntryState::OutsideSystemFolder;
                out.title = catalog::defaultTitle(entry.metadata, entry.path);
                snapshot.entries.push_back(std::move(out));
                continue;
            }
            const std::string& key = entry.key.key();
            const bool underUnlisted = std::any_of(unlisted.begin(), unlisted.end(), [&](const std::string& folder) {
                return key.size() > folder.size() && key.starts_with(folder) && key[folder.size()] == '/';
            });
            const auto found = listed.find(key);
            if (found != listed.end()) {
                // The file as listed: the entry's own spelling when the disk holds it, else the first.
                const auto exact = std::find_if(found->second.begin(), found->second.end(), [&](const ListedFile& f) {
                    return f.relativePath == entry.key.relativePath();
                });
                const ListedFile& onDisk = exact != found->second.end() ? *exact : found->second.front();
                out.key = catalog::makeLookupKey(onDisk.relativePath).value_or(entry.key);
                out.file = onDisk.facts;
                const bool configured =
                    std::find(extensions.begin(), extensions.end(), extensionOf(onDisk.relativePath)) != extensions.end();
                if (!configured) {
                    out.state = catalog::EntryState::NotAGame;
                    out.notAGameReason = catalog::NotAGameReason::ExtensionNotConfigured;
                } else if (!settings.showHiddenFiles && onDisk.facts.hidden) {
                    out.state = catalog::EntryState::NotAGame;
                    out.notAGameReason = catalog::NotAGameReason::ExcludedBySetting;
                    out.excludingSetting = "ShowHiddenFiles";
                } else if (!settings.showHiddenGames && entry.flags.hidden) {
                    out.state = catalog::EntryState::NotAGame;
                    out.notAGameReason = catalog::NotAGameReason::ExcludedBySetting;
                    out.excludingSetting = "ShowHiddenGames";
                }
            } else if (system.availability != catalog::Availability::Available || folderError || underUnlisted) {
                out.key = entry.key;
                out.availability = catalog::Availability::SourceUnavailable;
                out.unavailableReason = system.availability != catalog::Availability::Available
                                            ? system.unavailableReason
                                            : catalog::SourceUnavailableReason::NotListable;
                const bool configured = std::find(extensions.begin(), extensions.end(),
                                                  extensionOf(entry.key.relativePath())) != extensions.end();
                out.state = configured ? catalog::EntryState::Game : catalog::EntryState::NotAGame;
                out.notAGameReason =
                    configured ? catalog::NotAGameReason::None : catalog::NotAGameReason::ExtensionNotConfigured;
            } else {
                out.key = entry.key;
                out.state = catalog::EntryState::MissingFile;
                out.availability = catalog::Availability::Missing;
            }
            out.title = catalog::defaultTitle(entry.metadata, out.key.relativePath(),
                                              out.file.has_value() && out.file->directory);
            snapshot.entries.push_back(std::move(out));
        }
        if (folderError) {
            snapshot.issues.push_back(catalog::Issue{
                catalog::IssueSeverity::Error, "rom-folder-unlisted", system.name.str(),
                core::pathToUtf8Lossy(system.romFolder),
                fmt::format("the ROM folder of {} cannot be listed: {}", system.name.str(), folderError->toString())});
        }
    }

    std::optional<catalog::SystemName> m_only;
};

// --- What both forms share ---------------------------------------------------------------------------------------------

/// The library, its settings and roots, its systems.
struct Library {
    catalog::LibraryProfile profile;
    esde::EsdeSettings settings;
    esde::ResolvedRoot romRoot;
    esde::ResolvedRoot mediaRoot;
    /// Nothing when the systems cannot be read (`systemsError` says why).
    std::optional<esde::SystemsConfiguration> systems;
    std::string systemsError;
};

/// The output a command's --output option names, else the context's.
struct Destination {
    std::optional<Output> file;
    Output* out = nullptr;
};

ExitCode finish(Destination& destination, CommandContext& context, ExitCode code) {
    if (destination.file) {
        if (const Status written = destination.file->finish(); !written) {
            context.err.writeLine(fmt::format("{}: {}", kName, written.error().toString()));
            return ExitCode::RuntimeFailure;
        }
    }
    return code;
}

/// "90.9" (one decimal, as the tables show shares), "-" when nothing is known.
std::string percentText(std::optional<double> share) {
    return share ? fmt::format("{:.1f}", *share * 100.0) : std::string("-");
}

Json countsJson(const catalog::CoverageCounts& counts) {
    Json withKind = Json::object();
    for (const MediaKind kind : catalog::kMediaKinds) {
        withKind[std::string(catalog::toString(kind))] = counts.with(kind);
    }
    return Json{{"entries", counts.entries},
                {"unknown", counts.unknown},
                {"withAny", counts.withAny},
                {"withKind", std::move(withKind)}};
}

Json populationsJson(const catalog::SystemCoverage& coverage) {
    Json populations = Json::object();
    for (const CoveragePopulation population : catalog::kCoveragePopulations) {
        populations[std::string(catalog::toString(population))] = countsJson(coverage[population]);
    }
    return populations;
}

void writeIssues(Output& out, std::span<const catalog::Issue> issues) {
    if (issues.empty()) {
        return;
    }
    out.writeLine();
    out.writeLine(fmt::format("Issues ({})", issues.size()));
    for (const catalog::Issue& issue : issues) {
        out.writeLine(fmt::format("  {:<8}{} {}", catalog::toString(issue.severity), issue.code, issue.reason));
    }
}

bool hasErrors(std::span<const catalog::Issue> issues) {
    return std::any_of(issues.begin(), issues.end(),
                       [](const catalog::Issue& issue) { return issue.severity == catalog::IssueSeverity::Error; });
}

// --- media --system --path ----------------------------------------------------------------------------------------

/// What the ROM folder holds at a path.
enum class RomPathKind { File, Folder, Missing, Unknown };

std::string_view toString(RomPathKind kind) {
    switch (kind) {
    case RomPathKind::File:
        return "file";
    case RomPathKind::Folder:
        return "folder";
    case RomPathKind::Missing:
        return "missing";
    case RomPathKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

RomPathKind romPathKind(const fs::path& romFolder, std::string_view relative) {
    const auto path = core::extendedLengthPathBelow(romFolder, relative);
    if (!path) {
        return RomPathKind::Unknown;
    }
    std::error_code error;
    const fs::file_status status = fs::status(*path, error);
    if (status.type() == fs::file_type::not_found) {
        // The folder above it may be gone too, or its drive: only a folder that is there makes the path missing.
        std::error_code folderError;
        return fs::is_directory(core::extendedLengthPath(romFolder), folderError) ? RomPathKind::Missing
                                                                                   : RomPathKind::Unknown;
    }
    if (error) {
        return RomPathKind::Unknown;
    }
    return fs::is_directory(status) ? RomPathKind::Folder : RomPathKind::File;
}

ExitCode showPath(CommandContext& context, Output& out, const Library& library, const catalog::SystemName& system,
                  std::string_view romPath, bool json) {
    const auto failure = [&](ExitCode code, const std::string& message) {
        context.err.writeLine(fmt::format("{}: {}", kName, message));
        return code;
    };
    const esde::SystemDefinition& definition = *library.systems->find(system.str());
    const auto romFolder = esde::systemRomFolder(definition, library.romRoot, library.profile);
    if (!romFolder) {
        return failure(ExitCode::ProblemsFound,
                       fmt::format("the ROM folder of {} is not known (its path or the ROM root cannot be resolved)",
                                   system.str()));
    }
    auto relative = core::resolveRelativePath(romPath, *romFolder);
    if (!relative) {
        return failure(ExitCode::UsageError,
                       relative.error().code == ErrorCode::OutOfRange
                           ? fmt::format("--path leads out of the ROM folder of {}: {}", system.str(),
                                         relative.error().toString())
                           : fmt::format("--path is no path below a ROM folder: {}", relative.error().toString()));
    }
    auto key = catalog::makeLookupKey(*relative);
    if (!key) {
        return failure(ExitCode::UsageError, fmt::format("--path: {}", key.error().toString()));
    }
    const RomPathKind kind = romPathKind(*romFolder, key->relativePath());
    const bool directory = kind == RomPathKind::Folder;

    esde::EsdeMediaResolver resolver(library.mediaRoot);
    const auto media = resolver.presence(system, *key, directory);
    if (!media) {
        return failure(ExitCode::ProblemsFound,
                       fmt::format("the media of {} cannot be known: {}", system.str(), media.error().toString()));
    }
    struct Row {
        MediaKind kind;
        catalog::MediaPresence presence;
        std::optional<fs::path> file;
    };
    std::vector<Row> rows;
    std::size_t present = 0;
    for (const MediaKind mediaKind : catalog::kMediaKinds) {
        Row row{mediaKind, (*media)[mediaKind], std::nullopt};
        if (row.presence.present) {
            ++present;
            if (auto file = resolver.path(system, *key, directory, mediaKind, row.presence)) {
                row.file = std::move(*file);
            }
        }
        rows.push_back(std::move(row));
    }
    const std::string mediaName = catalog::mediaStem(key->relativePath(), directory);

    if (json) {
        Json items = Json::array();
        for (const Row& row : rows) {
            items.push_back(Json{{"kind", std::string(catalog::toString(row.kind))},
                                 {"present", row.presence.present},
                                 {"extension", row.presence.present ? Json(row.presence.extension) : Json(nullptr)},
                                 {"path", row.file ? Json(pathText(*row.file)) : Json(nullptr)}});
        }
        const Json report{{"system", system.str()},
                          {"path", key->relativePath()},
                          {"romPath", std::string(toString(kind))},
                          {"mediaName", mediaName},
                          {"mediaRoot", rootJson(library.mediaRoot)},
                          {"media", std::move(items)}};
        out.write(report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
        return ExitCode::Ok;
    }
    const auto line = [&](std::string_view label, const std::string& value) {
        out.writeLine(fmt::format("{:<14}{}", label, value));
    };
    const std::string_view kindText = kind == RomPathKind::Folder    ? "a folder: its media keep its whole name"
                                      : kind == RomPathKind::File    ? "a file"
                                      : kind == RomPathKind::Missing ? "not there: its media are named as a file's"
                                                                     : "not known: its media are named as a file's";
    line("System", fmt::format("{} ({})", system.str(), definition.fullName));
    line("ROM path", fmt::format("{} ({})", key->relativePath(), kindText));
    line("Media name", mediaName);
    line("Media root", rootText(library.mediaRoot, "MediaDirectory", "unknown"));
    line("Media", fmt::format("{} of {} kinds", present, catalog::kMediaKindCount));
    for (const Row& row : rows) {
        out.writeLine(fmt::format("  {:<14}{}", catalog::toString(row.kind),
                                  row.file ? pathText(*row.file) : std::string("none")));
    }
    return ExitCode::Ok;
}

// --- media --coverage -------------------------------------------------------------------------------------------------

void writeCoverageTable(Output& out, std::string_view title, const catalog::MediaCoverage& coverage,
                        CoveragePopulation population) {
    out.writeLine();
    out.writeLine(title);
    std::string header = fmt::format("{:<14}{:>8}", "System", "Entries");
    for (const std::string_view column : kKindColumns) {
        header += fmt::format("{:>7}", column);
    }
    out.writeLine(header);
    const auto row = [&](std::string_view name, const catalog::CoverageCounts& counts) {
        std::string text = fmt::format("{:<14}{:>8}", name, counts.known());
        for (const MediaKind kind : catalog::kMediaKinds) {
            text += fmt::format("{:>7}", percentText(counts.share(kind)));
        }
        if (counts.unknown > 0) {
            text += fmt::format("  ({} unknown)", counts.unknown);
        }
        out.writeLine(text);
    };
    for (const catalog::SystemCoverage& system : coverage.systems) {
        if (system[population].entries > 0) {
            row(system.system.str(), system[population]);
        }
    }
    row("All", coverage.total[population]);
}

ExitCode showCoverage(CommandContext& context, Output& out, const Library& library,
                      const std::optional<catalog::SystemName>& system, bool json) {
    GamelistSource source(system);
    const auto snapshot = source.load(library.profile, catalog::LibraryLoadOptions{});
    if (!snapshot) {
        context.err.writeLine(fmt::format("{}: the library cannot be read: {}", kName, snapshot.error().toString()));
        return ExitCode::ProblemsFound;
    }
    esde::EsdeMediaResolver resolver(library.mediaRoot);
    const Status root = resolver.checkRoot();
    const auto coverage = catalog::measureCoverage(*snapshot, resolver, system);
    if (!coverage) {
        context.err.writeLine(fmt::format("{}: {}", kName, coverage.error().toString()));
        return ExitCode::RuntimeFailure;
    }

    // The issues: the media root's, the snapshot's (gamelists, ROM folders), each listed system's, the coverage's.
    std::vector<catalog::Issue> issues;
    if (!root) {
        issues.push_back(catalog::Issue{catalog::IssueSeverity::Error, std::string(esde::kMediaIssueRootUnavailable),
                                        std::string(), root.error().context,
                                        fmt::format("no media can be known: {}", root.error().message)});
    }
    for (const catalog::Issue& issue : snapshot->issues) {
        if (issue.severity != catalog::IssueSeverity::Info) {
            issues.push_back(issue);
        }
    }
    struct Folder {
        catalog::SystemName system;
        esde::MediaFolderState state;
    };
    std::vector<Folder> folders;
    for (const catalog::SystemCoverage& systemCoverage : coverage->systems) {
        const auto listed = resolver.listing(systemCoverage.system);
        if (!listed) {
            continue;
        }
        folders.push_back(Folder{systemCoverage.system, (*listed)->state});
        issues.insert(issues.end(), (*listed)->issues.begin(), (*listed)->issues.end());
    }
    issues.insert(issues.end(), coverage->issues.begin(), coverage->issues.end());
    const esde::MediaListingStats& stats = resolver.stats();
    spdlog::info("Media coverage: {} system folder(s) listed, {} files, {:.2f} s", stats.systemsListed, stats.files,
                 stats.seconds);
    const ExitCode code = hasErrors(issues) ? ExitCode::ProblemsFound : ExitCode::Ok;

    if (json) {
        Json kinds = Json::array();
        for (const MediaKind kind : catalog::kMediaKinds) {
            kinds.push_back(std::string(catalog::toString(kind)));
        }
        Json populations = Json::object();
        for (const CoveragePopulation population : catalog::kCoveragePopulations) {
            populations[std::string(catalog::toString(population))] = std::string(populationText(population));
        }
        Json systems = Json::array();
        for (const catalog::SystemCoverage& systemCoverage : coverage->systems) {
            const auto folder = std::find_if(folders.begin(), folders.end(),
                                             [&](const Folder& f) { return f.system == systemCoverage.system; });
            systems.push_back(Json{
                {"system", systemCoverage.system.str()},
                {"mediaFolder", folder != folders.end() ? Json(std::string(esde::toString(folder->state)))
                                                         : Json(nullptr)},
                {"populations", populationsJson(systemCoverage)}});
        }
        Json issueList = Json::array();
        for (const catalog::Issue& issue : issues) {
            issueList.push_back(issueJson(issue));
        }
        const Json report{{"mediaRoot", rootJson(library.mediaRoot)},
                          {"entries", "gamelists"},
                          {"kinds", std::move(kinds)},
                          {"populations", std::move(populations)},
                          {"listed", Json{{"systemFolders", stats.systemsListed}, {"files", stats.files}}},
                          {"total", populationsJson(coverage->total)},
                          {"systems", std::move(systems)},
                          {"issues", std::move(issueList)}};
        out.write(report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
        return code;
    }

    out.writeLine(fmt::format("{:<14}{}", "Media root", rootText(library.mediaRoot, "MediaDirectory", "unknown")));
    out.writeLine(fmt::format("{:<14}{}", "Entries",
                              "from the gamelists (the ROM scan of I2 step 7 adds the games without a gamelist entry)"));
    out.writeLine(fmt::format("{:<14}{} system folder(s) listed, {} files", "Listed", stats.systemsListed,
                              stats.files));
    std::string legend;
    for (std::size_t i = 0; i < kKindColumns.size(); ++i) {
        legend += fmt::format("{}{} {}", i == 0 ? "" : ", ", kKindColumns[i], catalog::toString(catalog::kMediaKinds[i]));
    }
    out.writeLine(fmt::format("{:<14}{}", "Columns", legend));
    out.writeLine(fmt::format("{:<14}{}", "", "a share of the entries whose media are known, in percent"));

    out.writeLine();
    out.writeLine("Populations");
    std::string header = fmt::format("{:<28}{:>8}", "Population", "Entries");
    for (const std::string_view column : kKindColumns) {
        header += fmt::format("{:>7}", column);
    }
    out.writeLine(header);
    for (const CoveragePopulation population : catalog::kCoveragePopulations) {
        const catalog::CoverageCounts& counts = coverage->total[population];
        std::string text = fmt::format("{:<28}{:>8}", catalog::toString(population), counts.known());
        for (const MediaKind kind : catalog::kMediaKinds) {
            text += fmt::format("{:>7}", percentText(counts.share(kind)));
        }
        if (counts.unknown > 0) {
            text += fmt::format("  ({} unknown)", counts.unknown);
        }
        out.writeLine(text);
    }
    for (const CoveragePopulation population : catalog::kCoveragePopulations) {
        out.writeLine(fmt::format("  {}: {}", catalog::toString(population), populationText(population)));
    }
    writeCoverageTable(out, "The gamelist entries whose file exists, per system", *coverage,
                       CoveragePopulation::GamelistEntriesWithFile);
    writeCoverageTable(out,
                       "The games, per system (those with a gamelist entry, until the ROM scan of I2 step 7 adds the "
                       "others)",
                       *coverage, CoveragePopulation::Games);
    writeIssues(out, issues);
    return code;
}

} // namespace

ExitCode runMedia(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Shows the media of a ROM path of a system, or the media coverage of the library.");
    parser.option("system", "name", "The ES-DE system (its name in es_systems.xml).")
        .option("path", "relative path", "A ROM path of the system, relative to its ROM folder (or as a gamelist "
                                         "writes it: \"./...\").")
        .flag("coverage", "Show the share of the entries that have each kind of media, per system.")
        .option("game", "id", "A game of the catalog (from I2 step 11).")
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
    const auto usage = [&](const std::string& message) {
        context.err.writeLine(fmt::format("{}: {}", kName, message));
        return ExitCode::UsageError;
    };
    if (args->has("game")) {
        return usage(fmt::format("--game: not implemented yet (I2 step {})", kGameStep));
    }
    const bool coverage = args->has("coverage");
    if (coverage && args->has("path")) {
        return usage("--coverage takes no --path");
    }
    if (!coverage && (!args->has("system") || !args->has("path"))) {
        return usage("give --system and --path (the media of a ROM path), or --coverage");
    }
    std::optional<catalog::SystemName> system;
    if (args->has("system")) {
        auto name = catalog::SystemName::parse(args->value("system").value_or(""));
        if (!name) {
            return usage(fmt::format("--system: {}", name.error().toString()));
        }
        system = std::move(*name);
    }
    const std::string romPath = args->value("path").value_or("");
    if (!coverage && romPath.empty()) {
        return usage("--path must not be empty");
    }
    auto output = outputFileOption(*args);
    if (!output) {
        return usage(output.error().toString());
    }
    Destination destination{std::move(*output), nullptr};
    destination.out = destination.file ? &*destination.file : &context.out;

    // The library library.json records; the dispatcher checked the user data directory against its folders.
    const fs::path file = context.paths.configDir() / esde::kLibraryFileName;
    auto profile = esde::readLibraryFile(file);
    if (!profile) {
        if (profile.error().code == ErrorCode::NotFound) {
            return usage(fmt::format("no library yet ({} does not exist): run 'retroverse-cli library create'",
                                     pathText(file)));
        }
        context.err.writeLine(fmt::format("{}: {}", kName, profile.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    Library library;
    library.profile = std::move(*profile);
    if (auto settings = esde::readSettings(library.profile.esdeHome)) {
        library.settings = std::move(*settings);
    } else {
        context.err.writeLine(fmt::format("{}: the ES-DE settings cannot be read: {}", kName,
                                          settings.error().toString()));
        return ExitCode::ProblemsFound;
    }
    library.romRoot = esde::resolveRomRoot(library.settings, library.profile);
    library.mediaRoot = esde::resolveMediaRoot(library.settings, library.profile);
    if (auto systems = esde::loadSystems(library.profile)) {
        library.systems = std::move(*systems);
    } else {
        context.err.writeLine(fmt::format("{}: the systems cannot be read: {}", kName, systems.error().toString()));
        return ExitCode::ProblemsFound;
    }
    if (system && library.systems->find(system->str()) == nullptr) {
        return usage(fmt::format("--system: no system named \"{}\" in es_systems.xml", system->str()));
    }

    const ExitCode code = coverage ? showCoverage(context, *destination.out, library, system, args->has("json"))
                                   : showPath(context, *destination.out, library, *system, romPath, args->has("json"));
    if (destination.file && destination.file->text().empty()) {
        return code; // nothing to write: the command failed before its results
    }
    return finish(destination, context, code);
}

} // namespace rv::cli
