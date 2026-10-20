// A floor of the building in retroverse.exe (I4 step 3, ADR-0013 §6, §9): the lights of the area that holds the eye.
//   - areas: tests/smoke/scenes/two_areas.json, two areas each lit by its own key spot, rendered from a spawn in each
//     and from a variant without areas: from the west only the west's pool is lit (the east's shows the ambient
//     alone), from the east the reverse, and without areas both, the run report naming the area, its lights and its
//     key light;
//   - floor: the building fixture (tests/fixtures/esde/building) recorded as the library of the case's data directory,
//     imported and planned with retroverse-cli (worldplan generate), gen4's floor written with worldplan show --floor
//     gen4 --scene, and retroverse.exe --scene of it walking tests/input_scripts/floor_walk_into_room.json from the
//     elevator along the hall and through the snes room's doorway (its stop left out on a floor): in the hall the
//     hall's lights alone are packed, in the room the room's own with its key light, the walker past the doorway, no
//     high or medium GL message; the ground floor's lobby from the elevator likewise. The screenshots stay in
//     build/<preset>/smoke/world_floor for a look.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/scene/scene_description.hpp>

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
        for (const std::string& light : areas.lights) {
            CHECK(startsWith(light, "hall:gen4/light-"));
        }
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
        for (const std::string& light : areas.lights) {
            CHECK(startsWith(light, "room:snes/"));
        }
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
