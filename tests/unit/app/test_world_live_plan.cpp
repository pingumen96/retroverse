// The live update's decision (I3 step 15b, rv/app/world_live_plan.hpp), GPU-free: on hand-made rooms (nothing to do,
// the covers served again, the items applied in place, the room committed again, a build with nothing to show kept
// out), and on the snes-room fixture (tests/fixtures/esde/snes-room) imported: the room's job run again after a ROM is
// added and the catalog imported gives the same static part with one item more (applied in place, nothing moved), and
// after a ROM is deleted keeps its slot (the item dimmed in place).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/app/room_snapshot.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_live_plan.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/scene/instance_data.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_test_support.hpp"

namespace fs = std::filesystem;
using rv::app::LiveUpdateKind;
using rv::app::LiveUpdatePlan;
using rv::app::ShownRoom;
using rv::app::WorldBuild;
using rv::app::WorldOutcome;
using rv::test::ImportedFixture;

namespace {

rv::catalog::GameId gameId(std::uint64_t value) {
    return rv::catalog::GameId::fromValue(value).value();
}

rv::world::CollectionItem item(std::uint64_t game, std::uint32_t pick, float x = 0.0f, std::uint32_t flags = 0) {
    rv::world::CollectionItem made;
    made.game = gameId(game);
    made.pickId = pick;
    made.transform = glm::translate(glm::mat4(1.0f), glm::vec3(x, 1.0f, 0.0f));
    made.flags = flags;
    return made;
}

rv::world::CollectionLayout layoutOf(std::vector<rv::world::CollectionItem> items, std::string family = "cartridge") {
    rv::world::CollectionLayout layout;
    layout.items = std::move(items);
    rv::world::CollectionGroup group;
    group.mediaFamily = std::move(family);
    group.item = rv::world::PrefabRef{"media.cartridge.generic", "cartridge"};
    layout.groups.push_back(group);
    return layout;
}

rv::app::CoverItem cover(std::uint64_t game, std::string title) {
    rv::app::CoverItem made;
    made.game = gameId(game);
    made.title = std::move(title);
    made.coverPath = "snes/covers/" + made.title + ".png";
    return made;
}

/// A build with a plan (its digest), a scene (its digest), its data, `layout` and `covers`, the media root known.
WorldBuild built(std::string digest, std::string sceneDigest, rv::world::CollectionLayout layout,
                 std::vector<rv::app::CoverItem> covers) {
    WorldBuild build;
    build.outcome = WorldOutcome::Updated;
    build.plan = rv::world::WorldPlan{};
    build.planDigest = std::move(digest);
    build.scene = rv::scene::SceneDescription{};
    build.sceneDigest = std::move(sceneDigest);
    build.sceneData = rv::world::SceneData{};
    build.layout = std::move(layout);
    build.covers = std::move(covers);
    build.mediaRoot = fs::path("M:/media");
    return build;
}

/// The room of three titles shown.
WorldBuild threeTitles() {
    return built("plan-a", "scene-a", layoutOf({item(10, 1), item(20, 2, 0.1f), item(30, 3, 0.2f)}),
                 {cover(10, "Amber Lantern"), cover(20, "Bramble Burrow Rally"), cover(30, "Cobalt Compass")});
}

ShownRoom shownOf(const WorldBuild& build) {
    std::optional<ShownRoom> shown = rv::app::shownRoomOf(build);
    REQUIRE(shown.has_value());
    return std::move(*shown);
}

} // namespace

TEST_CASE("app/world_live_plan: what the session keeps of the room it shows", "[app][world_live]") {
    const WorldBuild build = threeTitles();
    const ShownRoom shown = shownOf(build);
    CHECK(shown.planDigest == "plan-a");
    CHECK(shown.sceneDigest == "scene-a");
    CHECK(shown.layout.items.size() == 3);
    CHECK(shown.covers == build.covers);
    CHECK(shown.mediaKnown);
    WorldBuild nothing;
    CHECK_FALSE(rv::app::shownRoomOf(nothing).has_value());
    CHECK(rv::app::toString(LiveUpdateKind::Items) == "items");
    CHECK(rv::app::toString(LiveUpdateKind::Kept) == "kept");
}

TEST_CASE("app/world_live_plan: an unchanged room does nothing; changed covers are served again", "[app][world_live]") {
    const ShownRoom shown = shownOf(threeTitles());
    SECTION("the same room") {
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, threeTitles());
        CHECK(plan.kind == LiveUpdateKind::Unchanged);
        CHECK(plan.difference == rv::app::LayoutDifference{3, 0, 0, 0, 0});
        CHECK_FALSE(plan.reloadCovers);
        CHECK(plan.added.empty());
        CHECK(plan.coversChanged.empty());
    }
    SECTION("a cover replaced: NoRebuild, its picture swapped") {
        WorldBuild build = threeTitles();
        build.covers[1].coverPath = "snes/covers/Bramble Burrow Rally (Europe).png";
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Textures);
        CHECK(plan.reloadCovers);
        CHECK(plan.coversChanged == std::vector<rv::catalog::GameId>{gameId(20)});
    }
    SECTION("the media root known now (it was out of reach)") {
        ShownRoom away = shown;
        away.mediaKnown = false;
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(away, threeTitles());
        CHECK(plan.kind == LiveUpdateKind::Textures);
        CHECK(plan.coversChanged.empty());
    }
    SECTION("the hero CRT's games changed alone") {
        WorldBuild build = threeTitles();
        build.screenGames.push_back(rv::app::ScreenGame{});
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Unchanged);
        CHECK(plan.screenGamesChanged);
    }
}

TEST_CASE("app/world_live_plan: titles placed into free slots of the same static part are applied in place",
          "[app][world_live]") {
    const ShownRoom shown = shownOf(threeTitles());
    SECTION("one title added") {
        WorldBuild build = built("plan-b", "scene-a",
                                 layoutOf({item(10, 1), item(15, 7, 0.05f), item(20, 2, 0.1f), item(30, 3, 0.2f)}),
                                 {cover(10, "Amber Lantern"), cover(15, "Zinnia Zephyr"),
                                  cover(20, "Bramble Burrow Rally"), cover(30, "Cobalt Compass")});
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Items);
        CHECK(plan.difference == rv::app::LayoutDifference{3, 0, 0, 1, 0});
        CHECK(plan.added == std::vector<rv::catalog::GameId>{gameId(15)});
        CHECK(plan.removed.empty());
        CHECK(plan.reloadCovers);
        CHECK(plan.coversChanged.empty());
        CHECK(plan.reason.empty());
    }
    SECTION("a game unavailable now keeps its slot, dimmed; a slot freed by an alias loses its item") {
        WorldBuild build = built("plan-b", "scene-a",
                                 layoutOf({item(10, 1), item(20, 2, 0.1f, rv::scene::instance_flags::kUnavailable)}),
                                 {cover(10, "Amber Lantern"), cover(20, "Bramble Burrow Rally")});
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Items);
        CHECK(plan.difference == rv::app::LayoutDifference{1, 0, 1, 0, 1});
        CHECK(plan.removed == std::vector<rv::catalog::GameId>{gameId(30)});
    }
    SECTION("the same plan with an availability changed") {
        WorldBuild build = threeTitles();
        build.layout->items[2].flags = rv::scene::instance_flags::kUnavailable;
        CHECK(rv::app::planLiveUpdate(shown, build).kind == LiveUpdateKind::Items);
    }
}

TEST_CASE("app/world_live_plan: another static part, a moved item or another family commit the room again; a build "
          "with nothing to show keeps it",
          "[app][world_live]") {
    const ShownRoom shown = shownOf(threeTitles());
    SECTION("a dressed level taken or a module added: another scene") {
        WorldBuild build = threeTitles();
        build.planDigest = "plan-b";
        build.sceneDigest = "scene-b";
        build.layout->items.push_back(item(40, 9, 3.0f));
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Rebuild);
        CHECK_FALSE(plan.reason.empty());
        CHECK(plan.added == std::vector<rv::catalog::GameId>{gameId(40)});
    }
    SECTION("an item elsewhere (never by the incremental rules)") {
        WorldBuild build = threeTitles();
        build.planDigest = "plan-b";
        build.layout->items[0].transform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 1.0f, 0.0f));
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Rebuild);
        CHECK(plan.difference.moved == 1);
    }
    SECTION("another media family") {
        WorldBuild build = built("plan-b", "scene-a", layoutOf({item(10, 1), item(20, 2, 0.1f)}, "disc"), {});
        CHECK(rv::app::planLiveUpdate(shown, build).kind == LiveUpdateKind::Rebuild);
    }
    SECTION("a room shown whose static part is unknown") {
        ShownRoom unknown = shown;
        unknown.sceneDigest.clear();
        WorldBuild build = threeTitles();
        build.planDigest = "plan-b";
        build.layout->items.pop_back();
        CHECK(rv::app::planLiveUpdate(unknown, build).kind == LiveUpdateKind::Rebuild);
    }
    SECTION("no plan, no scene") {
        WorldBuild nothing;
        nothing.reason = "the stored plan cannot be used: edited by hand";
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, nothing);
        CHECK(plan.kind == LiveUpdateKind::Kept);
        CHECK(plan.reason == nothing.reason);
        WorldBuild sceneless = threeTitles();
        sceneless.scene.reset();
        CHECK(rv::app::planLiveUpdate(shown, sceneless).kind == LiveUpdateKind::Kept);
    }
}

namespace {

const rv::catalog::SystemName kSnes = rv::catalog::SystemName::parse("snes").value();

/// The room's job on the imported fixture's catalog as it is now (the catalog worker's snapshot, then buildWorld).
WorldBuild roomJob(const ImportedFixture& imported) {
    auto database = imported.openDatabase();
    auto query = rv::catalog_db::SqliteCatalogQuery::open(database.openReader().value()).value();
    auto store = rv::catalog_db::SqliteCatalogStore::open(database).value();
    const rv::catalog::LibraryProfile library = rv::test::fixtureLibrary(imported.fixture);
    const auto settings = rv::esde::readSettings(library.esdeHome);
    REQUIRE(settings.has_value());
    rv::app::RoomSnapshotRequest request;
    request.serial = 1;
    request.system = kSnes;
    if (auto stored =
            rv::world::readStoredPlan(rv::world::worldFiles(imported.paths.userDataRoot(), library.id).plan)) {
        for (const rv::world::Placement& placement : stored->plan.placements) {
            request.placedGames.push_back(placement.game);
        }
    }
    rv::app::WorldBuildInput input;
    input.paths = rv::app::WorldPaths{imported.paths.assetRoot(), imported.paths.userDataRoot(),
                                      imported.paths.configDir(), imported.paths.cacheDir()};
    input.library = library.id;
    input.snapshot = rv::app::takeRoomSnapshot(*query, store.get(), rv::esde::resolveMediaRoot(*settings, library),
                                               library.id, "Lumen 16 Home System", request);
    input.writer = "retroverse (test)";
    return rv::app::buildWorld(input, rv::core::JobContext{});
}

} // namespace

namespace {

/// Lays `title`'s ROM and cover from the fixture's additions (G10's games) over its root.
void addGame(const ImportedFixture& imported, std::string_view title) {
    const std::string rom = "roms/snes/" + std::string(title) + " (USA).sfc";
    const std::string cover = "media/snes/covers/" + std::string(title) + " (USA).png";
    for (const std::string& file : {rom, cover}) {
        fs::copy_file(imported.fixture.path("additions/" + file), imported.fixture.path(file));
    }
}

} // namespace

TEST_CASE("app/world_live_plan: ROMs added and imported while the room shows: the first takes a dressed level (the "
          "room committed again), the next a free slot (in place); nothing moves",
          "[app][world_live]") {
    const ImportedFixture imported("world-live-added", true, "esde/snes-room");
    const WorldBuild first = roomJob(imported);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    CHECK(first.sceneDigest.size() == 64);
    const ShownRoom shown = shownOf(first);

    // The refresh found nothing new: nothing to do, nothing written.
    const WorldBuild again = roomJob(imported);
    CHECK(again.outcome == WorldOutcome::Unchanged);
    CHECK_FALSE(again.written);
    CHECK(again.sceneDigest == first.sceneDigest);
    CHECK(rv::app::planLiveUpdate(shown, again).kind == LiveUpdateKind::Unchanged);

    // The 50 titles all stand face-out: the first title added enters a spine-out level that was dressed (ADR-0010
    // §10's LocalReflow), so the static part changes.
    addGame(imported, "Zinnia Zephyr");
    imported.importLibrary();
    const WorldBuild added = roomJob(imported);
    REQUIRE(added.outcome == WorldOutcome::Updated);
    CHECK(added.written);
    REQUIRE(added.changes.has_value());
    CHECK(added.changes->added.size() == 1);
    CHECK(added.changes->dressingRemoved.size() == 1);
    CHECK(added.sceneDigest != first.sceneDigest);
    const LiveUpdatePlan dressed = rv::app::planLiveUpdate(shown, added);
    CHECK(dressed.kind == LiveUpdateKind::Rebuild);
    CHECK(dressed.difference == rv::app::LayoutDifference{50, 0, 0, 1, 0});
    CHECK(dressed.added == added.changes->added);

    // The next title takes a free slot of that level: the static part as it was, the items applied in place.
    const ShownRoom fiftyOne = shownOf(added);
    addGame(imported, "Kestrel Kite Festival");
    imported.importLibrary();
    const WorldBuild next = roomJob(imported);
    REQUIRE(next.outcome == WorldOutcome::Updated);
    REQUIRE(next.changes.has_value());
    CHECK(next.changes->dressingRemoved.empty());
    CHECK(next.changes->fixturesAdded.empty());
    CHECK(next.sceneDigest == added.sceneDigest);
    const LiveUpdatePlan plan = rv::app::planLiveUpdate(fiftyOne, next);
    CHECK(plan.kind == LiveUpdateKind::Items);
    CHECK(plan.difference == rv::app::LayoutDifference{51, 0, 0, 1, 0});
    CHECK(plan.added == next.changes->added);
    CHECK(plan.coversChanged.empty());
}

TEST_CASE("app/world_live_plan: a ROM deleted and imported while the room shows keeps its slot", "[app][world_live]") {
    const ImportedFixture imported("world-live-removed", true, "esde/snes-room");
    const WorldBuild first = roomJob(imported);
    REQUIRE(first.outcome == WorldOutcome::Generated);
    const ShownRoom shown = shownOf(first);
    REQUIRE(fs::remove(imported.fixture.path("roms/snes/Cobalt Compass (USA).sfc")));
    imported.importLibrary();
    const WorldBuild after = roomJob(imported);
    REQUIRE(after.plan.has_value());
    REQUIRE(after.layout.has_value());
    // ADR-0010 §10: every placement kept, the game unavailable now; nothing of the static part changes.
    CHECK(after.plan->placements == first.plan->placements);
    CHECK(after.sceneDigest == first.sceneDigest);
    const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, after);
    CHECK(plan.difference.moved == 0);
    CHECK(plan.difference.removed == 0);
    CHECK(plan.difference.added == 0);
    CHECK(plan.difference.flagsChanged == 1);
    CHECK(plan.kind == LiveUpdateKind::Items);
}
