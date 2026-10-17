// retroverse-cli assign (I2 step 12): Retroverse's choice of how a system's games, or one game, launch (ADR-0008 §6,
// §8): one of the system's ES-DE commands, by its label, or an emulator profile that serves the system, by its id. The
// choice is stored in the catalog database (rv_launch_system_choices, rv_launch_game_choices: state tables, kept by `db
// rebuild-index` and across restarts), never written into the user's files, and translated again at every launch, so a
// label or profile that disappears later makes the launch "requires launcher configuration", naming it.
//
//   assign <system> [<label|profile>] [--command <label> | --profile <id> | --clear]
//   assign --game <id> [<label|profile>] [--command <label> | --profile <id> | --clear]
//
// A value is checked against what exists: an ES-DE command label of the system (compared exactly, as ES-DE compares
// labels) or the id of a profile serving it (emulators.json, the shipped ones and the user's); a value that is both is
// refused until --command or --profile says which. Without a value the command shows the choice stored; --clear removes
// it. Order (launch_resolution.hpp): the game's choice, the game's <altemulator>, the system's choice (assign's, before
// emulators.json's "systemChoices" for the same system: the user's latest explicit choice), the gamelist's
// <alternativeEmulator>, the first command. Exit codes: 0; 1 a --game the catalog does not have; 2 invalid options, an
// unknown system, a value the system does not have, a --game of another library or that is no game; 3 when ES-DE's
// configuration or the catalog cannot be read or written (no catalog yet: an import makes it).

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "launch_text.hpp"
#include "session_text.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli assign";

enum class Action : std::uint8_t {
    Show,
    Set,
    Clear,
};

std::string_view toString(Action action) noexcept {
    switch (action) {
    case Action::Show:
        return "show";
    case Action::Set:
        return "set";
    case Action::Clear:
        return "clear";
    }
    return "unknown";
}

/// What the command did.
struct Report {
    Action action = Action::Show;
    const catalog::SystemInfo* system = nullptr;
    std::optional<CatalogGame> game;
    std::optional<catalog_db::StoredChoice> previous;
    std::optional<catalog_db::StoredChoice> current;
    bool changed = false;
    /// emulators.json's choice for the system, which assign's ranks before.
    std::optional<launcher::LaunchChoice> emulatorsJsonChoice;
    std::vector<std::string> labels;
    std::vector<std::string> profiles;
    /// The readiness of the current choice for the system (what does not depend on a game), when there is one.
    std::optional<catalog::LaunchTranslation> translation;
};

/// "\"Fake Emulator\", \"Home Emulator\"" or "none".
std::string listText(const std::vector<std::string>& values, bool quoted) {
    if (values.empty()) {
        return "none";
    }
    std::string text;
    for (const std::string& value : values) {
        text += fmt::format("{}{}", text.empty() ? "" : ", ", quoted ? fmt::format("\"{}\"", value) : value);
    }
    return text;
}

/// The choice a value names among the system's labels and the profiles serving it. On failure the message goes to
/// stderr (exit 2).
std::optional<launcher::LaunchChoice> chooseValue(CommandContext& context, const Report& report,
                                                  const LaunchLibrary& library, const std::optional<std::string>& value,
                                                  const std::optional<std::string>& label,
                                                  const std::optional<std::string>& profile) {
    const std::string& system = report.system->name.str();
    const bool hasLabel = [&](const std::string& text) {
        return !text.empty() && std::find(report.labels.begin(), report.labels.end(), text) != report.labels.end();
    }(label ? *label : value.value_or(""));
    const bool hasProfile = [&](const std::string& text) {
        return std::find(report.profiles.begin(), report.profiles.end(), text) != report.profiles.end();
    }(profile ? *profile : value.value_or(""));
    const std::string options = fmt::format("its commands: {}; the profiles serving it: {}",
                                            listText(report.labels, true), listText(report.profiles, false));
    if (label) {
        if (!hasLabel) {
            context.err.writeLine(fmt::format("{}: the system {} has no command labelled \"{}\" ({})", kName, system,
                                              *label, options));
            return std::nullopt;
        }
        return launcher::LaunchChoice{launcher::LaunchChoiceKind::EsdeCommand, *label};
    }
    if (profile) {
        if (!hasProfile) {
            context.err.writeLine(fmt::format(
                "{}: {} ({})", kName,
                library.profiles.find(*profile) != nullptr
                    ? fmt::format("the profile '{}' does not serve the system {} (its \"systems\")", *profile, system)
                    : fmt::format("there is no emulator profile '{}' (emulators.json)", *profile),
                options));
            return std::nullopt;
        }
        return launcher::LaunchChoice{launcher::LaunchChoiceKind::Profile, *profile};
    }
    if (hasLabel && hasProfile) {
        context.err.writeLine(fmt::format("{}: \"{}\" is both a command of {} and a profile serving it: say which with "
                                          "--command or --profile",
                                          kName, *value, system));
        return std::nullopt;
    }
    if (hasLabel) {
        return launcher::LaunchChoice{launcher::LaunchChoiceKind::EsdeCommand, *value};
    }
    if (hasProfile) {
        return launcher::LaunchChoice{launcher::LaunchChoiceKind::Profile, *value};
    }
    context.err.writeLine(fmt::format("{}: \"{}\" is neither a command of {} nor a profile serving it ({})", kName,
                                      *value, system, options));
    return std::nullopt;
}

Json choiceOrNull(const std::optional<launcher::LaunchChoice>& choice) {
    return choice ? choiceJson(*choice) : Json(nullptr);
}

Json reportJson(const LaunchLibrary& library, const Report& report) {
    Json json;
    json["library"] = library.profile.id.str();
    json["system"] = report.system->name.str();
    if (report.game) {
        const catalog::CatalogEntry& entry = report.game->details.indexed.entry;
        json["game"] = Json{{"id", report.game->id.toString()},
                            {"requested", report.game->requested.toString()},
                            {"title", entry.title},
                            {"path", entry.key.relativePath()}};
    } else {
        json["game"] = nullptr;
    }
    json["action"] = std::string(toString(report.action));
    json["choice"] = storedChoiceJson(report.current);
    json["previous"] = storedChoiceJson(report.previous);
    json["changed"] = report.changed;
    json["emulatorsJsonChoice"] = report.game ? Json(nullptr) : choiceOrNull(report.emulatorsJsonChoice);
    json["readiness"] = report.translation ? Json(std::string(catalog::toString(report.translation->readiness())))
                                           : Json(nullptr);
    json["blockers"] = report.translation ? blockersJson(report.translation->blockers) : Json::array();
    json["commands"] = report.labels;
    json["profiles"] = report.profiles;
    return json;
}

void writeText(const Report& report, Output& out) {
    const auto line = [&](std::string_view label, const std::string& text) {
        out.writeLine(fmt::format("{:<20}{}", label, text));
    };
    const std::string target = report.game ? fmt::format("game {} ({} \"{}\")", report.game->id.toString(),
                                                         report.system->name.str(),
                                                         report.game->details.indexed.entry.key.relativePath())
                                           : fmt::format("system {}", report.system->name.str());
    line("For", target);
    const auto stored = [](const std::optional<catalog_db::StoredChoice>& choice) {
        return choice ? fmt::format("{} (chosen {})", choiceText(choice->choice),
                                    core::formatUtcIso8601(choice->chosenAt))
                      : std::string("none");
    };
    switch (report.action) {
    case Action::Show:
        line("Choice", stored(report.current));
        break;
    case Action::Set:
        line("Choice", stored(report.current));
        if (report.previous && report.changed) {
            line("Replaces", choiceText(report.previous->choice));
        } else if (!report.changed) {
            line("Unchanged", "it was the choice already");
        }
        break;
    case Action::Clear:
        line("Cleared", report.previous ? choiceText(report.previous->choice) : std::string("there was no choice"));
        break;
    }
    if (!report.game) {
        if (report.emulatorsJsonChoice) {
            line("emulators.json", fmt::format("{}{}", choiceText(*report.emulatorsJsonChoice),
                                               report.current ? " (ranks after the choice above)" : " (in effect)"));
        }
    } else {
        line("Ranks", "first: before the game's <altemulator> and the system's choices");
    }
    if (report.translation) {
        line("Readiness", fmt::format("{} for the system (each game's own file is checked at its launch)",
                                      catalog::describe(report.translation->readiness())));
        for (const catalog::ReadinessBlocker& blocker : report.translation->blockers) {
            line("Blocked", fmt::format("{} ({}): {}", catalog::describe(blocker.readiness), blocker.code,
                                        blocker.message));
        }
    }
    line("Commands", listText(report.labels, true));
    line("Profiles", listText(report.profiles, false));
    out.writeLine("Stored in the catalog database; the user's files are never written.");
}

} // namespace

ExitCode runAssign(CommandContext& context) {
    core::ArgParser parser(
        std::string(kName),
        "Chooses how a system's games, or one game (--game), launch: one of the system's ES-DE commands by its label, or "
        "an emulator profile serving it by its id. Stored in the catalog database, never in the user's files; it ranks "
        "before emulators.json's choice for the system. Without a value, shows the choice; --clear removes it.");
    parser.positional("target", "The system (an ES-DE system name); with --game, the value.")
        .positional("value", "An ES-DE command label of the system, or the id of a profile serving it.")
        .option("game", "id", "Choose for this game of the catalog (its GameId) instead of its whole system.")
        .option("command", "label", "The value is this ES-DE command label.")
        .option("profile", "id", "The value is this emulator profile id.")
        .flag("clear", "Remove the choice.")
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
    const auto usage = [&](std::string_view message) { return usageError(context, message); };
    auto output = outputFileOption(*args);
    if (!output) {
        return usage(output.error().toString());
    }
    const std::optional<std::string> first = args->positional("target");
    const std::optional<std::string> second = args->positional("value");
    std::optional<catalog::GameId> gameId;
    std::optional<std::string> systemName;
    std::optional<std::string> value;
    if (args->has("game")) {
        const auto id = catalog::GameId::parse(args->value("game").value_or(""));
        if (!id) {
            return usage(fmt::format("--game: {}", id.error().toString()));
        }
        gameId = *id;
        if (second) {
            return usage("with --game, give one value: assign --game <id> <label|profile>");
        }
        value = first;
    } else {
        if (!first || first->empty()) {
            return usage("name a system (assign <system> <label|profile>) or a game (assign --game <id> ...)");
        }
        systemName = first;
        value = second;
    }
    const std::optional<std::string> label = args->has("command") ? args->value("command") : std::nullopt;
    const std::optional<std::string> profile = args->has("profile") ? args->value("profile") : std::nullopt;
    const bool clear = args->has("clear");
    if ((value ? 1 : 0) + (label ? 1 : 0) + (profile ? 1 : 0) + (clear ? 1 : 0) > 1) {
        return usage("give one of a value, --command, --profile or --clear");
    }
    if ((value && value->empty()) || (label && label->empty()) || (profile && profile->empty())) {
        return usage("an empty value names no command label or profile");
    }

    ExitCode failure = ExitCode::Ok;
    const auto library = loadLaunchLibrary(context, kName, failure);
    if (!library) {
        return failure;
    }
    auto catalog = openLaunchCatalog(context.paths);
    if (!catalog) {
        return catalogFailure(context, catalog.error());
    }
    catalog_db::SessionStore& store = *catalog->store;

    Report report;
    report.action = clear ? Action::Clear : (value || label || profile) ? Action::Set : Action::Show;
    if (gameId) {
        report.game = readCatalogGame(context, *catalog->query, store, *gameId, library->profile.id, failure);
        if (!report.game) {
            return failure;
        }
        systemName = report.game->details.indexed.entry.system.str();
    }
    report.system = library->findSystem(*systemName);
    if (report.system == nullptr) {
        context.err.writeLine(fmt::format("{}: the library configures no system named \"{}\"{}", kName, *systemName,
                                          report.game ? " (the game's system)" : ""));
        return ExitCode::UsageError;
    }
    const catalog::SystemInfo& system = *report.system;
    report.labels = library->translator->commandLabels(system.name).value_or(std::vector<std::string>{});
    for (const launcher::EmulatorProfile* serving : library->profiles.serving(system.name.str())) {
        report.profiles.push_back(serving->id);
    }
    if (const launcher::LaunchChoice* choice = library->profiles.systemChoice(system.name.str())) {
        report.emulatorsJsonChoice = *choice;
    }

    const auto readStored = [&]() -> Expected<std::optional<catalog_db::StoredChoice>> {
        return report.game ? store.gameChoice(report.game->id) : store.systemChoice(library->profile.id, system.name);
    };
    auto previous = readStored();
    if (!previous) {
        return catalogFailure(context, previous.error());
    }
    report.previous = *previous;
    report.current = report.previous;
    const auto now = std::chrono::system_clock::now();
    if (report.action == Action::Set) {
        const auto choice = chooseValue(context, report, *library, value, label, profile);
        if (!choice) {
            return ExitCode::UsageError;
        }
        report.changed = !report.previous || report.previous->choice != *choice;
        if (report.changed) {
            const Status stored = report.game ? store.setGameChoice(report.game->id, *choice, now,
                                                                    catalog_db::kPatientRetry)
                                              : store.setSystemChoice(library->profile.id, system.name, *choice, now,
                                                                      catalog_db::kPatientRetry);
            if (!stored) {
                context.err.writeLine(fmt::format("{}: the choice cannot be stored: {}", kName,
                                                  stored.error().toString()));
                if (stored.error().code == ErrorCode::InvalidArgument) {
                    context.err.writeLine("Run 'retroverse-cli import' first: the catalog records the library then.");
                }
                return ExitCode::RuntimeFailure;
            }
            report.current = catalog_db::StoredChoice{*choice, now};
            spdlog::info("assign: {} now launches with {}", report.game ? report.game->id.toString() : system.name.str(),
                         choiceText(*choice));
        }
    } else if (report.action == Action::Clear) {
        auto cleared = report.game ? store.clearGameChoice(report.game->id, catalog_db::kPatientRetry)
                                   : store.clearSystemChoice(library->profile.id, system.name, catalog_db::kPatientRetry);
        if (!cleared) {
            context.err.writeLine(fmt::format("{}: the choice cannot be removed: {}", kName,
                                              cleared.error().toString()));
            return ExitCode::RuntimeFailure;
        }
        report.changed = *cleared;
        report.current.reset();
        if (*cleared) {
            spdlog::info("assign: the choice for {} removed",
                         report.game ? report.game->id.toString() : system.name.str());
        }
    }
    // The current choice's readiness for the system: what does not depend on a game.
    if (report.current) {
        const launcher::LaunchChoice& choice = report.current->choice;
        if (choice.kind == launcher::LaunchChoiceKind::EsdeCommand) {
            report.translation = library->translator->translate(system.name, choice.value, nullptr);
        } else if (const launcher::EmulatorProfile* chosen = library->profiles.find(choice.value)) {
            report.translation = catalog::translateProfile(system, nullptr, *chosen, library->context());
        } else {
            catalog::LaunchTranslation unknown;
            unknown.blockers.push_back(catalog::ReadinessBlocker{
                catalog::LaunchReadiness::RequiresConfiguration, "profile-unknown",
                fmt::format("the emulator profile '{}' does not exist (emulators.json)", choice.value)});
            report.translation = std::move(unknown);
        }
    }

    Output& out = *output ? **output : context.out;
    if (args->has("json")) {
        out.write(jsonText(reportJson(*library, report)));
    } else {
        writeText(report, out);
    }
    if (!finishOutput(context, *output, kName)) {
        return ExitCode::RuntimeFailure;
    }
    return ExitCode::Ok;
}

} // namespace rv::cli
