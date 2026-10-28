// The editor in the app (I4b step 3; ADR-0015 §4, §6): the building fixture's room:snes (tests/fixtures/esde/building,
// planned with retroverse-cli), retroverse.exe --start-room room:snes, lockstep (--fixed-dt), hidden, driven by
// tests/input_scripts/world_edit_session.json from the Focus pose of a display case's game A whose left neighbour slot is
// free: the directory's "Modifica stanza", A picked up and moved into the free slot (its dressing giving way: the floor
// built again from the draft), A's old slot then a game B targeted with the D-pad, B picked up and swapped with the game
// C above it (laid out again in place), the menu, Salva. The run report's world.edits record the session; the stored plan
// holds A in the free slot and B and C swapped, the overrides their three pins (origin editor), the snapshot of the
// action "edit" is there; a restart finds the plan as saved (nothing written). A session discarded with "Esci senza
// salvare" (world_edit_discard.json) leaves the plan's SHA-256 unchanged. Screenshots of the bar: a game in the hand over
// the free slot, the menu (build/smoke/world_edit/session/*.png).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "building_support.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The window's size: the UI is laid out for it and drawn into the screenshot scaled uniformly.
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};
constexpr std::string_view kRoom = "room:snes";

fs::path buildAssets() {
    return buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
}

/// The games and slots the session's script reaches from the plan as planned: A on a display case with a free slot on
/// its left, the game B two steps right of that slot (A's own slot between), the game C one step up from B.
struct SessionTargets {
    rv::world::CollectionItem a;
    rv::world::FreeSlot free;
    rv::world::CollectionItem b;
    rv::world::CollectionItem c;
    rv::app::SpawnPoint spawn;
};

SessionTargets sessionTargets(const PlannedBuilding& building) {
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::RoomId room = rv::world::RoomId::parse(kRoom).value();
    auto data = rv::world::loadSceneData(buildAssets(), stored.plan, room);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored.plan, room, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored.plan, room, *data, {});
    REQUIRE(layout.has_value());
    auto edit = rv::app::EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(*layout));
    REQUIRE(edit.has_value());
    const rv::app::EditRoom& targets = **edit;
    std::optional<SessionTargets> found;
    const auto onDisplayCase = [&](const rv::world::SlotId& slot) {
        return std::any_of(stored.plan.fixtures.begin(), stored.plan.fixtures.end(), [&](const rv::world::Fixture& f) {
            return f.id == slot.fixture() && f.role == rv::world::FixtureRole::Display &&
                   f.prefab.manifest.find("display_case") != std::string::npos;
        });
    };
    for (const rv::world::CollectionItem& item : layout->items) {
        if (item.stance != rv::world::ItemStance::FaceOut || !onDisplayCase(item.slot)) {
            continue;
        }
        // The free slot on the same display case.
        const auto left = targets.step(item.pickId, rv::world::StepDirection::Left);
        if (!left || targets.freeSlot(*left) == nullptr ||
            targets.freeSlot(*left)->slot.fixture() != item.slot.fixture() ||
            targets.step(*left, rv::world::StepDirection::Right) != item.pickId) {
            continue;
        }
        const auto right = targets.step(item.pickId, rv::world::StepDirection::Right);
        if (!right || targets.item(*right) == nullptr) {
            continue;
        }
        const auto up = targets.step(*right, rv::world::StepDirection::Up);
        if (!up || targets.item(*up) == nullptr || *up == item.pickId) {
            continue;
        }
        found = SessionTargets{item, *targets.freeSlot(*left), *targets.item(*right), *targets.item(*up), {}};
        rv::app::FocusViewer viewer;
        viewer.lens = scene->camera.lens;
        viewer.aspect = kWindowSize.x / kWindowSize.y;
        const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
        found->spawn.feet = pose.feet;
        found->spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
        found->spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
        break;
    }
    INFO("a display case game with a free slot on its left in " << kRoom);
    REQUIRE(found.has_value());
    return *found;
}

std::vector<std::string> sessionArguments(const SessionTargets& targets, std::uint32_t frames, std::string_view script,
                                          const std::optional<fs::path>& shot) {
    std::vector<std::string> arguments{"--start-room",
                                       std::string(kRoom),
                                       "--exit-after-frames",
                                       "--frames",
                                       std::to_string(frames),
                                       "--fixed-dt",
                                       "0.016667",
                                       "--spawn",
                                       spawnArgument(targets.spawn),
                                       "--input-script",
                                       arg(sourceDir() / "tests" / "input_scripts" / std::string(script))};
    if (shot) {
        arguments.insert(arguments.end(), {"--screenshot", arg(*shot)});
    }
    return arguments;
}

const rv::world::Placement* placementOf(const rv::world::WorldPlan& plan, const rv::catalog::GameId& game) {
    const auto found = std::find_if(plan.placements.begin(), plan.placements.end(),
                                    [&](const rv::world::Placement& placement) { return placement.game == game; });
    return found != plan.placements.end() ? &*found : nullptr;
}

rv::world::WorldFiles worldFilesOf(const PlannedBuilding& building) {
    return rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse("esde").value());
}

const rv::app::WorldEditRecord& onlySession(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.state == "room");
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.room == kRoom);
    CHECK(edit.via == "directory");
    CHECK(edit.failed.empty());
    REQUIRE(edit.readyFrame.has_value());
    return edit;
}

} // namespace

TEST_CASE("world_edit: a scripted session moves a cartridge into a display case's free slot and swaps two, saves, "
          "and a restart finds them there",
          "[world][edit]") {
    const PlannedBuilding building("world_edit/session");
    const SessionTargets targets = sessionTargets(building);
    const rv::world::StoredPlan before = building.plan();
    INFO("A " << targets.a.slot.toString() << ", free " << targets.free.slot.toString() << ", B "
              << targets.b.slot.toString() << ", C " << targets.c.slot.toString());

    // The directory with "Modifica stanza" focused above the floors, the bar with A in the hand over the free slot
    // (where it would stand outlined), then with the menu open: runs that stop before the save, so the world stays as
    // planned.
    const fs::path directory = building.dir / "directory.png";
    const AppRun directoryRun = runApp(retroverseExe(), building.dir, "directory",
                                       sessionArguments(targets, 46, "world_edit_session.json", directory));
    skipWithoutOpenGl(directoryRun);
    requireCleanRun(directoryRun);
    static_cast<void>(requireScreenshot(directoryRun, directory, kShotSize));
    CHECK(directoryRun.report->world.edits.empty());
    const fs::path holding = building.dir / "holding.png";
    const AppRun holdingRun =
        runApp(retroverseExe(), building.dir, "holding", sessionArguments(targets, 105, "world_edit_session.json", holding));
    {
        requireCleanRun(holdingRun);
        static_cast<void>(requireScreenshot(holdingRun, holding, kShotSize));
        REQUIRE(holdingRun.report->world.edits.size() == 1);
        const rv::app::WorldEditRecord& open = holdingRun.report->world.edits.front();
        CHECK(open.end.empty()); // still open at the run's end
        CHECK(open.operations.empty());
    }
    const fs::path menu = building.dir / "menu.png";
    const AppRun menuRun =
        runApp(retroverseExe(), building.dir, "menu", sessionArguments(targets, 280, "world_edit_session.json", menu));
    {
        requireCleanRun(menuRun);
        static_cast<void>(requireScreenshot(menuRun, menu, kShotSize));
        REQUIRE(menuRun.report->world.edits.size() == 1);
        CHECK(menuRun.report->world.edits.front().menus == 1);
        CHECK(menuRun.report->world.edits.front().operations.size() == 2);
    }
    CHECK(building.plan().digest == before.digest);

    // The whole session, saved.
    const fs::path end = building.dir / "session.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "session", sessionArguments(targets, 320, "world_edit_session.json", end));
    const rv::app::WorldEditRecord& edit = onlySession(run);
    static_cast<void>(requireScreenshot(run, end, kShotSize));
    CHECK(edit.end == "saved");
    CHECK(edit.written);
    CHECK_FALSE(edit.replayed);
    CHECK(edit.saveRefused.empty());
    CHECK(edit.menus == 1);
    REQUIRE(edit.operations.size() == 2);
    const rv::app::EditOperationRecord& move = edit.operations[0];
    CHECK(move.op == "move");
    CHECK(move.operation == "move " + targets.a.game.toString() + " to " + targets.free.slot.toString());
    CHECK(move.refused.empty());
    // A dressed slot: the dressing gives way, the floor built again from the draft.
    CHECK(move.rebuild == "local-reflow");
    CHECK(move.path == "floor");
    CHECK(move.shownFrame.has_value());
    const rv::app::EditOperationRecord& swap = edit.operations[1];
    CHECK(swap.op == "swap");
    CHECK(swap.operation.find(targets.b.game.toString()) != std::string::npos);
    CHECK(swap.operation.find(targets.c.game.toString()) != std::string::npos);
    CHECK(swap.refused.empty());
    CHECK(swap.rebuild == "no-rebuild");
    CHECK(swap.path == "in-place");
    CHECK(swap.shownFrame.has_value());
    REQUIRE(move.shownMs.has_value());
    REQUIRE(swap.shownMs.has_value());
    UNSCOPED_INFO("the move shown in " << *move.shownMs << " ms, the swap in " << *swap.shownMs << " ms");

    // The stored world: A in the free slot, B and C swapped, three pins of the editor, the snapshot of the action.
    const rv::world::StoredPlan after = building.plan();
    CHECK(after.digest != before.digest);
    CHECK(edit.planDigest == after.digest);
    REQUIRE(placementOf(after.plan, targets.a.game) != nullptr);
    CHECK(placementOf(after.plan, targets.a.game)->slot == targets.free.slot);
    CHECK(placementOf(after.plan, targets.b.game)->slot == targets.c.slot);
    CHECK(placementOf(after.plan, targets.c.game)->slot == targets.b.slot);
    const rv::world::WorldFiles files = worldFilesOf(building);
    auto overrides = rv::world::loadOverrides(files.overrides);
    REQUIRE(overrides.has_value());
    REQUIRE(overrides->gamePins.size() == 3);
    for (const rv::world::GamePin& pin : overrides->gamePins) {
        CHECK(pin.provenance.origin == rv::world::PinOrigin::Editor);
        CHECK(placementOf(after.plan, pin.game)->slot == pin.slot);
    }
    REQUIRE_FALSE(edit.snapshot.empty());
    CHECK(fs::is_directory(files.snapshots / edit.snapshot));

    // A restart finds them there: the building's update of the unchanged catalog writes nothing.
    const AppRun restart = runApp(retroverseExe(), building.dir, "restart",
                                  {"--start-room", std::string(kRoom), "--exit-after-frames", "--frames", "5",
                                   "--fixed-dt", "0.016667"});
    requireCleanRun(restart);
    INFO(restart.describe());
    CHECK(restart.report->world.state == "room");
    CHECK_FALSE(restart.report->world.planWritten);
    CHECK(restart.report->world.planDigest == after.digest);
    CHECK(restart.report->world.edits.empty());
    CHECK(building.plan().digest == after.digest);
}

TEST_CASE("world_edit: a session left with \"Esci senza salvare\" leaves the plan's SHA-256 unchanged", "[world][edit]") {
    const PlannedBuilding building("world_edit/discard");
    const SessionTargets targets = sessionTargets(building);
    const std::string before = building.plan().digest;
    const fs::path shot = building.dir / "discarded.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "discard", sessionArguments(targets, 220, "world_edit_discard.json", shot));
    skipWithoutOpenGl(run);
    const rv::app::WorldEditRecord& edit = onlySession(run);
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
    CHECK(edit.end == "discarded");
    CHECK_FALSE(edit.written);
    CHECK(edit.snapshot.empty());
    REQUIRE(edit.operations.size() == 1);
    CHECK(edit.operations.front().path == "floor");
    REQUIRE(edit.backMs.has_value());
    CHECK(building.plan().digest == before);
    const rv::world::WorldFiles files = worldFilesOf(building);
    CHECK_FALSE(fs::exists(files.overrides));
    CHECK(run.report->world.planDigest == before);
}

// ADR-0015 §9's row (an edit shown within 0.25 s), measured by hand in Release (hidden: [.bench]): the overflow
// fixture's amiga room (1,561 big boxes, the size of the reference library's largest room), two swaps and a pin through
// the editor (tests/input_scripts/world_edit_bench.json), each one's time from its request to the frame that shows it
// (world.edits' shownMs) printed. Run: rv_smoke_tests.exe "[.bench][edit]" -s (build/i4b/step3's measurement).
TEST_CASE("world_edit: an edit's display time in a room of 1,561 big boxes", "[.bench][edit]") {
    const PlannedBuilding building("world_edit/bench", true, "esde/overflow");
    const rv::world::StoredPlan stored = building.plan();
    const std::string room = building.roomOf("amiga");
    const rv::world::RoomId roomId = rv::world::RoomId::parse(room).value();
    auto data = rv::world::loadSceneData(buildAssets(), stored.plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored.plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored.plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    auto targets = rv::app::EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(*layout));
    REQUIRE(targets.has_value());
    // A spine-out box with a box on its right, and a box below that one.
    std::optional<rv::world::CollectionItem> a;
    for (const rv::world::CollectionItem& item : layout->items) {
        if (item.stance != rv::world::ItemStance::SpineOut) {
            continue;
        }
        const auto right = (*targets)->step(item.pickId, rv::world::StepDirection::Right);
        const auto down = right ? (*targets)->step(*right, rv::world::StepDirection::Down) : std::nullopt;
        if (right && down && (*targets)->item(*right) != nullptr && (*targets)->item(*down) != nullptr &&
            item.slot.index() > 4) {
            a = item;
            break;
        }
    }
    REQUIRE(a.has_value());
    rv::app::FocusViewer viewer;
    viewer.lens = scene->camera.lens;
    viewer.aspect = kWindowSize.x / kWindowSize.y;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(*a, viewer);
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    const fs::path shot = building.dir / "bench.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "bench",
                              {"--start-room", room, "--exit-after-frames", "--frames", "300", "--fixed-dt",
                               "0.016667", "--spawn", spawnArgument(spawn), "--input-script",
                               arg(sourceDir() / "tests" / "input_scripts" / "world_edit_bench.json"), "--screenshot",
                               arg(shot)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    WARN("the room's data and floor read in " << edit.prepareMs.value_or(-1.0) << " ms; saved in "
                                              << edit.saveMs.value_or(-1.0) << " ms, the room back "
                                              << edit.backMs.value_or(-1.0) << " ms after Salva");
    for (const rv::app::EditOperationRecord& operation : edit.operations) {
        WARN(operation.operation << ": " << operation.path << ", the job " << operation.editMs.value_or(-1.0)
                                 << " ms, shown " << operation.shownMs.value_or(-1.0) << " ms after the request");
    }
    CHECK(edit.end == "saved");
    CHECK(edit.operations.size() == 3);
}
