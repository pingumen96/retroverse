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

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_inspect.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
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
    /// with --frames).
    explicit ImportedRoom(std::string_view name, bool launches = false)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("xr_room/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        if (launches) {
            const nlohmann::json profile{
                {"id", "fake-return"},
                {"executable", arg(fakeEmulatorExe())},
                // A window for 150 frames (about 2.5 s), which the headset's run captures.
                {"arguments", {"--frames", "150", "--exit-timeout-ms", "30000", "--", "{rom}"}},
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

TEST_CASE("xr: a game launched in the headset plays on the hero CRT, captured from its window (I10 step 6)",
          "[gpu][xr]") {
    ensureLaunchAllowList();
    const ImportedRoom room("launch", true);
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
    // Inspected at frame 40, Avvia (the pad's Start) at frame 70; the game's window shows about 2.5 s, captured
    // meanwhile. I10 step 10: from frame 75 to 100, while the game runs, the left controller's stick pulled back walks
    // the user back from the shelf.
    const fs::path script = room.dir / "launch.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, fmt::format(R"([
  {{"fromFrame": 0, "toFrame": 899, "headPose": {{"position": [0, 1.65, 0]}}, {0}}},
  {{"fromFrame": 40, "toFrame": 40, "actions": {{"interact": 1}}}},
  {{"fromFrame": 70, "toFrame": 70, "actions": {{"confirm": 1}}}},
  {{"fromFrame": 75, "toFrame": 100, "leftStick": [0, -1]}}
])",
                                                              hand))
                .has_value());
    rv::app::SpawnPoint spawnPoint;
    spawnPoint.feet = spawn.feet;
    spawnPoint.yawDeg = rv::core::radiansToDegrees(spawn.yaw);
    spawnPoint.pitchDeg = 0.0f;
    const AppRun run = runApp(retroverseExe(), room.dir, "vr",
                              {"--vr", "--xr-runtime", "fake", "--start-room", "room:snes", "--spawn",
                               spawnArgument(spawnPoint), "--input-script", arg(script), "--exit-after-frames",
                               "--frames", "900", "--fixed-dt", "0.016667"});
    requireCleanRun(run);
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
}
