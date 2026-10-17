// retroverse-cli media (I2 step 8): the media of an entry and the media coverage of the library, read in place from
// ES-DE's media folders (ADR-0008 §5, ADR-0003) with esde::EsdeMediaResolver, read-only.
//  - media --system <name> --path <relative path> [--json] [--output <file>]: the media of a ROM path of a system (as
//    listed on disk, or as a gamelist writes it: "./Collection A/Pocket Puzzle (World).gb"), kind by kind: the file
//    ES-DE shows, or none. The path is taken as the disk spells it, as far as it is there (ES-DE names media after the
//    file it lists). A folder (on disk) keeps its whole name, a file loses its last extension, a path that is not
//    there is named as a file is (a gamelist entry whose file is gone keeps its media).
//  - media --coverage [--system <name>] [--json] [--output <file>]: per system and kind, the share of the entries that
//    have a file of the kind, over the populations of catalog::CoveragePopulation (every gamelist entry, those whose
//    file exists, the scraped games, every game), the library's totals first. The entries are the library snapshot the
//    import reads (esde::EsdeLibrarySource, library_snapshot.hpp: every system's ROM folder listed with ES-DE's rules,
//    its gamelist merged in, disc tracks grouped under their sheet), only the system's with --system.
//  - media --game <id> [--json] [--output <file>] (I2 step 11a): the media of a game of the catalog as the last import
//    stored them, presence and extension per kind, each file's path derived again under the library's media root
//    (esde::EsdeMediaResolver::path(): paths are never stored, ADR-0008 §5); nothing on disk is looked at. The catalog
//    is read as `show` reads it (nothing written into it).
// The library is the one <user data>/config/library.json records (library create). Exit codes: 0; 1 when something
// could not be read (the media root, a media folder, the settings, the systems, a gamelist, a ROM folder, the ROM
// folder of the path), and for a --game the catalog does not have; 2 for invalid options, an unknown system, a path
// that leads out of its ROM folder, no library.json, a --game of another library than library.json's; 3 when
// library.json is invalid, the catalog cannot be read or the results cannot be written.

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/coverage.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "esde_text.hpp"
#include "library_snapshot.hpp"

namespace rv::cli {
namespace {

namespace fs = std::filesystem;
using catalog::CoveragePopulation;
using catalog::MediaKind;

constexpr std::string_view kName = "retroverse-cli media";
/// Where the coverage's entries come from (its JSON's "source"): the library snapshot of the ROM scan.
constexpr std::string_view kCoverageSource = "rom-scan";

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
        return "every game ES-DE shows, scraped or not, disc tracks grouped under their sheet";
    }
    return "";
}

// --- What both forms share -------------------------------------------------------------------------------------------

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

// --- media --system --path -------------------------------------------------------------------------------------------

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

/// `relative` (a path below `romFolder`, '/' between the elements) spelled as the file system holds it, element by
/// element while they are there: the file ES-DE lists, whose spelling names its media (which matters in a media folder
/// with case-sensitive names). The elements from the first one that is not there, or that holds a wildcard character,
/// stay as given.
std::string spelledOnDisk(const fs::path& romFolder, std::string_view relative) {
    std::string spelled;
    bool there = true;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = relative.find('/', start);
        std::string element(
            relative.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (there && element.find_first_of("*?") == std::string::npos) {
            const auto path = core::extendedLengthPathBelow(romFolder, spelled + element);
            WIN32_FIND_DATAW data{};
            const HANDLE find =
                path ? FindFirstFileExW(path->c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0)
                     : INVALID_HANDLE_VALUE;
            there = find != INVALID_HANDLE_VALUE;
            if (there) {
                FindClose(find);
                if (auto name = core::wideToUtf8(data.cFileName)) {
                    element = std::move(*name);
                }
            }
        } else {
            there = false;
        }
        spelled += element;
        if (slash == std::string_view::npos) {
            return spelled;
        }
        spelled += '/';
        start = slash + 1;
    }
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
        return failure(
            ExitCode::UsageError,
            relative.error().code == ErrorCode::OutOfRange
                ? fmt::format("--path leads out of the ROM folder of {}: {}", system.str(), relative.error().toString())
                : fmt::format("--path is no path below a ROM folder: {}", relative.error().toString()));
    }
    auto key = catalog::makeLookupKey(*relative);
    if (!key) {
        return failure(ExitCode::UsageError, fmt::format("--path: {}", key.error().toString()));
    }
    if (auto onDisk = catalog::makeLookupKey(spelledOnDisk(*romFolder, key->relativePath()))) {
        key = std::move(onDisk);
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

// --- media --game ----------------------------------------------------------------------------------------------------

ExitCode showGame(CommandContext& context, Output& out, const catalog::LibraryProfile& profile, catalog::GameId id,
                  bool json) {
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        return catalogFailure(context, reader.error());
    }
    const auto details = reader->query->details(id);
    if (!details) {
        return catalogFailure(context, details.error());
    }
    if (!*details) {
        context.err.writeLine(fmt::format("{}: the catalog has no game {}", kName, id.toString()));
        return ExitCode::ProblemsFound;
    }
    const catalog_db::GameDetails& game = **details;
    if (game.library != profile.id) {
        context.err.writeLine(fmt::format("{}: the game {} is of the library '{}', library.json names '{}'", kName,
                                          id.toString(), game.library.str(), profile.id.str()));
        return ExitCode::UsageError;
    }
    const catalog::IndexedEntry& indexed = game.indexed;
    const catalog::CatalogEntry& entry = indexed.entry;
    const esde::ResolvedRoot mediaRoot = mediaRootOf(profile);
    const auto paths = derivedMediaPaths(indexed, mediaRoot);
    const bool directory = entry.file ? entry.file->directory : entry.state == catalog::EntryState::Folder;
    const std::string mediaName = catalog::mediaStem(entry.key.relativePath(), directory);
    std::size_t present = 0;
    for (const catalog::MediaPresence& media : indexed.media.kinds) {
        present += media.present ? 1 : 0;
    }

    if (json) {
        Json items = Json::array();
        for (std::size_t i = 0; i < catalog::kMediaKindCount; ++i) {
            const catalog::MediaPresence& media = indexed.media.kinds[i];
            items.push_back(Json{{"kind", std::string(catalog::toString(catalog::kMediaKinds[i]))},
                                 {"present", media.present},
                                 {"extension", media.present ? Json(media.extension) : Json(nullptr)},
                                 {"path", paths[i] ? Json(pathText(*paths[i])) : Json(nullptr)}});
        }
        const Json report{{"game", id.toString()},
                          {"title", entry.title},
                          {"system", entry.system.str()},
                          {"path", entry.key.relativePath()},
                          {"directory", directory},
                          {"mediaName", mediaName},
                          {"mediaRoot", rootJson(mediaRoot)},
                          {"media", std::move(items)}};
        out.write(report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
        return ExitCode::Ok;
    }
    const auto line = [&](std::string_view label, const std::string& value) {
        out.writeLine(fmt::format("{:<14}{}", label, value));
    };
    line("Game", fmt::format("{} {}", id.toString(), entry.title));
    line("System", game.system ? fmt::format("{} ({})", entry.system.str(), game.system->fullName) : entry.system.str());
    line("ROM path", fmt::format("{}{}", entry.key.relativePath(), directory ? " (a folder: its media keep its whole name)"
                                                                             : ""));
    line("Media name", mediaName);
    line("Media root", rootText(mediaRoot, "MediaDirectory", "unknown (the ES-DE settings cannot be read)"));
    line("Media", fmt::format("{} of {} kinds, as the last import found them", present, catalog::kMediaKindCount));
    for (std::size_t i = 0; i < catalog::kMediaKindCount; ++i) {
        const catalog::MediaPresence& media = indexed.media.kinds[i];
        const std::string value = !media.present ? std::string("none")
                                  : paths[i]     ? pathText(*paths[i])
                                                 : fmt::format("{} (the path cannot be derived: no media root)",
                                                               media.extension);
        out.writeLine(fmt::format("  {:<14}{}", catalog::toString(catalog::kMediaKinds[i]), value));
    }
    return ExitCode::Ok;
}

// --- media --coverage ------------------------------------------------------------------------------------------------

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
    const auto read =
        readLibrarySnapshot(library.profile, system ? std::optional<std::string>(system->str()) : std::nullopt);
    if (!read) {
        context.err.writeLine(fmt::format("{}: the library cannot be read: {}", kName, read.error().toString()));
        return ExitCode::ProblemsFound;
    }
    const catalog::LibrarySnapshot& snapshot = read->snapshot;
    spdlog::info("Media coverage: the library snapshot read in {:.2f} s, {} entr(ies), {} ROM folder(s) listed",
                 std::chrono::duration<double>(read->statistics.totalTime).count(), snapshot.entries.size(),
                 read->statistics.systemsScanned);
    esde::EsdeMediaResolver resolver(library.mediaRoot);
    const Status root = resolver.checkRoot();
    const auto coverage = catalog::measureCoverage(snapshot, resolver, system);
    if (!coverage) {
        context.err.writeLine(fmt::format("{}: {}", kName, coverage.error().toString()));
        return ExitCode::RuntimeFailure;
    }

    // The issues: the media root's, the snapshot's (settings, systems, gamelists, ROM folders, disc groups), each
    // listed system's, the coverage's.
    std::vector<catalog::Issue> issues;
    if (!root) {
        issues.push_back(catalog::Issue{catalog::IssueSeverity::Error, std::string(esde::kMediaIssueRootUnavailable),
                                        std::string(), root.error().context,
                                        fmt::format("no media can be known: {}", root.error().message)});
    }
    for (const catalog::Issue& issue : snapshot.issues) {
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
    spdlog::info("Media coverage: the media folders of {} system(s) looked up, {} files, {:.2f} s; {} folder(s) asked "
                 "for their case rule, {} media file(s) looked up by ES-DE's spelling",
                 stats.systemsListed, stats.files, stats.seconds, stats.caseChecks, stats.fileChecks);
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
            systems.push_back(
                Json{{"system", systemCoverage.system.str()},
                     {"mediaFolder",
                      folder != folders.end() ? Json(std::string(esde::toString(folder->state))) : Json(nullptr)},
                     {"populations", populationsJson(systemCoverage)}});
        }
        Json issueList = Json::array();
        for (const catalog::Issue& issue : issues) {
            issueList.push_back(issueJson(issue));
        }
        const Json report{{"mediaRoot", rootJson(library.mediaRoot)},
                          {"source", std::string(kCoverageSource)},
                          {"entries", snapshot.entries.size()},
                          {"kinds", std::move(kinds)},
                          {"populations", std::move(populations)},
                          {"listed", Json{{"systems", stats.systemsListed},
                                          {"files", stats.files},
                                          {"caseChecks", stats.caseChecks},
                                          {"fileChecks", stats.fileChecks}}},
                          {"total", populationsJson(coverage->total)},
                          {"systems", std::move(systems)},
                          {"issues", std::move(issueList)}};
        out.write(report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
        return code;
    }

    out.writeLine(fmt::format("{:<14}{}", "Media root", rootText(library.mediaRoot, "MediaDirectory", "unknown")));
    out.writeLine(fmt::format("{:<14}{} in the library snapshot (the ROM folders listed with ES-DE's rules, the "
                              "gamelists merged in, disc tracks grouped under their sheet)",
                              "Entries", snapshot.entries.size()));
    out.writeLine(
        fmt::format("{:<14}{} system(s), {} files in their media folders", "Listed", stats.systemsListed, stats.files));
    std::string legend;
    for (std::size_t i = 0; i < kKindColumns.size(); ++i) {
        legend +=
            fmt::format("{}{} {}", i == 0 ? "" : ", ", kKindColumns[i], catalog::toString(catalog::kMediaKinds[i]));
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
    writeCoverageTable(out, "The games, per system", *coverage, CoveragePopulation::Games);
    writeIssues(out, issues);
    return code;
}

} // namespace

ExitCode runMedia(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Shows the media of a ROM path of a system, or the media coverage of the library.");
    parser.option("system", "name", "The ES-DE system (its name in es_systems.xml).")
        .option("path", "relative path",
                "A ROM path of the system, relative to its ROM folder (or as a gamelist "
                "writes it: \"./...\").")
        .flag("coverage", "Show the share of the entries that have each kind of media, per system.")
        .option("game", "id", "A game of the catalog (its GameId): its media as the last import stored them.")
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
        if (args->has("coverage") || args->has("path") || args->has("system")) {
            return usage("--game takes neither --system, --path nor --coverage");
        }
        const auto id = catalog::GameId::parse(args->value("game").value_or(""));
        if (!id) {
            return usage(fmt::format("--game: {}", id.error().toString()));
        }
        auto output = outputFileOption(*args);
        if (!output) {
            return usage(output.error().toString());
        }
        Destination destination{std::move(*output), nullptr};
        destination.out = destination.file ? &*destination.file : &context.out;
        ExitCode failure = ExitCode::RuntimeFailure;
        const auto profile = readRecordedLibrary(context, kName, failure);
        if (!profile) {
            return failure;
        }
        const ExitCode code = showGame(context, *destination.out, *profile, *id, args->has("json"));
        if (destination.file && destination.file->text().empty()) {
            return code; // nothing to write: the command failed before its results
        }
        return finish(destination, context, code);
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

    ExitCode failure = ExitCode::RuntimeFailure;
    auto profile = readRecordedLibrary(context, kName, failure);
    if (!profile) {
        return failure;
    }
    Library library;
    library.profile = std::move(*profile);
    if (auto settings = esde::readSettings(library.profile.esdeHome)) {
        library.settings = std::move(*settings);
    } else {
        context.err.writeLine(
            fmt::format("{}: the ES-DE settings cannot be read: {}", kName, settings.error().toString()));
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
