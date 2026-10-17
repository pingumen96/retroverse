#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// What the app's catalog worker resolves launches with (I2 step 13; ADR-0008 §8): the library <user data>/config/
// library.json records, ES-DE's settings, systems and find rules as they are now, the emulator profiles, and the
// machine (platform_win::Win32SystemProbe). The same reading as retroverse-cli's `launch --game` (tools/cli/src/
// launch_text.hpp, cmd_launch.cpp: loadLaunchLibrary, resolveCatalogGame), which keeps its own private copy.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/platform_win/system_probe.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::app {

/// Everything a launch of the ES-DE catalog is resolved with. Held by pointer: the translator and the context point
/// into it. Used on one thread (the catalog worker).
struct CatalogLaunchLibrary {
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
    /// What reading the library met (settings, roots, systems, find rules, profiles), for the log.
    std::vector<catalog::Issue> issues;

    [[nodiscard]] catalog::LaunchContext context() const {
        return catalog::LaunchContext{translator.get(), &profiles, &probe, nullptr};
    }
    /// The system `name` as the library configures it now; null when it no longer does.
    [[nodiscard]] const catalog::SystemInfo* findSystem(std::string_view name) const;
};

/// Reads the library `profile` (library.json's) describes and the emulator profiles of `paths` (the shipped and the
/// user's emulators.json). Errors: ES-DE's settings or systems cannot be read (the catalog can still be browsed; its
/// games cannot be launched).
[[nodiscard]] Expected<std::unique_ptr<CatalogLaunchLibrary>> loadCatalogLaunchLibrary(catalog::LibraryProfile profile,
                                                                                      const core::Paths& paths);

/// The choices of a game of `system` (ADR-0008 §8; docs/i2/step-12.md): Retroverse's for the game (`game`, the catalog
/// database's), the game's <altemulator> (`gameAltEmulator`) while AlternativeEmulatorPerGame is on, Retroverse's for
/// the system (`assignedSystem`, the catalog database's from retroverse-cli assign, before emulators.json's
/// "systemChoices"), the gamelist's <alternativeEmulator> (`systemAlternativeEmulator`, as the last import read it).
[[nodiscard]] catalog::LaunchChoices catalogLaunchChoices(const CatalogLaunchLibrary& library,
                                                          const catalog::SystemName& system,
                                                          const std::optional<launcher::LaunchChoice>& game,
                                                          std::string gameAltEmulator,
                                                          const std::optional<launcher::LaunchChoice>& assignedSystem,
                                                          std::string systemAlternativeEmulator);

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
/// Retroverse's choices given (catalogLaunchChoices()).
[[nodiscard]] catalog::LaunchResolution resolveCatalogLaunch(const CatalogLaunchLibrary& library,
                                                             const catalog_db::GameDetails& details,
                                                             const std::optional<launcher::LaunchChoice>& gameChoice,
                                                             const std::optional<launcher::LaunchChoice>& assignedSystem,
                                                             GameFileCheck check);

/// Resolves what the games of `system` share (catalog::resolveLaunch without a game): the program, the core, the
/// tokens, a %STARTDIR% folder that does not depend on the game. `system` as the library configures it now, or the
/// catalog's when it no longer does (then system-not-configured); `alternativeEmulator` is the catalog's.
[[nodiscard]] catalog::LaunchResolution resolveSystemLaunch(const CatalogLaunchLibrary& library,
                                                            const catalog::SystemInfo& catalogSystem,
                                                            const std::optional<launcher::LaunchChoice>& assignedSystem);

/// A system's resolution leaves something to each game (LaunchTranslation::notes without a game: an %INJECT% file, a
/// %STARTDIR%=%GAMEDIR% folder), so each game's readiness may differ from the system's.
[[nodiscard]] bool resolvedPerGame(const catalog::LaunchResolution& systemResolution) noexcept;

} // namespace rv::app
