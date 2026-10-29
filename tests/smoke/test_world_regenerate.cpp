// Regeneration and undo in the app (I4b step 6; ADR-0015 §5, §6, §9): the building fixture's room:snes
// (tests/fixtures/esde/building, planned with retroverse-cli), one of its games moved into a free slot with `worldplan
// edit move` (pinned there, origin cli), then retroverse.exe --start-room room:snes, lockstep (--fixed-dt), hidden,
// driven by input scripts the test writes: the walker walks into the room, M opens the directory, the shoulders reach
// its "Modifiche" tab, a row asks the preview (its dry run on the pool), and its buttons choose.
//   - "Rigenera stanza" with the pin kept: the preview's counts, "Rigenera" committed behind a snapshot (action
//     regenerate), the room rebuilt alone in place (step 4's path, I4b step 6's review), the pinned game still in its
//     slot, the walker at the room's doorway; then "Annulla ultima modifica" (its preview focused on "Indietro")
//     restores the plan's SHA-256 and overrides.json byte for byte, the room rebuilt alone again.
//   - "Rigenera e scarta le personalizzazioni": a run that stops at the second confirmation writes nothing; one that
//     confirms drops the room's pins, the room as the fixture first planned it.
//   - "Rigenera mondo"'s preview, within ADR-0015 §9's 2 s for the room (checked in Release; printed in Debug).
//   - "Rigenera piano" on the handheld floor: its rooms decided again and placed again (the floor committed again), the
//     walker of a room gone at the elevator, and so the walker that stood in the floor's hall.
// Screenshots (build/<preset>/smoke/world_regenerate/...): the directory's Modifiche, the previews of a room and of the
// world, the second confirmation, the room after the regeneration and after the undo.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/run_report.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/file_io.hpp>
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
constexpr std::string_view kRoom = "room:snes";

fs::path buildAssets() {
    return buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
}

rv::world::WorldFiles worldFilesOf(const PlannedBuilding& building) {
    return rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse("esde").value());
}

const rv::world::Placement* placementOf(const rv::world::WorldPlan& plan, const rv::catalog::GameId& game) {
    const auto found = std::find_if(plan.placements.begin(), plan.placements.end(),
                                    [&](const rv::world::Placement& placement) { return placement.game == game; });
    return found != plan.placements.end() ? &*found : nullptr;
}

std::optional<std::string> fileText(const fs::path& file) {
    if (!fs::exists(file)) {
        return std::nullopt;
    }
    auto text = rv::core::readTextFile(file);
    REQUIRE(text.has_value());
    return *text;
}

/// A game of room:snes moved into one of its free slots with the CLI (pinned there): the customization the
/// regenerations keep or drop. The game and its slot.
struct Customized {
    rv::catalog::GameId game;
    rv::world::SlotId slot;
};

Customized customize(const PlannedBuilding& building) {
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::RoomId room = rv::world::RoomId::parse(kRoom).value();
    auto data = rv::world::loadSceneData(buildAssets(), stored.plan, room);
    REQUIRE(data.has_value());
    auto layout = rv::world::collectionLayout(stored.plan, room, *data, {});
    REQUIRE(layout.has_value());
    REQUIRE_FALSE(layout->freeSlots.empty());
    const rv::world::FreeSlot& free = layout->freeSlots.front();
    // A game of the room on another fixture than the free slot's.
    std::optional<Customized> found;
    for (const rv::world::CollectionItem& item : layout->items) {
        if (item.slot.fixture() != free.slot.fixture() && item.stance == free.stance) {
            found = Customized{item.game, free.slot};
            break;
        }
    }
    INFO("a game of " << kRoom << " to move into " << free.slot.toString());
    REQUIRE(found.has_value());
    const AppRun moved = building.cli(
        "edit_move", {"worldplan", "edit", "move", "--game", found->game.toString(), "--slot", found->slot.toString()});
    INFO(moved.describe());
    REQUIRE(moved.exitCode == 0);
    return *found;
}

/// One step of an input script: `actions` (JSON members) on frame `frame`.
struct Step {
    int frame = 0;
    std::string actions;
};

/// The walk into the room and the directory's "Modifiche" tab: frames 5 to 24 forward, 30 M (the directory on its
/// floors), 36 and 42 the right shoulder (the games, then Modifiche), 48 End (its last row: "Annulla ultima modifica").
std::vector<Step> toChanges() {
    return {
        {30, R"("map": 1)"}, {36, R"("ui_filter_next": 1)"}, {42, R"("ui_filter_next": 1)"}, {48, R"("ui_end": 1)"}};
}

fs::path writeScript(const PlannedBuilding& building, std::string_view name, const std::vector<Step>& steps) {
    const fs::path script = building.dir / (std::string(name) + ".json");
    std::ofstream out(script, std::ios::binary);
    out << "[\n  {\"fromFrame\": 5, \"toFrame\": 24, \"actions\": {\"move_forward\": 1}}";
    for (const Step& step : steps) {
        out << ",\n  {\"fromFrame\": " << step.frame << ", \"toFrame\": " << step.frame << ", \"actions\": {"
            << step.actions << "}}";
    }
    out << "\n]\n";
    return script;
}

AppRun runRegeneration(const PlannedBuilding& building, std::string_view name, const std::vector<Step>& steps,
                       int frames, const std::optional<fs::path>& shot) {
    const fs::path script = writeScript(building, name, steps);
    std::vector<std::string> arguments{"--start-room", std::string(kRoom),     "--exit-after-frames",
                                       "--frames",     std::to_string(frames), "--fixed-dt",
                                       "0.016667",     "--input-script",       arg(script)};
    if (shot) {
        arguments.insert(arguments.end(), {"--screenshot", arg(*shot)});
    }
    return runApp(retroverseExe(), building.dir, name, std::move(arguments));
}

const rv::app::WorldRegenerationRecord& onlyRegeneration(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.state == "room");
    REQUIRE(run.report->world.regenerations.size() == 1);
    const rv::app::WorldRegenerationRecord& record = run.report->world.regenerations.front();
    CHECK(record.via == "directory");
    REQUIRE(record.previewFrame.has_value());
    REQUIRE(record.previewMs.has_value());
    CHECK(record.failure.empty());
    return record;
}

/// ADR-0015 §9: a room's preview within 2 s (Release; Debug is about ten times slower and only prints it).
void checkPreviewTime(const rv::app::WorldRegenerationRecord& record, double limitMs) {
    UNSCOPED_INFO(record.kind << " " << record.scope << ": the preview shown " << record.previewMs.value_or(-1.0)
                              << " ms after the request (its job " << record.previewJobMs.value_or(-1.0)
                              << " ms, the inputs " << (record.inputsTaken ? "taken by it" : "ready") << " "
                              << record.inputsMs.value_or(0.0) << " ms, the regeneration "
                              << record.regenerateMs.value_or(-1.0) << " ms)");
#if defined(NDEBUG)
    CHECK(record.previewMs.value_or(1e9) < limitMs);
#else
    static_cast<void>(limitMs);
#endif
}

} // namespace

TEST_CASE("world_regenerate: a room regenerated with its pin kept, then undone to the plan's SHA-256",
          "[world][regenerate]") {
    const PlannedBuilding building("world_regenerate/keep");
    const Customized moved = customize(building);
    const rv::world::WorldFiles files = worldFilesOf(building);
    const rv::world::StoredPlan edited = building.plan();
    const std::optional<std::string> editedOverrides = fileText(files.overrides);
    REQUIRE(editedOverrides.has_value());

    // The directory's Modifiche, "Rigenera stanza" selected (End, then up past "Rigenera mondo" and "Rigenera piano").
    std::vector<Step> steps = toChanges();
    steps.push_back({52, R"("ui_up": 1)"});
    steps.push_back({56, R"("ui_up": 1)"});
    steps.push_back({60, R"("ui_up": 1)"});
    const fs::path directory = building.dir / "directory.png";
    const AppRun directoryRun = runRegeneration(building, "directory", steps, 64, directory);
    skipWithoutOpenGl(directoryRun);
    requireCleanRun(directoryRun);
    static_cast<void>(requireScreenshot(directoryRun, directory, kShotSize));
    CHECK(directoryRun.report->world.regenerations.empty());

    // The preview (its dry run on the pool: lockstep waits), shown and photographed: nothing written.
    steps.push_back({66, R"("ui_activate": 1)"});
    const fs::path preview = building.dir / "preview_room.png";
    const AppRun previewRun = runRegeneration(building, "preview", steps, 100, preview);
    {
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(previewRun);
        static_cast<void>(requireScreenshot(previewRun, preview, kShotSize));
        CHECK(record.kind == "room");
        CHECK(record.scope == kRoom);
        CHECK(record.changed);
        CHECK(record.roomsRegenerated == 1);
        CHECK(record.pinsKept == 1);
        CHECK(record.conflicts == 0);
        CHECK(record.overrides == 1);
        CHECK(record.choice.empty());
        CHECK(record.end.empty());
        checkPreviewTime(record, 2000.0);
    }
    CHECK(building.plan().digest == edited.digest);
    CHECK(fileText(files.overrides) == editedOverrides);

    // "Rigenera" (focused): committed behind a snapshot, the room shown again, the walker at its doorway.
    steps.push_back({110, R"("ui_activate": 1)"});
    const fs::path regenerated = building.dir / "regenerated.png";
    const AppRun run = runRegeneration(building, "regenerate", steps, 160, regenerated);
    std::string regeneratedDigest;
    {
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(run);
        static_cast<void>(requireScreenshot(run, regenerated, kShotSize));
        CHECK(record.choice == "regenerate");
        CHECK(record.end == "committed");
        CHECK(record.written);
        CHECK(record.reused); // the preview's own result: the world as it read it
        CHECK_FALSE(record.snapshot.empty());
        CHECK(record.walker == "doorway:" + std::string(kRoom));
        // The room rebuilt alone in place (its static part and its collection; the floor's hall as it stood).
        CHECK(record.path == "room");
        REQUIRE(record.backMs.has_value());
        UNSCOPED_INFO("the room shown again " << *record.backMs << " ms after Rigenera (the commit's job "
                                              << record.commitMs.value_or(-1.0) << " ms, path " << record.path << ")");
        const std::vector<rv::app::TeleportRecord>& teleports = run.report->world.teleports;
        REQUIRE_FALSE(teleports.empty());
        CHECK(teleports.back().reason == "regeneration");
        CHECK(teleports.back().to == kRoom);
        const rv::world::StoredPlan after = building.plan();
        regeneratedDigest = after.digest;
        CHECK(after.digest != edited.digest);
        CHECK(record.planDigest == after.digest);
        CHECK(run.report->world.planDigest == after.digest);
        // The pin kept: the game where it was put, the pin in overrides.json without a conflict.
        REQUIRE(placementOf(after.plan, moved.game) != nullptr);
        CHECK(placementOf(after.plan, moved.game)->slot == moved.slot);
        auto overrides = rv::world::loadOverrides(files.overrides);
        REQUIRE(overrides.has_value());
        REQUIRE(overrides->gamePins.size() == 1);
        CHECK(overrides->gamePins.front().game == moved.game);
        CHECK_FALSE(overrides->gamePins.front().conflict.has_value());
        // The snapshot of the action: undoable.
        CHECK(fs::exists(files.snapshots / record.snapshot / "snapshot.json"));
    }

    // The room's preview again: nothing to change, the drop alone offered and "Annulla" focused (I4b step 6's review:
    // a focus never rests first on what cannot be undone). Its "Rigenera e scarta" stands where a double-click's second
    // press would find no "Scarta e rigenera" in the second confirmation (confirm.png; the product UI also ignores a
    // click within 0.5 s of the buttons changing).
    {
        std::vector<Step> again = toChanges();
        again.push_back({52, R"("ui_up": 1)"});
        again.push_back({56, R"("ui_up": 1)"});
        again.push_back({60, R"("ui_up": 1)"});
        again.push_back({66, R"("ui_activate": 1)"});
        const fs::path dropOnly = building.dir / "preview_drop_only.png";
        const AppRun dropOnlyRun = runRegeneration(building, "preview_drop_only", again, 100, dropOnly);
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(dropOnlyRun);
        static_cast<void>(requireScreenshot(dropOnlyRun, dropOnly, kShotSize));
        CHECK_FALSE(record.changed);
        CHECK(record.overrides == 1);
        CHECK(record.end.empty());
        CHECK(building.plan().digest == regeneratedDigest);
    }

    // "Annulla ultima modifica" (the tab's last row): the plan and the overrides as they were before Rigenera.
    std::vector<Step> undo = toChanges();
    undo.push_back({54, R"("ui_activate": 1)"});
    const fs::path undoPreview = building.dir / "preview_undo.png";
    const AppRun undoPreviewRun = runRegeneration(building, "undo_preview", undo, 90, undoPreview);
    {
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(undoPreviewRun);
        static_cast<void>(requireScreenshot(undoPreviewRun, undoPreview, kShotSize));
        CHECK(record.kind == "undo");
        CHECK(record.scope == kRoom);
        CHECK_FALSE(record.undoSnapshot.empty());
    }
    CHECK(building.plan().digest == regeneratedDigest);
    // "Indietro" focused (no redo brings an undone action back): Up to "Annulla la modifica".
    undo.push_back({92, R"("ui_up": 1)"});
    undo.push_back({96, R"("ui_activate": 1)"});
    const fs::path undone = building.dir / "undone.png";
    const AppRun undoRun = runRegeneration(building, "undo", undo, 150, undone);
    {
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(undoRun);
        static_cast<void>(requireScreenshot(undoRun, undone, kShotSize));
        CHECK(record.choice == "undo");
        CHECK(record.end == "committed");
        CHECK(record.written);
        CHECK(record.path == "room");
        CHECK(record.walker == "doorway:" + std::string(kRoom));
        REQUIRE(record.backMs.has_value());
        UNSCOPED_INFO("the room shown again " << *record.backMs << " ms after the undo (path " << record.path << ")");
    }
    CHECK(building.plan().digest == edited.digest);
    CHECK(fileText(files.overrides) == editedOverrides);
    CHECK(undoRun.report->world.planDigest == edited.digest);
}

TEST_CASE("world_regenerate: \"Rigenera e scarta le personalizzazioni\" drops the room's pins only after the second "
          "confirmation",
          "[world][regenerate]") {
    const PlannedBuilding building("world_regenerate/drop");
    const std::string generated = building.plan().digest;
    const Customized moved = customize(building);
    const rv::world::WorldFiles files = worldFilesOf(building);
    const rv::world::StoredPlan edited = building.plan();
    const std::optional<std::string> editedOverrides = fileText(files.overrides);
    REQUIRE(editedOverrides.has_value());

    // The preview, Down to the drop, Activate: the second confirmation ("Indietro" focused).
    std::vector<Step> steps = toChanges();
    steps.push_back({52, R"("ui_up": 1)"});
    steps.push_back({56, R"("ui_up": 1)"});
    steps.push_back({60, R"("ui_up": 1)"});
    steps.push_back({66, R"("ui_activate": 1)"});
    steps.push_back({90, R"("ui_down": 1)"});
    steps.push_back({96, R"("ui_activate": 1)"});
    const fs::path confirm = building.dir / "confirm.png";
    const AppRun confirmRun = runRegeneration(building, "confirm", steps, 120, confirm);
    skipWithoutOpenGl(confirmRun);
    {
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(confirmRun);
        static_cast<void>(requireScreenshot(confirmRun, confirm, kShotSize));
        CHECK(record.confirmations == 1);
        CHECK(record.choice.empty());
        CHECK_FALSE(record.written);
    }
    // Nothing without the second confirmation.
    CHECK(building.plan().digest == edited.digest);
    CHECK(fileText(files.overrides) == editedOverrides);

    // Back from the confirmation keeps the preview; then the drop again and "Scarta e rigenera".
    steps.push_back({106, R"("ui_back": 1)"});
    steps.push_back({112, R"("ui_activate": 1)"});
    steps.push_back({118, R"("ui_down": 1)"}); // from "Indietro" to "Scarta e rigenera", below it
    steps.push_back({124, R"("ui_activate": 1)"});
    const fs::path dropped = building.dir / "dropped.png";
    const AppRun run = runRegeneration(building, "drop", steps, 170, dropped);
    const rv::app::WorldRegenerationRecord& record = onlyRegeneration(run);
    static_cast<void>(requireScreenshot(run, dropped, kShotSize));
    CHECK(record.confirmations == 2);
    CHECK(record.choice == "regenerate-drop");
    CHECK(record.end == "committed");
    CHECK(record.written);
    CHECK_FALSE(record.reused);
    CHECK(record.path == "room");
    // The room's pin dropped: the room as a fresh building plans it, the plan the fixture's first.
    auto overrides = rv::world::loadOverrides(files.overrides);
    REQUIRE(overrides.has_value());
    CHECK(overrides->gamePins.empty());
    CHECK(overrides->fixturePins.empty());
    CHECK(building.plan().digest == generated);
    CHECK(placementOf(building.plan().plan, moved.game) != nullptr);
}

TEST_CASE("world_regenerate: the world's preview, then Annulla, writes nothing", "[world][regenerate]") {
    const PlannedBuilding building("world_regenerate/world");
    static_cast<void>(customize(building));
    const rv::world::WorldFiles files = worldFilesOf(building);
    const std::string before = building.plan().digest;
    const std::optional<std::string> overrides = fileText(files.overrides);
    std::vector<Step> steps = toChanges();
    steps.push_back({52, R"("ui_up": 1)"}); // "Rigenera mondo"
    steps.push_back({58, R"("ui_activate": 1)"});
    const fs::path preview = building.dir / "preview_world.png";
    const AppRun previewRun = runRegeneration(building, "preview", steps, 90, preview);
    skipWithoutOpenGl(previewRun);
    {
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(previewRun);
        static_cast<void>(requireScreenshot(previewRun, preview, kShotSize));
        CHECK(record.kind == "world");
        CHECK(record.scope == "world");
        CHECK(record.changed);
        CHECK(record.pinsKept == 1);
        CHECK(record.overrides == 1);
        CHECK(record.roomsRegenerated == building.plan().plan.rooms.size());
        checkPreviewTime(record, 15000.0);
    }
    // Annulla (Down past the drop): nothing written.
    steps.push_back({90, R"("ui_down": 1)"});
    steps.push_back({96, R"("ui_down": 1)"});
    steps.push_back({102, R"("ui_activate": 1)"});
    const AppRun run = runRegeneration(building, "cancel", steps, 120, std::nullopt);
    const rv::app::WorldRegenerationRecord& record = onlyRegeneration(run);
    CHECK(record.choice == "cancel");
    CHECK(record.end == "cancelled");
    CHECK(building.plan().digest == before);
    CHECK(fileText(files.overrides) == overrides);
}

TEST_CASE("world_regenerate: a floor regenerated decides its rooms again and moves them; the walker of a room gone "
          "stands at the elevator",
          "[world][regenerate]") {
    // The handheld floor planned with a console room per system (sharedRoomMinTitles 0), then world.json's default
    // threshold back: "Rigenera piano" puts its two systems under it (pocketlab, pokemini) into the floor's shared room
    // and keeps gb's own room, placed again from the row's start (ADR-0015 §5). The walker starts in pocketlab's room.
    const PlannedBuilding building("world_regenerate/floor");
    const rv::world::StoredPlan planned = building.plan();
    REQUIRE(std::count_if(planned.plan.rooms.begin(), planned.plan.rooms.end(),
                          [](const rv::world::Room& room) { return room.floor == "handheld"; }) == 3);
    // A fixture of each room that goes pinned (`worldplan edit pin-fixture`): customizations the floor's regeneration
    // cannot honour (room-gone), listed in a crowded preview (I4b step 6's review: the body scrolls, the choices stay).
    for (const std::string_view room : {std::string_view("room:pocketlab"), std::string_view("room:pokemini")}) {
        const auto fixture = std::find_if(planned.plan.fixtures.begin(), planned.plan.fixtures.end(),
                                          [&](const rv::world::Fixture& candidate) {
                                              return candidate.id.room().toString() == room;
                                          });
        REQUIRE(fixture != planned.plan.fixtures.end());
        const AppRun pinned = building.cli("pin_" + std::string(room.substr(5)),
                                           {"worldplan", "edit", "pin-fixture", "--fixture", fixture->id.toString()});
        INFO(pinned.describe());
        REQUIRE(pinned.exitCode == 0);
    }
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "config" / "world.json", "{}\n").has_value());
    std::vector<Step> steps = toChanges();
    steps.push_back({52, R"("ui_up": 1)"});
    steps.push_back({56, R"("ui_up": 1)"}); // "Rigenera piano"
    steps.push_back({62, R"("ui_activate": 1)"});
    const fs::path previewShot = building.dir / "preview_floor.png";
    const fs::path previewScript = writeScript(building, "floor_preview", steps);
    const AppRun previewRun =
        runApp(retroverseExe(), building.dir, "floor_preview",
               {"--start-room", "room:pocketlab", "--exit-after-frames", "--frames", "95", "--fixed-dt", "0.016667",
                "--input-script", arg(previewScript), "--screenshot", arg(previewShot)});
    skipWithoutOpenGl(previewRun);
    {
        const rv::app::WorldRegenerationRecord& record = onlyRegeneration(previewRun);
        static_cast<void>(requireScreenshot(previewRun, previewShot, kShotSize));
        CHECK(record.roomsAdded == 1);
        CHECK(record.roomsRemoved == 2);
        CHECK(record.conflicts == 2);
        CHECK(record.end.empty());
    }
    steps.push_back({100, R"("ui_activate": 1)"}); // "Rigenera"
    const fs::path shot = building.dir / "floor.png";
    const fs::path script = writeScript(building, "floor", steps);
    const AppRun run = runApp(retroverseExe(), building.dir, "floor",
                              {"--start-room", "room:pocketlab", "--exit-after-frames", "--frames", "150", "--fixed-dt",
                               "0.016667", "--input-script", arg(script), "--screenshot", arg(shot)});
    const rv::app::WorldRegenerationRecord& record = onlyRegeneration(run);
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
    CHECK(record.kind == "floor");
    CHECK(record.scope == "floor:handheld");
    CHECK(record.roomsAdded == 1);
    CHECK(record.roomsRemoved == 2);
    CHECK(record.end == "committed");
    CHECK(record.path == "floor");
    CHECK(record.walker == "elevator");
    checkPreviewTime(record, 15000.0);
    const rv::world::StoredPlan after = building.plan();
    std::vector<std::string> handheld;
    for (const rv::world::Room& room : after.plan.rooms) {
        if (room.floor == "handheld") {
            handheld.push_back(room.id.toString());
        }
    }
    CHECK(handheld == std::vector<std::string>{"area:shared:handheld", "room:gb"});
    // The floor shown is the regenerated one: its rooms in the run report.
    CHECK(run.report->world.floor == "handheld");
    CHECK(run.report->world.planDigest == after.digest);
    CHECK(std::any_of(run.report->world.rooms.begin(), run.report->world.rooms.end(),
                      [](const rv::app::WorldRoomRecord& room) { return room.id == "area:shared:handheld"; }));
}

TEST_CASE("world_regenerate: \"Rigenera piano\" from the floor's hall puts the walker at the elevator",
          "[world][regenerate]") {
    // As above, the walker backing out of pocketlab's doorway into the handheld floor's hall first (I4b step 6's
    // review: a hall is covered by its floor's regeneration, whose rooms are placed again).
    const PlannedBuilding building("world_regenerate/floor_hall");
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "config" / "world.json", "{}\n").has_value());
    const fs::path script = building.dir / "floor_hall.json";
    {
        std::ofstream out(script, std::ios::binary);
        out << "[\n"
            << R"(  {"fromFrame": 5, "toFrame": 54, "actions": {"move_forward": -1}},)" << "\n"
            << R"(  {"fromFrame": 60, "toFrame": 60, "actions": {"map": 1}},)" << "\n"
            << R"(  {"fromFrame": 66, "toFrame": 66, "actions": {"ui_filter_next": 1}},)" << "\n"
            << R"(  {"fromFrame": 72, "toFrame": 72, "actions": {"ui_filter_next": 1}},)" << "\n"
            << R"(  {"fromFrame": 78, "toFrame": 78, "actions": {"ui_end": 1}},)" << "\n"
            << R"(  {"fromFrame": 82, "toFrame": 82, "actions": {"ui_up": 1}},)" << "\n"
            << R"(  {"fromFrame": 86, "toFrame": 86, "actions": {"ui_up": 1}},)" << "\n"
            << R"(  {"fromFrame": 92, "toFrame": 92, "actions": {"ui_activate": 1}},)" << "\n"
            << R"(  {"fromFrame": 130, "toFrame": 130, "actions": {"ui_activate": 1}})" << "\n"
            << "]\n";
    }
    const AppRun run = runApp(retroverseExe(), building.dir, "floor_hall",
                              {"--start-room", "room:pocketlab", "--exit-after-frames", "--frames", "170", "--fixed-dt",
                               "0.016667", "--input-script", arg(script)});
    skipWithoutOpenGl(run);
    const rv::app::WorldRegenerationRecord& record = onlyRegeneration(run);
    CHECK(record.kind == "floor");
    CHECK(record.scope == "floor:handheld");
    CHECK(record.end == "committed");
    REQUIRE(record.choiceFrame.has_value());
    // The eye in the hall when "Rigenera" was chosen.
    REQUIRE(run.report->sceneAreas.has_value());
    std::string area;
    for (const rv::app::SceneAreaChange& change : run.report->sceneAreas->changes) {
        if (change.frame <= *record.choiceFrame) {
            area = change.area;
        }
    }
    CHECK(area == "hall:handheld");
    CHECK(record.walker == "elevator");
    const std::vector<rv::app::TeleportRecord>& teleports = run.report->world.teleports;
    REQUIRE_FALSE(teleports.empty());
    CHECK(teleports.back().reason == "regeneration");
    CHECK(teleports.back().to == "elevator");
}
