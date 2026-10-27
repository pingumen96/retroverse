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
//    "labelPalette": ["#683D42", ...],      // optional: the generated spines' and labels' grounds, 2 to 16 colours
//    "neon": "#B34DCE",                     // optional: the room's neon accent
//    "neonGlow": {"intensity": 4, "range": 3.5},  // optional: the neon's glow on the wall
//    "bloomStrength": 0.28,                 // optional: the room's bloom
//    "lightTints": {"key": "#E6EEFF"},      // optional: the profile's roles tinted
//    "furniture": {...}}                    // optional: the pack's furniture variants (I6 step 3b, below)
//   (all of them I6 step 3; a pack leaving one out takes classic_16bit_warm's: its palette, its magenta, the profile's
//   glow and bloom, no tints, no variants). A hero corner part listed under heroParts takes those materials instead of
//   its kind's (the sign: its neon lettering's material, "picture", glows in the pack's "neon" colour, unit-tested).
//   The label palette replaces assets::kDefaultLabelPalette for the room's generated spines and labels
//   (classic_16bit_warm's is that palette). "neon" colours the light on the sign (themedLighting()): the profile's
//   accent light anchored on it ("sign_glow"), and its picture light, whose colour is the profile's times the neon
//   brought to full brightness and mixed with white (kSignLightNeonShare), so no pack tints it by hand. "neonGlow" sets
//   that accent's intensity and range (the profile's when left out), "bloomStrength" the bloom of the pack's rooms (the
//   profile's when left out; the room's area's in a floor's scene, eased like its other terms). "lightTints" multiply,
//   in linear light, the colours of the profile's roles in the pack's rooms ("key", "pendant", "lamp", "picture",
//   "wallWasher") and halls ("hall", the ceiling lights, and "doorwayFill"): an evening's tungsten cooler in the
//   optical era, magenta and cyan in the arcade; a role not listed keeps the profile's colour.
//   "furniture" (I6 step 3b, ADR-0014 §1 amended: rooms that differ by more than colour) draws the hero corner's parts
//   and the decoration as the pack's era has them, within their footprints; the plan's fixtures, slots and lights stay
//   as they are (the scene builder, scene_builder.hpp, places the pieces):
//     "furniture": {
//       "heroParts": {"armchair": [<piece>, ...], "lamp": [<piece>], "rug": [<piece>]},  // by the part's role
//                                                    // (never "sign" or "console": the reader refuses them; "crt"
//                                                    // only as arcade cabinets, I6 step 6, below)
//       "decoration": {"poster_frame": [{"weight": 0.5, <piece>}, ...], ...}}          // by the decoration's kind
//   A piece: {"prefab": {"manifest": "furniture.sofa.generic", "kind": "sofa"}, "params": {...} (merged over the
//   manifest's), "materials": {"<role>": "MAT_..."} (over the pack's kind's), "textures": {"<role>": "<pattern>"}
//   (a scene pattern, scene::kTexturePatternNames, drawn with an invented title and the piece's seed), and for a hero
//   part's piece "xMm", "zMm", "yawCdeg" (in the hero corner socket's frame, as the template's parts; the part's own
//   when left out) and "yMm" (I6 step 6: its height above the room's floor; the part's when left out)}. A hero part's
//   list replaces the part by its pieces (one or more: a desk and its chair, two stools), or, empty (I6 step 6), leaves
//   the part undrawn: another part's piece stands in its place (the arcade's hero cabinet on the floor where the TV's
//   cabinet stood; the reader refuses an empty list without "crt" pieces, and the scene draws the part as planned
//   whenever the CRT is not drawn as the pack's cabinet, I6 step 6's review). The "crt" (the room's video) is listed
//   only as arcade cabinets ("arcade_cabinet" pieces, the first carrying a ScreenSurface: the room's video plays on
//   it; one that does not is left out, the CRT drawn as planned): the hero cabinet of ADR-0014 §6, a part substitution by the arcade room's pack, whose kind it is. Both the "crt"'s
//   pieces and a part left out apply in the arcade room only (its hero corner holds no console): another room drawn
//   with the pack keeps its CRT, TV cabinet and console as planned; a decoration
//   kind's list is a weighted choice (weights from 0 to 1, one at least above 0: the share of
//   that kind's sockets each piece takes, drawn from the socket's seed), each piece standing against its socket's back
//   edge, centred across it, at the plan's mount height. These replace step 3's "decorationWeights", which nothing
//   read: the weights are the variants' (ThemeFurniture). A piece that leaves its footprint where it is drawn is not
//   drawn (scene_builder.hpp: the plan's part or decoration is, with a warning). A showcase kind with "board_light"
//   draws its strip under the cap (geometry::ShowcaseParams::boardLight).
//   A shelf kind with the roles "board_light" and "board_glow" (both or neither; the reader refuses one alone), or a
//   wall shelf kind with "board_light" (a wall shelf has no glow), is drawn with its lit interior (I6 step 3,
//   geometry::ShelfParams::boardLight): a strip under each board and, on a shelf with a back, a glow on the back panel
//   of each compartment, emissive, no light of their own.
//   "cartridge" is the collection's item (plastic_shell, label, spine). "hall" (I6 step 2) is the floor's hall
//   (floor_scene.hpp), a kind of the pack's own whose roles name what the hall is built of: floor, wall (the plaster
//   above the wainscot), ceiling, trim (casings, lintels, the wainscot's rail and battens, the sconces' backplates, the
//   portal signs' frames), baseboard, wainscot (its panels), runner (the rug down its middle), beam, bench (the frame),
//   cushion, plant (the pot; the rest the "plant" kind's), saucer (the plant's), picture (the prints' frames; the
//   print the "poster_frame" kind's picture), sconce (the lit shade), wash (the sconce's light on its wall) and sign
//   (the portal and blade signs' lettering). A role the pack lacks is the built-in pack's (so is a role of the
//   "plant" and "poster_frame" kinds the hall uses).
//   Since I6 step 6's review, optional: "accents": [<accent>, ...], the profile's accent format (lighting profiles),
//   with "colorAlt" (a display accent's colour on every other display fixture), drawn after the profile's; and
//   "roomDressing": {"cove": {"yMm", "west", "east", "westWash", "eastWash"} (a lit strip along the west and east
//   walls, materials, and optionally the light each throws on its wall above and below it),
//   "pictures": {"centreYMm", "widthMm", "heightMm", "roles": ["hero-module", ...], "kinds": ["tv_cabinet", ...],
//   "frame", "picture", "pattern"} (a framed picture high on the wall over each fixture of those roles, or decoration
//   of those kinds, standing against a wall, its seed from the room's and the fixture's), "cans": {"roles": ["key",
//   ...], "body", "lens"} (a can over each spot light of those roles, hung from the ceiling, its lens lit) and
//   "runner": {"widthMm", "lengthMm", "material"} (a runner from the hero corner's rug's back edge out into the room,
//   centred on it: a pack leaving the hero's "rug" out draws it alone), "signIcon": {"widthMm",
//   "heightMm", "pattern", "material"} (a lit pictogram on the hero corner's sign's right, framed as the sign),
//   "cardPerSlot": true (a display case's empty level a riser card per slot, the item's size)}: a look
//   of the room's upper walls, ceiling and floor, never colliding nor casting shadows (scene_builder.hpp).

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
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
    /// A display accent's colour on every other display fixture (the second, fourth, ... the room lists), sRGB
    /// "#RRGGBB" (I6 step 6's review: the arcade's pairs lit cyan and red in turn); empty: `color` on every one.
    std::string colorAlt;
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

/// The neon's glow on the wall (a theme pack's "neonGlow"): the sign_glow accent's intensity and range.
struct NeonGlow {
    double intensity = 1.5;
    double range = 2.5;

    bool operator==(const NeonGlow&) const = default;
};

/// A lit strip along a room's west and east walls (a pack's "roomDressing" "cove", I6 step 6's review): `yMm` above
/// the floor, of material `west` on the room's west wall (its -X side in its own frame) and `east` on its east wall.
struct CoveStrip {
    std::int64_t yMm = 2600;
    std::string west;
    std::string east;
    /// The light each strip throws on its wall, up to the ceiling and down below it (optional, both or neither): an
    /// unlit, alpha-blended material whose picture fades away from its bottom edge (procedural:shelfglow).
    std::string westWash;
    std::string eastWash;

    bool operator==(const CoveStrip&) const = default;
};

/// Pictures hung high on the walls over some fixtures (a pack's "roomDressing" "pictures", I6 step 6's review).
struct WallPictures {
    std::int64_t centreYMm = 2200;
    std::int64_t widthMm = 500;
    std::int64_t heightMm = 600;
    /// Over the fixtures of these roles standing against a wall...
    std::vector<FixtureRole> roles;
    /// ... and over the decorations of these prefab kinds.
    std::vector<std::string> kinds;
    std::string frame;   ///< the frame's material
    std::string picture; ///< the picture's material
    std::string pattern; ///< a scene texture pattern (scene::kTexturePatternNames)

    bool operator==(const WallPictures&) const = default;
};

/// Visible cans over the spot lights of some roles (a pack's "roomDressing" "cans", I6 step 6's review).
struct SpotCans {
    std::vector<LightRole> roles;
    std::string body; ///< the can's and its stem's material
    std::string lens; ///< the lit lens under it

    bool operator==(const SpotCans&) const = default;
};

/// A runner on the floor leading to the hero corner (a pack's "roomDressing" "runner", I6 step 6's review): from the
/// back edge of the hero corner's rug (the plan's part) out into the room along it, centred on it.
struct HeroRunner {
    std::int64_t widthMm = 900;
    std::int64_t lengthMm = 3500;
    std::string material;

    bool operator==(const HeroRunner&) const = default;
};

/// A lit pictogram beside the hero corner's sign (a pack's "roomDressing" "signIcon", I6 step 6's review): a picture of
/// `pattern` in `material` (a neon), framed as the sign, on the sign's right on its wall.
struct SignIcon {
    std::int64_t widthMm = 400;
    std::int64_t heightMm = 300;
    std::string pattern;
    std::string material;

    bool operator==(const SignIcon&) const = default;
};

/// What a pack adds to its rooms' upper walls, ceiling and floor beyond the plan's fixtures (I6 step 6's review): a
/// look, never the plan's: nothing collides, nothing casts a shadow.
struct RoomDressing {
    std::optional<CoveStrip> cove;
    std::optional<WallPictures> pictures;
    std::optional<SpotCans> cans;
    std::optional<HeroRunner> runner;
    std::optional<SignIcon> signIcon;
    /// A display case's empty level shows a riser card in each slot rather than one across the span.
    bool cardPerSlot = false;

    [[nodiscard]] bool empty() const noexcept {
        return !cove && !pictures && !cans && !runner && !signIcon && !cardPerSlot;
    }
    bool operator==(const RoomDressing&) const = default;
};

/// The share of the pack's neon in its sign's picture light (themedLighting()).
inline constexpr double kSignLightNeonShare = 0.45;

/// A piece of furniture a pack draws in place of a hero corner's part or a socket's decoration (I6 step 3b, ADR-0014
/// §1 amended): its prefab, parameters over the manifest's, materials over the pack's kind's, picture patterns by
/// role, and (a hero part's piece only) where it stands in the hero corner socket's frame (the part's when left out).
struct FurniturePiece {
    PrefabRef prefab;
    nlohmann::json params = nlohmann::json::object();
    std::map<std::string, std::string> materials;
    /// By material role: a scene texture pattern (scene::kTexturePatternNames).
    std::map<std::string, std::string> textures;
    std::optional<std::int64_t> xMm;
    std::optional<std::int64_t> zMm;
    std::optional<std::int32_t> yawCdeg;
    /// A hero part's piece: its height above the room's floor (I6 step 6); the part's when left out.
    std::optional<std::int64_t> yMm;

    bool operator==(const FurniturePiece&) const = default;
};

/// One of a decoration kind's variants and its weight, in thousandths (the reader's weight from 0 to 1, rounded).
struct FurnitureVariant {
    std::uint32_t weightPermille = 1000;
    FurniturePiece piece;

    bool operator==(const FurnitureVariant&) const = default;
};

/// A pack's furniture variants: the pieces that replace a hero corner's part (by its role; none: the part is not drawn,
/// I6 step 6), and the weighted variants of a decoration (by its prefab kind).
struct ThemeFurniture {
    std::map<std::string, std::vector<FurniturePiece>, std::less<>> heroParts;
    std::map<std::string, std::vector<FurnitureVariant>, std::less<>> decoration;

    [[nodiscard]] bool empty() const noexcept { return heroParts.empty() && decoration.empty(); }
    bool operator==(const ThemeFurniture&) const = default;
};

/// A theme pack: its materials per material role of a prefab kind, and of the hero corner's parts that differ, its
/// label palette, neon accent and glow, bloom, light tints (I6 step 3) and furniture variants (I6 step 3b).
struct ThemeMaterials {
    std::string id;
    std::map<std::string, std::map<std::string, std::string>, std::less<>> kinds;
    std::map<std::string, std::map<std::string, std::string>, std::less<>> heroParts;
    /// The generated spines' and labels' palette, sRGB "#RRGGBB" (empty: assets::kDefaultLabelPalette).
    std::vector<std::string> labelPalette;
    /// The neon accent, sRGB "#RRGGBB": the colour of the profile's accent light on the sign and of its picture light.
    std::string neon = "#B34DCE";
    /// The neon's glow on the wall; none: the profile's.
    std::optional<NeonGlow> neonGlow;
    /// The bloom of its rooms, [0, 1]; none: the profile's.
    std::optional<double> bloomStrength;
    /// By role ("key", "pendant", "lamp", "picture", "wallWasher", "hall", "doorwayFill"): sRGB tints multiplied, in
    /// linear light, into the profile's colours.
    std::map<std::string, std::string, std::less<>> lightTints;
    /// The pack's furniture variants (I6 step 3b): what draws the hero corner's parts and the decoration.
    ThemeFurniture furniture;
    /// The pack's accent lights (I6 step 6's review), drawn after the profile's while the room's lights leave room.
    std::vector<AccentLight> accents;
    /// The pack's dressing of its rooms' upper walls, ceiling and floor (I6 step 6's review).
    RoomDressing dressing;

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
/// pack's lightTints; its sign's picture light the profile's times signLightTint() of the pack's neon (then its
/// "picture" tint, if any); its accent lights anchored on the hero corner's sign in the pack's neon colour, with its
/// neonGlow's intensity and range; its bloom the pack's bloomStrength. Other intensities, ranges, cones and the
/// environment unchanged.
[[nodiscard]] LightingProfile themedLighting(const LightingProfile& profile, const ThemeMaterials& theme);

/// The tint of a sign's picture light from its neon (sRGB "#RRGGBB"): the neon scaled to its brightest channel, mixed
/// with white by kSignLightNeonShare, in sRGB; white for a colour that is not such a colour or black.
[[nodiscard]] std::string signLightTint(std::string_view neon);

} // namespace rv::world
