// The rooms' secondary TVs playing (I5 step 3, ADR-0016 §2 and §5): the snes-room fixture (tests/fixtures/esde/snes-room:
// invented titles, generated clips with their frame number burned in) recorded as the library of the case's data
// directory, imported and planned with retroverse-cli, then retroverse.exe --start-room room:snes, lockstep
// (--fixed-dt), hidden. The room's pack (classic_16bit_warm) stands its secondary TV in one of its sideboards' sockets
// (world::secondaryTvFixtures()); the run reports tell what the screens did (the video section's screens and slots'
// events):
//  - walking from the doorway towards the secondary TV with video.json's proximityMeters short of it, the hero keeps
//    the first slot from the room's first update while the secondary (`ambient`) has none and shows its still, then takes the
//    second slot on the frame the walker comes within its proximity, in view;
//  - with one decoder (--max-decoders 1) the hero keeps the slot and the secondary shows its still the whole walk.

#include "building_support.hpp"
#include "smoke_support.hpp"

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr std::string_view kRoom = "room:snes";
constexpr std::string_view kHeroScreen = "room:snes/fx:hero.crt";
constexpr double kStep = 0.016667;
/// scene::FirstPersonController's walk (2.5 m/s, at once: tests/input_scripts/walk_forward_2s.json) and eye.
constexpr double kWalkSpeed = 2.5;
constexpr float kEyeHeight = 1.65f;
/// How far short of the secondary's screen the proximity ends, seen from the doorway: the walk crosses it after
/// about this distance (1.2 m: 29 frames).
constexpr double kShortOfProximity = 1.2;
constexpr int kWalkFrames = 70;

/// The assets the runs of this build read (the source tree's in developer builds, the deployed copy otherwise).
fs::path buildAssets() {
    return buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
}

/// The room's secondary TV as its scene draws it: its object's id in the floor's scene (the screen's id), the middle
/// of its screen and the room's doorway camera (the room's frame: --start-room's).
struct SecondaryTv {
    std::string screen;
    glm::vec3 screenPoint{0.0f};
    /// The middle of its object's bounds: the screen service's distance to it (ScreenService::update()).
    glm::vec3 boundsCentre{0.0f};
    glm::vec3 doorwayEye{0.0f};
};

SecondaryTv secondaryOf(const PlannedBuilding& building) {
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::RoomId room = rv::world::RoomId::parse(kRoom).value();
    auto data = rv::world::loadSceneData(buildAssets(), stored.plan, room);
    REQUIRE(data.has_value());
    auto tvs = rv::world::secondaryTvFixtures(stored.plan, room, *data);
    REQUIRE(tvs.has_value());
    INFO("the snes room's secondary TV (its pack's sideboard sockets)");
    REQUIRE(tvs->size() == 1);
    auto scene = rv::world::toSceneDescription(stored.plan, room, *data);
    REQUIRE(scene.has_value());
    const std::string id = tvs->front().localText();
    const auto object = std::find_if(scene->objects.begin(), scene->objects.end(),
                                     [&](const rv::scene::SceneObject& candidate) { return candidate.id == id; });
    REQUIRE(object != scene->objects.end());
    CHECK(object->kind == "secondary_tv");
    auto prefab = rv::geometry::buildPrefab(object->kind, object->params);
    REQUIRE(prefab.has_value());
    const rv::geometry::Socket* surface = rv::geometry::findSocket(prefab->sockets, "ScreenSurface");
    REQUIRE(surface != nullptr);
    SecondaryTv tv;
    tv.screen = std::string(kRoom) + "/" + id;
    tv.screenPoint = glm::vec3(object->instanceTransforms().front() * surface->transform *
                               glm::vec4(surface->bounds.center(), 1.0f));
    tv.boundsCentre = glm::vec3(object->instanceTransforms().front() * glm::vec4(prefab->bounds.center(), 1.0f));
    const rv::world::Room& planned = roomIn(stored.plan, kRoom);
    const rv::world::DoorwayCamera doorway = rv::world::doorwayCamera(planned, data->roomTemplate);
    tv.doorwayEye = glm::vec3(static_cast<float>(doorway.x), kEyeHeight, static_cast<float>(doorway.z));
    return tv;
}

/// The walker at the doorway's eye, turned towards the secondary's screen (it stays in view as the walk nears it).
rv::app::SpawnPoint towards(const SecondaryTv& tv) {
    const glm::vec3 look = tv.screenPoint - tv.doorwayEye;
    rv::app::SpawnPoint spawn;
    spawn.feet = glm::vec3(tv.doorwayEye.x, 0.0f, tv.doorwayEye.z);
    spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    spawn.pitchDeg = rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    return spawn;
}

/// Where the eye stands after walking `frames` frames from the doorway towards the secondary's screen (2.5 m/s, along
/// the floor).
glm::vec3 eyeAfter(const SecondaryTv& tv, int frames) {
    glm::vec3 along = tv.screenPoint - tv.doorwayEye;
    along.y = 0.0f;
    along = glm::normalize(along);
    return tv.doorwayEye + along * static_cast<float>(kWalkSpeed * kStep * frames);
}

void writeVideoSettings(const PlannedBuilding& building, const Json& settings) {
    const fs::path config = building.data / "config";
    fs::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", settings.dump(2) + "\n").has_value());
}

/// A lockstep run of `frames` frames from the doorway towards the secondary, walking from frame 0.
AppRun walkTowards(const PlannedBuilding& building, std::string_view name, const SecondaryTv& tv,
                   std::vector<std::string> extra) {
    const fs::path script = building.dir / "walk.json";
    const Json steps = Json::array({Json{{"fromFrame", 0}, {"toFrame", kWalkFrames - 1}, {"actions", {{"move_forward", 1.0}}}}});
    REQUIRE(rv::core::writeTextFileAtomic(script, steps.dump(2) + "\n").has_value());
    std::vector<std::string> arguments{"--start-room", std::string(kRoom),       "--exit-after-frames", "--frames",
                                       std::to_string(kWalkFrames), "--fixed-dt", "0.016667",   "--spawn",
                                       spawnArgument(towards(tv)), "--input-script", arg(script), "--screenshot",
                                       arg(building.dir / (std::string(name) + ".png"))};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), building.dir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    CHECK(run.report->world.room == kRoom);
    REQUIRE(run.report->video.bound);
    CHECK(run.report->video.screen == kHeroScreen);
    return run;
}

std::vector<rv::app::VideoSlotEventRecord> eventsOf(const rv::app::VideoRecord& video, std::string_view action,
                                                    std::string_view screen) {
    std::vector<rv::app::VideoSlotEventRecord> events;
    std::copy_if(video.slotEvents.begin(), video.slotEvents.end(), std::back_inserter(events), [&](const auto& event) {
        return event.action == action &&
               std::find(event.screens.begin(), event.screens.end(), screen) != event.screens.end();
    });
    return events;
}

/// The slot stops of `screen`'s clips but the run's end ("shutdown").
std::size_t stopsBeforeExit(const rv::app::VideoRecord& video, std::string_view screen) {
    const auto stops = eventsOf(video, "stop", screen);
    return static_cast<std::size_t>(
        std::count_if(stops.begin(), stops.end(), [](const auto& event) { return event.reason != "shutdown"; }));
}

const rv::app::VideoScreenRecord& screenOf(const rv::app::VideoRecord& video, std::string_view screen) {
    const auto found = std::find_if(video.screens.begin(), video.screens.end(),
                                    [&](const rv::app::VideoScreenRecord& record) { return record.screen == screen; });
    INFO("the report's screen " << screen);
    REQUIRE(found != video.screens.end());
    return *found;
}

} // namespace

TEST_CASE("gpu.secondary_tv: walking from the doorway, the secondary takes the second slot within its proximity and in "
          "view while the hero keeps the first",
          "[gpu][secondary_tv]") {
    PlannedBuilding building("secondary_tv/proximity", true, "esde/snes-room");
    const SecondaryTv tv = secondaryOf(building);
    const double start = glm::distance(tv.doorwayEye, tv.boundsCentre);
    INFO("the secondary " << tv.screen << " " << start << " m from the doorway");
    REQUIRE(start > kShortOfProximity + 2.0);
    // The proximity ends kShortOfProximity short of the doorway's distance (the eye's to the TV's bounds' middle).
    const double proximity = start - kShortOfProximity;
    writeVideoSettings(building, Json{{"proximityMeters", proximity}});
    const AppRun run = walkTowards(building, "walk", tv, {});
    const rv::app::VideoRecord& video = run.report->video;
    CHECK(video.maxDecoders == 2);
    CHECK(video.maxOpenDecoders == 2);
    CHECK(video.maxDecoderThreads <= 2);
    CHECK(video.joinedAtExit);

    // The hero keeps the first slot from the first frame: opened on frame 0, never stopped.
    const auto heroOpens = eventsOf(video, "open", kHeroScreen);
    REQUIRE(heroOpens.size() == 1);
    CHECK(heroOpens.front().frame <= 1);
    CHECK(heroOpens.front().slot == 0);
    CHECK(stopsBeforeExit(video, kHeroScreen) == 0);

    // The secondary opens its clip on the second slot once the walker is within its proximity, never before; the
    // walk's distance after `frame` frames (2.5 m/s from the first frame's update) crosses it on `crossing`.
    const auto opens = eventsOf(video, "open", tv.screen);
    REQUIRE(opens.size() == 1);
    CHECK(opens.front().slot == 1);
    CHECK(opens.front().gameId != heroOpens.front().gameId);
    int crossing = 0;
    while (crossing < kWalkFrames && glm::distance(eyeAfter(tv, crossing), tv.boundsCentre) > proximity) {
        ++crossing;
    }
    INFO("the walk comes within " << proximity << " m on about frame " << crossing);
    REQUIRE(crossing < kWalkFrames - 10);
    CHECK(opens.front().frame + 1 >= static_cast<std::uint64_t>(crossing));
    CHECK(opens.front().frame <= static_cast<std::uint64_t>(crossing) + 2);
    // At the end: both play, the hero on slot 0, the secondary (ambient, in view) on slot 1, each its own texture.
    const rv::app::VideoScreenRecord& hero = screenOf(video, kHeroScreen);
    const rv::app::VideoScreenRecord& secondary = screenOf(video, tv.screen);
    CHECK(hero.kind == "hero");
    CHECK(hero.slot == std::optional<std::uint64_t>(0));
    CHECK(hero.picture == "video");
    CHECK(secondary.kind == "secondary");
    CHECK(secondary.screenClass == "ambient");
    CHECK(secondary.visible);
    CHECK(secondary.slot == std::optional<std::uint64_t>(1));
    CHECK(secondary.picture == "video");
    CHECK(secondary.playing);
    CHECK(secondary.textureId != hero.textureId);
    REQUIRE(run.report->finalCamera.has_value());
    CHECK(glm::distance(glm::vec3(run.report->finalCamera->feet.x, kEyeHeight, run.report->finalCamera->feet.z),
                        tv.boundsCentre) < proximity);
}

TEST_CASE("gpu.secondary_tv: with one decoder the hero keeps the slot and the secondary shows its still",
          "[gpu][secondary_tv]") {
    PlannedBuilding building("secondary_tv/one_decoder", true, "esde/snes-room");
    const SecondaryTv tv = secondaryOf(building);
    const AppRun run = walkTowards(building, "walk", tv, {"--max-decoders", "1"});
    const rv::app::VideoRecord& video = run.report->video;
    CHECK(video.maxDecoders == 1);
    CHECK(video.maxOpenDecoders == 1);
    CHECK(video.maxDecoderThreads <= 1);
    CHECK(video.joinedAtExit);
    const auto heroOpens = eventsOf(video, "open", kHeroScreen);
    REQUIRE(heroOpens.size() == 1);
    CHECK(heroOpens.front().frame <= 1);
    CHECK(stopsBeforeExit(video, kHeroScreen) == 0);
    // The secondary never opens: the walk ends within its proximity (12 m: the whole room) and in view, its still up.
    CHECK(eventsOf(video, "open", tv.screen).empty());
    const rv::app::VideoScreenRecord& secondary = screenOf(video, tv.screen);
    CHECK(secondary.kind == "secondary");
    CHECK(secondary.screenClass == "ambient");
    CHECK(secondary.visible);
    CHECK_FALSE(secondary.slot.has_value());
    CHECK_FALSE(secondary.playing);
    CHECK((secondary.picture == "screenshot" || secondary.picture == "title-screen" || secondary.picture == "cover" ||
           secondary.picture == "title-card"));
    CHECK(secondary.textureId != 0);
    const rv::app::VideoScreenRecord& hero = screenOf(video, kHeroScreen);
    CHECK(hero.slot == std::optional<std::uint64_t>(0));
    CHECK(hero.picture == "video");
}
