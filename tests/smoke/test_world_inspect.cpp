// The inspection in the generated room (I3 step 13b; ADR-0012 §4, §7): the snes-room fixture (tests/fixtures/esde/
// snes-room: 50 invented titles with generated covers and clips, every one face-out) imported into the case's data
// directory, retroverse.exe without --scene, lockstep (--fixed-dt), hidden, driven by input scripts
// (tests/input_scripts/world_inspect*.json) from the Focus pose of the band's item 5.4 computed from the plan's layout as
// retroverse.exe makes it. The checks read the run report's world.interaction record (its events, its inspect record:
// the held pose in the camera's frame, the zoom's distances, the dimmed frames, Vedi media on the north button) and its
// video record (the game the hero CRT shows, its selections), and the screenshots (the scene behind the held item
// dimmed against the same view before the inspection; the held cartridge, upright and turned, reviewed by eye): the
// lift into the hand, the turn, the zoom within 0.4 to 1.0 m, the north button read as Vedi media and never as the
// quick catalog, Back to Focus then FreeRoam with the static instance back; the hero CRT switching to the selected
// item's clip after its 300 ms dwell and not while the D-pad steps faster. (The helpers repeat a few of
// test_world_focus.cpp's, which steps 14a and 15b may edit in the same wave: fold them into a shared header after it.)

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_inspect.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
constexpr float kEyeHeight = 1.65f;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The snes-room fixture recorded as the library of <caseDir>/data and imported with retroverse-cli.
/// video.json with the secondary TVs' autoplay off (I5 step 3).
constexpr std::string_view kSecondaryOff = "{\"secondary\": {\"enabled\": false}}\n";

struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("world_inspect/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        // These cases follow the hero CRT alone, on its own decoder: the room's secondary TV (I5 step 3) is left to
        // selections (video.json's secondary autoplay off), so that it never opens a second one.
        REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "video.json", kSecondaryOff).has_value());
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }
};

std::vector<std::string> roomArguments(std::uint32_t frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames",     std::to_string(frames), "--fixed-dt",
                                       "0.016667",            "--screenshot", arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The room's collection as retroverse.exe laid it out from the plan the run wrote.
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

std::string pickText(rv::world::PickId id) {
    return id == rv::world::kNoPick ? std::string() : fmt::format("{:08X}", id);
}

/// The walker at `item`'s Focus pose as --spawn takes it.
rv::app::SpawnPoint focusSpawn(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = 1280.0f / 720.0f;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    return spawn;
}

std::vector<rv::app::InteractionEventRecord> eventsOf(const rv::app::InteractionRecord& record, std::string_view kind) {
    std::vector<rv::app::InteractionEventRecord> found;
    for (const rv::app::InteractionEventRecord& event : record.events) {
        if (event.event == kind) {
            found.push_back(event);
        }
    }
    return found;
}

const rv::app::InteractionRecord& requireInteraction(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.state == "room");
    const rv::app::InteractionRecord& interaction = run.report->world.interaction;
    REQUIRE(interaction.bound);
    CHECK(interaction.staticDrawnWhilePulledFrames == 0);
    CHECK(interaction.eventsDropped == 0);
    return interaction;
}

/// The mean linear luminance of the window of `radius` around `centre` (the sRGB bytes decoded).
double linearLuminance(const rv::assets::ImageData& image, glm::ivec2 centre, int radius) {
    const auto decode = [](std::uint8_t byte) {
        const double c = byte / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    double sum = 0.0;
    std::size_t count = 0;
    for (int y = std::max(0, centre.y - radius); y <= std::min(image.height - 1, centre.y + radius); ++y) {
        for (int x = std::max(0, centre.x - radius); x <= std::min(image.width - 1, centre.x + radius); ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(image.channels);
            sum += 0.2126 * decode(image.pixels[at]) + 0.7152 * decode(image.pixels[at + 1]) +
                   0.0722 * decode(image.pixels[at + 2]);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return glm::length(a - b) <= tolerance;
}

} // namespace

TEST_CASE("world inspect: the cartridge lifted into the hand, turned, zoomed and lowered, the scene behind dimmed",
          "[gpu][world][world_inspect]") {
    ImportedRoom room("held");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.4");
    const std::string id = pickText(item.pickId);
    const std::vector<std::string> spawn{"--spawn", spawnArgument(focusSpawn(item, shown.camera))};
    std::vector<std::string> script = spawn;
    script.insert(script.end(), {"--input-script", arg(inputScript("world_inspect.json"))});

    // The same view without the inspection: the item hovered and pulled out (its outline), nothing dimmed.
    const fs::path plainShot = room.dir / "plain.png";
    const AppRun plain = runApp(retroverseExe(), room.dir, "plain", roomArguments(100, plainShot, spawn));
    const rv::app::InteractionRecord& still = requireInteraction(plain);
    INFO(plain.describe());
    CHECK(still.state == "free roam");
    CHECK(still.inspect.dimmedFrames == 0);
    const rv::assets::ImageData plainImage = requireScreenshot(plain, plainShot, kShotSize);

    // Held, turned 45 degrees to the right and tilted 15 degrees up, 0.6 m in front of the eye, its cover to it.
    const fs::path heldShot = room.dir / "held.png";
    const AppRun held = runApp(retroverseExe(), room.dir, "held", roomArguments(100, heldShot, script));
    const rv::app::InteractionRecord& inHand = requireInteraction(held);
    INFO(held.describe());
    CHECK(inHand.state == "inspect");
    CHECK(inHand.focusedPickId == id);
    CHECK(inHand.highlightedPickId.empty()); // no outline in Inspect
    CHECK(inHand.label.empty());
    CHECK(inHand.pulled == std::vector<std::string>{id});
    const rv::app::InspectRecord& pose = inHand.inspect;
    CHECK(pose.inspections == 1);
    CHECK(pose.pickId == id);
    CHECK(pose.lift == 1.0);
    CHECK(pose.liftFrames >= 16);
    CHECK(pose.liftFrames <= 19);
    CHECK(std::fabs(pose.yawDeg - 45.0) < 0.1);
    CHECK(std::fabs(pose.pitchDeg - 15.0) < 0.1);
    CHECK(std::fabs(pose.distance - 0.45) < 1e-6);
    CHECK(pose.drawnDistance == pose.distance);
    // In the hand: left of and below the view's centre, beside the card (rv::app::kHeldViewOffset).
    const glm::vec3 handOffset(rv::app::kHeldViewOffset, -1.0f);
    CHECK(near(pose.heldCenter, 0.45f * handOffset, 1e-3f));
    const float yaw = rv::core::degreesToRadians(45.0f);
    const float pitch = rv::core::degreesToRadians(15.0f);
    // Turned about the camera's up, then tilted about its right (heldItemTransform()).
    CHECK(near(pose.heldFront,
               glm::vec3(std::sin(yaw), std::cos(yaw) * std::sin(pitch), std::cos(yaw) * std::cos(pitch)), 1e-3f));
    CHECK(near(pose.heldUp, glm::vec3(0.0f, std::cos(pitch), -std::sin(pitch)), 1e-3f));
    CHECK(pose.dimmedFrames > 0);
    CHECK(pose.heroFront); // the cover's hero texture on the held item
    // The walker never moved: the camera of the plain run.
    REQUIRE(held.report->finalCamera.has_value());
    REQUIRE(plain.report->finalCamera.has_value());
    CHECK(near(held.report->finalCamera->feet, plain.report->finalCamera->feet, 1e-5f));
    // The scene behind dimmed by one stop (and the vignette) away from the item and from the card (the right 35%).
    const rv::assets::ImageData heldImage = requireScreenshot(held, heldShot, kShotSize);
    for (const glm::ivec2 probe : {glm::ivec2(240, 540), glm::ivec2(600, 950), glm::ivec2(960, 120)}) {
        INFO("probe " << probe.x << ", " << probe.y);
        const double before = linearLuminance(plainImage, probe, 40);
        const double after = linearLuminance(heldImage, probe, 40);
        REQUIRE(before > 0.0);
        CHECK(after / before > 0.25);
        CHECK(after / before < 0.55);
    }

    // Zoomed in to the zoom's nearest end, 0.3 m.
    const fs::path zoomShot = room.dir / "zoom_in.png";
    const AppRun zoomed = runApp(retroverseExe(), room.dir, "zoom_in", roomArguments(131, zoomShot, script));
    const rv::app::InteractionRecord& close = requireInteraction(zoomed);
    INFO(zoomed.describe());
    CHECK(std::fabs(close.inspect.distance - 0.3) < 1e-6);
    CHECK(std::fabs(close.inspect.drawnDistance - 0.3) < 1e-6);
    CHECK(near(close.inspect.heldCenter, 0.3f * handOffset, 1e-3f));
    requireScreenshot(zoomed, zoomShot, kShotSize);

    // The whole script: zoomed out to 1.0 m (drawn short of the band), Vedi media on the north button (the viewer
    // closed again at frame 245), Indietro to Focus at frame 260, Back to FreeRoam at 300, the item back in its slot.
    const fs::path endShot = room.dir / "end.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "inspect", roomArguments(340, endShot, script));
    const rv::app::InteractionRecord& record = requireInteraction(run);
    INFO(run.describe());
    const auto states = eventsOf(record, "state");
    REQUIRE(states.size() == 3);
    CHECK(states[0].frame == 40);
    CHECK(states[0].state == "inspect");
    CHECK(states[1].frame == 260);
    CHECK(states[1].state == "focus");
    CHECK(states[2].frame == 300);
    CHECK(states[2].state == "free roam");
    const rv::app::InspectRecord& inspect = record.inspect;
    CHECK(inspect.inspections == 1);
    REQUIRE(inspect.minDistance.has_value());
    REQUIRE(inspect.maxDistance.has_value());
    CHECK(std::fabs(*inspect.minDistance - 0.3) < 1e-6);
    CHECK(std::fabs(*inspect.maxDistance - 1.0) < 1e-6);
    REQUIRE(inspect.maxDrawnDistance.has_value());
    CHECK(*inspect.maxDrawnDistance < 1.0);
    CHECK(*inspect.maxDrawnDistance > 0.6);
    CHECK(inspect.mediaRequests == 1);
    CHECK(inspect.catalogOpenedInInspect == 0);
    // Since step 14b Vedi media opens the media viewer over the card; the ui_back at frame 245 closes it, Inspect stays.
    std::vector<std::string> screens;
    for (const rv::app::GameScreenEventRecord& event : run.report->screens.events) {
        if (event.event == "media" || event.event == "closed") {
            screens.push_back(std::to_string(event.frame) + ":" + event.event + ":" + event.from);
        }
    }
    CHECK(screens == std::vector<std::string>{"230:media:card", "245:closed:card"});
    // The lift (300 ms) and the lowering (200 ms): about 18 + 12 frames between pulled out and held.
    CHECK(inspect.liftFrames >= 27);
    CHECK(inspect.liftFrames <= 31);
    CHECK(inspect.lift < 0.2); // the last frame off the pulled pose, on the way down
    CHECK(record.state == "free roam");
    CHECK(record.focusedPickId.empty());
    CHECK(record.pulled.empty());
    requireScreenshot(run, endShot, kShotSize);
}

TEST_CASE("world inspect: the hero CRT switches to the selected item's clip after its dwell, not while stepping faster",
          "[gpu][world][world_inspect]") {
    ImportedRoom room("crt");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.4");
    const std::vector<std::string> script{"--spawn", spawnArgument(focusSpawn(item, shown.camera)), "--input-script",
                                          arg(inputScript("world_inspect_crt.json"))};
    REQUIRE(first.report->video.bound);

    // The whole script: the inspected item selected, then four D-pad steps 10 frames apart, the last item kept.
    const AppRun run = runApp(retroverseExe(), room.dir, "steps", roomArguments(170, room.dir / "steps.png", script));
    const rv::app::InteractionRecord& record = requireInteraction(run);
    INFO(run.describe());
    const auto steps = eventsOf(record, "step");
    REQUIRE_FALSE(steps.empty());
    const std::uint64_t lastStep = steps.back().frame;
    CHECK(lastStep >= 120);
    REQUIRE(record.focusedGame.size() > 0);
    CHECK(run.report->video.gameId == record.focusedGame);

    // Up to 14 frames (233 ms) after the last step: the inspected item's clip still, one selection fewer.
    const AppRun early = runApp(retroverseExe(), room.dir, "early",
                                roomArguments(static_cast<std::uint32_t>(lastStep + 15), room.dir / "early.png",
                                              script));
    const rv::app::InteractionRecord& before = requireInteraction(early);
    INFO(early.describe());
    CHECK(before.focusedGame == record.focusedGame);
    CHECK(early.report->video.gameId == item.game.toString());
    CHECK(early.report->video.selections + 1 == run.report->video.selections);
    CHECK(run.report->video.maxDecoderThreads <= 1);
}
