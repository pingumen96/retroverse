#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <rv/core/expected.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_plan.hpp>
#include <rv/ui/catalog_source.hpp>

namespace rv::app {

inline constexpr int kMockCatalogSchemaVersion = 1;

/// How a game of the mock catalog launches: an emulator profile (docs/i2/step-5.md) and the target it plans with.
struct MockLaunch {
    /// The profile's id: one of the catalog's own profiles, or the shipped and the user's ones.
    std::string profile;
    /// The ROM (absolute: a relative path in the file is relative to the catalog file's folder, made absolute against the
    /// current directory when the file's path is relative), its ES-DE system name,
    /// the launch's variables and the game's title.
    launcher::LaunchTarget target;
};

/// A game of the mock catalog: what the quick catalog shows, and how it launches (nothing: it cannot).
struct MockGame {
    ui::CatalogEntry entry;
    std::optional<MockLaunch> launch;
};

/// The quick catalog's games of the tests, instead of the ES-DE catalog (I2 step 13): invented games in a JSON
/// file (--mock-catalog), each with a launch target, and the emulator profiles they use, so that the application shell
/// launches emulators (the fake emulator of the tests, tests/fake_emulator) end to end. Format, schemaVersion 1, JSON
/// with comments:
///
///     {
///       "schemaVersion": 1,
///       "profiles": [ ... ],            // optional: emulator profiles (docs/i2/step-5.md), over the shipped and the
///                                       // user's ones (a profile with the same id replaces theirs)
///       "games": [                      // required (may be empty), in the order the catalog lists them
///         {
///           "id": "g0000000000000001",  // required: a CatalogEntry id (ui::isValidEntryId), unique
///           "title": "Lanterna di Vetro", // required, not empty
///           "platform": "Portatile 8 bit", "year": 1991, "developer": "...", "publisher": "...", "genre": "...",
///           "players": "1", "description": "...", // optional (year: 0 to 9999, 0 unknown)
///           "state": "",                // optional: a state key the documents label (ui::quick_catalog::kState*)
///           "launch": {                 // optional: without it the game cannot launch
///             "profile": "fake-return", // required: a profile id
///             "rom": "roms/lanterna.gb",// required: absolute, or relative to the catalog file's folder
///             "system": "gb",           // optional: the ES-DE system name ({system}, the profile's system settings)
///             "variables": {"k": "v"}   // optional: the launch's {var:NAME} values (strings)
///           }
///         }
///       ]
///     }
///
/// Unknown fields are errors at every level, so that a misspelled field never passes for a default.
struct MockCatalog {
    /// The file it was read from.
    std::filesystem::path file;
    std::vector<MockGame> games;
    /// Its "profiles", as a profile document (issues, origin User, the catalog file as their source); empty without
    /// the field.
    launcher::ProfileDocument profiles;

    /// The game `id`, or null.
    [[nodiscard]] const MockGame* find(std::string_view id) const noexcept;
    /// The entries the quick catalog lists, in order.
    [[nodiscard]] std::vector<ui::CatalogEntry> entries() const;
};

/// Parses a mock catalog document read from `file` (which names it in errors and resolves relative ROM paths).
/// Errors (context "<file>:<JSON pointer>"): Unsupported for another schemaVersion (checked first); ParseError for a
/// missing required field, a value of the wrong type or an unknown field; ValidationFailed for an invalid or repeated id,
/// an empty title, a year out of range, a path that is not valid UTF-8. Problems of the profiles are issues of
/// MockCatalog::profiles, not errors.
[[nodiscard]] Expected<MockCatalog> parseMockCatalog(const nlohmann::json& document, const std::filesystem::path& file);

/// Reads (core::readJsonFile: comments allowed) and parses `file`.
[[nodiscard]] Expected<MockCatalog> loadMockCatalog(const std::filesystem::path& file);

/// The profiles in effect for the mock catalog's games: `base` (the shipped and the user's profiles,
/// launcher::loadEmulatorProfiles), each replaced by the catalog's profile with the same id, then the catalog's other
/// profiles (launcher::mergeProfileDocuments: the catalog's issues after base's).
[[nodiscard]] launcher::EmulatorProfiles mockCatalogProfiles(launcher::EmulatorProfiles base,
                                                            const MockCatalog& catalog);

} // namespace rv::app
