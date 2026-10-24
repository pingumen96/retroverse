#include <rv/world/floor_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/seed.hpp>

namespace rv::world {
namespace {

using nlohmann::json;

/// The walker's eye above the floor and the camera's pitch (scene_builder.cpp's doorway camera's).
constexpr double kEyeHeight = 1.65;
constexpr double kCameraPitchDeg = -6.0;
/// Facing east down the hall (yaw 0 looks north, positive yaw turns right).
constexpr double kFacingEastDeg = 90.0;
/// The far plane beyond the floor's diagonal, and the least it may be (a room's camera's).
constexpr double kFarMarginM = 10.0;
constexpr double kMinFarM = 100.0;
/// The hall's surfaces: the procedural textures repeat every 2 m (the room shell's scale), the floor and ceiling are
/// slabs this thick outside the interior; wood in meters along each piece (the prefabs' grain, 1 m per repeat).
constexpr double kMetersPerTile = 2.0;
constexpr double kWoodMetersPerTile = 1.0;
constexpr std::int64_t kSlabMm = 20;
/// The hall's fixed materials (library names): the elevator's graphite frame and brass doors, a black call panel
/// with a lit button, the lamps' shades, the blade signs' graphite arms, the lobby's walnut and varnished entrance. The
/// rest is the theme pack's "hall" kind (room_look.hpp).
constexpr std::string_view kElevatorFrameMaterial = "MAT_Metal_Graphite";
constexpr std::string_view kElevatorDoorMaterial = "MAT_Metal_Brass";
constexpr std::string_view kBladeArmMaterial = "MAT_Metal_Graphite";
constexpr std::string_view kPanelMaterial = "MAT_Plastic_Black";
constexpr std::string_view kButtonMaterial = "MAT_Screen_Emissive";
constexpr std::string_view kLampMaterial = "MAT_Lamp_Shade";
constexpr std::string_view kEntranceFrameMaterial = "MAT_Wood_Walnut";
constexpr std::string_view kEntranceDoorMaterial = "MAT_Wood_Varnished";
/// The lamps of the hall's lights (a drum pendant just above each light, its bulb at the drum's foot, on a stem from
/// the ceiling: high enough that from down the hall it never hides the floor's sign over the elevator) and the
/// elevator's call panel and button, millimetres.
constexpr std::int64_t kLampRadiusMm = 200;
constexpr std::int64_t kLampHeightMm = 200;
constexpr std::int64_t kLampAboveLightMm = 120;
constexpr int kLampSegments = 32;
constexpr std::int64_t kLampStemMm = 30;
constexpr std::int64_t kDoorGapMm = 10;
constexpr std::int64_t kDoorLeafDepthMm = 20;
constexpr std::int64_t kPanelWidthMm = 140;
constexpr std::int64_t kPanelHeightMm = 300;
constexpr std::int64_t kPanelDepthMm = 20;
constexpr std::int64_t kPanelBottomMm = 1000;
constexpr std::int64_t kPanelGapMm = 150;
constexpr std::int64_t kButtonMm = 40;
/// The floor's sign above the elevator: the rooms' sign panel (decor.sign.room.generic's parameters), its centre this
/// high on the west wall.
constexpr double kSignWidthM = 1.6;
constexpr double kSignHeightM = 0.4;
constexpr double kSignFrameM = 0.025;
constexpr double kSignDepthM = 0.04;
constexpr std::int64_t kSignCentreMm = 2750;
/// The fill spot before each doorway (I6 step 1): this far into the hall from its edge, this far below the doorway's
/// head, aimed at a point this far into the room at this height (the cases and the hero wall at eye height rather than
/// the floor).
constexpr std::int64_t kDoorwayFillSetbackMm = 900;
constexpr std::int64_t kDoorwayFillBelowHeadMm = 300;
constexpr std::int64_t kDoorwayFillReachMm = 4000;
constexpr std::int64_t kDoorwayFillTargetHeightMm = 1200;
/// The hall's trim (I6 step 2), like the rooms' (geometry's room_shell): a baseboard, a panelled wainscot (panels under
/// battens at a fixed pitch from the floor's origin, a rail on top), the plaster above it as a thin skin over the
/// rooms' walls as over the hall's own, a crown moulding under the ceiling; heights from the floor and depths from the
/// wall's face.
constexpr std::int64_t kBaseboardHeightMm = 100;
constexpr std::int64_t kBaseboardDepthMm = 15;
constexpr std::int64_t kWainscotTopMm = 1000;
constexpr std::int64_t kRailHeightMm = 35;
constexpr std::int64_t kRailDepthMm = 25;
constexpr std::int64_t kWainscotDepthMm = 10;
constexpr std::int64_t kBattenWidthMm = 40;
constexpr std::int64_t kBattenDepthMm = 18;
constexpr std::int64_t kBattenPitchMm = 600;
constexpr std::int64_t kSkinDepthMm = 5;
constexpr std::int64_t kCrownHeightMm = 70;
constexpr std::int64_t kCrownDepthMm = 40;
/// A doorway's portal on the hall's side: a casing around the opening, a lintel over it, the room's sign above the
/// lintel (the rooms' sign panel, smaller), a beam across the hall's ceiling over the doorway.
constexpr std::int64_t kCasingWidthMm = 100;
constexpr std::int64_t kCasingDepthMm = 25;
constexpr std::int64_t kLintelHeightMm = 50;
constexpr std::int64_t kLintelDepthMm = 45;
constexpr std::int64_t kLintelOverhangMm = 40;
constexpr double kPortalSignWidthM = 1.4;
constexpr double kPortalSignHeightM = 0.34;
constexpr double kPortalSignFrameM = 0.03;
constexpr double kPortalSignDepthM = 0.04;
constexpr std::int64_t kBeamWidthMm = 200;
constexpr std::int64_t kBeamDepthMm = 220;
/// The beams across the ceiling: one every kBeamPitchMm from the west end (x = pitch, 2 pitch, ... while
/// kBeamEndClearMm short of the east end: a longer hall appends beams), and one over each doorway that no beam of that
/// grid passes within kBeamMergeMm of (the grid's beam serves it).
constexpr std::int64_t kBeamPitchMm = 3000;
constexpr std::int64_t kBeamEndClearMm = 1000;
constexpr std::int64_t kBeamMergeMm = 750;
/// A blade sign beside each portal (the room's sign again, readable down the hall): a double-sided panel this wide and
/// high, centred this high and this far out from the wall, kBladeOffsetMm from the doorway's centre towards the
/// elevator, hanging from a graphite arm under the ceiling; left out of a hall narrower than kBladeMinHallMm (the
/// blades of two doorways facing each other must not meet).
constexpr double kBladeWidthM = 0.8;
constexpr double kBladeHeightM = 0.24;
constexpr double kBladeFrameM = 0.025;
constexpr double kBladeDepthM = 0.02;
constexpr std::int64_t kBladeCentreMm = 2620;
constexpr std::int64_t kBladeOutMm = 460;
constexpr std::int64_t kBladeOffsetMm = 1050;
constexpr std::int64_t kBladeArmMm = 20;
constexpr std::int64_t kBladeArmYMm = 2760;
constexpr std::int64_t kBladeArmLengthMm = 900;
constexpr std::int64_t kBladeMinHallMm = 2400;
/// What a portal's sign reads when its room has no sign text (never the room's id).
constexpr std::string_view kGenericSign = "GAME ROOM";
/// A sconce: a backplate and a lit shade standing off it, centred this high (above the head: it blocks nothing, and
/// collides with nothing), and its wash on the wall: a quad this wide and high, centred this far above the shade, this
/// far off the plaster.
constexpr std::int64_t kSconceCentreMm = 1950;
constexpr double kWashWidthM = 1.0;
constexpr double kWashHeightM = 1.5;
constexpr std::int64_t kWashAboveMm = 50;
constexpr double kWashOffM = 0.0015;
constexpr std::int64_t kSconcePlateWidthMm = 100;
constexpr std::int64_t kSconcePlateHeightMm = 240;
constexpr std::int64_t kSconcePlateDepthMm = 18;
constexpr std::int64_t kSconceShadeHeightMm = 170;
constexpr int kSconceShadeSegments = 24;
/// The print on the east end wall of a hall (not the lobby's, whose entrance stands there): larger, centred on it.
constexpr double kEndPrintWidthM = 0.9;
constexpr double kEndPrintHeightM = 1.2;
constexpr std::int64_t kEndPrintCentreMm = 1750;
/// A print: a framed picture (portrait, or landscape: floor_layout.hpp's widths turned) centred this high.
constexpr std::int64_t kPictureCentreMm = 1620;
constexpr double kPictureHeightM = 0.76;
constexpr double kPictureFrameM = 0.04;
/// A bench: two end boards, a seat board and a cushion, 30 mm off the wall.
constexpr std::int64_t kBenchGapMm = 30;
constexpr std::int64_t kBenchEndMm = 60;
constexpr std::int64_t kBenchSeatMm = 400;
constexpr std::int64_t kBenchBoardMm = 40;
constexpr std::int64_t kBenchCushionMm = 60;
constexpr std::int64_t kBenchCushionInsetMm = 30;
/// A plant: the plant prefab this tall, its pot this wide and tall, this many leaves, on a saucer a little wider than
/// the pot.
constexpr double kPlantHeightM = 1.15;
constexpr double kPlantPotRadiusM = 0.17;
constexpr double kPlantPotHeightM = 0.34;
constexpr int kPlantLeaves = 16;
constexpr double kPlantSaucerRadiusM = 0.196;
/// The runner: rugs (the rug prefab's own size, its picture's) end to end down the hall's middle from past the
/// elevator's zone, while they end this far before the east end: a longer hall appends rugs and moves none.
constexpr std::int64_t kRunnerStartMm = 1800;
constexpr std::int64_t kRunnerLengthMm = 2400;
constexpr std::int64_t kRunnerWidthMm = 1600;
constexpr std::int64_t kRunnerEndClearMm = 600;
/// Invented print titles for the hall's pictures (style §17.2: no real title or brand). A print takes the title of
/// its place along its edge (its slot / 2) plus its edge's and its floor's offsets, so that neighbours never repeat
/// (the same title comes back every 16 prints of an edge, 48 m) and a longer hall appends titles; each title is drawn
/// with one seed (its own), so that the prints share a small pool of pictures.
constexpr std::array<std::string_view, 16> kHallPrintTitles{
    "SAVE POINT", "PLAYER ONE", "HIGH SCORE", "LEVEL SELECT", "INSERT COIN", "BONUS STAGE", "CONTINUE",  "PRESS START",
    "EXTRA LIFE", "WORLD MAP",  "TIME ATTACK", "POWER UP",    "BOSS RUSH",   "NEW RECORD",  "TWO PLAYERS", "GAME OVER"};
/// The south edge's titles start half the list on from the north edge's.
constexpr std::size_t kSouthTitleOffset = 8;

double meters(std::int64_t millimetres) noexcept {
    return static_cast<double>(millimetres) / 1000.0;
}

/// `value` rounded to 1 / `perUnit` (scene_builder.cpp's rounding: a float computed from the prefabs writes one text).
double rounded(double value, double perUnit) noexcept {
    const double result = std::round(value * perUnit) / perUnit;
    return result == 0.0 ? 0.0 : result;
}

json vec3Json(double x, double y, double z) {
    return json::array({x, y, z});
}

/// A point [x, y, z] of a room's scene (meters) on its floor.
json floorPoint(const json& point, const RoomPlacement& placement) {
    const double sign = placement.yawHalfTurns % 2 == 0 ? 1.0 : -1.0;
    return vec3Json(rounded(meters(placement.xMm) + sign * point[0].get<double>(), 1e6), point[1].get<double>(),
                    rounded(meters(placement.zMm) + sign * point[2].get<double>(), 1e6));
}

/// A yaw (degrees) turned by the placement: plus half a turn for the south row, within (-180, 180].
double floorYaw(double yawDeg, const RoomPlacement& placement) {
    if (placement.yawHalfTurns % 2 == 0) {
        return yawDeg;
    }
    double turned = yawDeg + 180.0;
    while (turned > 180.0) {
        turned -= 360.0;
    }
    while (turned <= -180.0) {
        turned += 360.0;
    }
    return rounded(turned, 1e4);
}

/// How a box's faces are textured: in meters every kMetersPerTile (planks), the same anchored at the box's place on the
/// floor (plaster: boxes side by side tile on without a seam), or along the grain (wood).
enum class BoxUvMode : std::uint8_t { Meters, PlacedMeters, Grain };

/// A box primitive: `footprint` on the floor from `bottomMm` to `topMm`, of `material`, uvs in meters (anchored at its
/// place, or along the grain).
json boxObject(std::string id, const Footprint& footprint, std::int64_t bottomMm, std::int64_t topMm,
               std::string_view material, bool collides, bool castsShadows, BoxUvMode uv = BoxUvMode::Meters) {
    const double centreX = static_cast<double>(footprint.minXMm + footprint.maxXMm) / 2000.0;
    const double centreY = static_cast<double>(bottomMm + topMm) / 2000.0;
    const double centreZ = static_cast<double>(footprint.minZMm + footprint.maxZMm) / 2000.0;
    json object{{"id", std::move(id)},
                {"primitive", "box"},
                {"params", json{{"size", vec3Json(meters(footprint.maxXMm - footprint.minXMm), meters(topMm - bottomMm),
                                                  meters(footprint.maxZMm - footprint.minZMm))},
                                {"uv", uv == BoxUvMode::Grain ? "grain" : "meters"},
                                {"metersPerTile", uv == BoxUvMode::Grain ? kWoodMetersPerTile : kMetersPerTile}}},
                {"position", vec3Json(centreX, centreY, centreZ)},
                {"material", std::string(material)}};
    if (uv == BoxUvMode::PlacedMeters) {
        object["params"]["uvOrigin"] = vec3Json(centreX, centreY, centreZ);
    }
    if (!collides) {
        object["collides"] = false;
    }
    if (!castsShadows) {
        object["castsShadows"] = false;
    }
    return object;
}

/// An area of the floor; `look` its own lit terms (the hall's), none for an area lit as the scene (a room).
json areaJson(const FloorArea& area, const AreaLook* look) {
    json boxes = json::array();
    for (std::size_t i = 0; i < area.footprints.size(); ++i) {
        const Footprint& footprint = area.footprints[i];
        const std::int64_t height = i > 0 && area.passageHeightMm > 0 ? area.passageHeightMm : area.heightMm;
        boxes.push_back(json{{"min", vec3Json(meters(footprint.minXMm), 0.0, meters(footprint.minZMm))},
                             {"max", vec3Json(meters(footprint.maxXMm), meters(height), meters(footprint.maxZMm))}});
    }
    json result{{"id", area.id}, {"boxes", std::move(boxes)}};
    if (look != nullptr) {
        result["environment"] = json{{"exposure", look->exposure},
                                     {"ambientSky", look->ambientSky},
                                     {"ambientGround", look->ambientGround},
                                     {"ambientIntensity", look->ambientIntensity},
                                     {"fillColor", look->fillColor},
                                     {"fillIntensity", look->fillIntensity}};
    }
    return result;
}

/// The materials of the roles of `kind` the hall uses: the theme's, a role it lacks the built-in pack's.
std::map<std::string, std::string> kindMaterials(const ThemeMaterials& theme, std::string_view kind) {
    std::map<std::string, std::string> materials = builtInThemeMaterials().materials(kind);
    for (const auto& [role, material] : theme.materials(kind)) {
        materials[role] = material;
    }
    return materials;
}

json materialsJson(const std::map<std::string, std::string>& materials) {
    json object = json::object();
    for (const auto& [role, material] : materials) {
        object[role] = material;
    }
    return object;
}

/// A wall of the hall seen from inside: its face at `faceMm` (z for an edge, x for an end wall), the hall on the side
/// of `inward` (+1 or -1) along z (an edge) or x (an end wall).
struct WallFace {
    bool alongX = true;
    std::int64_t faceMm = 0;
    std::int64_t inward = 1;

    /// The footprint of a piece over [from, to] along the wall, `depth` deep from `offset` off the face.
    [[nodiscard]] Footprint piece(std::int64_t from, std::int64_t to, std::int64_t depth,
                                  std::int64_t offset = 0) const noexcept {
        const std::int64_t near = faceMm + inward * offset;
        const std::int64_t far = near + inward * depth;
        const std::int64_t low = std::min(near, far);
        const std::int64_t high = std::max(near, far);
        return alongX ? Footprint{from, low, to, high} : Footprint{low, from, high, to};
    }
};

/// The parts of [low, high] that none of `cuts` covers, low to high (cuts clamped to [low, high]).
std::vector<std::pair<std::int64_t, std::int64_t>> runsBetween(std::int64_t low, std::int64_t high,
                                                               std::vector<std::pair<std::int64_t, std::int64_t>> cuts) {
    std::sort(cuts.begin(), cuts.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> runs;
    std::int64_t cursor = low;
    for (const auto& [cutFrom, cutTo] : cuts) {
        const std::int64_t from = std::clamp(cutFrom, low, high);
        const std::int64_t to = std::clamp(cutTo, low, high);
        if (to <= from) {
            continue;
        }
        if (from > cursor) {
            runs.emplace_back(cursor, from);
        }
        cursor = std::max(cursor, to);
    }
    if (cursor < high) {
        runs.emplace_back(cursor, high);
    }
    return runs;
}

/// The multiple of `pitch` at or above `value` (value may be negative), divided by the pitch.
std::int64_t ceilDiv(std::int64_t value, std::int64_t pitch) noexcept {
    const std::int64_t quotient = value / pitch;
    return quotient * pitch < value ? quotient + 1 : quotient;
}

/// The trim of a run [from, to] of a wall (see kBaseboardHeightMm): its baseboard, wainscot panel, battens (at the
/// pitch's multiples along the wall, whole ones only, so that a longer run appends battens and moves none), rail and,
/// with `skin`, the plaster above up to the crown. A run's end that meets the hall's other walls (`insetFrom`,
/// `insetTo`) shortens each piece by its depth (an end wall's pieces stop against the edges' ones).
void addWallRun(const std::string& id, const WallFace& wall, std::int64_t from, std::int64_t to, bool insetFrom,
                bool insetTo, bool skin, std::int64_t heightMm, const std::map<std::string, std::string>& materials,
                json& objects) {
    const auto span = [&](std::int64_t depth) {
        return std::pair{from + (insetFrom ? depth : 0), to - (insetTo ? depth : 0)};
    };
    const auto add = [&](const std::string& part, std::int64_t depth, std::int64_t bottom, std::int64_t top,
                         const std::string& role, BoxUvMode uv) {
        const auto [a, b] = span(depth);
        if (b - a < 10) {
            return;
        }
        objects.push_back(boxObject(id + "/" + part, wall.piece(a, b, depth), bottom, top, materials.at(role), false,
                                    false, uv));
    };
    add("baseboard", kBaseboardDepthMm, 0, kBaseboardHeightMm, "baseboard", BoxUvMode::Grain);
    add("wainscot", kWainscotDepthMm, kBaseboardHeightMm, kWainscotTopMm - kRailHeightMm, "wainscot", BoxUvMode::Grain);
    // Battens a batten's width clear of the run's ends.
    const auto [battensFrom, battensTo] = span(kRailDepthMm);
    for (std::int64_t k = ceilDiv(battensFrom + kBattenWidthMm * 3 / 2, kBattenPitchMm);; ++k) {
        const std::int64_t centre = k * kBattenPitchMm;
        if (centre + kBattenWidthMm * 3 / 2 > battensTo) {
            break;
        }
        objects.push_back(boxObject(id + "/batten" + std::to_string(k),
                                    wall.piece(centre - kBattenWidthMm / 2, centre + kBattenWidthMm / 2, kBattenDepthMm),
                                    kBaseboardHeightMm, kWainscotTopMm - kRailHeightMm, materials.at("trim"), false,
                                    false, BoxUvMode::Grain));
    }
    add("rail", kRailDepthMm, kWainscotTopMm - kRailHeightMm, kWainscotTopMm, "trim", BoxUvMode::Grain);
    if (skin) {
        add("plaster", kSkinDepthMm, kWainscotTopMm, heightMm - kCrownHeightMm, "wall", BoxUvMode::PlacedMeters);
    }
}

/// A sconce on an edge's wall at `x`: a backplate on the plaster and its lit shade, a short cylinder, standing off it,
/// and the wash of its light on the wall (an unlit quad, the wall wash's picture; every sconce's the same mesh and
/// material, instanced). Above the head, they collide with nothing.
void addSconce(const std::string& id, const WallFace& wall, std::int64_t x,
               const std::map<std::string, std::string>& materials, json& objects) {
    const std::int64_t plateBottom = kSconceCentreMm - kSconcePlateHeightMm / 2;
    objects.push_back(boxObject(id + "/backplate",
                                wall.piece(x - kSconcePlateWidthMm / 2, x + kSconcePlateWidthMm / 2, kSconcePlateDepthMm,
                                           kSkinDepthMm),
                                plateBottom, plateBottom + kSconcePlateHeightMm, materials.at("trim"), false, false,
                                BoxUvMode::Grain));
    const std::int64_t radius = kHallSconceWidthMm / 2;
    const std::int64_t axis = wall.faceMm + wall.inward * (kSkinDepthMm + kSconcePlateDepthMm + radius);
    objects.push_back(json{{"id", id + "/shade"},
                           {"primitive", "cylinder"},
                           {"params", json{{"radius", meters(radius)},
                                           {"height", meters(kSconceShadeHeightMm)},
                                           {"segments", kSconceShadeSegments}}},
                           {"position", vec3Json(meters(x), meters(kSconceCentreMm), meters(axis))},
                           {"material", materials.at("sconce")},
                           {"castsShadows", false},
                           {"collides", false}});
    // The quad faces +Z: the north edge's wall faces +Z, the south edge's -Z.
    const double wash = meters(wall.faceMm) + static_cast<double>(wall.inward) * (meters(kSkinDepthMm) + kWashOffM);
    objects.push_back(json{{"id", id + "/wash"},
                           {"primitive", "quad"},
                           {"params", json{{"size", json::array({kWashWidthM, kWashHeightM})}}},
                           {"position",
                            vec3Json(meters(x), meters(kSconceCentreMm + kWashAboveMm), rounded(wash, 1e6))},
                           {"rotationDeg", vec3Json(0.0, wall.inward > 0 ? 0.0 : 180.0, 0.0)},
                           {"material", materials.at("wash")},
                           {"castsShadows", false},
                           {"collides", false}});
}

/// A framed panel on an edge's wall (the poster frame prefab): its back on the hall's plaster at `x`, centred
/// `centreMm` high, facing into the hall.
json wallFrame(const std::string& id, const WallFace& wall, std::int64_t x, std::int64_t centreMm, json params,
               json materials, json picture) {
    const std::int64_t back = wall.faceMm + wall.inward * kSkinDepthMm;
    // The prefab faces +Z: the north edge's wall faces +Z, the south edge's -Z.
    return json{{"id", id},
                {"prefab", "poster_frame"},
                {"params", std::move(params)},
                {"position", vec3Json(meters(x), meters(centreMm), meters(back))},
                {"rotationDeg", vec3Json(0.0, wall.inward > 0 ? 0.0 : 180.0, 0.0)},
                {"materials", std::move(materials)},
                {"textures", json{{"picture", std::move(picture)}}},
                {"castsShadows", false}};
}

/// A bench against an edge's wall, centred at `x`: two end boards, a seat board and its cushion.
void addBench(const std::string& id, const WallFace& wall, std::int64_t x,
              const std::map<std::string, std::string>& materials, json& objects) {
    const std::int64_t half = kHallBenchLengthMm / 2;
    const std::int64_t depth = kHallBenchDepthMm - kBenchGapMm;
    const std::string& wood = materials.at("bench");
    objects.push_back(boxObject(id + "/end-west", wall.piece(x - half, x - half + kBenchEndMm, depth, kBenchGapMm), 0,
                                kBenchSeatMm, wood, true, true, BoxUvMode::Grain));
    objects.push_back(boxObject(id + "/end-east", wall.piece(x + half - kBenchEndMm, x + half, depth, kBenchGapMm), 0,
                                kBenchSeatMm, wood, true, true, BoxUvMode::Grain));
    objects.push_back(boxObject(id + "/seat", wall.piece(x - half, x + half, depth, kBenchGapMm), kBenchSeatMm,
                                kBenchSeatMm + kBenchBoardMm, wood, true, true, BoxUvMode::Grain));
    objects.push_back(boxObject(id + "/cushion",
                                wall.piece(x - half + kBenchCushionInsetMm, x + half - kBenchCushionInsetMm,
                                           depth - 2 * kBenchCushionInsetMm, kBenchGapMm + kBenchCushionInsetMm),
                                kBenchSeatMm + kBenchBoardMm, kBenchSeatMm + kBenchBoardMm + kBenchCushionMm,
                                materials.at("cushion"), true, true));
}

/// The hall's objects (see the header): its floor, ceiling and walls, its lamps, the elevator, the sign and the lobby's
/// entrance, then its dressing (I6 step 2): the trim of its walls, a portal at each doorway, the beams over them, the
/// sconces, prints, benches and plants, the runner.
void addHallObjects(const Hall& hall, std::string_view sign, const ThemeMaterials& theme,
                    const std::map<RoomId, std::string>& roomSigns, json& objects) {
    const std::string prefix = hallAreaId(hall.floor) + "/";
    const std::int64_t height = hall.heightMm;
    const std::int64_t half = hall.widthMm / 2;
    const std::int64_t east = hall.interior.maxXMm;
    const std::map<std::string, std::string> materials = kindMaterials(theme, "hall");
    objects.push_back(boxObject(prefix + "floor", hall.interior, -kSlabMm, 0, materials.at("floor"), false, false));
    objects.push_back(
        boxObject(prefix + "ceiling", hall.interior, height, height + kSlabMm, materials.at("ceiling"), false, false));
    for (const HallWall& wall : hall.walls) {
        objects.push_back(boxObject(prefix + "wall-" + wall.id, wall.footprint, 0, height, materials.at("wall"), true,
                                    true, BoxUvMode::PlacedMeters));
    }
    // The beams across the ceiling, by x: a fixed grid from the west end, and one over each doorway the grid leaves
    // without a beam nearby (one where two doorways face each other).
    std::set<std::int64_t> beams;
    for (std::int64_t x = kBeamPitchMm; x + kBeamEndClearMm <= east; x += kBeamPitchMm) {
        beams.insert(x);
    }
    const std::set<std::int64_t> grid = beams;
    for (const FloorDoorway& doorway : hall.doorways) {
        const std::int64_t centre = (doorway.minMm + doorway.maxMm) / 2;
        const auto near = grid.lower_bound(centre - kBeamMergeMm);
        if (doorway.opensOntoHall() && (near == grid.end() || *near > centre + kBeamMergeMm)) {
            beams.insert(centre);
        }
    }
    // The lamps: a drum shade over each light on a stem from the ceiling (through a beam over it).
    for (std::size_t i = 0; i < hall.lights.size(); ++i) {
        const FloorPoint& light = hall.lights[i];
        const std::int64_t centre = height - kHallLightDropMm + kLampAboveLightMm;
        objects.push_back(json{{"id", prefix + "lamp-" + std::to_string(i)},
                               {"primitive", "cylinder"},
                               {"params", json{{"radius", meters(kLampRadiusMm)},
                                               {"height", meters(kLampHeightMm)},
                                               {"segments", kLampSegments}}},
                               {"position", vec3Json(meters(light.xMm), meters(centre), meters(light.zMm))},
                               {"material", std::string(kLampMaterial)},
                               {"castsShadows", false},
                               {"collides", false}});
        const Footprint stem{light.xMm - kLampStemMm / 2, light.zMm - kLampStemMm / 2, light.xMm + kLampStemMm / 2,
                             light.zMm + kLampStemMm / 2};
        objects.push_back(boxObject(prefix + "lamp-" + std::to_string(i) + "/stem", stem, centre + kLampHeightMm / 2,
                                    height, materials.at("trim"), false, false, BoxUvMode::Grain));
    }
    // The elevator: a frame proud of the west wall around two closed leaves, a call panel south of it.
    const Footprint& frame = hall.elevator;
    const std::int64_t doorHalf = kElevatorDoorWidthMm / 2;
    objects.push_back(boxObject(prefix + "elevator/frame-north", {frame.minXMm, frame.minZMm, frame.maxXMm, -doorHalf},
                                0, kElevatorDoorHeightMm + kElevatorFrameMm, kElevatorFrameMaterial, true, true));
    objects.push_back(boxObject(prefix + "elevator/frame-south", {frame.minXMm, doorHalf, frame.maxXMm, frame.maxZMm},
                                0, kElevatorDoorHeightMm + kElevatorFrameMm, kElevatorFrameMaterial, true, true));
    objects.push_back(boxObject(prefix + "elevator/frame-head", {frame.minXMm, -doorHalf, frame.maxXMm, doorHalf},
                                kElevatorDoorHeightMm, kElevatorDoorHeightMm + kElevatorFrameMm,
                                kElevatorFrameMaterial, true, true));
    objects.push_back(boxObject(prefix + "elevator/door-north", {0, -doorHalf, kDoorLeafDepthMm, -kDoorGapMm / 2}, 0,
                                kElevatorDoorHeightMm, kElevatorDoorMaterial, true, true));
    objects.push_back(boxObject(prefix + "elevator/door-south", {0, kDoorGapMm / 2, kDoorLeafDepthMm, doorHalf}, 0,
                                kElevatorDoorHeightMm, kElevatorDoorMaterial, true, true));
    const std::int64_t panelZ = frame.maxZMm + kPanelGapMm;
    objects.push_back(boxObject(prefix + "elevator/panel", {0, panelZ, kPanelDepthMm, panelZ + kPanelWidthMm},
                                kPanelBottomMm, kPanelBottomMm + kPanelHeightMm, kPanelMaterial, true, true));
    const std::int64_t buttonZ = panelZ + (kPanelWidthMm - kButtonMm) / 2;
    const std::int64_t buttonY = kPanelBottomMm + (kPanelHeightMm - kButtonMm) / 2;
    objects.push_back(boxObject(prefix + "elevator/button",
                                {kPanelDepthMm, buttonZ, kPanelDepthMm + kButtonMm / 2, buttonZ + kButtonMm}, buttonY,
                                buttonY + kButtonMm, kButtonMaterial, false, false));
    // The floor's sign above the elevator, facing east: the rooms' sign panel with the floor's text.
    json signObject{{"id", prefix + "sign"},
                    {"prefab", "poster_frame"},
                    {"params", json{{"width", kSignWidthM},
                                    {"height", kSignHeightM},
                                    {"frameWidth", kSignFrameM},
                                    {"depth", kSignDepthM}}},
                    {"position", vec3Json(0.0, meters(kSignCentreMm), 0.0)},
                    {"rotationDeg", vec3Json(0.0, kFacingEastDeg, 0.0)},
                    {"textures", json{{"picture", json{{"pattern", "sign"}, {"title", std::string(sign)}}}}},
                    {"castsShadows", false},
                    {"collides", false}};
    signObject["materials"] = materialsJson(theme.materials("poster_frame", "sign"));
    objects.push_back(std::move(signObject));
    // The lobby's entrance at its east end: two closed leaves in a frame (decoration).
    if (hall.lobby) {
        const Footprint& door = hall.entrance;
        const std::int64_t inner = east - kElevatorFrameDepthMm;
        const std::int64_t entranceHalf = kEntranceWidthMm / 2;
        objects.push_back(boxObject(prefix + "entrance/frame-north",
                                    {inner, door.minZMm - kElevatorFrameMm, east, -entranceHalf}, 0,
                                    kEntranceHeightMm + kElevatorFrameMm, kEntranceFrameMaterial, true, true));
        objects.push_back(boxObject(prefix + "entrance/frame-south",
                                    {inner, entranceHalf, east, door.maxZMm + kElevatorFrameMm}, 0,
                                    kEntranceHeightMm + kElevatorFrameMm, kEntranceFrameMaterial, true, true));
        objects.push_back(boxObject(prefix + "entrance/frame-head", {inner, -entranceHalf, east, entranceHalf},
                                    kEntranceHeightMm, kEntranceHeightMm + kElevatorFrameMm, kEntranceFrameMaterial,
                                    true, true));
        objects.push_back(boxObject(prefix + "entrance/door-north",
                                    {east - kDoorLeafDepthMm, -entranceHalf, east, -kDoorGapMm / 2}, 0,
                                    kEntranceHeightMm, kEntranceDoorMaterial, true, true));
        objects.push_back(boxObject(prefix + "entrance/door-south",
                                    {east - kDoorLeafDepthMm, kDoorGapMm / 2, east, entranceHalf}, 0,
                                    kEntranceHeightMm, kEntranceDoorMaterial, true, true));
    }

    // The trim of the long edges: runs between the doorways' casings, the plaster skin over the rooms' walls as over
    // the hall's own, a crown along the whole edge.
    const WallFace northWall{true, -half, 1};
    const WallFace southWall{true, half, -1};
    for (const bool north : {true, false}) {
        const WallFace& wall = north ? northWall : southWall;
        const std::string edge = north ? "north" : "south";
        std::vector<std::pair<std::int64_t, std::int64_t>> cuts;
        for (const FloorDoorway& doorway : hall.doorways) {
            if (doorway.opensOntoHall() && (doorway.hallEdgeZMm < 0) == north) {
                cuts.emplace_back(doorway.minMm - kCasingWidthMm, doorway.maxMm + kCasingWidthMm);
            }
        }
        const auto runs = runsBetween(0, east, cuts);
        for (std::size_t i = 0; i < runs.size(); ++i) {
            // A run is named by where it starts (a doorway's casing, or the west end), so that a doorway further
            // east leaves the runs west of it as they were.
            addWallRun(prefix + "trim/" + edge + "-" + std::to_string(runs[i].first), wall, runs[i].first,
                       runs[i].second, false, false, true, height, materials, objects);
        }
        objects.push_back(boxObject(prefix + "trim/" + edge + "/crown", wall.piece(0, east, kCrownDepthMm),
                                    height - kCrownHeightMm, height, materials.at("trim"), false, false,
                                    BoxUvMode::Grain));
    }
    // The end walls: the west one's either side of the elevator, the east one's whole (the lobby's either side of its
    // entrance); their pieces stop against the edges'.
    const std::int64_t frameHalf = kElevatorDoorWidthMm / 2 + kElevatorFrameMm;
    const WallFace westWall{false, 0, 1};
    addWallRun(prefix + "trim/west-north", westWall, -half, -frameHalf, true, false, false, height, materials, objects);
    addWallRun(prefix + "trim/west-south", westWall, frameHalf, half, false, true, false, height, materials, objects);
    const WallFace eastWall{false, east, -1};
    if (hall.lobby) {
        const std::int64_t entranceHalf = kEntranceWidthMm / 2 + kElevatorFrameMm;
        addWallRun(prefix + "trim/east-north", eastWall, -half, -entranceHalf, true, false, false, height, materials,
                   objects);
        addWallRun(prefix + "trim/east-south", eastWall, entranceHalf, half, false, true, false, height, materials,
                   objects);
    } else {
        addWallRun(prefix + "trim/east", eastWall, -half, half, true, true, false, height, materials, objects);
    }
    for (const auto& [name, wall] : {std::pair{std::string("west"), westWall}, std::pair{std::string("east"), eastWall}}) {
        objects.push_back(boxObject(prefix + "trim/" + name + "/crown",
                                    wall.piece(-half + kCrownDepthMm, half - kCrownDepthMm, kCrownDepthMm),
                                    height - kCrownHeightMm, height, materials.at("trim"), false, false,
                                    BoxUvMode::Grain));
    }

    // A portal at each doorway that opens onto the hall: the casing, the lintel, the plaster over them and the room's
    // sign above (lit lettering: it reads from the hall without a light of its own).
    for (const FloorDoorway& doorway : hall.doorways) {
        if (!doorway.opensOntoHall()) {
            continue;
        }
        const WallFace& wall = doorway.hallEdgeZMm < 0 ? northWall : southWall;
        const std::string id = prefix + "portal:" + doorway.room.toString();
        const std::int64_t head = doorway.heightMm;
        const std::int64_t from = doorway.minMm - kCasingWidthMm;
        const std::int64_t to = doorway.maxMm + kCasingWidthMm;
        objects.push_back(boxObject(id + "/casing-west", wall.piece(from, doorway.minMm, kCasingDepthMm), 0, head,
                                    materials.at("trim"), false, false, BoxUvMode::Grain));
        objects.push_back(boxObject(id + "/casing-east", wall.piece(doorway.maxMm, to, kCasingDepthMm), 0, head,
                                    materials.at("trim"), false, false, BoxUvMode::Grain));
        objects.push_back(boxObject(id + "/casing-head", wall.piece(from, to, kCasingDepthMm), head,
                                    head + kCasingWidthMm, materials.at("trim"), false, false, BoxUvMode::Grain));
        const std::int64_t lintel = head + kCasingWidthMm;
        objects.push_back(boxObject(id + "/lintel",
                                    wall.piece(from - kLintelOverhangMm, to + kLintelOverhangMm, kLintelDepthMm),
                                    lintel, lintel + kLintelHeightMm, materials.at("trim"), false, false,
                                    BoxUvMode::Grain));
        objects.push_back(boxObject(id + "/plaster", wall.piece(from, to, kSkinDepthMm), lintel,
                                    height - kCrownHeightMm, materials.at("wall"), false, false,
                                    BoxUvMode::PlacedMeters));
        const auto text = roomSigns.find(doorway.room);
        const std::string title =
            text != roomSigns.end() && !text->second.empty() ? text->second : std::string(kGenericSign);
        const std::int64_t signCentre = (lintel + kLintelHeightMm + height - kBeamDepthMm) / 2;
        json signFrame = wallFrame(id + "/sign", wall, (doorway.minMm + doorway.maxMm) / 2, signCentre,
                                   json{{"width", kPortalSignWidthM},
                                        {"height", kPortalSignHeightM},
                                        {"frameWidth", kPortalSignFrameM},
                                        {"depth", kPortalSignDepthM}},
                                   json{{"frame", materials.at("trim")}, {"picture", materials.at("sign")}},
                                   json{{"pattern", "sign"}, {"title", title}});
        signFrame["collides"] = false;
        objects.push_back(std::move(signFrame));
        // The blade sign: the same lettering on both faces of a panel standing out from the wall towards the
        // elevator, read from down the hall either way; a graphite arm under the ceiling holds it.
        if (hall.widthMm < kBladeMinHallMm) {
            continue;
        }
        const std::int64_t bladeX = (doorway.minMm + doorway.maxMm) / 2 - kBladeOffsetMm;
        const double bladeZ = meters(wall.faceMm + wall.inward * kBladeOutMm);
        for (const auto& [side, yaw] : {std::pair{std::string_view("east"), kFacingEastDeg},
                                        std::pair{std::string_view("west"), -kFacingEastDeg}}) {
            const double back = meters(bladeX) + (yaw > 0.0 ? 0.001 : -0.001);
            objects.push_back(json{{"id", id + "/blade-" + std::string(side)},
                                   {"prefab", "poster_frame"},
                                   {"params", json{{"width", kBladeWidthM},
                                                   {"height", kBladeHeightM},
                                                   {"frameWidth", kBladeFrameM},
                                                   {"depth", kBladeDepthM}}},
                                   {"position", vec3Json(rounded(back, 1e6), meters(kBladeCentreMm), bladeZ)},
                                   {"rotationDeg", vec3Json(0.0, yaw, 0.0)},
                                   {"materials",
                                    json{{"frame", materials.at("trim")}, {"picture", materials.at("sign")}}},
                                   {"textures", json{{"picture", json{{"pattern", "sign"}, {"title", title}}}}},
                                   {"castsShadows", false},
                                   {"collides", false}});
        }
        objects.push_back(boxObject(id + "/blade-arm",
                                    wall.piece(bladeX - kBladeArmMm / 2, bladeX + kBladeArmMm / 2, kBladeArmLengthMm,
                                               kSkinDepthMm),
                                    kBladeArmYMm - kBladeArmMm / 2, kBladeArmYMm + kBladeArmMm / 2, kBladeArmMaterial,
                                    false, false));
    }
    for (const std::int64_t beam : beams) {
        objects.push_back(boxObject(prefix + "beam-" + std::to_string(beam),
                                    {beam - kBeamWidthMm / 2, -half, beam + kBeamWidthMm / 2, half},
                                    height - kBeamDepthMm, height, materials.at("beam"), false, true,
                                    BoxUvMode::Grain));
    }

    // The sconces, prints, benches and plants (deriveHall()'s dressing); the plants seeded by the floor and the piece's
    // id, the prints' titles by their place (kHallPrintTitles), each title's picture by its own seed.
    std::map<std::string, std::string> plant = kindMaterials(theme, "plant");
    plant["pot"] = materials.at("plant");
    plant["saucer"] = materials.at("saucer");
    std::map<std::string, std::string> print = kindMaterials(theme, "poster_frame");
    print["frame"] = materials.at("picture");
    const std::size_t titleOffset =
        static_cast<std::size_t>(core::deriveSeed("hall-prints", hall.floor) % kHallPrintTitles.size());
    const auto printPicture = [&](std::size_t index) {
        const std::string_view title = kHallPrintTitles[index % kHallPrintTitles.size()];
        return json{{"pattern", "print"},
                    {"title", std::string(title)},
                    {"seed", core::deriveSeed("hall-print", title) % 1000000}};
    };
    for (const HallPiece& piece : hall.dressing) {
        const WallFace& wall = piece.north ? northWall : southWall;
        const std::string id = prefix + piece.id;
        const std::uint64_t seed = core::deriveSeed("hall", hall.floor, piece.id);
        switch (piece.kind) {
        case HallPieceKind::Sconce:
            addSconce(id, wall, piece.xMm, materials, objects);
            break;
        case HallPieceKind::Picture: {
            const std::size_t place = static_cast<std::size_t>(piece.slot / 2) + titleOffset +
                                      (piece.north ? std::size_t{0} : kSouthTitleOffset);
            const double width = meters(kHallPictureWidthMm);
            objects.push_back(wallFrame(id, wall, piece.xMm, kPictureCentreMm,
                                        json{{"width", piece.landscape ? kPictureHeightM : width},
                                             {"height", piece.landscape ? width : kPictureHeightM},
                                             {"frameWidth", kPictureFrameM},
                                             {"depth", meters(kHallPictureDepthMm - kSkinDepthMm)}},
                                        materialsJson(print), printPicture(place)));
            break;
        }
        case HallPieceKind::Bench:
            addBench(id, wall, piece.xMm, materials, objects);
            break;
        case HallPieceKind::Plant: {
            const std::int64_t z = (piece.footprint.minZMm + piece.footprint.maxZMm) / 2;
            objects.push_back(json{{"id", id},
                                   {"prefab", "plant"},
                                   {"params", json{{"height", kPlantHeightM},
                                                   {"potRadius", kPlantPotRadiusM},
                                                   {"potHeight", kPlantPotHeightM},
                                                   {"spread", meters(kHallPlantSpreadMm)},
                                                   {"leaves", kPlantLeaves},
                                                   {"seed", static_cast<std::int64_t>(seed % 2147483647u)},
                                                   {"saucerRadius", kPlantSaucerRadiusM}}},
                                   {"position", vec3Json(meters(piece.xMm), 0.0, meters(z))},
                                   {"rotationDeg", vec3Json(0.0, 0.0, 0.0)},
                                   {"materials", materialsJson(plant)}});
            break;
        }
        }
    }
    // The east end wall's print (it stands with that wall, which moves east as the hall grows; the lobby's entrance
    // stands there).
    if (!hall.lobby) {
        objects.push_back(json{{"id", prefix + "east/print"},
                               {"prefab", "poster_frame"},
                               {"params", json{{"width", kEndPrintWidthM},
                                               {"height", kEndPrintHeightM},
                                               {"frameWidth", kPictureFrameM},
                                               {"depth", meters(kHallPictureDepthMm)}}},
                               {"position", vec3Json(meters(east), meters(kEndPrintCentreMm), 0.0)},
                               {"rotationDeg", vec3Json(0.0, -kFacingEastDeg, 0.0)},
                               {"materials", materialsJson(print)},
                               {"textures", json{{"picture", printPicture(titleOffset + kHallPrintTitles.size() / 4)}}},
                               {"castsShadows", false}});
    }
    // The runner: rugs end to end down the middle, from past the elevator's zone.
    for (std::int64_t k = 0;; ++k) {
        const std::int64_t from = kRunnerStartMm + k * kRunnerLengthMm;
        if (from + kRunnerLengthMm + kRunnerEndClearMm > east) {
            break;
        }
        objects.push_back(json{{"id", prefix + "runner-" + std::to_string(k)},
                               {"prefab", "rug"},
                               {"params", json{{"width", meters(kRunnerLengthMm)}, {"depth", meters(kRunnerWidthMm)}}},
                               {"position", vec3Json(meters(from + kRunnerLengthMm / 2), 0.0, 0.0)},
                               {"rotationDeg", vec3Json(0.0, 0.0, 0.0)},
                               {"materials", json{{"fabric", materials.at("runner")}}},
                               {"castsShadows", false},
                               {"collides", false}});
    }
}

/// The hall's lights, in the hall's area: the profile's hall light on its centre line below the ceiling, then a fill
/// spot before each doorway that opens onto it (by room id), low in the hall and aimed through the doorway into the
/// room, its outer cone the half angle the opening's width subtends at the aim point's distance plus the style's margin
/// (at least its minimum), so that it lights the room's middle rather than the walls beside the doorway.
void addHallLights(const Hall& hall, const LightingProfile& lighting, json& lights) {
    const LightStyle& style = lighting.hall.light;
    const std::string area = hallAreaId(hall.floor);
    for (std::size_t i = 0; i < hall.lights.size(); ++i) {
        const FloorPoint& light = hall.lights[i];
        lights.push_back(json{{"name", area + "/light-" + std::to_string(i)},
                              {"type", "point"},
                              {"position", vec3Json(meters(light.xMm), meters(hall.heightMm - kHallLightDropMm),
                                                    meters(light.zMm))},
                              {"color", style.color},
                              {"intensity", style.intensity},
                              {"range", style.range},
                              {"area", area}});
    }
    const LightStyle& fill = lighting.hall.doorwayFill;
    for (const FloorDoorway& doorway : hall.doorways) {
        if (!doorway.opensOntoHall()) {
            continue;
        }
        // Into the room: north (-z) for the north row, south for the south row.
        const std::int64_t inward = doorway.hallEdgeZMm < 0 ? -1 : 1;
        const std::int64_t x = (doorway.minMm + doorway.maxMm) / 2;
        const std::int64_t y = std::max<std::int64_t>(kDoorwayFillTargetHeightMm, doorway.heightMm - kDoorwayFillBelowHeadMm);
        const std::int64_t z = doorway.hallEdgeZMm - inward * kDoorwayFillSetbackMm;
        // The opening's half width seen from the aim point's distance: the cone covers the room's middle through the
        // doorway, not the walls beside it inside the room.
        const double halfOpening = std::atan2(meters(doorway.maxMm - doorway.minMm) / 2.0,
                                              meters(kDoorwayFillSetbackMm + kDoorwayFillReachMm));
        const double outer = std::clamp(halfOpening * 180.0 / 3.14159265358979323846 + fill.coneMarginDeg,
                                        fill.minOuterConeDeg, kMaxOuterConeDeg);
        lights.push_back(json{{"name", area + "/doorway-fill:" + doorway.room.toString()},
                              {"type", "spot"},
                              {"position", vec3Json(meters(x), meters(y), meters(z))},
                              {"target", vec3Json(meters(x), meters(kDoorwayFillTargetHeightMm),
                                                  meters(doorway.hallEdgeZMm + inward * kDoorwayFillReachMm))},
                              {"color", fill.color},
                              {"intensity", fill.intensity},
                              {"range", fill.range},
                              {"innerConeDeg", rounded(outer * fill.innerConeRatio, 1e4)},
                              {"outerConeDeg", rounded(outer, 1e4)},
                              {"area", area}});
    }
}

} // namespace

std::vector<const Room*> roomsOnFloor(const WorldPlan& plan, std::string_view floor) {
    std::vector<const Room*> rooms;
    for (const Room& room : plan.rooms) {
        if (room.floor == floor) {
            rooms.push_back(&room);
        }
    }
    std::sort(rooms.begin(), rooms.end(), [](const Room* a, const Room* b) { return a->id < b->id; });
    return rooms;
}

std::string floorObjectId(const RoomId& room, std::string_view local) {
    return room.toString() + "/" + std::string(local);
}

Expected<FloorSceneData> loadFloorSceneData(const std::filesystem::path& assetRoot, const WorldPlan& plan,
                                            std::string_view floor, const std::optional<LightingProfile>& lighting,
                                            const PlatformTagsBySystem& platformTags) {
    if (std::none_of(plan.floors.begin(), plan.floors.end(), [&](const Floor& entry) { return entry.id == floor; })) {
        return makeError(ErrorCode::NotFound, "the plan has no such floor", std::string(floor));
    }
    FloorSceneData data;
    auto building = loadBuildingData(assetRoot);
    if (!building) {
        return propagate(building.error());
    }
    data.building = std::move(*building);
    auto systems = loadSystemsData(assetRoot);
    if (!systems) {
        return propagate(systems.error());
    }
    const std::string pack = floorThemePack(floor, *systems, data.building);
    if (auto theme = loadThemeMaterials(assetRoot, pack)) {
        data.hallTheme = std::move(*theme);
    } else {
        spdlog::warn("World: the theme pack \"{}\" of the floor {} does not load ({}); its hall takes the built-in "
                     "materials",
                     pack, floor, theme.error().toString());
        data.hallTheme = builtInThemeMaterials();
    }
    if (lighting) {
        data.lighting = *lighting;
    } else if (auto profile = loadLightingProfile(assetRoot, kDefaultLightingProfile)) {
        data.lighting = std::move(*profile);
    } else {
        spdlog::warn("World: the lighting profile \"{}\" does not load ({}); the built-in evening profile is used",
                     kDefaultLightingProfile, profile.error().toString());
        data.lighting = eveningLightingProfile();
    }
    for (const Room* room : roomsOnFloor(plan, floor)) {
        const catalog::SystemName system = room->sections.empty() ? room->id.system() : room->sections.front();
        const auto tags = platformTags.find(system);
        const std::span<const std::string> roomTags =
            tags != platformTags.end() ? std::span<const std::string>(tags->second) : std::span<const std::string>();
        auto roomData = loadSceneData(assetRoot, plan, room->id, data.lighting, roomTags);
        if (!roomData) {
            return propagate(roomData.error());
        }
        data.rooms.emplace(room->id, std::move(*roomData));
    }
    return data;
}

Expected<FloorSceneBuild> toFloorSceneFile(const WorldPlan& plan, std::string_view floor, const FloorSceneData& data) {
    if (std::none_of(plan.floors.begin(), plan.floors.end(), [&](const Floor& entry) { return entry.id == floor; })) {
        return makeError(ErrorCode::NotFound, "the plan has no such floor", std::string(floor));
    }
    FloorSceneBuild build;
    std::vector<FloorRoom> floorRooms;
    for (const Room* room : roomsOnFloor(plan, floor)) {
        const auto roomData = data.rooms.find(room->id);
        if (roomData == data.rooms.end()) {
            return makeError(ErrorCode::NotFound, "no scene data for the floor's room", room->id.toString());
        }
        floorRooms.push_back(FloorRoom{room, &roomData->second.roomTemplate});
        build.rooms.push_back(room->id);
    }
    build.hall = deriveHall(floor, floorRooms, data.building.layout);

    json objects = json::array();
    json lights = json::array();
    json environment = json::object();
    for (const FloorRoom& entry : floorRooms) {
        const Room& room = *entry.room;
        auto roomScene = toSceneFile(plan, room.id, data.rooms.at(room.id));
        if (!roomScene) {
            return propagate(roomScene.error());
        }
        // Every room's data holds the floor's profile: the floor's environment is its rooms'.
        if (environment.empty()) {
            environment = roomScene->sceneFile["environment"];
        }
        build.undressed.insert(build.undressed.end(), roomScene->undressed.begin(), roomScene->undressed.end());
        for (json& object : roomScene->sceneFile["objects"]) {
            const std::string local = object["id"].get<std::string>();
            object["id"] = floorObjectId(room.id, local);
            // The shell's doorway opens onto the hall: its stop is left out.
            if (object.value("prefab", "") == "room_shell") {
                json& doorway = object["params"]["doorway"];
                if (doorway.is_object()) {
                    doorway["stop"] = false;
                }
            }
            object["position"] = floorPoint(object["position"], room.placement);
            if (room.placement.yawHalfTurns % 2 != 0) {
                json rotation = object.contains("rotationDeg") ? object["rotationDeg"] : vec3Json(0.0, 0.0, 0.0);
                rotation[1] = floorYaw(rotation[1].get<double>(), room.placement);
                object["rotationDeg"] = std::move(rotation);
            }
            objects.push_back(std::move(object));
        }
        for (json& light : roomScene->sceneFile["lights"]) {
            light["name"] = floorObjectId(room.id, light.value("name", ""));
            light["position"] = floorPoint(light["position"], room.placement);
            if (light.contains("target")) {
                light["target"] = floorPoint(light["target"], room.placement);
            }
            light["area"] = room.id.toString();
            lights.push_back(std::move(light));
        }
    }

    const FloorSpec* spec = data.building.floor(floor);
    const std::string sign = spec != nullptr && !spec->sign.empty() ? spec->sign : std::string(floor);
    // The floor's pack dresses and tints the hall (I6 step 3, ADR-0014 §5).
    std::map<RoomId, std::string> roomSigns;
    for (const FloorRoom& entry : floorRooms) {
        roomSigns.emplace(entry.room->id, data.rooms.at(entry.room->id).platform.signText);
    }
    addHallObjects(build.hall, sign, data.hallTheme, roomSigns, objects);
    addHallLights(build.hall, themedLighting(data.lighting, data.hallTheme), lights);

    // The rooms' areas are lit as the scene (the profile's environment); the hall by the hall role's own terms (the
    // lobby by its own).
    json areas = json::array();
    const std::string hallArea = hallAreaId(floor);
    for (const FloorArea& area : floorAreas(build.hall, floorRooms)) {
        const AreaLook& hallLook =
            build.hall.lobby ? data.lighting.hall.lobbyEnvironment : data.lighting.hall.environment;
        areas.push_back(areaJson(area, area.id == hallArea ? &hallLook : nullptr));
    }

    // The floor's extent: the hall with its walls and every room's footprint.
    Footprint extent = build.hall.footprint();
    extent.minZMm -= build.hall.wallMm;
    extent.maxZMm += build.hall.wallMm;
    for (const FloorRoom& entry : floorRooms) {
        const Footprint footprint = floorFootprint(*entry.room, data.building.layout);
        extent.minXMm = std::min(extent.minXMm, footprint.minXMm);
        extent.minZMm = std::min(extent.minZMm, footprint.minZMm);
        extent.maxXMm = std::max(extent.maxXMm, footprint.maxXMm);
        extent.maxZMm = std::max(extent.maxZMm, footprint.maxZMm);
    }
    const double diagonal = std::hypot(meters(extent.maxXMm - extent.minXMm), meters(extent.maxZMm - extent.minZMm));
    const double far = std::max(kMinFarM, std::ceil(diagonal) + kFarMarginM);

    if (environment.empty()) {
        const LightingProfile& profile = data.lighting;
        environment = json{{"clearColor", profile.clearColor},
                           {"ambientSky", profile.ambientSky},
                           {"ambientGround", profile.ambientGround},
                           {"ambientIntensity", profile.ambientIntensity},
                           {"fillColor", profile.fillColor},
                           {"fillIntensity", profile.fillIntensity},
                           {"exposure", profile.exposure},
                           {"bloomStrength", profile.bloomStrength},
                           {"bloomRadius", profile.bloomRadius},
                           {"vignette", profile.vignette},
                           {"ssao", json{{"enabled", profile.ssaoEnabled},
                                         {"radius", profile.ssaoRadius},
                                         {"intensity", profile.ssaoIntensity}}}};
    }

    build.sceneFile = json{{"schemaVersion", scene::kSceneSchemaVersion},
                           {"name", "floor:" + std::string(floor)},
                           {"camera", json{{"position", vec3Json(meters(build.hall.arrival.xMm), kEyeHeight,
                                                                 meters(build.hall.arrival.zMm))},
                                           {"yawDeg", kFacingEastDeg},
                                           {"pitchDeg", kCameraPitchDeg},
                                           {"horizontalFovDeg", 75.0},
                                           {"near", 0.05},
                                           {"far", far}}},
                           {"environment", std::move(environment)},
                           {"areas", std::move(areas)},
                           {"lights", std::move(lights)},
                           {"objects", std::move(objects)}};
    return build;
}

Expected<scene::SceneDescription> toFloorSceneDescription(const WorldPlan& plan, std::string_view floor,
                                                          const FloorSceneData& data) {
    auto build = toFloorSceneFile(plan, floor, data);
    if (!build) {
        return propagate(build.error());
    }
    return scene::parseSceneDescription(build->sceneFile, "floor:" + std::string(floor));
}

CollectionLayout floorCollectionLayout(CollectionLayout layout, const RoomPlacement& placement) {
    const glm::mat4 toFloor = roomToFloorMatrix(placement);
    for (CollectionItem& item : layout.items) {
        item.slotFrame = toFloor * item.slotFrame;
        item.transform = toFloor * item.transform;
    }
    for (CollectionCluster& cluster : layout.clusters) {
        cluster.bounds = cluster.bounds.transformed(toFloor);
    }
    for (core::Aabb& occluder : layout.occluders) {
        occluder = occluder.transformed(toFloor);
    }
    return layout;
}

} // namespace rv::world
