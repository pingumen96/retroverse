// retroverse-cli profiles (I2 step 10): the emulator profiles Retroverse knows and, for each system of the library
// library.json records, how its games launch (ADR-0008 §8): the command or profile chosen and why (Retroverse's choice
// for the system in emulators.json, the gamelist's <alternativeEmulator>, the first command), its readiness without a
// game, the find rule and entry that found the program and the core, the blockers; then every command of the system
// and every profile serving it with their own readiness, so that a system that cannot launch shows what could. What
// depends on each game (its file, its <altemulator>, an %INJECT% file) is the launch's: `launch --dry-run`. Nothing is
// started or written. Exit codes: 0; 1 with --system when that system is not ready; 2 for invalid options, an unknown
// system or no library.json; 3 when ES-DE's configuration or the results cannot be read or written.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/launcher/arg_template.hpp>
#include <rv/launcher/emulator_profile.hpp>

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

/// What a system's report holds.
struct SystemReport {
    const catalog::SystemInfo* system = nullptr;
    std::string alternativeEmulator;
    std::optional<launcher::LaunchChoice> systemChoice;
    catalog::LaunchResolution resolution;
    struct Command {
        std::string label;
        std::string text;
        catalog::LaunchTranslation translation;
    };
    std::vector<Command> commands;
    struct Profile {
        std::string id;
        catalog::LaunchTranslation translation;
    };
    std::vector<Profile> profiles;
};

struct Report {
    std::vector<ProfileReport> profiles;
    std::vector<SystemReport> systems;
};

SystemReport reportSystem(LaunchLibrary& library, const catalog::SystemInfo& system) {
    SystemReport report;
    report.system = &system;
    if (const auto gamelist = readSystemGamelist(library, system)) {
        report.alternativeEmulator = gamelist->alternativeEmulator;
    }
    const catalog::LaunchChoices choices = systemChoices(library, system, report.alternativeEmulator);
    report.systemChoice = choices.system;
    const catalog::LaunchContext context = library.context();
    report.resolution = catalog::resolveLaunch(system, nullptr, choices, context);
    if (const esde::SystemDefinition* definition = library.systems.find(system.name.str())) {
        for (const esde::SystemCommand& command : definition->commands) {
            report.commands.push_back(
                {command.label, command.text, library.translator->translate(system.name, command.label, nullptr)});
        }
    }
    for (const launcher::EmulatorProfile* profile : library.profiles.serving(system.name.str())) {
        report.profiles.push_back({profile->id, catalog::translateProfile(system, nullptr, *profile, context)});
    }
    return report;
}

std::string labelText(const std::string& label) {
    return label.empty() ? std::string("(unlabelled)") : label;
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
    if (!library.profiles.systemChoices.empty()) {
        line("System choices", fmt::format("{} (emulators.json):", library.profiles.systemChoices.size()));
        for (const auto& [system, choice] : library.profiles.systemChoices) {
            out.writeLine(fmt::format("  {:<16}{}", system, choiceText(choice)));
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
    line("Systems", fmt::format("{}{}{}", report.systems.size(), summary.empty() ? "" : " (" + summary + ")",
                                report.systems.empty() ? "" : ":"));
    for (const SystemReport& system : report.systems) {
        const catalog::LaunchResolution& resolution = system.resolution;
        out.writeLine(fmt::format("  {} ({})", system.system->name.str(), system.system->fullName));
        out.writeLine(fmt::format("    {:<18}{}: {} ({})", "launches with", catalog::describe(resolution.readiness()),
                                  choiceText(resolution.chosen.choice), catalog::describe(resolution.chosen.source)));
        for (const std::string& warning : resolution.chosen.warnings) {
            out.writeLine(fmt::format("    {:<18}{}", "warning", warning));
        }
        writeTranslationDetails(out, resolution.translation, "    ", 18, false);
        if (!system.commands.empty()) {
            out.writeLine("    commands");
            for (const SystemReport::Command& command : system.commands) {
                const bool chosen = resolution.chosen.choice.kind == launcher::LaunchChoiceKind::EsdeCommand &&
                                    resolution.chosen.choice.value == command.label && !resolution.chosen.blocker;
                const catalog::LaunchTranslation& translation = command.translation;
                out.writeLine(fmt::format("      {} {:<34}{}{}", chosen ? '*' : ' ', labelText(command.label),
                                          catalog::describe(translation.readiness()),
                                          translation.blockers.empty()
                                              ? std::string()
                                              : ": " + translation.blockers.front().message));
            }
        }
        if (!system.profiles.empty()) {
            out.writeLine("    profiles");
            for (const SystemReport::Profile& profile : system.profiles) {
                const bool chosen = resolution.chosen.choice.kind == launcher::LaunchChoiceKind::Profile &&
                                    resolution.chosen.choice.value == profile.id;
                const catalog::LaunchTranslation& translation = profile.translation;
                out.writeLine(fmt::format("      {} {:<34}{}{}", chosen ? '*' : ' ', profile.id,
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
    Json choices = Json::object();
    for (const auto& [system, choice] : library.profiles.systemChoices) {
        choices[system] = choiceJson(choice);
    }
    json["systemChoices"] = std::move(choices);
    Json systems = Json::array();
    for (const SystemReport& system : report.systems) {
        Json entry = translationJson(system.resolution.translation);
        entry["name"] = system.system->name.str();
        entry["fullName"] = system.system->fullName;
        entry["romFolder"] = system.system->romFolder.empty() ? Json(nullptr) : Json(pathText(system.system->romFolder));
        entry["availability"] = std::string(catalog::toString(system.system->availability));
        entry["alternativeEmulator"] =
            system.alternativeEmulator.empty() ? Json(nullptr) : Json(system.alternativeEmulator);
        entry["systemChoice"] = system.systemChoice ? choiceJson(*system.systemChoice) : Json(nullptr);
        entry["chosen"] = chosenJson(system.resolution.chosen);
        Json commands = Json::array();
        for (const SystemReport::Command& command : system.commands) {
            Json item = translationJson(command.translation);
            item["label"] = command.label;
            item["command"] = command.text;
            commands.push_back(std::move(item));
        }
        entry["commands"] = std::move(commands);
        Json serving = Json::array();
        for (const SystemReport::Profile& profile : system.profiles) {
            Json item = translationJson(profile.translation);
            item["id"] = profile.id;
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
                           "Lists the emulator profiles and, for each system of the library, the command or profile "
                           "its games launch with, why, and its readiness. Starts nothing, writes nothing.");
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

    Report report;
    const launcher::EnvironmentLookup environment = launcher::processEnvironmentLookup();
    for (const launcher::EmulatorProfile& profile : library->profiles.profiles) {
        report.profiles.push_back({&profile, launcher::locateExecutable(profile.executable, library->probe,
                                                                        environment)});
    }
    for (const catalog::SystemInfo& system : library->systemInfos) {
        if (!only || system.name.str() == *only) {
            report.systems.push_back(reportSystem(*library, system));
        }
    }
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
