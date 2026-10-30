#pragma once

// The CRT presets (I5 step 6, ADR-0016 §7, specification §9.6): the picture tube's looks as named sets of CrtEffect
// values, which the screens of the world take (app::ScreenLooks chooses one per screen). An effect at 0 is off.
//
// assets/materials/crt_presets.json (JSON with comments), the shipped defaults:
//   {"schemaVersion": 1,
//    "presets": {
//      "neutral":  {"lineCount": 240, "scanlineStrength": 0, "maskTriadCount": 320, "maskStrength": 0,
//                   "curvature": 0, "vignette": 0, "glassReflection": 0.25},   // the clean video
//      "consumer": {...},    // the consumer television: the look every CRT showed before the presets
//      "arcade":   {...},    // the arcade monitor: deeper scanlines, a stronger, coarser mask, more curvature
//      "monitor":  {...}}}   // the computer monitor: a fine mask, little curvature
// Every preset and every key of each (CrtEffect's seven, in its ranges) is required. The user's
// <user data>/config/crt_presets.json, read and never written, overrides them key by key (a preset's keys merged member
// by member over the shipped ones): {"schemaVersion": 1, "presets": {"arcade": {"scanlineStrength": 0.4}}}. A user
// file that cannot be read, parsed or checked is left out whole (an issue), the shipped values used; a shipped file
// that does not load leaves the built-in values (builtInCrtPresets(), a copy of the shipped file, unit-tested equal).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// The presets' file under assets/materials and <user data>/config.
inline constexpr std::string_view kCrtPresetsFile = "crt_presets.json";
inline constexpr int kCrtPresetsSchemaVersion = 1;

/// The four presets of specification §9.6 (Neutral, Consumer CRT, Arcade CRT, High quality monitor).
enum class CrtPreset : std::uint8_t {
    Neutral,
    Consumer,
    Arcade,
    Monitor,
};
inline constexpr std::size_t kCrtPresetCount = 4;
inline constexpr std::array<CrtPreset, kCrtPresetCount> kCrtPresets{CrtPreset::Neutral, CrtPreset::Consumer,
                                                                    CrtPreset::Arcade, CrtPreset::Monitor};

/// "neutral", "consumer", "arcade", "monitor".
[[nodiscard]] std::string_view toString(CrtPreset preset) noexcept;
/// The preset named `name` (toString()'s names, case-sensitive); none for any other text.
[[nodiscard]] std::optional<CrtPreset> crtPresetFromName(std::string_view name) noexcept;

/// The four presets' values.
struct CrtPresets {
    std::array<CrtEffect, kCrtPresetCount> effects{};

    [[nodiscard]] const CrtEffect& operator[](CrtPreset preset) const noexcept {
        return effects[static_cast<std::size_t>(preset)];
    }
    [[nodiscard]] CrtEffect& operator[](CrtPreset preset) noexcept { return effects[static_cast<std::size_t>(preset)]; }

    bool operator==(const CrtPresets&) const = default;
};

/// The shipped file's values, built in: what the screens take when the file does not load. Its consumer preset is
/// CrtEffect's defaults.
[[nodiscard]] const CrtPresets& builtInCrtPresets() noexcept;

/// One CrtEffect object: `base` with the object's keys over it ("lineCount", "scanlineStrength", "maskTriadCount",
/// "maskStrength", "curvature", "vignette", "glassReflection"; the ranges of CrtEffect), every key required when
/// `requireAll`. Errors (`context` and the key): ParseError for a value that is no number, an unknown or missing key;
/// OutOfRange for a number out of its range.
[[nodiscard]] Expected<CrtEffect> parseCrtEffect(const nlohmann::json& object, const CrtEffect& base, bool requireAll,
                                                 std::string_view context);

/// Reads a whole crt_presets.json document (the shipped one, or the shipped one with the user's merged over it).
/// Errors (context and JSON pointer): ParseError for a key missing or unknown, a value of another type; OutOfRange for
/// a value out of its range; Unsupported for another schemaVersion.
[[nodiscard]] Expected<CrtPresets> parseCrtPresets(const nlohmann::json& document, std::string_view context);

/// The presets the run draws with, where they come from, and what was wrong with the files.
struct LoadedCrtPresets {
    CrtPresets presets = builtInCrtPresets();
    /// "shipped" (assets/materials alone), "user" (the user's file over it), "built-in" (the shipped file did not
    /// load).
    std::string_view source = "built-in";
    /// The shipped file that did not load, or the user's file left out whole.
    std::vector<Error> issues;
};

/// <assetRoot>/materials/crt_presets.json with <userConfigDir>/crt_presets.json over it (a missing user file is no
/// issue). Never fails: see LoadedCrtPresets.
[[nodiscard]] LoadedCrtPresets loadCrtPresets(const std::filesystem::path& assetRoot,
                                              const std::filesystem::path& userConfigDir);

} // namespace rv::assets
