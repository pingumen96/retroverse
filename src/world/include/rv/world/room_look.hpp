#pragma once

// How a room looks when it is drawn (I3 step 16, ADR-0010 §4, style §4.2, §8.1, §14): the lighting profile and the
// theme pack's materials, data under the asset root, never stored in the plan, so that switching a profile moves
// nothing.
//
// A lighting profile, assets/world/lighting/<id>.json (JSON with comments; every key required unless marked):
//   {"schemaVersion": 1, "id": "evening", "name": "Evening",
//    "roles": {                      // the plan's light roles: colour, intensity and range of each, a spot's cones
//      "key": {"color": "#FFE0C0", "intensity": 70, "range": 12,
//              "minOuterConeDeg": 30, "coneMarginDeg": 3, "innerConeRatio": 0.55},   // cones: spots only
//      "pendant": {...}, "lamp": {...}, "picture": {...}, "wallWasher": {...}},
//    "keyShadowStrength": 1.0,
//    "environment": {"clearColor": "#221E1A", "ambientSky": "#E6E2DC", "ambientGround": "#4A4642",
//                    "ambientIntensity": 0.25, "fillColor": "#FFD8B0", "fillIntensity": 0.05, "exposure": 1.5,
//                    "bloomStrength": 0.18, "bloomRadius": 0.5, "vignette": 0.05,
//                    "ssao": {"enabled": true, "radius": 0.4, "intensity": 1.0}},   // the rooms' (and the scene's)
//    "hall": {                       // the halls' role (I6 step 1, ADR-0014 §4)
//      "light": {"color": "#FFD2A0", "intensity": 10, "range": 8},      // its ceiling lights (points)
//      "doorwayFill": {"color": ..., "intensity": ..., "range": ..., "minOuterConeDeg": ..., "coneMarginDeg": ...,
//                      "innerConeRatio": ...},   // a low spot in the hall before each doorway, aimed into the room
//      "environment": {"exposure": 1.5, "ambientSky": ..., "ambientGround": ..., "ambientIntensity": ...,
//                      "fillColor": ..., "fillIntensity": ...}},  // the hall's area's own lit terms
//    "accents": [                    // optional: lights of the look alone, anchored on a hero corner's part
//      {"name": "crt-glow", "anchor": "crt", "type": "spot", "position": [0, 0.35, 0.4], "target": [0, 0, 1.4],
//       "color": "#D2D8E4", "intensity": 2.5, "range": 3.5, "innerConeDeg": 45, "outerConeDeg": 80},
//      {"name": "case-light", "on": "display", "anchor": "display_case", ...}]}   // one per display case
//   "on" (optional) is "hero-corner" (the default: "anchor" names a hero corner part's role) or "display" ("anchor"
//   names a prefab kind, and the accent is drawn on every display fixture of that kind, in the plan's fixture order).
//   An accent's position and target are in its anchor's frame (meters: the part's or the fixture's pivot, turned with
//   it); it is drawn only while the room's lights leave room for it under the scene's limit (16), in the order listed,
//   and never casts shadows. Colours are sRGB "#RRGGBB" (uppercase hexadecimal).
//
// A theme pack, assets/world/themes/<id>.json (the platform manifest's themePack):
//   {"schemaVersion": 1, "id": "classic_16bit_warm", "name": "Classic16BitWarm",
//    "kinds": {"<prefab kind>": {"<material role>": "<library material>", ...}, ...},
//    "heroParts": {"<hero corner part role>": {"<material role>": "<library material>", ...}}}
//   A hero corner part listed under heroParts takes those materials instead of its kind's (the sign's diffuser).
//   "cartridge" is the collection's item (plastic_shell, label, spine).

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/expected.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// How the lights of one role look: the plan places them, the profile gives them colour, intensity, range and cones.
struct LightStyle {
    /// sRGB "#RRGGBB".
    std::string color = "#FFE0C0";
    double intensity = 1.0;
    /// Meters, > 0.
    double range = 5.0;
    /// Spots only: the outer cone is the half angle the span subtends at the target plus `coneMarginDeg`, at least
    /// `minOuterConeDeg`, at most kMaxOuterConeDeg; the inner one `innerConeRatio` of it.
    double minOuterConeDeg = 30.0;
    double coneMarginDeg = 4.0;
    double innerConeRatio = 0.55;

    bool operator==(const LightStyle&) const = default;
};

/// The widest outer cone a profile gives a spot (the scene format takes less than 90 degrees).
inline constexpr double kMaxOuterConeDeg = 80.0;

/// What an accent light is anchored on.
enum class AccentAnchor : std::uint8_t {
    HeroCorner, ///< the hero corner's part whose role the anchor names (one light)
    Display,    ///< every display fixture whose prefab kind the anchor names (one light each)
};

/// A light of the look alone (no plan light): anchored on a hero corner's part or on display fixtures, in their frame.
struct AccentLight {
    std::string name;
    AccentAnchor on = AccentAnchor::HeroCorner;
    /// The hero corner part's role ("crt", "sign", ...) or the display fixtures' prefab kind ("display_case",
    /// "wall_shelf"); an accent whose anchor the room lacks is left out.
    std::string anchor;
    bool spot = true;
    glm::dvec3 position{0.0};
    /// Spots only: where it points, in the anchor's frame.
    glm::dvec3 target{0.0, 0.0, 1.0};
    std::string color = "#FFFFFF";
    double intensity = 1.0;
    double range = 3.0;
    double innerConeDeg = 30.0;
    double outerConeDeg = 45.0;

    bool operator==(const AccentLight&) const = default;
};

/// The lit terms of an area (scene_description.hpp's AreaEnvironment), colours as sRGB "#RRGGBB".
struct AreaLook {
    double exposure = 1.5;
    std::string ambientSky = "#E6E2DC";
    std::string ambientGround = "#4A4642";
    double ambientIntensity = 0.25;
    std::string fillColor = "#FFD8B0";
    double fillIntensity = 0.0;

    bool operator==(const AreaLook&) const = default;
};

/// How a floor's hall is lit (the profile's "hall" role, I6 step 1): its ceiling lights, the fill spot before each
/// doorway, its area's environment.
struct HallLook {
    LightStyle light;
    LightStyle doorwayFill;
    AreaLook environment;

    bool operator==(const HallLook&) const = default;
};

struct LightingProfile {
    std::string name;
    LightStyle key;
    LightStyle pendant;
    LightStyle lamp;
    LightStyle picture;
    LightStyle wallWasher;
    /// The key light's shadowStrength, [0, 1].
    double keyShadowStrength = 1.0;
    /// The scene's environment (scene_description.hpp's keys): colours as sRGB "#RRGGBB".
    std::string clearColor = "#221E1A";
    std::string ambientSky = "#E6E2DC";
    std::string ambientGround = "#4A4642";
    double ambientIntensity = 0.25;
    std::string fillColor = "#FFD8B0";
    double fillIntensity = 0.0;
    double exposure = 1.5;
    double bloomStrength = 0.18;
    double bloomRadius = 0.5;
    double vignette = 0.05;
    bool ssaoEnabled = true;
    double ssaoRadius = 0.4;
    double ssaoIntensity = 1.0;
    std::vector<AccentLight> accents;
    /// The halls' lights and environment.
    HallLook hall;

    [[nodiscard]] const LightStyle& style(LightRole role) const noexcept;
    /// The rooms' lit terms (the environment's exposure, ambient and fill).
    [[nodiscard]] AreaLook roomLook() const;

    bool operator==(const LightingProfile&) const = default;
};

/// The built-in profile (I3 step 10a's evening, from the test room's lights): what a room is drawn with when no
/// profile file loads (logged), and the empty room's light before the data is read.
[[nodiscard]] const LightingProfile& eveningLightingProfile();

/// Reads a lighting profile. Errors (context and line): ParseError, ValidationFailed (a key missing, unknown or out of
/// range, a schemaVersion other than 1).
[[nodiscard]] Expected<LightingProfile> parseLightingProfile(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/lighting/<id>.json, whose "id" must be `id`. Errors: those of parseLightingProfile(), of
/// reading the file (NotFound, ...), InvalidArgument for an id that is not a data id.
[[nodiscard]] Expected<LightingProfile> loadLightingProfile(const std::filesystem::path& assetRoot, std::string_view id);

/// The profiles under <assetRoot>/world/lighting, by id, sorted.
[[nodiscard]] std::vector<std::string> lightingProfileIds(const std::filesystem::path& assetRoot);

/// The profile a room of this installation is drawn with: the one world.json ("lightingProfile") names under
/// `userConfigDir`, else kDefaultLightingProfile; a profile that does not load falls back to the default one, then to
/// eveningLightingProfile(), each fallback logged as a warning. Never fails.
[[nodiscard]] LightingProfile roomLightingProfile(const std::filesystem::path& assetRoot,
                                                  const std::filesystem::path& userConfigDir);

/// A theme pack's materials per material role of a prefab kind, and of the hero corner's parts that differ.
struct ThemeMaterials {
    std::string id;
    std::map<std::string, std::map<std::string, std::string>, std::less<>> kinds;
    std::map<std::string, std::map<std::string, std::string>, std::less<>> heroParts;

    /// The materials of `kind`, those of hero corner part `heroPart` when the theme lists that part (empty: none);
    /// empty for a kind the theme does not dress.
    [[nodiscard]] std::map<std::string, std::string> materials(std::string_view kind,
                                                               std::string_view heroPart = {}) const;

    bool operator==(const ThemeMaterials&) const = default;
};

/// Reads a theme pack. Errors (context and line): ParseError, ValidationFailed (a key missing or unknown, a material
/// name that is not MAT_<letters, digits, _>, a schemaVersion other than 1).
[[nodiscard]] Expected<ThemeMaterials> parseThemeMaterials(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/themes/<id>.json, whose "id" must be `id`.
[[nodiscard]] Expected<ThemeMaterials> loadThemeMaterials(const std::filesystem::path& assetRoot, std::string_view id);

/// The theme pack classic_16bit_warm as built in, a copy of its file (unit-tested equal): what a room is drawn with
/// when its theme file does not load (logged), and the empty room's shell before the data is read.
[[nodiscard]] const ThemeMaterials& builtInThemeMaterials();

/// builtInThemeMaterials().materials(kind): the collection's item ("cartridge") for the app's collection instance.
[[nodiscard]] std::map<std::string, std::string> themeMaterials(std::string_view kind);

} // namespace rv::world
