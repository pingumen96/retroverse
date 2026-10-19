#pragma once

// WorldConfig (ADR-0010 §2): the settings a world is generated with. The shipped presets
// (assets/world/presets/<id>.json; RetroMuseumCozy, style §18, is the one curated preset) give every value; the user's
// <user data>/config/world.json, read and never written, overrides them per key and may name another preset or a seed.
// Values are integers in the plan's units, so the generator's arithmetic stays integer from the file to the plan
// (ADR-0010 §5, §8): ratios in permille, lengths in millimetres.
//
// A preset file (JSON with comments; every key required):
//   {"schemaVersion": 1, "id": "retro_museum_cozy", "name": "RetroMuseumCozy", "decorationDensityPermille": 400,
//    "growthReservePermille": 150, "mainPassageWidthMm": 1400, "minClearWidthMm": 1200,
//    "placement": {"include": "covered"}}
// world.json (JSON with comments; every key optional): "preset", "seed" (an integer from 0 to 2^64 - 1, or null for the
// default), "decorationDensityPermille", "growthReservePermille", "mainPassageWidthMm", "minClearWidthMm",
// "placement": {"include"}, "reduceMotion" (ADR-0012 §3; until I7's settings screen), "lightingProfile" (a lighting
// profile's id under assets/world/lighting, I3 step 16: how the room is lit when drawn, never what it holds).
// The canonical form (worldConfigToJson()) holds every key, "seed" null when it is not set.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>

namespace rv::world {

/// The preset a world uses when world.json names none.
inline constexpr std::string_view kDefaultPreset = "retro_museum_cozy";
/// The lighting profile a room is drawn with when world.json names none (style §8.1's default; room_look.hpp).
inline constexpr std::string_view kDefaultLightingProfile = "evening";

/// Which games of a system are eligible for its room (ADR-0010 §3).
enum class PlacementInclude : std::uint8_t {
    Covered, ///< the games with a cover (every game when no game of the system has one)
    All,     ///< every game
};

/// "covered", "all".
[[nodiscard]] constexpr std::string_view toString(PlacementInclude include) noexcept {
    switch (include) {
    case PlacementInclude::Covered:
        return "covered";
    case PlacementInclude::All:
        return "all";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<PlacementInclude> parsePlacementInclude(std::string_view text) noexcept {
    return detail::enumFromString(text, PlacementInclude::All);
}

/// The ranges every configuration keeps (checkWorldConfig()).
inline constexpr std::int64_t kMaxPermille = 1000;
inline constexpr std::int64_t kMinClearWidthMm = 600;
inline constexpr std::int64_t kMaxPassageWidthMm = 5000;

struct WorldConfig {
    /// The preset the values came from (a data id).
    std::string preset = std::string(kDefaultPreset);
    /// The world seed when the user names one; otherwise worldSeed() derives it from the library id.
    std::optional<std::uint64_t> seed;
    /// The share of decoration sockets that get decoration (ADR-0010 §2's 0.4).
    std::int64_t decorationDensityPermille = 400;
    /// The spine-out section's growth reserve (ADR-0010 §2, §5: 0.15).
    std::int64_t growthReservePermille = 150;
    /// The main passage's width and the hard minimum clear width of every passage (ADR-0010 §2: 1.4 m and 1.2 m).
    std::int64_t mainPassageWidthMm = 1400;
    std::int64_t minClearWidthMm = 1200;
    PlacementInclude include = PlacementInclude::Covered;
    /// Every transition instant (ADR-0012 §3). How the room is shown, not what it holds: the generator reads it never.
    bool reduceMotion = false;
    /// The lighting profile the room is drawn with (a data id; room_look.hpp). How the room is shown, like
    /// reduceMotion: the generator reads it never, so that switching profiles moves nothing.
    std::string lightingProfile = std::string(kDefaultLightingProfile);

    bool operator==(const WorldConfig&) const = default;
};

/// The world seed: the configured one, else core::deriveSeed("world", libraryId) (ADR-0010 §7), so a library always
/// gets the same world unless the user names another seed.
[[nodiscard]] std::uint64_t worldSeed(const WorldConfig& config, const catalog::LibraryId& library);

/// The values within their ranges: permilles from 0 to 1000, the minimum clear width from kMinClearWidthMm to the main
/// passage's width, which is at most kMaxPassageWidthMm; the preset and the lighting profile data ids. Errors:
/// InvalidArgument naming the key.
[[nodiscard]] Status checkWorldConfig(const WorldConfig& config);

/// Reads a preset file. Errors (context and line): ParseError, ValidationFailed (a key missing, unknown or out of
/// range, a schemaVersion other than 1).
[[nodiscard]] Expected<WorldConfig> parsePreset(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/presets/<id>.json, whose "id" must be `id`. Errors: those of parsePreset(), of reading the
/// file (NotFound, ...), InvalidArgument for an id that is not a data id.
[[nodiscard]] Expected<WorldConfig> loadPreset(const std::filesystem::path& assetRoot, std::string_view id);

/// What world.json sets; a missing key keeps the preset's value.
struct WorldConfigOverrides {
    std::optional<std::string> preset;
    /// Set when "seed" is there: nothing inside for null (the default seed).
    std::optional<std::optional<std::uint64_t>> seed;
    std::optional<std::int64_t> decorationDensityPermille;
    std::optional<std::int64_t> growthReservePermille;
    std::optional<std::int64_t> mainPassageWidthMm;
    std::optional<std::int64_t> minClearWidthMm;
    std::optional<PlacementInclude> include;
    std::optional<bool> reduceMotion;
    std::optional<std::string> lightingProfile;

    bool operator==(const WorldConfigOverrides&) const = default;
};

/// Reads world.json. Errors (context and line): ParseError, ValidationFailed (an unknown key, a value of another type
/// or out of its range).
[[nodiscard]] Expected<WorldConfigOverrides> parseWorldConfigOverrides(std::string_view text, std::string_view context);
/// `base` with the overrides' keys (their "preset" is only recorded: loadWorldConfig() loads that preset first).
/// Errors: those of checkWorldConfig() for the result.
[[nodiscard]] Expected<WorldConfig> applyOverrides(WorldConfig base, const WorldConfigOverrides& overrides);

/// The configuration a world is generated with, and what was wrong with the user's file.
struct LoadedWorldConfig {
    WorldConfig config;
    /// Problems of <userConfigDir>/world.json: a file that cannot be read or parsed, a value out of range, a preset
    /// that does not load, a combination checkWorldConfig() refuses. The file is then left out whole and the default
    /// preset used, as the other configuration files of the user are (CLAUDE.md, "Configuration files").
    std::vector<Error> issues;
};

/// The default preset, or the one world.json names, with world.json's keys. A missing world.json is no issue. Errors:
/// the default preset does not load (a broken installation).
[[nodiscard]] Expected<LoadedWorldConfig> loadWorldConfig(const std::filesystem::path& assetRoot,
                                                          const std::filesystem::path& userConfigDir);

/// The canonical document of the configuration (core::writeCanonicalJson). Errors: InvalidArgument for a preset that
/// is not a data id or values checkWorldConfig() refuses.
[[nodiscard]] Expected<nlohmann::json> worldConfigToJson(const WorldConfig& config);
/// Its canonical text. Errors: those of worldConfigToJson().
[[nodiscard]] Expected<std::string> writeWorldConfig(const WorldConfig& config);
/// Reads a canonical text back. Errors (context and line): ParseError, ValidationFailed (not canonical, a key missing,
/// unknown or out of range).
[[nodiscard]] Expected<WorldConfig> readWorldConfig(std::string_view text, std::string_view context);

} // namespace rv::world
