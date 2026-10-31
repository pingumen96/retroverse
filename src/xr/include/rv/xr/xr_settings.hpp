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
//                 "moveRelativeTo": "head"}   // "hand": the stick walks along the left controller (I10 step 4)
//   }

#include <cstdint>
#include <filesystem>
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

    bool operator==(const XrSettings&) const = default;
};

/// The settings of a merged document (every key present, none unknown); `context` names it in errors.
[[nodiscard]] Expected<XrSettings> parseXrSettings(const nlohmann::json& document, std::string_view context);

/// The settings, and what was wrong with the user's file (left out whole).
struct LoadedXrSettings {
    XrSettings settings;
    std::vector<Error> issues;
};

/// The shipped defaults under `assetRoot`/config, overridden by `userConfigDir`/xr.json when it exists. Errors: the
/// defaults cannot be read or checked.
[[nodiscard]] Expected<LoadedXrSettings> loadXrSettings(const std::filesystem::path& assetRoot,
                                                        const std::filesystem::path& userConfigDir);

} // namespace rv::xr
