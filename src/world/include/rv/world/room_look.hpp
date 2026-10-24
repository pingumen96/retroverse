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
//    "environment": {"clearColor": "#221E1A", "ambientSky": "#E6E2DC", "ambientGround": "#B49C88",
//                    "ambientIntensity": 0.3, "fillColor": "#FFE0C8", "fillIntensity": 0.22, "exposure": 1.5,
//                    "bloomStrength": 0.18, "bloomRadius": 0.5, "vignette": 0.05,
//                    "ssao": {"enabled": true, "radius": 0.4, "intensity": 1.0}},   // the rooms' (and the scene's)
//    "hall": {                       // the halls' role (I6 step 1, ADR-0014 §4)
//      "light": {"color": "#FFD0A0", "intensity": 12, "range": 9},      // its ceiling lights (points)
//      "doorwayFill": {"color": "#FFD6AE", "intensity": 3.5, "range": 6, "minOuterConeDeg": 28, "coneMarginDeg": 6,
//                      "innerConeRatio": 0.7},   // a low spot in the hall before each doorway, aimed into the room
//      "environment": {"exposure": 1.5, "ambientSky": "#8A8078", "ambientGround": "#BCA08A",
//                      "ambientIntensity": 0.38, "fillColor": "#FFDCC0", "fillIntensity": 0.15},  // its area's terms
//      "lobbyEnvironment": {...}},   // optional, the same keys: the ground floor's lobby's (default: the hall's)
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
// A theme pack, assets/world/themes/<id>.json (the look of a room, resolved when its scene is built: building.hpp's
// themePackOf(), ADR-0014 §1, §5):
//   {"schemaVersion": 1, "id": "classic_16bit_warm", "name": "Classic16BitWarm",
//    "kinds": {"<prefab kind>": {"<material role>": "<library material>", ...}, ...},
//    "heroParts": {"<hero corner part role>": {"<material role>": "<library material>", ...}},
//    "labelPalette": ["#683D42", ...],      // the generated spines' and labels' grounds (I6 step 3), 2 to 16 colours
//    "neon": "#B34DCE",                     // the room's neon accent (I6 step 3)
//    "lightTints": {"picture": "#A8C0FF"},  // optional: the profile's roles tinted (I6 step 3)
//    "decorationWeights": {"plant": 1.0}}   // optional: the pack's decoration weights (I6 step 3)
//   A hero corner part listed under heroParts takes those materials instead of its kind's (the sign: its neon
//   lettering's material, "picture", glows in the pack's "neon" colour, unit-tested). The label palette replaces
//   assets::kDefaultLabelPalette for the room's generated spines and labels (classic_16bit_warm's is that palette).
//   "neon" is the colour of the profile's accent light anchored on the sign. "lightTints" multiply, in linear light,
//   the colours of the profile's roles in the pack's rooms ("key", "pendant", "lamp", "picture", "wallWasher") and
//   halls ("hall", the ceiling lights, and "doorwayFill"): an evening's tungsten a little cooler in the optical era, the
//   sign's picture light towards the pack's neon; a role not listed keeps the profile's colour (themedLighting()).
//   "decorationWeights" weigh the prefab kinds of the template's decoration choices for the layout candidates ADR-0014
//   defers; nothing reads them yet (a pack never moves a placement), the reader checks them (kinds as data ids,
//   weights from 0 to 1).
//   A shelf or wall shelf kind with the roles "board_light" (and, for the shelf, "board_glow") is drawn with its lit
//   interior (I6 step 3, geometry::ShelfParams::boardLight): a strip under each board and a glow on the back panel of
//   each compartment, emissive, no light of their own.
//   "cartridge" is the collection's item (plastic_shell, label, spine). "hall" (I6 step 2) is the floor's hall
//   (floor_scene.hpp), a kind of the pack's own whose roles name what the hall is built of: floor, wall (the plaster
//   above the wainscot), ceiling, trim (casings, lintels, the wainscot's rail and battens, the sconces' backplates, the
//   portal signs' frames), baseboard, wainscot (its panels), runner (the rug down its middle), beam, bench (the frame),
//   cushion, plant (the pot; the rest the "plant" kind's), saucer (the plant's), picture (the prints' frames; the
//   print the "poster_frame" kind's picture), sconce (the lit shade), wash (the sconce's light on its wall) and sign
//   (the portal and blade signs' lettering). A role the pack lacks is the built-in pack's (so is a role of the
//   "plant" and "poster_frame" kinds the hall uses).

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
/// doorway, its area's environment, and the ground floor's lobby's (the hall's when the profile names none).
struct HallLook {
    LightStyle light;
    LightStyle doorwayFill;
    AreaLook environment;
    AreaLook lobbyEnvironment;

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

/// A theme pack: its materials per material role of a prefab kind, and of the hero corner's parts that differ, its
/// label palette, neon accent, light tints and decoration weights (I6 step 3).
struct ThemeMaterials {
    std::string id;
    std::map<std::string, std::map<std::string, std::string>, std::less<>> kinds;
    std::map<std::string, std::map<std::string, std::string>, std::less<>> heroParts;
    /// The generated spines' and labels' palette, sRGB "#RRGGBB" (empty: assets::kDefaultLabelPalette).
    std::vector<std::string> labelPalette;
    /// The neon accent, sRGB "#RRGGBB": the colour of the profile's accent light on the sign.
    std::string neon = "#B34DCE";
    /// By role ("key", "pendant", "lamp", "picture", "wallWasher", "hall", "doorwayFill"): sRGB tints multiplied, in
    /// linear light, into the profile's colours.
    std::map<std::string, std::string, std::less<>> lightTints;
    /// By prefab kind, from 0 to 1 (read by nothing yet: the layout candidates are deferred).
    std::map<std::string, double, std::less<>> decorationWeights;

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

/// The theme pack ids under <assetRoot>/world/themes, sorted.
[[nodiscard]] std::vector<std::string> themePackIds(const std::filesystem::path& assetRoot);

/// `colour` (sRGB "#RRGGBB") multiplied in linear light by `tint` (sRGB "#RRGGBB"), back in sRGB "#RRGGBB" (uppercase,
/// rounded); `colour` itself when either is not such a colour.
[[nodiscard]] std::string tintedColour(std::string_view colour, std::string_view tint);

/// `profile` as a room of `theme` is lit (I6 step 3): its roles' colours (the rooms' and the halls') tinted by the
/// pack's lightTints, its accent lights anchored on the hero corner's sign in the pack's neon colour. Intensities,
/// ranges, cones and the environment unchanged; equal to `profile` for a pack without tints whose neon is the sign
/// accents' colour (classic_16bit_warm under evening).
[[nodiscard]] LightingProfile themedLighting(const LightingProfile& profile, const ThemeMaterials& theme);

} // namespace rv::world
