// The hover, the pull-out and Focus in the generated room (I3 step 13a; ADR-0012 §1-§3): the snes-room fixture
// (tests/fixtures/esde/snes-room: 50 invented titles with generated covers, every one face-out) imported into the
// case's data directory, retroverse.exe without --scene, lockstep (--fixed-dt: the room and its layers before the first
// frame), hidden, driven by input scripts (tests/input_scripts/world_*.json) from walker poses computed from the plan's
// layout as retroverse.exe makes it. The checks read the run report's world.interaction record (the hovered and focused
// pick ids, its events by frame, the frames an item was off its static pose and whether its static records were drawn
// meanwhile, the "animation" phase) and the screenshots (the outline's pixels around the item; the label and the pose
// reviewed by eye): the hover within 6 frames, the pull-out after the dwell and back when the hover leaves; Focus and
// the D-pad steps along the band, across into the wall shelf, between its ledges, held steps repeating, a still stick,
// Back to FreeRoam; a spine-out cartridge (one of G10's additions, imported again) sliding out with its cover turned to
// the viewer and entering Focus; everything instant under the world's reduceMotion; the cursor's ray with the developer
// overlay open (developer builds).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/app/frame_timing.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/core/math.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/slot_navigation.hpp>
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

/// The snes-room fixture recorded as the library of <caseDir>/data and imported with retroverse-cli (as
/// test_world_room.cpp's).
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("world_focus/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    /// One of G10's additions, its ROM and its cover, imported: the plan's next update places it spine-out (a new title
    /// enters the spine-out reserve, ADR-0010 §3).
    void addTitle() const {
        for (const std::string_view file :
             {"roms/snes/Zinnia Zephyr (USA).sfc", "media/snes/covers/Zinnia Zephyr (USA).png"}) {
            std::error_code error;
            fs::copy_file(fixture.path(std::string("additions/") + std::string(file)), fixture.path(file), error);
            INFO(file << ": " << error.message());
            REQUIRE_FALSE(error);
        }
        cli("import_again", {"import"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }
};

/// A lockstep run of the room: `frames` frames at a fixed step, a screenshot at the end.
std::vector<std::string> roomArguments(std::uint32_t frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames",     std::to_string(frames), "--fixed-dt",
                                       "0.016667",            "--screenshot", arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The room's collection as retroverse.exe laid it out from the plan the run wrote (the assets of its build).
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

/// The item in `slot` ("fx:hw-w/s:5.4").
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

/// The walker at `item`'s Focus pose (as retroverse.exe computes it: the walker's floor and eye, the default lens and
/// window), looking `pitchOffsetDeg` higher, as --spawn takes it; and the camera of the pose itself.
struct Pose {
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
};

Pose focusPose(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens,
               float pitchOffsetDeg = 0.0f) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = 1280.0f / 720.0f;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    Pose result;
    result.spawn.feet = pose.feet;
    result.spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    result.spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch) + pitchOffsetDeg;
    result.camera = lens;
    result.camera.position = pose.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    result.camera.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    result.camera.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    return result;
}

/// The events of `kind` (and of `pickId` when given), in order.
std::vector<rv::app::InteractionEventRecord> eventsOf(const rv::app::InteractionRecord& record, std::string_view kind,
                                                      const std::string& pickId = {}) {
    std::vector<rv::app::InteractionEventRecord> found;
    for (const rv::app::InteractionEventRecord& event : record.events) {
        if (event.event == kind && (pickId.empty() || event.pickId == pickId)) {
            found.push_back(event);
        }
    }
    return found;
}

/// The first frame of an event, or nothing.
std::optional<std::uint64_t> firstFrame(const rv::app::InteractionRecord& record, std::string_view kind,
                                        const std::string& pickId = {}) {
    const auto events = eventsOf(record, kind, pickId);
    return events.empty() ? std::nullopt : std::optional<std::uint64_t>(events.front().frame);
}

/// The outline's pixels (#F4EFE6 exactly) in the square of `radius` around `centre`.
std::size_t outlinePixels(const rv::assets::ImageData& image, glm::ivec2 centre, int radius) {
    std::size_t count = 0;
    for (int y = std::max(0, centre.y - radius); y <= std::min(image.height - 1, centre.y + radius); ++y) {
        for (int x = std::max(0, centre.x - radius); x <= std::min(image.width - 1, centre.x + radius); ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(image.channels);
            if (image.pixels[at] == 0xF4 && image.pixels[at + 1] == 0xEF && image.pixels[at + 2] == 0xE6) {
                ++count;
            }
        }
    }
    return count;
}

/// What every run of these cases must report: a clean run of the room, its interaction bound, no static record drawn
/// while its item was off its pose.
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

/// The "animation" phase: its frames and their times (printed: the smoke runs share the machine with whatever else
/// runs; the budget is checked by the hidden bench below, in Release on a quiet machine).
rv::app::TimeSummary checkAnimationPhase(const AppRun& run) {
    const auto phase = std::find_if(run.report->phases.begin(), run.report->phases.end(), [](const auto& record) {
        return record.name == rv::app::kPhaseAnimation;
    });
    REQUIRE(phase != run.report->phases.end());
    CHECK(phase->frames > 0);
    REQUIRE(phase->times.has_value());
    WARN("animation phase: " << phase->frames << " frames, p50 " << phase->times->p50Ms << " ms, p99 "
                             << phase->times->p99Ms << " ms, max " << phase->times->maxMs << " ms");
    return *phase->times;
}

/// The D-pad step `direction` from `from` over the layout's navigation, or `from` when there is none.
rv::world::PickId stepOf(const rv::world::SlotNavigation& navigation, rv::world::PickId from,
                         rv::world::StepDirection direction) {
    return navigation.step(from, direction).value_or(from);
}

} // namespace

TEST_CASE("world focus: the hover outlines and labels a cartridge, pulls it out after its dwell and puts it back",
          "[gpu][world][world_focus]") {
    ImportedRoom room("hover");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.4");
    const std::string id = pickText(item.pickId);
    const Pose pose = focusPose(item, shown.camera, 25.0f);
    const std::vector<std::string> script{"--spawn", spawnArgument(pose.spawn), "--input-script",
                                          arg(inputScript("world_hover_pull.json"))};

    // Mid-way: hovered, outlined, labelled, pulled out (tipped towards the viewer).
    const fs::path pulledShot = room.dir / "pulled.png";
    const AppRun pulled = runApp(retroverseExe(), room.dir, "pulled", roomArguments(100, pulledShot, script));
    const rv::app::InteractionRecord& during = requireInteraction(pulled);
    INFO(pulled.describe());
    const auto hoverAt = firstFrame(during, "hover", id);
    REQUIRE(hoverAt.has_value());
    // Nothing hovered before the walker looks down (frame 40); the reticle on the middle of the item's face from frame
    // 50 at the latest: hovered within 6 frames (the hysteresis's 3).
    CHECK(eventsOf(during, "hover").size() == 1);
    CHECK(*hoverAt > 40);
    CHECK(*hoverAt <= 55);
    const auto pullAt = firstFrame(during, "pull-out", id);
    const auto outAt = firstFrame(during, "pulled", id);
    REQUIRE(pullAt.has_value());
    REQUIRE(outAt.has_value());
    // After the 150 ms dwell (9 frames at 60 fps), fully out 250 ms later (15 frames).
    CHECK(*pullAt >= *hoverAt + 8);
    CHECK(*pullAt <= *hoverAt + 10);
    CHECK(*outAt >= *pullAt + 14);
    CHECK(*outAt <= *pullAt + 16);
    CHECK(during.state == "free roam");
    CHECK(during.hoveredPickId == id);
    CHECK(during.highlightedPickId == id);
    CHECK(during.pulled == std::vector<std::string>{id});
    CHECK(during.pulledFrames > 0);
    CHECK(during.maxPulled == 1);
    CHECK_FALSE(during.label.empty());
    CHECK(during.labelFrames > 0);
    CHECK(pulled.report->world.hoveredPickId == id);
    const rv::assets::ImageData image = requireScreenshot(pulled, pulledShot, kShotSize);
    const std::optional<glm::ivec2> centre = projectToPixel(pose.camera, kShotSize, rv::app::itemFace(item).center);
    REQUIRE(centre.has_value());
    CHECK(outlinePixels(image, *centre, 260) >= 200);

    // Later: the hover left it (the walker looked up again), back in its slot, drawn by its static records again.
    const fs::path backShot = room.dir / "back.png";
    const AppRun back = runApp(retroverseExe(), room.dir, "back", roomArguments(180, backShot, script));
    const rv::app::InteractionRecord& after = requireInteraction(back);
    INFO(back.describe());
    const auto leftAt = eventsOf(after, "hover");
    REQUIRE(leftAt.size() >= 2);
    CHECK(leftAt.back().pickId.empty());
    CHECK(leftAt.size() == 2);
    CHECK(leftAt.back().frame > 120);
    CHECK(leftAt.back().frame <= 135);
    const auto inAt = firstFrame(after, "in", id);
    REQUIRE(inAt.has_value());
    // Back in 200 ms (12 frames) from where it was.
    CHECK(*inAt >= leftAt.back().frame + 11);
    CHECK(*inAt <= leftAt.back().frame + 13);
    CHECK(after.pulled.empty());
    CHECK(after.hoveredPickId.empty());
    CHECK(after.label.empty());
    CHECK(after.pullOuts == 1);
    // The run's frames identical to the shorter run's up to its end (the same script, the fixed step).
    REQUIRE(after.events.size() >= during.events.size());
    CHECK(std::vector(after.events.begin(), after.events.begin() + static_cast<std::ptrdiff_t>(during.events.size())) ==
          during.events);
    checkAnimationPhase(back);
    const rv::assets::ImageData backImage = requireScreenshot(back, backShot, kShotSize);
    CHECK(outlinePixels(backImage, kShotSize / 2, 960) == 0);
}

TEST_CASE("world focus: Focus on the band, D-pad steps along it, across into the wall shelf and between its ledges, "
          "Back to FreeRoam",
          "[gpu][world][world_focus]") {
    ImportedRoom room("steps");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& start = itemIn(shown, "fx:hw-w/s:5.4");
    auto navigation = rv::world::SlotNavigation::build(shown.layout.navigation);
    REQUIRE(navigation.has_value());
    using rv::world::StepDirection;
    // The steps the script asks for, as the layout's navigation answers them.
    struct Step {
        std::uint64_t frame;
        rv::world::PickId item;
    };
    std::vector<Step> expected;
    rv::world::PickId at = start.pickId;
    const auto step = [&](std::uint64_t frame, StepDirection direction) {
        const rv::world::PickId next = stepOf(*navigation, at, direction);
        if (next != at) {
            expected.push_back(Step{frame, next});
        }
        at = next;
    };
    step(170, StepDirection::Right);
    const rv::world::PickId band5 = at;
    CHECK(band5 == itemIn(shown, "fx:hw-w/s:5.5").pickId);
    step(200, StepDirection::Right);
    const rv::world::PickId shelf = at;
    CHECK(rv::app::CollectionRoom::sectionOf(shelf) != rv::app::CollectionRoom::sectionOf(band5));
    const rv::world::CollectionItem* shelfItem = shown.layout.find(shelf);
    REQUIRE(shelfItem != nullptr);
    CHECK(shelfItem->slot.fixture().localText() == "fx:fs01");
    step(240, StepDirection::Up);
    step(280, StepDirection::Down);
    step(320, StepDirection::Left);
    CHECK(at == band5);
    for (const std::uint64_t frame : {360u, 384u, 390u, 396u}) {
        step(frame, StepDirection::Left);
    }
    const std::vector<std::string> script{"--spawn", spawnArgument(focusPose(start, shown.camera).spawn),
                                          "--input-script", arg(inputScript("world_focus_steps.json"))};

    // In Focus on the wall shelf, its item tipped out, outlined and labelled.
    const fs::path focusShot = room.dir / "focus.png";
    const AppRun focus = runApp(retroverseExe(), room.dir, "focus", roomArguments(230, focusShot, script));
    const rv::app::InteractionRecord& mid = requireInteraction(focus);
    INFO(focus.describe());
    CHECK(mid.state == "focus");
    CHECK(mid.focusedPickId == pickText(shelf));
    CHECK(mid.highlightedPickId == pickText(shelf));
    CHECK_FALSE(mid.focusedGame.empty());
    CHECK(std::find(mid.pulled.begin(), mid.pulled.end(), pickText(shelf)) != mid.pulled.end());
    CHECK_FALSE(mid.label.empty());
    const rv::assets::ImageData focusImage = requireScreenshot(focus, focusShot, kShotSize);
    const Pose shelfPose = focusPose(*shelfItem, shown.camera);
    const std::optional<glm::ivec2> shelfCentre =
        projectToPixel(shelfPose.camera, kShotSize, rv::app::itemFace(*shelfItem).center);
    REQUIRE(shelfCentre.has_value());
    CHECK(outlinePixels(focusImage, *shelfCentre, 260) >= 200);

    // The whole script.
    const fs::path endShot = room.dir / "end.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "steps", roomArguments(480, endShot, script));
    const rv::app::InteractionRecord& record = requireInteraction(run);
    INFO(run.describe());
    // The states: Inspect at once on the face-out item (frame 40), Focus at Indietro (70), FreeRoam at Back (440).
    const auto states = eventsOf(record, "state");
    REQUIRE(states.size() == 3);
    CHECK(states[0].frame == 40);
    CHECK(states[0].state == "inspect");
    CHECK(states[1].frame == 70);
    CHECK(states[1].state == "focus");
    CHECK(states[2].frame == 440);
    CHECK(states[2].state == "free roam");
    // Every step on its frame, to the item the navigation names; no other change of the focus (the still stick of
    // frames 100-159 included).
    const auto steps = eventsOf(record, "step");
    REQUIRE(steps.size() == expected.size());
    for (std::size_t index = 0; index < steps.size(); ++index) {
        INFO("step " << index);
        CHECK(steps[index].frame == expected[index].frame);
        CHECK(steps[index].pickId == pickText(expected[index].item));
    }
    CHECK(record.steps == expected.size());
    CHECK(record.focusChanges == expected.size());
    CHECK(record.walkerEaseFrames > 0);
    // Back in FreeRoam: the item put back, every item in its slot; the hover on the last one, outlined.
    CHECK(record.state == "free roam");
    CHECK(record.focusedPickId.empty());
    CHECK(record.pulled.empty());
    CHECK(record.highlightedPickId == pickText(at));
    checkAnimationPhase(run);
    requireScreenshot(run, endShot, kShotSize);

    // Under the world's reduceMotion (world.json): the same steps, every transition instant (no ease, the pull-outs
    // whole on the frame they start), the dwell kept.
    {
        std::ofstream config(room.data / "config" / "world.json", std::ios::binary);
        config << "{\"reduceMotion\": true}\n";
    }
    const fs::path instantShot = room.dir / "instant.png";
    const AppRun instant = runApp(retroverseExe(), room.dir, "instant", roomArguments(480, instantShot, script));
    const rv::app::InteractionRecord& still = requireInteraction(instant);
    INFO(instant.describe());
    CHECK(instant.report->world.outcome == "unchanged"); // how the room moves, not what it holds
    CHECK(still.reduceMotion);
    CHECK(still.walkerEaseFrames == 0);
    CHECK(still.animationFrames == 0);
    const auto instantSteps = eventsOf(still, "step");
    REQUIRE(instantSteps.size() == expected.size());
    for (std::size_t index = 0; index < instantSteps.size(); ++index) {
        CHECK(instantSteps[index].frame == expected[index].frame);
        CHECK(instantSteps[index].pickId == pickText(expected[index].item));
    }
    for (const rv::app::InteractionEventRecord& pullOut : eventsOf(still, "pull-out")) {
        const auto out = eventsOf(still, "pulled", pullOut.pickId);
        CHECK(std::any_of(out.begin(), out.end(), [&](const auto& event) { return event.frame == pullOut.frame; }));
    }
    const auto firstHover = firstFrame(still, "hover", pickText(start.pickId));
    const auto firstPull = firstFrame(still, "pull-out", pickText(start.pickId));
    REQUIRE(firstHover.has_value());
    REQUIRE(firstPull.has_value());
    CHECK(*firstPull >= *firstHover + 8);
}

TEST_CASE("world focus: a spine-out cartridge slides out with its cover to the viewer, interact focuses it",
          "[gpu][world][world_focus]") {
    ImportedRoom room("spine");
    const AppRun before = runApp(retroverseExe(), room.dir, "before", roomArguments(2, room.dir / "before.png"));
    skipWithoutOpenGl(before);
    requireCleanRun(before);
    room.addTitle();
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    requireCleanRun(first);
    CHECK(first.report->world.outcome == "updated");
    CHECK(first.report->world.spine == 1);
    const ShownRoom shown = shownRoom(room, *first.report);
    const auto spine = std::find_if(shown.layout.items.begin(), shown.layout.items.end(), [](const auto& item) {
        return item.stance == rv::world::ItemStance::SpineOut;
    });
    REQUIRE(spine != shown.layout.items.end());
    const std::string id = pickText(spine->pickId);
    const Pose pose = focusPose(*spine, shown.camera);
    const std::vector<std::string> script{"--spawn", spawnArgument(pose.spawn), "--input-script",
                                          arg(inputScript("world_focus_enter.json"))};

    const fs::path focusShot = room.dir / "spine_focus.png";
    const AppRun focus = runApp(retroverseExe(), room.dir, "focus", roomArguments(70, focusShot, script));
    const rv::app::InteractionRecord& mid = requireInteraction(focus);
    INFO(focus.describe());
    CHECK(mid.state == "focus");
    CHECK(mid.focusedPickId == id);
    CHECK(mid.pulled == std::vector<std::string>{id});
    const auto states = eventsOf(mid, "state");
    REQUIRE(states.size() == 1);
    CHECK(states[0].frame == 40);
    CHECK(states[0].state == "focus");
    const rv::assets::ImageData image = requireScreenshot(focus, focusShot, kShotSize);
    const std::optional<glm::ivec2> centre = projectToPixel(pose.camera, kShotSize, rv::app::itemFace(*spine).center);
    REQUIRE(centre.has_value());
    CHECK(outlinePixels(image, *centre, 300) >= 200);

    const AppRun back =
        runApp(retroverseExe(), room.dir, "back", roomArguments(130, room.dir / "spine_back.png", script));
    const rv::app::InteractionRecord& after = requireInteraction(back);
    INFO(back.describe());
    CHECK(after.state == "free roam");
    // Put back while the hover rests on it: it stays in until the hover leaves and comes back.
    CHECK(after.pulled.empty());
    CHECK(after.hoveredPickId == id);
    CHECK(after.pullOuts == 1);
    checkAnimationPhase(back);
}

TEST_CASE("world focus: with the developer overlay open the cursor's ray picks", "[gpu][world][world_focus]") {
    if (!buildConfig().developerOverlay) {
        SKIP("this build has no developer overlay");
    }
    ImportedRoom room("cursor");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.3");
    // The mouse free for the overlay's panels: the ray goes through the cursor (frame-limited runs rest it at the
    // window's centre, never reading the user's mouse).
    const AppRun run = runApp(retroverseExe(), room.dir, "cursor",
                              roomArguments(20, room.dir / "cursor.png",
                                            {"--spawn", spawnArgument(focusPose(item, shown.camera).spawn),
                                             "--devui-open"}));
    const rv::app::InteractionRecord& record = requireInteraction(run);
    INFO(run.describe());
    CHECK(record.cursorFrames == 20);
    CHECK(record.hoveredPickId == pickText(item.pickId));
}

// Hidden ([.bench], run by hand in Release on a quiet machine: rv_smoke_tests "[world_focus][.bench]"): the frame
// budget during the animations (docs/roadmap.md, I3 budgets: frame p99 at most 33.3 ms over the animation phases,
// NFR-04), the steps script at 1920 x 1080 without vsync, every pull, push-back, lift and walker ease of it in the
// "animation" phase.
TEST_CASE("world focus: the frame budget during the animations", "[.bench][gpu][world][world_focus]") {
    ImportedRoom room("budget");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(2, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const ShownRoom shown = shownRoom(room, *first.report);
    const rv::world::CollectionItem& start = itemIn(shown, "fx:hw-w/s:5.4");
    const AppRun run = runApp(retroverseExe(), room.dir, "steps",
                              roomArguments(480, room.dir / "steps.png",
                                            {"--spawn", spawnArgument(focusPose(start, shown.camera).spawn),
                                             "--input-script", arg(inputScript("world_focus_steps.json")), "--no-vsync",
                                             "--width", "1920", "--height", "1080"}));
    requireInteraction(run);
    const rv::app::TimeSummary animation = checkAnimationPhase(run);
#if defined(NDEBUG)
    CHECK(animation.p99Ms <= 33.3);
#else
    static_cast<void>(animation);
#endif
}
