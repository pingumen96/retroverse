#pragma once

// Launch resolution (ADR-0008 §8): which command or profile a game launches with, its translation into a launch
// request, and its readiness. The catalog sees the library's commands only through a port (ICommandTranslator), which
// the adapter that read them implements (rv_esde's EsdeCommandTranslator translates ES-DE's commands with its find
// rules); Retroverse's own emulator profiles are planned here with rv_launcher (planLaunch). Nothing is started: the
// result is a launcher::LaunchRequest for a LaunchSession, or the blockers that say why the game cannot start.
//
// The choice, most specific first: Retroverse's choice for the game (the catalog database's, retroverse-cli assign
// --game: I2 step 12), the game's <altemulator> while ES-DE's AlternativeEmulatorPerGame is on, Retroverse's choice for
// the system (the catalog database's from retroverse-cli assign, the user's latest explicit choice, which ranks before
// emulators.json's "systemChoices" for the same system: the caller fills LaunchChoices::system with the first of the
// two), the gamelist's <alternativeEmulator>, the system's first command. Retroverse's
// choices are stored as a label or a profile id and translated again at every refresh and launch: one that no longer
// exists makes the game "requires launcher configuration", naming it; ES-DE's own labels that name no command fall back
// to the first command with ES-DE's warning, as ES-DE's do: an invalid <alternativeEmulator> to the first command, an
// invalid game's <altemulator> past the gamelist's <alternativeEmulator> too (ES-DE never looks at it then), so to
// Retroverse's choice for the system when there is one, else to the first command.
//
// Readiness is the first that applies, in LaunchReadiness's order: source unavailable, ROM missing, requires launcher
// configuration, emulator not found, core not found, ready. It is computed at refresh (for a system, without a game, or
// for each game) and again before each launch; "ready" does not wait for a launch test.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/launcher/arg_template.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_request.hpp>

namespace rv::catalog {

/// Whether a game (or a system's games) can launch. The values are in ADR-0008 §8's precedence: of several that apply,
/// the first one is the readiness.
enum class LaunchReadiness : std::uint8_t {
    SourceUnavailable,     ///< the system's ROM folder, or the folder holding the game, cannot be reached or listed
    RomMissing,            ///< the game's file is gone (a gamelist entry left behind)
    RequiresConfiguration, ///< the command or profile cannot run as it is: a token Retroverse does not run, a shell or
                           ///< launcher, a stored choice that no longer exists, a request Windows would refuse
    EmulatorNotFound,      ///< its program is nowhere on the machine
    CoreNotFound,          ///< its program is there, but a core (a file the command or profile needs) is not
    Ready,                 ///< nothing blocks the launch
};

/// "source-unavailable", "rom-missing", "requires-configuration", "emulator-not-found", "core-not-found", "ready".
[[nodiscard]] constexpr std::string_view toString(LaunchReadiness readiness) noexcept {
    switch (readiness) {
    case LaunchReadiness::SourceUnavailable:
        return "source-unavailable";
    case LaunchReadiness::RomMissing:
        return "rom-missing";
    case LaunchReadiness::RequiresConfiguration:
        return "requires-configuration";
    case LaunchReadiness::EmulatorNotFound:
        return "emulator-not-found";
    case LaunchReadiness::CoreNotFound:
        return "core-not-found";
    case LaunchReadiness::Ready:
        return "ready";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<LaunchReadiness> parseLaunchReadiness(std::string_view text) noexcept {
    return detail::enumFromString(text, LaunchReadiness::Ready);
}

/// For people: "source unavailable", "ROM missing", "requires launcher configuration", "emulator not found", "core not
/// found", "ready".
[[nodiscard]] std::string_view describe(LaunchReadiness readiness) noexcept;

/// One reason a launch cannot start.
struct ReadinessBlocker {
    LaunchReadiness readiness = LaunchReadiness::RequiresConfiguration;
    /// Stable, lowercase, words joined by '-' (the codes are listed with the code that reports them: docs/i2/step-10.md):
    /// "source-unavailable", "rom-missing", "choice-stale", "profile-unknown", "token-unsupported", "program-refused",
    /// "emulator-not-found", "core-not-found", ...
    std::string code;
    /// One sentence naming what is missing or wrong: the token, the label, the program, the path looked for.
    std::string message;

    bool operator==(const ReadinessBlocker&) const = default;
};

/// The first readiness that applies among `blockers`; Ready when there is none.
[[nodiscard]] LaunchReadiness readinessOf(std::span<const ReadinessBlocker> blockers) noexcept;

/// Where a launch's program or core was found, and how.
struct FoundLocation {
    std::filesystem::path path;
    /// The rule: an ES-DE find rule type ("winregistrypath", "winregistryvalue", "systempath", "staticpath",
    /// "corepath"), "command" for an absolute path the command names itself, or a profile's rule ("App Paths", "path").
    std::string rule;
    /// The rule's entry as written ("retroarch.exe", "%EMUPATH%\cores", the profile's path), or the command's element.
    std::string entry;
    /// Where the rule found it, for people: the registry hive ("HKLM"), the folder of the executable search; empty
    /// when the path says it all.
    std::string detail;

    bool operator==(const FoundLocation&) const = default;
};

/// A command or a profile translated for a game, or for a system (no game: what does not depend on the game).
struct LaunchTranslation {
    /// The request as far as it could be built: there once the program is found and the game given, also when a
    /// blocker remains (a dry run shows its arguments; a token that blocks the launch stays in them as written). Only a
    /// request without blockers may start (LaunchResolution::request()), and it has passed
    /// launcher::validateLaunchRequest().
    std::optional<launcher::LaunchRequest> request;
    /// Why it cannot start, in the order they were found; empty when ready.
    std::vector<ReadinessBlocker> blockers;
    /// The program, when found.
    std::optional<FoundLocation> program;
    /// The core, when the command names one (%CORE_X%) and it was found.
    std::optional<FoundLocation> core;
    /// What was looked at and failed while the program or the core was looked for, in order ("App Paths
    /// \"retroarch.exe\": not registered"): what "emulator not found" and "core not found" rest on.
    std::vector<std::string> attempts;
    /// What a dry run shows besides: what a system's readiness leaves to each game (an %INJECT% file), an %INJECT%
    /// element dropped because ES-DE would inject nothing.
    std::vector<std::string> notes;

    [[nodiscard]] LaunchReadiness readiness() const noexcept { return readinessOf(blockers); }
};

/// Where the choice of a launch comes from, most specific first.
enum class ChoiceSource : std::uint8_t {
    RetroverseGame,   ///< Retroverse's choice for the game (the catalog database's: assign --game, I2 step 12)
    EsdeGame,         ///< the game's <altemulator>, while ES-DE's AlternativeEmulatorPerGame is on
    RetroverseSystem, ///< Retroverse's choice for the system (the catalog database's from assign, else emulators.json's
                      ///< "systemChoices")
    EsdeSystem,       ///< the gamelist's top-level <alternativeEmulator>
    FirstCommand,     ///< the system's first command: ES-DE's default
};

/// "retroverse-game", "esde-game", "retroverse-system", "esde-system", "first-command".
[[nodiscard]] constexpr std::string_view toString(ChoiceSource source) noexcept {
    switch (source) {
    case ChoiceSource::RetroverseGame:
        return "retroverse-game";
    case ChoiceSource::EsdeGame:
        return "esde-game";
    case ChoiceSource::RetroverseSystem:
        return "retroverse-system";
    case ChoiceSource::EsdeSystem:
        return "esde-system";
    case ChoiceSource::FirstCommand:
        return "first-command";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<ChoiceSource> parseChoiceSource(std::string_view text) noexcept {
    return detail::enumFromString(text, ChoiceSource::FirstCommand);
}

/// For people: "Retroverse's choice for the game", "the game's <altemulator>", "Retroverse's choice for the system",
/// "the gamelist's <alternativeEmulator>", "the system's first command".
[[nodiscard]] std::string_view describe(ChoiceSource source) noexcept;

/// What a launch is chosen from.
struct LaunchChoices {
    /// Retroverse's choice for the game (the catalog database's: catalog_db::SessionStore::gameChoice(), I2 step 12).
    std::optional<launcher::LaunchChoice> game;
    /// The game's <altemulator> as written (CatalogEntry::altEmulator); empty when none.
    std::string gameAltEmulator;
    /// ES-DE's AlternativeEmulatorPerGame: false leaves the game's <altemulator> unused, as ES-DE leaves it.
    bool alternativeEmulatorPerGame = true;
    /// Retroverse's choice for the system: the catalog database's (catalog_db::SessionStore::systemChoice(), from
    /// retroverse-cli assign) when there is one, else launcher::EmulatorProfiles::systemChoice() (I2 step 12).
    std::optional<launcher::LaunchChoice> system;
    /// The gamelist's <alternativeEmulator> label (SystemInfo::alternativeEmulator); empty when none.
    std::string systemAlternativeEmulator;
};

/// The choice made.
struct ChosenLaunch {
    ChoiceSource source = ChoiceSource::FirstCommand;
    /// An ES-DE command label (empty for a system's single unlabelled command) or a profile id.
    launcher::LaunchChoice choice;
    /// ES-DE's own labels that name no command of the system, passed over as ES-DE passes over them (the header
    /// comment says where each falls back to), each with what ES-DE warns about it; and a game's <altemulator> left
    /// unused while AlternativeEmulatorPerGame is off.
    std::vector<std::string> warnings;
    /// A Retroverse choice naming a label or a profile that no longer exists (or a system without commands): requires
    /// launcher configuration, naming it. Nothing is translated then.
    std::optional<ReadinessBlocker> blocker;

    bool operator==(const ChosenLaunch&) const = default;
};

/// Chooses as the header comment says. `labels` are the system's command labels in order (ICommandTranslator::
/// commandLabels()); `profiles` tells which profile ids exist.
[[nodiscard]] ChosenLaunch chooseLaunch(const LaunchChoices& choices, std::span<const std::string> labels,
                                        const launcher::EmulatorProfiles& profiles);

/// A game to launch, as the catalog knows it.
struct LaunchGame {
    /// The path relative to the system's ROM folder as listed on disk, '/' between the elements
    /// (LookupKey::relativePath()): what ES-DE's %ROM% ends with.
    std::string relativePath;
    /// The game is a folder (a directory whose name carries a configured extension).
    bool directory = false;
    /// Whether its file is there (CatalogEntry::availability): Missing makes it "ROM missing", SourceUnavailable
    /// "source unavailable".
    Availability availability = Availability::Available;
    SourceUnavailableReason unavailableReason = SourceUnavailableReason::None;
    /// Names the launch (LaunchRequest::label: "<title> with <command label or profile name>"); empty: the file name.
    std::string title;
};

/// The port through which the catalog translates a library's own commands (rv_esde: EsdeCommandTranslator).
class ICommandTranslator {
public:
    virtual ~ICommandTranslator() = default;

    /// The labels of the commands of `system` in their order, the first one the default (an unlabelled single command
    /// is ""); nothing when the library does not configure the system.
    [[nodiscard]] virtual std::optional<std::vector<std::string>> commandLabels(const SystemName& system) const = 0;
    /// The command labelled `label` of `system` translated for `game`, or for the system when `game` is null (the
    /// program, the core and the tokens that do not depend on a game; no request). A label the system does not have
    /// gives a "choice-stale" blocker.
    [[nodiscard]] virtual LaunchTranslation translate(const SystemName& system, std::string_view label,
                                                      const LaunchGame* game) const = 0;
};

/// What a resolution runs with. Every pointer must be set.
struct LaunchContext {
    const ICommandTranslator* translator = nullptr;
    const launcher::EmulatorProfiles* profiles = nullptr;
    /// The machine a profile's executable and required files are looked for on.
    const core::ISystemProbe* probe = nullptr;
    /// The values of a profile's {env:NAME}; nullptr: the process's (launcher::processEnvironmentLookup()).
    launcher::EnvironmentLookup environment;
};

/// A launch resolved for a game, or for a system.
struct LaunchResolution {
    ChosenLaunch chosen;
    /// The chosen command's or profile's translation; its blockers begin with those of the game and the choice (source
    /// unavailable, ROM missing, a stale choice).
    LaunchTranslation translation;

    [[nodiscard]] LaunchReadiness readiness() const noexcept { return translation.readiness(); }
    /// The request when ready, else nothing.
    [[nodiscard]] std::optional<launcher::LaunchRequest> request() const;
};

/// Resolves the launch of `game` of `system` (null: the system's readiness, which a game of it shares unless its own
/// file or choice differ): the choice (chooseLaunch() over the translator's labels), the game's and the system's
/// availability, then the translation of the chosen command (the translator) or profile (translateProfile()).
[[nodiscard]] LaunchResolution resolveLaunch(const SystemInfo& system, const LaunchGame* game,
                                             const LaunchChoices& choices, const LaunchContext& context);

/// Plans `profile` for `game` of `system` (null: the system, with a stand-in ROM path in its folder) with
/// launcher::planLaunch(), its blockers mapped to readiness: an unknown token or variable, a refused program or a
/// request Windows would refuse require launcher configuration, the executable not found is "emulator not found", a
/// required file missing "core not found". The ROM's own presence is the game's availability, not checked here.
[[nodiscard]] LaunchTranslation translateProfile(const SystemInfo& system, const LaunchGame* game,
                                                 const launcher::EmulatorProfile& profile,
                                                 const LaunchContext& context);

} // namespace rv::catalog
