#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// player.json (ADR-0018 §2): what had no file before I7. The shipped defaults are assets/config/player.json (JSON with
// comments, every key); the user's <user data>/config/player.json, read and never written, overrides them key by key;
// settings.json's "player" section goes over both. A user file that cannot be read, parsed or checked is left out whole.
//
//   {"schemaVersion": 1, "horizontalFovDeg": null, "mouseSensitivity": 1, "stickSensitivity": 1, "invertY": false,
//    "displayMode": "window", "textScale": 1}
//
// "horizontalFovDeg" null keeps the scene's lens, so that with no settings every golden stays as it is.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>

namespace rv::app {

/// The file's name under assets/config and <user data>/config, and settings.json's section.
inline constexpr std::string_view kPlayerSettingsFile = "player.json";
inline constexpr int kPlayerSettingsSchemaVersion = 1;
inline constexpr std::string_view kPlayerSettingsSection = "player";

/// How the window shows (ADR-0018 §3: exclusive fullscreen is not offered).
enum class DisplayMode : std::uint8_t { Window, Borderless };
/// "window", "borderless".
[[nodiscard]] std::string_view toString(DisplayMode mode) noexcept;

inline constexpr double kMinPlayerFovDeg = 30.0;
inline constexpr double kMaxPlayerFovDeg = 150.0;
inline constexpr double kMinPlayerSensitivity = 0.1;
inline constexpr double kMaxPlayerSensitivity = 5.0;
inline constexpr double kMinTextScale = 0.5;
inline constexpr double kMaxTextScale = 3.0;

struct PlayerSettings {
    /// None: the scene's lens.
    std::optional<double> horizontalFovDeg;
    double mouseSensitivity = 1.0;
    double stickSensitivity = 1.0;
    bool invertY = false;
    DisplayMode displayMode = DisplayMode::Window;
    double textScale = 1.0;

    bool operator==(const PlayerSettings&) const = default;
};

/// The keys the settings screen may set in settings.json's section (every key of the file).
[[nodiscard]] std::span<const std::string_view> playerSettingKeys() noexcept;

/// Reads a whole document (the defaults, or the defaults merged with the user's). Errors (context and JSON pointer):
/// ParseError for a key missing or of another type, an unknown key; ValidationFailed for a value out of its range or an
/// unknown name; Unsupported for another schemaVersion.
[[nodiscard]] Expected<PlayerSettings> parsePlayerSettings(const nlohmann::json& document, std::string_view context);

/// The settings of the run, and what was wrong with the user's file.
struct LoadedPlayerSettings {
    PlayerSettings settings;
    /// The user's file could not be read, parsed or checked: left out whole.
    std::vector<Error> issues;
    /// The document under settings.json: the defaults with the user's file over them ("Ripristina"'s values).
    nlohmann::json belowSettings;
    /// Keys of settings.json whose value the parser refused: left out, each alone.
    std::vector<std::string> refusedKeys;
};

/// assets/config/player.json, <userConfigDir>/player.json over it (a missing file is no issue), the keys of the
/// settings.json beside that folder over both. Errors: the defaults do not load (a broken installation).
[[nodiscard]] Expected<LoadedPlayerSettings> loadPlayerSettings(const std::filesystem::path& assetRoot,
                                                                const std::filesystem::path& userConfigDir);

} // namespace rv::app
