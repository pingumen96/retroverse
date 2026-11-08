// The VR mode in a generated room (I10 step 4, ADR-0017 §5) with the fake runtime: the snes-room fixture
// (tests/fixtures/esde/snes-room, every item face-out) imported into the case's data directory, a first desktop run
// planning the room, then retroverse.exe --vr --xr-runtime fake from the Focus pose of the hero wall's band item
// fx:hw-w/s:5.4, standing still. The input script's right controller points at the item from the hip; interact at
// frame 40 inspects it, and it is lifted into the right hand: held 12 cm in front of the controller's grip, not in
// front of the eyes. The ray rested on the item, so the controller vibrated; the walker never moved. I10 step 6: Avvia
// on it launches the fake emulator of this build (in the background: a small window that never takes the focus),
// whose window the headset's run captures onto the hero CRT while it shows, the frames going on. Helpers repeat
// test_world_card.cpp's.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/picture_bounds.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_inspect.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/platform_win/audio_endpoint.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using Catch::Approx;
using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

/// The window's size: the Focus pose frames the item for it (as --spawn takes it).
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};
constexpr glm::ivec2 kEyeSize(1024, 1024);
constexpr float kEyeHeight = 1.65f;
/// The right controller in the tracking space: at the hip, a little forward.
constexpr glm::vec3 kRightHand(0.2f, 1.25f, -0.3f);

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    /// `launches`: Avvia starts this build's fake emulator (a "fake-return" profile with a window, test_world_card.cpp's
    /// with --frames), with `game` before its own arguments (I10 step 11: a pillarboxed picture).
    explicit ImportedRoom(std::string_view name, bool launches = false, std::vector<std::string> game = {})
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("xr_room/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        if (launches) {
            // A window for 150 frames (about 2.5 s), which the headset's run captures.
            std::vector<std::string> arguments = std::move(game);
            for (const char* argument : {"--frames", "150", "--exit-timeout-ms", "30000", "--", "{rom}"}) {
                arguments.emplace_back(argument);
            }
            const nlohmann::json profile{
                {"id", "fake-return"},
                {"executable", arg(fakeEmulatorExe())},
                {"arguments", arguments},
                {"windowTimeoutSeconds", 0},
                {"hideConsoleWindow", true},
                {"systems", {{"snes", nlohmann::json::object()}}}};
            const nlohmann::json document{{"schemaVersion", 1}, {"profiles", nlohmann::json::array({profile})}};
            REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "emulators.json", document.dump(2) + "\n")
                        .has_value());
            cli("assign", {"assign", "snes", "--profile", "fake-return"});
        }
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }
};

struct ShownRoom {
    rv::scene::CameraSetup camera;
    rv::world::CollectionLayout layout;
};

ShownRoom shownRoom(const ImportedRoom& room, const rv::app::RunReport& report) {
    const fs::path plan =
        rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan;
    auto stored = rv::world::readStoredPlan(plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    return ShownRoom{scene->camera, std::move(*layout)};
}

const rv::world::CollectionItem& itemIn(const ShownRoom& room, std::string_view slot) {
    const auto found = std::find_if(room.layout.items.begin(), room.layout.items.end(), [&](const auto& item) {
        return item.slot.toString().ends_with("/" + std::string(slot));
    });
    INFO("the item in " << slot);
    REQUIRE(found != room.layout.items.end());
    return *found;
}

rv::scene::WalkerPose focusPose(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = kWindowSize.x / kWindowSize.y;
    return rv::app::focusPoseFor(item, viewer);
}

/// A tracking-space offset in the world for a walker at yaw `yaw` (the tracking space turned with it, ADR-0017 §5).
glm::vec3 turned(const glm::vec3& tracked, float yaw) {
    return glm::vec3(tracked.x * std::cos(yaw) - tracked.z * std::sin(yaw), tracked.y,
                     tracked.x * std::sin(yaw) + tracked.z * std::cos(yaw));
}

/// The screenshots a run's input script took (<data>/screenshots), in order.
std::vector<fs::path> screenshotsOf(const ImportedRoom& room) {
    std::vector<fs::path> taken;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(room.data / "screenshots", error)) {
        if (entry.path().extension() == ".png") {
            taken.push_back(entry.path());
        }
    }
    std::sort(taken.begin(), taken.end());
    return taken;
}

} // namespace

TEST_CASE("xr: the right controller's ray inspects a cartridge, held in that hand, the walker unmoved (I10 step 4)",
          "[gpu][xr]") {
    const ImportedRoom room("inspect");
    const fs::path firstShot = room.dir / "first.png";
    const AppRun first = runApp(retroverseExe(), room.dir, "first",
                                {"--start-room", "room:snes", "--exit-after-frames", "--frames", "3", "--fixed-dt",
                                 "0.016667", "--screenshot", arg(firstShot)});
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.4");
    const rv::scene::WalkerPose spawn = focusPose(item, shown.camera);
    const glm::vec3 itemCenter(item.slotFrame * glm::vec4(item.slotBox.center(), 1.0f));

    // The controller's pose pointing at the item's centre, in the tracking space (turned back by the walker's yaw).
    const glm::vec3 handWorld = spawn.feet + turned(kRightHand, spawn.yaw);
    const glm::vec3 toItem = glm::normalize(itemCenter - handWorld);
    const glm::vec3 local(toItem.x * std::cos(spawn.yaw) + toItem.z * std::sin(spawn.yaw), toItem.y,
                          -toItem.x * std::sin(spawn.yaw) + toItem.z * std::cos(spawn.yaw));
    const float handYaw = rv::core::radiansToDegrees(std::atan2(local.x, -local.z));
    const float handPitch = rv::core::radiansToDegrees(std::asin(std::clamp(local.y, -1.0f, 1.0f)));
    const fs::path script = room.dir / "inspect.json";
    const std::string hand = fmt::format(R"("rightHand": {{"yaw": {}, "pitch": {}, "position": [{}, {}, {}]}})",
                                         handYaw, handPitch, kRightHand.x, kRightHand.y, kRightHand.z);
    const std::string head = R"("headPose": {"position": [0, 1.65, 0]})";
    REQUIRE(rv::core::writeTextFileAtomic(
                script, fmt::format(R"([
  {{"fromFrame": 0, "toFrame": 39, {0}, {1}}},
  {{"fromFrame": 40, "toFrame": 40, {0}, {1}, "actions": {{"interact": 1}}}},
  {{"fromFrame": 41, "toFrame": 89, {0}, {1}}}
])",
                                    head, hand))
                .has_value());

    rv::app::SpawnPoint spawnPoint;
    spawnPoint.feet = spawn.feet;
    spawnPoint.yawDeg = rv::core::radiansToDegrees(spawn.yaw);
    spawnPoint.pitchDeg = 0.0f;
    const fs::path shot = room.dir / "eyes.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "vr",
                              {"--vr", "--xr-runtime", "fake", "--start-room", "room:snes", "--spawn",
                               spawnArgument(spawnPoint), "--input-script", arg(script), "--exit-after-frames",
                               "--frames", "90", "--fixed-dt", "0.016667", "--screenshot", arg(shot)});
    requireCleanRun(run);
    INFO(run.describe());
    requireScreenshot(run, shot, glm::ivec2(2 * kEyeSize.x, kEyeSize.y));
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.framesWithViews == 90);
    CHECK(xr.handFrames == 90);
    CHECK(xr.pointedFrames > 0);
    // The ray came onto the item: one vibration of the right controller.
    CHECK(xr.vibrations >= 1);

    // Inspected, and held in the right hand: 12 cm along the controller's pointing axis.
    REQUIRE(run.report->world.state == "room");
    CHECK(run.report->world.interaction.state == "inspect");
    const rv::app::InspectRecord& inspect = run.report->world.interaction.inspect;
    CHECK(inspect.lift == 1.0);
    const glm::vec3 heldWorld = handWorld + toItem * rv::app::kHandHoldDistance;
    const glm::vec3 eye = spawn.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    const glm::vec3 forward(std::sin(spawn.yaw), 0.0f, -std::cos(spawn.yaw));
    const glm::vec3 right(std::cos(spawn.yaw), 0.0f, std::sin(spawn.yaw));
    const glm::vec3 inCamera(glm::dot(heldWorld - eye, right), heldWorld.y - eye.y,
                             -glm::dot(heldWorld - eye, forward));
    CHECK(inspect.heldCenter.x == Approx(inCamera.x).margin(0.01));
    CHECK(inspect.heldCenter.y == Approx(inCamera.y).margin(0.01));
    CHECK(inspect.heldCenter.z == Approx(inCamera.z).margin(0.01));

    // Focus and Inspect never moved the walker: the head stands where it started.
    REQUIRE(xr.head.size() >= 90);
    CHECK(glm::length(xr.head[89].position - xr.head[0].position) < 1e-4f);
    CHECK(glm::length(glm::vec2(xr.head[0].position.x - spawn.feet.x, xr.head[0].position.z - spawn.feet.z)) < 1e-4f);
}

namespace {

/// The room's first run (the room shown), then the headset's: the right controller's ray on fx:hw-w/s:5.4 inspects it
/// at frame 40, Avvia (the pad's Start) at frame 70 launches the fake emulator, `steps` (input script steps, each
/// ending with a comma) on top; `frames` frames in all.
AppRun launchInHeadset(const ImportedRoom& room, std::string_view steps, int frames,
                       const std::vector<std::string>& extra = {}) {
    const AppRun first = runApp(retroverseExe(), room.dir, "first",
                                {"--start-room", "room:snes", "--exit-after-frames", "--frames", "3", "--fixed-dt",
                                 "0.016667", "--screenshot", arg(room.dir / "first.png")});
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.4");
    const rv::scene::WalkerPose spawn = focusPose(item, shown.camera);
    const glm::vec3 itemCenter(item.slotFrame * glm::vec4(item.slotBox.center(), 1.0f));
    const glm::vec3 handWorld = spawn.feet + turned(kRightHand, spawn.yaw);
    const glm::vec3 toItem = glm::normalize(itemCenter - handWorld);
    const glm::vec3 local(toItem.x * std::cos(spawn.yaw) + toItem.z * std::sin(spawn.yaw), toItem.y,
                          -toItem.x * std::sin(spawn.yaw) + toItem.z * std::cos(spawn.yaw));
    const std::string hand = fmt::format(R"("rightHand": {{"yaw": {}, "pitch": {}, "position": [{}, {}, {}]}})",
                                         rv::core::radiansToDegrees(std::atan2(local.x, -local.z)),
                                         rv::core::radiansToDegrees(std::asin(std::clamp(local.y, -1.0f, 1.0f))),
                                         kRightHand.x, kRightHand.y, kRightHand.z);
    const fs::path script = room.dir / "launch.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, fmt::format(R"([
  {}
  {{"fromFrame": 0, "toFrame": {}, "headPose": {{"position": [0, 1.65, 0]}}, {}}},
  {{"fromFrame": 40, "toFrame": 40, "actions": {{"interact": 1}}}},
  {{"fromFrame": 70, "toFrame": 70, "actions": {{"confirm": 1}}}}
])",
                                                              steps, frames - 1, hand))
                .has_value());
    rv::app::SpawnPoint spawnPoint;
    spawnPoint.feet = spawn.feet;
    spawnPoint.yawDeg = rv::core::radiansToDegrees(spawn.yaw);
    spawnPoint.pitchDeg = 0.0f;
    std::vector<std::string> arguments{"--vr",          "--xr-runtime",      "fake",       "--start-room",
                                       "room:snes",     "--spawn",           spawnArgument(spawnPoint),
                                       "--input-script", arg(script),        "--exit-after-frames",
                                       "--frames",      std::to_string(frames), "--fixed-dt", "0.016667"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), room.dir, "vr", arguments);
    requireCleanRun(run);
    return run;
}

} // namespace

TEST_CASE("xr: a game launched in the headset plays on the hero CRT, captured from its window (I10 step 6)",
          "[gpu][xr]") {
    ensureLaunchAllowList();
    const ImportedRoom room("launch", true);
    // The game's window shows about 2.5 s, captured meanwhile. I10 step 10: from frame 75 to 100, while the game runs,
    // the left controller's stick pulled back walks the user back from the shelf.
    const AppRun run = launchInHeadset(room, R"({"fromFrame": 75, "toFrame": 100, "leftStick": [0, -1]},)", 900);
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    CHECK(run.report->launches.front().profile == "fake-return");
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    // The headset never stopped: every frame submitted with both eyes, the game's window captured onto the CRT.
    CHECK(xr.framesWithViews == 900);
    CHECK(xr.captureError.empty());
    CHECK(xr.captureStarts == 1);
    CHECK(xr.captureFrames > 0);
    CHECK(xr.captureShownFrames == xr.captureFrames);
    CHECK(xr.captureSize.x > 0);
    CHECK(xr.captureSize.y > 0);
    // The controllers walked while the game ran (the pad's actions were the game's).
    CHECK(xr.playingWalkFrames > 0);
    const auto headAt = [&xr](std::uint64_t frame) {
        const auto sample = std::find_if(xr.head.begin(), xr.head.end(),
                                         [frame](const rv::app::XrHeadSample& head) { return head.frame == frame; });
        REQUIRE(sample != xr.head.end());
        return sample->position;
    };
    const glm::vec3 before = headAt(74);
    const glm::vec3 after = headAt(101);
    CHECK(glm::length(glm::vec2(after.x - before.x, after.z - before.z)) > 0.2f);
    // A window with no bar shows its whole client area (I10 step 11: the test window's frame and title bar left out):
    // its 16:9.
    CHECK(xr.captureCropSamples > 0);
    const glm::vec2 shown = glm::vec2(xr.captureCrop.z, xr.captureCrop.w) * glm::vec2(xr.captureSize);
    CHECK(shown.x / shown.y == Catch::Approx(16.0 / 9.0).margin(0.05));
}

TEST_CASE("xr: a game pillarboxed in its wide window fills the hero CRT without its bars (I10 step 11)",
          "[gpu][xr]") {
    ensureLaunchAllowList();
    // The fake emulator's 16:9 window holds its picture in the middle 4:3, black at the sides, for at least 5 s.
    const ImportedRoom room("pillarbox", true, {"--pillarbox", "--min-window-ms", "5000"});
    const AppRun run = launchInHeadset(room, "", 1800);
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.captureError.empty());
    CHECK(xr.captureCropSamples >= rv::app::PictureBounds::kNarrowSamples);
    CHECK(xr.captureCropChanges >= 1);
    // The capture holds the test's window with its frame and title bar: the crop is its client area's middle 4:3, an
    // eighth of the client's width left out on each side (less a sampled column kept), centred in the capture.
    const glm::vec2 capture(xr.captureSize);
    const glm::vec2 shown = glm::vec2(xr.captureCrop.z, xr.captureCrop.w) * capture;
    CHECK(shown.x / shown.y == Catch::Approx(4.0 / 3.0).margin(0.05));
    CHECK(xr.captureCrop.z < 0.8f);
    CHECK(xr.captureCrop.x + 0.5f * xr.captureCrop.z == Catch::Approx(0.5f).margin(0.02f));
    CHECK(xr.captureCrop.y + xr.captureCrop.w <= 1.0f);
}

TEST_CASE("xr: a game launched where no CRT is bound shows on the large screen (I11 step 14)", "[gpu][xr]") {
    // The lobby binds no screen; the media viewer's clip makes the screens' service there, unbound (I5 step 8's
    // review), which then has no hero to show the game's window on. Since I11 step 14 (ADR-0020 §6) the large screen in
    // front of the user shows it. The game's window shows at least 6 s; a screenshot of both eyes at frame 330.
    ensureLaunchAllowList();
    const ImportedRoom room("no_crt", true, {"--min-window-ms", "6000"});
    const AppRun first = runApp(retroverseExe(), room.dir, "first", {"--exit-after-frames", "--frames", "3"});
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    // The quick catalog on Amber Lantern's card (frame 52), its viewer (56), the clip's page (60 to 75), closed at 170,
    // Avvia at 180.
    nlohmann::json steps = nlohmann::json::array();
    const auto press = [&steps](int frame, std::string_view action) {
        steps.push_back({{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}});
    };
    press(40, "quick_catalog");
    press(43, "ui_up");
    press(46, "ui_up");
    steps.push_back({{"fromFrame", 49}, {"toFrame", 49}, {"text", "amber"}});
    press(52, "ui_activate");
    press(56, "ui_media");
    for (int frame = 60; frame <= 75; frame += 3) {
        press(frame, "ui_right");
    }
    press(170, "ui_back");
    press(180, "ui_activate");
    press(330, "screenshot");
    const fs::path script = room.dir / "no_crt.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, steps.dump(2) + "\n").has_value());
    const AppRun run = runApp(retroverseExe(), room.dir, "vr",
                              {"--vr", "--xr-runtime", "fake", "--input-script", arg(script), "--exit-after-frames",
                               "--frames", "900", "--fixed-dt", "0.016667"});
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    CHECK(report.world.room.empty()); // the lobby
    CHECK(report.video.screens.empty());
    CHECK(report.video.viewer.clips >= 1);
    REQUIRE(report.launches.size() == 1);
    CHECK(report.launches.front().profile == "fake-return");
    REQUIRE(report.xr);
    const rv::app::XrRecord& xr = *report.xr;
    CHECK(xr.framesWithViews == 900);
    CHECK(xr.captureError.empty());
    CHECK(xr.captureStarts == 1);
    CHECK(xr.captureFrames > 0);
    CHECK(xr.captureShownFrames == xr.captureFrames);
    CHECK(xr.largeScreenFrames == xr.captureFrames);
    // The screenshot of both eyes with the large screen, kept for a look.
    CHECK(screenshotsOf(room).size() == 1);
}

TEST_CASE("xr: the menu button held stops watching the game, the room comes back, and the game is watched again "
          "(I11 step 14)",
          "[gpu][xr]") {
    ensureLaunchAllowList();
    // The game's window shows at least 12 s. Frames 300 to 380 the left menu button held (1.3 s); the session panel asks
    // with "Torna al gioco" focused, which the press at 470 takes; screenshots at 430 (stopped) and 700 (watching).
    const ImportedRoom room("stop_watching", true, {"--min-window-ms", "12000"});
    const AppRun run = launchInHeadset(room,
                                       R"({"fromFrame": 300, "toFrame": 380, "leftMenu": true},
  {"fromFrame": 430, "toFrame": 430, "actions": {"screenshot": 1}},
  {"fromFrame": 470, "toFrame": 470, "actions": {"ui_activate": 1}},
  {"fromFrame": 700, "toFrame": 700, "actions": {"screenshot": 1}},)",
                                       2400);
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    const rv::app::LaunchRecord& launch = run.report->launches.front();
    CHECK(launch.watchStops == 1);
    CHECK(launch.watchResumes == 1);
    CHECK(std::find(launch.panels.begin(), launch.panels.end(), "playing") != launch.panels.end());
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.captureError.empty());
    // Stopped about 90 frames (380 to 470); the capture ended meanwhile and started again on the same window.
    CHECK(xr.stoppedWatchingFrames >= 60);
    CHECK(xr.stoppedWatchingFrames <= 120);
    CHECK(xr.captureStarts >= 2);
    CHECK(xr.captureShownFrames == xr.captureFrames);
    CHECK(xr.largeScreenFrames == 0);
    // The CRT's decoders stopped for the launch, ran again while the user did not watch, and stopped once more.
    CHECK(run.report->video.launches.size() >= 2);
    // Both eyes stopped (the panel before the room) and watching again (the game on the CRT), kept for a look.
    CHECK(screenshotsOf(room).size() == 2);
}

TEST_CASE("xr: xr.json's large screen beside a CRT, and the notice of a game playing on the PC's audio output "
          "(I11 step 14)",
          "[gpu][xr]") {
    ensureLaunchAllowList();
    const ImportedRoom room("large_audio", true, {"--min-window-ms", "6000"});
    const std::string largeScreen = R"({"gameScreen": "large"})";
    REQUIRE(rv::core::writeTextFileAtomic(room.data / "config" / "xr.json", largeScreen).has_value());
    // A headset endpoint no Windows device has: Windows' default output (when there is one) is never the headset.
    const AppRun run =
        launchInHeadset(room, R"({"fromFrame": 200, "toFrame": 200, "actions": {"screenshot": 1}},)", 900,
                        {"--xr-fake-audio-device", "{0.0.0.00000000}.{00000000-0000-0000-0000-00000000f4e5}"});
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.captureError.empty());
    CHECK(xr.captureFrames > 0);
    CHECK(xr.largeScreenFrames == xr.captureFrames);
    CHECK(xr.audioOutputKnown);
    if (rv::platform_win::defaultAudioOutput()) {
        CHECK(run.report->launches.front().audioElsewhere);
        CHECK(xr.audioNoticeFrames > 0);
    } else {
        WARN("this machine has no default audio output: the notice is not expected");
        CHECK_FALSE(run.report->launches.front().audioElsewhere);
    }
    // Both eyes with the large screen and the notice, kept for a look.
    CHECK(screenshotsOf(room).size() == 1);
}

TEST_CASE("xr: the CRT follows the game's window past a splash and beside a main window (I11 step 11)", "[gpu][xr]") {
    ensureLaunchAllowList();
    for (const std::string mode : {"splash", "two-windows"}) {
        DYNAMIC_SECTION(mode) {
            // The fake's other windows are gray and smaller than its purple game window, which shows at least 5 s.
            const ImportedRoom room("follow_" + mode, true, {"--window-mode", mode, "--min-window-ms", "5000"});
            const AppRun run = launchInHeadset(room, "", 1800);
            INFO(run.describe());
            REQUIRE(run.report->launches.size() == 1);
            REQUIRE(run.report->xr);
            const rv::app::XrRecord& xr = *run.report->xr;
            CHECK(xr.captureError.empty());
            CHECK(xr.captureWindows >= (mode == "splash" ? 2u : 1u));
            CHECK(xr.captureStarts >= xr.captureWindows);
            CHECK(xr.captureFrames > 0);
            CHECK(xr.captureCropSamples > 0);
            // The last picture the CRT took is the game's purple (#7A2E8C with its sweeping bar), not the gray
            // (#808080) of the splash or the main window.
            INFO("colour " << xr.captureColor.x << ", " << xr.captureColor.y << ", " << xr.captureColor.z);
            CHECK(xr.captureColor.y < 100.0f);
            CHECK(xr.captureColor.z > xr.captureColor.y + 50.0f);
            CHECK_FALSE(xr.captureBlack);
        }
    }
}

TEST_CASE("xr: a game's window that shows only black is reported (I11 step 11, ADR-0020 §5)", "[gpu][xr]") {
    ensureLaunchAllowList();
    // What a capture of an exclusive fullscreen game gives: an all-black window, for at least 9 s (5 s to report).
    const ImportedRoom room("black", true, {"--black", "--min-window-ms", "9000"});
    const AppRun run = launchInHeadset(room, "", 1800);
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.captureError.empty());
    CHECK(xr.captureFrames > 0);
    CHECK(xr.captureBlack);
    // I11 step 13 (ADR-0020 §5): the launch records it, and the session panel's notice stands before the CRT.
    CHECK(run.report->launches.front().exclusiveFullscreen);
    CHECK(xr.exclusiveNoticeFrames > 0);
}

TEST_CASE("xr: a launch in VR takes its program's arguments and append-file (I11 step 12, ADR-0020 §1)", "[gpu][xr]") {
    ensureLaunchAllowList();
    // The fake's own arguments ask for an all-black window; its program's VR settings drop that, ask for its log, and
    // pass Retroverse's append-file with --config, whose colour line makes its window green instead of its purple.
    const ImportedRoom room("vr_program", true, {"--black", "--min-window-ms", "4000"});
    const fs::path fakeLog = room.dir / "fake_vr_log.json";
    const fs::path profiles = room.data / "config" / "emulators.json";
    auto document = rv::core::readJsonFile(profiles);
    REQUIRE(document.has_value());
    (*document)["schemaVersion"] = 2;
    (*document)["programs"] = nlohmann::json::array(
        {{{"id", "fake"},
          {"files", {"rv_fake_emulator.exe"}},
          {"vr",
           {{"addArguments", {"--log", arg(fakeLog)}},
            {"dropArguments", {"--black"}},
            {"appendConfig",
             {{"option", "--config"}, {"file", "fake.cfg"}, {"lines", {"# the fake's file for VR", "color = 2E8C3A"}}}},
            {"inGame", "heavy"}}}}});
    REQUIRE(rv::core::writeTextFileAtomic(profiles, document->dump(2) + "\n").has_value());

    const AppRun run = launchInHeadset(room, "", 1800);
    INFO(run.describe());
    REQUIRE(run.report->launches.size() == 1);
    const rv::app::LaunchRecord& launch = run.report->launches.front();
    REQUIRE(launch.program.has_value());
    CHECK(launch.program->id == "fake");
    CHECK(launch.program->vr);
    CHECK(launch.program->refused.empty());
    CHECK(launch.program->inGame == "heavy");
    CHECK(launch.program->dropped == std::vector<std::string>{"--black"});
    // Retroverse's own file, in its data directory.
    const fs::path appendFile = room.data / "launcher" / "vr" / "fake.cfg";
    CHECK(fs::equivalent(rv::core::pathFromUtf8(launch.program->appendFile), appendFile));
    REQUIRE(launch.program->added.size() == 4);
    CHECK(launch.program->added[0] == "--log");
    CHECK(launch.program->added[2] == "--config");
    const auto written = rv::core::readTextFile(appendFile);
    REQUIRE(written.has_value());
    CHECK(*written == "# the fake's file for VR\ncolor = 2E8C3A\n");

    // What the fake received: the program's arguments first, its own --black gone, the file read.
    const auto log = rv::core::readJsonFile(fakeLog);
    REQUIRE(log.has_value());
    const std::vector<std::string> args = (*log)["args"].get<std::vector<std::string>>();
    REQUIRE(args.size() >= 4);
    CHECK(args[0] == "--log");
    CHECK(args[2] == "--config");
    CHECK(fs::equivalent(rv::core::pathFromUtf8(args[3]), appendFile));
    CHECK(std::find(args.begin(), args.end(), "--black") == args.end());
    CHECK((*log)["config"] == *written);

    // The CRT shows the colour the file set (#2E8C3A), not black.
    REQUIRE(run.report->xr);
    const rv::app::XrRecord& xr = *run.report->xr;
    CHECK(xr.captureError.empty());
    CHECK(xr.captureFrames > 0);
    CHECK_FALSE(xr.captureBlack);
    INFO("colour " << xr.captureColor.x << ", " << xr.captureColor.y << ", " << xr.captureColor.z);
    CHECK(xr.captureColor.y > xr.captureColor.x + 40.0f);
    CHECK(xr.captureColor.y > xr.captureColor.z + 40.0f);
}
