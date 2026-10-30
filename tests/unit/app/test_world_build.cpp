// The generated building up to its commit, GPU-free (I3 step 10b's room, I4 step 5a's building and floor;
// rv/app/room_snapshot.hpp and world_build.hpp): a system's snapshot (the bulk query, the media root checked, the media
// paths, the aliases of the placed games it lacks) and the building's (every system's within one read transaction on a
// reader of its own), the building job on the pool (the world lock, the stored plan, a first building or the update of
// its rooms, savePlan, each system's room items) and the floor's build (the floor's scene in the scene's frame,
// each collection room's layout and cover requests) with their outcomes and notices, and the main thread's flow over a
// catalog and a worker pool (the test room without a catalog or without a room for any system, the stored plan read,
// the preview first, the building and the floor built once). I3 step 10c: the stored plan's floor previewed before the
// building job and reconciled with its result, the rooms' items kept for it, the hero CRT's games of the start room, an
// object fallback warned once. I4 step 1: a stored schema-1 plan lifted and saved once in schema 2, a building of two
// rooms (the snes room updated, the other as it stood), one plan for the app and retroverse-cli's catalog commands, a
// room kept as it is with the regeneration's notice. I4 step 5a: the start room's frame (its scene numbers those of its
// standalone scene), a start room the building lacks, the building fixture's floors; at the merge with step 4 its shared
// and arcade rooms (their sections' covers and CRT games, the update by sections, a room held). I4 step 6b: the new
// systems entered by the app's job, the building stamp (an unchanged start plans nothing; none while a room waits for
// an update), the preview's CRT games. On the snes-room fixture (tests/fixtures/esde/snes-room: 50 titles with covers)
// and the building fixture imported into a user data directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
#include <rv/catalog_db/catalog_stamp.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/math.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>
#include <rv/world/building.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_store.hpp>

#include <nlohmann/json.hpp>

#include "../world/world_generation_support.hpp"
#include "catalog_test_support.hpp"
#include "support/log_capture.hpp"

namespace fs = std::filesystem;
using rv::app::BuildingSnapshot;
using rv::app::RoomCollection;
using rv::app::RoomSnapshotRequest;
using rv::app::RoomSnapshotResult;
using rv::app::WorldBuild;
using rv::app::WorldOutcome;
using rv::test::ImportedFixture;

namespace {

const rv::catalog::SystemName kSnes = rv::catalog::SystemName::parse("snes").value();
const rv::world::RoomId kSnesRoom = rv::world::RoomId::parse("room:snes").value();

rv::app::WorldPaths worldPathsOf(const ImportedFixture& imported) {
    return rv::app::WorldPaths{imported.paths.assetRoot(), imported.paths.userDataRoot(), imported.paths.configDir(),
                               imported.paths.cacheDir()};
}

/// The snapshot of the imported fixture's `system` as takeRoomSnapshot takes it, with `placed` asked for aliases.
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

/// The building job's reader on the imported fixture's catalog, as the catalog worker opens it.
std::shared_ptr<rv::app::WorldCatalogReader> readerOf(const ImportedFixture& imported) {
    auto database = imported.openDatabase();
    auto reader = std::make_shared<rv::app::WorldCatalogReader>();
    reader->query = rv::catalog_db::SqliteCatalogQuery::open(database.openReader().value()).value();
    reader->profile = rv::test::fixtureLibrary(imported.fixture);
    return reader;
}

/// Every system's snapshot of the imported fixture, as the building job takes it.
BuildingSnapshot buildingSnapshotOf(const ImportedFixture& imported, std::vector<rv::catalog::GameId> placed = {}) {
    const auto reader = readerOf(imported);
    return rv::app::takeBuildingSnapshot(*reader, placed, rv::app::roomSystemsFilter(imported.paths.assetRoot()));
}

/// The building job's input over the whole building, starting in `startRoom` (the snes room by default).
rv::app::WorldBuildInput inputOf(const ImportedFixture& imported, BuildingSnapshot snapshot,
                                 std::optional<rv::world::RoomId> startRoom = kSnesRoom) {
    rv::app::WorldBuildInput input;
    input.paths = worldPathsOf(imported);
    input.library = rv::test::fixtureLibrary(imported.fixture).id;
    input.snapshot = std::move(snapshot);
    input.writer = "retroverse (test)";
    input.startRoom = std::move(startRoom);
    return input;
}

rv::app::WorldBuildInput inputOf(const ImportedFixture& imported) {
    return inputOf(imported, buildingSnapshotOf(imported));
}

fs::path planFileOf(const ImportedFixture& imported) {
    return rv::world::worldFiles(imported.paths.userDataRoot(), rv::test::fixtureLibrary(imported.fixture).id).plan;
}

/// The collection room `room` of `build` (which must have it).
const RoomCollection& roomOf(const WorldBuild& build, const rv::world::RoomId& room = kSnesRoom) {
    const RoomCollection* found = build.findRoom(room);
    INFO(room.toString());
    REQUIRE(found != nullptr);
    return *found;
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

/// A catalog for the flow: its state, its library and the building job's reader.
class FakeCatalog final : public rv::app::AppCatalog {
public:
    rv::app::CatalogState state = rv::app::CatalogState::Opening;
    bool ended = false;
    rv::catalog::LibraryId library = rv::catalog::LibraryId::parse("esde").value();
    std::shared_ptr<rv::app::WorldCatalogReader> reader;

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
    [[nodiscard]] std::shared_ptr<rv::app::WorldCatalogReader> worldReader() const override { return reader; }
    [[nodiscard]] bool workerEnded() const noexcept override { return ended; }

private:
    rv::ui::InMemoryCatalogSource m_source;
};

/// The schema-1 text I3 wrote for the one-room schema-2 plan `text`: no floors, the room without the building's fields,
/// generator 1.2, canonical (the same elements: generator 1.3 places a console room as 1.2 did).
std::string schemaOneText(const std::string& text) {
    nlohmann::json json = nlohmann::json::parse(text);
    json["schemaVersion"] = 1;
    json["generatorVersion"] = "1.2";
    json.erase("floors");
    for (nlohmann::json& room : json["rooms"]) {
        for (const char* key : {"expands", "floor", "generatorVersion", "kind", "placement", "sections"}) {
            room.erase(key);
        }
    }
    auto written = rv::core::writeCanonicalJson(json);
    REQUIRE(written.has_value());
    return *written;
}

/// The texts of the plans in the world's backups/ folder.
std::vector<std::string> backupTexts(const ImportedFixture& imported) {
    std::vector<std::string> texts;
    const fs::path folder =
        rv::world::worldFiles(imported.paths.userDataRoot(), rv::test::fixtureLibrary(imported.fixture).id).backups;
    if (fs::exists(folder)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
            texts.push_back(rv::core::readTextFile(entry.path()).value());
        }
    }
    return texts;
}

/// retroverse-cli run in this process on the imported fixture's data directory (stdout, stderr, the exit code).
struct CliRun {
    rv::cli::ExitCode code = rv::cli::ExitCode::Ok;
    std::string out;
    std::string err;
};

CliRun runCli(const ImportedFixture& imported, std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(imported.data)});
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    rv::cli::Output out = rv::cli::Output::memory();
    rv::cli::Output err = rv::cli::Output::memory();
    const rv::cli::ExitCode code = rv::cli::run(arguments, out, err, environment);
    // The dispatcher configures the logging for its run: the plain console logger back afterwards.
    rv::core::LogConfig config;
    config.console = false;
    rv::core::initLogging(config);
    rv::core::shutdownLogging();
    return CliRun{code, out.text(), err.text()};
}

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

/// The object of `description` whose id is `id`, or nullptr.
const rv::scene::SceneObject* objectOf(const rv::scene::SceneDescription& description, const std::string& id) {
    const auto found = std::find_if(description.objects.begin(), description.objects.end(),
                                    [&](const rv::scene::SceneObject& object) { return object.id == id; });
    return found != description.objects.end() ? &*found : nullptr;
}

} // namespace

TEST_CASE("app/room_snapshot: a system's snapshot: the snapshot, the media root, the paths, the aliases",
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

TEST_CASE("app/room_snapshot: the building's snapshot: every system with a room in one read, its own reader",
          "[app][world_build]") {
    const ImportedFixture imported("building-snapshot", true, "esde/building");
    const BuildingSnapshot snapshot = buildingSnapshotOf(imported);
    REQUIRE_FALSE(snapshot.error.has_value());
    CHECK(snapshot.library.str() == "esde");
    // Every system of the library with a room, by name: kodi is excluded (ADR-0013 §2); megadrive is known to the
    // catalog (es_systems.xml) without entries yet (its folder comes with additions/).
    std::vector<std::string> systems;
    for (const RoomSnapshotResult& system : snapshot.systems) {
        INFO(system.system.str());
        CHECK_FALSE(system.error.has_value());
        CHECK(system.snapshot.mediaKnown);
        systems.push_back(system.system.str());
    }
    CHECK(systems ==
          std::vector<std::string>{"amiga", "gb", "mame", "megadrive", "pocketlab", "pokemini", "psx", "snes"});
    CHECK(snapshot.find(rv::catalog::SystemName::parse("megadrive").value())->snapshot.entries.empty());
    CHECK(snapshot.mediaKnown());
    const RoomSnapshotResult* pocketlab = snapshot.find(rv::catalog::SystemName::parse("pocketlab").value());
    REQUIRE(pocketlab != nullptr);
    // The catalog's full names and platform tags (a custom system classified by them).
    CHECK(pocketlab->platformTags == std::vector<std::string>{"gb"});
    CHECK(pocketlab->systemFullName == "Pocket Lab Homebrew");
    CHECK(pocketlab->snapshot.entries.size() == 6);
    CHECK(snapshot.aliases.empty());

    // Without a filter every system of the library, the excluded one too.
    const auto reader = readerOf(imported);
    const BuildingSnapshot all = rv::app::takeBuildingSnapshot(*reader, {}, {});
    CHECK(all.systems.size() == 9);
    CHECK(all.find(rv::catalog::SystemName::parse("kodi").value()) != nullptr);

    // The media root out of reach: every system's media unknown.
    const MediaAway away(imported);
    const BuildingSnapshot unknown = buildingSnapshotOf(imported);
    REQUIRE_FALSE(unknown.error.has_value());
    CHECK_FALSE(unknown.mediaKnown());
}

TEST_CASE("app/world_build: a first plan generated and saved as worldplan generate makes it, then reused unchanged",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-first", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(first.reason);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    CHECK(first.notice.empty());
    CHECK(first.written);
    CHECK_FALSE(first.saveError.has_value());
    CHECK(first.hardFindings.empty());
    CHECK(first.startRoom == std::optional<rv::world::RoomId>(kSnesRoom));
    CHECK_FALSE(first.startRoomMissing);
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
    const RoomSnapshotResult* snes = input.snapshot.find(kSnes);
    REQUIRE(snes != nullptr);
    auto selection = rv::world::selectRoom(rv::world::RoomInput{input.library, snes->snapshot}, config);
    REQUIRE(selection.has_value());
    const auto data = rv::world::loadWorldData(input.paths.assetRoot, kSnes);
    REQUIRE(data.has_value());
    const auto generated =
        rv::world::generate(rv::world::GenerationInput{input.library, *selection, config, {}}, *data);
    REQUIRE(generated.has_value());
    CHECK(generated->changes.digestAfter == first.planDigest);
    // The floor: gen4, the snes room alone, its scene with the hall; its collection, every placement an item, every
    // item its cover request in item order.
    CHECK(first.floor == "gen4");
    REQUIRE(first.scene.has_value());
    CHECK(first.scene->objects.size() > 10);
    CHECK_FALSE(first.scene->lights.empty());
    CHECK(first.scene->areas.size() == 2);
    CHECK_FALSE(first.sceneDigest.empty());
    REQUIRE(first.rooms.size() == 1);
    const RoomCollection& room = roomOf(first);
    CHECK(room.scenePlacement == rv::world::RoomPlacement{});
    CHECK(room.placement == first.plan->rooms.front().placement);
    CHECK(room.layout.items.size() == 50);
    CHECK(room.layout.skipped.empty());
    CHECK_FALSE(room.doorway.isEmpty());
    REQUIRE(room.covers.size() == 50);
    for (std::size_t index = 0; index < room.covers.size(); ++index) {
        const rv::app::CoverItem& cover = room.covers[index];
        CHECK(cover.game == room.layout.items[index].game);
        CHECK(cover.frontKey == room.layout.items[index].frontLayerKey);
        CHECK(cover.spineKey == room.layout.items[index].spineLayerKey);
        CHECK_FALSE(cover.title.empty());
        CHECK_FALSE(cover.fallbackTitle.empty());
        CHECK(cover.systemName == "Lumen 16 Home System");
        CHECK(cover.coverPath.starts_with("snes/covers/"));
        CHECK_FALSE(cover.year.empty());
    }
    CHECK(first.mediaRoot.has_value());
    CHECK(first.itemsRead == 50);

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
    CHECK(second.sceneDigest == first.sceneDigest);
}

TEST_CASE("app/world_build: the start room's frame: the room's scene numbers its standalone scene's, the hall moved",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-frame", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
    const WorldBuild framed = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(framed.outcome == WorldOutcome::Generated);
    REQUIRE(framed.plan.has_value());
    REQUIRE(framed.scene.has_value());
    // The room's standalone scene (worldplan show --scene): every object of the room in the floor's scene, moved back
    // into the room's frame, stands exactly where it stands there; its camera is the doorway's.
    const fs::path assets = input.paths.assetRoot;
    const auto lighting = rv::world::roomLightingProfile(assets, input.paths.configDir);
    auto data = rv::world::loadSceneData(assets, *framed.plan, kSnesRoom, lighting);
    REQUIRE(data.has_value());
    auto standalone = rv::world::toSceneDescription(*framed.plan, kSnesRoom, *data);
    REQUIRE(standalone.has_value());
    std::size_t matched = 0;
    for (const rv::scene::SceneObject& object : standalone->objects) {
        const rv::scene::SceneObject* onFloor = objectOf(*framed.scene, "room:snes/" + object.id);
        INFO(object.id);
        REQUIRE(onFloor != nullptr);
        CHECK(onFloor->transform == object.transform);
        ++matched;
    }
    CHECK(matched == standalone->objects.size());
    CHECK(framed.scene->camera.position == standalone->camera.position);
    CHECK(framed.scene->camera.yawDeg == standalone->camera.yawDeg);
    CHECK(framed.scene->camera.pitchDeg == standalone->camera.pitchDeg);
    // The room's lights likewise, in the room's area.
    for (const rv::scene::Light& light : standalone->lights) {
        const auto found = std::find_if(framed.scene->lights.begin(), framed.scene->lights.end(),
                                        [&](const rv::scene::Light& candidate) {
                                            return candidate.name == "room:snes/" + light.name;
                                        });
        INFO(light.name);
        REQUIRE(found != framed.scene->lights.end());
        CHECK(found->position == light.position);
    }

    // Without a start room the scene is the floor's frame (here the ground floor's lobby: the snes room is not on it).
    const WorldBuild lobby = rv::app::buildWorld(inputOf(imported, buildingSnapshotOf(imported), std::nullopt),
                                                 rv::core::JobContext{});
    CHECK(lobby.outcome == WorldOutcome::Unchanged);
    CHECK(lobby.floor == "ground");
    REQUIRE(lobby.scene.has_value());
    CHECK(lobby.rooms.empty());
    CHECK(lobby.screenGames.empty());
    CHECK(lobby.scene->camera.position.x > 1.0f); // the elevator's arrival point, facing east
    CHECK(lobby.scene->camera.yawDeg == 90.0f);

    // The floor's frame on gen4: the room's layout moved by its placement.
    const rv::world::RoomPlacement placement = framed.plan->rooms.front().placement;
    const rv::world::RoomPlacement relative = rv::app::relativePlacement(placement, placement);
    CHECK(relative == rv::world::RoomPlacement{});
    const rv::world::RoomPlacement turned{5000, 6000, 1};
    const rv::world::RoomPlacement other{20000, -6000, 0};
    // A room at `other` seen from a half-turned start room: a half turn and the mirrored offset.
    CHECK(rv::app::relativePlacement(other, turned) == rv::world::RoomPlacement{-15000, 12000, 1});
    CHECK(rv::app::relativePlacement(turned, turned) == rv::world::RoomPlacement{});

    // A start room the building lacks: no floor, said for the run to end (exit 2).
    const WorldBuild missing =
        rv::app::buildWorld(inputOf(imported, buildingSnapshotOf(imported), rv::world::RoomId::parse("room:psx").value()),
                            rv::core::JobContext{});
    CHECK(missing.plan.has_value());
    CHECK(missing.startRoomMissing);
    CHECK_FALSE(missing.scene.has_value());
}

TEST_CASE("app/world_build: a ride's floor: any floor of the plan, the elevator's zone and arrival, each room's doorway",
          "[app][world_build]") {
    // I4 step 7b: the floor-build job of a ride builds the floor asked for (WorldBuild::requestedFloor) in the floor's
    // frame, or in the start room's on the start room's floor; with its elevator's proximity zone, its arrival camera
    // and every room's doorway camera in that frame.
    const ImportedFixture imported("world-build-ride", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
    const WorldBuild framed = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(framed.plan.has_value());
    REQUIRE(framed.scene.has_value());
    const rv::world::Room& snes = framed.plan->rooms.front();
    REQUIRE(snes.id == kSnesRoom);
    // The start room's doorway camera is its standalone scene's (the start's camera).
    REQUIRE(framed.rooms.size() == 1);
    CHECK(framed.rooms.front().doorwayCamera.position == framed.scene->camera.position);
    CHECK(framed.rooms.front().doorwayCamera.yawDeg == framed.scene->camera.yawDeg);
    CHECK(framed.rooms.front().doorwayCamera.pitchDeg == framed.scene->camera.pitchDeg);
    // The arrival camera and the zone: in front of the elevator at the hall's west end, moved into the room's frame.
    const glm::vec3 hallOrigin(-static_cast<float>(snes.placement.xMm) / 1000.0f, 0.0f,
                               -static_cast<float>(snes.placement.zMm) / 1000.0f);
    CHECK(glm::length(framed.arrival.position - (hallOrigin + glm::vec3(1.2f, 1.65f, 0.0f))) < 1e-5f);
    CHECK(framed.arrival.yawDeg == 90.0f);
    CHECK(glm::length(framed.elevatorZone.min - (hallOrigin + glm::vec3(0.0f, 0.0f, -1.0f))) < 1e-5f);
    CHECK(glm::length(framed.elevatorZone.max - (hallOrigin + glm::vec3(1.5f, 2.2f, 1.0f))) < 1e-5f);
    // The doors' middle: the elevator's frame on the west wall, 50 mm proud of it.
    CHECK(glm::length(framed.elevatorDoors - (hallOrigin + glm::vec3(0.025f, 0.0f, 0.0f))) < 1e-5f);

    const rv::app::FloorSources sources;
    const auto floorOf = [&](std::optional<rv::world::RoomId> startRoom, std::string floor) {
        WorldBuild build;
        build.library = framed.library;
        build.plan = framed.plan;
        build.planDigest = framed.planDigest;
        build.startRoom = std::move(startRoom);
        build.requestedFloor = std::move(floor);
        rv::app::buildFloor(input.paths, build, sources);
        return build;
    };
    // gen4 without a start room: the floor's frame, the room's doorway moved by its placement.
    const WorldBuild gen4 = floorOf(std::nullopt, snes.floor);
    CHECK(gen4.floor == snes.floor);
    CHECK(gen4.floorPlacement == rv::world::RoomPlacement{});
    REQUIRE(gen4.scene.has_value());
    REQUIRE(gen4.rooms.size() == 1);
    const glm::vec3 doorway = glm::vec3(rv::world::roomToFloorMatrix(snes.placement) *
                                        glm::vec4(framed.scene->camera.position, 1.0f));
    CHECK(glm::length(gen4.rooms.front().doorwayCamera.position - doorway) < 1e-5f);
    CHECK(gen4.rooms.front().doorwayCamera.yawDeg == framed.scene->camera.yawDeg);
    CHECK(glm::length(gen4.arrival.position - glm::vec3(1.2f, 1.65f, 0.0f)) < 1e-5f);
    CHECK(gen4.scene->camera.position == gen4.arrival.position);
    // The same floor asked with the start room: the start room's frame, as the start built it.
    const WorldBuild again = floorOf(kSnesRoom, snes.floor);
    CHECK(again.sceneDigest == framed.sceneDigest);
    CHECK(again.rooms.front().doorwayCamera.position == framed.rooms.front().doorwayCamera.position);
    // Another floor with a start room: the floor's frame (the lobby).
    const WorldBuild ground = floorOf(kSnesRoom, std::string(rv::world::kGroundFloor));
    CHECK(ground.floor == rv::world::kGroundFloor);
    CHECK(ground.floorPlacement == rv::world::RoomPlacement{});
    CHECK(ground.rooms.empty());
    CHECK(glm::length(ground.arrival.position - glm::vec3(1.2f, 1.65f, 0.0f)) < 1e-5f);
    CHECK(glm::length(ground.elevatorZone.min - glm::vec3(0.0f, 0.0f, -1.0f)) < 1e-5f);
    CHECK(glm::length(ground.elevatorDoors - glm::vec3(0.025f, 0.0f, 0.0f)) < 1e-5f);
    CHECK(glm::length(gen4.elevatorDoors - glm::vec3(0.025f, 0.0f, 0.0f)) < 1e-5f);
    // A floor the plan lacks: nothing to show.
    const WorldBuild missing = floorOf(std::nullopt, "gen7");
    CHECK_FALSE(missing.plan.has_value());
    CHECK_FALSE(missing.scene.has_value());
    CHECK(missing.notice == rv::app::world_notice::kFailed);
}

TEST_CASE("app/world_build: moving a floor's scene file into a half-turned room's frame and back is exact",
          "[app][world_build]") {
    // A floor's scene numbers as toFloorSceneFile writes them for a south-row room at (12.35, 6.65), then moved into
    // that room's frame: its own objects get back their numbers.
    nlohmann::json scene{
        {"camera", {{"position", {1.2, 1.65, 0.0}}, {"yawDeg", 90.0}, {"pitchDeg", -6.0}}},
        {"areas",
         {{{"id", "room:gb"}, {"boxes", {{{"min", {7.85, 0.0, 1.65}}, {"max", {16.85, 3.2, 11.65}}}}}}}},
        {"lights", {{{"name", "room:gb/key"}, {"position", {10.35, 3.0, 2.65}}, {"target", {9.35, 0.0, 8.65}}}}},
        {"objects",
         {{{"id", "room:gb/fx:a"}, {"position", {10.35, 0.0, 2.65}}, {"rotationDeg", {0.0, 135.5, 0.0}}},
          {{"id", "room:gb/room"}, {"position", {12.35, 0.0, 6.65}}, {"rotationDeg", {0.0, -180.0, 0.0}}}}}};
    rv::app::moveSceneFileToRoom(scene, rv::world::RoomPlacement{12350, 6650, 1});
    const nlohmann::json& object = scene["objects"][0];
    CHECK(object["position"][0].get<double>() == 2.0);
    CHECK(object["position"][2].get<double>() == 4.0);
    CHECK(object["rotationDeg"][1].get<double>() == -44.5);
    CHECK(scene["objects"][1]["position"][0].get<double>() == 0.0);
    CHECK(scene["objects"][1]["rotationDeg"][1].get<double>() == 0.0);
    CHECK(scene["lights"][0]["position"][0].get<double>() == 2.0);
    CHECK(scene["lights"][0]["target"][2].get<double>() == -2.0);
    // A box turned stays a box: its corners swapped.
    CHECK(scene["areas"][0]["boxes"][0]["min"][0].get<double>() == -4.5);
    CHECK(scene["areas"][0]["boxes"][0]["max"][0].get<double>() == 4.5);
    CHECK(scene["areas"][0]["boxes"][0]["min"][2].get<double>() == -5.0);
    CHECK(scene["areas"][0]["boxes"][0]["max"][2].get<double>() == 5.0);
    CHECK(scene["camera"]["yawDeg"].get<double>() == -90.0);
}

TEST_CASE("app/world_build: unknown media decide nothing: no first plan, a stored plan kept as it is",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-media", true, "esde/snes-room");
    {
        const MediaAway away(imported);
        const WorldBuild none = rv::app::buildWorld(inputOf(imported), rv::core::JobContext{});
        CHECK(none.outcome == WorldOutcome::NoPlan);
        CHECK(none.notice == rv::app::world_notice::kMediaUnknown);
        CHECK_FALSE(none.plan.has_value());
        CHECK_FALSE(none.scene.has_value());
        CHECK_FALSE(none.written);
        CHECK_FALSE(fs::exists(planFileOf(imported)));
    }
    const WorldBuild first = rv::app::buildWorld(inputOf(imported), rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    const auto before = rv::core::readTextFile(planFileOf(imported));
    REQUIRE(before.has_value());

    const MediaAway away(imported);
    const WorldBuild kept = rv::app::buildWorld(inputOf(imported), rv::core::JobContext{});
    CHECK(kept.outcome == WorldOutcome::Kept);
    CHECK(kept.notice == rv::app::world_notice::kMediaUnknownKept);
    CHECK(kept.planDigest == first.planDigest);
    CHECK_FALSE(kept.changes.has_value());
    CHECK_FALSE(kept.written);
    // Shown with its covers asked of the cache alone (the items the first build kept).
    const RoomCollection& room = roomOf(kept);
    CHECK(room.layout.items.size() == 50);
    CHECK_FALSE(kept.mediaRoot.has_value());
    CHECK(room.covers.size() == 50);
    CHECK(rv::core::readTextFile(planFileOf(imported)).value() == *before);
}

TEST_CASE("app/world_build: a plan edited by hand is refused and never overwritten; another seed is kept as it is",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-refused", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
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
        CHECK(kept.findRoom(kSnesRoom) != nullptr);
    }
}

TEST_CASE("app/world_flow: the test room without a catalog or without a room for any system", "[app][world_build]") {
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{1});
    const rv::test::TempDir dir("world-flow-test-room");
    rv::app::WorldFlow::Options options;
    options.paths = rv::app::WorldPaths{rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), dir.file("data"),
                                        dir.file("data/config"), dir.file("data/cache")};
    options.writer = "retroverse (test)";

    SECTION("no catalog") {
        FakeCatalog catalog;
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
    }
    SECTION("a catalog without a reader for the building") {
        FakeCatalog catalog;
        catalog.state = rv::app::CatalogState::Ready;
        rv::app::WorldFlow flow(options, pool);
        const auto result = runFlow(flow, catalog);
        REQUIRE(result.has_value());
        CHECK_FALSE(result->testRoom);
        REQUIRE(result->build.has_value());
        CHECK(result->build->outcome == WorldOutcome::NoPlan);
        CHECK(result->build->notice == rv::app::world_notice::kFailed);
    }
    CHECK_FALSE(fs::exists(dir.file("data/worlds")));
}

TEST_CASE("app/world_flow: a catalog whose systems give no room shows the test room", "[app][world_build]") {
    // An empty catalog (imported nothing): no system with a placeable title, no plan stored.
    const ImportedFixture imported("world-flow-no-games", false, "esde/snes-room");
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{1});
    rv::app::WorldFlow::Options options;
    options.paths = worldPathsOf(imported);
    options.writer = "retroverse (test)";
    FakeCatalog catalog;
    catalog.state = rv::app::CatalogState::Ready;
    catalog.reader = readerOf(imported);
    rv::app::WorldFlow flow(options, pool);
    const auto result = runFlow(flow, catalog);
    REQUIRE(result.has_value());
    CHECK(result->testRoom);
    CHECK(result->notice == rv::app::world_notice::kNoGames);
    // Nothing written into the data directory: no plan, not even the world's lock.
    CHECK_FALSE(fs::exists(planFileOf(imported).parent_path()));
}

TEST_CASE("app/world_flow: the stored plan read, the preview first, the building and its floor built once",
          "[app][world_build]") {
    const ImportedFixture imported("world-flow-room", true, "esde/snes-room");
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{2});
    rv::app::WorldFlow::Options options;
    options.paths = worldPathsOf(imported);
    options.startRoom = kSnesRoom;
    options.writer = "retroverse (test)";
    FakeCatalog catalog;
    catalog.state = rv::app::CatalogState::Ready;
    catalog.reader = readerOf(imported);

    rv::app::WorldFlow first(options, pool);
    const auto built = runFlow(first, catalog);
    REQUIRE(built.has_value());
    CHECK_FALSE(built->testRoom);
    REQUIRE(built->build.has_value());
    CHECK(built->build->outcome == WorldOutcome::Generated);
    CHECK(built->build->floor == "gen4");
    CHECK(built->build->scene.has_value());
    CHECK(first.times().snapshotMs.has_value());
    CHECK(first.times().snapshotWorkerMs.has_value());
    CHECK(first.times().snapshotAt.has_value());
    CHECK(first.times().builtMs.has_value());
    CHECK_FALSE(first.times().previewMs.has_value());

    // The next start (I3 step 10c) makes the stored plan's floor first, before the building job; once it is taken,
    // the building job runs, and keeps the plan.
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
    CHECK(second.phase() == rv::app::WorldPhase::Previewing);
    CHECK_FALSE(second.takePreview().has_value()); // once
    CHECK(preview->preview);
    CHECK(preview->outcome == WorldOutcome::Kept);
    CHECK(preview->planDigest == built->build->planDigest);
    CHECK(preview->sceneDigest == built->build->sceneDigest);
    CHECK(roomOf(*preview).layout.items.size() == 50);
    CHECK(roomOf(*preview).covers == roomOf(*built->build).covers); // the room's items the first build kept
    CHECK_FALSE(preview->mediaRoot.has_value());
    // Its CRT's games from the room's items, under the library's media root (I4 step 6b).
    CHECK(preview->screenGames.size() == 50);
    second.update(catalog);
    CHECK(second.phase() == rv::app::WorldPhase::Building);
    const auto again = runFlow(second, catalog);
    REQUIRE(again.has_value());
    REQUIRE(again->build.has_value());
    CHECK(again->build->outcome == WorldOutcome::Unchanged);
    CHECK(again->build->skipped); // I4 step 6b: the building stamp of the first building
    CHECK_FALSE(again->build->preview);
    CHECK(again->build->screenGames.size() == 50);
    CHECK(again->build->screenGames == preview->screenGames);
    CHECK(second.times().previewMs.has_value());
    CHECK(second.times().previewJobMs.has_value());
    const rv::app::ReconcilePlan reconciled = rv::app::planReconciliation(*preview, *again->build);
    CHECK(reconciled.kind == rv::app::Reconciliation::Textures);
    CHECK(reconciled.difference.kept == 50);
    CHECK(reconciled.difference.same());
    CHECK(reconciled.reloadCovers); // the media root known now: the covers' sources checked

    // A start room the stored building lacks: no preview, the building's result says so (the run ends: exit 2).
    options.startRoom = rv::world::RoomId::parse("room:psx").value();
    rv::app::WorldFlow third(options, pool);
    std::optional<WorldBuild> none;
    const auto missing = runFlow(third, catalog, &none);
    REQUIRE(missing.has_value());
    REQUIRE(missing->build.has_value());
    CHECK(missing->build->startRoomMissing);
    if (none) {
        CHECK_FALSE(none->scene.has_value());
        CHECK(none->startRoomMissing);
    }
}

// --- I3 step 10c: the persisted start, the hero CRT's games, the object fallbacks
// -------------------------------------

TEST_CASE("app/world_build: the rooms' items kept for the next start, written only when they change",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-items", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
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
    // The items the floor's covers were made from, with the system's platform tags, and their CRT's data (I4 step 6b).
    std::vector<rv::app::RoomItemText> withoutScreens = kept->items;
    std::size_t screens = 0;
    for (rv::app::RoomItemText& item : withoutScreens) {
        screens += item.screen ? 1 : 0;
        item.screen.reset();
    }
    CHECK(screens == 50);
    CHECK(withoutScreens == rv::app::roomItemsOf(roomOf(first).covers, roomOf(first).layout).items);
    CHECK(kept->platforms == std::vector<std::string>{"snes"});
    const std::string text = rv::core::readTextFile(items).value();
    // Only the catalog's text and the covers' paths relative to the media root: no absolute path.
    CHECK(text.find(":\\\\") == std::string::npos);
    CHECK(text.find(":/") == std::string::npos);
    CHECK(text.find("snes/covers/") != std::string::npos);

    // An unchanged catalog writes nothing; the preview made from the stored plan and the items equals the floor.
    const WorldBuild second = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(second.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(second.itemsWritten);
    CHECK(rv::core::readTextFile(items).value() == text);
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const WorldBuild preview = rv::app::previewWorld(worldPathsOf(imported), input.library, *stored, kSnesRoom);
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
    CHECK(preview.sceneDigest == second.sceneDigest);
    CHECK(rv::world::collectionLayoutToJson(roomOf(preview).layout) ==
          rv::world::collectionLayoutToJson(roomOf(second).layout));
    CHECK(roomOf(preview).covers == roomOf(second).covers);
    CHECK_FALSE(preview.mediaRoot.has_value());
    CHECK(preview.screenGames.empty()); // no media root for the CRT's games
    CHECK(preview.objectFallbacks.empty());
    const rv::app::ReconcilePlan unchanged = rv::app::planReconciliation(preview, second);
    CHECK(unchanged.kind == rv::app::Reconciliation::Textures);
    CHECK(unchanged.difference == rv::app::LayoutDifference{50, 0, 0, 0, 0});
    CHECK(unchanged.reloadCovers);
    // With the media root for the CRT (I4 step 6b), the preview's CRT games are the building's, from the items alone;
    // its covers still come from the cache alone.
    REQUIRE(second.mediaRoot.has_value());
    CHECK(rv::app::readableMediaRoot(rv::test::fixtureLibrary(imported.fixture)) == second.mediaRoot);
    const WorldBuild withScreen =
        rv::app::previewWorld(worldPathsOf(imported), input.library, *stored, kSnesRoom, second.mediaRoot);
    CHECK_FALSE(withScreen.mediaRoot.has_value());
    CHECK(withScreen.screenGames.size() == 50);
    CHECK(withScreen.screenGames == second.screenGames);
    CHECK(roomOf(withScreen).screenGames == roomOf(second).screenGames);
    CHECK(roomOf(withScreen).contentDigest == roomOf(second).contentDigest);
    CHECK(roomOf(withScreen).contentDigest == rv::world::roomContentDigest(*second.plan, kSnesRoom).value());

    // Without the items (a cleared cache): the same floor, its items pending (no cover request), none flagged.
    REQUIRE(fs::remove(items));
    const WorldBuild bare = rv::app::previewWorld(worldPathsOf(imported), input.library, *stored, kSnesRoom);
    CHECK(bare.itemsRead == 0);
    CHECK(roomOf(bare).covers.empty());
    for (const rv::world::CollectionItem& item : roomOf(bare).layout.items) {
        CHECK(item.flags == 0);
    }
    const rv::app::ReconcilePlan reloaded = rv::app::planReconciliation(bare, second);
    CHECK(reloaded.kind == rv::app::Reconciliation::Textures);
    CHECK(reloaded.difference.same());
    CHECK(reloaded.reloadCovers);

    // A broken file is left out (logged): the items pending, the floor shown all the same.
    REQUIRE(rv::core::writeTextFileAtomic(items, "{ not json").has_value());
    const WorldBuild broken = rv::app::previewWorld(worldPathsOf(imported), input.library, *stored, kSnesRoom);
    CHECK(broken.scene.has_value());
    CHECK(roomOf(broken).covers.empty());
}

TEST_CASE("app/world_build: a system's items hold the placements of every room that holds it, a game it no longer "
          "lists too",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-items-rooms", true, "esde/snes-room");
    const RoomSnapshotResult snapshot = snapshotOf(imported);
    REQUIRE_FALSE(snapshot.error.has_value());
    REQUIRE_FALSE(snapshot.snapshot.entries.empty());
    const rv::catalog::SystemName gb = rv::catalog::SystemName::parse("gb").value();

    // A plan by hand (the rule reads the rooms' sections and the placements alone): a shared room holding snes and gb,
    // an expansion of the snes room (its id's system), and gb's console room.
    rv::world::WorldPlan plan;
    const auto room = [&](rv::world::RoomId id, rv::world::RoomKind kind, std::vector<rv::catalog::SystemName> held) {
        rv::world::Room made;
        made.id = std::move(id);
        made.kind = kind;
        made.floor = "gen4";
        made.sections = std::move(held);
        plan.rooms.push_back(std::move(made));
        return plan.rooms.back().id;
    };
    const rv::world::RoomId shared =
        room(rv::world::RoomId::sharedArea("gen4").value(), rv::world::RoomKind::Shared, {gb, kSnes});
    const rv::world::RoomId expansion = room(rv::world::RoomId::forSystem(kSnes, 2).value(),
                                             rv::world::RoomKind::Console, {});
    const rv::world::RoomId gbRoom = room(rv::world::RoomId::forSystem(gb).value(), rv::world::RoomKind::Console, {gb});
    std::uint32_t slot = 0;
    const auto place = [&](const rv::world::RoomId& in, const rv::catalog::GameId& game) {
        const rv::world::FixtureId fixture = rv::world::FixtureId::make(in, "w01").value();
        plan.placements.push_back(rv::world::Placement{rv::world::SlotId::make(fixture, 1, ++slot).value(), game});
    };
    const rv::catalog::GameId listed = snapshot.snapshot.entries.front().id;
    const rv::catalog::GameId goneShared = rv::catalog::GameId::fromValue(900001).value();
    const rv::catalog::GameId goneExpansion = rv::catalog::GameId::fromValue(900002).value();
    const rv::catalog::GameId goneOther = rv::catalog::GameId::fromValue(900003).value();
    place(gbRoom, listed); // a game the snapshot lists, wherever it stands
    place(shared, goneShared);
    place(expansion, goneExpansion);
    place(gbRoom, goneOther); // another system's room: not snes's to keep

    const rv::app::RoomItems items = rv::app::snapshotRoomItems(snapshot, plan);
    CHECK(items.systemName == "Lumen 16 Home System");
    std::vector<rv::catalog::GameId> games;
    for (const rv::app::RoomItemText& item : items.items) {
        games.push_back(item.game);
    }
    std::vector<rv::catalog::GameId> expected{listed, goneShared, goneExpansion};
    std::sort(expected.begin(), expected.end());
    CHECK(games == expected);
    const rv::app::RoomItemText* known = items.find(listed);
    REQUIRE(known != nullptr);
    CHECK_FALSE(known->title.empty());
    CHECK_FALSE(known->coverPath.empty());
    // The games it no longer lists keep their slots with no text (their items keep the pending picture's label).
    for (const rv::catalog::GameId& game : {goneShared, goneExpansion}) {
        const rv::app::RoomItemText* gone = items.find(game);
        REQUIRE(gone != nullptr);
        CHECK(gone->title.empty());
        CHECK(gone->fallbackTitle.empty());
        CHECK(gone->year.empty());
        CHECK(gone->coverPath.empty());
    }
    CHECK(items.find(goneOther) == nullptr);
}

TEST_CASE("app/world_build: an unchanged start plans nothing (the building stamp); whatever a snapshot or the world's "
          "inputs could change plans again",
          "[app][world_build]") {
    // I4 step 6b: the building job's shortcut. A first building writes its stamp beside the rooms' items.
    const ImportedFixture imported("world-build-stamp", true, "esde/snes-room");
    const rv::app::WorldPaths paths = worldPathsOf(imported);
    const rv::catalog::LibraryProfile profile = rv::test::fixtureLibrary(imported.fixture);
    const WorldBuild first = rv::app::buildWorld(inputOf(imported), rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    const fs::path stampFile = rv::app::buildingStampFile(paths.cacheDir, profile.id);
    const auto stamp = rv::app::readBuildingStamp(stampFile);
    REQUIRE(stamp.has_value());
    CHECK(stamp->planDigest == first.planDigest);
    CHECK(rv::world::isSha256Hex(stamp->catalog));
    CHECK(stamp->inputs == rv::app::worldInputsDigest(paths, profile.id, *first.mediaRoot));
    CHECK(stamp->softFindings == first.softFindings);
    const auto catalogStampNow = [&] {
        const auto reader = readerOf(imported);
        auto read = rv::catalog_db::catalogStamp(reader->query->connection(), profile.id);
        REQUIRE(read.has_value());
        return *read;
    };
    const auto stored = [&] {
        auto read = rv::world::readStoredPlan(planFileOf(imported));
        REQUIRE(read.has_value());
        return *read;
    };

    // Nothing changed: the stored plan as it is, without a snapshot; its floor built from the items files is the
    // building's, CRT games included.
    std::optional<rv::app::BuildingResult> unchanged =
        rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom);
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->build.outcome == WorldOutcome::Unchanged);
    CHECK(unchanged->build.skipped);
    CHECK_FALSE(unchanged->build.changes.has_value());
    CHECK(unchanged->build.planDigest == first.planDigest);
    CHECK(unchanged->build.softFindings == first.softFindings); // as the plan's validation found them
    CHECK(unchanged->sources.mediaRoot == first.mediaRoot);
    rv::app::buildFloor(paths, unchanged->build, unchanged->sources);
    REQUIRE(unchanged->build.scene.has_value());
    CHECK(unchanged->build.sceneDigest == first.sceneDigest);
    CHECK(roomOf(unchanged->build).covers == roomOf(first).covers);
    CHECK(unchanged->build.screenGames == first.screenGames);
    CHECK(unchanged->build.mediaRoot == first.mediaRoot);

    // A play session that counts (roomEntries' rule): the catalog's stamp moves.
    const std::string before = catalogStampNow();
    {
        auto database = imported.openDatabase();
        auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
        rv::catalog::PlaySession session;
        session.game = first.plan->placements.front().game;
        session.started = std::chrono::system_clock::now();
        REQUIRE(store->insertSession(session).has_value());
    }
    CHECK(catalogStampNow() != before);
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    // The whole job then runs (no placement moved; the plan's inputDigest records the selections' play counts, the
    // items their last play) and stamps what it read.
    const WorldBuild played = rv::app::buildWorld(inputOf(imported), rv::core::JobContext{});
    CHECK(played.outcome == WorldOutcome::Updated);
    CHECK(played.plan->placements == first.plan->placements);
    CHECK_FALSE(played.skipped);
    CHECK(played.itemsWritten);
    CHECK(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());

    // An import, even one that changes nothing: another catalog.
    imported.importLibrary();
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    CHECK_FALSE(rv::app::buildWorld(inputOf(imported), rv::core::JobContext{}).skipped);
    CHECK(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());

    // The world's configuration changed (world.json): the world's inputs.
    REQUIRE(rv::core::writeTextFileAtomic(imported.paths.configDir() / "world.json", "{}\n").has_value());
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    static_cast<void>(rv::app::buildWorld(inputOf(imported), rv::core::JobContext{}));
    CHECK(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());

    // A room's items file gone, the plan file another, the media root out of reach, no stamp, another stamp schema.
    const fs::path items = rv::app::roomItemsFile(paths.cacheDir, profile.id, kSnes);
    const std::string itemsText = rv::core::readTextFile(items).value();
    REQUIRE(fs::remove(items));
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    REQUIRE(rv::core::writeTextFileAtomic(items, itemsText).has_value());
    CHECK(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    rv::world::StoredPlan other = stored();
    other.digest = std::string(64, '0');
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, other, catalogStampNow(), kSnesRoom).has_value());
    {
        const MediaAway away(imported);
        CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    }
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), std::string(), kSnesRoom).has_value());
    const std::string stampText = rv::core::readTextFile(stampFile).value();
    REQUIRE(rv::core::writeTextFileAtomic(stampFile, "{\"schemaVersion\": 1}\n").has_value());
    CHECK(rv::app::readBuildingStamp(stampFile).error().code == rv::ErrorCode::Unsupported);
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    REQUIRE(fs::remove(stampFile));
    CHECK(rv::app::readBuildingStamp(stampFile).error().code == rv::ErrorCode::NotFound);
    CHECK_FALSE(rv::app::unchangedBuilding(paths, profile, stored(), catalogStampNow(), kSnesRoom).has_value());
    // The stamp is written only when it changes.
    REQUIRE(rv::core::writeTextFileAtomic(stampFile, stampText).has_value());
    CHECK_FALSE(rv::app::writeBuildingStamp(stampFile, rv::app::readBuildingStamp(stampFile).value()).value());
}

TEST_CASE("app/world_build: a game added between two starts: the stored floor previewed, then rebuilt, nothing moved",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-added", true, "esde/snes-room");
    const WorldBuild first = rv::app::buildWorld(inputOf(imported), rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    for (const std::string_view file :
         {"roms/snes/Zinnia Zephyr (USA).sfc", "media/snes/covers/Zinnia Zephyr (USA).png"}) {
        fs::copy_file(imported.fixture.path(std::string("additions/") + std::string(file)),
                      imported.fixture.path(file));
    }
    imported.importLibrary();
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const rv::app::WorldBuildInput input = inputOf(imported);
    const WorldBuild preview = rv::app::previewWorld(worldPathsOf(imported), input.library, *stored, kSnesRoom);
    CHECK(roomOf(preview).layout.items.size() == 50);
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
    const WorldBuild build = rv::app::buildWorld(inputOf(imported), rv::core::JobContext{});
    REQUIRE(build.plan.has_value());
    RoomSnapshotResult snapshot = snapshotOf(imported);
    const rv::world::CollectionLayout& layout = roomOf(build).layout;
    // The build's games (from the room's items) are those of the start room's snapshot, with their title cards (I5
    // step 1: each its item's text in its label's colour, which the build adds).
    std::vector<rv::app::ScreenGame> withoutCards = build.screenGames;
    for (rv::app::ScreenGame& game : withoutCards) {
        CHECK_FALSE(game.card.empty());
        CHECK(game.card.systemName == roomOf(build).covers.front().systemName);
        game.card = {};
    }
    CHECK(withoutCards == rv::app::roomScreenGames(*build.plan, layout, snapshot));
    const auto titleOf = [&](const std::string& id) {
        for (const rv::catalog::RoomEntry& entry : snapshot.snapshot.entries) {
            if (entry.id.toString() == id) {
                return entry.title;
            }
        }
        return std::string();
    };
    const std::vector<rv::app::ScreenGame>& games = build.screenGames;
    // Every placed game, each with a video, a screenshot, a title screen or a cover (ADR-0013 §9: the CRT plays the
    // games that stand in its room; the two other releases and the one without a cover are not placed).
    CHECK(games.size() == 50);
    for (const rv::app::ScreenGame& game : games) {
        CHECK(layout.findGame(rv::catalog::GameId::parse(game.id).value()) != nullptr);
        CHECK(game.startCandidate);
        CHECK_FALSE(game.lastPlayed.has_value()); // the fixture has no session of Retroverse
    }
    // Never played: the first featured title with a video, the first by priority (a favourite, 42 plays).
    const std::optional<std::size_t> start = rv::app::chooseStartGame(games);
    REQUIRE(start.has_value());
    CHECK(titleOf(games[*start].id) == "Amber Lantern");
    CHECK(fs::exists(games[*start].media.video));
    CHECK(games[*start].media.video.extension() == ".mp4");
    CHECK_FALSE(games[*start].media.screenshot.empty());
    CHECK_FALSE(games[*start].media.titleScreen.empty());
    CHECK_FALSE(games[*start].media.cover.empty());
    // The game without a cover is not placed: the CRT never plays it, though it has a clip.
    CHECK(std::none_of(games.begin(), games.end(),
                       [&](const rv::app::ScreenGame& game) { return titleOf(game.id) == "Thistle Trellis"; }));

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
    const std::vector<rv::app::ScreenGame> again = rv::app::roomScreenGames(*build.plan, layout, snapshot);
    const std::optional<std::size_t> recent = rv::app::chooseStartGame(again);
    REQUIRE(recent.has_value());
    CHECK(titleOf(again[*recent].id) == "Juniper Jamboree");
    // The same from the items the building job keeps (the CRT's data of each placed game, its last play included).
    const rv::app::RoomItems items = rv::app::snapshotRoomItems(snapshot, *build.plan);
    const std::vector<rv::app::ScreenGame> fromItems = rv::app::roomScreenGames(
        layout, [&](const rv::catalog::GameId& game) { return items.find(game); }, *snapshot.mediaRoot);
    CHECK(fromItems == again);
    // The items' text round-trips (canonical JSON).
    CHECK(rv::app::parseRoomItems(rv::app::roomItemsToJson(items), "items").value() == items);

    // The media unknown: nothing to play (the root out of reach is never read for them).
    snapshot.snapshot.mediaKnown = false;
    CHECK(rv::app::roomScreenGames(*build.plan, layout, snapshot).empty());
}

TEST_CASE("app/room_screen: a shared room's CRT plays its sections' placed games in the room's featured order, "
          "whatever their section, then the others",
          "[app][world_build]") {
    // I4 step 4 (ADR-0013 §9): the sections' snapshots taken together, never one section's games after another's. Two
    // invented sections, each with a clip among its placed games: the room's featured-first game is the second
    // section's favourite, so a per-section concatenation would start the CRT on the first section's clip.
    using rv::catalog::GameId;
    using rv::catalog::MediaKind;
    const auto game = [](std::uint64_t value) { return GameId::fromValue(value).value(); };
    const GameId mango = game(0x01);  // aaa: placed, featured, a clip
    const GameId zebra = game(0x02);  // aaa: placed, featured, a cover
    const GameId spare = game(0x03);  // aaa: not placed, a clip
    const GameId pear = game(0x11);   // bbb: placed, featured, a favourite, a clip
    const GameId apple = game(0x12);  // bbb: placed, in the band, played often, a cover
    const auto sectionSnapshot = [](std::string_view system, const fs::path& root) {
        RoomSnapshotResult result;
        result.system = rv::catalog::SystemName::parse(system).value();
        result.snapshot.system = result.system;
        result.snapshot.mediaKnown = true;
        result.mediaRoot = root;
        return result;
    };
    // Entries and media paths by GameId, as a snapshot holds them.
    const auto add = [](RoomSnapshotResult& section, const GameId& id, std::string_view sortKey, bool clip) {
        rv::catalog::RoomEntry entry;
        entry.id = id;
        entry.title = std::string(sortKey);
        entry.sortKey = std::string(sortKey);
        section.snapshot.entries.push_back(entry);
        rv::esde::RoomMediaPaths paths;
        paths.game = id;
        const std::string stem = section.system.str() + "/" + std::string(sortKey);
        paths.relativePaths[static_cast<std::size_t>(MediaKind::Cover)] = stem + ".png";
        if (clip) {
            paths.relativePaths[static_cast<std::size_t>(MediaKind::Video)] = stem + ".mp4";
        }
        section.mediaPaths.push_back(paths);
        return &section.snapshot.entries.back();
    };
    RoomSnapshotResult aaa = sectionSnapshot("aaa", fs::path(L"Z:\\invented\\media"));
    add(aaa, mango, "mango", true);
    add(aaa, zebra, "zebra", false);
    add(aaa, spare, "spare", true);
    RoomSnapshotResult bbb = sectionSnapshot("bbb", fs::path(L"Z:\\invented\\media"));
    add(bbb, pear, "pear", true)->favorite = true;
    add(bbb, apple, "apple", false)->esdePlayCount = 7;
    // The room's items in slot order: each section's together, as a shared room places them.
    rv::world::CollectionLayout layout;
    for (const auto& [id, section] :
         {std::pair{mango, rv::world::Section::Featured}, std::pair{zebra, rv::world::Section::Featured},
          std::pair{pear, rv::world::Section::Featured}, std::pair{apple, rv::world::Section::Band}}) {
        rv::world::CollectionItem item;
        item.game = id;
        item.section = section;
        layout.items.push_back(item);
    }
    const rv::world::WorldPlan plan{};
    const auto idsOf = [](const std::vector<rv::app::ScreenGame>& games) {
        std::vector<std::string> ids;
        for (const rv::app::ScreenGame& screen : games) {
            ids.push_back(screen.id);
        }
        return ids;
    };

    const RoomSnapshotResult* const both[] = {&aaa, &bbb};
    const std::vector<rv::app::ScreenGame> games = rv::app::roomScreenGames(plan, layout, both);
    // The featured items by priority (the favourite, then by sort key), then the band's; never the unplaced game
    // (ADR-0013 §9: the CRT plays the games that stand in its room).
    CHECK(idsOf(games) ==
          std::vector<std::string>{pear.toString(), mango.toString(), zebra.toString(), apple.toString()});
    REQUIRE(games.size() == 4);
    for (const rv::app::ScreenGame& screen : games) {
        CHECK(screen.startCandidate);
    }
    CHECK(games[0].media.video.extension() == ".mp4");
    CHECK(games[0].media.cover.filename() == "pear.png");
    // Never played: the room's featured-first game with a clip, the second section's, not the first section's clip.
    const std::optional<std::size_t> start = rv::app::chooseStartGame(games);
    REQUIRE(start.has_value());
    CHECK(games[*start].id == pear.toString());
    // The sections given in the other order: the same games in the same order.
    const RoomSnapshotResult* const swapped[] = {&bbb, &aaa};
    CHECK(rv::app::roomScreenGames(plan, layout, swapped) == games);

    // A section whose media are unknown gives none of its games; the other's keep their order.
    bbb.snapshot.mediaKnown = false;
    const std::vector<rv::app::ScreenGame> known = rv::app::roomScreenGames(plan, layout, both);
    CHECK(idsOf(known) == std::vector<std::string>{mango.toString(), zebra.toString()});
    const std::optional<std::size_t> knownStart = rv::app::chooseStartGame(known);
    REQUIRE(knownStart.has_value());
    CHECK(known[*knownStart].id == mango.toString());
}

TEST_CASE("app/world_build: a manifest removed from a scratch asset root falls back to its family's prefab, warned "
          "once, the room built",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-fallback", true, "esde/snes-room");
    // Style §26.3's "an asset lost on purpose": the hero-wall module's manifest removed from a copy of the assets.
    const fs::path assets = imported.dir.file("assets");
    fs::copy(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), assets, fs::copy_options::recursive);
    REQUIRE(fs::remove(rv::assets::prefabManifestPath(assets, "furniture.shelf.hero_module.generic")));
    rv::app::WorldBuildInput input = inputOf(imported);
    input.paths.assetRoot = assets;
    rv::test::LogCapture log;
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(first.reason);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    CHECK(first.hardFindings.empty());
    const RoomCollection& room = roomOf(first);
    CHECK(room.layout.items.size() == 50);
    CHECK(room.layout.skipped.empty());
    REQUIRE(first.objectFallbacks.size() == 1);
    CHECK(first.objectFallbacks.front() ==
          rv::app::ObjectFallback{"furniture.shelf.hero_module.generic", "family generic", "the manifest is missing"});
    // The side-wall module stands in the hero wall's sockets: no hero band, the titles past the display slots
    // spine-out there.
    std::size_t band = 0;
    std::size_t spine = 0;
    for (const rv::world::CollectionItem& item : room.layout.items) {
        band += item.section == rv::world::Section::Band ? 1 : 0;
        spine += item.section == rv::world::Section::Spine ? 1 : 0;
    }
    CHECK(band == 0);
    CHECK(spine == 6);
    // The next start's preview and reconciliation resolve it again: still one warning in all.
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const WorldBuild preview = rv::app::previewWorld(input.paths, input.library, *stored, kSnesRoom);
    CHECK(preview.scene.has_value());
    CHECK(preview.objectFallbacks == first.objectFallbacks);
    const WorldBuild second = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(second.outcome == WorldOutcome::Unchanged);
    CHECK(log.count("warning Prefab 'furniture.shelf.hero_module.generic' cannot be used") == 1);
}

TEST_CASE("app/world_build: a stored schema-1 plan is lifted, saved once in schema 2 after a backup, then left as it is",
          "[app][world_build]") {
    // The user's I3 room on disk (ADR-0013 §5): the app's own first plan, written back as I3 wrote it.
    const ImportedFixture imported("world-build-lift", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    const fs::path planFile = planFileOf(imported);
    const std::string schemaTwo = rv::core::readTextFile(planFile).value();
    const std::string schemaOne = schemaOneText(schemaTwo);
    REQUIRE(rv::core::writeTextFileAtomic(planFile, schemaOne).has_value());

    // The persisted start reads it lifted, its digest the file's.
    const rv::app::StoredWorldPlan read = rv::app::readWorldPlan(worldPathsOf(imported), input.library);
    REQUIRE_FALSE(read.error.has_value());
    REQUIRE(read.stored.has_value());
    CHECK(read.stored->digest == rv::core::sha256Hex(schemaOne));
    CHECK(read.stored->plan.schemaVersion == rv::world::kPlanSchemaVersion);
    REQUIRE(read.stored->plan.rooms.size() == 1);
    CHECK(read.stored->plan.rooms.front().floor == "gen4");
    CHECK(read.stored->plan.rooms.front().generatorVersion == rv::world::GeneratorVersion{1, 2});
    CHECK(read.stored->plan.placements == first.plan->placements);
    const WorldBuild preview = rv::app::previewWorld(worldPathsOf(imported), input.library, *read.stored, kSnesRoom);
    CHECK(roomOf(preview).layout.items.size() == 50);

    // The first update writes schema 2 (the room's elements as they were: the same plan the app made in schema 2) and
    // keeps the schema-1 file in backups/.
    const WorldBuild updated = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(updated.reason);
    REQUIRE(updated.outcome == WorldOutcome::Updated);
    CHECK(updated.written);
    REQUIRE(updated.changes.has_value());
    CHECK(updated.changes->digestBefore == rv::core::sha256Hex(schemaOne));
    CHECK(updated.changes->kept == 50);
    CHECK(updated.changes->added.empty());
    CHECK(updated.notice.empty());
    CHECK(rv::core::readTextFile(planFile).value() == schemaTwo);
    const std::vector<std::string> backups = backupTexts(imported);
    CHECK(std::find(backups.begin(), backups.end(), schemaOne) != backups.end());
    CHECK(roomOf(updated).layout.items.size() == 50);

    // The next start: nothing written.
    const WorldBuild again = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(again.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(again.written);
    CHECK(backupTexts(imported).size() == backups.size());
}

TEST_CASE("app/world_build: a building of two rooms: the snes room updated and shown, the other as it stood",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-building", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
    const fs::path assets = input.paths.assetRoot;
    const auto config = rv::world::loadWorldConfig(assets, input.paths.configDir).value().config;
    const RoomSnapshotResult* snapshot = input.snapshot.find(kSnes);
    REQUIRE(snapshot != nullptr);
    auto snes = rv::world::selectRoom(rv::world::RoomInput{input.library, snapshot->snapshot}, config);
    REQUIRE(snes.has_value());
    // The snes room made before its last title came (one no other version names), and a megadrive room beside it,
    // as `worldplan generate --selection a --selection b` makes the building.
    rv::catalog::GameId later;
    for (auto title = snes->titles.rbegin(); title != snes->titles.rend() && !later.isValid(); ++title) {
        const bool represents =
            std::any_of(snes->unplaced.begin(), snes->unplaced.end(),
                        [&](const rv::world::UnplacedGame& game) { return game.representative == title->game; });
        if (!represents) {
            later = title->game;
        }
    }
    REQUIRE(later.isValid());
    rv::world::RoomSelection earlier = *snes;
    std::erase_if(earlier.titles, [&](const rv::world::RoomTitle& title) { return title.game == later; });
    rv::world::RoomSelection megadrive = rv::test::world::makeSelection(30, 2);
    megadrive.room = *rv::world::RoomId::forSystem(rv::catalog::SystemName::parse("megadrive").value());
    for (rv::world::UnplacedGame& game : megadrive.unplaced) {
        game.room = megadrive.room;
    }
    rv::world::BuildingInput building;
    building.library = input.library;
    building.selections = {earlier, megadrive};
    building.config = config;
    const std::vector<rv::world::WorldData> rooms{rv::world::loadWorldData(assets, kSnes).value(),
                                                  rv::world::loadWorldData(assets, megadrive.room.system()).value()};
    auto made = rv::world::generateBuilding(building, rv::world::loadBuildingData(assets).value(),
                                            rv::world::loadSystemsData(assets).value(), rooms);
    REQUIRE(made.has_value());
    REQUIRE(made->validation.passed());
    const fs::path planFile = planFileOf(imported);
    REQUIRE(rv::core::writeTextFileAtomic(planFile, rv::world::writePlan(made->plan).value()).has_value());

    // The app updates the rooms of the catalog's systems: the new title placed, the megadrive room byte for byte as it
    // stood (the catalog has no megadrive games).
    const WorldBuild updated = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(updated.reason);
    REQUIRE(updated.outcome == WorldOutcome::Updated);
    REQUIRE(updated.changes.has_value());
    CHECK(updated.changes->added == std::vector<rv::catalog::GameId>{later});
    REQUIRE(updated.plan.has_value());
    CHECK(updated.plan->rooms.size() == 2);
    CHECK(rv::world::roomContentDigest(*updated.plan, megadrive.room).value() ==
          rv::world::roomContentDigest(made->plan, megadrive.room).value());
    CHECK(rv::world::roomContentDigest(*updated.plan, earlier.room).value() !=
          rv::world::roomContentDigest(made->plan, earlier.room).value());
    // The floor of gen4 holds both rooms (snes and megadrive): each its collection, the snes room's 50 items in its
    // own frame (the start room), the megadrive room's moved by its placement relative to it.
    CHECK(updated.floor == "gen4");
    REQUIRE(updated.rooms.size() == 2);
    const RoomCollection& snesRoom = roomOf(updated);
    CHECK(snesRoom.layout.items.size() == 50);
    for (const rv::world::CollectionItem& item : snesRoom.layout.items) {
        CHECK(item.slot.fixture().room() == earlier.room);
    }
    const RoomCollection& megadriveRoom = roomOf(updated, megadrive.room);
    CHECK(megadriveRoom.scenePlacement ==
          rv::app::relativePlacement(megadriveRoom.placement, snesRoom.placement));
    CHECK(megadriveRoom.scenePlacement != rv::world::RoomPlacement{});
    // The megadrive room's items have no text (the catalog has no such system): no cover request.
    CHECK(megadriveRoom.covers.empty());
    // The next start, the catalog unchanged: nothing written (the plan's inputDigest, of both rooms, kept).
    const std::string written = rv::core::readTextFile(planFile).value();
    const WorldBuild again = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(again.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(again.written);
    CHECK(rv::core::readTextFile(planFile).value() == written);
}

TEST_CASE("app/world_build: the app and retroverse-cli's catalog commands write one plan for one catalog",
          "[app][world_build]") {
    // The app and the CLI classify by the catalog's platform tags; a system with a row of its own hashes alike
    // whatever tags its caller knows: a plan either writes the other leaves as it is.
    const ImportedFixture imported("world-build-cli", true, "esde/snes-room");
    const rv::app::WorldBuildInput input = inputOf(imported);
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    const fs::path planFile = planFileOf(imported);
    const std::string text = rv::core::readTextFile(planFile).value();

    const CliRun updated = runCli(imported, {"worldplan", "update"});
    INFO(updated.err);
    CHECK(updated.code == rv::cli::ExitCode::Ok);
    CHECK(updated.out.find("is up to date") != std::string::npos);
    CHECK(rv::core::readTextFile(planFile).value() == text);
    const CliRun regenerated = runCli(imported, {"worldplan", "generate", "--replace"});
    INFO(regenerated.err);
    CHECK(regenerated.code == rv::cli::ExitCode::Ok);
    CHECK(regenerated.out.find("the previous plan was this plan already") != std::string::npos);
    CHECK(rv::core::readTextFile(planFile).value() == text);
    CHECK(backupTexts(imported).empty());

    // And the CLI's plan, made first, is the app's.
    fs::remove(planFile);
    const CliRun generated = runCli(imported, {"worldplan", "generate"});
    INFO(generated.err);
    REQUIRE(generated.code == rv::cli::ExitCode::Ok);
    CHECK(rv::core::readTextFile(planFile).value() == text);
    const WorldBuild second = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(second.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(second.written);
}

TEST_CASE("app/world_build: the building fixture planned whole, as the CLI plans it; a custom system classified by "
          "the catalog's platform tags",
          "[app][world_build]") {
    // I4 step 2's pocketlab, a system systems.json does not name, with gb's platform tag: the building job's snapshot
    // carries the catalog's tags, so the first building, the scene, the rooms' items and the preview classify it as
    // gb's row (the handheld cartridge, gb's sign), as worldplan does. Every system with a room gets its place, kodi
    // (excluded) none.
    const ImportedFixture imported("world-build-custom", true, "esde/building");
    const rv::world::RoomId pocketlabRoom = rv::world::RoomId::parse("room:pocketlab").value();
    const rv::catalog::SystemName pocketlab = rv::catalog::SystemName::parse("pocketlab").value();
    // Its 6 titles keep a room of their own (I4 step 4: else a section of the handheld floor's shared room, which the
    // shared-room case below shows).
    REQUIRE(rv::core::writeTextFileAtomic(imported.paths.configDir() / "world.json", "{\"sharedRoomMinTitles\": 0}\n")
                .has_value());
    const rv::app::WorldBuildInput input = inputOf(imported, buildingSnapshotOf(imported), pocketlabRoom);
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(first.reason);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    REQUIRE(first.plan.has_value());
    const auto roomInPlan = [&](const rv::world::WorldPlan& plan, const rv::world::RoomId& id) {
        return std::find_if(plan.rooms.begin(), plan.rooms.end(),
                            [&](const rv::world::Room& room) { return room.id == id; });
    };
    const auto pocketlabInPlan = roomInPlan(*first.plan, pocketlabRoom);
    REQUIRE(pocketlabInPlan != first.plan->rooms.end());
    CHECK(pocketlabInPlan->mediaFamily == "cartridge_handheld");
    CHECK(pocketlabInPlan->floor == "handheld");
    for (const rv::world::Room& room : first.plan->rooms) {
        for (const rv::catalog::SystemName& system : room.sections) {
            CHECK(system.str() != "kodi");
        }
    }
    const auto items =
        rv::app::readRoomItems(rv::app::roomItemsFile(imported.paths.cacheDir(), input.library, pocketlab));
    REQUIRE(items.has_value());
    CHECK(items->platforms == std::vector<std::string>{"gb"});
    CHECK(items->items.size() == 6);
    // The handheld floor: its collection rooms, the pocketlab games among them.
    CHECK(first.floor == "handheld");
    std::size_t pocketlabItems = 0;
    for (const RoomCollection& room : first.rooms) {
        for (const rv::app::CoverItem& cover : room.covers) {
            pocketlabItems += cover.systemName == "Pocket Lab Homebrew" ? 1 : 0;
        }
    }
    CHECK(pocketlabItems == 6);
    if (const RoomCollection* room = first.findRoom(pocketlabRoom)) {
        CHECK(room->sceneData.platform.signText == "POCKET GAMES");
        CHECK(room->scenePlacement == rv::world::RoomPlacement{});
    }

    // The next start's preview, before the building job: the rooms' items carry the tags.
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const WorldBuild preview = rv::app::previewWorld(worldPathsOf(imported), input.library, *stored, pocketlabRoom);
    REQUIRE(preview.scene.has_value());
    CHECK(preview.sceneDigest == first.sceneDigest);

    // The CLI's update of the building finds it up to date (one classification, one inputDigest); the app's too.
    const fs::path planFile = planFileOf(imported);
    const std::string text = rv::core::readTextFile(planFile).value();
    const CliRun updated = runCli(imported, {"worldplan", "update"});
    INFO(updated.err);
    CHECK(updated.code == rv::cli::ExitCode::Ok);
    CHECK(updated.out.find("is up to date") != std::string::npos);
    CHECK(rv::core::readTextFile(planFile).value() == text);
    const WorldBuild second = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(second.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(second.written);
    // And the CLI's whole building, made first, is the app's.
    fs::remove(planFile);
    const CliRun generated = runCli(imported, {"worldplan", "generate"});
    INFO(generated.err);
    REQUIRE(generated.code == rv::cli::ExitCode::Ok);
    CHECK(rv::core::readTextFile(planFile).value() == text);
}

TEST_CASE("app/world_build: a room kept as it is (its prefab levels changed): shown as it stands, with the "
          "regeneration's notice",
          "[app][world_build]") {
    const ImportedFixture imported("world-build-kept", true, "esde/snes-room");
    rv::app::WorldBuildInput input = inputOf(imported);
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    REQUIRE(first.outcome == WorldOutcome::Generated);
    // A copy of the assets whose hero-wall module holds an eighth level (a manifest edited). Every level the room's
    // placements and dressing name keeps its slots, so it shows as it stands (a prefab whose slots no longer hold
    // them cannot be drawn, kept or not).
    const fs::path assets = imported.dir.file("assets");
    fs::copy(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), assets, fs::copy_options::recursive);
    const fs::path manifest = rv::assets::prefabManifestPath(assets, "furniture.shelf.hero_module.generic");
    std::string text = rv::core::readTextFile(manifest).value();
    const std::string levels = R"("levels": 7)";
    const std::size_t at = text.find(levels);
    REQUIRE(at != std::string::npos);
    text.replace(at, levels.size(), R"("levels": 8)");
    REQUIRE(rv::core::writeTextFileAtomic(manifest, text).has_value());
    input.paths.assetRoot = assets;

    rv::test::LogCapture log;
    const WorldBuild kept = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(kept.reason);
    REQUIRE(kept.changes.has_value());
    CHECK(kept.changes->outcome == rv::world::GenerationOutcome::Updated);
    REQUIRE(kept.changes->keptRooms.size() == 1);
    CHECK(kept.changes->keptRooms.front().room.toString() == "room:snes");
    CHECK(kept.notice == rv::app::world_notice::kRegeneration);
    CHECK(kept.hardFindings.empty());
    REQUIRE(kept.plan.has_value());
    CHECK(kept.plan->placements == first.plan->placements);
    CHECK(kept.plan->fixtures == first.plan->fixtures);
    CHECK(kept.plan->unplaced == first.plan->unplaced);
    CHECK(roomOf(kept).layout.items.size() == 50);
    CHECK(log.count("is kept as it is until 'retroverse-cli worldplan generate --replace'") == 1);
    // No building stamp while a room waits for its regeneration (I4 step 6b): the first build's is removed, so that
    // every next start runs the job and says so.
    CHECK_FALSE(fs::exists(rv::app::buildingStampFile(input.paths.cacheDir, input.library)));
    CHECK(log.count("the building stamp is removed") == 1);
    // The next start: the same notice, nothing written.
    const WorldBuild again = rv::app::buildWorld(input, rv::core::JobContext{});
    CHECK(again.notice == rv::app::world_notice::kRegeneration);
    CHECK_FALSE(again.written);
    CHECK_FALSE(again.skipped);
    CHECK_FALSE(fs::exists(rv::app::buildingStampFile(input.paths.cacheDir, input.library)));
}

TEST_CASE("app/world_flow: a start room kept until a regeneration shows its notice at every start, whichever room the "
          "last start was in",
          "[app][world_build]") {
    // I4 step 6b's shortcut never hides a room kept as it is: its job writes no building stamp, so the next starts run
    // the whole job again, its notice and warnings with it. The app's flow, three starts after a first building.
    const ImportedFixture imported("world-flow-kept", true, "esde/snes-room");
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{2});
    rv::app::WorldFlow::Options options;
    options.paths = worldPathsOf(imported);
    options.startRoom = kSnesRoom;
    options.writer = "retroverse (test)";
    FakeCatalog catalog;
    catalog.state = rv::app::CatalogState::Ready;
    catalog.reader = readerOf(imported);
    const fs::path stampFile = rv::app::buildingStampFile(options.paths.cacheDir, catalog.library);
    {
        rv::app::WorldFlow first(options, pool);
        const auto built = runFlow(first, catalog);
        REQUIRE(built.has_value());
        REQUIRE(built->build.has_value());
        CHECK(built->build->outcome == WorldOutcome::Generated);
        CHECK(fs::exists(stampFile));
    }
    // The hero-wall module's manifest edited (an eighth level), in a copy of the assets: room:snes is kept as it is.
    const fs::path assets = imported.dir.file("assets");
    fs::copy(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), assets, fs::copy_options::recursive);
    const fs::path manifest = rv::assets::prefabManifestPath(assets, "furniture.shelf.hero_module.generic");
    std::string text = rv::core::readTextFile(manifest).value();
    const std::string levels = R"("levels": 7)";
    const std::size_t at = text.find(levels);
    REQUIRE(at != std::string::npos);
    text.replace(at, levels.size(), R"("levels": 8)");
    REQUIRE(rv::core::writeTextFileAtomic(manifest, text).has_value());
    options.paths.assetRoot = assets;
    const auto start = [&](std::optional<rv::world::RoomId> room) {
        options.startRoom = std::move(room);
        rv::app::WorldFlow flow(options, pool);
        std::optional<WorldBuild> preview;
        auto result = runFlow(flow, catalog, &preview);
        REQUIRE(result.has_value());
        REQUIRE(result->build.has_value());
        CHECK(preview.has_value()); // the stored floor first
        return std::move(*result->build);
    };
    for (int run = 0; run < 2; ++run) {
        INFO("start " << run + 2 << " in room:snes");
        rv::test::LogCapture log;
        const WorldBuild build = start(kSnesRoom);
        CHECK_FALSE(build.skipped);
        CHECK(build.notice == rv::app::world_notice::kRegeneration);
        REQUIRE(build.changes.has_value());
        CHECK(build.changes->keptRooms.size() == 1);
        CHECK(log.count("is kept as it is until 'retroverse-cli worldplan generate --replace'") == 1);
        CHECK_FALSE(fs::exists(stampFile));
    }
    // A start in the lobby (no notice there: the start room is not the room kept) writes no stamp either: the next start
    // in room:snes shows the notice again.
    const WorldBuild lobby = start(std::nullopt);
    CHECK_FALSE(lobby.skipped);
    CHECK(lobby.notice.empty());
    CHECK_FALSE(fs::exists(stampFile));
    const WorldBuild again = start(kSnesRoom);
    CHECK_FALSE(again.skipped);
    CHECK(again.notice == rv::app::world_notice::kRegeneration);
}

TEST_CASE("app/world_flow: the preview's CRT starts on the game the building chooses, a game played since the items "
          "were written included",
          "[app][world_build]") {
    // ADR-0012 §7, I4 step 6b: the start choice is the most recently played placed game with a clip. The items files
    // hold each game's last play as their last write saw it; the preview takes the catalog's sessions as they are now
    // (catalog_db::lastPlaySessions()), so a session recorded after them (a game played, Retroverse quit, started
    // again) starts the stored floor's CRT on that game, and the building's result never retargets it.
    const ImportedFixture imported("world-flow-played", true, "esde/snes-room");
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{2});
    rv::app::WorldFlow::Options options;
    options.paths = worldPathsOf(imported);
    options.startRoom = kSnesRoom;
    options.writer = "retroverse (test)";
    FakeCatalog catalog;
    catalog.state = rv::app::CatalogState::Ready;
    catalog.reader = readerOf(imported);
    std::vector<rv::app::ScreenGame> games;
    {
        rv::app::WorldFlow first(options, pool);
        const auto built = runFlow(first, catalog);
        REQUIRE(built.has_value());
        REQUIRE(built->build.has_value());
        games = built->build->screenGames;
    }
    const std::optional<std::size_t> before = rv::app::chooseStartGame(games);
    REQUIRE(before.has_value());
    // Another placed game with a clip, played now (a session that counts).
    const auto other = std::find_if(games.begin(), games.end(), [&](const rv::app::ScreenGame& game) {
        return game.id != games[*before].id && rv::app::isPlayableVideoFile(game.media.video);
    });
    REQUIRE(other != games.end());
    const rv::catalog::GameId played = rv::catalog::GameId::parse(other->id).value();
    const auto startedAt = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    {
        auto database = imported.openDatabase();
        auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
        rv::catalog::PlaySession session;
        session.game = played;
        session.started = startedAt;
        REQUIRE(store->insertSession(session).has_value());
    }
    {
        const auto reader = readerOf(imported);
        const auto sessions = rv::catalog_db::lastPlaySessions(reader->query->connection());
        REQUIRE(sessions.has_value());
        REQUIRE(sessions->size() == 1);
        CHECK(sessions->at(played) == startedAt);
    }
    // The items files as the first building left them: the game never played there.
    const fs::path itemsFile = rv::app::roomItemsFile(options.paths.cacheDir, catalog.library, kSnes);
    const auto items = rv::app::readRoomItems(itemsFile);
    REQUIRE(items.has_value());
    REQUIRE(items->find(played) != nullptr);
    REQUIRE(items->find(played)->screen.has_value());
    CHECK_FALSE(items->find(played)->screen->lastPlayed.has_value());
    catalog.reader = readerOf(imported);
    rv::app::WorldFlow second(options, pool);
    std::optional<WorldBuild> preview;
    const auto result = runFlow(second, catalog, &preview);
    REQUIRE(preview.has_value());
    REQUIRE(result.has_value());
    REQUIRE(result->build.has_value());
    CHECK_FALSE(result->build->skipped); // a session counts: the catalog's stamp moved
    const std::int64_t seconds =
        std::chrono::duration_cast<std::chrono::seconds>(startedAt.time_since_epoch()).count();
    const auto previewedGame =
        std::find_if(preview->screenGames.begin(), preview->screenGames.end(),
                     [&](const rv::app::ScreenGame& game) { return game.id == other->id; });
    REQUIRE(previewedGame != preview->screenGames.end());
    CHECK(previewedGame->lastPlayed == std::optional<std::int64_t>(seconds)); // the catalog's, not the items'
    // The building rewrote the items with it.
    CHECK(rv::app::readRoomItems(itemsFile).value().find(played)->screen->lastPlayed ==
          std::optional<std::int64_t>(seconds));
    const std::optional<std::size_t> previewed = rv::app::chooseStartGame(preview->screenGames);
    const std::optional<std::size_t> reconciled = rv::app::chooseStartGame(result->build->screenGames);
    REQUIRE(previewed.has_value());
    REQUIRE(reconciled.has_value());
    CHECK(preview->screenGames[*previewed].id == other->id);
    CHECK(result->build->screenGames[*reconciled].id == other->id);
    // Without the catalog's sessions the preview would have started on the old choice (the items' last plays).
    for (const rv::app::ScreenGame& game : preview->screenGames) {
        const auto found = std::find_if(result->build->screenGames.begin(), result->build->screenGames.end(),
                                        [&](const rv::app::ScreenGame& built) { return built.id == game.id; });
        REQUIRE(found != result->build->screenGames.end());
        CHECK(found->lastPlayed == game.lastPlayed);
    }
}

TEST_CASE("app/world_build: the shared and arcade rooms in the app: their sections' covers and CRT games, the update "
          "by sections, a room held for want of a section's selection",
          "[app][world_build]") {
    // I4 step 4 at the merge with step 5a: the building fixture under the shipped threshold (30 distinct titles), so
    // pocketlab (6) and pokemini (8) take the sections of area:shared:handheld and mame the arcade room's. The app plans
    // the building the CLI plans; each section's items file serves its own games' text; the shared room's collection
    // shows both sections' covers, its CRT plays both sections' games.
    const ImportedFixture imported("world-build-shared", true, "esde/building");
    const rv::world::RoomId shared = rv::world::RoomId::parse("area:shared:handheld").value();
    const rv::world::RoomId arcade = rv::world::RoomId::parse("area:arcade").value();
    const rv::catalog::SystemName pocketlab = rv::catalog::SystemName::parse("pocketlab").value();
    const rv::catalog::SystemName pokemini = rv::catalog::SystemName::parse("pokemini").value();
    const rv::catalog::SystemName mame = rv::catalog::SystemName::parse("mame").value();
    const rv::app::WorldBuildInput input = inputOf(imported, buildingSnapshotOf(imported), shared);
    const WorldBuild first = rv::app::buildWorld(input, rv::core::JobContext{});
    INFO(first.reason);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    REQUIRE(first.plan.has_value());
    const auto roomInPlan = [&](const rv::world::RoomId& id) -> const rv::world::Room& {
        const auto found = std::find_if(first.plan->rooms.begin(), first.plan->rooms.end(),
                                        [&](const rv::world::Room& room) { return room.id == id; });
        INFO(id.toString());
        REQUIRE(found != first.plan->rooms.end());
        return *found;
    };
    const rv::world::Room& sharedInPlan = roomInPlan(shared);
    CHECK(sharedInPlan.kind == rv::world::RoomKind::Shared);
    CHECK(sharedInPlan.sections == std::vector<rv::catalog::SystemName>{pocketlab, pokemini});
    CHECK(sharedInPlan.floor == "handheld");
    CHECK(sharedInPlan.mediaFamily == rv::world::kSharedRoomFamily);
    const rv::world::Room& arcadeInPlan = roomInPlan(arcade);
    CHECK(arcadeInPlan.kind == rv::world::RoomKind::Arcade);
    CHECK(arcadeInPlan.sections == std::vector<rv::catalog::SystemName>{mame});
    CHECK(arcadeInPlan.floor == "ground");

    // The shared room on its floor: its 14 items, each with its own section's text and cover.
    CHECK(first.floor == "handheld");
    const RoomCollection& room = roomOf(first, shared);
    CHECK(room.scenePlacement == rv::world::RoomPlacement{});
    CHECK(room.sceneData.platform.signText == "MORE GAMES");
    CHECK_FALSE(room.sceneData.platform.console.has_value());
    REQUIRE(room.layout.items.size() == 14);
    REQUIRE(room.covers.size() == 14);
    // Every cover carries its room's pack's label palette (I6 step 3: the handheld floor's handheld_archive), one
    // palette for the room; the arcade room's its own pack's.
    const auto paletteOf = [](const rv::world::ThemeMaterials& theme) {
        std::vector<rv::assets::Rgb8> colours;
        for (const std::string& hex : theme.labelPalette) {
            const auto colour = rv::core::parseHexColor(hex);
            REQUIRE(colour.has_value());
            const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(value * 255.0f)); };
            colours.push_back({byte(colour->r), byte(colour->g), byte(colour->b)});
        }
        return colours;
    };
    CHECK(room.sceneData.theme.id == "handheld_archive");
    const std::vector<rv::assets::Rgb8> handheld = paletteOf(room.sceneData.theme);
    REQUIRE(handheld.size() == 8);
    for (const rv::app::CoverItem& cover : room.covers) {
        REQUIRE(cover.palette.colours != nullptr);
        CHECK(cover.palette.colours == room.covers.front().palette.colours);
        CHECK(std::ranges::equal(cover.palette.view(), handheld));
    }
    std::map<std::string, std::size_t> bySection;
    for (const rv::app::CoverItem& cover : room.covers) {
        INFO(cover.game.toString() << " " << cover.systemName << " " << cover.coverPath);
        CHECK_FALSE(cover.title.empty());
        CHECK_FALSE(cover.fallbackTitle.empty());
        if (cover.systemName == "Pocket Lab Homebrew") {
            CHECK(cover.coverPath.starts_with("pocketlab/covers/"));
        } else {
            CHECK(cover.systemName == "Pico Pocket");
            CHECK(cover.coverPath.starts_with("pokemini/covers/"));
        }
        ++bySection[cover.systemName];
    }
    CHECK(bySection == std::map<std::string, std::size_t>{{"Pico Pocket", 8}, {"Pocket Lab Homebrew", 6}});
    for (const rv::world::CollectionItem& item : room.layout.items) {
        CHECK_FALSE(item.title.empty());
    }
    // Each section's items file: every item of the room that holds it, the text of its own games only.
    for (const auto& [system, own] : {std::pair{pocketlab, std::size_t{6}}, std::pair{pokemini, std::size_t{8}}}) {
        INFO(system.str());
        const auto items =
            rv::app::readRoomItems(rv::app::roomItemsFile(imported.paths.cacheDir(), input.library, system));
        REQUIRE(items.has_value());
        CHECK(items->items.size() == 14);
        const auto withText = std::count_if(items->items.begin(), items->items.end(),
                                            [](const rv::app::RoomItemText& text) { return !text.coverPath.empty(); });
        CHECK(static_cast<std::size_t>(withText) == own);
    }
    // Its CRT plays both sections' games, every one placed (their featured order across the sections: the test
    // "app/room_screen: a shared room's CRT plays its sections' placed games ...").
    std::set<std::string> screenGames;
    std::size_t candidates = 0;
    for (const rv::app::ScreenGame& game : first.screenGames) {
        screenGames.insert(game.id);
        CHECK(game.startCandidate == (room.layout.findGame(rv::catalog::GameId::parse(game.id).value()) != nullptr));
        candidates += game.startCandidate ? 1 : 0;
    }
    CHECK(first.screenGames.size() == 14);
    CHECK(candidates == 14);
    std::set<std::string> placed;
    for (const rv::world::CollectionItem& item : room.layout.items) {
        placed.insert(item.game.toString());
    }
    CHECK(screenGames == placed);

    // The next start's preview of the stored floor, from the items files alone: the same covers.
    const auto stored = rv::world::readStoredPlan(planFileOf(imported));
    REQUIRE(stored.has_value());
    const WorldBuild preview = rv::app::previewWorld(worldPathsOf(imported), input.library, *stored, shared);
    REQUIRE(preview.scene.has_value());
    CHECK(preview.sceneDigest == first.sceneDigest);
    const RoomCollection& previewed = roomOf(preview, shared);
    REQUIRE(previewed.covers.size() == room.covers.size());
    for (std::size_t index = 0; index < room.covers.size(); ++index) {
        CHECK(previewed.covers[index].title == room.covers[index].title);
        CHECK(previewed.covers[index].coverPath == room.covers[index].coverPath);
        CHECK(previewed.covers[index].systemName == room.covers[index].systemName);
    }

    // The arcade room from its doorway: the ground floor, mame's 15 sets with a cover, its CRT their games.
    const WorldBuild arcadeBuild =
        rv::app::buildWorld(inputOf(imported, buildingSnapshotOf(imported), arcade), rv::core::JobContext{});
    CHECK(arcadeBuild.outcome == WorldOutcome::Unchanged);
    CHECK(arcadeBuild.floor == "ground");
    const RoomCollection& arcadeRoom = roomOf(arcadeBuild, arcade);
    CHECK(arcadeRoom.sceneData.platform.signText == "ARCADE");
    CHECK_FALSE(arcadeRoom.sceneData.platform.console.has_value());
    REQUIRE(arcadeRoom.covers.size() == 15);
    CHECK(arcadeRoom.sceneData.theme.id == "arcade_after_dark");
    const std::vector<rv::assets::Rgb8> arcadePalette = paletteOf(arcadeRoom.sceneData.theme);
    CHECK_FALSE(std::ranges::equal(arcadePalette, handheld));
    for (const rv::app::CoverItem& cover : arcadeRoom.covers) {
        CHECK(cover.systemName == "Arcade Board Library");
        CHECK(cover.coverPath.starts_with("mame/covers/"));
        CHECK(std::ranges::equal(cover.palette.view(), arcadePalette));
    }
    CHECK(arcadeBuild.screenGames.size() == 15);

    // The CLI's whole building is the app's, and its update finds it up to date.
    const fs::path planFile = planFileOf(imported);
    const std::string text = rv::core::readTextFile(planFile).value();
    const CliRun updated = runCli(imported, {"worldplan", "update"});
    INFO(updated.err);
    CHECK(updated.code == rv::cli::ExitCode::Ok);
    CHECK(updated.out.find("is up to date") != std::string::npos);
    CHECK(rv::core::readTextFile(planFile).value() == text);
    fs::remove(planFile);
    const CliRun generated = runCli(imported, {"worldplan", "generate"});
    INFO(generated.err);
    REQUIRE(generated.code == rv::cli::ExitCode::Ok);
    CHECK(rv::core::readTextFile(planFile).value() == text);

    // A section the catalog no longer lists (its snapshot NotFound): the shared room held as it is, said in the log,
    // its other section's selection not read; the floor shows as it stood, its covers from the items files.
    {
        BuildingSnapshot snapshot = buildingSnapshotOf(imported);
        for (RoomSnapshotResult& system : snapshot.systems) {
            if (system.system == pokemini) {
                system.error = rv::Error{rv::ErrorCode::NotFound, "no entries", {}};
            }
        }
        rv::test::LogCapture log;
        const WorldBuild held =
            rv::app::buildWorld(inputOf(imported, std::move(snapshot), shared), rv::core::JobContext{});
        INFO(held.reason);
        CHECK(held.outcome == WorldOutcome::Unchanged);
        CHECK_FALSE(held.written);
        REQUIRE(held.changes.has_value());
        REQUIRE(held.changes->heldRooms.size() == 1);
        CHECK(held.changes->heldRooms.front().room == shared);
        CHECK(held.changes->heldRooms.front().reason.find("pokemini") != std::string::npos);
        CHECK(log.count("area:shared:handheld is held as it is") == 1);
        CHECK(rv::core::readTextFile(planFile).value() == text);
        const RoomCollection& heldRoom = roomOf(held, shared);
        REQUIRE(heldRoom.covers.size() == 14);
        for (std::size_t index = 0; index < room.covers.size(); ++index) {
            CHECK(heldRoom.covers[index].coverPath == room.covers[index].coverPath);
        }
    }

    // additions/ imported: the shared room updated with both sections' selections (pokemini's 24 new titles in its
    // section, past 30 titles and sticky), snes's and psx's rooms with theirs (34 titles in all), nothing moved;
    // megadrive enters the building (below).
    REQUIRE(imported.fixture.applyOverlay("additions").has_value());
    imported.importLibrary();
    rv::test::LogCapture addedLog;
    const WorldBuild added =
        rv::app::buildWorld(inputOf(imported, buildingSnapshotOf(imported), shared), rv::core::JobContext{});
    INFO(added.reason);
    REQUIRE(added.outcome == WorldOutcome::Updated);
    CHECK(added.written);
    REQUIRE(added.changes.has_value());
    REQUIRE(added.plan.has_value());
    CHECK(added.changes->heldRooms.empty());
    CHECK(added.changes->added.size() == 46); // the 34 titles, and megadrive's 12 in its new section
    for (const rv::world::Placement& placement : first.plan->placements) {
        INFO(placement.game.toString());
        CHECK(std::find(added.plan->placements.begin(), added.plan->placements.end(), placement) !=
              added.plan->placements.end());
    }
    CHECK(roomOf(added, shared).covers.size() == 38);
    const auto sharedAfter = std::find_if(added.plan->rooms.begin(), added.plan->rooms.end(),
                                          [&](const rv::world::Room& candidate) { return candidate.id == shared; });
    REQUIRE(sharedAfter != added.plan->rooms.end());
    CHECK(sharedAfter->sections == std::vector<rv::catalog::SystemName>{pocketlab, pokemini});
    // The app's building job enters the catalog's new systems as the CLI's whole update does (I4 steps 6a and 6b):
    // megadrive, 12 titles, takes a section of gen4's new shared room in the south row; the log says so.
    const auto megadriveRoom = std::find_if(added.plan->rooms.begin(), added.plan->rooms.end(),
                                            [](const rv::world::Room& candidate) {
                                                return candidate.id.toString() == "area:shared:gen4";
                                            });
    REQUIRE(megadriveRoom != added.plan->rooms.end());
    CHECK(megadriveRoom->sections ==
          std::vector<rv::catalog::SystemName>{rv::catalog::SystemName::parse("megadrive").value()});
    REQUIRE(added.changes->systemsEntered.size() == 1);
    CHECK(added.changes->systemsEntered.front().system.str() == "megadrive");
    CHECK(added.changes->roomsAdded.size() == 1);
    CHECK(added.changes->systemsWaiting.empty());
    INFO(addedLog.text());
    CHECK(addedLog.count("World: megadrive enters the building in area:shared:gen4 (a new room)") == 1);
    // kodi, excluded (no room), left out with a log line, as `worldplan update` notes it.
    CHECK(addedLog.count("system(s) of the catalog left out of the building, no room: kodi") == 1);
    // The CLI's update then finds the app's plan up to date.
    const std::string addedText = rv::core::readTextFile(planFile).value();
    const CliRun again = runCli(imported, {"worldplan", "update"});
    INFO(again.out);
    INFO(again.err);
    CHECK(again.code == rv::cli::ExitCode::Ok);
    CHECK(again.out.find("is up to date") != std::string::npos);
    CHECK(rv::core::readTextFile(planFile).value() == addedText);
}

TEST_CASE("app/world_build: the expansions an update added are named only once it saved the building (I4b step 8)",
          "[app][world_build][expansion]") {
    const auto room = [](std::string_view id) { return rv::world::RoomId::parse(id).value(); };
    WorldBuild build;
    build.outcome = WorldOutcome::Updated;
    build.written = true;
    build.changes.emplace();
    build.changes->roomsAdded = {room("area:arcade:2"), room("room:snes"), room("room:amiga:2")};
    // The expansions only: a room alone the update added is no chain's news.
    CHECK(rv::app::expansionsAddedBy(build) ==
          std::vector<rv::world::RoomId>{room("area:arcade:2"), room("room:amiga:2")});
    // The save failed (the world's lock held by the CLI): nothing named; the next start's update names them.
    WorldBuild unsaved = build;
    unsaved.written = false;
    unsaved.saveError = rv::Error{rv::ErrorCode::Conflict, "the world's lock is held by another program", {}};
    CHECK(rv::app::expansionsAddedBy(unsaved).empty());
    // A first building, a preview, no change report: none.
    WorldBuild generated = build;
    generated.outcome = WorldOutcome::Generated;
    CHECK(rv::app::expansionsAddedBy(generated).empty());
    WorldBuild preview = build;
    preview.preview = true;
    CHECK(rv::app::expansionsAddedBy(preview).empty());
    WorldBuild none = build;
    none.changes.reset();
    CHECK(rv::app::expansionsAddedBy(none).empty());
}
