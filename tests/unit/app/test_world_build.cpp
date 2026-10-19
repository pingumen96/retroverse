// The generated room up to its commit, GPU-free (I3 step 10b, rv/app/room_snapshot.hpp and world_build.hpp): the room's
// snapshot as the catalog worker takes it (the bulk query, the media root checked, the media paths, the aliases of the
// placed games it lacks), the room's job on the pool (the world lock, the stored plan, a first plan or the incremental
// update, savePlan, the scene, the collection's layout, the cover requests) with its outcomes and notices, and the
// main thread's flow over a catalog and a worker pool (the test room without a catalog or without games of the room's
// system, the stored plan read, the snapshot asked with its placed games, the build taken once). I3 step 10c: the
// stored plan's room previewed before the snapshot is asked for and reconciled with its result, the room's items kept
// for it, the hero CRT's games of the room, an object fallback warned once. On the snes-room fixture
// (tests/fixtures/esde/snes-room: 50 titles with covers) imported into a user data directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rv/app/room_items.hpp>
#include <rv/app/room_screen.hpp>
#include <rv/app/room_snapshot.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_reconcile.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_test_support.hpp"
#include "support/log_capture.hpp"

namespace fs = std::filesystem;
using rv::app::RoomSnapshotRequest;
using rv::app::RoomSnapshotResult;
using rv::app::WorldBuild;
using rv::app::WorldOutcome;
using rv::test::ImportedFixture;

namespace {

const rv::catalog::SystemName kSnes = rv::catalog::SystemName::parse("snes").value();

rv::app::WorldPaths worldPathsOf(const ImportedFixture& imported) {
    return rv::app::WorldPaths{imported.paths.assetRoot(), imported.paths.userDataRoot(), imported.paths.configDir(),
                               imported.paths.cacheDir()};
}

/// The snapshot of the imported fixture's snes system as the catalog worker takes it, with `placed` asked for aliases.
RoomSnapshotResult snapshotOf(const ImportedFixture& imported, std::vector<rv::catalog::GameId> placed = {},
                              rv::catalog::SystemName system = kSnes) {
    auto database = imported.openDatabase();
    auto query = rv::catalog_db::SqliteCatalogQuery::open(database.openReader().value()).value();
    auto store = rv::catalog_db::SqliteCatalogStore::open(database).value();
    const rv::catalog::LibraryProfile library = rv::test::fixtureLibrary(imported.fixture);
    const auto settings = rv::esde::readSettings(library.esdeHome);
    REQUIRE(settings.has_value());
    RoomSnapshotRequest request;
    request.serial = 7;
    request.system = std::move(system);
    request.placedGames = std::move(placed);
    return rv::app::takeRoomSnapshot(*query, store.get(), rv::esde::resolveMediaRoot(*settings, library), library.id,
                                     "Lumen 16 Home System", request);
}

rv::app::WorldBuildInput inputOf(const ImportedFixture& imported, RoomSnapshotResult snapshot) {
    rv::app::WorldBuildInput input;
    input.paths = worldPathsOf(imported);
    input.library = rv::test::fixtureLibrary(imported.fixture).id;
    input.snapshot = std::move(snapshot);
    input.writer = "retroverse (test)";
    return input;
}

fs::path planFileOf(const ImportedFixture& imported) {
    return rv::world::worldFiles(imported.paths.userDataRoot(), rv::test::fixtureLibrary(imported.fixture).id).plan;
}

/// Renames the fixture's media folder away (the media root out of reach) for the object's life.
struct MediaAway {
    fs::path media;
    fs::path away;

    explicit MediaAway(const ImportedFixture& imported)
        : media(imported.fixture.path("media")), away(imported.fixture.path("media-away")) {
        fs::rename(media, away);
    }
    ~MediaAway() {
        std::error_code ignored;
        fs::rename(away, media, ignored);
    }
    MediaAway(const MediaAway&) = delete;
    MediaAway& operator=(const MediaAway&) = delete;
};

/// A catalog for the flow: its state, its systems, and the room's snapshot it answers with (from the fixture's
/// database) once asked.
class FakeRoomCatalog final : public rv::app::AppCatalog {
public:
    rv::app::CatalogState state = rv::app::CatalogState::Opening;
    bool snesGames = true;
    bool ended = false;
    rv::catalog::LibraryId library = rv::catalog::LibraryId::parse("esde").value();
    /// Makes the answer to a request (none: never answered).
    std::function<std::optional<RoomSnapshotResult>(const RoomSnapshotRequest&)> answer;
    std::vector<RoomSnapshotRequest> requests;

    [[nodiscard]] rv::ui::CatalogSource& source() noexcept override { return m_source; }
    [[nodiscard]] std::string_view notice() const noexcept override { return {}; }
    bool requestLaunch(std::uint64_t, const rv::ui::CatalogEntry&) override { return false; }
    [[nodiscard]] std::optional<rv::app::LaunchDecision> takeDecision() override { return std::nullopt; }
    [[nodiscard]] bool deciding() const noexcept override { return false; }
    rv::app::CatalogChanges poll() override { return {}; }
    [[nodiscard]] bool busy() const noexcept override { return false; }
    void record(rv::app::CatalogRecord&) const override {}

    [[nodiscard]] rv::app::CatalogState roomCatalogState() const noexcept override { return state; }
    [[nodiscard]] rv::catalog::LibraryId roomLibrary() const override { return library; }
    [[nodiscard]] bool hasGamesOf(const rv::catalog::SystemName& system) const override {
        return snesGames && system == kSnes;
    }
    bool requestRoomSnapshot(RoomSnapshotRequest request) override {
        if (state != rv::app::CatalogState::Ready) {
            return false;
        }
        requests.push_back(request);
        m_answer = answer ? answer(request) : std::nullopt;
        return true;
    }
    [[nodiscard]] std::optional<RoomSnapshotResult> takeRoomSnapshot() override {
        return std::exchange(m_answer, std::nullopt);
    }
    [[nodiscard]] bool workerEnded() const noexcept override { return ended; }

private:
    rv::ui::InMemoryCatalogSource m_source;
    std::optional<RoomSnapshotResult> m_answer;
};

/// Updates `flow` until it has its result (or 60 s pass), taking its preview (I3 step 10c) into `preview` when it
/// makes one, as the session shows it.
std::optional<rv::app::WorldResult> runFlow(rv::app::WorldFlow& flow, rv::app::AppCatalog& catalog,
                                            std::optional<WorldBuild>* preview = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        flow.update(catalog);
        if (auto taken = flow.takePreview(); taken && preview != nullptr) {
            *preview = std::move(taken);
        }
        if (auto result = flow.takeResult()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("app/room_snapshot: the catalog worker's job: the snapshot, the media root, the paths, the aliases",
          "[app][world_build]") {
    const ImportedFixture imported("room-snapshot", true, "esde/snes-room");
    const RoomSnapshotResult result = snapshotOf(imported);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.serial == 7);
    CHECK(result.system == kSnes);
    CHECK(result.library.str() == "esde");
    CHECK(result.systemFullName == "Lumen 16 Home System");
    CHECK(result.snapshot.mediaKnown);
    CHECK(result.snapshot.entries.size() == 55);
    // Every game with media has its paths (the two hacks have none), each cover named as the catalog derives it.
    CHECK(result.mediaPaths.size() == 53);
    REQUIRE(result.mediaRoot.has_value());
    CHECK(rv::core::pathToUtf8(result.mediaRoot->filename()) == "media");
    std::size_t covers = 0;
    for (const rv::catalog::RoomEntry& entry : result.snapshot.entries) {
        const std::string cover = rv::app::coverPathOf(result.mediaPaths, entry.id);
        if (!cover.empty()) {
            ++covers;
            CHECK(cover.starts_with("snes/covers/"));
        }
    }
    CHECK(covers == 52);
    CHECK(result.aliases.empty());
    CHECK_FALSE(result.aliasError.has_value());

    // Placed games the snapshot lists, and one it lacks that is no alias: no alias.
    const RoomSnapshotResult asked =
        snapshotOf(imported, {result.snapshot.entries.front().id, rv::catalog::GameId::fromValue(12345).value()});
    CHECK(asked.aliases.empty());
    CHECK_FALSE(asked.aliasError.has_value());

    // A system the catalog lacks.
    const RoomSnapshotResult missing = snapshotOf(imported, {}, rv::catalog::SystemName::parse("psx").value());
    REQUIRE(missing.error.has_value());
    CHECK(missing.error->code == rv::ErrorCode::NotFound);

    // The media root out of reach: the media unknown, the paths derived all the same (the cache serves them).
    const MediaAway away(imported);
    const RoomSnapshotResult unknown = snapshotOf(imported);
    REQUIRE_FALSE(unknown.error.has_value());
    CHECK_FALSE(unknown.snapshot.mediaKnown);
    CHECK_FALSE(unknown.snapshot.mediaUnknownReason.empty());
    CHECK(unknown.mediaPaths.size() == 53);
}

TEST_CASE("app/world_build: a first plan generated and saved as worldplan generate makes it, then reused unchanged",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-first", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported, snapshotOf(imported));
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(first.reason);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    CHECK(first.notice.empty());
    CHECK(first.written);
    CHECK_FALSE(first.saveError.has_value());
    CHECK(first.hardFindings.empty());
    CHECK(first.room.toString() == "room:snes");
    REQUIRE(first.plan.has_value());
    CHECK(first.plan->placements.size() == 50);
    REQUIRE(first.changes.has_value());
    CHECK(first.changes->added.size() == 50);
    // The plan saved is the one shown.
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    CHECK(stored->digest == first.planDigest);
    CHECK(stored->plan == *first.plan);
    // The same plan as world::generate of the same selection (the CLI's worldplan generate).
    const auto config = rv::world::loadWorldConfig(input.paths.assetRoot, input.paths.configDir).value().config;
    auto selection = rv::world::selectRoom(rv::world::RoomInput{input.library, input.snapshot.snapshot}, config);
    REQUIRE(selection.has_value());
    const auto data = rv::world::loadWorldData(input.paths.assetRoot, kSnes);
    REQUIRE(data.has_value());
    const auto generated =
        rv::world::generate(rv::world::GenerationInput{input.library, *selection, config, {}}, *data);
    REQUIRE(generated.has_value());
    CHECK(generated->changes.digestAfter == first.planDigest);
    // The scene and the collection: every placement an item, every item its cover request in item order.
    REQUIRE(first.scene.has_value());
    CHECK(first.scene->objects.size() > 10);
    CHECK_FALSE(first.scene->lights.empty());
    REQUIRE(first.sceneData.has_value());
    REQUIRE(first.layout.has_value());
    CHECK(first.layout->items.size() == 50);
    CHECK(first.layout->skipped.empty());
    REQUIRE(first.covers.size() == 50);
    for (std::size_t index = 0; index < first.covers.size(); ++index) {
        const rv::app::CoverItem& cover = first.covers[index];
        CHECK(cover.game == first.layout->items[index].game);
        CHECK(cover.frontKey == first.layout->items[index].frontLayerKey);
        CHECK(cover.spineKey == first.layout->items[index].spineLayerKey);
        CHECK_FALSE(cover.title.empty());
        CHECK_FALSE(cover.fallbackTitle.empty());
        CHECK(cover.systemName == "Lumen 16 Home System");
        CHECK(cover.coverPath.starts_with("snes/covers/"));
        CHECK_FALSE(cover.year.empty());
    }
    CHECK(first.mediaRoot.has_value());

    // A second start: the catalog unchanged, the plan unchanged, nothing written.
    const WorldBuild second = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(second.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(second.written);
    CHECK(second.planDigest == first.planDigest);
    REQUIRE(second.plan.has_value());
    CHECK(*second.plan == *first.plan);
    REQUIRE(second.changes.has_value());
    CHECK(second.changes->rebuild == rv::world::RebuildLevel::NoRebuild);
    CHECK(second.changes->kept == 50);
}

TEST_CASE("app/world_build: unknown media decide nothing: no first plan, a stored plan kept as it is",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-media", true, "esde/snes-room");
    {
        const MediaAway away(imported);
        const WorldBuild none = rv::app::buildWorld(inputOf(imported, snapshotOf(imported)), rv::core::JobContext{});
        CHECK(none.outcome == WorldOutcome::NoPlan);
        CHECK(none.notice == rv::app::world_notice::kMediaUnknown);
        CHECK_FALSE(none.plan.has_value());
        CHECK_FALSE(none.scene.has_value());
        CHECK_FALSE(none.written);
        CHECK_FALSE(fs::exists(planFileOf(imported)));
    }
    const WorldBuild first = rv::app::buildWorld(inputOf(imported, snapshotOf(imported)), rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    const auto before = rv::core::readTextFile(planFileOf(imported));
    REQUIRE(before.has_value());

    const MediaAway away(imported);
    const WorldBuild kept = rv::app::buildWorld(inputOf(imported, snapshotOf(imported)), rv::core::JobContext{});
    CHECK(kept.outcome == WorldOutcome::Kept);
    CHECK(kept.notice == rv::app::world_notice::kMediaUnknownKept);
    CHECK(kept.planDigest == first.planDigest);
    CHECK_FALSE(kept.changes.has_value());
    CHECK_FALSE(kept.written);
    // Shown with its covers asked of the cache alone.
    REQUIRE(kept.layout.has_value());
    CHECK(kept.layout->items.size() == 50);
    CHECK_FALSE(kept.mediaRoot.has_value());
    CHECK(kept.covers.size() == 50);
    CHECK(rv::core::readTextFile(planFileOf(imported)).value() == *before);
}

TEST_CASE("app/world_build: a plan edited by hand is refused and never overwritten; another seed is kept as it is",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-refused", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported, snapshotOf(imported));
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    const fs::path plan = planFileOf(imported);
    const std::string text = rv::core::readTextFile(plan).value();

    SECTION("edited by hand") {
        REQUIRE(rv::core::writeTextFileAtomic(plan, text + " ").has_value());
        const WorldBuild refused = rv::app::buildWorld(input, rv::core::JobContext{});
        CHECK(refused.outcome == WorldOutcome::NoPlan);
        CHECK(refused.notice == rv::app::world_notice::kRefused);
        CHECK_FALSE(refused.written);
        CHECK(rv::core::readTextFile(plan).value() == text + " ");
    }
    SECTION("another seed in world.json") {
        REQUIRE(rv::core::writeTextFileAtomic(imported.paths.configDir() / "world.json", "{\"seed\": 102938}\n")
                    .has_value());
        const WorldBuild kept = rv::app::buildWorld(input, rv::core::JobContext{});
        CHECK(kept.outcome == WorldOutcome::Kept);
        CHECK(kept.notice == rv::app::world_notice::kRegeneration);
        CHECK(kept.planDigest == first.planDigest);
        REQUIRE(kept.changes.has_value());
        CHECK(kept.changes->outcome == rv::world::GenerationOutcome::RequiresExplicitRegeneration);
        CHECK_FALSE(kept.written);
        CHECK(rv::core::readTextFile(plan).value() == text);
        CHECK(kept.layout.has_value());
    }
}

TEST_CASE("app/world_flow: the test room without a catalog or without games of the room's system",
          "[app][world_build]") {
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{1});
    const rv::test::TempDir dir("world-flow-test-room");
    rv::app::WorldFlow::Options options;
    options.paths = rv::app::WorldPaths{rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), dir.file("data"),
                                        dir.file("data/config"), dir.file("data/cache")};
    options.system = kSnes;
    options.writer = "retroverse (test)";

    SECTION("no catalog") {
        FakeRoomCatalog catalog;
        rv::app::WorldFlow flow(options, pool);
        flow.update(catalog);
        CHECK(flow.phase() == rv::app::WorldPhase::WaitingForCatalog);
        CHECK(flow.busy());
        catalog.state = rv::app::CatalogState::NoLibrary;
        const auto result = runFlow(flow, catalog);
        REQUIRE(result.has_value());
        CHECK(result->testRoom);
        CHECK(result->notice == rv::app::world_notice::kNoCatalog);
        CHECK(result->catalogState == "no-library");
        CHECK_FALSE(result->build.has_value());
        CHECK(flow.phase() == rv::app::WorldPhase::Done);
        CHECK_FALSE(flow.busy());
        CHECK_FALSE(flow.takeResult().has_value());
        CHECK(catalog.requests.empty());
    }
    SECTION("no games of the system") {
        FakeRoomCatalog catalog;
        catalog.state = rv::app::CatalogState::Ready;
        catalog.snesGames = false;
        rv::app::WorldFlow flow(options, pool);
        const auto result = runFlow(flow, catalog);
        REQUIRE(result.has_value());
        CHECK(result->testRoom);
        CHECK(result->notice == rv::app::world_notice::kNoGames);
        CHECK(catalog.requests.empty());
    }
    CHECK_FALSE(fs::exists(dir.file("data/worlds")));
}

TEST_CASE("app/world_flow: the stored plan read, the snapshot asked with its placed games, the room built once",
          "[app][world_build]") {
    const ImportedFixture imported("world-flow-room", true, "esde/snes-room");
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{2});
    rv::app::WorldFlow::Options options;
    options.paths = worldPathsOf(imported);
    options.system = kSnes;
    options.writer = "retroverse (test)";
    FakeRoomCatalog catalog;
    catalog.state = rv::app::CatalogState::Ready;
    catalog.answer = [&](const RoomSnapshotRequest& request) -> std::optional<RoomSnapshotResult> {
        RoomSnapshotResult result = snapshotOf(imported, request.placedGames);
        result.serial = request.serial;
        return result;
    };

    rv::app::WorldFlow first(options, pool);
    const auto built = runFlow(first, catalog);
    REQUIRE(built.has_value());
    CHECK_FALSE(built->testRoom);
    REQUIRE(built->build.has_value());
    CHECK(built->build->outcome == WorldOutcome::Generated);
    REQUIRE(catalog.requests.size() == 1);
    CHECK(catalog.requests.front().placedGames.empty());
    CHECK(first.times().snapshotMs.has_value());
    CHECK(first.times().builtMs.has_value());

    // The next start (I3 step 10c) makes the stored plan's room first, before the snapshot is even asked for; once it
    // is taken, asks the snapshot for the 50 games its stored plan places, and keeps the plan.
    rv::app::WorldFlow second(options, pool);
    std::optional<WorldBuild> preview;
    for (int step = 0; step < 30000 && !preview; ++step) {
        second.update(catalog);
        preview = second.takePreview();
        if (!preview) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    REQUIRE(preview.has_value());
    CHECK(catalog.requests.size() == 1); // the first flow's alone
    CHECK(second.phase() == rv::app::WorldPhase::Previewing);
    CHECK_FALSE(second.takePreview().has_value()); // once
    CHECK(preview->preview);
    CHECK(preview->outcome == WorldOutcome::Kept);
    CHECK(preview->planDigest == built->build->planDigest);
    REQUIRE(preview->layout.has_value());
    CHECK(preview->layout->items.size() == 50);
    CHECK(preview->covers == built->build->covers); // the room's items the first build kept
    CHECK_FALSE(preview->mediaRoot.has_value());
    CHECK(preview->screenGames.empty());
    second.update(catalog);
    CHECK(catalog.requests.size() == 2);
    const auto again = runFlow(second, catalog);
    REQUIRE(again.has_value());
    REQUIRE(again->build.has_value());
    CHECK(again->build->outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(again->build->preview);
    REQUIRE(catalog.requests.size() == 2);
    CHECK(catalog.requests.back().placedGames.size() == 50);
    CHECK(second.times().previewMs.has_value());
    CHECK(second.times().previewJobMs.has_value());
    const rv::app::ReconcilePlan reconciled = rv::app::planReconciliation(*preview, *again->build);
    CHECK(reconciled.kind == rv::app::Reconciliation::Textures);
    CHECK(reconciled.difference.kept == 50);
    CHECK(reconciled.difference.same());
    CHECK(reconciled.reloadCovers); // the media root known now: the covers' sources checked

    // A worker that ends before it answers: no room, its notice.
    catalog.answer = nullptr;
    rv::app::WorldFlow third(options, pool);
    for (int step = 0; step < 30000 && third.phase() != rv::app::WorldPhase::Snapshot; ++step) {
        third.update(catalog);
        static_cast<void>(third.takePreview());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(third.phase() == rv::app::WorldPhase::Snapshot);
    catalog.ended = true;
    const auto ended = runFlow(third, catalog);
    REQUIRE(ended.has_value());
    REQUIRE(ended->build.has_value());
    CHECK(ended->build->outcome == WorldOutcome::NoPlan);
    CHECK(ended->build->notice == rv::app::world_notice::kFailed);
}

// --- I3 step 10c: the persisted start, the hero CRT's games, the object fallbacks
// -------------------------------------

TEST_CASE("app/world_build: the room's items kept for the next start, written only when they change",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-items", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported, snapshotOf(imported));
    const fs::path items = rv::app::roomItemsFile(imported.paths.cacheDir(), input.library, kSnes);
    CHECK_FALSE(fs::exists(items));
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    CHECK(first.itemsWritten);
    CHECK_FALSE(first.itemsError.has_value());
    const auto kept = rv::app::readRoomItems(items);
    REQUIRE(kept.has_value());
    CHECK(kept->systemName == "Lumen 16 Home System");
    CHECK(kept->items.size() == 50);
    CHECK(*kept == rv::app::roomItemsOf(first.covers, *first.layout));
    const std::string text = rv::core::readTextFile(items).value();
    // Only the catalog's text and the covers' paths relative to the media root: no absolute path.
    CHECK(text.find(":\\\\") == std::string::npos);
    CHECK(text.find(":/") == std::string::npos);
    CHECK(text.find("snes/covers/") != std::string::npos);

    // An unchanged catalog writes nothing; the preview made from the stored plan and the items equals the room.
    const WorldBuild second = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(second.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(second.itemsWritten);
    CHECK(rv::core::readTextFile(items).value() == text);
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const WorldBuild preview = rv::app::previewWorld(worldPathsOf(imported), input.library, kSnes, *stored);
    INFO(preview.reason);
    CHECK(preview.preview);
    CHECK(preview.outcome == WorldOutcome::Kept);
    CHECK(preview.notice.empty());
    CHECK(preview.planDigest == second.planDigest);
    CHECK_FALSE(preview.changes.has_value());
    CHECK_FALSE(preview.written);
    CHECK_FALSE(preview.itemsWritten);
    CHECK(preview.itemsRead == 50);
    REQUIRE(preview.scene.has_value());
    REQUIRE(preview.layout.has_value());
    CHECK(rv::world::collectionLayoutToJson(*preview.layout) == rv::world::collectionLayoutToJson(*second.layout));
    CHECK(preview.covers == second.covers);
    CHECK_FALSE(preview.mediaRoot.has_value());
    CHECK(preview.screenGames.empty());
    CHECK(preview.objectFallbacks.empty());
    const rv::app::ReconcilePlan unchanged = rv::app::planReconciliation(preview, second);
    CHECK(unchanged.kind == rv::app::Reconciliation::Textures);
    CHECK(unchanged.difference == rv::app::LayoutDifference{50, 0, 0, 0, 0});
    CHECK(unchanged.reloadCovers);

    // Without the items (a cleared cache): the same room, its items pending (no cover request), none flagged.
    REQUIRE(fs::remove(items));
    const WorldBuild bare = rv::app::previewWorld(worldPathsOf(imported), input.library, kSnes, *stored);
    REQUIRE(bare.layout.has_value());
    CHECK(bare.itemsRead == 0);
    CHECK(bare.covers.empty());
    for (const rv::world::CollectionItem& item : bare.layout->items) {
        CHECK(item.flags == 0);
    }
    const rv::app::ReconcilePlan reloaded = rv::app::planReconciliation(bare, second);
    CHECK(reloaded.kind == rv::app::Reconciliation::Textures);
    CHECK(reloaded.difference.same());
    CHECK(reloaded.reloadCovers);

    // A broken file is left out (logged): the items pending, the room shown all the same.
    REQUIRE(rv::core::writeTextFileAtomic(items, "{ not json").has_value());
    const WorldBuild broken = rv::app::previewWorld(worldPathsOf(imported), input.library, kSnes, *stored);
    CHECK(broken.layout.has_value());
    CHECK(broken.covers.empty());
}

TEST_CASE("app/world_build: a game added between two starts: the stored room previewed, then rebuilt, nothing moved",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-added", true, "esde/snes-room");
    const WorldBuild first = rv::app::buildWorld(inputOf(imported, snapshotOf(imported)), rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    for (const std::string_view file :
         {"roms/snes/Zinnia Zephyr (USA).sfc", "media/snes/covers/Zinnia Zephyr (USA).png"}) {
        fs::copy_file(imported.fixture.path(std::string("additions/") + std::string(file)),
                      imported.fixture.path(file));
    }
    imported.importLibrary();
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const rv::app::WorldBuildInput input = inputOf(imported, snapshotOf(imported));
    const WorldBuild preview = rv::app::previewWorld(worldPathsOf(imported), input.library, kSnes, *stored);
    REQUIRE(preview.layout.has_value());
    CHECK(preview.layout->items.size() == 50);
    const WorldBuild updated = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(updated.outcome == WorldOutcome::Updated);
    CHECK(updated.itemsWritten);
    const rv::app::ReconcilePlan plan = rv::app::planReconciliation(preview, updated);
    CHECK(plan.kind == rv::app::Reconciliation::Rebuild);
    CHECK(plan.difference == rv::app::LayoutDifference{50, 0, 0, 1, 0});
    CHECK(rv::app::readRoomItems(rv::app::roomItemsFile(imported.paths.cacheDir(), input.library, kSnes))
              .value()
              .items.size() == 51);
}

TEST_CASE("app/room_screen: the room's hero CRT plays the most recently played placed game with a video, else the "
          "first featured one",
          "[app][world_build]") {
    const ImportedFixture imported("room-screen", true, "esde/snes-room");
    RoomSnapshotResult snapshot = snapshotOf(imported);
    const WorldBuild build = rv::app::buildWorld(inputOf(imported, snapshot), rv::core::JobContext{});
    REQUIRE(build.layout.has_value());
    REQUIRE(build.plan.has_value());
    // The build's games are those of its snapshot.
    CHECK(build.screenGames == rv::app::roomScreenGames(*build.plan, *build.layout, snapshot));
    const auto titleOf = [&](const std::string& id) {
        for (const rv::catalog::RoomEntry& entry : snapshot.snapshot.entries) {
            if (entry.id.toString() == id) {
                return entry.title;
            }
        }
        return std::string();
    };
    const std::vector<rv::app::ScreenGame>& games = build.screenGames;
    // Every game with a video, a screenshot, a title screen or a cover (the 50 placed, the two other releases, the one
    // without a cover); the hacks have no media.
    CHECK(games.size() == 53);
    std::size_t candidates = 0;
    for (std::size_t index = 0; index < games.size(); ++index) {
        const rv::app::ScreenGame& game = games[index];
        const auto* placed = build.layout->findGame(rv::catalog::GameId::parse(game.id).value());
        CHECK(game.startCandidate == (placed != nullptr));
        // The placed games first.
        if (game.startCandidate) {
            CHECK(index == candidates);
            ++candidates;
        }
        CHECK_FALSE(game.lastPlayed.has_value()); // the fixture has no session of Retroverse
    }
    CHECK(candidates == 50);
    // Never played: the first featured title with a video, the first by priority (a favourite, 42 plays).
    const std::optional<std::size_t> start = rv::app::chooseStartGame(games);
    REQUIRE(start.has_value());
    CHECK(titleOf(games[*start].id) == "Amber Lantern");
    CHECK(fs::exists(games[*start].media.video));
    CHECK(games[*start].media.video.extension() == ".mp4");
    CHECK_FALSE(games[*start].media.screenshot.empty());
    CHECK_FALSE(games[*start].media.titleScreen.empty());
    CHECK_FALSE(games[*start].media.cover.empty());
    // The game without a cover is no start candidate, though it has a clip: played when selected.
    const auto thistle = std::find_if(games.begin(), games.end(), [&](const rv::app::ScreenGame& game) {
        return titleOf(game.id) == "Thistle Trellis";
    });
    REQUIRE(thistle != games.end());
    CHECK_FALSE(thistle->startCandidate);
    CHECK_FALSE(thistle->media.video.empty());

    // The most recently played placed game with a video, by Retroverse's sessions; not an unplaced one.
    const auto played = [&](std::string_view title, int hour) {
        for (rv::catalog::RoomEntry& entry : snapshot.snapshot.entries) {
            if (entry.title == title) {
                entry.lastSession = std::chrono::sys_days{std::chrono::year{2026} / 10 / 1} + std::chrono::hours(hour);
            }
        }
    };
    played("Cobalt Compass", 9);
    played("Juniper Jamboree", 11);
    played("Thistle Trellis", 12);
    const std::vector<rv::app::ScreenGame> again = rv::app::roomScreenGames(*build.plan, *build.layout, snapshot);
    const std::optional<std::size_t> recent = rv::app::chooseStartGame(again);
    REQUIRE(recent.has_value());
    CHECK(titleOf(again[*recent].id) == "Juniper Jamboree");

    // The media unknown: nothing to play (the root out of reach is never read for them).
    snapshot.snapshot.mediaKnown = false;
    CHECK(rv::app::roomScreenGames(*build.plan, *build.layout, snapshot).empty());
}

TEST_CASE("app/world_build: a manifest removed from a scratch asset root falls back to its family's prefab, warned "
          "once, the room built",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-fallback", true, "esde/snes-room");
    // Style §26.3's "an asset lost on purpose": the hero-wall module's manifest removed from a copy of the assets.
    const fs::path assets = imported.dir.file("assets");
    fs::copy(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), assets, fs::copy_options::recursive);
    REQUIRE(fs::remove(rv::assets::prefabManifestPath(assets, "furniture.shelf.hero_module.generic")));
    rv::app::WorldBuildInput input = inputOf(imported, snapshotOf(imported));
    input.paths.assetRoot = assets;
    rv::test::LogCapture log;
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(first.reason);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    CHECK(first.hardFindings.empty());
    REQUIRE(first.layout.has_value());
    CHECK(first.layout->items.size() == 50);
    CHECK(first.layout->skipped.empty());
    REQUIRE(first.objectFallbacks.size() == 1);
    CHECK(first.objectFallbacks.front() ==
          rv::app::ObjectFallback{"furniture.shelf.hero_module.generic", "family generic", "the manifest is missing"});
    // The side-wall module stands in the hero wall's sockets: no hero band, the titles past the display slots
    // spine-out there.
    std::size_t band = 0;
    std::size_t spine = 0;
    for (const rv::world::CollectionItem& item : first.layout->items) {
        band += item.section == rv::world::Section::Band ? 1 : 0;
        spine += item.section == rv::world::Section::Spine ? 1 : 0;
    }
    CHECK(band == 0);
    CHECK(spine == 6);
    // The next start's preview and reconciliation resolve it again: still one warning in all.
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const WorldBuild preview = rv::app::previewWorld(input.paths, input.library, kSnes, *stored);
    CHECK(preview.layout.has_value());
    CHECK(preview.objectFallbacks == first.objectFallbacks);
    const WorldBuild second = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(second.outcome == WorldOutcome::Unchanged);
    CHECK(log.count("warning Prefab 'furniture.shelf.hero_module.generic' cannot be used") == 1);
}
