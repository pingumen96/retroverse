// The rooms' secondary TVs playing (I5 step 3, ADR-0016 §2 and §5): the snes-room fixture
// (tests/fixtures/esde/snes-room: invented titles, generated clips with their frame number burned in) recorded as the
// library of the case's data directory, imported and planned with retroverse-cli, then retroverse.exe --start-room
// room:snes, lockstep (--fixed-dt), hidden. The room's pack (classic_16bit_warm) stands its secondary TV in one of its
// sideboards' sockets (world::secondaryTvFixtures()); the run reports tell what the screens did (the video section's
// screens and slots' events):
//  - walking from the doorway towards the secondary TV with video.json's proximityMeters short of it, the hero keeps
//    the first slot from the room's first update while the secondary (`ambient`) has none and shows its still, then
//    takes the second slot on the frame the walker comes within its proximity, in view;
//  - with one decoder (--max-decoders 1) and both screens in view the hero keeps the slot and the secondary shows its
//    still;
//  - I5 step 3's review, on the product defaults (proximityMeters 12): the TV behind the walker on entering opens no
//    slot (one decoder), turning towards it opens the second;
//  - I5 step 3's review: the room editor picks the TV up, nudges and turns it and drops it (the room's static part
//    rebuilt alone): the TV plays nothing while the editor runs, its screen moved onto its new object in place
//    (ScreenService::rebind(): no bind, no event on the hero's slot), its bounds where it stands now; after Salva it
//    plays again on the second slot.

#include "building_support.hpp"
#include "smoke_support.hpp"

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_edit.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    rv::world::FixtureId fixture;
    std::string screen;
    glm::vec3 screenPoint{0.0f};
    /// The middle of its object's bounds: the screen service's distance to it (ScreenService::update()).
    glm::vec3 boundsCentre{0.0f};
    glm::vec3 doorwayEye{0.0f};
    /// The middle of the hero CRT's screen.
    glm::vec3 heroPoint{0.0f};
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
    tv.fixture = tvs->front();
    tv.screen = std::string(kRoom) + "/" + id;
    tv.screenPoint = glm::vec3(object->instanceTransforms().front() * surface->transform *
                               glm::vec4(surface->bounds.center(), 1.0f));
    tv.boundsCentre = glm::vec3(object->instanceTransforms().front() * glm::vec4(prefab->bounds.center(), 1.0f));
    const rv::scene::SceneObject* hero = scene->findObject("fx:hero.crt");
    REQUIRE(hero != nullptr);
    auto heroPrefab = rv::geometry::buildPrefab(hero->kind, hero->params);
    REQUIRE(heroPrefab.has_value());
    const rv::geometry::Socket* heroSurface = rv::geometry::findSocket(heroPrefab->sockets, "ScreenSurface");
    REQUIRE(heroSurface != nullptr);
    tv.heroPoint = glm::vec3(hero->instanceTransforms().front() * heroSurface->transform *
                             glm::vec4(heroSurface->bounds.center(), 1.0f));
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
    const Json steps =
        Json::array({Json{{"fromFrame", 0}, {"toFrame", kWalkFrames - 1}, {"actions", {{"move_forward", 1.0}}}}});
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

namespace {

/// The walker in front of the middle between two screens, both in view (inside the room, at most 55 degrees apart).
rv::app::SpawnPoint bothInView(const rv::world::Room& room, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec2 middle(0.5f * (a.x + b.x), 0.5f * (a.z + b.z));
    const glm::vec2 along(b.x - a.x, b.z - a.z);
    glm::vec2 normal = glm::normalize(glm::vec2(-along.y, along.x));
    const glm::vec2 centre(0.5f * static_cast<float>(room.minXMm + room.maxXMm) / 1000.0f,
                           0.5f * static_cast<float>(room.minZMm + room.maxZMm) / 1000.0f);
    if (glm::dot(centre - middle, normal) < 0.0f) {
        normal = -normal;
    }
    const float distance = 0.5f * glm::length(along) / std::tan(glm::radians(27.5f));
    glm::vec2 eye = middle + distance * normal;
    eye.x = std::clamp(eye.x, static_cast<float>(room.minXMm) / 1000.0f + 0.8f,
                       static_cast<float>(room.maxXMm) / 1000.0f - 0.8f);
    eye.y = std::clamp(eye.y, static_cast<float>(room.minZMm) / 1000.0f + 0.8f,
                       static_cast<float>(room.maxZMm) / 1000.0f - 0.8f);
    const glm::vec3 look(middle.x - eye.x, 0.5f * (a.y + b.y) - kEyeHeight, middle.y - eye.y);
    rv::app::SpawnPoint spawn;
    spawn.feet = glm::vec3(eye.x, 0.0f, eye.y);
    spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    spawn.pitchDeg = rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    return spawn;
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
    // Both screens in view (an ambient screen in view ranks above a hero out of view: ADR-0016 §2).
    PlannedBuilding building("secondary_tv/one_decoder", true, "esde/snes-room");
    const SecondaryTv tv = secondaryOf(building);
    const AppRun run = runApp(
        retroverseExe(), building.dir, "both",
        {"--start-room", std::string(kRoom), "--exit-after-frames", "--frames", std::to_string(kWalkFrames),
         "--fixed-dt", "0.016667", "--spawn",
         spawnArgument(bothInView(roomIn(building.plan().plan, kRoom), tv.heroPoint, tv.screenPoint)),
         "--max-decoders", "1", "--screenshot", arg(building.dir / "both.png")});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->video.bound);
    const rv::app::VideoRecord& video = run.report->video;
    CHECK(video.maxDecoders == 1);
    CHECK(video.maxOpenDecoders == 1);
    CHECK(video.maxDecoderThreads <= 1);
    CHECK(video.joinedAtExit);
    const auto heroOpens = eventsOf(video, "open", kHeroScreen);
    REQUIRE(heroOpens.size() == 1);
    CHECK(heroOpens.front().frame <= 1);
    CHECK(stopsBeforeExit(video, kHeroScreen) == 0);
    // The secondary never opens: within its proximity (12 m: the whole room) and in view, its still up.
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

namespace {

/// The mouse's look per pixel (scene::FirstPersonSettings').
constexpr float kRadiansPerPixel = 0.0022f;

/// `report`'s screen record of `screen`, if bound.
const rv::app::VideoScreenRecord* findScreen(const rv::app::VideoRecord& video, std::string_view screen) {
    const auto found = std::find_if(video.screens.begin(), video.screens.end(),
                                    [&](const rv::app::VideoScreenRecord& record) { return record.screen == screen; });
    return found != video.screens.end() ? &*found : nullptr;
}

} // namespace

TEST_CASE("gpu.secondary_tv: on the product defaults, the TV behind the walker on entering opens no slot; turning "
          "towards it opens the second",
          "[gpu][secondary_tv]") {
    // I5 step 3's review (ADR-0016 §5 amended): an ambient screen opens a slot within proximityMeters and in view.
    PlannedBuilding building("secondary_tv/behind", true, "esde/snes-room");
    const SecondaryTv tv = secondaryOf(building);
    const double distance = glm::distance(tv.doorwayEye, tv.boundsCentre);
    INFO("the secondary " << tv.screen << " " << distance << " m from the doorway");
    REQUIRE(distance < 12.0); // within the product's proximityMeters from the start
    // At the doorway, the TV right behind the walker; at frame kTurnFrame half a turn towards it.
    constexpr int kTurnFrame = 90;
    constexpr int kFrames = 150;
    const glm::vec3 away = tv.doorwayEye - tv.screenPoint;
    rv::app::SpawnPoint spawn;
    spawn.feet = glm::vec3(tv.doorwayEye.x, 0.0f, tv.doorwayEye.z);
    spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(away.x, -away.z));
    spawn.pitchDeg = 0.0f;
    const fs::path script = building.dir / "turn.json";
    const Json steps = Json::array({Json{{"fromFrame", kTurnFrame},
                                         {"toFrame", kTurnFrame},
                                         {"actions", {{"look_delta_x", std::numbers::pi / kRadiansPerPixel}}}}});
    REQUIRE(rv::core::writeTextFileAtomic(script, steps.dump(2) + "\n").has_value());
    const AppRun run = runApp(retroverseExe(), building.dir, "turn",
                              {"--start-room", std::string(kRoom), "--exit-after-frames", "--frames",
                               std::to_string(kFrames), "--fixed-dt", "0.016667", "--spawn", spawnArgument(spawn),
                               "--input-script", arg(script), "--screenshot", arg(building.dir / "turn.png")});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.screen == kHeroScreen);
    CHECK(video.maxDecoders == 2);
    // The hero from the first frame; the secondary not before the turn, then on the second slot.
    const auto heroOpens = eventsOf(video, "open", kHeroScreen);
    REQUIRE(heroOpens.size() == 1);
    CHECK(heroOpens.front().frame <= 1);
    CHECK(heroOpens.front().slot == 0);
    CHECK(stopsBeforeExit(video, kHeroScreen) == 0);
    const auto opens = eventsOf(video, "open", tv.screen);
    REQUIRE(opens.size() == 1);
    CHECK(opens.front().frame >= static_cast<std::uint64_t>(kTurnFrame));
    CHECK(opens.front().frame <= static_cast<std::uint64_t>(kTurnFrame) + 3);
    CHECK(opens.front().slot == 1);
    // Until the turn one decoder was open: no event of the secondary's, no second slot.
    for (const rv::app::VideoSlotEventRecord& event : video.slotEvents) {
        if (event.frame < static_cast<std::uint64_t>(kTurnFrame)) {
            CHECK(event.slot == 0);
        }
    }
    const rv::app::VideoScreenRecord* secondary = findScreen(video, tv.screen);
    REQUIRE(secondary != nullptr);
    CHECK(secondary->screenClass == "ambient");
    CHECK(secondary->visible);
    CHECK(secondary->slot == std::optional<std::uint64_t>(1));
    CHECK(secondary->picture == "video");
    CHECK(video.maxOpenDecoders == 2);
}

namespace {

/// What an edit of the floor of `room` reads (app::prepareEditData()'s): the configuration, the building, every room
/// of the floor, from the data directory `data`.
rv::world::EditData editDataOf(const fs::path& data, const rv::world::WorldPlan& plan, const rv::world::RoomId& room) {
    auto config = rv::world::loadWorldConfig(buildAssets(), data / "config");
    REQUIRE(config.has_value());
    auto building = rv::world::loadBuildingData(buildAssets());
    REQUIRE(building.has_value());
    rv::world::EditData edit;
    edit.config = config->config;
    edit.building = std::move(*building);
    std::string floor;
    for (const rv::world::Room& candidate : plan.rooms) {
        if (candidate.id == room) {
            floor = candidate.floor;
        }
    }
    for (const rv::world::Room& candidate : plan.rooms) {
        if (candidate.floor == floor) {
            auto loaded = rv::world::loadRoomData(buildAssets(), candidate);
            REQUIRE(loaded.has_value());
            edit.rooms.emplace(candidate.id, std::make_shared<const rv::world::WorldData>(std::move(*loaded)));
        }
    }
    return edit;
}

/// The editor's session on the TV: where the walker stands (in front of it, looking at its CRT), the nudges (a
/// direction, a count) and the turn ("edit_rotate_left": +15 degrees, or right), and where it drops.
struct TvSession {
    rv::app::SpawnPoint spawn;
    std::string nudge;
    int steps = 0;
    std::string turn;
    rv::world::FixtureTransform home;
    rv::world::FixtureTransform place;
};

TvSession planTvSession(const PlannedBuilding& building, const rv::world::WorldPlan& plan, const SecondaryTv& tv) {
    const rv::world::RoomId room = rv::world::RoomId::parse(kRoom).value();
    const auto fixture = std::find_if(plan.fixtures.begin(), plan.fixtures.end(),
                                      [&](const rv::world::Fixture& candidate) { return candidate.id == tv.fixture; });
    REQUIRE(fixture != plan.fixtures.end());
    TvSession session;
    session.home = rv::world::FixtureTransform{fixture->xMm, fixture->zMm, fixture->yawCdeg};
    // In front of its box, 2.3 m out along the way it faces (yaw 0 faces +Z, 9000 +X).
    const rv::world::FixtureBox box = rv::world::fixtureBox(*fixture);
    // Aimed at the CRT's upper half, over the planned sideboard's box (0.72 m: the TV stands 0.9 m tall), which the
    // editor's pick box covers too (I5 step 3's review).
    const glm::vec3 middle(static_cast<float>(box.minXMm + box.maxXMm) / 2000.0f, 0.82f,
                           static_cast<float>(box.minZMm + box.maxZMm) / 2000.0f);
    REQUIRE(static_cast<float>(box.maxYMm) / 1000.0f < 0.8f);
    const float yaw = glm::radians(static_cast<float>(fixture->yawCdeg) / 100.0f);
    const glm::vec3 facing(std::sin(yaw), 0.0f, std::cos(yaw));
    const glm::vec3 eye = glm::vec3(middle.x, kEyeHeight, middle.z) + facing * 2.3f;
    const glm::vec3 look = middle - eye;
    session.spawn.feet = glm::vec3(eye.x, 0.0f, eye.z);
    session.spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    session.spawn.pitchDeg =
        rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    // The nudges seen along the view's axis (the editor's, test_world_edit_furniture.cpp's nudgeStep()), snapped, then
    // the turn: the first combination the edit accepts.
    const rv::world::EditData edit = editDataOf(building.data, plan, room);
    const rv::world::EditedWorld start{plan, {}};
    const glm::ivec2 forward = std::abs(look.x) > std::abs(look.z) ? glm::ivec2(look.x > 0.0f ? 1 : -1, 0)
                                                                    : glm::ivec2(0, look.z > 0.0f ? 1 : -1);
    const glm::ivec2 right(-forward.y, forward.x);
    const rv::world::PinProvenance provenance{rv::world::PinOrigin::Editor, "2026-10-29T12:00:00.000Z"};
    for (const auto& [nudge, step] : {std::pair{"ui_right", right}, std::pair{"ui_left", -right},
                                      std::pair{"ui_down", -forward}}) {
        for (const int steps : {2, 3, 1}) {
            for (const auto& [turn, by] :
                 {std::pair{"edit_rotate_left", 1500}, std::pair{"edit_rotate_right", -1500}}) {
                rv::world::FixtureTransform place = session.home;
                for (int index = 0; index < steps; ++index) {
                    const auto snapped = rv::world::snapFixture(
                        start, tv.fixture,
                        rv::world::FixtureTransform{place.xMm + step.x * rv::world::kEditGridMm,
                                                    place.zMm + step.y * rv::world::kEditGridMm, place.yawCdeg},
                        edit);
                    REQUIRE(snapped.has_value());
                    place = *snapped;
                }
                const auto turned = rv::world::snapFixture(
                    start, tv.fixture,
                    rv::world::FixtureTransform{place.xMm, place.zMm, ((place.yawCdeg + by) % 36000 + 36000) % 36000},
                    edit);
                REQUIRE(turned.has_value());
                place = *turned;
                if (place.xMm == session.home.xMm && place.zMm == session.home.zMm) {
                    continue;
                }
                rv::world::EditedWorld trial = start;
                auto result = rv::world::applyEdit(
                    trial, rv::world::EditOperation::moveFixture(tv.fixture, place.xMm, place.zMm, place.yawCdeg),
                    edit, provenance);
                REQUIRE(result.has_value());
                if (!result->refused) {
                    session.nudge = nudge;
                    session.steps = steps;
                    session.turn = turn;
                    session.place = place;
                    return session;
                }
            }
        }
    }
    FAIL("no nudge and turn of the secondary TV " << tv.fixture.toString() << " that the edit accepts");
    return session;
}

} // namespace

TEST_CASE("gpu.secondary_tv: the room editor moves and turns the TV: it plays nothing meanwhile, its screen rebound "
          "in place, and plays again after Salva",
          "[gpu][secondary_tv]") {
    PlannedBuilding building("secondary_tv/editor", true, "esde/snes-room");
    const SecondaryTv tv = secondaryOf(building);
    const rv::world::StoredPlan before = building.plan();
    const TvSession session = planTvSession(building, before.plan, tv);
    INFO("the TV " << tv.fixture.toString() << " " << session.steps << " x " << session.nudge << ", " << session.turn
                   << ": from " << session.home.xMm << ", " << session.home.zMm << " at " << session.home.yawCdeg
                   << " to " << session.place.xMm << ", " << session.place.zMm << " at " << session.place.yawCdeg);
    // The directory's "Modifica stanza" (test_world_edit.cpp's frames), the TV picked up, nudged, turned and dropped
    // (the room's static part rebuilt alone), the menu, Salva; then the room as saved.
    std::vector<Json> steps;
    const auto at = [&](int frame, Json actions) {
        steps.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", std::move(actions)}});
    };
    at(30, {{"map", 1}});
    at(36, {{"ui_home", 1}});
    at(42, {{"ui_up", 1}});
    at(48, {{"ui_activate", 1}});
    int frame = 70;
    at(frame, {{"interact", 1}});
    for (int step = 0; step < session.steps; ++step) {
        frame += 10;
        at(frame, {{session.nudge, 1}});
    }
    frame += 10;
    at(frame, {{session.turn, 1}});
    frame += 10;
    const int drop = frame;
    at(drop, {{"interact", 1}});
    frame += 30;
    at(frame, {{"confirm", 1}});
    frame += 20;
    const int save = frame;
    at(save, {{"ui_activate", 1}});
    const int frames = save + 150;
    const fs::path script = building.dir / "edit_tv.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, Json(steps).dump(2) + "\n").has_value());
    const fs::path shot = building.dir / "edited.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "edit_tv",
                              {"--start-room", std::string(kRoom), "--exit-after-frames", "--frames",
                               std::to_string(frames), "--fixed-dt", "0.016667", "--spawn",
                               spawnArgument(session.spawn), "--input-script", arg(script), "--screenshot",
                               arg(shot)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());

    // The session: the TV's one move, shown by its room's static part rebuilt alone, saved.
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.end == "saved");
    REQUIRE(edit.readyFrame.has_value());
    REQUIRE(edit.endFrame.has_value());
    REQUIRE(edit.operations.size() == 1);
    CHECK(edit.operations.front().op == "move-fixture");
    CHECK(edit.operations.front().refused.empty());
    CHECK(edit.operations.front().path == "room");
    const rv::world::StoredPlan after = building.plan();
    const auto moved = std::find_if(after.plan.fixtures.begin(), after.plan.fixtures.end(),
                                    [&](const rv::world::Fixture& candidate) { return candidate.id == tv.fixture; });
    REQUIRE(moved != after.plan.fixtures.end());
    CHECK(moved->xMm == session.place.xMm);
    CHECK(moved->zMm == session.place.zMm);
    CHECK(moved->yawCdeg == session.place.yawCdeg);

    // The screens: moved onto their new objects in place (no bind after the first), never a bind's stop or open.
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.screen == kHeroScreen);
    CHECK(video.binds.empty());
    CHECK(video.rebinds >= 1);
    const auto heroOpens = eventsOf(video, "open", kHeroScreen);
    REQUIRE(heroOpens.size() == 1);
    CHECK(heroOpens.front().frame <= 1);
    CHECK(stopsBeforeExit(video, kHeroScreen) == 0);
    // The TV: playing in view from the start (on the first slot: in view, it ranks before the hero behind the
    // walker), stopped once the editor runs ("unwanted": it asks for no clip), no event of its slot while the session
    // runs (the drop's rebuild rebinds it in place), and open again after it ended.
    const auto opens = eventsOf(video, "open", tv.screen);
    // (After the session its playlist goes on: a short clip ends, the next opens on the same slot.)
    REQUIRE(opens.size() >= 2);
    CHECK(opens[0].frame < 30);
    const std::uint64_t tvSlot = opens[0].slot;
    // Its slot's stop (a stop lists the screens still showing the clip: none, the TV having let it go).
    const auto stop = std::find_if(video.slotEvents.begin(), video.slotEvents.end(), [&](const auto& event) {
        return event.slot == tvSlot && event.action == "stop" && event.frame > opens[0].frame;
    });
    REQUIRE(stop != video.slotEvents.end());
    CHECK(stop->reason == "unwanted");
    CHECK(stop->gameId == opens[0].gameId);
    CHECK(stop->frame >= edit.enterFrame);
    CHECK(stop->frame <= *edit.readyFrame + 2);
    for (const rv::app::VideoSlotEventRecord& event : video.slotEvents) {
        if (event.slot == tvSlot && event.frame > stop->frame && event.frame < *edit.endFrame) {
            FAIL_CHECK("an event on the TV's slot while the editor ran: frame " << event.frame << " " << event.action
                                                                                << " (" << event.reason << ")");
        }
    }
    CHECK(opens[1].frame >= *edit.endFrame);
    CHECK(opens[1].frame <= *edit.endFrame + 4);
    CHECK(opens[1].slot == tvSlot);
    CHECK(static_cast<int>(*edit.endFrame) >= save);
    // At the end it plays where it stands now: its bounds moved by the drop (and turned with it).
    const rv::app::VideoScreenRecord* secondary = findScreen(video, tv.screen);
    REQUIRE(secondary != nullptr);
    CHECK(secondary->picture == "video");
    CHECK(secondary->playing);
    CHECK(secondary->visible);
    REQUIRE(secondary->bounds.size() == 6u);
    const glm::vec3 centre(static_cast<float>(0.5 * (secondary->bounds[0] + secondary->bounds[3])),
                           static_cast<float>(0.5 * (secondary->bounds[1] + secondary->bounds[4])),
                           static_cast<float>(0.5 * (secondary->bounds[2] + secondary->bounds[5])));
    const glm::vec2 shift(static_cast<float>(session.place.xMm - session.home.xMm) / 1000.0f,
                          static_cast<float>(session.place.zMm - session.home.zMm) / 1000.0f);
    INFO("the TV's bounds' middle " << centre.x << ", " << centre.z << ", at home " << tv.boundsCentre.x << ", "
                                    << tv.boundsCentre.z);
    CHECK(glm::distance(glm::vec2(centre.x, centre.z), glm::vec2(tv.boundsCentre.x, tv.boundsCentre.z)) >
          0.5f * glm::length(shift));
    CHECK(glm::distance(glm::vec2(centre.x, centre.z), glm::vec2(tv.boundsCentre.x, tv.boundsCentre.z) + shift) < 0.2f);
    static_cast<void>(requireScreenshot(run, shot, {1920, 1080}));
}

TEST_CASE("gpu.secondary_tv: screenshots for a look by hand", "[.secondary_tv_shots]") {
    // build/i5/step3/ under the source tree: the console room with the hero and its secondary TV playing (the secondary
    // on one sound clip through video.json's per-screen keys), and the same view with one decoder (its still).
    PlannedBuilding building("secondary_tv/shots", true, "esde/snes-room");
    const SecondaryTv tv = secondaryOf(building);
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::RoomId room = rv::world::RoomId::parse(kRoom).value();
    auto data = rv::world::loadSceneData(buildAssets(), stored.plan, room);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored.plan, room, *data);
    REQUIRE(scene.has_value());
    const rv::scene::SceneObject* hero = scene->findObject("fx:hero.crt");
    REQUIRE(hero != nullptr);
    auto heroPrefab = rv::geometry::buildPrefab(hero->kind, hero->params);
    REQUIRE(heroPrefab.has_value());
    const rv::geometry::Socket* surface = rv::geometry::findSocket(heroPrefab->sockets, "ScreenSurface");
    REQUIRE(surface != nullptr);
    const glm::vec3 heroPoint(hero->instanceTransforms().front() * surface->transform *
                              glm::vec4(surface->bounds.center(), 1.0f));
    const AppRun search = building.cli("search", {"search", "Dappled Dovecote", "--json"});
    auto found = rv::core::readTextFile(search.outputFile);
    REQUIRE(found.has_value());
    const std::string game = Json::parse(*found).at("games").front().at("id").get<std::string>();
    const std::string screen = tv.screen; // "room:snes/fx:..."
    writeVideoSettings(building,
                       Json{{"rooms", {{"room:snes", {{"screens", {{screen, {{"playlist", "explicit"},
                                                                              {"games", Json::array({game})}}}}}}}}}});
    const fs::path out = sourceDir() / "build" / "i5" / "step3";
    fs::create_directories(out);
    const std::string spawn = spawnArgument(bothInView(roomIn(stored.plan, kRoom), heroPoint, tv.screenPoint));
    for (const auto& [name, extra] : std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"console_room_hero_and_secondary", {}}, {"secondary_still_one_decoder", {"--max-decoders", "1"}}}) {
        std::vector<std::string> arguments{"--start-room", std::string(kRoom), "--exit-after-frames",
                                           "--frames", "150", "--fixed-dt", "0.016667", "--spawn", spawn,
                                           "--screenshot", arg(out / (name + ".png")), "--screenshot-size",
                                           "1920x1080"};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        const AppRun run = runApp(retroverseExe(), building.dir, name, arguments);
        skipWithoutOpenGl(run);
        requireCleanRun(run);
    }
}
