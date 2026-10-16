// retroverse-cli discover (I2 step 2): finds the ES-DE installation the way ES-DE 3.4.1 finds itself (ADR-0008 §6, §8)
// and prints what a library would read: the ES-DE home and install folder with their sources, ES-DE's version, the
// settings Retroverse uses, the ROM and media roots with their sources, the find rules, the systems that have a ROM
// folder and the gamelists found, then the issues met on the way. It only reads: nothing is written (library create
// records a library). Exit codes: 0, 1 when an error was met (no ES-DE home, a configuration that cannot be read), 2
// for invalid options, 3 when the results cannot be written.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/snapshot.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/platform_win/system_probe.hpp>

#include "commands.hpp"
#include "esde_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli discover";

std::string text(const std::filesystem::path& path) {
    return pathText(path);
}

/// "1 core", "2 cores", "0 entries".
std::string counted(std::size_t count, std::string_view one, std::string_view many = {}) {
    if (count == 1) {
        return fmt::format("1 {}", one);
    }
    return many.empty() ? fmt::format("{} {}s", count, one) : fmt::format("{} {}", count, many);
}

/// Everything discover found.
struct Report {
    esde::EsdeDiscovery discovery;
    std::optional<catalog::LibraryProfile> profile;
    /// The settings, or why they could not be read; nothing without an ES-DE home.
    std::optional<Expected<esde::EsdeSettings>> settings;
    std::optional<esde::ResolvedRoot> romRoot;
    std::optional<esde::ResolvedRoot> mediaRoot;
    std::optional<esde::FindRules> findRules;
    std::optional<Expected<esde::SystemsConfiguration>> systems;
    std::vector<catalog::SystemInfo> systemInfos;
    struct Gamelist {
        std::string system;
        std::filesystem::path file;
        bool configured = false;
    };
    std::vector<Gamelist> gamelists;
    std::vector<catalog::Issue> issues;

    [[nodiscard]] bool failed() const {
        return std::any_of(issues.begin(), issues.end(), [](const catalog::Issue& issue) {
            return issue.severity == catalog::IssueSeverity::Error;
        });
    }
};

void append(std::vector<catalog::Issue>& to, const std::vector<catalog::Issue>& issues) {
    to.insert(to.end(), issues.begin(), issues.end());
}

catalog::Issue failureIssue(std::string code, const Error& error) {
    return catalog::Issue{catalog::IssueSeverity::Error, std::move(code), {}, error.context, error.message};
}

/// A folder name as the file system compares it (its upper case; the name itself when the system cannot case it).
std::string folderKey(const std::string& name) {
    auto upper = core::fileSystemUpperCase(name);
    return upper ? std::move(*upper) : name;
}

/// The gamelists ES-DE would read for the configured systems, then the gamelists of folders no system names (the
/// folder names compared as the file system compares them: gamelists\NES is nes's).
std::vector<Report::Gamelist> findGamelists(const Report& report, const esde::EsdeSettings& settings,
                                            const std::filesystem::path& esdeHome) {
    std::vector<Report::Gamelist> found;
    std::vector<std::string> configured;
    for (const catalog::SystemInfo& system : report.systemInfos) {
        configured.push_back(folderKey(system.name.str()));
        const std::optional<std::filesystem::path> folder =
            system.romFolder.empty() ? std::nullopt : std::optional(system.romFolder);
        std::filesystem::path file = esde::gamelistFile(system.name, folder, settings, esdeHome);
        std::error_code error;
        if (std::filesystem::is_regular_file(file, error)) {
            found.push_back({system.name.str(), std::move(file), true});
        }
    }
    std::error_code error;
    std::vector<Report::Gamelist> others;
    for (std::filesystem::directory_iterator it(esdeHome / "gamelists", error), end; !error && it != end;
         it.increment(error)) {
        std::error_code entryError;
        if (!it->is_directory(entryError)) {
            continue;
        }
        const auto name = core::tryPathToUtf8(it->path().filename());
        if (!name || std::find(configured.begin(), configured.end(), folderKey(*name)) != configured.end()) {
            continue;
        }
        std::filesystem::path file = it->path() / "gamelist.xml";
        if (std::filesystem::is_regular_file(file, entryError)) {
            others.push_back({*name, std::move(file), false});
        }
    }
    std::sort(others.begin(), others.end(),
              [](const Report::Gamelist& a, const Report::Gamelist& b) { return a.system < b.system; });
    found.insert(found.end(), others.begin(), others.end());
    return found;
}

Report discover(const esde::EsdeDiscovery& discovery) {
    Report report;
    report.discovery = discovery;
    append(report.issues, discovery.issues);
    auto profile = esde::profileOf(discovery, catalog::LibraryId());
    if (!profile) {
        return report;
    }
    report.profile = std::move(*profile);
    report.settings = esde::readSettings(report.profile->esdeHome);
    if (*report.settings) {
        const esde::EsdeSettings& settings = **report.settings;
        append(report.issues, settings.issues);
        report.romRoot = esde::resolveRomRoot(settings, *report.profile);
        report.mediaRoot = esde::resolveMediaRoot(settings, *report.profile);
        append(report.issues, report.romRoot->issues);
        append(report.issues, report.mediaRoot->issues);
    } else {
        report.issues.push_back(failureIssue("settings-unreadable", report.settings->error()));
    }
    report.findRules = esde::loadFindRules(*report.profile);
    append(report.issues, report.findRules->issues);
    report.systems = esde::loadSystems(*report.profile);
    if (!*report.systems) {
        report.issues.push_back(failureIssue("systems-unreadable", report.systems->error()));
        return report;
    }
    append(report.issues, (*report.systems)->issues);
    if (report.romRoot) {
        report.systemInfos = esde::systemInfos(**report.systems, *report.romRoot, *report.profile, report.issues);
        report.gamelists = findGamelists(report, **report.settings, report.profile->esdeHome);
    }
    return report;
}

std::string sourceText(const esde::EsdeDiscovery& discovery) {
    switch (discovery.homeSource) {
    case esde::HomeSource::Explicit:
        return "given";
    case esde::HomeSource::Portable:
        return fmt::format("portable: {}", text(discovery.portableFile.value_or(std::filesystem::path())));
    case esde::HomeSource::Default:
        return "default: the user's home folder";
    case esde::HomeSource::None:
        break;
    }
    return "not found";
}

std::vector<const catalog::SystemInfo*> withRomFolder(const Report& report) {
    std::vector<const catalog::SystemInfo*> systems;
    for (const catalog::SystemInfo& system : report.systemInfos) {
        if (system.availability == catalog::Availability::Available) {
            systems.push_back(&system);
        }
    }
    return systems;
}

std::size_t customCount(const esde::SystemsConfiguration& systems) {
    return static_cast<std::size_t>(
        std::count_if(systems.systems.begin(), systems.systems.end(), [](const esde::SystemDefinition& system) {
            return system.origin == esde::ConfigOrigin::Custom;
        }));
}

void writeText(const Report& report, Output& out) {
    const esde::EsdeDiscovery& discovery = report.discovery;
    const auto line = [&](std::string_view label, const std::string& value) {
        out.writeLine(fmt::format("{:<18}{}", label, value));
    };
    if (discovery.esdeHome) {
        line("ES-DE home", fmt::format("{} ({}{})", text(*discovery.esdeHome), sourceText(discovery),
                                       discovery.esdeHomeExists ? "" : "; missing"));
        line("Home folder (~)", text(*discovery.homeFolder()));
    } else {
        line("ES-DE home", "not found");
    }
    if (discovery.installFolder) {
        const std::string source = discovery.installSource == esde::InstallSource::Explicit
                                       ? std::string("given")
                                       : fmt::format("uninstall entry {}", discovery.uninstallKey);
        line("Install folder", fmt::format("{} ({}{})", text(*discovery.installFolder), source,
                                           discovery.executableFound ? "" : "; no ES-DE.exe"));
    } else {
        line("Install folder", "not found");
    }
    line("Version", discovery.version.empty() ? std::string("unknown") : discovery.version);
    line("Portable", discovery.homeSource == esde::HomeSource::Portable ? std::string("yes")
                     : discovery.portableFile ? std::string("no (portable.txt ignored)")
                                              : std::string("no"));
    if (report.settings) {
        if (*report.settings) {
            const esde::EsdeSettings& settings = **report.settings;
            line("Settings", fmt::format("{} ({})", text(settings.file), esde::toString(settings.source)));
            const auto value = [&](std::string_view key, const std::string& shown) {
                out.writeLine(fmt::format("  {:<28}{}", key, shown));
            };
            const auto flag = [](bool on) { return std::string(on ? "true" : "false"); };
            value("ROMDirectory", settings.romDirectory);
            value("MediaDirectory", settings.mediaDirectory);
            value("ShowHiddenFiles", flag(settings.showHiddenFiles));
            value("ShowHiddenGames", flag(settings.showHiddenGames));
            value("ParseGamelistOnly", flag(settings.parseGamelistOnly));
            value("LegacyGamelistFileLocation", flag(settings.legacyGamelistFileLocation));
            value("AlternativeEmulatorPerGame", flag(settings.alternativeEmulatorPerGame));
            value("UIMode", std::string(esde::toString(settings.uiMode)));
        } else {
            line("Settings", fmt::format("cannot be read: {}", report.settings->error().message));
        }
    }
    if (report.profile) {
        line("ROM root", rootText(report.romRoot, "ROMDirectory", "unknown (the settings cannot be read)"));
        line("Media root", rootText(report.mediaRoot, "MediaDirectory", "unknown (the settings cannot be read)"));
    }
    if (report.findRules) {
        line("Find rules", fmt::format("{}, {}, {}", counted(report.findRules->emulators.size(), "emulator"),
                                       counted(report.findRules->cores.size(), "core"),
                                       counted(report.findRules->entryCount(), "entry", "entries")));
    }
    if (report.systems && *report.systems) {
        const esde::SystemsConfiguration& systems = **report.systems;
        const std::vector<const catalog::SystemInfo*> available = withRomFolder(report);
        line("Systems", fmt::format("{} configured ({} custom{}), {} commands; {} with a ROM folder:",
                                    systems.systems.size(), customCount(systems),
                                    systems.loadExclusive ? ", loadExclusive" : "", systems.commandCount(),
                                    available.size()));
        for (const catalog::SystemInfo* system : available) {
            out.writeLine(fmt::format("  {:<16}{:<44}{}", system->name.str(), system->fullName,
                                      text(system->romFolder)));
        }
    } else if (report.systems) {
        line("Systems", fmt::format("cannot be read: {}", report.systems->error().message));
    }
    if (report.profile) {
        line("Gamelists", fmt::format("{} found{}", report.gamelists.size(), report.gamelists.empty() ? "" : ":"));
        for (const Report::Gamelist& gamelist : report.gamelists) {
            out.writeLine(fmt::format("  {:<16}{}{}", gamelist.system, text(gamelist.file),
                                      gamelist.configured ? "" : " (no such system)"));
        }
    }
    line("Issues", fmt::format("{}{}", report.issues.size(), report.issues.empty() ? "" : ":"));
    for (const catalog::Issue& issue : report.issues) {
        out.writeLine(fmt::format("  {:<8} {}: {}{}", catalog::toString(issue.severity), issue.code, issue.reason,
                                  issue.file.empty() ? "" : fmt::format(" [{}]", issue.file)));
    }
}


Json reportJson(const Report& report) {
    const esde::EsdeDiscovery& discovery = report.discovery;
    Json json;
    json["esdeHome"] = Json{{"path", pathJson(discovery.esdeHome)},
                            {"source", std::string(esde::toString(discovery.homeSource))},
                            {"exists", discovery.esdeHomeExists}};
    json["homeFolder"] = pathJson(discovery.homeFolder());
    const auto textOrNull = [](const std::string& value) { return value.empty() ? Json(nullptr) : Json(value); };
    json["install"] = Json{{"path", pathJson(discovery.installFolder)},
                           {"source", std::string(esde::toString(discovery.installSource))},
                           {"uninstallKey", textOrNull(discovery.uninstallKey)},
                           {"executableFound", discovery.executableFound}};
    json["version"] = textOrNull(discovery.version);
    json["portable"] = Json{{"file", pathJson(discovery.portableFile)},
                            {"inUse", discovery.homeSource == esde::HomeSource::Portable}};
    if (report.settings && *report.settings) {
        const esde::EsdeSettings& settings = **report.settings;
        json["settings"] = Json{{"file", text(settings.file)},
                                {"source", std::string(esde::toString(settings.source))},
                                {"values",
                                 Json{{"ROMDirectory", settings.romDirectory},
                                      {"MediaDirectory", settings.mediaDirectory},
                                      {"ShowHiddenFiles", settings.showHiddenFiles},
                                      {"ShowHiddenGames", settings.showHiddenGames},
                                      {"ParseGamelistOnly", settings.parseGamelistOnly},
                                      {"LegacyGamelistFileLocation", settings.legacyGamelistFileLocation},
                                      {"AlternativeEmulatorPerGame", settings.alternativeEmulatorPerGame},
                                      {"UIMode", std::string(esde::toString(settings.uiMode))}}}};
    } else {
        json["settings"] = nullptr;
    }
    json["romRoot"] = rootJson(report.romRoot);
    json["mediaRoot"] = rootJson(report.mediaRoot);
    if (report.findRules) {
        Json files = Json::array();
        for (const std::filesystem::path& file : report.findRules->files) {
            files.push_back(text(file));
        }
        json["findRules"] = Json{{"emulators", report.findRules->emulators.size()},
                                 {"cores", report.findRules->cores.size()},
                                 {"entries", report.findRules->entryCount()},
                                 {"files", std::move(files)}};
    } else {
        json["findRules"] = nullptr;
    }
    if (report.systems && *report.systems) {
        const esde::SystemsConfiguration& systems = **report.systems;
        Json list = Json::array();
        for (const catalog::SystemInfo* system : withRomFolder(report)) {
            const esde::SystemDefinition* definition = systems.find(system->name.str());
            list.push_back(Json{{"name", system->name.str()},
                                {"fullName", system->fullName},
                                {"romFolder", text(system->romFolder)},
                                {"origin", std::string(esde::toString(definition != nullptr
                                                                          ? definition->origin
                                                                          : esde::ConfigOrigin::Bundled))}});
        }
        json["systems"] = Json{{"configured", systems.systems.size()},
                               {"custom", customCount(systems)},
                               {"commands", systems.commandCount()},
                               {"loadExclusive", systems.loadExclusive},
                               {"bundledFile", text(systems.bundledFile)},
                               {"customFile", pathJson(systems.customFile)},
                               {"withRomFolder", std::move(list)}};
    } else {
        json["systems"] = nullptr;
    }
    Json gamelists = Json::array();
    for (const Report::Gamelist& gamelist : report.gamelists) {
        gamelists.push_back(
            Json{{"system", gamelist.system}, {"file", text(gamelist.file)}, {"configured", gamelist.configured}});
    }
    json["gamelists"] = std::move(gamelists);
    Json issues = Json::array();
    for (const catalog::Issue& issue : report.issues) {
        issues.push_back(issueJson(issue));
    }
    json["issues"] = std::move(issues);
    return json;
}

} // namespace

ExitCode runDiscover(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Finds the ES-DE installation as ES-DE finds itself and prints what a library would read. "
                           "Writes nothing.");
    parser.option("home", "dir", "ES-DE's home folder, as ES-DE's own --home takes it: the ES-DE home is <dir>\\ES-DE.")
        .option("install", "dir", "ES-DE's install folder, the folder of ES-DE.exe (default: its uninstall entry).")
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
    auto home = folderOption(*args, "home");
    auto install = folderOption(*args, "install");
    for (const Error* error : {output ? nullptr : &output.error(), home ? nullptr : &home.error(),
                               install ? nullptr : &install.error()}) {
        if (error != nullptr) {
            context.err.writeLine(fmt::format("{}: {}", kName, error->toString()));
            return ExitCode::UsageError;
        }
    }

    esde::DiscoveryOptions options;
    options.homeFolder = *home;
    options.installFolder = *install;
    const platform_win::Win32SystemProbe probe;
    const auto discovery = esde::discoverEsde(options, probe);
    if (!discovery) {
        context.err.writeLine(fmt::format("{}: {}", kName, discovery.error().toString()));
        return ExitCode::UsageError;
    }
    const Report report = discover(*discovery);
    spdlog::info("discover: ES-DE home {} ({}), install folder {} ({}), {} issue(s)",
                 discovery->esdeHome ? "found" : "not found", esde::toString(discovery->homeSource),
                 discovery->installFolder ? "found" : "not found", esde::toString(discovery->installSource),
                 report.issues.size());
    spdlog::debug("discover: ES-DE home {}, install folder {}",
                  discovery->esdeHome ? text(*discovery->esdeHome) : std::string("-"),
                  discovery->installFolder ? text(*discovery->installFolder) : std::string("-"));

    Output& out = *output ? **output : context.out;
    if (args->has("json")) {
        out.write(reportJson(report).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    } else {
        writeText(report, out);
    }
    if (*output) {
        if (const Status written = (*output)->finish(); !written) {
            context.err.writeLine(fmt::format("{}: {}", kName, written.error().toString()));
            return ExitCode::RuntimeFailure;
        }
    }
    return report.failed() ? ExitCode::ProblemsFound : ExitCode::Ok;
}

} // namespace rv::cli
