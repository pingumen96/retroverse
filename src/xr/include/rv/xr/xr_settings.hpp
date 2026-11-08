#pragma once

// xr.json (I10 step 3, ADR-0017 §8): the VR mode's settings. The shipped defaults are assets/config/xr.json (JSON with
// comments, every key); the user's <user data>/config/xr.json, read and never written, overrides them key by key
// (objects merged member by member, any other value replaced). A user file that cannot be read, parsed or checked is
// left out whole, the defaults used (an issue logged), as video.json is.
//
//   {
//     "schemaVersion": 1,
//     "display": {"mode": "desktop"},   // "vr": every run starts in the headset, as with --vr
//     "renderScale": 1,                 // 0.25 to 2: the eyes' size over the runtime's recommendation
//     "refreshRate": 0,                 // Hz asked of the headset (XR_FB_display_refresh_rate); 0: the runtime's own
//     "posture": "standing",            // "seated": the head made up to the walker's eye height at each recentering
//     "quality": "auto",                // "full", "fast" (no SSAO, no FXAA), "auto": fast from 110 Hz (I10 step 9)
//     "comfort": {"turn": "snap", "snapDegrees": 30, "smoothDegreesPerSecond": 90, "vignette": 0,
//                 "moveRelativeTo": "head"},  // "hand": the stick walks along the left controller (I10 step 4)
//     "gameScreen": "crt"               // "large": a launched game on the large screen even beside a CRT (I11 step 14)
//   }

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/xr/locomotion.hpp>

namespace rv::xr {

/// The file's name under assets/config and <user data>/config.
inline constexpr std::string_view kXrSettingsFile = "xr.json";
inline constexpr int kXrSettingsSchemaVersion = 1;

/// The eyes' rendering (I10 step 9): the desktop's, or "fast" without the ambient occlusion and the anti-aliasing (about
/// a third of an eye's GPU time at Link's size), which "auto" takes from kFastFromHz.
enum class XrQuality : std::uint8_t { Auto, Full, Fast };

/// "auto", "full" or "fast".
[[nodiscard]] std::string_view toString(XrQuality quality) noexcept;

/// Auto's fast profile from this refresh rate on (Hz): 120 Hz, not 90.
inline constexpr float kFastFromHz = 110.0f;

/// The profile of `quality` at `refreshHz`: true for fast.
[[nodiscard]] bool fastProfile(XrQuality quality, float refreshHz) noexcept;

/// Where a launched game shows in the headset (I11 step 14, ADR-0020 §6): on the room's hero CRT, the large screen
/// where none is bound; or always on the large screen.
enum class GameScreen : std::uint8_t { Crt, Large };
/// "crt" or "large".
[[nodiscard]] std::string_view toString(GameScreen screen) noexcept;

struct XrSettings {
    static constexpr double kMinRenderScale = 0.25;
    static constexpr double kMaxRenderScale = 2.0;
    /// The refresh rates a headset may be asked for, Hz (0: the runtime's own).
    static constexpr double kMaxRefreshRate = 240.0;

    /// display.mode "vr": the run starts in the headset without --vr.
    bool vr = false;
    double renderScale = 1.0;
    /// 0: the runtime's own.
    float refreshRate = 0.0f;
    Posture posture = Posture::Standing;
    XrQuality quality = XrQuality::Auto;
    ComfortSettings comfort;
    GameScreen gameScreen = GameScreen::Crt;

    bool operator==(const XrSettings&) const = default;
};

/// The settings of a merged document (every key present, none unknown); `context` names it in errors.
[[nodiscard]] Expected<XrSettings> parseXrSettings(const nlohmann::json& document, std::string_view context);

/// The settings, and what was wrong with the user's file (left out whole).
struct LoadedXrSettings {
    XrSettings settings;
    std::vector<Error> issues;
    /// The document under settings.json: the defaults with the user's file over them ("Ripristina"'s values).
    nlohmann::json belowSettings;
    /// Keys of settings.json whose value parseXrSettings() refused: left out, each alone.
    std::vector<std::string> refusedKeys;
};

/// settings.json's section for this file and the keys the settings screen may set there (ADR-0018 §2, §3).
inline constexpr std::string_view kXrSettingsSection = "xr";
[[nodiscard]] std::span<const std::string_view> xrSettingKeys() noexcept;

/// The shipped defaults under `assetRoot`/config, overridden by `userConfigDir`/xr.json when it exists, with the keys of
/// settings.json beside that folder over both. Errors: the defaults cannot be read or checked.
[[nodiscard]] Expected<LoadedXrSettings> loadXrSettings(const std::filesystem::path& assetRoot,
                                                        const std::filesystem::path& userConfigDir);

} // namespace rv::xr
