// The editor in the app (I4b step 3; ADR-0015 §4, §6): the building fixture's room:snes (tests/fixtures/esde/building,
// planned with retroverse-cli), retroverse.exe --start-room room:snes, lockstep (--fixed-dt), hidden, driven by
// tests/input_scripts/world_edit_session.json from the Focus pose of a display case's game A whose left neighbour slot is
// free: the directory's "Modifica stanza", A picked up and moved into the free slot (its dressing giving way: the floor
// built again from the draft), A's old slot then a game B targeted with the D-pad, B picked up and swapped with the game
// C above it (laid out again in place), the menu, Salva. The run report's world.edits record the session; the stored plan
// holds A in the free slot and B and C swapped, the overrides their three pins (origin editor), the snapshot of the
// action "edit" is there; a restart finds the plan as saved (nothing written). A session discarded with "Esci senza
// salvare" (world_edit_discard.json) leaves the plan's SHA-256 unchanged and shows the stored plan again. "Sposta" on
// the card of a cartridge of the room's shelf takes it into the editor; the walker carries it across the room and the
// reticle drops it into a display case's free slot; Salva (a script the test writes: the walk and the looks that reach
// the slot depend on the plan). Screenshots of the bar: a game in the hand over the free slot, the menu
// (build/smoke/world_edit/session/*.png).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
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
#include "support/scoped_environment.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The window's size: the UI is laid out for it and drawn into the screenshot scaled uniformly.
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};
constexpr std::string_view kRoom = "room:snes";
/// The walker's eye above the feet and the mouse's look per pixel (scene::FirstPersonSettings' defaults).
constexpr float kEyeHeight = 1.65f;
constexpr float kRadiansPerPixel = 0.0022f;

fs::path buildAssets() {
    return buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
}

/// The slot is on a display case (a fixture of the role Display, the display case's prefab).
bool onDisplayCase(const rv::world::WorldPlan& plan, const rv::world::SlotId& slot) {
    return std::any_of(plan.fixtures.begin(), plan.fixtures.end(), [&](const rv::world::Fixture& f) {
        return f.id == slot.fixture() && f.role == rv::world::FixtureRole::Display &&
               f.prefab.manifest.find("display_case") != std::string::npos;
    });
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
    for (const rv::world::CollectionItem& item : layout->items) {
        if (item.stance != rv::world::ItemStance::FaceOut || !onDisplayCase(stored.plan, item.slot)) {
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

/// "Sposta"'s session: a cartridge A on the room's shelf (fs01 beside the hero corner: room:snes's titles all stand
/// face-out, so the card shows at once) and a display case's free slot F, 5 m apart in the middle of the room. The
/// walker starts where the reticle rests on A (`spawn`); with A in the hand it turns by `toWalk` mouse pixels (x right,
/// y up), walks `walk` metres straight to F's Focus pose (`walkFrames` frames at `walkAxis` of the walking speed,
/// nothing in the way), and turns by `toFree` onto F: the same picks and poses the app makes in that room.
struct CardTargets {
    rv::world::CollectionItem a;
    rv::world::FreeSlot free;
    rv::app::SpawnPoint spawn;
    glm::vec2 toWalk{0.0f};
    float walk = 0.0f;
    int walkFrames = 0;
    float walkAxis = 0.0f;
    glm::vec2 toFree{0.0f};
};

/// The yaw and pitch that look along `direction` (scene::Camera's conventions: yaw 0 towards -Z, positive to the right
/// and up).
glm::vec2 lookAlong(const glm::vec3& direction) {
    return {std::atan2(direction.x, -direction.z), std::asin(std::clamp(direction.y, -1.0f, 1.0f))};
}

/// `to - from` in mouse pixels, the yaw's difference taken the short way round.
glm::vec2 turnPixels(glm::vec2 from, glm::vec2 to) {
    glm::vec2 turn = to - from;
    while (turn.x > std::numbers::pi_v<float>) {
        turn.x -= 2.0f * std::numbers::pi_v<float>;
    }
    while (turn.x <= -std::numbers::pi_v<float>) {
        turn.x += 2.0f * std::numbers::pi_v<float>;
    }
    return turn / kRadiansPerPixel;
}

CardTargets cardTargets(const PlannedBuilding& building) {
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
    rv::app::FocusViewer viewer;
    viewer.lens = scene->camera.lens;
    viewer.aspect = kWindowSize.x / kWindowSize.y;
    // Where the walker may stand: its capsule (0.3 m, a margin) clear of every occluder (walls, fixtures, props) and of
    // every slot (a display case's glass is no occluder but stops the walker).
    std::vector<rv::core::Aabb> solids = layout->occluders;
    for (const rv::world::CollectionItem& item : layout->items) {
        solids.push_back(item.worldBounds());
    }
    for (const rv::world::FreeSlot& free : layout->freeSlots) {
        solids.push_back(free.worldBounds());
    }
    constexpr float kClear = 0.45f;
    const auto standable = [&](const glm::vec3& feet) {
        return std::none_of(solids.begin(), solids.end(), [&](const rv::core::Aabb& box) {
            return box.max.y > feet.y + 0.05f && box.min.y < feet.y + 1.8f && feet.x > box.min.x - kClear &&
                   feet.x < box.max.x + kClear && feet.z > box.min.z - kClear && feet.z < box.max.z + kClear;
        });
    };
    const auto clearWalk = [&](const glm::vec3& from, const glm::vec3& to) {
        const int steps = static_cast<int>(std::ceil(glm::length(to - from) / 0.05f));
        for (int step = 0; step <= steps; ++step) {
            if (!standable(glm::mix(from, to, static_cast<float>(step) / static_cast<float>(std::max(steps, 1))))) {
                return false;
            }
        }
        return true;
    };
    // The reticle on the target from the eye: the hover's own sample (on its core), as the app's index gives it.
    const auto seen = [&](const glm::vec3& eye, const glm::vec3& at, rv::world::PickId id) {
        const glm::vec3 to = at - eye;
        if (glm::length(to) > rv::world::kMaxPickDistance - 0.3f) {
            return false;
        }
        const rv::world::HoverSample sample =
            targets.index().sample(rv::core::Ray{eye, glm::normalize(to)}, rv::world::kNoPick);
        return sample.hit == id && sample.onCore;
    };
    const glm::vec3 up(0.0f, kEyeHeight, 0.0f);
    std::optional<CardTargets> best;
    for (const rv::world::FreeSlot& free : layout->freeSlots) {
        const std::optional<rv::scene::WalkerPose> there = targets.focusPose(free.pickId, viewer);
        if (!onDisplayCase(stored.plan, free.slot) || !there || !standable(there->feet) ||
            !seen(there->feet + up, free.worldBounds().center(), free.pickId)) {
            continue;
        }
        for (const rv::world::CollectionItem& item : layout->items) {
            if (onDisplayCase(stored.plan, item.slot)) {
                continue;
            }
            const glm::vec3 aAt = item.worldBounds().center();
            for (float x = -4.5f; x <= 4.5f; x += 0.1f) {
                for (float z = -5.0f; z <= 5.0f; z += 0.1f) {
                    const glm::vec3 feet(x, there->feet.y, z);
                    const float walk = glm::length(there->feet - feet);
                    if ((best && walk >= best->walk) || walk < 0.5f || !seen(feet + up, aAt, item.pickId) ||
                        !clearWalk(feet, there->feet)) {
                        continue;
                    }
                    const glm::vec2 toA = lookAlong(glm::normalize(aAt - feet - up));
                    const glm::vec2 heading(lookAlong(glm::normalize(there->feet - feet)).x, toA.y);
                    CardTargets found{item, free, {}, turnPixels(toA, heading), walk, 0, 0.0f,
                                      turnPixels(heading, glm::vec2(there->yaw, there->pitch))};
                    // At most 80% of the walking speed (2.5 m/s), frames of 1/60 s, the distance exact.
                    found.walkFrames = static_cast<int>(std::ceil(walk * 60.0f / (2.5f * 0.8f)));
                    found.walkAxis = walk * 60.0f / (2.5f * static_cast<float>(found.walkFrames));
                    found.spawn.feet = feet;
                    found.spawn.yawDeg = rv::core::radiansToDegrees(toA.x);
                    found.spawn.pitchDeg = rv::core::radiansToDegrees(toA.y);
                    best = found;
                }
            }
        }
    }
    INFO("a place in " << kRoom << " that sees a shelf's cartridge with a clear walk to a display case's free slot");
    REQUIRE(best.has_value());
    return *best;
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
    // The room shows the stored plan again: its digest (never the draft's), and the free slot the walker still faces
    // is free again (A, moved there in the draft, is back in its own slot: the reticle rests on nothing).
    CHECK(edit.planDigest == before);
    CHECK(run.report->world.hoveredGame != targets.a.game.toString());
    CHECK(run.report->world.hoveredGame.empty());
}

TEST_CASE("world_edit: \"Sposta\" on the card takes a shelf's cartridge into the editor and onto a display case",
          "[world][edit]") {
    const PlannedBuilding building("world_edit/card");
    const CardTargets targets = cardTargets(building);
    const rv::world::StoredPlan before = building.plan();
    INFO("A " << targets.a.slot.toString() << ", the display case's free slot " << targets.free.slot.toString()
              << ", a walk of " << targets.walk << " m in " << targets.walkFrames << " frames");
    // frame 40 interact: Inspect (a face-out cartridge: at once), the card beside A with Avvia focused
    // frames 70, 76 ui_down: Informazioni, Sposta      frame 82 ui_activate: Sposta: the editor, A in the hand
    // frame 100 the view turns to the walk's heading   frames 101 on: the walk to F's Focus pose
    // `turn` the view onto F                           `turn` + 20 interact: A moved there
    // `turn` + 50 Enter (confirm and ui_activate): the menu, Salva focused
    // `turn` + 70 ui_activate: Salva
    const int turn = 101 + targets.walkFrames + 5;
    const fs::path script = building.dir / "world_edit_card.json";
    {
        std::ofstream out(script, std::ios::binary);
        const auto at = [&](int frame, const std::string& actions, bool last = false) {
            out << "  {\"fromFrame\": " << frame << ", \"toFrame\": " << frame << ", \"actions\": {" << actions
                << "}}" << (last ? "" : ",") << "\n";
        };
        out << "[\n";
        at(40, R"("interact": 1)");
        at(70, R"("ui_down": 1)");
        at(76, R"("ui_down": 1)");
        at(82, R"("ui_activate": 1)");
        out << "  {\"fromFrame\": 100, \"toFrame\": 100, \"actions\": {\"look_delta_x\": " << targets.toWalk.x
            << ", \"look_delta_y\": " << targets.toWalk.y << "}},\n";
        out << "  {\"fromFrame\": 101, \"toFrame\": " << 100 + targets.walkFrames
            << ", \"actions\": {\"move_forward\": " << targets.walkAxis << "}},\n";
        out << "  {\"fromFrame\": " << turn << ", \"toFrame\": " << turn << ", \"actions\": {\"look_delta_x\": "
            << targets.toFree.x << ", \"look_delta_y\": " << targets.toFree.y << "}},\n";
        at(turn + 20, R"("interact": 1)");
        at(turn + 50, R"("confirm": 1, "ui_activate": 1)");
        at(turn + 70, R"("ui_activate": 1)", true);
        out << "]\n";
    }
    const fs::path shot = building.dir / "card.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "card",
                              {"--start-room", std::string(kRoom), "--exit-after-frames", "--frames", std::to_string(turn + 110),
                               "--fixed-dt", "0.016667", "--spawn", spawnArgument(targets.spawn), "--input-script",
                               arg(script), "--screenshot", arg(shot)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.room == kRoom);
    CHECK(edit.via == "card");
    CHECK(edit.heldAtStart == targets.a.game.toString());
    CHECK(edit.failed.empty());
    CHECK(edit.end == "saved");
    CHECK(edit.written);
    REQUIRE(edit.operations.size() == 1);
    CHECK(edit.operations.front().operation ==
          "move " + targets.a.game.toString() + " to " + targets.free.slot.toString());
    CHECK(edit.operations.front().refused.empty());
    // The stored plan: A on the display case, pinned there by the editor.
    const rv::world::StoredPlan after = building.plan();
    CHECK(after.digest != before.digest);
    CHECK(edit.planDigest == after.digest);
    REQUIRE(placementOf(after.plan, targets.a.game) != nullptr);
    CHECK(placementOf(after.plan, targets.a.game)->slot == targets.free.slot);
    CHECK(onDisplayCase(after.plan, placementOf(after.plan, targets.a.game)->slot));
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

// ADR-0015 §9's row for a move into a dressed slot (the floor built again from the draft: I4b step 3's review), on the
// real library's computer floor, measured by hand in Release (hidden: [.real-library]): only through
// scripts/real-library.ps1 (ES-DE closed, read-only snapshots of the library before and after), its -Measure script
// running rv_smoke_tests.exe "[real-library][edit]" with the RV_REAL_* variables set. The whole library is imported
// into a data directory under the build tree (build/<preset>/real-library/edit, emptied first), its building generated
// (retroverse-cli worldplan generate) and shown once (the covers into the cache); then, in the computer floor's room
// with the most placements that has a game beside a dressed free slot, a session moves that game into the slot and
// leaves with "Esci senza salvare" (nothing written): the move's job and display times printed.
TEST_CASE("world_edit: a move into a dressed slot on the real library's computer floor", "[.real-library][edit]") {
    const auto home = rv::test::environmentVariable(L"RV_REAL_ESDE_HOME");
    const auto install = rv::test::environmentVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = rv::test::environmentVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = rv::test::environmentVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    // Under the build tree, never in the library nor in %TEMP%.
    const fs::path dir = retroverseExe().parent_path().parent_path() / "real-library" / "edit";
    std::error_code error;
    fs::remove_all(dir, error);
    REQUIRE_FALSE(error);
    fs::create_directories(dir, error);
    REQUIRE_FALSE(error);
    const fs::path data = dir / "data";
    const auto cli = [&](std::string_view name, std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseExe().parent_path() / "retroverse-cli.exe", dir,
                                      std::string("cli_") + std::string(name), std::move(arguments));
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    };
    cli("library", {"library", "create", "--home", arg(fs::path(*home).parent_path()), "--install", arg(*install),
                    "--rom-root", arg(*romRoot), "--media-root", arg(*mediaRoot)});
    cli("import", {"import"});
    cli("generate", {"worldplan", "generate"});
    auto stored = rv::world::readStoredPlan(
        rv::world::worldFiles(data, rv::catalog::LibraryId::parse("esde").value()).plan);
    REQUIRE(stored.has_value());
    const rv::world::WorldPlan& plan = stored->plan;

    // The computer floor (else the floor with the most placements), its room with the most placements that has a game
    // beside a dressed free slot on the same fixture.
    std::map<std::string, std::size_t> placed;
    std::map<std::string, std::size_t> floorPlaced;
    for (const rv::world::Placement& placement : plan.placements) {
        ++placed[placement.slot.fixture().room().toString()];
    }
    for (const rv::world::Room& room : plan.rooms) {
        floorPlaced[room.floor] += placed[room.id.toString()];
    }
    REQUIRE_FALSE(floorPlaced.empty());
    const std::string floor =
        floorPlaced.contains("computer")
            ? std::string("computer")
            : std::max_element(floorPlaced.begin(), floorPlaced.end(), [](const auto& a, const auto& b) {
                  return a.second < b.second;
              })->first;
    std::vector<const rv::world::Room*> rooms;
    for (const rv::world::Room& room : plan.rooms) {
        if (room.floor == floor && placed[room.id.toString()] > 0) {
            rooms.push_back(&room);
        }
    }
    std::sort(rooms.begin(), rooms.end(), [&](const rv::world::Room* a, const rv::world::Room* b) {
        return placed[a->id.toString()] > placed[b->id.toString()];
    });
    std::optional<rv::app::SpawnPoint> spawn;
    std::string room;
    std::string direction;
    for (const rv::world::Room* candidate : rooms) {
        auto sceneData = rv::world::loadSceneData(buildAssets(), plan, candidate->id);
        REQUIRE(sceneData.has_value());
        auto scene = rv::world::toSceneDescription(plan, candidate->id, *sceneData);
        REQUIRE(scene.has_value());
        auto layout = rv::world::collectionLayout(plan, candidate->id, *sceneData, {});
        REQUIRE(layout.has_value());
        auto edit = rv::app::EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(*layout));
        REQUIRE(edit.has_value());
        for (const rv::world::CollectionItem& item : layout->items) {
            for (const auto& [step, name] : {std::pair{rv::world::StepDirection::Left, "ui_left"},
                                             std::pair{rv::world::StepDirection::Right, "ui_right"}}) {
                const auto next = (*edit)->step(item.pickId, step);
                const rv::world::FreeSlot* free = next ? (*edit)->freeSlot(*next) : nullptr;
                if (free == nullptr || !free->dressed || free->slot.fixture() != item.slot.fixture()) {
                    continue;
                }
                rv::app::FocusViewer viewer;
                viewer.lens = scene->camera.lens;
                viewer.aspect = kWindowSize.x / kWindowSize.y;
                const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
                spawn = rv::app::SpawnPoint{};
                spawn->feet = pose.feet;
                spawn->yawDeg = rv::core::radiansToDegrees(pose.yaw);
                spawn->pitchDeg = rv::core::radiansToDegrees(pose.pitch);
                direction = name;
                break;
            }
            if (spawn) {
                break;
            }
        }
        if (spawn) {
            room = candidate->id.toString();
            break;
        }
    }
    INFO("a game beside a dressed free slot on floor " << floor);
    REQUIRE(spawn.has_value());

    // The building shown once (its covers into the cache), then the session: "Modifica stanza", the game picked up,
    // the step onto the dressed slot, the move, "Esci senza salvare".
    const AppRun warm = runApp(retroverseExe(), dir, "warm",
                               {"--start-room", room, "--exit-after-frames", "--frames", "300", "--fixed-dt",
                                "0.016667", "--spawn", spawnArgument(*spawn)});
    skipWithoutOpenGl(warm);
    requireCleanRun(warm);
    const fs::path script = dir / "world_edit_real.json";
    {
        std::ofstream out(script, std::ios::binary);
        out << "[\n"
            << R"(  {"fromFrame": 30, "toFrame": 30, "actions": {"map": 1}},)" << "\n"
            << R"(  {"fromFrame": 36, "toFrame": 36, "actions": {"ui_home": 1}},)" << "\n"
            << R"(  {"fromFrame": 42, "toFrame": 42, "actions": {"ui_up": 1}},)" << "\n"
            << R"(  {"fromFrame": 48, "toFrame": 48, "actions": {"ui_activate": 1}},)" << "\n"
            << R"(  {"fromFrame": 70, "toFrame": 70, "actions": {"interact": 1}},)" << "\n"
            << R"(  {"fromFrame": 80, "toFrame": 80, "actions": {")" << direction << R"(": 1}},)" << "\n"
            << R"(  {"fromFrame": 110, "toFrame": 110, "actions": {"interact": 1}},)" << "\n"
            << R"(  {"fromFrame": 140, "toFrame": 140, "actions": {"confirm": 1}},)" << "\n"
            << R"(  {"fromFrame": 150, "toFrame": 150, "actions": {"ui_down": 1}},)" << "\n"
            << R"(  {"fromFrame": 160, "toFrame": 160, "actions": {"ui_down": 1}},)" << "\n"
            << R"(  {"fromFrame": 170, "toFrame": 170, "actions": {"ui_activate": 1}})" << "\n"
            << "]\n";
    }
    const std::string before = stored->digest;
    const AppRun run = runApp(retroverseExe(), dir, "session",
                              {"--start-room", room, "--exit-after-frames", "--frames", "220", "--fixed-dt",
                               "0.016667", "--spawn", spawnArgument(*spawn), "--input-script", arg(script)});
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    REQUIRE(edit.operations.size() == 1);
    const rv::app::EditOperationRecord& move = edit.operations.front();
    WARN("Real edit: " << room << " (" << placed[room] << " placed) on floor " << floor << " ("
                       << floorPlaced[floor] << " placed, " << plan.placements.size()
                       << " in the building): the session ready in " << edit.prepareMs.value_or(-1.0) << " ms; "
                       << move.operation << ": " << move.rebuild << ", " << move.path << ", the job "
                       << move.editMs.value_or(-1.0) << " ms, shown " << move.shownMs.value_or(-1.0)
                       << " ms after the request; Esci senza salvare's room back in " << edit.backMs.value_or(-1.0)
                       << " ms");
    CHECK(move.refused.empty());
    CHECK(move.path == "floor");
    CHECK(edit.end == "discarded");
    CHECK_FALSE(edit.written);
    auto after = rv::world::readStoredPlan(
        rv::world::worldFiles(data, rv::catalog::LibraryId::parse("esde").value()).plan);
    REQUIRE(after.has_value());
    CHECK(after->digest == before);
}
