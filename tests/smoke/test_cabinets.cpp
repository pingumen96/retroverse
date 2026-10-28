// The arcade cabinets playing their clips (I5 step 4, ADR-0016 §6): the building fixture (tests/fixtures/esde/building)
// recorded as the library of the case's data directory, every placed mame title given a generated clip
// (media::writeTestVideo: a moving pattern, its frame number burned in; never committed, never a real clip) before its
// import, retroverse.exe --start-room area:arcade, lockstep (--fixed-dt), hidden. The run reports tell what the screens
// did (the video section's cabinetEvents, screens and slots' events); F12 screenshots (<data>/screenshots) show it:
//  - the inspected cabinet plays its clip in the hand (`direct`), in Focus after Indietro (`focus`), and shows its cover
//    again on Back; the hero cabinet never follows the selection;
//  - attract plays on the nearest cabinet in view within 3 m facing the eye after its 1 s dwell, and stops when it
//    leaves the view;
//  - with one decoder the inspected cabinet takes the hero cabinet's slot, and the hero keeps its paused frame;
//  - no cabinet plays while the room editor runs (I4b's session: its fixtures move, are held and rebuilt), and attract
//    comes back after it.

#include "building_support.hpp"
#include "image_checks.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

#include <rv/app/cabinet_screens.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/media/test_video.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The window's size: Focus poses are framed for its aspect.
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};
/// The walker's eye above its feet (scene::FirstPersonController's default), the mouse's look per pixel.
constexpr float kEyeHeight = 1.65f;
constexpr float kRadiansPerPixel = 0.0022f;
constexpr std::string_view kRoom = "area:arcade";
/// The hero cabinet's screen (I6 step 6): it keeps the room's playlist.
constexpr std::string_view kHeroScreen = "area:arcade/fx:hero.crt";
/// A frame of the run's 60 a second: the attract dwell (1 s) is 60 of them.
constexpr int kDwellFrames = 60;

/// The clips the cabinets play: every mame title with a cover (the placed ones) gets one at media/mame/videos (ES-DE's
/// folder: the import finds it), 4 s at 30 fps; the second in name order a portrait 3:4 clip (a vertical monitor's,
/// which fills a cabinet's screen), the third a 16:9 one, the others 4:3 (letterboxed on the portrait screens).
void writeArcadeClips(const rv::test::Fixture& fixture) {
    std::vector<fs::path> covers;
    for (const fs::directory_entry& entry : fs::directory_iterator(fixture.path("media/mame/covers"))) {
        covers.push_back(entry.path());
    }
    std::sort(covers.begin(), covers.end());
    fs::create_directories(fixture.path("media/mame/videos"));
    for (std::size_t index = 0; index < covers.size(); ++index) {
        rv::media::TestVideoSpec spec;
        spec.width = index == 1 ? 360 : index == 2 ? 854 : 640;
        spec.height = 480;
        spec.frameRate = 30;
        spec.durationSeconds = 4.0;
        spec.seed = 400u + static_cast<std::uint32_t>(index);
        spec.quantizer = 5;
        spec.keyFrameInterval = 30;
        const fs::path clip =
            fixture.path("media/mame/videos") / rv::core::pathFromUtf8(rv::core::pathToUtf8(covers[index].stem()) + ".mp4");
        INFO("clip " << arg(clip));
        REQUIRE(rv::media::writeTestVideo(clip, spec).has_value());
    }
}

/// The building of a case under cabinets/, its mame titles with clips.
struct ArcadeBuilding : PlannedBuilding {
    explicit ArcadeBuilding(std::string_view name)
        : PlannedBuilding(std::string("cabinets/") + std::string(name), true, "esde/building", writeArcadeClips) {}
};

/// video.json in the building's data directory.
void writeVideoSettings(const PlannedBuilding& building, const Json& settings) {
    const fs::path config = building.data / "config";
    fs::create_directories(config);
    REQUIRE(rv::core::writeTextFileAtomic(config / "video.json", settings.dump(2) + "\n").has_value());
}

Json step(int from, int to, Json actions) {
    return Json{{"fromFrame", from}, {"toFrame", to}, {"actions", std::move(actions)}};
}

fs::path writeScript(const PlannedBuilding& building, std::string_view name, Json steps) {
    const fs::path file = building.dir / rv::core::pathFromUtf8(name);
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

/// A lockstep run in the arcade room: `frames` frames, a screenshot at the end, `extra` arguments.
AppRun runArcade(const PlannedBuilding& building, std::string_view name, int frames, std::vector<std::string> extra) {
    std::vector<std::string> arguments{"--start-room",    std::string(kRoom), "--exit-after-frames",
                                       "--frames",        std::to_string(frames), "--fixed-dt",
                                       "0.016667",        "--screenshot",         arg(building.dir / (std::string(name) + ".png"))};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), building.dir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    return run;
}

/// The checks of every run: clean, the arcade room shown, its hero cabinet bound, never more decoders than `budget`.
const rv::app::VideoRecord& requireArcade(const AppRun& run, std::uint64_t budget) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    CHECK(run.report->world.state == "room");
    CHECK(run.report->world.room == kRoom);
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.screen == kHeroScreen);
    CHECK(video.maxDecoders == budget);
    CHECK(video.maxOpenDecoders <= budget);
    CHECK(video.maxDecoderThreads <= budget);
    CHECK(video.joinedAtExit);
    return video;
}

/// The plan the run showed and the arcade room's scene and collection, as retroverse.exe made them from it.
struct ShownRoom {
    rv::world::StoredPlan stored;
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
};

ShownRoom shownRoom(const PlannedBuilding& building, const rv::app::RunReport& report) {
    auto stored = rv::world::readStoredPlan(
        rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    return ShownRoom{std::move(*stored), std::move(*scene), std::move(*layout)};
}

/// The walker 0.9 m in front of an item's visible face, looking at its middle; the face and the camera then.
struct Viewpoint {
    glm::vec3 face{0.0f};
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
};

Viewpoint viewpointOf(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens, float distance = 0.9f) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    const glm::vec3 face(item.slotFrame * glm::vec4(local, 1.0f));
    glm::vec3 out(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 eye(face.x + distance * out.x, kEyeHeight, face.z + distance * out.z);
    const glm::vec3 look = face - eye;
    Viewpoint viewpoint;
    viewpoint.face = face;
    viewpoint.spawn.feet = glm::vec3(eye.x, 0.0f, eye.z);
    viewpoint.spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    viewpoint.spawn.pitchDeg =
        rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    viewpoint.camera = lens;
    viewpoint.camera.position = eye;
    viewpoint.camera.yawDeg = *viewpoint.spawn.yawDeg;
    viewpoint.camera.pitchDeg = *viewpoint.spawn.pitchDeg;
    return viewpoint;
}

/// The walker at `item`'s Focus pose as --spawn takes it (the default window's aspect).
rv::app::SpawnPoint focusSpawn(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = kWindowSize.x / kWindowSize.y;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    return spawn;
}

/// The yaw and pitch that look along `direction` (scene::Camera's conventions: yaw 0 towards -Z, positive to the right
/// and up).
glm::vec2 lookAlong(const glm::vec3& direction) {
    return {std::atan2(direction.x, -direction.z), std::asin(std::clamp(direction.y, -1.0f, 1.0f))};
}

/// `to - from` in mouse pixels, the yaw's difference taken the short way round.
glm::vec2 turnPixels(glm::vec2 from, glm::vec2 to) {
    glm::vec2 turn = to - from;
    constexpr float kPi = 3.14159265f;
    while (turn.x > kPi) {
        turn.x -= 2.0f * kPi;
    }
    while (turn.x <= -kPi) {
        turn.x += 2.0f * kPi;
    }
    return turn / kRadiansPerPixel;
}

/// The middle of the hero cabinet's screen, about: its hero corner fixture's origin 1.3 m up.
glm::vec3 heroCabinetPoint(const ShownRoom& shown) {
    const auto& fixtures = shown.stored.plan.fixtures;
    const auto hero = std::find_if(fixtures.begin(), fixtures.end(), [](const rv::world::Fixture& fixture) {
        return fixture.id.room().toString() == kRoom && fixture.role == rv::world::FixtureRole::HeroCorner &&
               fixture.id.part() == "crt";
    });
    REQUIRE(hero != fixtures.end());
    return glm::vec3(rv::world::room_frames::fixtureTransform(*hero) * glm::vec4(0.0f, 1.3f, 0.0f, 1.0f));
}

/// A featured cabinet (the display pairs facing the aisle) whose game is not `except` (the hero cabinet's: its clip
/// would share the hero's slot).
const rv::world::CollectionItem& featuredCabinet(const ShownRoom& shown, std::string_view except) {
    const auto found = std::find_if(shown.layout.items.begin(), shown.layout.items.end(), [&](const auto& item) {
        return item.section == rv::world::Section::Featured && item.game.toString() != except;
    });
    REQUIRE(found != shown.layout.items.end());
    return *found;
}

/// The first run: the room planned, the covers into the cache, the hero cabinet's start choice.
struct FirstRun {
    ShownRoom shown;
    std::string heroGame;
};

FirstRun firstRun(const PlannedBuilding& building) {
    const AppRun first = runArcade(building, "first", 2, {});
    requireArcade(first, 2);
    return FirstRun{shownRoom(building, *first.report), first.report->video.gameId};
}

/// The F12 screenshots of a run (<data>/screenshots), in the order they were taken (their names).
std::vector<fs::path> takenScreenshots(const PlannedBuilding& building) {
    std::vector<fs::path> files;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(building.data / "screenshots", error)) {
        if (entry.path().extension() == ".png") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

rv::assets::ImageData readShot(const fs::path& file) {
    auto image = rv::assets::loadImage(file, 3);
    INFO(rv::core::pathToUtf8(file));
    REQUIRE(image.has_value());
    return std::move(*image);
}

/// Pixels of `a` and `b` (the same size) within the box [min, max] whose channels differ by more than 24 (the picture
/// changed there, not the dithering).
std::size_t changedPixels(const rv::assets::ImageData& a, const rv::assets::ImageData& b, glm::ivec2 min,
                          glm::ivec2 max) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    std::size_t changed = 0;
    for (int y = std::max(0, min.y); y <= std::min(a.height - 1, max.y); ++y) {
        for (int x = std::max(0, min.x); x <= std::min(a.width - 1, max.x); ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(a.channels);
            int largest = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                largest = std::max(largest, std::abs(int(a.pixels[at + c]) - int(b.pixels[at + c])));
            }
            changed += largest > 24 ? 1 : 0;
        }
    }
    return changed;
}

/// The cabinet events of `screen`, in order.
std::vector<rv::app::VideoCabinetEventRecord> cabinetEvents(const rv::app::VideoRecord& video, std::string_view screen) {
    std::vector<rv::app::VideoCabinetEventRecord> events;
    std::copy_if(video.cabinetEvents.begin(), video.cabinetEvents.end(), std::back_inserter(events),
                 [&](const rv::app::VideoCabinetEventRecord& event) { return event.screen == screen; });
    return events;
}

std::vector<rv::app::VideoSlotEventRecord> slotEvents(const rv::app::VideoRecord& video, std::string_view action,
                                                      std::string_view gameId) {
    std::vector<rv::app::VideoSlotEventRecord> events;
    std::copy_if(video.slotEvents.begin(), video.slotEvents.end(), std::back_inserter(events),
                 [&](const rv::app::VideoSlotEventRecord& event) {
                     return event.action == action && event.gameId == gameId;
                 });
    return events;
}

const rv::app::VideoScreenRecord* screenOf(const rv::app::VideoRecord& video, std::string_view id) {
    const auto found = std::find_if(video.screens.begin(), video.screens.end(),
                                    [id](const rv::app::VideoScreenRecord& screen) { return screen.screen == id; });
    return found != video.screens.end() ? &*found : nullptr;
}

/// The hero cabinet never followed a selection (ADR-0016 §6): every game it played came from its start choice or its
/// playlist.
void requireHeroUnselected(const rv::app::VideoRecord& video) {
    CHECK(video.selections == 0);
    for (const rv::app::VideoPlayedRecord& played : video.played) {
        INFO("the hero cabinet played " << played.gameId << " (" << played.origin << ") at " << played.atSeconds);
        CHECK(played.origin != "selection");
    }
}

} // namespace

TEST_CASE("cabinets: the inspected cabinet plays its clip in the hand, in Focus after Indietro, its cover after Back",
          "[gpu][video][cabinets]") {
    ArcadeBuilding building("inspect");
    // No attract here: after Back the cabinet stands in front of the walker, which would start it again after 1 s.
    writeVideoSettings(building, Json{{"cabinet", {{"enabled", false}}}});
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    // Frame 40 interact: inspected at once (a face-out item), lifted into the hand; F12 at 80 and 84. Frame 100
    // ui_back (Indietro): Focus on it, lowered to its pulled-out pose; F12 at 140 and 144. Frame 160 back: free roam,
    // the cabinet put back; F12 at 200 and 204.
    const fs::path script = writeScript(
        building, "inspect.json",
        Json::array({step(40, 40, {{"interact", 1}}), step(80, 80, {{"screenshot", 1}}),
                     step(84, 84, {{"screenshot", 1}}), step(100, 100, {{"ui_back", 1}}),
                     step(140, 140, {{"screenshot", 1}}), step(144, 144, {{"screenshot", 1}}),
                     step(160, 160, {{"back", 1}}), step(200, 200, {{"screenshot", 1}}),
                     step(204, 204, {{"screenshot", 1}})}));
    const AppRun run =
        runArcade(building, "inspect", 210,
                  {"--spawn", spawnArgument(focusSpawn(item, first.shown.scene.camera)), "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    // Its clip on the second slot (the hero cabinet keeps the first), shown in the hand from its first frame, its
    // cover again at the frame Back put it back.
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 2);
    CHECK(events[0].picture == "video");
    CHECK(events[0].gameId == item.game.toString());
    CHECK(events[0].screenClass == "direct");
    CHECK(events[0].frame >= 40u);
    CHECK(events[0].frame <= 70u);
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame >= 160u);
    CHECK(events[1].frame <= 161u);
    const auto opens = slotEvents(video, "open", item.game.toString());
    REQUIRE(opens.size() == 1);
    CHECK(opens.front().screens == std::vector<std::string>{screen});
    const auto stops = slotEvents(video, "stop", item.game.toString());
    REQUIRE_FALSE(stops.empty());
    CHECK(stops.front().reason == "unwanted");
    CHECK(video.maxOpenDecoders == 2);
    CHECK(video.fallbacks.empty());
    // The hero cabinet (behind the walker: paused from the start, on its cover) kept its own game throughout.
    requireHeroUnselected(video);
    CHECK(video.gameId == first.heroGame);
    // The screenshots: the clip moving in the hand (80 and 84) and in Focus (140 and 144), the cover still after Back
    // (200 and 204), each around the middle of the view where the cabinet's screen is.
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 6);
    const glm::ivec2 middleMin(kShotSize.x * 3 / 10, kShotSize.y * 2 / 10);
    const glm::ivec2 middleMax(kShotSize.x * 7 / 10, kShotSize.y * 8 / 10);
    const std::size_t inHand = changedPixels(readShot(shots[0]), readShot(shots[1]), middleMin, middleMax);
    const std::size_t inFocus = changedPixels(readShot(shots[2]), readShot(shots[3]), middleMin, middleMax);
    const std::size_t putBack = changedPixels(readShot(shots[4]), readShot(shots[5]), middleMin, middleMax);
    WARN("pixels changed in 4 frames: in the hand " << inHand << ", in Focus " << inFocus << ", put back " << putBack);
    CHECK(inHand > 500u);
    CHECK(inFocus > 500u);
    CHECK(putBack < 50u);
}

TEST_CASE("cabinets: attract plays the nearest cabinet facing the eye after 1 s and stops once it leaves the view",
          "[gpu][video][cabinets]") {
    ArcadeBuilding building("attract");
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    const Viewpoint view = viewpointOf(item, first.shown.scene.camera);
    // F12 at frame 120 (attract playing); frame 150 a half turn (the cabinet out of view); the run ends at 170.
    const float halfTurn = 3.14159265f / kRadiansPerPixel;
    const fs::path script =
        writeScript(building, "attract.json",
                    Json::array({step(120, 120, {{"screenshot", 1}}), step(150, 150, {{"look_delta_x", halfTurn}})}));
    const AppRun run =
        runArcade(building, "attract", 170, {"--spawn", spawnArgument(view.spawn), "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 2);
    CHECK(events[0].picture == "video");
    CHECK(events[0].screenClass == "ambient");
    CHECK(events[0].gameId == item.game.toString());
    // Its dwell from the first frame: asked for at frame 60, its first frame shown within half a second.
    CHECK(events[0].frame >= static_cast<std::uint64_t>(kDwellFrames));
    CHECK(events[0].frame <= static_cast<std::uint64_t>(kDwellFrames + 30));
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame == 150u);
    // No other cabinet played; the open came at the dwell's end.
    CHECK(video.cabinetEvents.size() == 2);
    const auto opens = slotEvents(video, "open", item.game.toString());
    REQUIRE(opens.size() == 1);
    CHECK(opens.front().frame == static_cast<std::uint64_t>(kDwellFrames));
    requireHeroUnselected(video);
    const std::vector<fs::path> shots = takenScreenshots(building);
    REQUIRE(shots.size() == 1);
    const rv::assets::ImageData shot = readShot(shots.front());
    const std::optional<glm::ivec2> face = projectToPixel(view.camera, kShotSize, view.face);
    REQUIRE(face.has_value());
    WARN("the attract cabinet's screen at (" << face->x << ", " << face->y << "): mean luma "
                                             << windowLuma(shot, *face, 40));
    CHECK(windowLuma(shot, *face, 40) > 30.0);
}

TEST_CASE("cabinets: with one decoder the inspected cabinet takes the slot and the hero cabinet keeps its paused frame",
          "[gpu][video][cabinets]") {
    ArcadeBuilding building("one_decoder");
    writeVideoSettings(building, Json{{"cabinet", {{"enabled", false}}}});
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    // The walker at the cabinet's Focus pose, looking at the hero cabinet first (its clip playing in view); frame 60
    // turned onto the cabinet, frame 70 interact: inspected. Frame 130 ui_back (Focus), frame 170 back (free roam).
    const rv::app::SpawnPoint focus = focusSpawn(item, first.shown.scene.camera);
    const glm::vec3 eye = focus.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    const glm::vec2 heroLook = lookAlong(glm::normalize(heroCabinetPoint(first.shown) - eye));
    const glm::vec2 itemLook(rv::core::degreesToRadians(*focus.yawDeg), rv::core::degreesToRadians(*focus.pitchDeg));
    rv::app::SpawnPoint spawn = focus;
    spawn.yawDeg = rv::core::radiansToDegrees(heroLook.x);
    spawn.pitchDeg = rv::core::radiansToDegrees(heroLook.y);
    const glm::vec2 turn = turnPixels(heroLook, itemLook);
    const fs::path script = writeScript(
        building, "inspect.json",
        Json::array({step(60, 60, {{"look_delta_x", turn.x}, {"look_delta_y", turn.y}}),
                     step(70, 70, {{"interact", 1}}), step(130, 130, {{"ui_back", 1}}), step(170, 170, {{"back", 1}})}));
    const AppRun run = runArcade(building, "one_decoder", 230,
                                 {"--max-decoders", "1", "--spawn", spawnArgument(spawn), "--input-script",
                                  arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 1);
    INFO(run.describe());
    CHECK(video.maxOpenDecoders == 1);
    // The inspected cabinet (direct) takes the hero's slot at once: the hero's clip stops, still wanted ("needed").
    const auto heroStops = slotEvents(video, "stop", first.heroGame);
    REQUIRE_FALSE(heroStops.empty());
    CHECK(heroStops.front().reason == "needed");
    CHECK(heroStops.front().frame == 70u);
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 2);
    CHECK(events[0].picture == "video");
    CHECK(events[0].screenClass == "direct");
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame >= 170u);
    CHECK(events[1].frame <= 171u);
    // Meanwhile the hero cabinet showed the frame it kept (ADR-0016 §6), then its clip opened again where it stopped once
    // the cabinet let the slot go.
    CHECK(video.heldFrameShows >= 90u);
    const auto heroOpens = slotEvents(video, "open", first.heroGame);
    REQUIRE(heroOpens.size() == 2);
    CHECK(heroOpens.back().frame >= 171u);
    const rv::app::VideoScreenRecord* hero = screenOf(video, kHeroScreen);
    REQUIRE(hero != nullptr);
    CHECK_FALSE(hero->heldFrame);
    requireHeroUnselected(video);
}

TEST_CASE("cabinets: no cabinet plays while the room editor runs, and attract comes back after it",
          "[gpu][video][cabinets][edit]") {
    ArcadeBuilding building("editor");
    const FirstRun first = firstRun(building);
    const rv::world::CollectionItem& item = featuredCabinet(first.shown, first.heroGame);
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    const Viewpoint view = viewpointOf(item, first.shown.scene.camera);
    // Attract from frame 60; frame 90 the directory (M), "Modifica stanza" (as world_edit_discard.json), the editor
    // begins at frame 108; frame 150 its menu, Esci senza salvare at frame 180; the run ends 120 frames after.
    const fs::path script =
        writeScript(building, "editor.json",
                    Json::array({step(90, 90, {{"map", 1}}), step(96, 96, {{"ui_home", 1}}),
                                 step(102, 102, {{"ui_up", 1}}), step(108, 108, {{"ui_activate", 1}}),
                                 step(150, 150, {{"confirm", 1}}), step(160, 160, {{"ui_down", 1}}),
                                 step(170, 170, {{"ui_down", 1}}), step(180, 180, {{"ui_activate", 1}})}));
    const AppRun run =
        runArcade(building, "editor", 300, {"--spawn", spawnArgument(view.spawn), "--input-script", arg(script)});
    const rv::app::VideoRecord& video = requireArcade(run, 2);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.room == kRoom);
    CHECK(edit.end == "exited"); // nothing edited: Esci senza salvare leaves at once
    REQUIRE(edit.endFrame.has_value());
    const std::vector<rv::app::VideoCabinetEventRecord> events = cabinetEvents(video, screen);
    REQUIRE(events.size() == 3);
    // Attract before the session; the cover from the frame the session began; attract again 1 s after it ended.
    CHECK(events[0].picture == "video");
    CHECK(events[0].frame < edit.enterFrame);
    CHECK(events[1].picture == "cover");
    CHECK(events[1].frame == edit.enterFrame);
    CHECK(events[2].picture == "video");
    CHECK(events[2].frame >= *edit.endFrame + kDwellFrames);
    CHECK(video.cabinetEvents.size() == 3);
}
