// The floors of the building (rv/world/floor_layout.hpp, floor_scene.hpp; I4 step 3, ADR-0013 §6, §7, §9): the golden
// building of the building fixture's systems (tests/fixtures/esde/building) compared byte for byte in Debug and
// Release, its floors' scenes as golden digests (rewritten only with RV_UPDATE_GOLDEN=1), the halls derived from the
// rooms (walls where no room stands, lights at a fixed pitch, doorways onto the hall), each room's scene moved onto the
// floor, the areas, walks from the elevator into a north-row and a south-row (half-turned) room through the floor's
// collision, and a room's collection in the floor's frame.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/sha256.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/scene_areas.hpp>
#include <rv/scene/scene_geometry.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/validation.hpp>

#include "plan_scene_support.hpp"
#include "world_generation_support.hpp"

using Catch::Approx;
using rv::catalog::SystemName;
using rv::world::Footprint;
using rv::world::Hall;
using rv::world::RoomId;
using rv::world::WorldPlan;

namespace {

constexpr std::uint64_t kWorldSeed = 102938;
constexpr float kFixedDt = 0.016667f;

const std::filesystem::path kGoldenPlanDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "golden";
const std::filesystem::path kSceneGoldenDir =
    rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "scene";
constexpr std::string_view kGoldenBuilding = "building-esde-seed-102938";

bool updatingGoldens() {
    const char* value = std::getenv("RV_UPDATE_GOLDEN");
    return value != nullptr && std::string_view(value) == "1";
}

/// Compares `text` with the golden file `path` (or rewrites it with RV_UPDATE_GOLDEN=1).
void checkGolden(const std::filesystem::path& path, const std::string& text) {
    if (updatingGoldens()) {
        REQUIRE(rv::core::writeTextFileAtomic(path, text).has_value());
        WARN("rewrote " << rv::core::pathToUtf8(path));
        return;
    }
    auto expected = rv::core::readTextFile(path);
    INFO(rv::core::pathToUtf8(path) << " (RV_UPDATE_GOLDEN=1 rewrites it)");
    REQUIRE(expected.has_value());
    CHECK(*expected == text);
}

std::string canonical(const nlohmann::json& value) {
    auto text = rv::core::writeCanonicalJson(value);
    REQUIRE(text.has_value());
    return std::move(*text);
}

SystemName name(std::string_view text) {
    return *SystemName::parse(text);
}

/// The systems of the building fixture (tests/fixtures/esde/building/README.md) with the titles its rooms place there
/// and its distinct titles over all its games (mame's 10 sets without a cover counted; 0: the titles); pocketlab, a
/// custom system, with gb's ES-DE platform tag (kodi is excluded: no room).
struct SystemCase {
    std::string_view system;
    std::size_t titles = 0;
    std::vector<std::string> tags;
    std::size_t distinctTitles = 0;
};

const std::vector<SystemCase>& fixtureSystems() {
    static const std::vector<SystemCase> systems{{"amiga", 20, {}},        {"gb", 30, {}},       {"mame", 15, {}, 25},
                                                {"pocketlab", 6, {"gb"}}, {"pokemini", 8, {}}, {"psx", 40, {}},
                                                {"snes", 40, {}}};
    return systems;
}

struct GoldenBuilding {
    WorldPlan plan;
    std::string text;
    rv::world::ValidationReport validation;
    rv::world::PlatformTagsBySystem tags;
};

/// The building of the fixture's systems (synthetic selections, makeSelection(), one seed each), generated anew.
GoldenBuilding generateGoldenBuilding() {
    rv::world::BuildingInput input;
    input.library = rv::test::world::library();
    input.config.seed = kWorldSeed;
    std::vector<rv::world::WorldData> rooms;
    GoldenBuilding result;
    std::uint64_t seed = 4000;
    for (const SystemCase& entry : fixtureSystems()) {
        rv::world::RoomDataRequest request;
        request.system = name(entry.system);
        request.platformTags = entry.tags;
        auto data = rv::world::loadWorldData(rv::test::world::assetRoot(), request);
        INFO(entry.system << ": " << (data ? std::string("loaded") : data.error().toString()));
        REQUIRE(data.has_value());
        rooms.push_back(std::move(*data));
        rv::world::RoomSelection selection = rv::test::world::makeSelection(entry.titles, seed++);
        selection.distinctTitles = std::max(entry.distinctTitles, entry.titles);
        selection.room = *RoomId::forSystem(name(entry.system));
        for (rv::world::UnplacedGame& game : selection.unplaced) {
            game.room = selection.room;
        }
        input.selections.push_back(std::move(selection));
        if (!entry.tags.empty()) {
            input.platformTags[name(entry.system)] = entry.tags;
            result.tags[name(entry.system)] = entry.tags;
        }
    }
    auto generated = rv::world::generateBuilding(input, rooms.front().building, rooms.front().systems, rooms);
    INFO((generated ? std::string("generated") : generated.error().toString()));
    REQUIRE(generated.has_value());
    result.plan = generated->plan;
    result.validation = generated->validation;
    auto text = rv::world::writePlan(result.plan);
    REQUIRE(text.has_value());
    result.text = std::move(*text);
    return result;
}

const GoldenBuilding& goldenBuilding() {
    static const GoldenBuilding building = generateGoldenBuilding();
    return building;
}

rv::world::FloorSceneData floorData(const WorldPlan& plan, std::string_view floor,
                                    const rv::world::PlatformTagsBySystem& tags) {
    auto data = rv::world::loadFloorSceneData(rv::test::world::assetRoot(), plan, floor, std::nullopt, tags);
    INFO(floor << ": " << (data ? std::string("loaded") : data.error().toString()));
    REQUIRE(data.has_value());
    return std::move(*data);
}

rv::world::FloorSceneBuild floorScene(const WorldPlan& plan, std::string_view floor,
                                      const rv::world::FloorSceneData& data) {
    auto build = rv::world::toFloorSceneFile(plan, floor, data);
    INFO(floor << ": " << (build ? std::string("built") : build.error().toString()));
    REQUIRE(build.has_value());
    return std::move(*build);
}

const rv::world::Room& roomOf(const WorldPlan& plan, std::string_view id) {
    const auto room = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                   [&](const rv::world::Room& candidate) { return candidate.id.toString() == id; });
    REQUIRE(room != plan.rooms.end());
    return *room;
}

const nlohmann::json* findObject(const nlohmann::json& scene, std::string_view id) {
    for (const nlohmann::json& object : scene["objects"]) {
        if (object["id"].get<std::string>() == id) {
            return &object;
        }
    }
    return nullptr;
}

std::int64_t mm(double meters) {
    return static_cast<std::int64_t>(std::llround(meters * 1000.0));
}

/// The length of [low, high] that `footprints` (intervals along x) cover.
std::int64_t covered(std::int64_t low, std::int64_t high, std::vector<std::pair<std::int64_t, std::int64_t>> spans) {
    std::sort(spans.begin(), spans.end());
    std::int64_t total = 0;
    std::int64_t cursor = low;
    for (const auto& [from, to] : spans) {
        const std::int64_t start = std::max(from, cursor);
        const std::int64_t end = std::min(to, high);
        if (end > start) {
            total += end - start;
            cursor = end;
        }
    }
    return total;
}

} // namespace

TEST_CASE("world/floor: the golden building of the building fixture's systems, two runs alike", "[world][floor]") {
    const GoldenBuilding& building = goldenBuilding();
    INFO(building.validation.findings.size() << " finding(s)");
    for (const rv::world::ValidationFinding& finding : building.validation.findings) {
        UNSCOPED_INFO(finding.rule << " " << finding.subject << ": " << finding.message);
    }
    CHECK(building.validation.passed());
    // Three console rooms and three rooms of sections on five floors (I4 step 4, ADR-0013 §3): mame in the arcade room,
    // the custom pocketlab (gb's row, so before pokemini) and pokemini in the handheld floor's shared room, amiga's 20
    // titles under the threshold of 30 in the computer floor's.
    std::vector<std::string> floors;
    for (const rv::world::Floor& floor : building.plan.floors) {
        floors.push_back(floor.id);
    }
    CHECK(floors == std::vector<std::string>{"ground", "gen4", "gen5", "handheld", "computer"});
    std::map<std::string, std::string> roomFloors;
    std::map<std::string, std::vector<std::string>> roomSections;
    for (const rv::world::Room& room : building.plan.rooms) {
        roomFloors[room.id.toString()] = room.floor;
        for (const SystemName& system : room.sections) {
            roomSections[room.id.toString()].push_back(system.str());
        }
    }
    CHECK(roomFloors == std::map<std::string, std::string>{{"area:arcade", "ground"},
                                                           {"area:shared:computer", "computer"},
                                                           {"area:shared:handheld", "handheld"},
                                                           {"room:gb", "handheld"},
                                                           {"room:psx", "gen5"},
                                                           {"room:snes", "gen4"}});
    CHECK(roomSections == std::map<std::string, std::vector<std::string>>{
                              {"area:arcade", {"mame"}},
                              {"area:shared:computer", {"amiga"}},
                              {"area:shared:handheld", {"pocketlab", "pokemini"}},
                              {"room:gb", {"gb"}},
                              {"room:psx", {"psx"}},
                              {"room:snes", {"snes"}}});
    CHECK(generateGoldenBuilding().text == building.text);
    checkGolden(kGoldenPlanDir / (std::string(kGoldenBuilding) + ".json"), building.text);
}

TEST_CASE("world/floor: golden digests of every floor's scene, two runs alike", "[world][floor]") {
    const GoldenBuilding& building = goldenBuilding();
    nlohmann::json digests = nlohmann::json::object();
    for (const rv::world::Floor& floor : building.plan.floors) {
        INFO(floor.id);
        const rv::world::FloorSceneData data = floorData(building.plan, floor.id, building.tags);
        const rv::world::FloorSceneBuild build = floorScene(building.plan, floor.id, data);
        const std::string text = rv::world::sceneFileText(build.sceneFile);
        CHECK(rv::world::sceneFileText(floorScene(building.plan, floor.id, data).sceneFile) == text);
        // The file reads back as the scene the app instantiates.
        auto description = rv::world::toFloorSceneDescription(building.plan, floor.id, data);
        INFO((description ? std::string("parsed") : description.error().toString()));
        REQUIRE(description.has_value());
        CHECK(description->areas.size() == build.rooms.size() + 1);
        digests[floor.id] = nlohmann::json{{"scene", rv::core::sha256Hex(text)},
                                           {"objects", build.sceneFile["objects"].size()},
                                           {"lights", build.sceneFile["lights"].size()},
                                           {"areas", build.sceneFile["areas"].size()},
                                           {"rooms", build.rooms.size()},
                                           {"hallLights", build.hall.lights.size()},
                                           {"hallWalls", build.hall.walls.size()},
                                           {"undressed", build.undressed.size()}};
        if (floor.id == "gen4") {
            checkGolden(kSceneGoldenDir / (std::string(kGoldenBuilding) + ".gen4.scene.json"), text);
        }
    }
    checkGolden(kSceneGoldenDir / (std::string(kGoldenBuilding) + ".floors.json"), canonical(digests));
}

TEST_CASE("world/floor: the hall is derived from its rooms: walls where none stands, lights at a fixed pitch",
          "[world][floor]") {
    const GoldenBuilding& building = goldenBuilding();
    const WorldPlan& plan = building.plan;
    for (const std::string floor : {"ground", "gen4", "handheld"}) {
        INFO(floor);
        const rv::world::FloorSceneData data = floorData(plan, floor, building.tags);
        const Hall hall = floorScene(plan, floor, data).hall;
        const std::int64_t half = floor == "ground" ? 3000 : 1500;
        CHECK(hall.lobby == (floor == "ground"));
        CHECK(hall.widthMm == 2 * half);
        CHECK(hall.heightMm == 3200);
        CHECK(hall.interior.minXMm == 0);
        CHECK(hall.interior.minZMm == -half);
        CHECK(hall.interior.maxZMm == half);
        // The east end is the easternmost room's edge (the rooms are 9.3 m wide with their walls, from x 3.0 m).
        std::int64_t east = rv::world::kMinHallLengthMm;
        std::vector<std::pair<std::int64_t, std::int64_t>> north;
        std::vector<std::pair<std::int64_t, std::int64_t>> south;
        for (const rv::world::Room* room : rv::world::roomsOnFloor(plan, floor)) {
            const Footprint footprint = rv::world::floorFootprint(*room, data.building.layout);
            east = std::max(east, footprint.maxXMm);
            // Every room abuts the hall: its footprint touches the hall's edge and does not enter it.
            CHECK_FALSE(footprint.overlaps(hall.footprint()));
            CHECK((footprint.maxZMm == -half || footprint.minZMm == half));
            (rv::world::rowOf(room->placement) == rv::world::Row::North ? north : south)
                .emplace_back(footprint.minXMm, footprint.maxXMm);
        }
        CHECK(hall.interior.maxXMm == east);
        // Along each edge the rooms and the hall's walls cover the hall's whole length exactly once.
        std::vector<std::pair<std::int64_t, std::int64_t>> northWalls;
        std::vector<std::pair<std::int64_t, std::int64_t>> southWalls;
        for (const rv::world::HallWall& wall : hall.walls) {
            if (wall.id.starts_with("north-")) {
                CHECK(wall.footprint.maxZMm == -half);
                CHECK(wall.footprint.minZMm == -half - 150);
                northWalls.emplace_back(wall.footprint.minXMm, wall.footprint.maxXMm);
            } else if (wall.id.starts_with("south-")) {
                CHECK(wall.footprint.minZMm == half);
                southWalls.emplace_back(wall.footprint.minXMm, wall.footprint.maxXMm);
            }
        }
        for (auto [rooms, walls] : {std::pair{north, northWalls}, std::pair{south, southWalls}}) {
            std::vector<std::pair<std::int64_t, std::int64_t>> both = rooms;
            both.insert(both.end(), walls.begin(), walls.end());
            CHECK(covered(0, east, both) == east);
            CHECK(covered(0, east, rooms) + covered(0, east, walls) == east);
        }
        // Lights every 6 m from 3 m, below the east end; the elevator in the west wall's middle.
        REQUIRE_FALSE(hall.lights.empty());
        for (std::size_t i = 0; i < hall.lights.size(); ++i) {
            CHECK(hall.lights[i].xMm == 3000 + 6000 * static_cast<std::int64_t>(i));
            CHECK(hall.lights[i].zMm == 0);
        }
        CHECK(hall.lights.back().xMm < east);
        CHECK(hall.lights.back().xMm + 6000 >= east);
        CHECK(hall.elevator == Footprint{0, -700, 50, 700});
        CHECK(hall.arrival == rv::world::FloorPoint{1200, 0});
        CHECK((hall.entrance.maxXMm > 0) == hall.lobby);
        // Every doorway opens onto the hall, in its room's doorway wall, its clear area inside the room.
        CHECK(hall.doorways.size() == rv::world::roomsOnFloor(plan, floor).size());
        for (const rv::world::FloorDoorway& doorway : hall.doorways) {
            INFO(doorway.room.toString());
            CHECK(doorway.opensOntoHall());
            CHECK(doorway.maxMm - doorway.minMm == 1600);
            CHECK(doorway.heightMm == 2300);
            CHECK(std::abs(doorway.clearCentre.zMm) > half + 150);
        }
    }
}

TEST_CASE("world/floor: a longer hall appends lights and moves none", "[world][floor]") {
    const rv::world::BuildingLayout layout = rv::test::world::snesData().building.layout;
    const rv::world::RoomTemplate& roomTemplate = rv::test::world::snesData().roomTemplate;
    std::vector<rv::world::Room> rooms;
    std::vector<rv::world::FloorRoom> floorRooms;
    rooms.reserve(6);
    std::vector<std::vector<rv::world::FloorPoint>> lightsBySize;
    for (std::string_view system : {"snes", "megadrive", "pcengine", "neogeo", "supergrafx", "tg16"}) {
        rv::world::Room room;
        room.id = *RoomId::forSystem(name(system));
        room.floor = "gen4";
        room.minXMm = -4500;
        room.maxXMm = 4500;
        room.minZMm = -5000;
        room.maxZMm = 5000;
        room.heightMm = 3200;
        room.placement = *rv::world::placeEnteringRoom(room, rv::world::floorRows(rooms, "gen4", layout), layout);
        rooms.push_back(room);
        floorRooms.clear();
        for (const rv::world::Room& entered : rooms) {
            floorRooms.push_back({&entered, &roomTemplate});
        }
        const Hall hall = rv::world::deriveHall("gen4", floorRooms, layout);
        lightsBySize.push_back(hall.lights);
        // The areas: each room's interior and passage, then the hall.
        const std::vector<rv::world::FloorArea> areas = rv::world::floorAreas(hall, floorRooms);
        REQUIRE(areas.size() == rooms.size() + 1);
        CHECK(areas.back().id == "hall:gen4");
        CHECK(areas.back().footprints == std::vector<Footprint>{hall.interior});
        for (std::size_t i = 0; i + 1 < areas.size(); ++i) {
            CHECK(areas[i].footprints.size() == 2);
            CHECK(areas[i].heightMm == 3200);
        }
    }
    for (std::size_t i = 1; i < lightsBySize.size(); ++i) {
        REQUIRE(lightsBySize[i].size() >= lightsBySize[i - 1].size());
        CHECK(std::equal(lightsBySize[i - 1].begin(), lightsBySize[i - 1].end(), lightsBySize[i].begin()));
    }
    CHECK(lightsBySize.back().size() > lightsBySize.front().size());
}

TEST_CASE("world/floor: each room's scene moved onto the floor, its doorway open, its lights in its area",
          "[world][floor]") {
    const GoldenBuilding& building = goldenBuilding();
    const WorldPlan& plan = building.plan;
    // The handheld floor: gb in the north row, then the shared room of pocketlab and pokemini in the south row.
    const rv::world::FloorSceneData data = floorData(plan, "handheld", building.tags);
    const rv::world::FloorSceneBuild build = floorScene(plan, "handheld", data);
    const nlohmann::json& scene = build.sceneFile;
    CHECK(scene["name"] == "floor:handheld");
    std::set<std::string> rows;
    for (const RoomId& id : build.rooms) {
        const rv::world::Room& room = roomOf(plan, id.toString());
        INFO(id.toString());
        rows.insert(room.placement.yawHalfTurns == 0 ? "north" : "south");
        auto roomScene = rv::world::toSceneFile(plan, id, data.rooms.at(id));
        REQUIRE(roomScene.has_value());
        const double sign = room.placement.yawHalfTurns == 0 ? 1.0 : -1.0;
        const double x0 = static_cast<double>(room.placement.xMm) / 1000.0;
        const double z0 = static_cast<double>(room.placement.zMm) / 1000.0;
        for (const nlohmann::json& object : roomScene->sceneFile["objects"]) {
            const std::string local = object["id"].get<std::string>();
            const nlohmann::json* moved = findObject(scene, rv::world::floorObjectId(id, local));
            INFO(local);
            REQUIRE(moved != nullptr);
            CHECK((*moved)["position"][0].get<double>() ==
                  Approx(x0 + sign * object["position"][0].get<double>()).margin(1e-6));
            CHECK((*moved)["position"][1] == object["position"][1]);
            CHECK((*moved)["position"][2].get<double>() ==
                  Approx(z0 + sign * object["position"][2].get<double>()).margin(1e-6));
            const double yaw = object.contains("rotationDeg") ? object["rotationDeg"][1].get<double>() : 0.0;
            const double movedYaw = moved->contains("rotationDeg") ? (*moved)["rotationDeg"][1].get<double>() : 0.0;
            const double turned = std::remainder(movedYaw - yaw - (sign > 0.0 ? 0.0 : 180.0), 360.0);
            CHECK(turned == Approx(0.0).margin(1e-3));
            const bool sameParams = moved->value("params", nlohmann::json()) == object.value("params", nlohmann::json());
            const bool openedShell =
                object.value("prefab", "") == "room_shell" && (*moved)["params"]["doorway"]["stop"] == false;
            CHECK((sameParams || openedShell));
        }
        // The shell's doorway opens onto the hall: its stop is left out, nothing else changes.
        const nlohmann::json* shell = findObject(scene, rv::world::floorObjectId(id, "room"));
        REQUIRE(shell != nullptr);
        CHECK((*shell)["params"]["doorway"]["stop"] == false);
        // The room's lights: its own, named under it, in its area, at most 16.
        std::size_t lights = 0;
        for (const nlohmann::json& light : scene["lights"]) {
            if (light["area"] == id.toString()) {
                ++lights;
                CHECK(light["name"].get<std::string>().starts_with(id.toString() + "/"));
            }
        }
        CHECK(lights == roomScene->sceneFile["lights"].size());
        CHECK(lights <= rv::world::kMaxSceneLights);
    }
    CHECK(rows == std::set<std::string>{"north", "south"});
    // The hall's lights in the hall's area, as many as its length takes; one key light per room.
    std::size_t hallLights = 0;
    std::map<std::string, std::size_t> keys;
    for (const nlohmann::json& light : scene["lights"]) {
        hallLights += light["area"] == "hall:handheld" ? 1 : 0;
        if (light.value("castsShadows", false)) {
            ++keys[light["area"].get<std::string>()];
        }
    }
    CHECK(hallLights == build.hall.lights.size());
    CHECK(keys.size() == build.rooms.size());
    for (const auto& [area, count] : keys) {
        CHECK(count == 1);
    }
    // The camera stands in front of the elevator facing east, its far plane beyond the floor.
    CHECK(scene["camera"]["position"] == nlohmann::json::array({1.2, 1.65, 0.0}));
    CHECK(scene["camera"]["yawDeg"] == 90.0);
    CHECK(scene["camera"]["far"].get<double>() >= 100.0);
    // The sign by the elevator reads the floor's text.
    const nlohmann::json* sign = findObject(scene, "hall:handheld/sign");
    REQUIRE(sign != nullptr);
    CHECK((*sign)["textures"]["picture"]["title"] == "HANDHELDS");
    CHECK(findObject(scene, "hall:handheld/elevator/door-north") != nullptr);
    CHECK(findObject(scene, "hall:handheld/entrance/door-north") == nullptr);
    // A scene description of it: the areas hold the lights' areas; the eye in a room's doorway is in that room.
    auto description = rv::world::toFloorSceneDescription(plan, "handheld", data);
    REQUIRE(description.has_value());
    for (const rv::world::FloorDoorway& doorway : build.hall.doorways) {
        const glm::vec3 eye(static_cast<float>(doorway.minMm + doorway.maxMm) / 2000.0f, 1.65f,
                            static_cast<float>(doorway.passage.minZMm + doorway.passage.maxZMm) / 2000.0f);
        const auto area = rv::scene::areaAt(description->areas, eye);
        REQUIRE(area.has_value());
        CHECK(description->areas[*area].id == doorway.room.toString());
    }
    CHECK(description->areas[*rv::scene::areaAt(description->areas, {1.2f, 1.65f, 0.0f})].id == "hall:handheld");

    // The ground floor's lobby has its entrance doors at its east end.
    const rv::world::FloorSceneData ground = floorData(plan, "ground", building.tags);
    const nlohmann::json groundScene = floorScene(plan, "ground", ground).sceneFile;
    CHECK(findObject(groundScene, "hall:ground/entrance/door-north") != nullptr);
    CHECK((*findObject(groundScene, "hall:ground/sign"))["textures"]["picture"]["title"] == "LOBBY AND ARCADE");

    // Refused: a floor the plan does not list.
    CHECK(rv::world::toFloorSceneFile(plan, "gen7", data).error().code == rv::ErrorCode::NotFound);
    CHECK(rv::world::loadFloorSceneData(rv::test::world::assetRoot(), plan, "gen7").error().code ==
          rv::ErrorCode::NotFound);
}

namespace {

/// Where a walk on a floor ended: the feet, and the areas that held the eye in turn.
struct FloorWalk {
    glm::vec3 feet{0.0f};
    float capsuleRadius = 0.0f;
    std::vector<std::string> areas;
};

/// `scriptName` (tests/input_scripts) played for 255 frames on `floor`'s scene through its collision world, from the
/// scene's camera, as retroverse.exe plays it; the walker never overlaps a collider, and is in the hall in front of the
/// first room's doorway (x 7.659) at frame 154.
FloorWalk walkFloor(const GoldenBuilding& building, std::string_view floor, std::string_view scriptName) {
    const rv::world::FloorSceneData data = floorData(building.plan, floor, building.tags);
    auto scene = rv::world::toFloorSceneDescription(building.plan, floor, data);
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
                                                       "input_scripts" / rv::core::pathFromUtf8(scriptName));
    REQUIRE(script.has_value());
    rv::input::InputSystem system;
    rv::input::InputScriptPlayer player(*script);
    rv::scene::AreaLighting lighting(scene->areas, scene->lights, scene->camera.position, 16);
    FloorWalk walk;
    walk.areas.push_back(scene->areas[*lighting.area()].id);
    for (std::uint64_t frame = 0; frame < 255; ++frame) {
        rv::input::ActionState actions = system.resolve(rv::input::RawInputFrame{});
        player.apply(frame, actions, system.settings().pressThreshold);
        controller.update(actions, kFixedDt, camera, &world);
        REQUIRE_FALSE(world.overlaps(controller.feetPosition(), controller.settings().capsule));
        lighting.update(scene->areas, scene->lights, camera.position());
        if (scene->areas[*lighting.area()].id != walk.areas.back()) {
            walk.areas.push_back(scene->areas[*lighting.area()].id);
        }
        if (frame == 154) {
            // At the doorway, in the hall.
            CHECK(controller.feetPosition().x == Approx(1.2f + 155.0f * 2.5f * kFixedDt).margin(1e-3));
            CHECK(controller.feetPosition().z == Approx(0.0f).margin(1e-3));
        }
    }
    walk.feet = controller.feetPosition();
    walk.capsuleRadius = controller.settings().capsule.radius;
    return walk;
}

} // namespace

TEST_CASE("world/floor: the walker walks from the elevator along the hall into a room", "[world][floor]") {
    const GoldenBuilding& building = goldenBuilding();
    SECTION("a north-row room: gen4's room:snes") {
        const FloorWalk walk = walkFloor(building, "gen4", "floor_walk_into_room.json");
        // Through the doorway: 3.75 m north of the hall's centre line, 2.1 m into the room past its south wall.
        CHECK(walk.feet.x == Approx(1.2f + 155.0f * 2.5f * kFixedDt).margin(1e-3));
        CHECK(walk.feet.z == Approx(-90.0f * 2.5f * kFixedDt).margin(1e-3));
        CHECK(walk.feet.z < -1.65f - walk.capsuleRadius);
        CHECK(walk.areas == std::vector<std::string>{"hall:gen4", "room:snes"});
    }
    SECTION("a south-row room, turned a half turn: handheld's shared room") {
        // Across the hall from room:gb, through the turned shell's doorway (its stop left out as well).
        REQUIRE(roomOf(building.plan, "area:shared:handheld").placement.yawHalfTurns == 1);
        const FloorWalk walk = walkFloor(building, "handheld", "floor_walk_into_south_room.json");
        // 3.75 m south of the hall's centre line, 2.1 m into the room past its north wall (its doorway wall).
        CHECK(walk.feet.x == Approx(1.2f + 155.0f * 2.5f * kFixedDt).margin(1e-3));
        CHECK(walk.feet.z == Approx(90.0f * 2.5f * kFixedDt).margin(1e-3));
        CHECK(walk.feet.z > 1.65f + walk.capsuleRadius);
        CHECK(walk.areas == std::vector<std::string>{"hall:handheld", "area:shared:handheld"});
    }
}

TEST_CASE("world/floor: a room's collection in its floor's frame", "[world][floor]") {
    const GoldenBuilding& building = goldenBuilding();
    const WorldPlan& plan = building.plan;
    const rv::world::FloorSceneData data = floorData(plan, "handheld", building.tags);
    for (const rv::world::Room* room : rv::world::roomsOnFloor(plan, "handheld")) {
        INFO(room->id.toString());
        const std::vector<rv::world::CollectionTitle> titles = rv::test::world::inventedTitles(plan);
        auto layout = rv::world::collectionLayout(plan, room->id, data.rooms.at(room->id), titles);
        REQUIRE(layout.has_value());
        const rv::world::CollectionLayout moved = rv::world::floorCollectionLayout(*layout, room->placement);
        const glm::mat4 toFloor = rv::world::roomToFloorMatrix(room->placement);
        REQUIRE(moved.items.size() == layout->items.size());
        REQUIRE_FALSE(moved.items.empty());
        for (std::size_t i = 0; i < moved.items.size(); ++i) {
            CHECK(moved.items[i].pickId == layout->items[i].pickId);
            CHECK(moved.items[i].frontLayerKey == layout->items[i].frontLayerKey);
            CHECK(rv::core::nearlyEqual(glm::vec3(moved.items[i].transform[3]),
                                        glm::vec3(toFloor * layout->items[i].transform[3])));
        }
        // The items stand inside the room's footprint on the floor, the occluders too.
        const Footprint footprint = rv::world::floorFootprint(*room, data.building.layout);
        for (const rv::world::CollectionItem& item : moved.items) {
            const glm::vec3 at = item.transform[3];
            CHECK(mm(at.x) > footprint.minXMm);
            CHECK(mm(at.x) < footprint.maxXMm);
            CHECK(mm(at.z) > footprint.minZMm);
            CHECK(mm(at.z) < footprint.maxZMm);
        }
        REQUIRE(moved.occluders.size() == layout->occluders.size());
        for (std::size_t i = 0; i < moved.occluders.size(); ++i) {
            CHECK(moved.occluders[i].size().x == Approx(layout->occluders[i].size().x).margin(1e-4));
            CHECK(moved.occluders[i].size().z == Approx(layout->occluders[i].size().z).margin(1e-4));
        }
        CHECK(moved.navigation.size() == layout->navigation.size());
    }
}
