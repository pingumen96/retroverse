// The app's regenerations and undo, GPU-free (I4b step 6; ADR-0015 §5, §6): rv/app/world_regenerate.hpp on the
// imported esde/snes-room fixture planned by the building job: the catalog's inputs, the preview's dry run (nothing
// written, its content), the commit of its own result behind a snapshot, a commit that regenerates again over a world
// changed meanwhile, the drop of the room's overrides, the undo's preview and commit (the plan's SHA-256 back, a stale
// one refused), the failures (an action in progress, nothing to undo), the scope of a walker's room and the room an
// update's needsConfirmation names. Since the step's review: the drop refused over overrides changed since the preview,
// a commit that changes nothing writes nothing (no items, the stamp kept), the room of the generator's own
// needs-confirmation (a pinned south-wall poster refusing a growth), a late job answer never taking a newer one's
// place, the rooms a commit changed alone.

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
#include <rv/catalog_db/database.hpp>
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

#include "../world/world_update_support.hpp"
#include "catalog_test_support.hpp"
#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

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
    // The room alone changed on its floor (its place, size and template kept), or the floor whole.
    const std::string floor = preview.previous->rooms.front().floor;
    if (const auto alone =
            rv::app::roomsChangedAlone(*preview.previous, preview.regeneration->result.plan, floor)) {
        CHECK(*alone == std::vector<rv::world::RoomId>{kSnesRoom});
    }
    CHECK(rv::app::roomsChangedAlone(*preview.previous, *preview.previous, floor) ==
          std::vector<rv::world::RoomId>{});

    // Rigenera: the preview's own result (the world as it read it), behind a snapshot; the pin kept.
    const rv::app::RegenerationCommit commit =
        rv::app::commitRegeneration(paths, preview, false, *inputs.inputs, "retroverse (test)");
    INFO(commit.error);
    REQUIRE(commit.failure.empty());
    CHECK(commit.reused);
    CHECK(commit.changed);
    CHECK(commit.written);
    CHECK_FALSE(commit.snapshot.empty());
    REQUIRE(commit.plan);
    const auto after = rv::world::readStoredPlan(files.plan);
    REQUIRE(after.has_value());
    // The plan committed is the regeneration's, its digest the commit's (nothing read back), the file's.
    CHECK(after->digest == commit.planDigest);
    CHECK(*commit.plan == after->plan);
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
    CHECK(undone.written);
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

    // "Rigenera" of a preview with nothing to change: nothing written, no items files, the building stamp kept.
    const fs::path stamp = rv::app::buildingStampFile(paths.cacheDir, inputs.inputs->library);
    REQUIRE(rv::app::writeBuildingStamp(stamp, rv::app::BuildingStamp{"plan", "catalog", "inputs", 0}).has_value());
    const std::string planBefore = rv::world::readStoredPlan(files.plan).value().digest;
    const rv::app::RegenerationCommit nothing =
        rv::app::commitRegeneration(paths, second, false, *inputs.inputs, "retroverse (test)");
    INFO(nothing.error);
    REQUIRE(nothing.failure.empty());
    CHECK(nothing.reused);
    CHECK_FALSE(nothing.changed);
    CHECK_FALSE(nothing.written);
    CHECK(nothing.items.empty());
    CHECK(nothing.planDigest == planBefore);
    CHECK(fs::exists(stamp));

    // The drop is consented to for the customizations the preview named: once overrides.json changed alone (a pin of
    // the room added beside the plan, as another program would write it; the plan the same), it is refused (changed)
    // and nothing is written.
    rv::world::WorldOverrides added = rv::world::loadOverrides(files.overrides).value();
    added.gamePins.push_back({rv::catalog::GameId::fromValue(0x123456789).value(),
                              rv::world::SlotId::parse("room:snes/fx:w01/s:0.0").value()});
    REQUIRE(rv::core::writeTextFileAtomic(files.overrides, rv::world::writeOverrides(added).value()).has_value());
    const std::optional<std::string> overridesBefore = fileText(files.overrides);
    const rv::app::RegenerationCommit stale =
        rv::app::commitRegeneration(paths, second, true, *inputs.inputs, "retroverse (test)");
    CHECK(stale.failure == rv::app::regenerate_failure::kChanged);
    CHECK_FALSE(stale.written);
    CHECK(rv::world::readStoredPlan(files.plan).value().digest == planBefore);
    CHECK(fileText(files.overrides) == overridesBefore);
    // Its preview again (the overrides as they are now), then the drop.
    const rv::app::RegenerationPreview third =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *inputs.inputs);
    REQUIRE(third.failure.empty());
    CHECK(third.content.overrides >= 2);
    const rv::app::RegenerationCommit dropped =
        rv::app::commitRegeneration(paths, third, true, *inputs.inputs, "retroverse (test)");
    INFO(dropped.error);
    REQUIRE(dropped.failure.empty());
    CHECK_FALSE(dropped.reused);
    CHECK(dropped.changed);
    CHECK(dropped.impact.overridesDropped.size() >= 2);
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
    // A hall: its floor's regeneration places its rooms again, the world's every floor's; a room's never.
    CHECK(rv::app::regenerationCovers("hall:gen4", "world", &plan));
    CHECK(rv::app::regenerationCovers("hall:gen4", "floor:gen4", &plan));
    CHECK_FALSE(rv::app::regenerationCovers("hall:gen4", "floor:handheld", &plan));
    CHECK_FALSE(rv::app::regenerationCovers("hall:gen4", "room:snes", &plan));
    // The room an update's needs-confirmation offers: the first of its rooms the plan holds, in its sentences' order.
    const rv::world::RoomId psx = rv::world::RoomId::parse("room:psx").value();
    CHECK(rv::app::confirmationRoomOf({psx, shared.id, kSnesRoom}, plan) == shared.id);
    CHECK_FALSE(rv::app::confirmationRoomOf({psx}, plan).has_value());
    CHECK_FALSE(rv::app::confirmationRoomOf({}, plan).has_value());
    CHECK(rv::app::localSnapshotTime("not a time").empty());
    CHECK(std::regex_match(rv::app::localSnapshotTime("2026-10-29T10:15:30.123Z"),
                           std::regex(R"(\d\d/\d\d/2026 \d\d:\d\d)")));
}

TEST_CASE("app/world_regenerate: the notice offers the room of the generator's own needs-confirmation",
          "[app][world_regenerate]") {
    using namespace rv::test::world;
    constexpr std::uint64_t kSelection = 20261018;
    constexpr std::uint64_t kWorld = 102938;
    // A room with a fixture pinned on its south wall: an update with more titles than its slots would grow it and
    // move that fixture, so the growth is refused and said so (incremental.cpp's sentence names the head's socket,
    // not its room: the room comes beside it).
    const rv::world::RoomSelection big = makeSelection(3205, kSelection);
    const rv::world::WorldPlan full = generateSelection(big, kWorld).plan;
    rv::world::WorldOverrides south;
    for (const rv::world::Fixture& fixture : full.fixtures) {
        const rv::world::TemplateSocket* socket = snesData().roomTemplate.socket(fixture.id.socket());
        if (socket != nullptr && socket->wall == rv::world::Wall::South && south.fixturePins.empty()) {
            south.fixturePins.push_back({fixture.id, std::nullopt});
        }
    }
    REQUIRE_FALSE(south.fixturePins.empty());
    rv::world::RoomSelection more = big;
    std::set<std::uint64_t> taken = gameValuesOf(big);
    rv::core::Random random(77);
    const SpineCounts counts = countSpine(full);
    for (std::uint64_t i = 0; i < counts.freeSlots + counts.dressedSlots + 3; ++i) {
        addTitle(more, random, taken);
    }
    const rv::world::GenerationResult held = updateRoom(full, more, kWorld, south);
    REQUIRE(held.changes.needsConfirmation.size() == 1);
    INFO(held.changes.needsConfirmation.front());
    CHECK(held.changes.needsConfirmation.front().find("room:snes") == std::string::npos);
    REQUIRE(held.changes.needsConfirmationRooms.size() == 1);
    CHECK(rv::app::confirmationRoomOf(held.changes.needsConfirmationRooms, held.plan) == kSnesRoom);
}

TEST_CASE("app/world_regenerate: a job's late answer never takes a newer job's place", "[app][world_regenerate]") {
    rv::app::JobMailbox<std::string> box;
    // The first job is asked (generation 1), then cancelled for a second one (generation 2) before it answers.
    box.reset(1);
    box.reset(2);
    // The second answers first; the first, cancelled but still running, answers late: refused.
    CHECK(box.post(2, "second"));
    CHECK_FALSE(box.post(1, "first"));
    CHECK_FALSE(box.take(1).has_value());
    CHECK(box.take(2) == std::optional<std::string>("second"));
    CHECK_FALSE(box.take(2).has_value()); // once
    // A late answer of the first job before the second's: refused too, the second's still taken.
    box.reset(3);
    CHECK_FALSE(box.post(2, "late"));
    CHECK_FALSE(box.take(3).has_value());
    CHECK(box.post(3, "third"));
    CHECK(box.take(3) == std::optional<std::string>("third"));
}

TEST_CASE("app/world_regenerate: the rooms a commit changed alone on a floor", "[app][world_regenerate]") {
    using namespace rv::test::world;
    const rv::world::WorldPlan before = generateSelection(makeSelection(120, 20261018), 102938).plan;
    REQUIRE(before.rooms.size() == 1);
    const std::string floor = before.rooms.front().floor;
    // Nothing changed: no room.
    CHECK(rv::app::roomsChangedAlone(before, before, floor) == std::vector<rv::world::RoomId>{});
    // Two games swapped within the room: the room alone.
    rv::world::WorldPlan moved = before;
    REQUIRE(moved.placements.size() >= 2);
    std::swap(moved.placements[0].game, moved.placements[1].game);
    CHECK(rv::app::roomsChangedAlone(before, moved, floor) == std::vector<rv::world::RoomId>{kSnesRoom});
    // Another floor's view: nothing of it changed.
    CHECK(rv::app::roomsChangedAlone(before, moved, "another") == std::vector<rv::world::RoomId>{});
    // The room moved, or grown: the hall changes, the floor is built again.
    rv::world::WorldPlan placed = moved;
    placed.rooms.front().placement.xMm += 1000;
    CHECK_FALSE(rv::app::roomsChangedAlone(before, placed, floor).has_value());
    rv::world::WorldPlan grown = moved;
    grown.rooms.front().maxZMm += 500;
    CHECK_FALSE(rv::app::roomsChangedAlone(before, grown, floor).has_value());
    // A room's generator version alone is no change of its place.
    rv::world::WorldPlan stamped = moved;
    stamped.rooms.front().generatorVersion = {};
    CHECK(rv::app::roomsChangedAlone(before, stamped, floor) == std::vector<rv::world::RoomId>{kSnesRoom});
}

TEST_CASE("app/world_regenerate: the inputs let go of while nothing needs them; a preview right after makes them again",
          "[app][world_regenerate]") {
    const ImportedFixture imported("world-regenerate-release", true, "esde/snes-room");
    const auto reader = readerOf(imported);
    static_cast<void>(planned(imported, *reader));
    const rv::app::WorldPaths paths = worldPathsOf(imported);
    rv::app::RegenerationInputsSlot slot;
    // The tab's read-ahead: made once, then served.
    bool taken = false;
    const rv::app::RegenerationInputsResult first = rv::app::regenerationInputsFor(slot, paths, reader.get(), taken);
    REQUIRE(first.inputs);
    CHECK(taken);
    CHECK(rv::app::regenerationInputsFor(slot, paths, reader.get(), taken).inputs == first.inputs);
    CHECK_FALSE(taken);
    // Needed (the directory open, a preview or a commit asked or running): kept.
    CHECK_FALSE(rv::app::releaseIdleInputs(slot, true));
    CHECK(slot.result.has_value());
    // The directory closed with nothing running: the slot is empty; a job that took them keeps them.
    CHECK(rv::app::releaseIdleInputs(slot, false));
    CHECK_FALSE(slot.result.has_value());
    CHECK_FALSE(rv::app::releaseIdleInputs(slot, false));
    const rv::app::RegenerationPreview held =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *first.inputs);
    CHECK(held.failure.empty());
    // A preview opened right after: its inputs made again, the preview as before.
    const rv::app::RegenerationInputsResult again = rv::app::regenerationInputsFor(slot, paths, reader.get(), taken);
    REQUIRE(again.inputs);
    CHECK(taken);
    CHECK(again.inputs != first.inputs);
    const rv::app::RegenerationPreview preview =
        rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *again.inputs);
    INFO(preview.error);
    REQUIRE(preview.failure.empty());
    CHECK(preview.content == held.content);
}

TEST_CASE("app/world_regenerate: a preview takes the plan shown when its file is that plan, in any order; another "
          "plan's digest has the file read",
          "[app][world_regenerate]") {
    const ImportedFixture imported("world-regenerate-known", true, "esde/snes-room");
    const auto reader = readerOf(imported);
    const rv::app::WorldBuild build = planned(imported, *reader);
    static_cast<void>(moveOne(imported)); // a pin: the preview changes the room
    const rv::world::WorldFiles files = filesOf(imported);
    const auto stored = rv::world::readStoredPlan(files.plan);
    REQUIRE(stored.has_value());
    const rv::app::WorldPaths paths = worldPathsOf(imported);
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(paths, *reader);
    INFO(inputs.error);
    REQUIRE(inputs.inputs);
    const rv::world::RegenerationScope scope = rv::world::RegenerationScope::ofRoom(kSnesRoom);

    // The file read: what every other preview is compared with.
    const rv::app::RegenerationPreview read = rv::app::previewRegeneration(paths, scope, *inputs.inputs);
    INFO(read.error);
    REQUIRE(read.failure.empty());
    CHECK_FALSE(read.knownPlan);
    REQUIRE(read.regeneration);
    const std::string expected = rv::world::writePlan(read.regeneration->result.plan).value();
    const auto same = [&](const rv::app::RegenerationPreview& preview) {
        INFO(preview.error);
        REQUIRE(preview.failure.empty());
        REQUIRE(preview.regeneration);
        CHECK(preview.content == read.content);
        CHECK(preview.planDigest == read.planDigest);
        CHECK(*preview.previous == *read.previous);
        CHECK(rv::world::writePlan(preview.regeneration->result.plan).value() == expected);
        CHECK(preview.regeneration->result.changes.digestBefore == read.regeneration->result.changes.digestBefore);
        CHECK(preview.regeneration->result.changes.digestAfter == read.regeneration->result.changes.digestAfter);
        CHECK(preview.regeneration->impact == read.regeneration->impact);
    };

    // The plan shown, its file's digest: taken, not parsed.
    const rv::app::KnownPlan shown{std::make_shared<const rv::world::WorldPlan>(stored->plan), stored->digest};
    const rv::app::RegenerationPreview taken = rv::app::previewRegeneration(paths, scope, *inputs.inputs, &shown);
    CHECK(taken.knownPlan);
    same(taken);
    // The standing rooms' data read ahead into the inputs' loader: the same preview.
    CHECK(rv::world::standingRoomRequests(stored->plan, inputs.inputs->platformTags).size() ==
          stored->plan.rooms.size());
    static_cast<void>(rv::app::warmRoomData(*inputs.inputs, stored->plan));
    same(rv::app::previewRegeneration(paths, scope, *inputs.inputs, &shown));

    // The plan shown with its lists in another order (a plan in memory need not be sorted): sorted first.
    rv::world::WorldPlan reordered = stored->plan;
    std::reverse(reordered.rooms.begin(), reordered.rooms.end());
    std::reverse(reordered.fixtures.begin(), reordered.fixtures.end());
    std::reverse(reordered.placements.begin(), reordered.placements.end());
    std::reverse(reordered.lights.begin(), reordered.lights.end());
    std::reverse(reordered.unplaced.begin(), reordered.unplaced.end());
    REQUIRE_FALSE(reordered == stored->plan);
    REQUIRE(rv::world::planDigest(reordered).value() == stored->digest);
    const rv::app::KnownPlan unsorted{std::make_shared<const rv::world::WorldPlan>(reordered), stored->digest};
    const rv::app::RegenerationPreview sorted = rv::app::previewRegeneration(paths, scope, *inputs.inputs, &unsorted);
    CHECK(sorted.knownPlan);
    same(sorted);

    // The building's plan from before the move: another digest, the file read.
    REQUIRE(build.plan.has_value());
    REQUIRE(build.planDigest != stored->digest);
    const rv::app::KnownPlan older{std::make_shared<const rv::world::WorldPlan>(*build.plan), build.planDigest};
    const rv::app::RegenerationPreview again = rv::app::previewRegeneration(paths, scope, *inputs.inputs, &older);
    CHECK_FALSE(again.knownPlan);
    same(again);

    // The world read apart (as the app's job reads it before it waits for the inputs), then previewed.
    const rv::app::RegenerationWorld world =
        rv::app::readRegenerationWorld(paths, inputs.inputs->library, &shown);
    CHECK(world.failure.empty());
    CHECK(world.knownPlan);
    CHECK(world.planDigest == stored->digest);
    same(rv::app::previewRegeneration(world, scope, *inputs.inputs));
    // A world of another library than the inputs' is refused.
    rv::app::RegenerationWorld other = world;
    other.library = rv::catalog::LibraryId::parse("other").value();
    CHECK(rv::app::previewRegeneration(other, scope, *inputs.inputs).failure == rv::app::regenerate_failure::kFailed);
    // A library without a world: no plan.
    CHECK(rv::app::readRegenerationWorld(paths, other.library, &shown).failure ==
          rv::app::regenerate_failure::kNoPlan);
}

// The time of a large room's preview (the memory margin's second part, docs/i5/memory-margin.md; I4b step 6 measured
// the real room:amstradcpc's at 1.6 s quiet and 3.2 s under load, against 2 s): esde/snes-stress (2,000 titles, 300 of
// them twice, generated at test time) imported and planned, then room:snes previewed three times on one set of inputs
// (the first with its rooms' data read, the others from the loader's cache), each preview's read, regeneration and
// whole printed. Informative, by hand in Release:
//   build/msvc-x64-release/bin/rv_tests.exe "app/bench: a large room's preview*"
TEST_CASE("app/bench: a large room's preview, its read and its regeneration", "[.bench]") {
    const ImportedFixture imported("world-regenerate-bench", true, "esde/snes-stress");
    const auto reader = readerOf(imported);
    static_cast<void>(planned(imported, *reader));
    const rv::app::WorldPaths paths = worldPathsOf(imported);
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(paths, *reader);
    INFO(inputs.error);
    REQUIRE(inputs.inputs);
    const auto stored = rv::world::readStoredPlan(filesOf(imported).plan);
    REQUIRE(stored.has_value());
    WARN("inputs: snapshot " << inputs.inputs->snapshotMs << " ms, selections " << inputs.inputs->selectMs
                             << " ms; the plan places " << stored->plan.placements.size() << " in "
                             << stored->plan.rooms.size() << " room(s)");
    for (int run = 0; run < 3; ++run) {
        const rv::app::RegenerationPreview preview =
            rv::app::previewRegeneration(paths, rv::world::RegenerationScope::ofRoom(kSnesRoom), *inputs.inputs);
        INFO(preview.error);
        REQUIRE(preview.failure.empty());
        WARN("preview " << run << ": read " << preview.readMs << " ms, regeneration " << preview.regenerateMs
                        << " ms, whole " << preview.totalMs << " ms (" << preview.content.gamesMoved
                        << " game(s) moved)");
    }
}

// The time of a room's preview in a large building (m3/preview-time, docs/i4b/preview-time.md): Gate M3 measured the
// real library's room previews at 2.3 to 4.1 s against 2 s, the cost being the world around the room (a large room
// alone previews in a tenth of a second, the bench above). A catalog of 90,000 generated entries
// (tests/support/catalog_generator.hpp, about the reference library's size) planned by the building job, then its
// largest room previewed three times on one set of inputs, each preview's read, regeneration and whole printed.
// Informative, by hand in Release:
//   build/msvc-x64-release/bin/rv_tests.exe "app/bench: a room's preview in a building*"
TEST_CASE("app/bench: a room's preview in a building of 90,000 generated entries", "[.bench]") {
    const rv::test::TempDir dir("rv-regenerate-bench");
    rv::test::CatalogGeneratorOptions options;
    options.entries = 90'000;
    auto generated = rv::test::generateCatalogDatabase(dir.file("retroverse.sqlite"), options, false);
    REQUIRE(generated.has_value());
    auto database = rv::catalog_db::openCatalogDatabase(dir.file("retroverse.sqlite"));
    REQUIRE(database.has_value());
    rv::app::WorldCatalogReader reader;
    reader.query = rv::catalog_db::SqliteCatalogQuery::open(database->openReader().value()).value();
    reader.profile = generated->library;
    reader.profile.esdeHome = dir.file("ES-DE");
    reader.profile.mediaRoot = dir.file("media");
    fs::create_directories(*reader.profile.mediaRoot);
    const rv::app::WorldPaths paths{rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), dir.file("data"),
                                    dir.file("data/config"), dir.file("data/cache")};
    rv::app::WorldBuildInput input;
    input.paths = paths;
    input.library = reader.profile.id;
    input.writer = "retroverse (bench)";
    input.snapshot = rv::app::takeBuildingSnapshot(reader, {}, rv::app::roomSystemsFilter(paths.assetRoot));
    const rv::app::BuildingResult built = rv::app::planBuilding(input, rv::core::JobContext{});
    REQUIRE(built.build.plan.has_value());
    const rv::world::WorldPlan& plan = *built.build.plan;
    std::map<rv::world::RoomId, std::size_t> placed;
    for (const rv::world::Placement& placement : plan.placements) {
        ++placed[placement.slot.fixture().room()];
    }
    REQUIRE_FALSE(placed.empty());
    const rv::world::RoomId room =
        std::max_element(placed.begin(), placed.end(), [](const auto& a, const auto& b) {
            return a.second < b.second;
        })->first;
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(paths, reader);
    INFO(inputs.error);
    REQUIRE(inputs.inputs);
    WARN("inputs: snapshot " << inputs.inputs->snapshotMs << " ms, selections " << inputs.inputs->selectMs
                             << " ms; the plan places " << plan.placements.size() << " in " << plan.rooms.size()
                             << " room(s), " << plan.unplaced.size() << " unplaced; " << room.toString() << " places "
                             << placed[room]);
    // The standing rooms' data read ahead with the inputs, as the directory's tab reads them.
    WARN("the standing rooms' data read ahead in " << rv::app::warmRoomData(*inputs.inputs, plan) << " ms");
    // Three previews reading the plan's file, then three taking the plan as the app holds it (rv::app::KnownPlan).
    const rv::app::KnownPlan known{std::make_shared<const rv::world::WorldPlan>(plan), built.build.planDigest};
    for (int run = 0; run < 6; ++run) {
        const rv::app::RegenerationPreview preview = rv::app::previewRegeneration(
            paths, rv::world::RegenerationScope::ofRoom(room), *inputs.inputs, run < 3 ? nullptr : &known);
        INFO(preview.error);
        REQUIRE(preview.failure.empty());
        CHECK_FALSE(preview.content.changed);
        CHECK(preview.knownPlan == (run >= 3));
        WARN("preview " << run << (preview.knownPlan ? " (the plan shown)" : " (the file parsed)") << ": read "
                        << preview.readMs << " ms, regeneration " << preview.regenerateMs << " ms, whole "
                        << preview.totalMs << " ms");
    }
}
