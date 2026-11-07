// retroverse-cli profiles (I2 step 10; since I7 step 4 over library::launchTable()): the emulator profiles Retroverse knows
// and, for each system of the library library.json records, how its games launch (ADR-0008 §8): the command or profile
// chosen and why (Retroverse's choice
// for the system: the catalog database's from `assign`, else emulators.json's, I2 step 12; the gamelist's
// <alternativeEmulator>, the first command), its readiness without a game, the find rule and entry that found the
// program and the core, the blockers; then every command of the system
// and every profile serving it with their own readiness, so that a system that cannot launch shows what could. What
// depends on each game (its file, its <altemulator>, an %INJECT% file) is the launch's: `launch --dry-run`. I11 step
// 12: the programs of emulators.json (ADR-0020 §1) with what each does in VR, the program each system's launch starts,
// and which programs the library's launches use. Nothing is started or written. Exit codes: 0; 1 with --system when that system is not ready; 2 for invalid options, an unknown
// system or no library.json; 3 when ES-DE's configuration or the results cannot be read or written.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/launcher/arg_template.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/library/launch_table.hpp>

#include "commands.hpp"
#include "launch_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli profiles";

/// A profile and where its executable is.
struct ProfileReport {
    const launcher::EmulatorProfile* profile = nullptr;
    Expected<launcher::LocatedExecutable> executable;
};

using SystemReport = library::SystemLaunchRow;

struct Report {
    std::vector<ProfileReport> profiles;
    std::vector<SystemReport> systems;
};

std::string labelText(const std::string& label) {
    return label.empty() ? std::string("(unlabelled)") : label;
}

/// The program a translation starts (I11 step 12), when it found one that emulators.json describes.
const launcher::ProgramProfile* programOf(const LaunchLibrary& library, const catalog::LaunchTranslation& translation) {
    return translation.program ? library.profiles.programFor(translation.program->path) : nullptr;
}

/// What a program does in VR, in a few words.
std::string vrSummary(const launcher::ProgramProfile& program) {
    const launcher::VrSettings& vr = program.vr;
    std::vector<std::string> parts;
    if (!vr.addArguments.empty()) {
        parts.push_back(fmt::format("adds {}", fmt::join(vr.addArguments, " ")));
    }
    if (!vr.dropArguments.empty()) {
        parts.push_back(fmt::format("drops {}", fmt::join(vr.dropArguments, " ")));
    }
    if (vr.appendConfig) {
        parts.push_back(fmt::format("passes {} {}", vr.appendConfig->option, vr.appendConfig->fileName));
    }
    parts.push_back(fmt::format("load {}", launcher::toString(vr.inGame)));
    return fmt::format("{}", fmt::join(parts, "; "));
}

void writeText(const LaunchLibrary& library, const Report& report, Output& out) {
    const auto line = [&](std::string_view label, const std::string& text) {
        out.writeLine(fmt::format("{:<18}{}", label, text));
    };
    line("Library", fmt::format("{}: ES-DE home {}, install folder {}", library.profile.id.str(),
                                pathText(library.profile.esdeHome),
                                library.profile.installFolder ? pathText(*library.profile.installFolder)
                                                              : std::string("not known")));
    line("ROM root", rootText(library.romRoot, "ROMDirectory", "unknown"));
    line("Profiles", fmt::format("{}{}", report.profiles.size(), report.profiles.empty() ? "" : ":"));
    for (const ProfileReport& entry : report.profiles) {
        const launcher::EmulatorProfile& profile = *entry.profile;
        out.writeLine(fmt::format("  {:<16}{} ({}, {} system(s)): {}", profile.id, profile.displayName,
                                  launcher::toString(profile.origin), profile.systems.size(),
                                  entry.executable ? fmt::format("{} ({} \"{}\")", pathText(entry.executable->path),
                                                                 launcher::toString(entry.executable->rule),
                                                                 entry.executable->entry)
                                                   : fmt::format("not found: {}", entry.executable.error().message)));
    }
    line("Programs", fmt::format("{}{}", library.profiles.programs.size(), library.profiles.programs.empty() ? "" : ":"));
    for (const launcher::ProgramProfile& program : library.profiles.programs) {
        out.writeLine(fmt::format("  {:<16}{} ({}, {}): VR {}{}", program.id, program.displayName,
                                  launcher::toString(program.origin), fmt::join(program.files, ", "), vrSummary(program),
                                  program.verified ? "; verified" : ""));
    }
    if (!library.assignedChoices.empty()) {
        line("Assigned", fmt::format("{} (the catalog database, retroverse-cli assign; before emulators.json's):",
                                     library.assignedChoices.size()));
        for (const auto& [system, choice] : library.assignedChoices) {
            out.writeLine(fmt::format("  {:<16}{}", system, choiceText(choice)));
        }
    }
    if (!library.profiles.systemChoices.empty()) {
        line("System choices", fmt::format("{} (emulators.json):", library.profiles.systemChoices.size()));
        for (const auto& [system, choice] : library.profiles.systemChoices) {
            out.writeLine(fmt::format("  {:<16}{}{}", system, choiceText(choice),
                                      library.assignedChoices.contains(system) ? " (assign's choice ranks first)"
                                                                               : ""));
        }
    }
    std::map<catalog::LaunchReadiness, std::size_t> counts;
    for (const SystemReport& system : report.systems) {
        ++counts[system.resolution.readiness()];
    }
    std::string summary;
    for (const auto& [readiness, count] : counts) {
        summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", count, catalog::describe(readiness));
    }
    // The programs the systems' launches start (I11 step 12).
    std::map<std::string, std::vector<std::string>> inUse;
    for (const SystemReport& system : report.systems) {
        const launcher::ProgramProfile* program = programOf(library, system.resolution.translation);
        inUse[program != nullptr ? program->id : std::string()].push_back(system.system.name.str());
    }
    if (!report.systems.empty()) {
        line("Programs in use", "");
        for (const auto& [id, systems] : inUse) {
            out.writeLine(fmt::format("  {:<16}{} system(s): {}", id.empty() ? "(none known)" : id, systems.size(),
                                      fmt::join(systems, ", ")));
        }
    }
    line("Systems", fmt::format("{}{}{}", report.systems.size(), summary.empty() ? "" : " (" + summary + ")",
                                report.systems.empty() ? "" : ":"));
    for (const SystemReport& system : report.systems) {
        const catalog::LaunchResolution& resolution = system.resolution;
        out.writeLine(fmt::format("  {} ({})", system.system.name.str(), system.system.fullName));
        out.writeLine(fmt::format("    {:<18}{}: {} ({})", "launches with", catalog::describe(resolution.readiness()),
                                  choiceText(resolution.chosen.choice),
                                  whyText(resolution.chosen, systemChoiceOrigin(library, system.system.name.str()))));
        for (const std::string& warning : resolution.chosen.warnings) {
            out.writeLine(fmt::format("    {:<18}{}", "warning", warning));
        }
        if (const launcher::ProgramProfile* program = programOf(library, resolution.translation)) {
            out.writeLine(fmt::format("    {:<18}{} ('{}')", "program", program->displayName, program->id));
        }
        writeTranslationDetails(out, resolution.translation, "    ", 18, false);
        if (!system.commands.empty()) {
            out.writeLine("    commands");
            for (const library::LaunchOption& command : system.commands) {
                const bool chosen = resolution.chosen.choice == command.choice && !resolution.chosen.blocker;
                const catalog::LaunchTranslation& translation = command.translation;
                out.writeLine(fmt::format("      {} {:<34}{}{}", chosen ? '*' : ' ', labelText(command.choice.value),
                                          catalog::describe(translation.readiness()),
                                          translation.blockers.empty()
                                              ? std::string()
                                              : ": " + translation.blockers.front().message));
            }
        }
        if (!system.profiles.empty()) {
            out.writeLine("    profiles");
            for (const library::LaunchOption& profile : system.profiles) {
                const bool chosen = resolution.chosen.choice == profile.choice;
                const catalog::LaunchTranslation& translation = profile.translation;
                out.writeLine(fmt::format("      {} {:<34}{}{}", chosen ? '*' : ' ', profile.choice.value,
                                          catalog::describe(translation.readiness()),
                                          translation.blockers.empty()
                                              ? std::string()
                                              : ": " + translation.blockers.front().message));
            }
        }
    }
    line("Issues", fmt::format("{}{}", library.issues.size(), library.issues.empty() ? "" : ":"));
    for (const catalog::Issue& issue : library.issues) {
        out.writeLine(fmt::format("  {:<8} {}: {}{}", catalog::toString(issue.severity), issue.code, issue.reason,
                                  issue.system.empty() ? "" : fmt::format(" [{}]", issue.system)));
    }
}

Json reportJson(const LaunchLibrary& library, const Report& report) {
    Json json;
    json["library"] = Json{{"id", library.profile.id.str()},
                           {"esdeHome", pathText(library.profile.esdeHome)},
                           {"installFolder", pathJson(library.profile.installFolder)},
                           {"romRoot", rootJson(library.romRoot)},
                           {"alternativeEmulatorPerGame", library.settings.alternativeEmulatorPerGame}};
    Json profiles = Json::array();
    for (const ProfileReport& entry : report.profiles) {
        const launcher::EmulatorProfile& profile = *entry.profile;
        Json systems = Json::array();
        for (const auto& [system, settings] : profile.systems) {
            systems.push_back(system);
        }
        profiles.push_back(
            Json{{"id", profile.id},
                 {"displayName", profile.displayName},
                 {"origin", std::string(launcher::toString(profile.origin))},
                 {"source", profile.source},
                 {"executable",
                  entry.executable ? Json{{"path", pathText(entry.executable->path)},
                                          {"rule", std::string(launcher::toString(entry.executable->rule))},
                                          {"entry", entry.executable->entry}}
                                   : Json(nullptr)},
                 {"notFound", entry.executable ? Json(nullptr) : Json(entry.executable.error().message)},
                 {"systems", std::move(systems)},
                 {"verified", profile.verified}});
    }
    json["profiles"] = std::move(profiles);
    Json programs = Json::array();
    for (const launcher::ProgramProfile& program : library.profiles.programs) {
        const launcher::VrSettings& vr = program.vr;
        programs.push_back(Json{
            {"id", program.id},
            {"displayName", program.displayName},
            {"origin", std::string(launcher::toString(program.origin))},
            {"source", program.source},
            {"files", program.files},
            {"vr",
             Json{{"addArguments", vr.addArguments},
                  {"dropArguments", vr.dropArguments},
                  {"appendConfig", vr.appendConfig ? Json{{"option", vr.appendConfig->option},
                                                          {"file", vr.appendConfig->fileName},
                                                          {"lines", vr.appendConfig->lines}}
                                                   : Json(nullptr)},
                  {"inGame", std::string(launcher::toString(vr.inGame))},
                  {"heavyWith", vr.heavyWith}}},
            {"verified", program.verified}});
    }
    json["programs"] = std::move(programs);
    Json choices = Json::object();
    for (const auto& [system, choice] : library.profiles.systemChoices) {
        choices[system] = choiceJson(choice);
    }
    json["systemChoices"] = std::move(choices);
    Json assigned = Json::object();
    for (const auto& [system, choice] : library.assignedChoices) {
        assigned[system] = choiceJson(choice);
    }
    json["assignedChoices"] = std::move(assigned);
    Json systems = Json::array();
    for (const SystemReport& system : report.systems) {
        Json entry = translationJson(system.resolution.translation);
        entry["name"] = system.system.name.str();
        entry["fullName"] = system.system.fullName;
        entry["romFolder"] = system.system.romFolder.empty() ? Json(nullptr) : Json(pathText(system.system.romFolder));
        entry["availability"] = std::string(catalog::toString(system.system.availability));
        entry["alternativeEmulator"] =
            system.alternativeEmulator.empty() ? Json(nullptr) : Json(system.alternativeEmulator);
        entry["systemChoice"] = system.systemChoice ? choiceJson(*system.systemChoice) : Json(nullptr);
        entry["chosen"] = chosenJson(system.resolution.chosen, systemChoiceOrigin(library, system.system.name.str()));
        const launcher::ProgramProfile* program = programOf(library, system.resolution.translation);
        entry["programProfile"] = program != nullptr ? Json(program->id) : Json(nullptr);
        Json commands = Json::array();
        for (const library::LaunchOption& command : system.commands) {
            Json item = translationJson(command.translation);
            item["label"] = command.choice.value;
            item["command"] = command.text;
            commands.push_back(std::move(item));
        }
        entry["commands"] = std::move(commands);
        Json serving = Json::array();
        for (const library::LaunchOption& profile : system.profiles) {
            Json item = translationJson(profile.translation);
            item["id"] = profile.choice.value;
            serving.push_back(std::move(item));
        }
        entry["profiles"] = std::move(serving);
        systems.push_back(std::move(entry));
    }
    json["systems"] = std::move(systems);
    Json issues = Json::array();
    for (const catalog::Issue& issue : library.issues) {
        issues.push_back(issueJson(issue));
    }
    json["issues"] = std::move(issues);
    return json;
}

} // namespace

ExitCode runProfiles(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Lists the emulator profiles and programs and, for each system of the library, the command "
                           "or profile its games launch with, why, its readiness and the program it starts. Starts "
                           "nothing, writes nothing.");
    parser.option("system", "name", "Only this system (an ES-DE system name).")
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
    const std::optional<std::string> only = args->has("system") ? args->value("system") : std::nullopt;

    ExitCode failure = ExitCode::Ok;
    const auto library = loadLaunchLibrary(context, kName, failure);
    if (!library) {
        return failure;
    }
    if (only && library->findSystem(*only) == nullptr) {
        context.err.writeLine(fmt::format("{}: the library configures no system named \"{}\"", kName, *only));
        return ExitCode::UsageError;
    }
    loadAssignedChoices(*library, context.paths);

    Report report;
    const launcher::EnvironmentLookup environment = launcher::processEnvironmentLookup();
    for (const launcher::EmulatorProfile& profile : library->profiles.profiles) {
        report.profiles.push_back({&profile, launcher::locateExecutable(profile.executable, library->probe,
                                                                        environment)});
    }
    report.systems = library::launchTable(*library, only ? std::optional<std::string_view>(*only) : std::nullopt);
    spdlog::info("profiles: {} profile(s), {} system(s)", report.profiles.size(), report.systems.size());

    Output& out = *output ? **output : context.out;
    if (args->has("json")) {
        out.write(reportJson(*library, report).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    } else {
        writeText(*library, report, out);
    }
    if (!finishOutput(context, *output, kName)) {
        return ExitCode::RuntimeFailure;
    }
    if (only && report.systems.front().resolution.readiness() != catalog::LaunchReadiness::Ready) {
        return ExitCode::ProblemsFound;
    }
    return ExitCode::Ok;
}

} // namespace rv::cli
