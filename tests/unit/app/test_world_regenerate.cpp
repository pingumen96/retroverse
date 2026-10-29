// The app's regenerations and undo, GPU-free (I4b step 6; ADR-0015 §5, §6): rv/app/world_regenerate.hpp on the
// imported esde/snes-room fixture planned by the building job: the catalog's inputs, the preview's dry run (nothing
// written, its content), the commit of its own result behind a snapshot, a commit that regenerates again over a world
// changed meanwhile, the drop of the room's overrides, the undo's preview and commit (the plan's SHA-256 back, a stale
// one refused), the failures (an action in progress, nothing to undo), the scope of a walker's room and the room an
// update's needsConfirmation names.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <rv/app/room_snapshot.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_regenerate.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_snapshots.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_test_support.hpp"

namespace fs = std::filesystem;

namespace {

using rv::test::ImportedFixture;

const rv::world::RoomId kSnesRoom = rv::world::RoomId::parse("room:snes").value();

rv::app::WorldPaths worldPathsOf(const ImportedFixture& imported) {
    return rv::app::WorldPaths{imported.paths.assetRoot(), imported.paths.userDataRoot(), imported.paths.configDir(),
                               imported.paths.cacheDir()};
}

std::shared_ptr<rv::app::WorldCatalogReader> readerOf(const ImportedFixture& imported) {
    auto database = imported.openDatabase();
    auto reader = std::make_shared<rv::app::WorldCatalogReader>();
    reader->query = rv::catalog_db::SqliteCatalogQuery::open(database.openReader().value()).value();
    reader->profile = rv::test::fixtureLibrary(imported.fixture);
    return reader;
}

rv::world::WorldFiles filesOf(const ImportedFixture& imported) {
    return rv::world::worldFiles(imported.paths.userDataRoot(), rv::test::fixtureLibrary(imported.fixture).id);
}

/// The fixture's building planned and saved by the building job (as the app's first start).
rv::app::WorldBuild planned(const ImportedFixture& imported, rv::app::WorldCatalogReader& reader) {
    rv::app::WorldBuildInput input;
    input.paths = worldPathsOf(imported);
    input.library = rv::test::fixtureLibrary(imported.fixture).id;
    input.snapshot = rv::app::takeBuildingSnapshot(reader, {}, rv::app::roomSystemsFilter(imported.paths.assetRoot()));
    input.writer = "retroverse (test)";
    input.startRoom = kSnesRoom;
    rv::app::WorldBuild build = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(build.reason);
    REQUIRE(build.written);
    return build;
}

/// retroverse-cli on the fixture's data directory; its exit code.
int runCli(const ImportedFixture& imported, std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(imported.data)});
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    rv::cli::Output out = rv::cli::Output::memory();
    rv::cli::Output err = rv::cli::Output::memory();
    const rv::cli::ExitCode code = rv::cli::run(arguments, out, err, environment);
    rv::core::LogConfig config;
    config.console = false;
    rv::core::initLogging(config);
    rv::core::shutdownLogging();
    INFO(out.text() << err.text());
    return static_cast<int>(code);
}

/// A game of room:snes moved into one of its free slots with `worldplan edit move` (pinned there, origin cli).
struct Moved {
    rv::catalog::GameId game;
    rv::world::SlotId slot;
};

Moved moveOne(const ImportedFixture& imported, std::size_t skip = 0) {
    const auto stored = rv::world::readStoredPlan(filesOf(imported).plan);
    REQUIRE(stored.has_value());
    auto data = rv::world::loadSceneData(imported.paths.assetRoot(), stored->plan, kSnesRoom);
    REQUIRE(data.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, kSnesRoom, *data, {});
    REQUIRE(layout.has_value());
    // The games pinned already stay where they are.
    std::set<rv::catalog::GameId> pinned;
    if (fs::exists(filesOf(imported).overrides)) {
        for (const rv::world::GamePin& pin : rv::world::loadOverrides(filesOf(imported).overrides).value().gamePins) {
            pinned.insert(pin.game);
        }
    }
    // The `skip`-th free slot that a game of another fixture of its stance can move into.
    std::size_t found = 0;
    for (const rv::world::FreeSlot& free : layout->freeSlots) {
        for (const rv::world::CollectionItem& item : layout->items) {
            if (item.slot.fixture() == free.slot.fixture() || item.stance != free.stance ||
                pinned.contains(item.game)) {
                continue;
            }
            if (found++ < skip) {
                break;
            }
            REQUIRE(runCli(imported, {"worldplan", "edit", "move", "--game", item.game.toString(), "--slot",
                                      free.slot.toString()}) == 0);
            return Moved{item.game, free.slot};
        }
    }
    FAIL("no game to move into a free slot of room:snes");
}

std::optional<std::string> fileText(const fs::path& file) {
    if (!fs::exists(file)) {
        return std::nullopt;
    }
    return rv::core::readTextFile(file).value();
}

/// The world's folder as it is: every file's path and bytes (world.lock's presence included).
std::map<std::string, std::string> folderOf(const fs::path& folder) {
    std::map<std::string, std::string> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            files.emplace(rv::core::pathToUtf8(fs::relative(entry.path(), folder)),
                          rv::core::readTextFile(entry.path()).value_or(std::string("(unreadable)")));
        }
    }
    return files;
}

} // namespace

TEST_CASE("app/world_regenerate: a room's preview writes nothing; Rigenera commits its own result behind a snapshot, "
          "the pin kept; undo restores the plan's SHA-256",
          "[app][world_regenerate]") {
    const ImportedFixture imported("world-regenerate-keep", true, "esde/snes-room");
    const auto reader = readerOf(imported);
    static_cast<void>(planned(imported, *reader));
    const Moved moved = moveOne(imported);
    const rv::world::WorldFiles files = filesOf(imported);
    const std::string edited = rv::world::readStoredPlan(files.plan).value().digest;
    const std::optional<std::string> editedOverrides = fileText(files.overrides);
    const rv::app::WorldPaths paths = worldPathsOf(imported);

    // The catalog's inputs: every system's selection, the world's data.
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(paths, *reader);
    INFO(inputs.error);
    REQUIRE(inputs.inputs);
    CHECK(inputs.failure.empty());
    REQUIRE(inputs.inputs->selections.size() == 1);
    CHECK(inputs.inputs->selections.front().room == kSnesRoom);
    REQUIRE(inputs.inputs->snapshot);

    // The preview: its content, nothing written (the world's folder byte for byte).
    const auto folderBefore = folderOf(files.folder);
    const rv::app::RegenerationPreview preview =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *inputs.inputs);
    INFO(preview.error);
    REQUIRE(preview.failure.empty());
    REQUIRE(preview.regeneration);
    CHECK(folderOf(files.folder) == folderBefore);
    CHECK(preview.planDigest == edited);
    const rv::ui::RegeneratePreviewContent& content = preview.content;
    CHECK(content.changed);
    CHECK(content.roomsRegenerated == 1);
    CHECK(content.roomsAdded == 0);
    CHECK(content.roomsRemoved == 0);
    CHECK(content.pinsKept == 1);
    CHECK(content.conflictCount == 0);
    CHECK(content.overrides == 1);
    CHECK(content.gamesMoved == preview.regeneration->impact.gamesMoved.size());
    CHECK(content.gamesMoved > 0);

    // Rigenera: the preview's own result (the world as it read it), behind a snapshot; the pin kept.
    const rv::app::RegenerationCommit commit =
        rv::app::commitRegeneration(paths, preview, false, *inputs.inputs, "retroverse (test)");
    INFO(commit.error);
    REQUIRE(commit.failure.empty());
    CHECK(commit.reused);
    CHECK(commit.written);
    CHECK_FALSE(commit.snapshot.empty());
    REQUIRE(commit.plan);
    const auto after = rv::world::readStoredPlan(files.plan);
    REQUIRE(after.has_value());
    CHECK(after->digest == commit.planDigest);
    CHECK(after->digest != edited);
    CHECK(after->plan == preview.regeneration->result.plan);
    const auto placement = std::find_if(after->plan.placements.begin(), after->plan.placements.end(),
                                        [&](const rv::world::Placement& p) { return p.game == moved.game; });
    REQUIRE(placement != after->plan.placements.end());
    CHECK(placement->slot == moved.slot);
    // The rooms' items written again, the building stamp gone (the next start runs the building job).
    CHECK(commit.items.contains(rv::catalog::SystemName::parse("snes").value()));
    CHECK_FALSE(fs::exists(rv::app::buildingStampFile(paths.cacheDir, inputs.inputs->library)));

    // The undo's preview names the regeneration; its commit restores the edited plan and overrides byte for byte.
    const rv::app::UndoPreview undo = rv::app::previewUndo(paths, inputs.inputs->library, commit.plan.get());
    INFO(undo.error);
    REQUIRE(undo.failure.empty());
    REQUIRE(undo.snapshot.has_value());
    CHECK(undo.snapshot->name == commit.snapshot);
    CHECK(undo.content.undoAction == "regenerate");
    CHECK(undo.content.undoScope == "room:snes");
    CHECK(undo.content.undoScopeTitle.roomKind == "console");
    CHECK(std::regex_match(undo.content.undoWhen, std::regex(R"(\d\d/\d\d/\d{4} \d\d:\d\d)")));
    const rv::app::UndoCommit undone = rv::app::commitUndo(paths, inputs.inputs->library, undo.snapshot->name,
                                                           "retroverse (test)", inputs.inputs->snapshot.get());
    INFO(undone.error);
    REQUIRE(undone.failure.empty());
    CHECK(undone.restored == commit.snapshot);
    CHECK_FALSE(undone.kept.empty());
    CHECK(undone.planDigest == edited);
    CHECK(rv::world::readStoredPlan(files.plan).value().digest == edited);
    CHECK(fileText(files.overrides) == editedOverrides);
    // The next undo is the CLI's edit; one asked for another snapshot is refused, nothing written.
    const rv::app::UndoPreview next = rv::app::previewUndo(paths, inputs.inputs->library, nullptr);
    REQUIRE(next.snapshot.has_value());
    CHECK(next.content.undoAction == "edit");
    const rv::app::UndoCommit stale =
        rv::app::commitUndo(paths, inputs.inputs->library, commit.snapshot, "retroverse (test)", nullptr);
    CHECK(stale.failure == rv::app::regenerate_failure::kChanged);
    CHECK(rv::world::readStoredPlan(files.plan).value().digest == edited);
}

TEST_CASE("app/world_regenerate: a world changed since the preview is regenerated again; the drop gives the "
          "generated plan back",
          "[app][world_regenerate]") {
    const ImportedFixture imported("world-regenerate-drop", true, "esde/snes-room");
    const auto reader = readerOf(imported);
    const rv::app::WorldBuild first = planned(imported, *reader);
    static_cast<void>(moveOne(imported));
    const rv::world::WorldFiles files = filesOf(imported);
    const rv::app::WorldPaths paths = worldPathsOf(imported);
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(paths, *reader);
    REQUIRE(inputs.inputs);
    const rv::app::RegenerationPreview preview =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *inputs.inputs);
    REQUIRE(preview.failure.empty());

    // Another writer moves a second game meanwhile: the commit regenerates the room again, both pins kept.
    static_cast<void>(moveOne(imported));
    const rv::app::RegenerationCommit again =
        rv::app::commitRegeneration(paths, preview, false, *inputs.inputs, "retroverse (test)");
    INFO(again.error);
    REQUIRE(again.failure.empty());
    CHECK_FALSE(again.reused);
    CHECK(again.written);
    CHECK(again.impact.pinsKept.size() == 2);
    REQUIRE(rv::world::loadOverrides(files.overrides).value().gamePins.size() == 2);

    // "Rigenera e scarta le personalizzazioni": the room's pins dropped, the plan the building job first generated.
    const rv::app::RegenerationPreview second =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *inputs.inputs);
    REQUIRE(second.failure.empty());
    CHECK_FALSE(second.content.changed); // the pins are honoured as they stand
    CHECK(second.content.overrides == 2);
    const rv::app::RegenerationCommit dropped =
        rv::app::commitRegeneration(paths, second, true, *inputs.inputs, "retroverse (test)");
    INFO(dropped.error);
    REQUIRE(dropped.failure.empty());
    CHECK_FALSE(dropped.reused);
    CHECK(dropped.impact.overridesDropped.size() == 2);
    CHECK(rv::world::loadOverrides(files.overrides).value().empty());
    CHECK(rv::world::readStoredPlan(files.plan).value().digest == first.planDigest);
}

TEST_CASE("app/world_regenerate: the failures: an action in progress, nothing to undo, no plan, unknown media",
          "[app][world_regenerate]") {
    const ImportedFixture imported("world-regenerate-fail", true, "esde/snes-room");
    const auto reader = readerOf(imported);
    const rv::app::WorldPaths paths = worldPathsOf(imported);
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(paths, *reader);
    REQUIRE(inputs.inputs);
    const rv::catalog::LibraryId library = inputs.inputs->library;
    // No plan yet.
    const rv::app::RegenerationPreview none =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofWorld(), *inputs.inputs);
    CHECK(none.failure == rv::app::regenerate_failure::kNoPlan);
    static_cast<void>(planned(imported, *reader));
    // Nothing to undo: the building job's writes are no explicit action.
    const rv::app::UndoPreview nothing = rv::app::previewUndo(paths, library, nullptr);
    CHECK(nothing.failure == rv::app::regenerate_failure::kNothingToUndo);
    // A journal there: an action in progress (or interrupted) for the preview and the undo alike.
    const rv::world::WorldFiles files = filesOf(imported);
    REQUIRE(rv::core::writeTextFileAtomic(files.transaction, "{}\n").has_value());
    const rv::app::RegenerationPreview busy =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *inputs.inputs);
    CHECK(busy.failure == rv::app::regenerate_failure::kBusy);
    CHECK(rv::app::previewUndo(paths, library, nullptr).failure == rv::app::regenerate_failure::kBusy);
    fs::remove(files.transaction);
    // A room the plan lacks.
    const rv::app::RegenerationPreview missing = rv::app::previewRegeneration(
        paths, rv::world::RegenerationScope::ofRoom(rv::world::RoomId::parse("room:psx").value()), *inputs.inputs);
    CHECK(missing.failure == rv::app::regenerate_failure::kFailed);
    // Unknown media decide nothing (ADR-0010 §3).
    auto snapshot = std::make_shared<rv::app::BuildingSnapshot>(*inputs.inputs->snapshot);
    for (rv::app::RoomSnapshotResult& system : snapshot->systems) {
        system.snapshot.mediaKnown = false;
        system.snapshot.mediaUnknownReason = "the media root is out of reach (test)";
    }
    const rv::app::RegenerationInputsResult unknown = rv::app::regenerationInputsOf(paths, snapshot);
    CHECK_FALSE(unknown.inputs);
    CHECK(unknown.failure == rv::app::regenerate_failure::kMediaUnknown);
}

TEST_CASE("app/world_regenerate: the walker's room in a scope; the room a needsConfirmation names",
          "[app][world_regenerate]") {
    rv::world::WorldPlan plan;
    rv::world::Room snes;
    snes.id = kSnesRoom;
    snes.floor = "gen4";
    rv::world::Room shared;
    shared.id = rv::world::RoomId::parse("area:shared:handheld").value();
    shared.floor = "handheld";
    plan.rooms = {snes, shared};
    // A room scope covers its chain; a floor its rooms; the world every room; a hall never.
    CHECK(rv::app::regenerationCovers("room:snes", "room:snes", &plan));
    CHECK(rv::app::regenerationCovers("room:snes:2", "room:snes", &plan));
    CHECK(rv::app::regenerationCovers("room:snes", "room:snes:2", &plan));
    CHECK_FALSE(rv::app::regenerationCovers("room:psx", "room:snes", &plan));
    CHECK(rv::app::regenerationCovers("room:snes", "floor:gen4", &plan));
    CHECK_FALSE(rv::app::regenerationCovers("area:shared:handheld", "floor:gen4", &plan));
    CHECK(rv::app::regenerationCovers("area:shared:handheld", "world", &plan));
    CHECK_FALSE(rv::app::regenerationCovers("hall:gen4", "world", &plan));
    // The update's sentences (a pin's slot, a fixture, a room not migrated): the first room of the plan named.
    CHECK(rv::app::confirmationRoomOf({"room:snes/fx:w01: a side-wall module would overlap room:snes/fx:dc01"}, plan) ==
          kSnesRoom);
    CHECK(rv::app::confirmationRoomOf({"fixture area:shared:handheld/fx:e02 pinned at x 100 z 200 mm, yaw 0 cdeg"},
                                      plan) == shared.id);
    CHECK(rv::app::confirmationRoomOf({"migrate room:psx from a to b: a pin names the room",
                                       "game 0123456789abcdef pinned to room:snes/fx:dc01/l0/s2"},
                                      plan) == kSnesRoom);
    CHECK_FALSE(rv::app::confirmationRoomOf({"migrate room:psx from a to b: a pin names the room"}, plan).has_value());
    CHECK(rv::app::localSnapshotTime("not a time").empty());
    CHECK(std::regex_match(rv::app::localSnapshotTime("2026-10-29T10:15:30.123Z"),
                           std::regex(R"(\d\d/\d\d/2026 \d\d:\d\d)")));
}
