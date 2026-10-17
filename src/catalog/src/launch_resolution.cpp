#include <rv/catalog/launch_resolution.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/launch_plan.hpp>

namespace rv::catalog {
namespace {

ReadinessBlocker blockerOf(LaunchReadiness readiness, std::string code, std::string message) {
    return ReadinessBlocker{readiness, std::move(code), std::move(message)};
}

/// A launcher plan's blocker as the catalog's readiness (launch_plan.hpp keeps the order this maps to).
ReadinessBlocker blockerOfPlan(const launcher::LaunchBlocker& blocker, std::string_view profileId) {
    LaunchReadiness readiness = LaunchReadiness::RequiresConfiguration;
    std::string_view code = "profile-invalid-request";
    switch (blocker.kind) {
    case launcher::BlockerKind::RomMissing:
        // Only a ROM path that is not absolute: the catalog checks the ROM's presence itself.
        readiness = LaunchReadiness::SourceUnavailable;
        code = "rom-folder-unresolved";
        break;
    case launcher::BlockerKind::TemplateError:
        code = "profile-template";
        break;
    case launcher::BlockerKind::VariableMissing:
        code = "profile-variable-missing";
        break;
    case launcher::BlockerKind::ExecutableNotFound:
        readiness = LaunchReadiness::EmulatorNotFound;
        code = "emulator-not-found";
        break;
    case launcher::BlockerKind::ProgramRefused:
        code = "program-refused";
        break;
    case launcher::BlockerKind::InvalidRequest:
        code = "invalid-request";
        break;
    case launcher::BlockerKind::RequiredFileMissing:
        readiness = LaunchReadiness::CoreNotFound;
        code = "core-not-found";
        break;
    }
    return blockerOf(readiness, std::string(code), fmt::format("profile '{}': {}", profileId, blocker.message));
}

/// Why a folder is unavailable, for people.
std::string_view reasonText(SourceUnavailableReason reason) {
    switch (reason) {
    case SourceUnavailableReason::FolderMissing:
        return "it does not exist";
    case SourceUnavailableReason::Unreachable:
        return "its drive or share cannot be reached";
    case SourceUnavailableReason::NotListable:
        return "it cannot be listed";
    case SourceUnavailableReason::None:
        break;
    }
    return "it cannot be read";
}

} // namespace

std::string_view describe(LaunchReadiness readiness) noexcept {
    switch (readiness) {
    case LaunchReadiness::SourceUnavailable:
        return "source unavailable";
    case LaunchReadiness::RomMissing:
        return "ROM missing";
    case LaunchReadiness::RequiresConfiguration:
        return "requires launcher configuration";
    case LaunchReadiness::EmulatorNotFound:
        return "emulator not found";
    case LaunchReadiness::CoreNotFound:
        return "core not found";
    case LaunchReadiness::Ready:
        return "ready";
    }
    return "unknown";
}

std::string_view describe(ChoiceSource source) noexcept {
    switch (source) {
    case ChoiceSource::RetroverseGame:
        return "Retroverse's choice for the game";
    case ChoiceSource::EsdeGame:
        return "the game's <altemulator>";
    case ChoiceSource::RetroverseSystem:
        return "Retroverse's choice for the system";
    case ChoiceSource::EsdeSystem:
        return "the gamelist's <alternativeEmulator>";
    case ChoiceSource::FirstCommand:
        return "the system's first command";
    }
    return "unknown";
}

LaunchReadiness readinessOf(std::span<const ReadinessBlocker> blockers) noexcept {
    LaunchReadiness first = LaunchReadiness::Ready;
    for (const ReadinessBlocker& blocker : blockers) {
        first = std::min(first, blocker.readiness);
    }
    return first;
}

ChosenLaunch chooseLaunch(const LaunchChoices& choices, std::span<const std::string> labels,
                          const launcher::EmulatorProfiles& profiles) {
    const auto hasLabel = [&](std::string_view label) {
        return std::find(labels.begin(), labels.end(), label) != labels.end();
    };
    std::vector<std::string> warnings;
    // Retroverse's stored choices are honoured or blocked, never skipped: falling back to another command would change
    // the emulator behind the user's back (a missing core where the chosen one was present, say).
    const auto stored = [&](const launcher::LaunchChoice& choice, ChoiceSource source) {
        ChosenLaunch chosen{source, choice, std::move(warnings), std::nullopt};
        if (choice.kind == launcher::LaunchChoiceKind::EsdeCommand && !hasLabel(choice.value)) {
            chosen.blocker = blockerOf(
                LaunchReadiness::RequiresConfiguration, "choice-stale",
                fmt::format("{} names the command \"{}\", which the system no longer has: choose another one",
                            describe(source), choice.value));
        } else if (choice.kind == launcher::LaunchChoiceKind::Profile && profiles.find(choice.value) == nullptr) {
            chosen.blocker = blockerOf(
                LaunchReadiness::RequiresConfiguration, "profile-unknown",
                fmt::format("{} names the emulator profile '{}', which does not exist (emulators.json): choose "
                            "another one",
                            describe(source), choice.value));
        }
        return chosen;
    };
    const auto esde = [&](std::string label, ChoiceSource source) {
        return ChosenLaunch{source, {launcher::LaunchChoiceKind::EsdeCommand, std::move(label)}, std::move(warnings),
                            std::nullopt};
    };

    if (choices.game) {
        return stored(*choices.game, ChoiceSource::RetroverseGame);
    }
    if (!choices.gameAltEmulator.empty()) {
        if (!choices.alternativeEmulatorPerGame) {
            warnings.push_back(fmt::format("the game's <altemulator> \"{}\" is not used: ES-DE's "
                                           "AlternativeEmulatorPerGame is off",
                                           choices.gameAltEmulator));
        } else if (hasLabel(choices.gameAltEmulator)) {
            return esde(choices.gameAltEmulator, ChoiceSource::EsdeGame);
        } else {
            // ES-DE logs that the game's alternative emulator is invalid and goes on to the system's.
            warnings.push_back(fmt::format("the game's <altemulator> \"{}\" names no command of the system: ES-DE "
                                           "ignores it as invalid",
                                           choices.gameAltEmulator));
        }
    }
    if (choices.system) {
        return stored(*choices.system, ChoiceSource::RetroverseSystem);
    }
    if (!choices.systemAlternativeEmulator.empty()) {
        if (hasLabel(choices.systemAlternativeEmulator)) {
            return esde(choices.systemAlternativeEmulator, ChoiceSource::EsdeSystem);
        }
        // ES-DE marks the label <INVALID> in its alternative emulators' screen, warns at startup and launches the
        // system's default command.
        warnings.push_back(fmt::format("the gamelist's <alternativeEmulator> \"{}\" names no command of the system: "
                                       "ES-DE marks it invalid and launches with the first command",
                                       choices.systemAlternativeEmulator));
    }
    if (labels.empty()) {
        ChosenLaunch none = esde({}, ChoiceSource::FirstCommand);
        none.blocker = blockerOf(LaunchReadiness::RequiresConfiguration, "no-command", "the system has no command");
        return none;
    }
    return esde(labels.front(), ChoiceSource::FirstCommand);
}

std::optional<launcher::LaunchRequest> LaunchResolution::request() const {
    if (!translation.blockers.empty()) {
        return std::nullopt;
    }
    return translation.request;
}

LaunchTranslation translateProfile(const SystemInfo& system, const LaunchGame* game,
                                   const launcher::EmulatorProfile& profile, const LaunchContext& context) {
    LaunchTranslation translation;
    if (system.romFolder.empty()) {
        translation.blockers.push_back(blockerOf(LaunchReadiness::SourceUnavailable, "rom-folder-unresolved",
                                                 fmt::format("the ROM folder of {} is not known", system.name.str())));
        return translation;
    }
    launcher::LaunchTarget target;
    target.system = system.name.str();
    if (game != nullptr) {
        auto relative = core::tryPathFromUtf8(game->relativePath);
        if (!relative || game->relativePath.empty()) {
            translation.blockers.push_back(blockerOf(LaunchReadiness::RomMissing, "rom-missing",
                                                     "the game's path is not valid UTF-8 text"));
            return translation;
        }
        target.rom = (system.romFolder / *relative).make_preferred();
        target.title = game->title;
    } else {
        // A system's readiness: a stand-in ROM path in the system's folder, never looked at (checkRom off).
        target.rom = system.romFolder / "game";
    }
    launcher::PlanOptions options;
    options.checkRom = false; // the game's availability says whether it is there (catalog::Availability)
    options.environment = context.environment;
    const launcher::LaunchPlan plan = launcher::planLaunch(target, profile, *context.probe, options);
    if (plan.executable) {
        translation.program = FoundLocation{plan.executable->path, std::string(launcher::toString(plan.executable->rule)),
                                            plan.executable->entry, {}};
    }
    if (plan.blocker) {
        translation.blockers.push_back(blockerOfPlan(*plan.blocker, profile.id));
    }
    if (game != nullptr && plan.request) {
        translation.request = *plan.request;
    }
    return translation;
}

LaunchResolution resolveLaunch(const SystemInfo& system, const LaunchGame* game, const LaunchChoices& choices,
                               const LaunchContext& context) {
    LaunchResolution resolution;
    std::vector<ReadinessBlocker> leading;
    if (system.availability != Availability::Available) {
        leading.push_back(blockerOf(LaunchReadiness::SourceUnavailable, "source-unavailable",
                                    fmt::format("the ROM folder of {} cannot be read: {}", system.name.str(),
                                                reasonText(system.unavailableReason))));
    } else if (game != nullptr && game->availability == Availability::SourceUnavailable) {
        leading.push_back(blockerOf(LaunchReadiness::SourceUnavailable, "source-unavailable",
                                    fmt::format("the folder holding the game cannot be read: {}",
                                                reasonText(game->unavailableReason))));
    }
    if (game != nullptr && game->availability == Availability::Missing) {
        leading.push_back(blockerOf(LaunchReadiness::RomMissing, "rom-missing",
                                    fmt::format("\"{}\" is not in the ROM folder of {}", game->relativePath,
                                                system.name.str())));
    }

    const auto labels = context.translator->commandLabels(system.name);
    if (!labels) {
        resolution.translation.blockers = std::move(leading);
        resolution.translation.blockers.push_back(
            blockerOf(LaunchReadiness::RequiresConfiguration, "system-unknown",
                      fmt::format("the library configures no system named {}", system.name.str())));
        return resolution;
    }
    resolution.chosen = chooseLaunch(choices, *labels, *context.profiles);
    if (resolution.chosen.blocker) {
        leading.push_back(*resolution.chosen.blocker);
    } else if (resolution.chosen.choice.kind == launcher::LaunchChoiceKind::EsdeCommand) {
        resolution.translation = context.translator->translate(system.name, resolution.chosen.choice.value, game);
    } else {
        resolution.translation =
            translateProfile(system, game, *context.profiles->find(resolution.chosen.choice.value), context);
    }
    std::vector<ReadinessBlocker>& blockers = resolution.translation.blockers;
    blockers.insert(blockers.begin(), std::make_move_iterator(leading.begin()), std::make_move_iterator(leading.end()));
    return resolution;
}

} // namespace rv::catalog
