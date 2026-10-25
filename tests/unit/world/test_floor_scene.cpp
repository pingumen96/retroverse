// The floors of the building (rv/world/floor_layout.hpp, floor_scene.hpp; I4 step 3, ADR-0013 §6, §7, §9): the golden
// building of the building fixture's systems (tests/fixtures/esde/building) compared byte for byte in Debug and
// Release, its floors' scenes as golden digests (rewritten only with RV_UPDATE_GOLDEN=1), the halls derived from the
// rooms (walls where no room stands, lights at a fixed pitch, doorways onto the hall), each room's scene moved onto the
// floor, the areas, walks from the elevator into a north-row and a south-row (half-turned) room through the floor's
// collision, the light a view packs on a full floor and in the I6 step 1 review's cases (the doorway's threshold, a
// slight turn), a room's collection in the floor's frame, and the halls' dressing (I6 step 2: portals with their flat
// and blade signs, beams on a grid and over the doorways, sconces with their wash and prints in turn, the prints'
// titles and their small pool of pictures, benches with a plant beside them off the passages and the doorways'
// approaches, obstacles to the walker, none in a hall too narrow, appended and never moved as the hall grows; the
// pack's hall kind, a role it lacks the built-in pack's).

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
#include <rv/world/nav_grid.hpp>
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
                                           {"hallDressing", build.hall.dressing.size()},
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
    // The hall's lights in the hall's area, as many as its length takes, in the profile's hall role (I6 step 1); a
    // low fill spot before each doorway aimed into its room; one key light per room, none in the hall.
    const rv::world::LightingProfile& profile = data.lighting;
    std::size_t hallLights = 0;
    std::map<std::string, const nlohmann::json*> fills;
    std::map<std::string, std::size_t> keys;
    for (const nlohmann::json& light : scene["lights"]) {
        const std::string name = light["name"].get<std::string>();
        if (name.rfind("hall:handheld/light-", 0) == 0) {
            ++hallLights;
            CHECK(light["area"] == "hall:handheld");
            CHECK(light["type"] == "point");
            CHECK(light["color"] == profile.hall.light.color);
            CHECK(light["intensity"] == profile.hall.light.intensity);
            CHECK(light["range"] == profile.hall.light.range);
        } else if (name.rfind("hall:handheld/doorway-fill:", 0) == 0) {
            fills[name.substr(std::string("hall:handheld/doorway-fill:").size())] = &light;
        }
        if (light.value("castsShadows", false)) {
            ++keys[light["area"].get<std::string>()];
        }
    }
    CHECK(hallLights == build.hall.lights.size());
    CHECK(keys.size() == build.rooms.size());
    CHECK_FALSE(keys.contains("hall:handheld"));
    for (const auto& [area, count] : keys) {
        CHECK(count == 1);
    }
    REQUIRE(fills.size() == build.hall.doorways.size());
    for (const rv::world::FloorDoorway& doorway : build.hall.doorways) {
        INFO(doorway.room.toString());
        REQUIRE(doorway.opensOntoHall());
        REQUIRE(fills.contains(doorway.room.toString()));
        const nlohmann::json& fill = *fills.at(doorway.room.toString());
        CHECK(fill["area"] == "hall:handheld");
        CHECK(fill["type"] == "spot");
        CHECK(fill["color"] == profile.hall.doorwayFill.color);
        CHECK(fill["intensity"] == profile.hall.doorwayFill.intensity);
        CHECK_FALSE(fill.value("castsShadows", false));
        // In the hall before the doorway's middle, below its head, aimed through it into the room.
        const double x = fill["position"][0].get<double>();
        const double y = fill["position"][1].get<double>();
        const double z = fill["position"][2].get<double>();
        const double edge = static_cast<double>(doorway.hallEdgeZMm) / 1000.0;
        CHECK(x * 1000.0 == Catch::Approx(static_cast<double>(doorway.minMm + doorway.maxMm) / 2.0));
        CHECK(y < static_cast<double>(doorway.heightMm) / 1000.0);
        CHECK(std::abs(z) < std::abs(edge));
        const double targetZ = fill["target"][2].get<double>();
        CHECK((edge < 0.0 ? targetZ < edge : targetZ > edge));
        CHECK(fill["outerConeDeg"].get<double>() >= profile.hall.doorwayFill.minOuterConeDeg);
        CHECK(fill["outerConeDeg"].get<double>() <= rv::world::kMaxOuterConeDeg);
    }
    // The hall's area has the hall role's environment; the rooms' are lit as the scene.
    for (const nlohmann::json& area : scene["areas"]) {
        if (area["id"] == "hall:handheld") {
            CHECK(area["environment"]["exposure"] == profile.hall.environment.exposure);
            CHECK(area["environment"]["ambientGround"] == profile.hall.environment.ambientGround);
            CHECK(area["environment"]["fillIntensity"] == profile.hall.environment.fillIntensity);
        } else {
            CHECK_FALSE(area.contains("environment"));
        }
    }
    CHECK(scene["environment"]["fillIntensity"] == profile.fillIntensity);
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
    // Each room the hall's neighbour through its doorway alone, the rooms never each other's (ADR-0014 §4).
    const auto neighbours = rv::scene::areaNeighbours(description->areas);
    const std::size_t hallIndex = *description->findArea("hall:handheld");
    CHECK(neighbours[hallIndex].size() == build.rooms.size());
    for (std::size_t i = 0; i < description->areas.size(); ++i) {
        if (i == hallIndex) {
            continue;
        }
        INFO(description->areas[i].id);
        REQUIRE(neighbours[i].size() == 1);
        CHECK(neighbours[i][0].area == hallIndex);
        REQUIRE(neighbours[i][0].portals.size() == 1);
        CHECK(neighbours[i][0].portals[0].size().x == Catch::Approx(1.6f));
    }

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

namespace {

/// The unit view direction of a yaw and pitch in degrees (rv::scene::Camera's convention: yaw 0 looks along -z).
glm::vec3 viewOf(float yawDeg, float pitchDeg) {
    const float yaw = rv::core::degreesToRadians(yawDeg);
    const float pitch = rv::core::degreesToRadians(pitchDeg);
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

/// How many lights of `lighting`'s selection are named under `prefix`.
std::size_t packedUnder(const rv::scene::AreaLighting& lighting, const rv::scene::SceneDescription& scene,
                        std::string_view prefix) {
    return static_cast<std::size_t>(
        std::count_if(lighting.selection().begin(), lighting.selection().end(),
                      [&](std::uint32_t index) { return scene.lights[index].name.starts_with(prefix); }));
}

/// The golden building's gen4 floor filled up (I6 step 1's review): room:snes cloned into every place its rows hold,
/// each clone with its own id and place, the lights and fixtures of snes's (no items); a hall of as many lights and
/// doorway fills as its length takes.
struct FullFloor {
    WorldPlan plan;
    rv::scene::SceneDescription scene;
    Hall hall;
};

/// The golden building's gen4 floor with room:snes cloned `clones` times (fewer when its rows are full) into the next
/// places its rows give, each clone with its own id and place, the lights and fixtures of snes's (no items).
struct GrownFloor {
    WorldPlan plan;
    rv::world::FloorSceneData data;
};

GrownFloor grownGen4Floor(const GoldenBuilding& building, int clones) {
    GrownFloor grown;
    grown.plan = building.plan;
    grown.data = floorData(grown.plan, "gen4", building.tags);
    const rv::world::Room snes = roomOf(grown.plan, "room:snes");
    for (int i = 0; i < clones; ++i) {
        rv::world::Room clone = snes;
        clone.id = *RoomId::forSystem(name("clone" + std::to_string(i)));
        const auto placement = rv::world::placeEnteringRoom(
            clone, rv::world::floorRows(grown.plan.rooms, "gen4", grown.data.building.layout),
            grown.data.building.layout);
        if (!placement) {
            break;
        }
        clone.placement = *placement;
        grown.plan.rooms.push_back(clone);
        grown.data.rooms.emplace(clone.id, grown.data.rooms.at(snes.id));
        // Its fixtures and lights, snes's under its id (the plan's lists are the whole building's).
        const auto moved = [&](const rv::world::FixtureId& id) {
            return *rv::world::FixtureId::make(clone.id, id.socket(), id.part());
        };
        for (std::size_t f = 0, count = grown.plan.fixtures.size(); f < count; ++f) {
            if (grown.plan.fixtures[f].id.room() == snes.id) {
                rv::world::Fixture fixture = grown.plan.fixtures[f];
                fixture.id = moved(fixture.id);
                grown.plan.fixtures.push_back(std::move(fixture));
            }
        }
        for (std::size_t l = 0, count = grown.plan.lights.size(); l < count; ++l) {
            if (grown.plan.lights[l].id.room() == snes.id) {
                rv::world::PlanLight light = grown.plan.lights[l];
                light.id = moved(light.id);
                grown.plan.lights.push_back(std::move(light));
            }
        }
    }
    return grown;
}

FullFloor fullGen4Floor(const GoldenBuilding& building) {
    GrownFloor grown = grownGen4Floor(building, 40);
    FullFloor full;
    full.plan = std::move(grown.plan);
    auto scene = rv::world::toFloorSceneDescription(full.plan, "gen4", grown.data);
    INFO((scene ? std::string("built") : scene.error().toString()));
    REQUIRE(scene.has_value());
    full.scene = std::move(*scene);
    full.hall = floorScene(full.plan, "gen4", grown.data).hall;
    return full;
}

} // namespace

TEST_CASE("world/floor: a full floor: the hall facing a doorway packs that room's lights, the room the hall's",
          "[world][floor][areas]") {
    const FullFloor full = fullGen4Floor(goldenBuilding());
    const rv::scene::SceneDescription& scene = full.scene;
    // Twelve rooms a row (the building's rows hold no more), 15 lights each.
    REQUIRE(full.hall.doorways.size() >= 20);
    const float hallHalf = static_cast<float>(full.hall.widthMm) / 2000.0f;
    std::size_t checked = 0;
    for (const rv::world::FloorDoorway& doorway : full.hall.doorways) {
        const std::string room = doorway.room.toString();
        INFO(room);
        const bool north = doorway.hallEdgeZMm < 0;
        const float x = static_cast<float>(doorway.minMm + doorway.maxMm) / 2000.0f;
        // From the hall's far side, facing the doorway, as world_floor_light's view.
        const glm::vec3 hallEye(x, 1.65f, north ? hallHalf - 0.4f : -hallHalf + 0.4f);
        rv::scene::AreaLighting lighting(scene.areas, scene.lights, hallEye, 16);
        (void)lighting.update(scene.areas, scene.lights, hallEye, viewOf(north ? 0.0f : 180.0f, -4.0f));
        REQUIRE(scene.areas[*lighting.area()].id == "hall:gen4");
        CHECK(lighting.selection().size() <= 16);
        CHECK(lighting.ownCount() == 16 - rv::scene::kNeighbourLightReserve);
        // Most of the neighbours' share is the faced room's, its key light (unshadowed) and its doorway's fill among
        // the lights packed.
        CHECK(packedUnder(lighting, scene, room + "/") >= 4);
        CHECK(packedUnder(lighting, scene, room + "/fx:hero.key-light") == 1);
        CHECK(packedUnder(lighting, scene, "hall:gen4/doorway-fill:" + room) == 1);
        CHECK_FALSE(lighting.keyLight().has_value());
        // From 4.5 m inside facing out: the room's own (its key light), then the hall's near the doorway.
        const float inside = static_cast<float>(north ? doorway.passage.minZMm : doorway.passage.maxZMm) / 1000.0f;
        const glm::vec3 roomEye(x, 1.65f, inside + (north ? -4.5f : 4.5f));
        (void)lighting.update(scene.areas, scene.lights, roomEye, viewOf(north ? 180.0f : 0.0f, -4.0f));
        REQUIRE(scene.areas[*lighting.area()].id == room);
        CHECK(lighting.selection().size() <= 16);
        CHECK(lighting.selection().size() - lighting.ownCount() >= 3);
        CHECK(packedUnder(lighting, scene, "hall:gen4/doorway-fill:" + room) == 1);
        REQUIRE(lighting.keyLight().has_value());
        CHECK(scene.lights[*lighting.keyLight()].name == room + "/fx:hero.key-light");
        ++checked;
    }
    CHECK(checked == full.hall.doorways.size());
}

TEST_CASE("world/floor: the review's cases: no light comes or goes at the doorway's threshold or at a slight turn",
          "[world][floor][areas]") {
    const GoldenBuilding& building = goldenBuilding();
    const rv::world::FloorSceneData data = floorData(building.plan, "gen4", building.tags);
    auto description = rv::world::toFloorSceneDescription(building.plan, "gen4", data);
    REQUIRE(description.has_value());
    const rv::scene::SceneDescription& scene = *description;
    constexpr float kEye = 1.65f;
    // Through the snes room's doorway (feet z -1.45 in the hall, -1.55 in the passage) at 2.5 m/s: the room's lights
    // fade in, the hall's out, no weight moves by more than a fade's step in a frame.
    rv::scene::AreaLighting walk(scene.areas, scene.lights, {7.65f, kEye, 1.0f}, 16);
    std::map<std::uint32_t, float> before;
    for (std::size_t i = 0; i < walk.selection().size(); ++i) {
        before[walk.selection()[i]] = walk.weights()[i];
    }
    for (float z = 1.0f; z > -6.0f; z -= 2.5f * kFixedDt) {
        (void)walk.update(scene.areas, scene.lights, {7.65f, kEye, z}, viewOf(0.0f, -4.0f), kFixedDt);
        std::map<std::uint32_t, float> now;
        for (std::size_t i = 0; i < walk.selection().size(); ++i) {
            now[walk.selection()[i]] = walk.weights()[i];
        }
        for (std::uint32_t index = 0; index < scene.lights.size(); ++index) {
            const float was = before.contains(index) ? before.at(index) : 0.0f;
            const float is = now.contains(index) ? now.at(index) : 0.0f;
            INFO(scene.lights[index].name << " at z " << z);
            CHECK(std::abs(is - was) <= 1.5f * kFixedDt / rv::scene::kLightFadeSeconds + 1e-4f);
        }
        before = std::move(now);
    }
    CHECK(scene.areas[*walk.area()].id == "room:snes");

    // Inside the room by the hero corner (feet 8.2718, -8.5292), pitch -20: turning between 195.5 and 195.9 degrees
    // and strafing 2 cm, frame after frame, changes nothing.
    const glm::vec3 eye(8.2718f, kEye, -8.5292f);
    rv::scene::AreaLighting turning(scene.areas, scene.lights, eye, 16);
    (void)turning.update(scene.areas, scene.lights, eye, viewOf(195.5f, -20.0f), kFixedDt, true);
    const std::vector<std::uint32_t> first = turning.chosen();
    const std::vector<std::uint32_t> selection(turning.selection().begin(), turning.selection().end());
    for (int frame = 0; frame < 240; ++frame) {
        const float yaw = frame % 2 == 0 ? 195.9f : 195.5f;
        const glm::vec3 strafe = frame % 4 < 2 ? glm::vec3(0.0f) : glm::vec3(0.02f, 0.0f, 0.0f);
        CHECK_FALSE(turning.update(scene.areas, scene.lights, eye + strafe, viewOf(yaw, -20.0f), kFixedDt));
    }
    CHECK(turning.chosen() == first);
    CHECK(std::vector<std::uint32_t>(turning.selection().begin(), turning.selection().end()) == selection);
    // The room and the hall beyond its doorway both packed.
    CHECK(packedUnder(turning, scene, "room:snes/") > 0);
    CHECK(packedUnder(turning, scene, "hall:gen4/") > 0);
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

namespace {

/// The hall's dressing objects that must never move: every object of the hall but its floor, ceiling, walls, the
/// walls' trim and the east end wall's print (they follow the walls, which a longer hall moves or splits).
bool isFixedDressing(std::string_view id, std::string_view hallPrefix) {
    if (!id.starts_with(hallPrefix)) {
        return false;
    }
    const std::string_view local = id.substr(hallPrefix.size());
    return !(local == "floor" || local == "ceiling" || local.starts_with("wall-") || local.starts_with("trim/") ||
             local.starts_with("east/"));
}

/// Checks a hall's dressing against its doorways, the elevator and the passage (see floor_layout.hpp): every piece in
/// the hall, the floor's pieces off the elevator's zone and the doorways' approaches, at least kHallMinClearMm clear
/// across the hall wherever they stand, and the navigation grid at the main passage's width (1.4 m) reaching the front
/// of every doorway from the elevator.
void checkDressingClear(const Hall& hall) {
    const std::int64_t half = hall.widthMm / 2;
    std::vector<Footprint> obstacles{hall.elevator};
    for (const rv::world::HallPiece& piece : hall.dressing) {
        INFO(piece.id);
        CHECK(piece.footprint.minXMm >= hall.interior.minXMm);
        CHECK(piece.footprint.maxXMm <= hall.interior.maxXMm);
        CHECK(piece.footprint.minZMm >= -half);
        CHECK(piece.footprint.maxZMm <= half);
        if (!piece.solid()) {
            continue;
        }
        obstacles.push_back(piece.footprint);
        CHECK_FALSE(piece.footprint.overlaps(hall.elevatorZone));
        CHECK(piece.footprint.minXMm >= rv::world::kHallFloorFirstMm);
        for (const rv::world::FloorDoorway& doorway : hall.doorways) {
            INFO(doorway.room.toString());
            CHECK((piece.footprint.maxXMm <= doorway.minMm - rv::world::kHallDoorApproachMm ||
                   piece.footprint.minXMm >= doorway.maxMm + rv::world::kHallDoorApproachMm));
        }
    }
    // The clear width across the hall at every 10 cm along it.
    for (std::int64_t x = 50; x < hall.interior.maxXMm; x += 100) {
        std::int64_t north = -half;
        std::int64_t south = half;
        for (const rv::world::HallPiece& piece : hall.dressing) {
            if (piece.solid() && piece.footprint.minXMm <= x && piece.footprint.maxXMm >= x) {
                if (piece.north) {
                    north = std::max(north, piece.footprint.maxZMm);
                } else {
                    south = std::min(south, piece.footprint.minZMm);
                }
            }
        }
        INFO("x " << x);
        CHECK(south - north >= rv::world::kHallMinClearMm);
    }
    // A body 1.4 m wide walks from the elevator to the front of every doorway.
    const rv::world::NavGrid grid(hall.interior, obstacles, rv::world::kHallMinClearMm / 2);
    const auto start = grid.cellAt(hall.arrival.xMm, hall.arrival.zMm);
    REQUIRE(start.has_value());
    REQUIRE(grid.isFree(*start));
    const rv::world::NavReach reach(grid, *start);
    for (const rv::world::FloorDoorway& doorway : hall.doorways) {
        INFO(doorway.room.toString());
        CHECK(reach.reachesNear((doorway.minMm + doorway.maxMm) / 2, 0, rv::world::kNavCellMm));
    }
}

} // namespace

TEST_CASE("world/floor: the hall's dressing: portals at the doorways, sconces and prints in turn, the passage clear",
          "[world][floor][dressing]") {
    const GoldenBuilding& building = goldenBuilding();
    const WorldPlan& plan = building.plan;
    auto materials = rv::assets::MaterialLibrary::loadFromFile(rv::test::world::assetRoot() / "materials" / "base" /
                                                               "materials.json");
    REQUIRE(materials.has_value());
    std::size_t benches = 0;
    std::size_t plants = 0;
    for (const rv::world::Floor& floor : plan.floors) {
        INFO(floor.id);
        const rv::world::FloorSceneData data = floorData(plan, floor.id, building.tags);
        const rv::world::FloorSceneBuild build = floorScene(plan, floor.id, data);
        const Hall& hall = build.hall;
        const nlohmann::json& scene = build.sceneFile;
        const std::string prefix = "hall:" + floor.id + "/";
        checkDressingClear(hall);
        std::set<std::int64_t> beams;
        for (const nlohmann::json& object : scene["objects"]) {
            const std::string id = object["id"].get<std::string>();
            if (id.starts_with(prefix + "beam-")) {
                beams.insert(std::stoll(id.substr(prefix.size() + 5)));
            }
        }
        // A portal at every doorway: its casing, lintel and the room's sign above, a beam over it.
        for (const rv::world::FloorDoorway& doorway : hall.doorways) {
            INFO(doorway.room.toString());
            REQUIRE(doorway.opensOntoHall());
            const std::string portal = prefix + "portal:" + doorway.room.toString();
            for (const std::string part : {"/casing-west", "/casing-east", "/casing-head", "/lintel"}) {
                CHECK(findObject(scene, portal + part) != nullptr);
            }
            const nlohmann::json* sign = findObject(scene, portal + "/sign");
            REQUIRE(sign != nullptr);
            CHECK((*sign)["textures"]["picture"]["pattern"] == "sign");
            CHECK((*sign)["textures"]["picture"]["title"] == data.rooms.at(doorway.room).platform.signText);
            CHECK((*sign)["materials"]["picture"] == "MAT_Sign_Tungsten");
            // Above the lintel, below the beam, facing into the hall.
            const double signY = (*sign)["position"][1].get<double>();
            CHECK(signY - 0.17 > static_cast<double>(doorway.heightMm) / 1000.0 + 0.15);
            CHECK(signY + 0.17 < static_cast<double>(hall.heightMm) / 1000.0 - 0.22);
            CHECK((*sign)["rotationDeg"][1].get<double>() == (doorway.hallEdgeZMm < 0 ? 0.0 : 180.0));
            // A beam over it: its own, or the grid's within 0.75 m.
            const std::int64_t centre = (doorway.minMm + doorway.maxMm) / 2;
            CHECK(std::any_of(beams.begin(), beams.end(), [&](std::int64_t x) { return std::abs(x - centre) <= 750; }));
            // Its two sconces beside the opening.
            CHECK(findObject(scene, portal + "/sconce-west/shade") != nullptr);
            CHECK(findObject(scene, portal + "/sconce-east/shade") != nullptr);
            // The blade sign: the room's lettering on both faces, read along the hall, standing out from the wall on
            // the elevator's side above the head and above the sconces, on its arm; nothing to walk into.
            const double edge = static_cast<double>(doorway.hallEdgeZMm) / 1000.0;
            for (const std::string side : {"/blade-east", "/blade-west"}) {
                INFO(side);
                const nlohmann::json* blade = findObject(scene, portal + side);
                REQUIRE(blade != nullptr);
                CHECK((*blade)["textures"]["picture"]["title"] == (*sign)["textures"]["picture"]["title"]);
                CHECK((*blade)["materials"]["picture"] == "MAT_Sign_Tungsten");
                CHECK(std::abs((*blade)["rotationDeg"][1].get<double>()) == 90.0);
                CHECK((*blade)["position"][0].get<double>() ==
                      Approx(static_cast<double>(centre - 1050) / 1000.0).margin(0.0011));
                CHECK(std::abs((*blade)["position"][2].get<double>() - edge) == Approx(0.46));
                CHECK((*blade)["position"][1].get<double>() - 0.12 >= 2.45);
                CHECK_FALSE(blade->value("collides", true));
            }
            const nlohmann::json* arm = findObject(scene, portal + "/blade-arm");
            REQUIRE(arm != nullptr);
            CHECK_FALSE(arm->value("collides", true));
        }
        // Beams every 3 m from the west end, while 1 m short of the east end.
        for (std::int64_t x = 3000; x + 1000 <= hall.interior.maxXMm; x += 3000) {
            CHECK(beams.contains(x));
        }
        // Sconces and prints in turn along each edge; the floor's pieces are objects that collide.
        std::map<std::string, rv::world::HallPieceKind> slots;
        // The prints along each edge by x (their titles), and the pictures they draw.
        std::map<bool, std::map<std::int64_t, std::string>> titles;
        std::set<std::string> pictures;
        const nlohmann::json* firstWash = nullptr;
        for (const rv::world::HallPiece& piece : hall.dressing) {
            INFO(piece.id);
            switch (piece.kind) {
            case rv::world::HallPieceKind::Sconce: {
                const nlohmann::json* shade = findObject(scene, prefix + piece.id + "/shade");
                REQUIRE(shade != nullptr);
                CHECK_FALSE(shade->value("collides", true));
                CHECK_FALSE(findObject(scene, prefix + piece.id + "/backplate")->value("collides", true));
                // Its wash on the wall: one quad and one material for every sconce, unlit, centred above the shade
                // on the plaster.
                const nlohmann::json* wash = findObject(scene, prefix + piece.id + "/wash");
                REQUIRE(wash != nullptr);
                CHECK((*wash)["primitive"] == "quad");
                CHECK((*wash)["material"] == "MAT_Wall_Wash");
                CHECK_FALSE(wash->value("collides", true));
                CHECK((*wash)["position"][1].get<double>() == Approx((*shade)["position"][1].get<double>() + 0.05));
                const double face = piece.north ? -static_cast<double>(hall.widthMm) / 2000.0
                                                : static_cast<double>(hall.widthMm) / 2000.0;
                CHECK(std::abs((*wash)["position"][2].get<double>() - face) == Approx(0.0065).margin(1e-6));
                CHECK((*wash)["rotationDeg"][1].get<double>() == (piece.north ? 0.0 : 180.0));
                if (firstWash == nullptr) {
                    firstWash = wash;
                }
                CHECK((*wash)["params"] == (*firstWash)["params"]);
                break;
            }
            case rv::world::HallPieceKind::Picture: {
                const nlohmann::json* print = findObject(scene, prefix + piece.id);
                REQUIRE(print != nullptr);
                const nlohmann::json& picture = (*print)["textures"]["picture"];
                CHECK(picture["pattern"] == "print");
                CHECK((*print)["materials"]["frame"] == "MAT_Wood_Walnut");
                // Portrait and landscape in turn along the edge.
                const double width = (*print)["params"]["width"].get<double>();
                const double height = (*print)["params"]["height"].get<double>();
                CHECK((width > height) == piece.landscape);
                CHECK(piece.landscape == ((piece.slot / 2) % 2 == 1));
                titles[piece.north][piece.xMm] = picture["title"].get<std::string>();
                pictures.insert(picture.dump() + (piece.landscape ? "/landscape" : "/portrait"));
                break;
            }
            case rv::world::HallPieceKind::Bench:
                // A bench with a plant beside it, the edges in turn every 6 m.
                CHECK(piece.slot % 8 == (piece.north ? 3 : 6));
                ++benches;
                for (const std::string part : {"/end-west", "/end-east", "/seat", "/cushion"}) {
                    const nlohmann::json* object = findObject(scene, prefix + piece.id + part);
                    REQUIRE(object != nullptr);
                    CHECK(object->value("collides", true));
                }
                break;
            case rv::world::HallPieceKind::Plant: {
                CHECK(piece.slot % 8 == (piece.north ? 2 : 7));
                ++plants;
                const nlohmann::json* object = findObject(scene, prefix + piece.id);
                REQUIRE(object != nullptr);
                CHECK(object->value("collides", true));
                // On a darker saucer a little wider than its pot.
                CHECK((*object)["params"]["saucerRadius"].get<double>() ==
                      Approx(1.15 * (*object)["params"]["potRadius"].get<double>()).margin(0.002));
                CHECK((*object)["materials"]["saucer"] == "MAT_Ceramic_Saucer");
                break;
            }
            }
            if (!piece.id.starts_with("portal:") && !piece.solid()) {
                slots[piece.id.substr(0, piece.id.find('/'))] = piece.kind;
            }
        }
        for (const auto& [slot, kind] : slots) {
            INFO(slot);
            const bool north = slot.starts_with("north-");
            const long index = std::stol(slot.substr(slot.find('-') + 1));
            CHECK((kind == rv::world::HallPieceKind::Sconce) == ((index % 2 == 0) == north));
        }
        // No title twice among five prints in a row along an edge; the prints share a pool of at most one picture a
        // title (16), the east end wall's apart.
        for (const auto& [north, byX] : titles) {
            std::vector<std::string> row;
            for (const auto& [x, title] : byX) {
                row.push_back(title);
            }
            for (std::size_t i = 0; i < row.size(); ++i) {
                for (std::size_t j = i + 1; j < std::min(row.size(), i + 5); ++j) {
                    INFO((north ? "north " : "south ") << i << " " << j);
                    CHECK(row[i] != row[j]);
                }
            }
        }
        CHECK(pictures.size() <= 16);
        // The east end wall's print in a hall, none in the lobby (its entrance stands there).
        CHECK((findObject(scene, prefix + "east/print") == nullptr) == hall.lobby);
        // The lamps: a drum over each light, the light at its foot, below the beams.
        for (std::size_t i = 0; i < hall.lights.size(); ++i) {
            const nlohmann::json* lamp = findObject(scene, prefix + "lamp-" + std::to_string(i));
            REQUIRE(lamp != nullptr);
            CHECK((*lamp)["primitive"] == "cylinder");
            CHECK((*lamp)["position"][0].get<double>() == Approx(static_cast<double>(hall.lights[i].xMm) / 1000.0));
            const double light = static_cast<double>(hall.heightMm - rv::world::kHallLightDropMm) / 1000.0;
            const double foot = (*lamp)["position"][1].get<double>() - (*lamp)["params"]["height"].get<double>() / 2.0;
            CHECK(foot >= light);
            CHECK(foot <= light + 0.05);
            CHECK(foot + (*lamp)["params"]["height"].get<double>() < static_cast<double>(hall.heightMm) / 1000.0 - 0.22);
        }
        // The wainscot's battens stand on the floor's 0.6 m grid, by their place (an edge's along x, an end wall's
        // along z), whatever their run; the plaster's uvs anchored at their place.
        std::size_t battens = 0;
        for (const nlohmann::json& object : scene["objects"]) {
            const std::string id = object["id"].get<std::string>();
            if (!id.starts_with(prefix + "trim/")) {
                continue;
            }
            INFO(id);
            const bool endWall = id.starts_with(prefix + "trim/west") || id.starts_with(prefix + "trim/east");
            if (id.find("/batten") != std::string::npos) {
                ++battens;
                const std::int64_t along = mm(object["position"][endWall ? 2 : 0].get<double>());
                CHECK(along % 600 == 0);
            }
            if (id.ends_with("/plaster")) {
                CHECK(object["params"]["uvOrigin"] == object["position"]);
            }
        }
        CHECK(battens > 10);
        // The runner starts past the elevator's zone; nothing of the dressing is a light.
        const nlohmann::json* runner = findObject(scene, prefix + "runner-0");
        REQUIRE(runner != nullptr);
        CHECK((*runner)["position"][0].get<double>() - 1.2 >= static_cast<double>(hall.elevatorZone.maxXMm) / 1000.0);
        for (const nlohmann::json& light : scene["lights"]) {
            const std::string lightName = light["name"].get<std::string>();
            CHECK((!lightName.starts_with(prefix) || lightName.starts_with(prefix + "light-") ||
                   lightName.starts_with(prefix + "doorway-fill:")));
        }
        // Through the collision world: the walker stands at the arrival point and before every doorway, never in a
        // bench or a plant.
        auto description = rv::world::toFloorSceneDescription(plan, floor.id, data);
        REQUIRE(description.has_value());
        const rv::scene::CollisionWorld world =
            rv::scene::buildCollisionWorld(rv::scene::buildSceneGeometry(*description, *materials));
        const rv::scene::Capsule capsule;
        CHECK_FALSE(world.overlaps({1.2f, 0.0f, 0.0f}, capsule));
        for (const rv::world::FloorDoorway& doorway : hall.doorways) {
            const float x = static_cast<float>(doorway.minMm + doorway.maxMm) / 2000.0f;
            const float edge = static_cast<float>(doorway.hallEdgeZMm) / 1000.0f;
            CHECK_FALSE(world.overlaps({x, 0.0f, edge * 0.6f}, capsule));
        }
        for (const rv::world::HallPiece& piece : hall.dressing) {
            if (piece.solid()) {
                INFO(piece.id);
                const glm::vec3 centre(static_cast<float>(piece.footprint.minXMm + piece.footprint.maxXMm) / 2000.0f,
                                       0.0f,
                                       static_cast<float>(piece.footprint.minZMm + piece.footprint.maxZMm) / 2000.0f);
                CHECK(world.overlaps(centre, capsule));
            }
        }
    }
    // The building's halls hold some of each.
    CHECK(benches >= 3);
    CHECK(plants >= 3);
    // A full floor's hall (every place of gen4's rows holding a room): the same rules at every doorway.
    const FullFloor full = fullGen4Floor(building);
    REQUIRE(full.hall.doorways.size() >= 20);
    checkDressingClear(full.hall);
}

TEST_CASE("world/floor: the hall's materials: the pack's hall kind, a role or kind it lacks the built-in pack's",
          "[world][floor][dressing]") {
    const GoldenBuilding& building = goldenBuilding();
    const WorldPlan& plan = building.plan;
    const rv::world::ThemeMaterials& builtIn = rv::world::builtInThemeMaterials();
    const auto materialOf = [](const nlohmann::json& scene, const std::string& id, const std::string& role) {
        const nlohmann::json* object = findObject(scene, id);
        REQUIRE(object != nullptr);
        return role.empty() ? (*object)["material"].get<std::string>()
                            : (*object)["materials"][role].get<std::string>();
    };
    rv::world::FloorSceneData data = floorData(plan, "gen4", building.tags);
    const RoomId snes = *RoomId::forSystem(name("snes"));
    REQUIRE(data.rooms.contains(snes));
    // The hall takes its floor's pack (I6 step 3), gen4's classic_16bit_warm.
    CHECK(data.hallTheme.id == "classic_16bit_warm");
    const auto everyPack = [&](const auto& change) { change(data.hallTheme); };
    // A pack without the hall kind: every role the built-in pack's.
    everyPack([](rv::world::ThemeMaterials& theme) { theme.kinds.erase("hall"); });
    nlohmann::json scene = floorScene(plan, "gen4", data).sceneFile;
    const std::map<std::string, std::string> hall = builtIn.materials("hall");
    CHECK(materialOf(scene, "hall:gen4/wall-west", "") == hall.at("wall"));
    CHECK(materialOf(scene, "hall:gen4/ceiling", "") == hall.at("ceiling"));
    CHECK(materialOf(scene, "hall:gen4/runner-0", "fabric") == hall.at("runner"));
    // A pack with a hall of its own but no runner, no plant and a poster frame without its picture: its own roles
    // kept, the others the built-in pack's.
    everyPack([](rv::world::ThemeMaterials& theme) {
        theme.kinds["hall"] = {{"wall", "MAT_Wall_Painted"}, {"sign", "MAT_Neon_Diffuser"}};
        theme.kinds.erase("plant");
        theme.kinds["poster_frame"] = {{"frame", "MAT_Wood_Oak"}};
    });
    scene = floorScene(plan, "gen4", data).sceneFile;
    CHECK(materialOf(scene, "hall:gen4/wall-west", "") == "MAT_Wall_Painted");
    CHECK(materialOf(scene, "hall:gen4/portal:room:snes/sign", "picture") == "MAT_Neon_Diffuser");
    CHECK(materialOf(scene, "hall:gen4/runner-0", "fabric") == hall.at("runner"));
    CHECK(materialOf(scene, "hall:gen4/portal:room:snes/sign", "frame") == hall.at("trim"));
    std::size_t prints = 0;
    std::size_t plants = 0;
    for (const nlohmann::json& object : scene["objects"]) {
        const std::string id = object["id"].get<std::string>();
        if (!id.starts_with("hall:gen4/")) {
            continue;
        }
        INFO(id);
        if (id.ends_with("/picture")) {
            ++prints;
            CHECK(object["materials"]["picture"] == builtIn.materials("poster_frame").at("picture"));
            CHECK(object["materials"]["frame"] == hall.at("picture"));
        }
        if (id.ends_with("/plant")) {
            ++plants;
            CHECK(object["materials"]["leaves"] == builtIn.materials("plant").at("leaves"));
            CHECK(object["materials"]["pot"] == hall.at("plant"));
        }
    }
    CHECK(prints > 0);
    CHECK(plants > 0);
    // A room without a sign text: its portal reads a generic sign, never its id.
    data.rooms.at(snes).platform.signText.clear();
    scene = floorScene(plan, "gen4", data).sceneFile;
    for (const std::string part : {"/sign", "/blade-east", "/blade-west"}) {
        const nlohmann::json* sign = findObject(scene, "hall:gen4/portal:room:snes" + part);
        REQUIRE(sign != nullptr);
        CHECK((*sign)["textures"]["picture"]["title"] == "GAME ROOM");
    }
}

TEST_CASE("world/floor: a narrow hall keeps its passage: floor pieces only where 1.4 m stays clear",
          "[world][floor][dressing]") {
    // gen4 filled up for a long hall, its rows' rooms left where they are (their doorways then off a narrower hall's
    // edge: no doorway margins), the hall narrowed.
    GrownFloor grown = grownGen4Floor(goldenBuilding(), 40);
    std::vector<rv::world::FloorRoom> rooms;
    for (const rv::world::Room* room : rv::world::roomsOnFloor(grown.plan, "gen4")) {
        rooms.push_back({room, &grown.data.rooms.at(room->id).roomTemplate});
    }
    const auto count = [](const Hall& hall, rv::world::HallPieceKind kind) {
        return std::count_if(hall.dressing.begin(), hall.dressing.end(),
                             [&](const rv::world::HallPiece& piece) { return piece.kind == kind; });
    };
    rv::world::BuildingLayout layout = grown.data.building.layout;
    for (const std::int64_t width : {3000, 2100, 2000, 1800}) {
        INFO("width " << width);
        layout.hallWidthMm = width;
        const Hall hall = rv::world::deriveHall("gen4", rooms, layout);
        for (const rv::world::HallPiece& piece : hall.dressing) {
            if (piece.solid()) {
                INFO(piece.id);
                const std::int64_t reach = piece.north ? piece.footprint.maxZMm + width / 2
                                                       : width / 2 - piece.footprint.minZMm;
                CHECK(width - reach >= rv::world::kHallMinClearMm);
            }
        }
        // A bench takes 0.45 m, a plant 0.63 m: at 2 m the benches stay and the plants go, at 1.8 m both go.
        CHECK((count(hall, rv::world::HallPieceKind::Bench) > 0) == (width >= 1850));
        CHECK((count(hall, rv::world::HallPieceKind::Plant) > 0) == (width >= 2030));
        CHECK(count(hall, rv::world::HallPieceKind::Picture) > 0);
    }
}

TEST_CASE("world/floor: a longer hall appends its dressing and moves none", "[world][floor][dressing]") {
    const GoldenBuilding& building = goldenBuilding();
    // gen4's room:snes, then clones of it in the places its rows give (north and south in turn): each floor's scene
    // against the one before.
    std::vector<nlohmann::json> scenes;
    std::vector<Hall> halls;
    for (const int clones : {0, 1, 2, 3, 5, 8}) {
        GrownFloor grown = grownGen4Floor(building, clones);
        const rv::world::FloorSceneBuild build = floorScene(grown.plan, "gen4", grown.data);
        scenes.push_back(build.sceneFile);
        halls.push_back(build.hall);
    }
    const std::string prefix = "hall:gen4/";
    const auto dressing = [&](const nlohmann::json& scene) {
        std::map<std::string, nlohmann::json> objects;
        for (const nlohmann::json& object : scene["objects"]) {
            const std::string id = object["id"].get<std::string>();
            if (isFixedDressing(id, prefix)) {
                objects.emplace(id, object);
            }
        }
        return objects;
    };
    for (std::size_t i = 1; i < scenes.size(); ++i) {
        INFO("step " << i);
        const auto before = dressing(scenes[i - 1]);
        const auto after = dressing(scenes[i]);
        CHECK(halls[i].interior.maxXMm >= halls[i - 1].interior.maxXMm);
        std::size_t kept = 0;
        for (const auto& [id, object] : before) {
            INFO(id);
            const auto found = after.find(id);
            if (found != after.end()) {
                CHECK(found->second == object); // the same place, size and look
                ++kept;
                continue;
            }
            // Gone: a slot's piece where a doorway that entered since stands (its margin).
            const std::string slot = id.substr(prefix.size(), id.find('/', prefix.size()) - prefix.size());
            REQUIRE((slot.starts_with("north-") || slot.starts_with("south-")));
            const auto piece =
                std::find_if(halls[i - 1].dressing.begin(), halls[i - 1].dressing.end(),
                             [&](const rv::world::HallPiece& candidate) { return candidate.id.starts_with(slot + "/"); });
            REQUIRE(piece != halls[i - 1].dressing.end());
            const bool nearNewDoorway = std::any_of(
                halls[i].doorways.begin(), halls[i].doorways.end(), [&](const rv::world::FloorDoorway& doorway) {
                    const bool entered = std::none_of(
                        halls[i - 1].doorways.begin(), halls[i - 1].doorways.end(),
                        [&](const rv::world::FloorDoorway& old) { return old.room == doorway.room; });
                    return entered &&
                           piece->footprint.maxXMm > doorway.minMm - rv::world::kHallSconceDoorMarginMm &&
                           piece->footprint.minXMm < doorway.maxMm + rv::world::kHallSconceDoorMarginMm;
                });
            CHECK(nearNewDoorway);
        }
        CHECK(kept > 0);
        // The pieces deriveHall() gives alike: a piece kept is the same piece.
        for (const rv::world::HallPiece& piece : halls[i - 1].dressing) {
            const auto same = std::find_if(halls[i].dressing.begin(), halls[i].dressing.end(),
                                           [&](const rv::world::HallPiece& candidate) { return candidate.id == piece.id; });
            if (same != halls[i].dressing.end()) {
                CHECK(*same == piece);
            }
        }
    }
    // The hall grew east: more dressing at the end than at the start.
    CHECK(dressing(scenes.back()).size() > dressing(scenes.front()).size());
    CHECK(halls.back().dressing.size() > halls.front().dressing.size());
}
