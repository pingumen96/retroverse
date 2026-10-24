// A floor of the building in retroverse.exe (I4 step 3, ADR-0013 §6, §9): the lights of the area that holds the eye.
//   - areas: tests/smoke/scenes/two_areas.json, two areas each lit by its own key spot, rendered from a spawn in each
//     and from a variant without areas: from the west only the west's pool is lit (the east's shows the ambient
//     alone), from the east the reverse, and without areas both, the run report naming the area, its lights and its
//     key light; the pillar of the eye's area shadowed by that area's key light (without areas the west spot is the
//     only key light, so from the east the east pillar casts no shadow);
//   - floor: the building fixture (tests/fixtures/esde/building) recorded as the library of the case's data directory,
//     imported and planned with retroverse-cli (worldplan generate), gen4's floor written with worldplan show --floor
//     gen4 --scene, and retroverse.exe --scene of it walking tests/input_scripts/floor_walk_into_room.json from the
//     elevator along the hall and through the snes room's doorway (its stop left out on a floor): in the hall the
//     hall's lights are packed first (then the room's near its doorway, I6 step 1), in the room the room's own with its
//     key light first, the walker past the doorway, no
//     high or medium GL message; the ground floor's lobby from the elevator likewise. The screenshots stay in
//     build/<preset>/smoke/world_floor for a look;
//   - light through doorways (I6 step 1, ADR-0014 §4): every floor of the building, each room seen from its hall and
//     the hall from the room, against the same scene lit as I4 lit it (world_floor_light).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{960, 540};
constexpr float kEyeHeight = 1.65f;
constexpr int kProbeRadius = 4;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The camera of a run spawned with its feet at `feet`, looking along `yawDeg` and `pitchDeg`.
rv::scene::CameraSetup cameraAt(const glm::vec3& feet, float yawDeg, float pitchDeg) {
    rv::scene::CameraSetup camera;
    camera.position = feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    camera.yawDeg = yawDeg;
    camera.pitchDeg = pitchDeg;
    return camera;
}

glm::ivec2 probe(const rv::scene::CameraSetup& camera, const glm::vec3& world) {
    const auto pixel = projectToPixel(camera, kShotSize, world);
    REQUIRE(pixel.has_value());
    return *pixel;
}

/// The mean red and blue of the window around `center` (sRGB bytes).
glm::dvec2 redBlue(const rv::assets::ImageData& image, glm::ivec2 center) {
    glm::dvec2 sum(0.0);
    int count = 0;
    const int channels = image.channels;
    for (int y = center.y - kProbeRadius; y <= center.y + kProbeRadius; ++y) {
        for (int x = center.x - kProbeRadius; x <= center.x + kProbeRadius; ++x) {
            if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
                continue;
            }
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                                    static_cast<std::size_t>(x)) *
                                   static_cast<std::size_t>(channels);
            sum += glm::dvec2(image.pixels[at], image.pixels[at + 2]);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : sum;
}

struct Shot {
    rv::app::RunReport report;
    rv::assets::ImageData image;
};

Shot renderAreas(const fs::path& dir, const std::string& name, const fs::path& scene, const glm::vec3& feet) {
    const fs::path shot = dir / (name + ".png");
    const AppRun run = runApp(retroverseExe(), dir, name,
                              {"--scene", arg(scene), "--smoke", "--frames", "3", "--spawn",
                               spawnArgument(rv::app::SpawnPoint{feet, 0.0f, -20.0f}), "--screenshot", arg(shot),
                               "--screenshot-size", "960x540"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    return Shot{*run.report, requireScreenshot(run, shot, kShotSize)};
}

/// The building fixture recorded as the library of <caseDir>/data, imported and planned with retroverse-cli.
struct PlannedBuilding {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    PlannedBuilding()
        : fixture(rv::test::Fixture::materialize("esde/building").value()), dir(freshCaseDir("world_floor")),
          data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        cli("generate", {"worldplan", "generate", "--seed", "102938"});
    }

    AppRun cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
        return run;
    }

    /// worldplan show --floor <floor> --scene <dir>/<floor>.scene.json.
    fs::path floorScene(std::string_view floor) const {
        const fs::path scene = dir / (std::string(floor) + ".scene.json");
        cli(std::string("show_") + std::string(floor), {"worldplan", "show", "--floor", std::string(floor), "--scene", arg(scene)});
        REQUIRE(fs::exists(scene));
        return scene;
    }
};

/// A frame-limited lockstep run of a floor's scene at 1920 x 1080 with the walk script, ending on `frames`.
AppRun walkFloor(const PlannedBuilding& building, const fs::path& scene, const std::string& name, int frames) {
    const fs::path shot = building.dir / (name + ".png");
    const AppRun run = runApp(retroverseExe(), building.dir, name,
                              {"--scene", arg(scene), "--exit-after-frames", "--frames", std::to_string(frames),
                               "--fixed-dt", "0.016667", "--input-script",
                               arg(sourceDir() / "tests/input_scripts/floor_walk_into_room.json"), "--screenshot",
                               arg(shot)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    (void)requireScreenshot(run, shot, {1920, 1080});
    return run;
}

bool startsWith(const std::string& text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

} // namespace

TEST_CASE("world floor: areas: the renderer packs the lights of the area that holds the eye", "[gpu][world_floor]") {
    const fs::path dir = freshCaseDir("world_floor_areas");
    const fs::path scene = sourceDir() / "tests/smoke/scenes/two_areas.json";
    const fs::path plain = dir / "two_areas_without_areas.json";
    writeSceneVariant(scene, plain, [](nlohmann::json& json) {
        json.erase("areas");
        for (nlohmann::json& light : json["lights"]) {
            light.erase("area");
        }
    });
    const glm::vec3 westFeet(-1.0f, 0.0f, 8.0f);
    const glm::vec3 eastFeet(1.0f, 0.0f, 8.0f);
    const Shot west = renderAreas(dir, "from_west", scene, westFeet);
    const Shot east = renderAreas(dir, "from_east", scene, eastFeet);
    const Shot bothFromWest = renderAreas(dir, "without_areas_from_west", plain, westFeet);
    const Shot bothFromEast = renderAreas(dir, "without_areas_from_east", plain, eastFeet);

    // The run reports: the area of the eye, its one light, its key light; none for the scene without areas.
    REQUIRE(west.report.sceneAreas.has_value());
    CHECK(west.report.sceneAreas->areas == 2);
    CHECK(west.report.sceneAreas->area == "west");
    CHECK(west.report.sceneAreas->lights == std::vector<std::string>{"west key"});
    CHECK(west.report.sceneAreas->keyLight == "west key");
    REQUIRE(east.report.sceneAreas.has_value());
    CHECK(east.report.sceneAreas->area == "east");
    CHECK(east.report.sceneAreas->lights == std::vector<std::string>{"east key"});
    CHECK(east.report.sceneAreas->keyLight == "east key");
    CHECK_FALSE(bothFromWest.report.sceneAreas.has_value());

    // The pools on the floor under each spot, seen from each spawn.
    const glm::vec3 westPool(-4.0f, 0.0f, 0.5f);
    const glm::vec3 eastPool(4.0f, 0.0f, 0.5f);
    const rv::scene::CameraSetup fromWest = cameraAt(westFeet, 0.0f, -20.0f);
    const rv::scene::CameraSetup fromEast = cameraAt(eastFeet, 0.0f, -20.0f);
    const auto lumaOf = [](const Shot& shot, const rv::scene::CameraSetup& camera, const glm::vec3& point) {
        return windowLuma(shot.image, probe(camera, point), kProbeRadius);
    };
    const double westLit = lumaOf(west, fromWest, westPool);
    const double westDark = lumaOf(west, fromWest, eastPool);
    const double eastLit = lumaOf(east, fromEast, eastPool);
    const double eastDark = lumaOf(east, fromEast, westPool);
    const double bothWest = lumaOf(bothFromWest, fromWest, eastPool);
    const double bothEast = lumaOf(bothFromEast, fromEast, westPool);
    INFO("from the west: its pool " << westLit << ", the east's " << westDark << " (" << bothWest
                                    << " with both lights); from the east: its pool " << eastLit << ", the west's "
                                    << eastDark << " (" << bothEast << " with both lights)");
    // The eye's area's pool is lit, the other area's shows the ambient alone: far darker than with both lights.
    CHECK(westLit > 4.0 * westDark);
    CHECK(eastLit > 4.0 * eastDark);
    CHECK(bothWest > 4.0 * westDark);
    CHECK(bothEast > 4.0 * eastDark);
    // The pool of the eye's area is as the scene without areas lights it (the other spot never reaches it).
    CHECK(windowMaxDifference(west.image, bothFromWest.image, probe(fromWest, westPool), kProbeRadius) <= 1);
    CHECK(windowMaxDifference(east.image, bothFromEast.image, probe(fromEast, eastPool), kProbeRadius) <= 1);
    // Each pool its own light's colour: the west's warm, the east's cool.
    const glm::dvec2 warm = redBlue(west.image, probe(fromWest, westPool));
    const glm::dvec2 cool = redBlue(east.image, probe(fromEast, eastPool));
    CHECK(warm.x > warm.y);
    CHECK(cool.y > cool.x);

    // The shadow-mapped light is the eye's area's key light: each pillar's shadow (the shadow of its top's centre, cast
    // towards the viewer) is dark beside the same point mirrored across its spot's axis, lit alike but unshadowed. The
    // scene without areas shadow-maps the west spot alone (the first caster of the list): from the east, the east
    // pillar casts nothing there.
    const glm::vec3 westShadow(-4.913f, 0.0f, 2.13f);
    const glm::vec3 westBeside(-3.087f, 0.0f, 2.13f);
    const glm::vec3 eastShadow(4.913f, 0.0f, 2.13f);
    const glm::vec3 eastBeside(3.087f, 0.0f, 2.13f);
    const double westShade = lumaOf(west, fromWest, westShadow) / lumaOf(west, fromWest, westBeside);
    const double eastShade = lumaOf(east, fromEast, eastShadow) / lumaOf(east, fromEast, eastBeside);
    const double westShadePlain =
        lumaOf(bothFromWest, fromWest, westShadow) / lumaOf(bothFromWest, fromWest, westBeside);
    const double eastShadePlain =
        lumaOf(bothFromEast, fromEast, eastShadow) / lumaOf(bothFromEast, fromEast, eastBeside);
    INFO("shadow beside lit floor: from the west " << westShade << " (" << westShadePlain
                                                   << " without areas), from the east " << eastShade << " ("
                                                   << eastShadePlain << " without areas)");
    CHECK(westShade < 0.5);
    CHECK(eastShade < 0.5);
    CHECK(westShadePlain < 0.5);
    CHECK(eastShadePlain > 0.85);
}

TEST_CASE("world floor: a walk from the elevator along the hall into a room, each area lit by its own lights",
          "[gpu][world_floor]") {
    const PlannedBuilding building;
    const fs::path gen4 = building.floorScene("gen4");
    const fs::path ground = building.floorScene("ground");

    // Half way down the hall (frame 119): the hall's lights alone.
    const AppRun hall = walkFloor(building, gen4, "gen4_hall", 120);
    {
        INFO(hall.describe());
        REQUIRE(hall.report->sceneAreas.has_value());
        const rv::app::SceneAreasRecord& areas = *hall.report->sceneAreas;
        CHECK(areas.areas == 2);
        CHECK(areas.area == "hall:gen4");
        REQUIRE_FALSE(areas.lights.empty());
        // The hall's own lights first, then (I6 step 1) the snes room's near its doorway; no key light.
        CHECK(startsWith(areas.lights.front(), "hall:gen4/light-"));
        const auto firstRoom = std::find_if(areas.lights.begin(), areas.lights.end(),
                                            [](const std::string& light) { return startsWith(light, "room:snes/"); });
        CHECK(std::all_of(areas.lights.begin(), firstRoom,
                          [](const std::string& light) { return startsWith(light, "hall:gen4/"); }));
        CHECK(std::all_of(firstRoom, areas.lights.end(),
                          [](const std::string& light) { return startsWith(light, "room:snes/"); }));
        CHECK(areas.keyLight.empty());
        REQUIRE(hall.report->finalCamera.has_value());
        CHECK(std::abs(hall.report->finalCamera->feet.x - (1.2f + 120.0f * 2.5f * 0.016667f)) < 0.01f);
        CHECK(std::abs(hall.report->finalCamera->feet.z) < 0.01f);
    }
    // Through the doorway into the snes room: its own lights and its key light; the walker past the doorway.
    const AppRun room = walkFloor(building, gen4, "gen4_room", 255);
    {
        INFO(room.describe());
        REQUIRE(room.report->sceneAreas.has_value());
        const rv::app::SceneAreasRecord& areas = *room.report->sceneAreas;
        CHECK(areas.area == "room:snes");
        REQUIRE_FALSE(areas.lights.empty());
        CHECK(areas.lights.size() <= 16);
        // The room's own lights first, then (I6 step 1) the hall's near the doorway, if any fit.
        const auto firstHall = std::find_if(areas.lights.begin(), areas.lights.end(),
                                            [](const std::string& light) { return startsWith(light, "hall:gen4/"); });
        CHECK(firstHall != areas.lights.begin());
        CHECK(std::all_of(areas.lights.begin(), firstHall,
                          [](const std::string& light) { return startsWith(light, "room:snes/"); }));
        CHECK(std::all_of(firstHall, areas.lights.end(),
                          [](const std::string& light) { return startsWith(light, "hall:gen4/"); }));
        CHECK(areas.keyLight == "room:snes/fx:hero.key-light");
        REQUIRE(areas.changes.size() == 2);
        CHECK(areas.changes[0].frame == 0);
        CHECK(areas.changes[0].area == "hall:gen4");
        CHECK(areas.changes[1].area == "room:snes");
        CHECK(areas.changes[1].frame > 165);
        REQUIRE(room.report->finalCamera.has_value());
        // 3.75 m north of the hall's centre line, past the room's south wall (its inner face at z -1.65).
        CHECK(room.report->finalCamera->feet.z < -3.7f);
        CHECK(std::abs(room.report->finalCamera->feet.x - 7.659f) < 0.01f);
        REQUIRE(room.report->lastView.has_value());
        CHECK(room.report->lastView->shadowDrawCalls > 0);
    }
    // The ground floor's lobby from the elevator: the lobby's lights.
    const fs::path shot = building.dir / "ground_lobby.png";
    const AppRun lobby = runApp(retroverseExe(), building.dir, "ground_lobby",
                                {"--scene", arg(ground), "--exit-after-frames", "--frames", "5", "--fixed-dt",
                                 "0.016667", "--screenshot", arg(shot)});
    skipWithoutOpenGl(lobby);
    requireCleanRun(lobby);
    const rv::assets::ImageData image = requireScreenshot(lobby, shot, {1920, 1080});
    REQUIRE(lobby.report->sceneAreas.has_value());
    CHECK(lobby.report->sceneAreas->area == "hall:ground");
    // The lobby reads lit: its floor ahead and walls well above black.
    CHECK(lumaSummary(image).mean > 20.0);
}

namespace {

/// The scene of a floor as I4 lit it, for a comparison (I6 step 1): no area neighbours another (each room's doorway
/// passage drawn back 2 mm from the hall), no doorway fill, no area environment, I4's evening ambient (ground #4A4642,
/// 0.25, no fill) and the hall's lights in the profile's pendant style.
void asLitInI4(nlohmann::json& scene) {
    for (nlohmann::json& area : scene["areas"]) {
        area.erase("environment");
        nlohmann::json& boxes = area["boxes"];
        for (std::size_t i = 1; i < boxes.size(); ++i) {
            boxes[i]["min"][2] = boxes[i]["min"][2].get<double>() + 0.002;
            boxes[i]["max"][2] = boxes[i]["max"][2].get<double>() - 0.002;
        }
    }
    nlohmann::json lights = nlohmann::json::array();
    for (nlohmann::json& light : scene["lights"]) {
        const std::string name = light["name"].get<std::string>();
        if (name.find("/doorway-fill:") != std::string::npos) {
            continue;
        }
        if (name.rfind("hall:", 0) == 0) {
            light["color"] = "#FFD7B0";
            light["intensity"] = 16.0;
            light["range"] = 10.0;
        }
        lights.push_back(light);
    }
    scene["lights"] = std::move(lights);
    nlohmann::json& environment = scene["environment"];
    environment["ambientGround"] = "#4A4642";
    environment["ambientIntensity"] = 0.25;
    environment.erase("fillColor");
    environment.erase("fillIntensity");
}

/// A room of a floor scene seen through its doorway: its area's id, its doorway's passage (floor frame), its doorway's
/// height and whether it lies north of the hall.
struct DoorwayView {
    std::string room;
    glm::vec3 passageMin{0.0f};
    glm::vec3 passageMax{0.0f};
    float doorHeight = 0.0f;
    bool north = true;
};

glm::vec3 vec3Of(const nlohmann::json& value) {
    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

/// The rooms of a floor scene with a doorway onto its hall (an area of two boxes: the interior, then the passage).
std::vector<DoorwayView> doorwaysOf(const nlohmann::json& scene) {
    std::vector<DoorwayView> views;
    for (const nlohmann::json& area : scene["areas"]) {
        if (area["boxes"].size() < 2) {
            continue;
        }
        DoorwayView view;
        view.room = area["id"].get<std::string>();
        view.passageMin = vec3Of(area["boxes"][1]["min"]);
        view.passageMax = vec3Of(area["boxes"][1]["max"]);
        view.north = area["boxes"][0]["min"][2].get<float>() < view.passageMin.z;
        for (const nlohmann::json& object : scene["objects"]) {
            if (object["id"] == view.room + "/room") {
                view.doorHeight = object["params"]["doorway"]["height"].get<float>();
            }
        }
        REQUIRE(view.doorHeight > 1.8f);
        views.push_back(view);
    }
    return views;
}

/// The mean luma of the doorway's opening between eye height and its head, as `camera` sees it in `image`: through it,
/// the room's upper walls and ceiling from the hall, the hall's from the room (`faceZ`: the passage's face nearest the
/// camera).
double openingLuma(const rv::assets::ImageData& image, const rv::scene::CameraSetup& camera, const DoorwayView& door,
                   float faceZ) {
    const glm::ivec2 size(image.width, image.height);
    const float inset = 0.15f;
    const auto a = projectToPixel(camera, size, {door.passageMin.x + inset, door.doorHeight - inset, faceZ});
    const auto b = projectToPixel(camera, size, {door.passageMax.x - inset, kEyeHeight + 0.15f, faceZ});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    const glm::ivec2 low = glm::min(*a, *b);
    const glm::ivec2 high = glm::max(*a, *b);
    REQUIRE(high.x - low.x >= 8);
    REQUIRE(high.y - low.y >= 4);
    double sum = 0.0;
    int count = 0;
    for (int y = low.y; y <= high.y; ++y) {
        for (int x = low.x; x <= high.x; ++x) {
            sum += lumaAt(image, x, y);
            ++count;
        }
    }
    return sum / count;
}

/// A frame-limited run of a floor's scene at 1920 x 1080 from `feet` (none: the scene's camera, before the elevator).
Shot renderFloor(const fs::path& dir, const std::string& name, const fs::path& scene,
                 const std::optional<rv::app::SpawnPoint>& feet) {
    const fs::path shot = dir / (name + ".png");
    std::vector<std::string> arguments{"--scene", arg(scene), "--exit-after-frames", "--frames", "3", "--fixed-dt",
                                       "0.016667", "--screenshot", arg(shot)};
    if (feet) {
        arguments.insert(arguments.end(), {"--spawn", spawnArgument(*feet)});
    }
    const AppRun run = runApp(retroverseExe(), dir, name, arguments);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    REQUIRE(run.report->sceneAreas.has_value());
    return Shot{*run.report, requireScreenshot(run, shot, {1920, 1080})};
}

} // namespace

TEST_CASE("world floor: light reaches through the doorways: each room glows seen from its hall, the hall from the room",
          "[gpu][world_floor]") {
    // I6 step 1 (ADR-0014 §4): every floor of esde/building, from its elevator, then for each room from the hall before
    // its doorway looking in and from 4.5 m inside looking out, with the step's light and with the same scene lit as
    // I4 lit it (asLitInI4). Through the doorway, between eye height and its head (the far side's upper walls and
    // ceiling): I4 showed about 4 to 8 of 255 there (black: the far area had the ambient alone, its ground term a dark
    // grey); now the far area's lights near the doorway, the fill spot and the warmer ambient light it, at least twice
    // as bright and above 12 (a dark but readable wall). The key light stays the eye's area's own: none in the hall,
    // the room's from the room. The screenshots stay in build/<preset>/smoke/world_floor_light for a look.
    PlannedBuilding building;
    const fs::path dir = freshCaseDir("world_floor_light");
    const auto stored = rv::world::readStoredPlan(
        rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse("esde").value()).plan);
    REQUIRE(stored.has_value());
    REQUIRE(stored->plan.floors.size() >= 5);
    std::size_t rooms = 0;
    for (const rv::world::Floor& floor : stored->plan.floors) {
        INFO(floor.id);
        const fs::path scene = building.floorScene(floor.id);
        const auto file = rv::core::readJsonFile(scene);
        REQUIRE(file.has_value());
        const fs::path old = dir / (floor.id + ".i4.scene.json");
        writeSceneVariant(scene, old, asLitInI4);
        const std::string hall = "hall:" + floor.id;

        const Shot elevator = renderFloor(dir, floor.id + "_elevator", scene, std::nullopt);
        CHECK(elevator.report.sceneAreas->area == hall);
        CHECK(elevator.report.sceneAreas->keyLight.empty());

        const nlohmann::json* hallArea = nullptr;
        for (const nlohmann::json& area : (*file)["areas"]) {
            hallArea = area["id"] == hall ? &area : hallArea;
        }
        REQUIRE(hallArea != nullptr);
        const nlohmann::json& hallBox = (*hallArea)["boxes"][0];
        for (const DoorwayView& door : doorwaysOf(*file)) {
            INFO(door.room);
            ++rooms;
            const float x = (door.passageMin.x + door.passageMax.x) / 2.0f;
            // From the hall's far side, facing the doorway; from 4.5 m inside the room, facing out.
            const float hallZ = door.north ? hallBox["max"][2].get<float>() - 0.4f : hallBox["min"][2].get<float>() + 0.4f;
            const float roomZ = door.north ? door.passageMin.z - 4.5f : door.passageMax.z + 4.5f;
            const float inYaw = door.north ? 0.0f : 180.0f;
            const float outYaw = door.north ? 180.0f : 0.0f;
            const rv::app::SpawnPoint fromHall{{x, 0.0f, hallZ}, inYaw, -4.0f};
            const rv::app::SpawnPoint fromRoom{{x, 0.0f, roomZ}, outYaw, -4.0f};
            std::string tag = floor.id + "_" + door.room;
            std::replace(tag.begin(), tag.end(), ':', '_');
            const Shot inward = renderFloor(dir, tag + "_from_hall", scene, fromHall);
            const Shot inwardI4 = renderFloor(dir, tag + "_from_hall_i4", old, fromHall);
            const Shot outward = renderFloor(dir, tag + "_looking_out", scene, fromRoom);
            const Shot outwardI4 = renderFloor(dir, tag + "_looking_out_i4", old, fromRoom);

            const float hallFace = door.north ? door.passageMax.z : door.passageMin.z;
            const float roomFace = door.north ? door.passageMin.z : door.passageMax.z;
            const rv::scene::CameraSetup hallCamera = cameraAt(fromHall.feet, inYaw, -4.0f);
            const rv::scene::CameraSetup roomCamera = cameraAt(fromRoom.feet, outYaw, -4.0f);
            const double roomSeen = openingLuma(inward.image, hallCamera, door, hallFace);
            const double roomSeenI4 = openingLuma(inwardI4.image, hallCamera, door, hallFace);
            const double hallSeen = openingLuma(outward.image, roomCamera, door, roomFace);
            const double hallSeenI4 = openingLuma(outwardI4.image, roomCamera, door, roomFace);
            INFO("through the doorway, above eye height: the room from the hall " << roomSeen << " (I4: " << roomSeenI4
                                                                                 << "), the hall from the room "
                                                                                 << hallSeen << " (I4: " << hallSeenI4
                                                                                 << ")");
            CHECK(roomSeen > 12.0);
            CHECK(roomSeen > 2.0 * roomSeenI4);
            CHECK(hallSeen > 12.0);
            CHECK(hallSeen > 2.0 * hallSeenI4);

            // The hall's view packs the hall's lights, then the room's near its doorway; no key light.
            const rv::app::SceneAreasRecord& hallView = *inward.report.sceneAreas;
            CHECK(hallView.area == hall);
            CHECK(hallView.keyLight.empty());
            CHECK(std::any_of(hallView.lights.begin(), hallView.lights.end(),
                              [&](const std::string& light) { return startsWith(light, door.room + "/"); }));
            CHECK(std::any_of(hallView.lights.begin(), hallView.lights.end(), [&](const std::string& light) {
                return light == hall + "/doorway-fill:" + door.room;
            }));
            // The room's view: its own lights first with its own key light, then the hall's.
            const rv::app::SceneAreasRecord& roomView = *outward.report.sceneAreas;
            CHECK(roomView.area == door.room);
            CHECK(startsWith(roomView.keyLight, door.room + "/"));
            CHECK(roomView.lights.size() <= 16);
            const auto firstHall = std::find_if(roomView.lights.begin(), roomView.lights.end(),
                                                [&](const std::string& light) { return startsWith(light, hall + "/"); });
            REQUIRE(firstHall != roomView.lights.end());
            CHECK(std::all_of(roomView.lights.begin(), firstHall,
                              [&](const std::string& light) { return startsWith(light, door.room + "/"); }));
            CHECK(std::all_of(firstHall, roomView.lights.end(),
                              [&](const std::string& light) { return startsWith(light, hall + "/"); }));
            // As I4 lit it, the room's view held its own lights alone.
            CHECK(std::all_of(outwardI4.report.sceneAreas->lights.begin(), outwardI4.report.sceneAreas->lights.end(),
                              [&](const std::string& light) { return startsWith(light, door.room + "/"); }));
        }
    }
    CHECK(rooms >= 6);
}
