#pragma once

// GPU-free (rv_library, ADR-0018 §5): how the systems of a library launch: for each, the command or profile chosen and
// why, its readiness without a game, and every ES-DE command and Retroverse profile that serves it with their own
// readiness, so that a system that cannot launch shows what could. retroverse-cli profiles prints it, the emulators screen
// shows it (through the catalog worker). Nothing is started or written; a system's games, the launch test's sample, are
// read from the catalog.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/core/expected.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/library/launch_library.hpp>

namespace rv::library {

/// A command or a profile that serves a system.
struct LaunchOption {
    launcher::LaunchChoice choice;
    /// The command as es_systems.xml writes it; a profile's display name.
    std::string text;
    /// Translated for the system: what does not depend on a game.
    catalog::LaunchTranslation translation;
};

/// A system's row of the table.
struct SystemLaunchRow {
    catalog::SystemInfo system;
    /// The gamelist's <alternativeEmulator>.
    std::string alternativeEmulator;
    /// Retroverse's choice for the system (assign's, else emulators.json's).
    std::optional<launcher::LaunchChoice> systemChoice;
    /// What its games launch with, why, and its readiness without a game.
    catalog::LaunchResolution resolution;
    std::vector<LaunchOption> commands;
    std::vector<LaunchOption> profiles;
};

/// The row of `system`, `alternativeEmulator` its gamelist's as known to the caller. A system the library no longer
/// configures has no options, and its resolution says so.
[[nodiscard]] SystemLaunchRow systemLaunchRow(const LaunchLibrary& library, const catalog::SystemInfo& system,
                                              const std::string& alternativeEmulator);

/// The rows of every system the library configures, or of `only`, each gamelist read for its <alternativeEmulator>.
[[nodiscard]] std::vector<SystemLaunchRow> launchTable(LaunchLibrary& library,
                                                       std::optional<std::string_view> only = std::nullopt);

/// SC-10's suggestion: the commands and profiles of `row`, the ready ones first, each kind in its own order.
[[nodiscard]] std::vector<LaunchOption> suggestedOptions(const SystemLaunchRow& row);

/// A game offered to a launch test.
struct SampleGame {
    catalog::GameId id;
    std::string title;

    bool operator==(const SampleGame&) const = default;
};

/// The games a launch test of `system` offers, at most `limit` (ADR-0018 §5): the available ones, the most recently
/// played first (as the directory's list says it: launch tests left out), then by title; the first is the sample.
[[nodiscard]] Expected<std::vector<SampleGame>> sampleGames(catalog_db::SqliteCatalogQuery& query,
                                                            const catalog::SystemName& system,
                                                            const catalog::LocalTimeToUtc& localToUtc,
                                                            std::size_t limit);

} // namespace rv::library
