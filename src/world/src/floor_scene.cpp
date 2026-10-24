#include <rv/world/floor_scene.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

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
/// slabs this thick outside the interior.
constexpr double kMetersPerTile = 2.0;
constexpr std::int64_t kSlabMm = 20;
/// The hall's materials (library names): an oak-plank floor, painted plaster, the elevator's graphite frame and brushed
/// doors, a black call panel with a lit button, the lamps' shades, the lobby's walnut and varnished entrance.
constexpr std::string_view kHallFloorMaterial = "MAT_Floor_OakPlanks";
constexpr std::string_view kHallWallMaterial = "MAT_Wall_Painted";
constexpr std::string_view kElevatorFrameMaterial = "MAT_Metal_Graphite";
constexpr std::string_view kElevatorDoorMaterial = "MAT_Metal_Brushed";
constexpr std::string_view kPanelMaterial = "MAT_Plastic_Black";
constexpr std::string_view kButtonMaterial = "MAT_Screen_Emissive";
constexpr std::string_view kLampMaterial = "MAT_Lamp_Shade";
constexpr std::string_view kEntranceFrameMaterial = "MAT_Wood_Walnut";
constexpr std::string_view kEntranceDoorMaterial = "MAT_Wood_Varnished";
/// The lamps under the hall's lights (flush with the ceiling) and the elevator's call panel and button, millimetres.
constexpr std::int64_t kLampSideMm = 500;
constexpr std::int64_t kLampDepthMm = 40;
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

/// A box primitive: `footprint` on the floor from `bottomMm` to `topMm`, of `material`, uvs in meters.
json boxObject(std::string id, const Footprint& footprint, std::int64_t bottomMm, std::int64_t topMm,
               std::string_view material, bool collides, bool castsShadows) {
    const double centreX = static_cast<double>(footprint.minXMm + footprint.maxXMm) / 2000.0;
    const double centreY = static_cast<double>(bottomMm + topMm) / 2000.0;
    const double centreZ = static_cast<double>(footprint.minZMm + footprint.maxZMm) / 2000.0;
    json object{{"id", std::move(id)},
                {"primitive", "box"},
                {"params", json{{"size", vec3Json(meters(footprint.maxXMm - footprint.minXMm), meters(topMm - bottomMm),
                                                  meters(footprint.maxZMm - footprint.minZMm))},
                                {"uv", "meters"},
                                {"metersPerTile", kMetersPerTile}}},
                {"position", vec3Json(centreX, centreY, centreZ)},
                {"material", std::string(material)}};
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

/// The hall's objects (see the header): its floor, ceiling and walls, its lamps, the elevator, the sign and the lobby's
/// entrance.
void addHallObjects(const Hall& hall, std::string_view sign, const ThemeMaterials& theme, json& objects) {
    const std::string prefix = hallAreaId(hall.floor) + "/";
    const std::int64_t height = hall.heightMm;
    objects.push_back(boxObject(prefix + "floor", hall.interior, -kSlabMm, 0, kHallFloorMaterial, false, false));
    objects.push_back(
        boxObject(prefix + "ceiling", hall.interior, height, height + kSlabMm, kHallWallMaterial, false, false));
    for (const HallWall& wall : hall.walls) {
        objects.push_back(boxObject(prefix + "wall-" + wall.id, wall.footprint, 0, height, kHallWallMaterial, true, true));
    }
    for (std::size_t i = 0; i < hall.lights.size(); ++i) {
        const FloorPoint& light = hall.lights[i];
        const Footprint lamp{light.xMm - kLampSideMm / 2, light.zMm - kLampSideMm / 2, light.xMm + kLampSideMm / 2,
                             light.zMm + kLampSideMm / 2};
        objects.push_back(boxObject(prefix + "lamp-" + std::to_string(i), lamp, height - kLampDepthMm, height,
                                    kLampMaterial, false, false));
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
    json materials = json::object();
    for (const auto& [role, material] : theme.materials("poster_frame", "sign")) {
        materials[role] = material;
    }
    signObject["materials"] = std::move(materials);
    objects.push_back(std::move(signObject));
    // The lobby's entrance at its east end: two closed leaves in a frame (decoration).
    if (hall.lobby) {
        const Footprint& door = hall.entrance;
        const std::int64_t east = hall.interior.maxXMm;
        const std::int64_t inner = east - kElevatorFrameDepthMm;
        const std::int64_t half = kEntranceWidthMm / 2;
        objects.push_back(boxObject(prefix + "entrance/frame-north", {inner, door.minZMm - kElevatorFrameMm, east, -half},
                                    0, kEntranceHeightMm + kElevatorFrameMm, kEntranceFrameMaterial, true, true));
        objects.push_back(boxObject(prefix + "entrance/frame-south", {inner, half, east, door.maxZMm + kElevatorFrameMm},
                                    0, kEntranceHeightMm + kElevatorFrameMm, kEntranceFrameMaterial, true, true));
        objects.push_back(boxObject(prefix + "entrance/frame-head", {inner, -half, east, half}, kEntranceHeightMm,
                                    kEntranceHeightMm + kElevatorFrameMm, kEntranceFrameMaterial, true, true));
        objects.push_back(boxObject(prefix + "entrance/door-north", {east - kDoorLeafDepthMm, -half, east, -kDoorGapMm / 2},
                                    0, kEntranceHeightMm, kEntranceDoorMaterial, true, true));
        objects.push_back(boxObject(prefix + "entrance/door-south", {east - kDoorLeafDepthMm, kDoorGapMm / 2, east, half},
                                    0, kEntranceHeightMm, kEntranceDoorMaterial, true, true));
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
    // The rooms' theme dresses the sign; a floor without rooms takes the built-in one.
    const ThemeMaterials& theme = floorRooms.empty() ? builtInThemeMaterials()
                                                     : data.rooms.at(floorRooms.front().room->id).theme;
    addHallObjects(build.hall, sign, theme, objects);
    addHallLights(build.hall, data.lighting, lights);

    // The rooms' areas are lit as the scene (the profile's environment); the hall by the hall role's own terms.
    json areas = json::array();
    const std::string hallArea = hallAreaId(floor);
    for (const FloorArea& area : floorAreas(build.hall, floorRooms)) {
        areas.push_back(areaJson(area, area.id == hallArea ? &data.lighting.hall.environment : nullptr));
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
