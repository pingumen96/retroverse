// retroverse-cli launch (I2 step 10: --dry-run only; --game with --wait and --test come in I2 step 12). A dry run
// resolves the launch of one game of the library library.json records, named by its system and its path relative to the
// system's ROM folder (game ids come with the import, I2 step 11), as ADR-0008 §8 says: the command or profile chosen and
// why (the game's <altemulator>, Retroverse's choice for the system, the gamelist's <alternativeEmulator>, the first
// command), its readiness, the program and core with the find rule and entry that found them, the blockers, and the
// request it would start: argv (the program, then each argument as the emulator receives it), the working folder, the
// environment overrides, the console window, the command line, and what the automation guard (RV_LAUNCH_ALLOWLIST)
// would say of it. The game's path is spelled as the folder lists it, as ES-DE's %ROM% spells it. Nothing is started,
// nothing is written. Exit codes: 0 ready; 1 not ready (the readiness and the blockers say why); 2 for invalid options,
// a launch without --dry-run, an unknown system, a path that is not below the system's ROM folder, or no library.json;
// 3 when ES-DE's configuration or the results cannot be read or written.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/launcher/launch_guard.hpp>

#include "commands.hpp"
#include "launch_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli launch";

/// What a dry run found.
struct DryRun {
    const catalog::SystemInfo* system = nullptr;
    /// The path as given, relative to the ROM folder ('/' between the elements).
    std::string requestedPath;
    catalog::LaunchGame game;
    std::filesystem::path romPath;
    /// The command as es_systems.xml writes it, when an ES-DE command was chosen.
    std::string commandText;
    std::string systemAlternativeEmulator;
    catalog::LaunchResolution resolution;
    std::optional<launcher::GuardVerdict> guard;
    std::optional<std::string> commandLine;
};

/// The game's availability on disk: its system's folder, then each element of its path as the folder spells it.
void findGame(const catalog::SystemInfo& system, DryRun& run) {
    run.game.relativePath = run.requestedPath;
    if (system.availability != catalog::Availability::Available) {
        run.game.availability = catalog::Availability::SourceUnavailable;
        run.game.unavailableReason = system.unavailableReason;
        return;
    }
    const auto listed = esde::listedPath(system.romFolder, run.requestedPath);
    if (listed) {
        run.game.relativePath = listed->relativePath;
        run.game.directory = listed->directory;
        return;
    }
    switch (listed.error().code) {
    case ErrorCode::NotFound:
        run.game.availability = catalog::Availability::Missing;
        break;
    case ErrorCode::Unavailable:
        run.game.availability = catalog::Availability::SourceUnavailable;
        run.game.unavailableReason = catalog::SourceUnavailableReason::Unreachable;
        break;
    default:
        run.game.availability = catalog::Availability::SourceUnavailable;
        run.game.unavailableReason = catalog::SourceUnavailableReason::NotListable;
        break;
    }
}

/// The game's own choice and title from the system's gamelist (the entry ES-DE applies to the file as listed).
void readGameEntry(LaunchLibrary& library, const catalog::SystemInfo& system, DryRun& run,
                   catalog::LaunchChoices& choices) {
    const auto gamelist = readSystemGamelist(library, system);
    run.game.title = catalog::defaultTitle({}, run.game.relativePath, run.game.directory);
    if (!gamelist) {
        return;
    }
    run.systemAlternativeEmulator = gamelist->alternativeEmulator;
    choices.systemAlternativeEmulator = gamelist->alternativeEmulator;
    const auto key = catalog::makeLookupKey(run.game.relativePath);
    if (!key) {
        return;
    }
    std::vector<const esde::GamelistEntry*> sameKey;
    for (const esde::GamelistEntry& entry : gamelist->entries) {
        if (!entry.outsideSystemFolder() && entry.key.key() == key->key()) {
            sameKey.push_back(&entry);
        }
    }
    const esde::GamelistEntry* entry = esde::entryForListed(sameKey, *key, esde::GamelistEntryKind::Game);
    if (entry != nullptr && entry->kind == esde::GamelistEntryKind::Game) {
        run.game.title = catalog::defaultTitle(entry->metadata, run.game.relativePath, run.game.directory);
        choices.gameAltEmulator = entry->altEmulator;
    }
}

Json requestJson(const launcher::LaunchRequest& request) {
    Json argv = Json::array();
    argv.push_back(pathText(request.executable));
    for (const std::string& argument : request.arguments) {
        argv.push_back(argument);
    }
    Json environment = Json::array();
    for (const launcher::EnvironmentOverride& override : request.environment) {
        environment.push_back(
            Json{{"name", override.name}, {"value", override.value ? Json(*override.value) : Json(nullptr)}});
    }
    return Json{{"argv", std::move(argv)},
                {"workingDirectory", pathText(launcher::workingDirectoryOf(request))},
                {"environment", std::move(environment)},
                {"hideConsoleWindow", request.hideConsoleWindow},
                {"waitStrategy", std::string(launcher::toString(request.waitStrategy))},
                {"label", request.label}};
}

Json dryRunJson(const DryRun& run) {
    const catalog::LaunchResolution& resolution = run.resolution;
    Json json = translationJson(resolution.translation);
    json["system"] = run.system->name.str();
    json["path"] = run.requestedPath;
    json["listedPath"] = run.game.relativePath;
    json["directory"] = run.game.directory;
    json["availability"] = std::string(catalog::toString(run.game.availability));
    json["romPath"] = pathText(run.romPath);
    json["title"] = run.game.title;
    json["alternativeEmulator"] = run.systemAlternativeEmulator.empty() ? Json(nullptr)
                                                                        : Json(run.systemAlternativeEmulator);
    json["chosen"] = chosenJson(resolution.chosen);
    json["command"] = run.commandText.empty() ? Json(nullptr) : Json(run.commandText);
    json["ready"] = resolution.readiness() == catalog::LaunchReadiness::Ready;
    json["request"] = resolution.translation.request ? requestJson(*resolution.translation.request) : Json(nullptr);
    json["commandLine"] = run.commandLine ? Json(*run.commandLine) : Json(nullptr);
    json["guard"] = run.guard ? Json{{"rule", std::string(launcher::toString(run.guard->rule))},
                                     {"allowed", run.guard->allowed()},
                                     {"message", run.guard->message}}
                              : Json(nullptr);
    json["started"] = false;
    return json;
}

void writeText(const DryRun& run, Output& out) {
    const catalog::LaunchResolution& resolution = run.resolution;
    const auto line = [&](std::string_view label, const std::string& text) {
        out.writeLine(fmt::format("{:<20}{}", label, text));
    };
    line("Game", fmt::format("{} \"{}\" ({}{})", run.system->name.str(), run.game.relativePath,
                             run.game.directory ? "folder, " : "", catalog::toString(run.game.availability)));
    line("Title", run.game.title);
    line("ROM", pathText(run.romPath));
    line("Launches with", fmt::format("{} ({})", choiceText(resolution.chosen.choice),
                                      catalog::describe(resolution.chosen.source)));
    if (!run.commandText.empty()) {
        line("Command", run.commandText);
    }
    for (const std::string& warning : resolution.chosen.warnings) {
        line("Warning", warning);
    }
    line("Readiness", std::string(catalog::describe(resolution.readiness())));
    writeTranslationDetails(out, resolution.translation, "", 20, true);
    if (const auto& request = resolution.translation.request) {
        out.writeLine(resolution.readiness() == catalog::LaunchReadiness::Ready ? "Argv:"
                                                                                : "Argv (blocked, not runnable):");
        out.writeLine(fmt::format("  [0] {}", pathText(request->executable)));
        for (std::size_t i = 0; i < request->arguments.size(); ++i) {
            out.writeLine(fmt::format("  [{}] {}", i + 1, request->arguments[i]));
        }
        line("Working folder", pathText(launcher::workingDirectoryOf(*request)));
        if (request->environment.empty()) {
            line("Environment", "inherited, unchanged");
        } else {
            for (const launcher::EnvironmentOverride& override : request->environment) {
                line("Environment", override.value ? fmt::format("{}={}", override.name, *override.value)
                                                   : fmt::format("{} removed", override.name));
            }
        }
        line("Console window", request->hideConsoleWindow ? "hidden (%HIDEWINDOW%)" : "shown");
        if (run.commandLine) {
            line("Command line", *run.commandLine);
        }
    }
    if (run.guard) {
        switch (run.guard->rule) {
        case launcher::GuardRule::Inactive:
            line("Automation guard", "inactive (RV_LAUNCH_ALLOWLIST is not set)");
            break;
        case launcher::GuardRule::Allowed:
            line("Automation guard", "allowed (RV_LAUNCH_ALLOWLIST)");
            break;
        default:
            line("Automation guard", fmt::format("would refuse it ({}): {}", launcher::toString(run.guard->rule),
                                                 run.guard->message));
            break;
        }
    }
    out.writeLine("Dry run: nothing was started.");
}

} // namespace

ExitCode runLaunch(CommandContext& context) {
    core::ArgParser parser(std::string(kName),
                           "Launches a game in its emulator. --dry-run resolves the launch of the game at --path in the "
                           "ROM folder of --system and prints what would run, without starting anything.");
    parser.flag("dry-run", "Resolve the launch and print it; start nothing.")
        .option("system", "name", "The game's system (an ES-DE system name).")
        .option("path", "path", "The game's path relative to the system's ROM folder.")
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
        context.err.writeLine(fmt::format("{}: not implemented yet (I2 step 12); --dry-run shows what would run",
                                          kName));
        return ExitCode::UsageError;
    }
    auto output = outputFileOption(*args);
    if (!output) {
        context.err.writeLine(fmt::format("{}: {}", kName, output.error().toString()));
        return ExitCode::UsageError;
    }
    const std::string systemName = args->value("system").value_or("");
    const std::string pathArgument = args->value("path").value_or("");
    if (systemName.empty() || pathArgument.empty()) {
        context.err.writeLine(fmt::format("{}: --dry-run needs --system <name> and --path <relative path>", kName));
        return ExitCode::UsageError;
    }

    ExitCode failure = ExitCode::Ok;
    const auto library = loadLaunchLibrary(context, kName, failure);
    if (!library) {
        return failure;
    }
    DryRun run;
    run.system = library->findSystem(systemName);
    if (run.system == nullptr) {
        context.err.writeLine(fmt::format("{}: the library configures no system named \"{}\"", kName, systemName));
        return ExitCode::UsageError;
    }
    const catalog::SystemInfo& system = *run.system;
    if (system.romFolder.empty()) {
        context.err.writeLine(
            fmt::format("{}: the ROM folder of {} cannot be resolved (see the issues of 'retroverse-cli discover')",
                        kName, systemName));
        return ExitCode::UsageError;
    }
    const auto relative = core::resolveRelativePath(pathArgument, system.romFolder);
    if (!relative) {
        context.err.writeLine(fmt::format("{}: --path: {}", kName, relative.error().toString()));
        return ExitCode::UsageError;
    }
    run.requestedPath = *relative;
    findGame(system, run);
    catalog::LaunchChoices choices = systemChoices(*library, system, {});
    readGameEntry(*library, system, run, choices);
    run.romPath = (system.romFolder / core::pathFromUtf8(run.game.relativePath)).make_preferred();
    run.resolution = catalog::resolveLaunch(system, &run.game, choices, library->context());
    if (run.resolution.chosen.choice.kind == launcher::LaunchChoiceKind::EsdeCommand && !run.resolution.chosen.blocker) {
        if (const esde::SystemDefinition* definition = library->systems.find(systemName)) {
            if (const esde::SystemCommand* command = definition->findCommand(run.resolution.chosen.choice.value)) {
                run.commandText = command->text;
            }
        }
    }
    if (const auto& request = run.resolution.translation.request) {
        // What an automated run (RV_LAUNCH_ALLOWLIST) would say of it; nothing is started either way.
        run.guard = launcher::LaunchGuard::fromEnvironment().check(*request);
        if (auto commandLine = launcher::commandLineOf(*request)) {
            run.commandLine = std::move(*commandLine);
        }
    }
    const catalog::LaunchReadiness readiness = run.resolution.readiness();
    spdlog::info("launch --dry-run: {}: {}", systemName, catalog::toString(readiness));
    if (run.commandLine) {
        spdlog::debug("launch --dry-run: command line {}", *run.commandLine);
    }

    Output& out = *output ? **output : context.out;
    if (args->has("json")) {
        out.write(dryRunJson(run).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    } else {
        writeText(run, out);
    }
    if (!finishOutput(context, *output, kName)) {
        return ExitCode::RuntimeFailure;
    }
    return readiness == catalog::LaunchReadiness::Ready ? ExitCode::Ok : ExitCode::ProblemsFound;
}

} // namespace rv::cli
