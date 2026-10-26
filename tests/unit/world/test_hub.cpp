// I6 step 5 (ADR-0014 §3, style §5.2, docs/i6/look.md's hub row): the ground floor's hall is the hub. Its derived
// geometry (the 18 x 14 x 5 m hall, the balcony and its columns, the landmark, the totem and its zone, the favourites'
// showcase and its screen, the sofas, rugs and plants, the lights), the pieces kept off the doorways' approaches and
// apart, the passages the walker needs (the arcade's doorway, the totem's zone, the showcase's front), the walk from the
// elevator into the arcade room through the floor's collision, the hub's scene, the showcase's slots, the re-placement
// of a stored building's ground floor rooms (placeOnHub()), a longer hub and the building's hub reader.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/math.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/scene_areas.hpp>
#include <rv/scene/scene_geometry.hpp>
#include <rv/world/building.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/nav_grid.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_plan.hpp>

#include "world_building_support.hpp"
#include "world_generation_support.hpp"

using Catch::Approx;
using rv::world::Footprint;
using rv::world::Hall;
using rv::world::HubPiece;
using rv::world::HubPieceKind;
using rv::world::Room;
using rv::world::RoomPlacement;
using rv::world::WorldPlan;

namespace {

/// A 9 x 10 m room as I4's lobby placed it (rows from 3.0 m, the lobby 6 m wide; another floor's hall 3 m wide).
Room lobbyRoom(std::string_view id, std::string_view floor, std::int64_t x, std::uint32_t yaw) {
    Room room;
    room.id = *rv::world::RoomId::parse(id);
    room.floor = std::string(floor);
    room.minXMm = -4500;
    room.maxXMm = 4500;
    room.minZMm = -5000;
    room.maxZMm = 5000;
    room.heightMm = 3200;
    room.placement.xMm = x;
    room.placement.yawHalfTurns = yaw;
    const std::int64_t half = floor == rv::world::kGroundFloor ? 3000 : 1500;
    room.placement.zMm = yaw == 0 ? -half - 5150 : half + 5150;
    return room;
}

constexpr float kFixedDt = 0.016667f;

/// The golden building of the building fixture's systems (test_floor_scene.cpp generates and pins it).
WorldPlan goldenPlan() {
    const std::filesystem::path file = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" /
                                       "golden" / "building-esde-seed-102938.json";
    auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "golden");
    REQUIRE(plan.has_value());
    return std::move(*plan);
}

rv::world::PlatformTagsBySystem tags() {
    return {{*rv::catalog::SystemName::parse("pocketlab"), {"gb"}}};
}

rv::world::FloorSceneData groundData(const WorldPlan& plan) {
    auto data = rv::world::loadFloorSceneData(rv::test::world::assetRoot(), plan, rv::world::kGroundFloor, std::nullopt,
                                              tags());
    REQUIRE(data.has_value());
    return std::move(*data);
}

rv::world::FloorSceneBuild groundScene(const WorldPlan& plan, const rv::world::FloorSceneData& data) {
    auto build = rv::world::toFloorSceneFile(plan, rv::world::kGroundFloor, data);
    INFO((build ? std::string("built") : build.error().toString()));
    REQUIRE(build.has_value());
    return std::move(*build);
}

const nlohmann::json* findObject(const nlohmann::json& scene, std::string_view id) {
    for (const nlohmann::json& object : scene["objects"]) {
        if (object["id"].get<std::string>() == id) {
            return &object;
        }
    }
    return nullptr;
}

std::size_t countObjects(const nlohmann::json& scene, std::string_view prefix) {
    return static_cast<std::size_t>(std::count_if(scene["objects"].begin(), scene["objects"].end(),
                                                  [&](const nlohmann::json& object) {
                                                      return object["id"].get<std::string>().starts_with(prefix);
                                                  }));
}

/// The approach before each doorway that opens onto the hub (kHubApproachMm beside it, kHubApproachDepthMm deep).
std::vector<Footprint> approaches(const Hall& hall) {
    std::vector<Footprint> result;
    for (const rv::world::FloorDoorway& doorway : hall.doorways) {
        if (!doorway.opensOntoHall()) {
            continue;
        }
        const std::int64_t inward = doorway.hallEdgeZMm < 0 ? 1 : -1;
        const std::int64_t far = doorway.hallEdgeZMm + inward * rv::world::kHubApproachDepthMm;
        result.push_back({doorway.minMm - rv::world::kHubApproachMm, std::min(doorway.hallEdgeZMm, far),
                          doorway.maxMm + rv::world::kHubApproachMm, std::max(doorway.hallEdgeZMm, far)});
    }
    return result;
}

/// Every check the hub's pieces must pass on any hub: within the interior, no solid piece in a doorway's approach, the
/// solid pieces apart from each other, the columns on the balcony's edges, the walker's body (`bodyMm`) reaching the
/// doorways, the totem's zone and the showcase's front from the arrival point.
void checkHub(const Hall& hall, std::span<const rv::world::FloorRoom> rooms, const rv::world::BuildingLayout& layout,
              std::int64_t bodyMm) {
    REQUIRE(hall.hub.has_value());
    const rv::world::Hub& hub = *hall.hub;
    const std::vector<Footprint> doors = approaches(hall);
    std::set<std::string> ids;
    for (const HubPiece& piece : hub.pieces) {
        INFO(piece.id);
        CHECK(ids.insert(piece.id).second);
        CHECK(piece.footprint.minXMm >= hall.interior.minXMm);
        CHECK(piece.footprint.maxXMm <= hall.interior.maxXMm);
        CHECK(piece.footprint.minZMm >= hall.interior.minZMm);
        CHECK(piece.footprint.maxZMm <= hall.interior.maxZMm);
        if (piece.kind == HubPieceKind::Rug) {
            continue;
        }
        for (const Footprint& door : doors) {
            CHECK_FALSE(door.overlaps(piece.footprint));
        }
        if (piece.kind == HubPieceKind::Column) {
            const bool onLong = piece.centre.zMm == hub.northEdgeZMm || piece.centre.zMm == hub.southEdgeZMm;
            CHECK((onLong || piece.centre.xMm == hub.eastEdgeXMm));
        }
        if (!piece.solid()) {
            continue;
        }
        for (const HubPiece& other : hub.pieces) {
            if (&other != &piece && other.solid()) {
                INFO(other.id);
                CHECK_FALSE(other.footprint.overlaps(piece.footprint));
            }
        }
        CHECK_FALSE(piece.footprint.overlaps(hall.elevatorZone));
        CHECK_FALSE(piece.footprint.overlaps(hub.totemZone));
    }
    // The walk: the hall's walls (not those above the rooms), the elevator, the solid pieces and the rooms' walls.
    std::vector<Footprint> obstacles;
    for (const rv::world::HallWall& wall : hall.walls) {
        if (wall.bottomMm == 0) {
            obstacles.push_back(wall.footprint);
        }
    }
    obstacles.push_back(hall.elevator);
    for (const HubPiece& piece : hub.pieces) {
        if (piece.solid()) {
            obstacles.push_back(piece.footprint);
        }
    }
    Footprint extent = hall.footprint();
    for (const rv::world::FloorRoom& entry : rooms) {
        if (entry.room->floor != hall.floor || entry.roomTemplate == nullptr) {
            continue;
        }
        for (const Footprint& wall : rv::world::floorRoomWalls(*entry.room, *entry.roomTemplate)) {
            obstacles.push_back(wall);
        }
        const Footprint footprint = rv::world::floorFootprint(*entry.room, layout);
        extent = Footprint{std::min(extent.minXMm, footprint.minXMm), std::min(extent.minZMm, footprint.minZMm),
                           std::max(extent.maxXMm, footprint.maxXMm), std::max(extent.maxZMm, footprint.maxZMm)};
    }
    const rv::world::NavGrid grid(extent, obstacles, (bodyMm + 1) / 2);
    const auto start = grid.cellAt(hall.arrival.xMm, hall.arrival.zMm);
    REQUIRE(start.has_value());
    REQUIRE(grid.isFree(*start));
    const rv::world::NavReach reach(grid, *start);
    constexpr std::int64_t kNear = 400;
    for (const rv::world::FloorDoorway& doorway : hall.doorways) {
        INFO(doorway.room.toString());
        CHECK(reach.reachesNear(doorway.clearCentre.xMm, doorway.clearCentre.zMm, kNear));
    }
    if (hub.find("totem") != nullptr) {
        CHECK(reach.reachesNear((hub.totemZone.minXMm + hub.totemZone.maxXMm) / 2,
                                (hub.totemZone.minZMm + hub.totemZone.maxZMm) / 2, kNear));
    }
    if (const HubPiece* showcase = hub.find("showcase")) {
        CHECK(reach.reachesNear(showcase->centre.xMm, showcase->footprint.minZMm - rv::world::kHubShowcaseFrontMm,
                                kNear));
    }
}

} // namespace

TEST_CASE("world/hub: the ground floor's hall is the hub, 18 x 14 x 5 m, its balcony, columns and pieces",
          "[world][hub]") {
    const WorldPlan plan = goldenPlan();
    const rv::world::FloorSceneData data = groundData(plan);
    const rv::world::FloorSceneBuild build = groundScene(plan, data);
    const Hall& hall = build.hall;
    REQUIRE(hall.hub.has_value());
    const rv::world::Hub& hub = *hall.hub;
    CHECK(hall.lobby);
    CHECK(hall.interior == Footprint{0, -7000, 18000, 7000});
    CHECK(hall.heightMm == 5000);
    // The arcade room stands in the middle of the north wall, its doorway onto the hub; a wall of the hub's own above it.
    const auto arcade = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                     [](const Room& room) { return room.id.toString() == "area:arcade"; });
    REQUIRE(arcade != plan.rooms.end());
    CHECK(arcade->placement == RoomPlacement{9000, -12150, 0});
    REQUIRE(hall.doorways.size() == 1);
    CHECK(hall.doorways.front().opensOntoHall());
    CHECK((hall.doorways.front().minMm + hall.doorways.front().maxMm) / 2 == 9000);
    const auto upper = std::find_if(hall.walls.begin(), hall.walls.end(),
                                    [](const rv::world::HallWall& wall) { return wall.id == "north-upper-0"; });
    REQUIRE(upper != hall.walls.end());
    CHECK(upper->footprint == Footprint{4350, -7150, 13650, -7000});
    CHECK(upper->bottomMm == 3200);
    // The balcony's edges; the columns every 3 m along the long ones (none before the arcade's portal or the showcase),
    // at the east one's corners and either side of the entrance.
    CHECK(hub.northEdgeZMm == -5800);
    CHECK(hub.southEdgeZMm == 5800);
    CHECK(hub.eastEdgeXMm == 16800);
    CHECK(hub.centreXMm == 9000);
    std::set<std::string> columns;
    for (const HubPiece& piece : hub.pieces) {
        if (piece.kind == HubPieceKind::Column) {
            columns.insert(piece.id);
        }
    }
    // The showcase stands between the south columns at 6 and 9 m (I6 step 5's review: off the arcade portal's axis).
    CHECK(columns == std::set<std::string>{"column-north-3000", "column-north-6000", "column-north-12000",
                                           "column-north-15000", "column-south-3000", "column-south-6000",
                                           "column-south-9000", "column-south-12000", "column-south-15000",
                                           "column-east--5800", "column-east--2900", "column-east-2900",
                                           "column-east-5800"});
    // The landmark in the middle, the totem west of it facing the elevator, its zone before it; the showcase and its
    // screen in the middle of the south wall; two sofas facing the landmark; the plants and the rugs.
    const HubPiece* landmark = hub.find("landmark");
    REQUIRE(landmark != nullptr);
    CHECK(landmark->centre == rv::world::FloorPoint{9000, 0});
    CHECK(landmark->footprint == Footprint{6900, -2100, 11100, 2100});
    const HubPiece* totem = hub.find("totem");
    REQUIRE(totem != nullptr);
    CHECK(totem->yawCdeg == 27000);
    CHECK(hub.totemFace == rv::world::FloorPoint{totem->footprint.minXMm, totem->centre.zMm});
    CHECK(hub.totemZone.maxXMm == totem->footprint.minXMm);
    CHECK(hub.totemZone.maxXMm - hub.totemZone.minXMm == rv::world::kHubTotemZoneDepthMm);
    CHECK_FALSE(hub.totemZone.overlaps(hall.elevatorZone));
    const HubPiece* showcase = hub.find("showcase");
    REQUIRE(showcase != nullptr);
    CHECK(showcase->yawCdeg == 18000);
    CHECK(showcase->footprint.maxZMm == 7000 - rv::world::kHubShowcaseGapMm);
    REQUIRE(hub.find("screen") != nullptr);
    CHECK_FALSE(hub.find("screen")->solid());
    REQUIRE(hub.find("sofa-west") != nullptr);
    REQUIRE(hub.find("sofa-east") != nullptr);
    CHECK(hub.find("sofa-west")->yawCdeg == 18000);
    for (const std::string_view id : {"plant-elevator-north", "plant-elevator-south", "plant-entrance-north",
                                      "plant-entrance-south", "plant-showcase-west", "plant-showcase-east",
                                      "plant-north-west", "plant-north-east", "rug-north", "rug-sofa-west",
                                      "rug-sofa-east", "rug-runner-0"}) {
        INFO(id);
        CHECK(hub.find(id) != nullptr);
    }
    // A lit bookcase against each long wall in the middle of every column bay off the core's middle.
    std::set<std::string> bookcases;
    for (const HubPiece& piece : hub.pieces) {
        if (piece.kind == HubPieceKind::Bookcase) {
            bookcases.insert(piece.id);
            CHECK(piece.solid());
            CHECK((piece.footprint.minZMm == -7000 + rv::world::kHubBookcaseGapMm ||
                   piece.footprint.maxZMm == 7000 - rv::world::kHubBookcaseGapMm));
        }
    }
    CHECK(bookcases == std::set<std::string>{"bookcase-north-1500", "bookcase-north-4500", "bookcase-north-13500",
                                             "bookcase-north-16500", "bookcase-south-1500", "bookcase-south-4500",
                                             "bookcase-south-13500", "bookcase-south-16500"});
    // At least 1.4 m between the landmark and the sofas (style §6.2's main passage).
    CHECK(hub.find("sofa-west")->footprint.minZMm - landmark->footprint.maxZMm >= rv::world::kHallMinClearMm);
    // Lights: eight over the core and one by the entrance, four under the balcony; the dressing of its walls (no
    // bench), a sconce on the gallery's walls every bay.
    CHECK(hall.lights.size() == 9);
    CHECK(hall.lowLights.size() == 4);
    CHECK(std::none_of(hall.dressing.begin(), hall.dressing.end(), [](const rv::world::HallPiece& piece) {
        return piece.kind == rv::world::HallPieceKind::Bench || piece.kind == rv::world::HallPieceKind::Plant;
    }));
    const auto gallery = std::count_if(hall.dressing.begin(), hall.dressing.end(), [](const rv::world::HallPiece& piece) {
        return piece.id.starts_with("gallery-") && piece.centreMm == rv::world::kHubGallerySconceMm;
    });
    CHECK(gallery == 2 * 5);
    // No wall piece of the dressing behind the showcase, the screen or a bookcase.
    for (const rv::world::HallPiece& piece : hall.dressing) {
        if (piece.centreMm != 0) {
            continue;
        }
        INFO(piece.id);
        for (const HubPiece& other : hub.pieces) {
            const bool onWall = other.kind == HubPieceKind::Showcase || other.kind == HubPieceKind::Screen ||
                                other.kind == HubPieceKind::Bookcase;
            const bool sameWall = (other.centre.zMm < 0) == piece.north;
            if (onWall && sameWall) {
                INFO(other.id);
                CHECK((piece.footprint.maxXMm <= other.footprint.minXMm || piece.footprint.minXMm >= other.footprint.maxXMm));
            }
        }
    }
    // The other floors have no hub.
    for (const std::string_view floor : {"gen4", "handheld", "computer"}) {
        auto other = rv::world::loadFloorSceneData(rv::test::world::assetRoot(), plan, floor, std::nullopt, tags());
        REQUIRE(other.has_value());
        auto otherBuild = rv::world::toFloorSceneFile(plan, floor, *other);
        REQUIRE(otherBuild.has_value());
        CHECK_FALSE(otherBuild->hall.hub.has_value());
        CHECK(otherBuild->hall.lightDropMm == rv::world::kHallLightDropMm);
    }
}

TEST_CASE("world/hub: its pieces keep off the doorways' approaches and apart; the walker reaches the doorway, the "
          "totem and the showcase",
          "[world][hub]") {
    const WorldPlan plan = goldenPlan();
    const rv::world::FloorSceneData data = groundData(plan);
    const rv::world::FloorSceneBuild build = groundScene(plan, data);
    std::vector<rv::world::FloorRoom> rooms;
    for (const Room* room : rv::world::roomsOnFloor(plan, rv::world::kGroundFloor)) {
        rooms.push_back({room, &data.rooms.at(room->id).roomTemplate});
    }
    // The configuration's body (1.2 m) and style §6.2's 1.4 m passage.
    for (const std::int64_t body : {std::int64_t{1200}, rv::world::kHallMinClearMm}) {
        INFO("body " << body);
        checkHub(build.hall, rooms, data.building.layout, body);
    }
}

TEST_CASE("world/hub: the walker walks from the elevator past the landmark into the arcade room", "[world][hub]") {
    // hub_walk_into_arcade.json through the ground floor's collision world, from the scene's camera: never inside a
    // collider, in the hub north of the landmark at frame 230, in the arcade room at the end.
    const WorldPlan plan = goldenPlan();
    const rv::world::FloorSceneData data = groundData(plan);
    auto scene = rv::world::toFloorSceneDescription(plan, rv::world::kGroundFloor, data);
    REQUIRE(scene.has_value());
    auto materials = rv::assets::MaterialLibrary::loadFromFile(rv::test::world::assetRoot() / "materials" / "base" /
                                                               "materials.json");
    REQUIRE(materials.has_value());
    const rv::scene::CollisionWorld world =
        rv::scene::buildCollisionWorld(rv::scene::buildSceneGeometry(*scene, *materials));
    rv::scene::FirstPersonController controller;
    rv::scene::Camera camera;
    const glm::vec3 feet = scene->camera.position - glm::vec3(0.0f, controller.settings().eyeHeight, 0.0f);
    controller.teleport(feet, rv::core::degreesToRadians(scene->camera.yawDeg),
                        rv::core::degreesToRadians(scene->camera.pitchDeg));
    REQUIRE_FALSE(world.overlaps(controller.feetPosition(), controller.settings().capsule));
    auto script = rv::input::InputScript::loadFromFile(rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" /
                                                       "input_scripts" / "hub_walk_into_arcade.json");
    REQUIRE(script.has_value());
    rv::input::InputSystem system;
    rv::input::InputScriptPlayer player(*script);
    rv::scene::AreaLighting lighting(scene->areas, scene->lights, scene->camera.position, 16);
    std::vector<std::string> areas{scene->areas[*lighting.area()].id};
    for (std::uint64_t frame = 0; frame < 341; ++frame) {
        rv::input::ActionState actions = system.resolve(rv::input::RawInputFrame{});
        player.apply(frame, actions, system.settings().pressThreshold);
        controller.update(actions, kFixedDt, camera, &world);
        REQUIRE_FALSE(world.overlaps(controller.feetPosition(), controller.settings().capsule));
        lighting.update(scene->areas, scene->lights, camera.position());
        if (scene->areas[*lighting.area()].id != areas.back()) {
            areas.push_back(scene->areas[*lighting.area()].id);
        }
        if (frame == 230) {
            // Before the portal, the walk unhindered (9.2 m from the arrival point).
            CHECK(controller.feetPosition().x == Approx(9.0f).margin(0.02));
            CHECK(controller.feetPosition().z == Approx(-4.9f).margin(0.02));
        }
    }
    CHECK(controller.feetPosition().x == Approx(9.0f).margin(0.02));
    CHECK(controller.feetPosition().z < -7.15f - controller.settings().capsule.radius);
    CHECK(areas == std::vector<std::string>{"hall:ground", "area:arcade"});
}

TEST_CASE("world/hub: the hub's scene: the balcony, the columns, the coffers, the landmark, the totem, the showcase",
          "[world][hub]") {
    const WorldPlan plan = goldenPlan();
    const rv::world::FloorSceneData data = groundData(plan);
    const rv::world::FloorSceneBuild build = groundScene(plan, data);
    const nlohmann::json& scene = build.sceneFile;
    const std::string prefix = "hall:ground/hub/";
    // The balcony's three slabs and fascias, its railings set back behind the columns, which rise to the ceiling.
    for (const std::string_view side : {"north", "south", "east"}) {
        INFO(side);
        REQUIRE(findObject(scene, prefix + "balcony/" + std::string(side)) != nullptr);
        REQUIRE(findObject(scene, prefix + "fascia/" + std::string(side)) != nullptr);
        const nlohmann::json* railing = findObject(scene, prefix + "railing/" + std::string(side));
        REQUIRE(railing != nullptr);
        CHECK((*railing)["prefab"] == "railing");
        CHECK((*railing)["position"][1].get<double>() == Approx(rv::world::kHubBalconyFloorMm / 1000.0));
        CHECK_FALSE(railing->value("collides", true));
    }
    std::size_t columns = 0;
    for (const HubPiece& piece : build.hall.hub->pieces) {
        const nlohmann::json* object = findObject(scene, prefix + piece.id);
        INFO(piece.id);
        REQUIRE(object != nullptr);
        if (piece.kind == HubPieceKind::Column) {
            ++columns;
            CHECK((*object)["prefab"] == "column");
            CHECK((*object)["params"]["height"].get<double>() == Approx(5.0));
        }
        CHECK(object->value("collides", true) == piece.solid());
    }
    CHECK(columns == 13);
    // The bookcases: the lit shelf, a row of books on each of its six levels.
    const nlohmann::json* bookcase = findObject(scene, prefix + "bookcase-north-1500");
    REQUIRE(bookcase != nullptr);
    CHECK((*bookcase)["prefab"] == "shelf");
    CHECK((*bookcase)["params"] == rv::world::hubBookcaseParams());
    CHECK((*bookcase)["materials"]["board_light"] == "MAT_Shelf_Light");
    CHECK(countObjects(scene, prefix + "bookcase-north-1500/books-") == 6);
    CHECK(countObjects(scene, prefix + "bookcase-") == 8 * 7);
    // The coffered ceiling: beams across at the columns' x and the east balcony's edge, along at the edges and either
    // side of the middle; downlights in the coffers and under the balcony; the chandelier over the landmark.
    CHECK(countObjects(scene, prefix + "beam-x") == 6);
    CHECK(countObjects(scene, prefix + "beam-z") == 4);
    CHECK(countObjects(scene, prefix + "downlight-") > 10);
    CHECK(countObjects(scene, prefix + "soffit-") > 20);
    CHECK(countObjects(scene, prefix + "chandelier/") == 20 + 10 + 2 + 3);
    // The landmark: the hub_landmark prefab in the middle, its materials the pack's, its globe lit by its own material.
    const nlohmann::json* landmark = findObject(scene, prefix + "landmark");
    REQUIRE(landmark != nullptr);
    CHECK((*landmark)["prefab"] == "hub_landmark");
    CHECK((*landmark)["materials"]["globe"] == "MAT_Globe_Lines");
    CHECK((*landmark)["materials"]["globe_core"] == "MAT_Globe_Core");
    CHECK(countObjects(scene, prefix + "landmark/plant-") == 4);
    // Since the step's review: the globe lower and larger, more and larger consoles, a boxed game on each riser.
    CHECK((*landmark)["params"] == rv::world::hubLandmarkParams());
    CHECK(countObjects(scene, prefix + "landmark/box-") == 9);
    CHECK((*landmark)["materials"]["fabric"] == "MAT_Fabric_Display");
    // The totem: its screen the building's floors on the board, its plate's lettering.
    const nlohmann::json* totem = findObject(scene, prefix + "totem");
    REQUIRE(totem != nullptr);
    CHECK((*totem)["prefab"] == "info_totem");
    CHECK((*totem)["rotationDeg"][1].get<double>() == Approx(270.0));
    CHECK((*totem)["textures"]["screen"]["pattern"] == "board");
    const std::string board = (*totem)["textures"]["screen"]["title"].get<std::string>();
    // The plan's floors (the golden building's five) in the elevator's order.
    CHECK(board == "DIRECTORY|LOBBY AND ARCADE|16-BIT CONSOLES|32 AND 64-BIT CONSOLES|HANDHELDS|COMPUTERS AND MORE");
    // The showcase: a display case of two levels; the information screen above it.
    const nlohmann::json* showcase = findObject(scene, prefix + "showcase");
    REQUIRE(showcase != nullptr);
    CHECK((*showcase)["prefab"] == "display_case");
    CHECK((*showcase)["params"] == rv::world::hubShowcaseParams());
    const nlohmann::json* screen = findObject(scene, prefix + "screen");
    REQUIRE(screen != nullptr);
    CHECK((*screen)["textures"]["picture"]["pattern"] == "board");
    CHECK((*screen)["materials"]["picture"] == "MAT_Screen_Board");
    // The portals: the elevator's (its floor's sign proud of the entablature), the entrance's with its sign, the
    // arcade's sign again on the balcony's fascia.
    CHECK(findObject(scene, prefix + "portal:elevator/entablature") != nullptr);
    CHECK(findObject(scene, "hall:ground/sign")->at("position")[0].get<double>() == Approx(0.13));
    CHECK(findObject(scene, prefix + "portal:entrance/sign")->at("textures")["picture"]["title"] == "MAIN ENTRANCE");
    CHECK(findObject(scene, prefix + "fascia-sign:area:arcade")->at("textures")["picture"]["title"] == "ARCADE");
    // Since the step's review: the fascia's sign in its room's neon (the arcade pack's), the portal's own a small plaque;
    // the elevator's floor sign signage green, the entrance's a calm blue; large posters on the gallery's walls.
    const nlohmann::json* arcadeSign = findObject(scene, prefix + "fascia-sign:area:arcade");
    CHECK((*arcadeSign)["materials"]["picture"] == "MAT_Neon_Pink");
    CHECK((*arcadeSign)["textures"]["picture"]["pattern"] == "neon");
    CHECK(findObject(scene, "hall:ground/portal:area:arcade/sign")->at("params")["width"].get<double>() ==
          Approx(0.8));
    CHECK(findObject(scene, "hall:ground/sign")->at("materials")["picture"] == "MAT_Neon_Green");
    CHECK(findObject(scene, prefix + "portal:entrance/sign")->at("materials")["picture"] == "MAT_Neon_Blue");
    CHECK(countObjects(scene, prefix + "gallery-poster/") == 4 + 5 + 2 + 2); // the arcade's bay left out
    // Its lights in the hub's area: the core's, those under the balcony, the arcade doorway's fill; the hall's drum
    // pendants left out (lanterns instead) and its runner (the hub's rugs).
    std::size_t lights = 0;
    for (const nlohmann::json& light : scene["lights"]) {
        if (light["area"] == "hall:ground") {
            ++lights;
        }
    }
    // Since the step's review the globe has its own light, after the doorway's fill.
    CHECK(lights == 9 + 4 + 1 + 1);
    const auto lightNamed = [&](const std::string& name) -> const nlohmann::json* {
        for (const nlohmann::json& light : scene["lights"]) {
            if (light["name"] == name) {
                return &light;
            }
        }
        return nullptr;
    };
    const nlohmann::json* globeLight = lightNamed("hall:ground/globe");
    REQUIRE(globeLight != nullptr);
    CHECK((*globeLight)["color"] == "#4C8BC9");
    CHECK((*globeLight)["position"][1].get<double>() == Approx(1.92).margin(0.01));
    CHECK(scene["lights"].back()["name"] == "hall:ground/globe");
    CHECK(countObjects(scene, "hall:ground/lamp-") == 0);
    CHECK(countObjects(scene, "hall:ground/runner-") == 0);
    CHECK(countObjects(scene, prefix + "lantern-") == 2 * 8); // a bulb and its stem each, the chandelier's light none
}

TEST_CASE("world/hub: the showcase's slots: twelve face-out slots of the display case, front level first",
          "[world][hub]") {
    const WorldPlan plan = goldenPlan();
    const rv::world::FloorSceneData data = groundData(plan);
    const rv::world::FloorSceneBuild build = groundScene(plan, data);
    const rv::world::HubShowcaseSlots slots = rv::world::hubShowcaseSlots(build.hall);
    REQUIRE(slots.frames.size() == rv::world::kHubShowcaseSlots);
    CHECK(slots.size.x == Approx(0.135f));
    CHECK(slots.size.y == Approx(0.19f));
    const HubPiece& showcase = *build.hall.hub->find("showcase");
    for (std::size_t i = 0; i < slots.frames.size(); ++i) {
        INFO(i);
        const glm::vec3 origin(slots.frames[i][3]);
        // Inside the case's footprint, above its base; its face out towards the hub (north, -Z).
        CHECK(origin.x * 1000.0f > static_cast<float>(showcase.footprint.minXMm));
        CHECK(origin.x * 1000.0f < static_cast<float>(showcase.footprint.maxXMm));
        CHECK(origin.z * 1000.0f > static_cast<float>(showcase.footprint.minZMm));
        CHECK(origin.z * 1000.0f < static_cast<float>(showcase.footprint.maxZMm));
        CHECK(origin.y > 0.5f);
        CHECK(glm::vec3(slots.frames[i][2]).z < -0.5f);
    }
    // The front level first, left to right as one faces the case (seen from the north: west to east is right to left).
    CHECK(slots.frames[0][3].z < slots.frames[6][3].z);
    CHECK(slots.frames[0][3].x > slots.frames[5][3].x);
    // No showcase on another floor.
    CHECK(rv::world::hubShowcaseSlots(Hall{}).frames.empty());
}

TEST_CASE("world/hub: a stored building's ground floor rooms at I4's lobby are re-placed once onto the hub",
          "[world][hub][building]") {
    const rv::world::BuildingLayout layout = rv::test::world::shippedWorld(102938).building.layout;
    // Three rooms of the ground floor as I4's lobby placed them (rows from 3.0 m, the lobby 6 m wide) and one of gen4.
    const auto roomAt = lobbyRoom;
    std::vector<Room> rooms{roomAt("area:arcade", "ground", 7650, 0), roomAt("room:snes", "gen4", 7650, 0),
                            roomAt("area:arcade:2", "ground", 7650, 1), roomAt("area:arcade:3", "ground", 16950, 0)};
    for (const Room& room : rooms) {
        CHECK(rv::world::standsOnHub(room, layout) == (room.floor != "ground"));
    }
    const std::vector<Room> before = rooms;
    const std::vector<rv::world::ReplacedRoom> replaced = rv::world::placeOnHub(rooms, layout);
    // West to east by where they stood (the north row first), each at the east end of its own row on the hub, its z
    // abutting the hub's 14 m: the first of each row in the middle of an 18 m wall.
    REQUIRE(replaced.size() == 3);
    CHECK(rooms[0].placement == RoomPlacement{9000, -12150, 0});
    CHECK(rooms[2].placement == RoomPlacement{9000, 12150, 1});
    CHECK(rooms[3].placement == RoomPlacement{18300, -12150, 0});
    CHECK(rooms[1] == before[1]); // another floor's room untouched
    for (const rv::world::ReplacedRoom& entry : replaced) {
        const auto room = std::find_if(rooms.begin(), rooms.end(), [&](const Room& r) { return r.id == entry.room; });
        const auto old = std::find_if(before.begin(), before.end(), [&](const Room& r) { return r.id == entry.room; });
        CHECK(entry.from == old->placement);
        CHECK(entry.to == room->placement);
        CHECK(entry.to.yawHalfTurns == entry.from.yawHalfTurns);
        CHECK(rv::world::standsOnHub(*room, layout));
    }
    CHECK(std::is_sorted(replaced.begin(), replaced.end()));
    // Once: the next call moves nothing.
    CHECK(rv::world::placeOnHub(rooms, layout).empty());
}

TEST_CASE("world/hub: a mixed ground floor: the room at I4's lobby joins its row after the room on the hub",
          "[world][hub][building]") {
    const rv::world::BuildingLayout layout = rv::test::world::shippedWorld(102938).building.layout;
    // The arcade room already on the hub (the middle of the north wall), another north room still at I4's lobby.
    std::vector<Room> rooms{lobbyRoom("area:arcade", "ground", 7650, 0), lobbyRoom("area:arcade:2", "ground", 7650, 0)};
    rooms[0].placement = RoomPlacement{9000, -12150, 0};
    REQUIRE(rv::world::standsOnHub(rooms[0], layout));
    REQUIRE_FALSE(rv::world::standsOnHub(rooms[1], layout));
    const Room standing = rooms[0];
    std::vector<rv::world::RoomId> refused;
    const std::vector<rv::world::ReplacedRoom> replaced = rv::world::placeOnHub(rooms, layout, &refused);
    CHECK(refused.empty());
    REQUIRE(replaced.size() == 1);
    CHECK(replaced.front().room == rooms[1].id);
    CHECK(rooms[0] == standing); // the room on the hub never moves
    CHECK(rooms[1].placement == RoomPlacement{18300, -12150, 0}); // its own row's east end, its yaw kept
    CHECK(rv::world::standsOnHub(rooms[1], layout));
    CHECK(rv::world::placeOnHub(rooms, layout).empty());
}

TEST_CASE("world/hub: a room whose own row on the hub is full is refused, never turned to the other row; nothing moves",
          "[world][hub][building]") {
    const rv::world::BuildingLayout layout = rv::test::world::shippedWorld(102938).building.layout;
    // maxRoomsPerRow + 1 north rooms at I4's lobby (the south row empty) and one south room.
    std::vector<Room> rooms;
    for (std::uint32_t i = 0; i <= layout.maxRoomsPerRow; ++i) {
        rooms.push_back(lobbyRoom("area:shared:n" + std::to_string(i), "ground", 7650 + 9300 * std::int64_t{i}, 0));
    }
    rooms.push_back(lobbyRoom("area:shared:s0", "ground", 7650, 1));
    const std::vector<Room> before = rooms;
    std::vector<rv::world::RoomId> refused;
    CHECK(rv::world::placeOnHub(rooms, layout, &refused).empty());
    // The easternmost north room finds its row full: refused by id, every room where it stood (all or nothing).
    REQUIRE(refused.size() == 1);
    CHECK(refused.front() == rooms[layout.maxRoomsPerRow].id);
    CHECK(rooms == before);
}

TEST_CASE("world/hub: a longer hub grows east with its rows; a doorway's approach takes the showcase's place",
          "[world][hub]") {
    // The golden building's arcade room and two more rooms of its template on the hub: the second the south row's first
    // place (its doorway in the middle of the south wall, where the showcase stood), the third the north row's next
    // (east of 18 m: the hub grows).
    WorldPlan plan = goldenPlan();
    rv::world::FloorSceneData data = groundData(plan);
    const Room arcade = *std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                      [](const Room& room) { return room.id.toString() == "area:arcade"; });
    for (const std::string_view id : {"area:arcade:2", "area:arcade:3"}) {
        Room clone = arcade;
        clone.id = *rv::world::RoomId::parse(id);
        clone.placement = *rv::world::placeEnteringRoom(
            clone, rv::world::floorRows(plan.rooms, "ground", data.building.layout), data.building.layout);
        plan.rooms.push_back(clone);
        data.rooms.emplace(clone.id, data.rooms.at(arcade.id));
    }
    CHECK(plan.rooms.back().placement == RoomPlacement{18300, -12150, 0});
    std::vector<rv::world::FloorRoom> rooms;
    for (const Room* room : rv::world::roomsOnFloor(plan, rv::world::kGroundFloor)) {
        rooms.push_back({room, &data.rooms.at(room->id).roomTemplate});
    }
    const Hall hall = rv::world::deriveHall(rv::world::kGroundFloor, rooms, data.building.layout);
    REQUIRE(hall.hub.has_value());
    CHECK(hall.interior.maxXMm == 18300 + 4650);
    CHECK(hall.hub->eastEdgeXMm == 18300 + 4650 - 1200);
    CHECK(hall.doorways.size() == 3);
    // The showcase and its screen gave way to the south room's doorway; the core's other pieces stand where they did.
    CHECK(hall.hub->find("showcase") == nullptr);
    CHECK(hall.hub->find("screen") == nullptr);
    CHECK(hall.hub->find("landmark") != nullptr);
    CHECK(hall.hub->find("totem") != nullptr);
    CHECK(hall.hub->find("column-north-18000") == nullptr); // before the third room's portal
    CHECK(hall.hub->find("column-south-18000") != nullptr); // appended along the longer edge
    checkHub(hall, rooms, data.building.layout, 1200);
    // The scene builds, its validation's hall rules hold.
    auto scene = rv::world::toFloorSceneFile(plan, rv::world::kGroundFloor, data);
    REQUIRE(scene.has_value());
    CHECK(findObject(scene->sceneFile, "hall:ground/hub/showcase") == nullptr);
}

TEST_CASE("world/hub: building.json's hub: defaults, the shipped size and refusals", "[world][hub][building]") {
    const std::string floors = R"({"schemaVersion": 1, "floors": [{"id": "ground", "kinds": ["arcade"]},
        {"id": "home", "kinds": ["home", "handheld"]}, {"id": "pc", "kinds": ["computer", "other"]}],
        "layout": {"hallWidthMm": 3000, "lobbyWidthMm": 6000, "rowStartXMm": 3000, "wallAllowanceMm": 150,
                   "maxRoomsPerRow": 12)";
    auto defaults = rv::world::parseBuildingData(floors + "}}", "building.json");
    REQUIRE(defaults.has_value());
    CHECK(defaults->layout.hub == rv::world::HubLayout{});
    CHECK(defaults->layout.hub == rv::world::HubLayout{18000, 14000, 5000, 4350});
    auto custom = rv::world::parseBuildingData(
        floors + R"(, "hub": {"lengthMm": 20000, "widthMm": 16000, "heightMm": 6000, "rowStartXMm": 5000}}})",
        "building.json");
    REQUIRE(custom.has_value());
    CHECK(custom->layout.hub == rv::world::HubLayout{20000, 16000, 6000, 5000});
    for (const std::string_view bad :
         {R"(, "hub": {"lengthMm": 9000, "widthMm": 14000, "heightMm": 5000, "rowStartXMm": 4350}}})",
          R"(, "hub": {"lengthMm": 18000, "widthMm": 14001, "heightMm": 5000, "rowStartXMm": 4350}}})",
          R"(, "hub": {"lengthMm": 18000, "widthMm": 14000, "heightMm": 3000, "rowStartXMm": 4350}}})",
          R"(, "hub": {"lengthMm": 18000, "widthMm": 14000, "heightMm": 5000}}})",
          R"(, "hub": {"lengthMm": 18000, "widthMm": 14000, "heightMm": 5000, "rowStartXMm": 4350, "x": 1}}})"}) {
        INFO(bad);
        CHECK_FALSE(rv::world::parseBuildingData(floors + std::string(bad), "building.json").has_value());
    }
    // The shipped building.json: the hub of ADR-0014 §3; the inputDigest's document leaves it out (the other floors'
    // plans keep their provenance).
    const rv::world::BuildingData shipped = rv::test::world::shippedWorld(102938).building;
    CHECK(shipped.layout.hub == rv::world::HubLayout{18000, 14000, 5000, 4350});
    CHECK_FALSE(rv::world::buildingDataToJson(shipped)["layout"].contains("hub"));
    CHECK(rv::world::hubLayoutToJson(shipped.layout.hub) ==
          nlohmann::json{{"heightMm", 5000}, {"lengthMm", 18000}, {"rowStartXMm", 4350}, {"widthMm", 14000}});
}
