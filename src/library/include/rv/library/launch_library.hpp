#pragma once

// GPU-free (rv_library, ADR-0018 §13): the one launch library. What a launch of the ES-DE catalog is resolved with:
// library.json's library, ES-DE's settings, systems and find rules as they are now, the emulator profiles, and the
// machine (platform_win::Win32SystemProbe). retroverse-cli's `profiles`, `launch` and `assign` and the app's catalog
// worker read through it (it was two copies until I7 step 2).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/platform_win/system_probe.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::library {

/// Everything a launch is resolved with. Held by pointer: the translator and the context point into it. Used on one
/// thread.
struct LaunchLibrary {
    catalog::LibraryProfile profile;
    esde::EsdeSettings settings;
    esde::ResolvedRoot romRoot;
    esde::SystemsConfiguration systems;
    esde::FindRules findRules;
    /// The systems as the library configures them now (their ROM folders looked at: availability).
    std::vector<catalog::SystemInfo> systemInfos;
    launcher::EmulatorProfiles profiles;
    platform_win::Win32SystemProbe probe;
    std::unique_ptr<esde::EsdeCommandTranslator> translator;
    /// Retroverse's choices for the library's systems that the catalog database holds (retroverse-cli assign), by
    /// system name: they rank before emulators.json's "systemChoices" (systemChoices()). Empty until
    /// loadAssignedChoices() or the caller's own read of the database.
    std::map<std::string, launcher::LaunchChoice, std::less<>> assignedChoices;
    /// The programs chosen for emulators (ADR-0018 §5: rv_launch_program_choices, ahead of the find rules): the
    /// translator reads this map. Empty until loadAssignedChoices() or setProgramChoices().
    esde::ProgramChoices programChoices;
    /// What reading the library met: settings, roots, systems, find rules, gamelists, profiles.
    std::vector<catalog::Issue> issues;

    [[nodiscard]] catalog::LaunchContext context() const {
        return catalog::LaunchContext{translator.get(), &profiles, &probe, nullptr};
    }
    /// The system `name` as the library configures it now; null when it no longer does.
    [[nodiscard]] const catalog::SystemInfo* findSystem(std::string_view name) const;
};

/// Reads the library `profile` (library.json's) describes and the emulator profiles of `paths` (the shipped and the
/// user's emulators.json). Errors: ES-DE's settings or systems cannot be read (the code the read's, the message "ES-DE's
/// settings cannot be read: <the read's error>"; the catalog can still be browsed, its games cannot be launched).
[[nodiscard]] Expected<std::unique_ptr<LaunchLibrary>> loadLaunchLibrary(catalog::LibraryProfile profile,
                                                                         const core::Paths& paths);

/// The gamelist of `system`, when it has one that can be read: what gives its <alternativeEmulator> and the games'
/// <altemulator>. A gamelist that fails is an issue (the choices it holds are then unknown).
[[nodiscard]] std::optional<esde::Gamelist> readSystemGamelist(LaunchLibrary& library,
                                                               const catalog::SystemInfo& system);

/// The choices of a game of `system` (ADR-0008 §8; docs/i2/step-12.md): Retroverse's for the game (`game`, the catalog
/// database's), the game's <altemulator> (`gameAltEmulator`) while AlternativeEmulatorPerGame is on, Retroverse's for
/// the system (`assignedSystem`, the catalog database's from retroverse-cli assign, before emulators.json's
/// "systemChoices"), the gamelist's <alternativeEmulator> (`systemAlternativeEmulator`, as the last import read it).
[[nodiscard]] catalog::LaunchChoices launchChoices(const LaunchLibrary& library, const catalog::SystemName& system,
                                                   const std::optional<launcher::LaunchChoice>& game,
                                                   std::string gameAltEmulator,
                                                   const std::optional<launcher::LaunchChoice>& assignedSystem,
                                                   std::string systemAlternativeEmulator);

/// The choices of a system without a game: Retroverse's for the system (library.assignedChoices, else emulators.json's),
/// the gamelist's <alternativeEmulator>.
[[nodiscard]] catalog::LaunchChoices systemChoices(const LaunchLibrary& library, const catalog::SystemInfo& system,
                                                   const std::string& alternativeEmulator);

/// Where Retroverse's choice for a system comes from (I2 step 12): the catalog database's (`assign`), which ranks first
/// as the user's latest explicit choice, else emulators.json's "systemChoices".
inline constexpr std::string_view kAssignedOrigin = "assign";
inline constexpr std::string_view kEmulatorsJsonOrigin = "emulators.json";

/// Where Retroverse's choice for `system` comes from: kAssignedOrigin, kEmulatorsJsonOrigin, or empty when it has none.
[[nodiscard]] std::string_view systemChoiceOrigin(const LaunchLibrary& library, std::string_view system);

/// `rows` as the library's programs chosen for emulators.
void setProgramChoices(LaunchLibrary& library, const std::vector<catalog_db::ProgramChoiceRow>& rows);

/// Reads into `library` Retroverse's choices for its systems and the programs chosen for its emulators from the catalog
/// database (retroverse-cli assign), opened for reading only: never made, migrated or written. No database yet, or one that holds no choice of the library: no
/// choices. A database that cannot be read is an issue (assign's choices are then not known).
void loadAssignedChoices(LaunchLibrary& library, const core::Paths& paths);

/// How a game's file is looked at by resolveCatalogLaunch().
enum class GameFileCheck : std::uint8_t {
    /// As the last import saw it (the catalog's availability): the card's readiness, which must not touch the disk for
    /// every game shown.
    Catalog,
    /// On disk now, spelled as its folder lists it (esde::listedPath): right before a launch.
    Disk,
};

/// Resolves the launch of the game the catalog holds as `details` (catalog::resolveLaunch): the system as the library
/// configures it now (a system it no longer configures: "requires launcher configuration", system-not-configured), the
/// game's file as `check` says, its <altemulator> and its system's <alternativeEmulator> as the last import read them,
/// Retroverse's choices given (launchChoices()).
[[nodiscard]] catalog::LaunchResolution resolveCatalogLaunch(const LaunchLibrary& library,
                                                             const catalog_db::GameDetails& details,
                                                             const std::optional<launcher::LaunchChoice>& gameChoice,
                                                             const std::optional<launcher::LaunchChoice>& assignedSystem,
                                                             GameFileCheck check);

/// Resolves what the games of `system` share (catalog::resolveLaunch without a game): the program, the core, the
/// tokens, a %STARTDIR% folder that does not depend on the game. `system` as the library configures it now, or the
/// catalog's when it no longer does (then system-not-configured); `alternativeEmulator` is the catalog's.
[[nodiscard]] catalog::LaunchResolution resolveSystemLaunch(const LaunchLibrary& library,
                                                            const catalog::SystemInfo& catalogSystem,
                                                            const std::optional<launcher::LaunchChoice>& assignedSystem);

/// A system's resolution leaves something to each game (LaunchTranslation::notes without a game: an %INJECT% file, a
/// %STARTDIR%=%GAMEDIR% folder), so each game's readiness may differ from the system's.
[[nodiscard]] bool resolvedPerGame(const catalog::LaunchResolution& systemResolution) noexcept;

} // namespace rv::library
