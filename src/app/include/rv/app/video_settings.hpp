#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// video.json (I5 step 1, ADR-0016 §4, specification §9.2's [video] section): how the screens play. The shipped defaults
// are assets/config/video.json (JSON with comments, every key); the user's <user data>/config/video.json, read and never
// written, overrides them key by key (objects merged member by member, any other value replaced). A user file that cannot
// be read, parsed or checked is left out whole, the defaults used (an issue logged), as world.json is.
//
//   {
//     "schemaVersion": 1,
//     "enabled": true,                 // clips play at all (false: the screens show stills and title cards only)
//     "maxDecoders": 2,                // 1 or 2 (ADR-0016 §2; step 2's scheduler)
//     "maxRows": 480, "viewerMaxRows": 720,
//     "proximityMeters": 12,
//     "crtEffect": "light",            // "off", "light", "vintage" (step 6's presets)
//     "audio": {"mode": "nearest-focused", "master": 1, "video": 1, "effects": 1, "ambience": 1, "muted": false},
//     "hero": {"enabled": true, "playlist": "shuffle", "previewDuration": 30, "showOnFocus": true, "volume": 1,
//              "crtPreset": ""},
//     "secondary": {...}, "cabinet": {...},   // the same keys as "hero"
//     "rooms": {"room:snes": {"hero": {"playlist": "explicit", "games": ["g0123456789abcdef"]}}}
//   }
//
// Per screen kind: "enabled" (autoplay: false leaves the screen to selections), "playlist" (playlist.hpp's modes),
// "previewDuration" (seconds a clip plays before the next, 1 to 3600), "showOnFocus" (the focused or inspected game
// plays), "volume" (0 to 1), "crtPreset" ("" for the kind's default, else "neutral", "consumer", "arcade", "monitor").
// Per room ("rooms.<room id>.<kind>"): the same keys, each optional, and "games" (the explicit mode's GameIds). Step 1
// uses "enabled", the hero's keys and the rooms'; the others are read and checked for the steps that follow.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/playlist.hpp>
#include <rv/core/expected.hpp>

namespace rv::app {

/// The file's name under assets/config and <user data>/config.
inline constexpr std::string_view kVideoSettingsFile = "video.json";
inline constexpr int kVideoSettingsSchemaVersion = 1;

/// The screens' kinds that video.json configures (ADR-0016 §1; the viewer's is not configured per kind).
enum class ScreenKind : std::uint8_t {
    Hero,
    Secondary,
    Cabinet,
};
/// "hero", "secondary", "cabinet".
[[nodiscard]] std::string_view toString(ScreenKind kind) noexcept;

/// The user's CRT effect (specification §9.6).
enum class CrtEffect : std::uint8_t {
    Off,
    Light,
    Vintage,
};
[[nodiscard]] std::string_view toString(CrtEffect effect) noexcept;

/// What the audible screen follows (step 5).
enum class AudioMode : std::uint8_t {
    NearestFocused,
    Off,
};
[[nodiscard]] std::string_view toString(AudioMode mode) noexcept;

struct AudioSettings {
    AudioMode mode = AudioMode::NearestFocused;
    double master = 1.0;
    double video = 1.0;
    double effects = 1.0;
    double ambience = 1.0;
    bool muted = false;

    bool operator==(const AudioSettings&) const = default;
};

/// One screen kind's settings, or a room's for that kind once resolved (VideoSettings::screen()).
struct ScreenSettings {
    bool enabled = true;
    PlaylistMode playlist = PlaylistMode::Shuffle;
    double previewSeconds = 30.0;
    bool showOnFocus = true;
    double volume = 1.0;
    std::string crtPreset;
    /// The explicit mode's GameIds (a room's "games").
    std::vector<std::string> games;

    bool operator==(const ScreenSettings&) const = default;
};

/// A room's keys for one kind, each optional.
struct ScreenOverrides {
    std::optional<bool> enabled;
    std::optional<PlaylistMode> playlist;
    std::optional<double> previewSeconds;
    std::optional<bool> showOnFocus;
    std::optional<double> volume;
    std::optional<std::string> crtPreset;
    std::optional<std::vector<std::string>> games;

    bool operator==(const ScreenOverrides&) const = default;
};

struct RoomVideoOverrides {
    std::optional<ScreenOverrides> hero;
    std::optional<ScreenOverrides> secondary;
    std::optional<ScreenOverrides> cabinet;

    bool operator==(const RoomVideoOverrides&) const = default;
};

/// The ranges the values keep.
inline constexpr int kMinVideoRows = 144;
inline constexpr int kMaxVideoRows = 2160;
inline constexpr double kMinPreviewSeconds = 1.0;
inline constexpr double kMaxPreviewSeconds = 3600.0;
inline constexpr double kMaxProximityMeters = 100.0;

struct VideoSettings {
    bool enabled = true;
    int maxDecoders = 2;
    int maxRows = 480;
    int viewerMaxRows = 720;
    double proximityMeters = 12.0;
    CrtEffect crtEffect = CrtEffect::Light;
    AudioSettings audio;
    ScreenSettings hero;
    /// A secondary TV plays its own playlist, never the focused game (ADR-0016 §5).
    ScreenSettings secondary{.showOnFocus = false};
    ScreenSettings cabinet;
    /// By room id.
    std::map<std::string, RoomVideoOverrides, std::less<>> rooms;

    /// The settings of `kind`'s screen in room `roomId`: the kind's, with the room's keys over them.
    [[nodiscard]] ScreenSettings screen(ScreenKind kind, std::string_view roomId) const;

    bool operator==(const VideoSettings&) const = default;
};

/// Reads a whole video.json document (the defaults, or the defaults merged with the user's). Errors (context and JSON
/// pointer): ParseError for a key missing or of another type, an unknown key; ValidationFailed for a value out of its
/// range or an unknown name; Unsupported for another schemaVersion.
[[nodiscard]] Expected<VideoSettings> parseVideoSettings(const nlohmann::json& document, std::string_view context);

/// `base` with `overrides` merged in: members of objects present in both merged recursively, anything else replaced.
[[nodiscard]] nlohmann::json mergeVideoDocuments(nlohmann::json base, const nlohmann::json& overrides);

/// The settings the run plays with, and what was wrong with the user's file.
struct LoadedVideoSettings {
    VideoSettings settings;
    /// The user's file could not be read, parsed or checked: left out whole.
    std::vector<Error> issues;
};

/// assets/config/video.json with <userConfigDir>/video.json over it (a missing user file is no issue). Errors: the
/// defaults do not load (a broken installation).
[[nodiscard]] Expected<LoadedVideoSettings> loadVideoSettings(const std::filesystem::path& assetRoot,
                                                              const std::filesystem::path& userConfigDir);

} // namespace rv::app
