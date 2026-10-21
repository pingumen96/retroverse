// The live update's decision (I3 step 15b, rv/app/world_live_plan.hpp; floors since I4 step 5a), GPU-free: on
// hand-made floors (nothing to do, the covers served again, the items applied in place, the floor committed again, a
// build with nothing to show kept out; a floor of two rooms compared room by room), and on the snes-room fixture
// (tests/fixtures/esde/snes-room) imported: the building job run again after a ROM is added and the catalog imported
// gives the same static part with one item more (applied in place, nothing moved), and after a ROM is deleted keeps
// its slot (the item dimmed in place).

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
#include <rv/app/world_reconcile.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/scene/instance_data.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_test_support.hpp"

namespace fs = std::filesystem;
using rv::app::LiveUpdateKind;
using rv::app::LiveUpdatePlan;
using rv::app::ShownFloor;
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

const rv::world::RoomId kSnesRoom = rv::world::RoomId::parse("room:snes").value();

rv::world::CollectionLayout layoutOf(std::vector<rv::world::CollectionItem> items, std::string family = "cartridge",
                                     rv::world::RoomId room = kSnesRoom) {
    rv::world::CollectionLayout layout;
    layout.room = room;
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

/// A floor's collection room `layout` (its room's) with `covers`.
rv::app::RoomCollection collectionOf(rv::world::CollectionLayout layout, std::vector<rv::app::CoverItem> covers) {
    rv::app::RoomCollection room;
    room.room = layout.room;
    room.layout = std::move(layout);
    room.covers = std::move(covers);
    return room;
}

/// A build with a plan (its digest), a floor's scene (its digest), one collection room `layout` and `covers`, the media
/// root known.
WorldBuild built(std::string digest, std::string sceneDigest, rv::world::CollectionLayout layout,
                 std::vector<rv::app::CoverItem> covers) {
    WorldBuild build;
    build.outcome = WorldOutcome::Updated;
    build.plan = rv::world::WorldPlan{};
    build.planDigest = std::move(digest);
    build.floor = "gen4";
    build.scene = rv::scene::SceneDescription{};
    build.sceneDigest = std::move(sceneDigest);
    build.rooms.push_back(collectionOf(std::move(layout), std::move(covers)));
    build.mediaRoot = fs::path("M:/media");
    return build;
}

/// The room of three titles shown.
WorldBuild threeTitles() {
    return built("plan-a", "scene-a", layoutOf({item(10, 1), item(20, 2, 0.1f), item(30, 3, 0.2f)}),
                 {cover(10, "Amber Lantern"), cover(20, "Bramble Burrow Rally"), cover(30, "Cobalt Compass")});
}

ShownFloor shownOf(const WorldBuild& build) {
    std::optional<ShownFloor> shown = rv::app::shownFloorOf(build);
    REQUIRE(shown.has_value());
    return std::move(*shown);
}

} // namespace

TEST_CASE("app/world_live_plan: what the session keeps of the floor it shows", "[app][world_live]") {
    const WorldBuild build = threeTitles();
    const ShownFloor shown = shownOf(build);
    CHECK(shown.planDigest == "plan-a");
    CHECK(shown.sceneDigest == "scene-a");
    CHECK(shown.floor == "gen4");
    REQUIRE(shown.rooms.size() == 1);
    CHECK(shown.rooms.front().room == kSnesRoom);
    CHECK(shown.rooms.front().layout.items.size() == 3);
    CHECK(shown.rooms.front().covers == build.rooms.front().covers);
    CHECK(shown.find(kSnesRoom) == &shown.rooms.front());
    CHECK(shown.mediaKnown);
    WorldBuild nothing;
    CHECK_FALSE(rv::app::shownFloorOf(nothing).has_value());
    CHECK(rv::app::toString(LiveUpdateKind::Items) == "items");
    CHECK(rv::app::toString(LiveUpdateKind::Kept) == "kept");
}

TEST_CASE("app/world_live_plan: an unchanged room does nothing; changed covers are served again", "[app][world_live]") {
    const ShownFloor shown = shownOf(threeTitles());
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
        build.rooms.front().covers[1].coverPath = "snes/covers/Bramble Burrow Rally (Europe).png";
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Textures);
        CHECK(plan.reloadCovers);
        CHECK(plan.coversChanged == std::vector<rv::catalog::GameId>{gameId(20)});
    }
    SECTION("the media root known now (it was out of reach)") {
        ShownFloor away = shown;
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
    const ShownFloor shown = shownOf(threeTitles());
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
        build.rooms.front().layout.items[2].flags = rv::scene::instance_flags::kUnavailable;
        CHECK(rv::app::planLiveUpdate(shown, build).kind == LiveUpdateKind::Items);
    }
}

TEST_CASE("app/world_live_plan: another static part, a moved item or another family commit the room again; a build "
          "with nothing to show keeps it",
          "[app][world_live]") {
    const ShownFloor shown = shownOf(threeTitles());
    SECTION("a dressed level taken or a module added: another scene") {
        WorldBuild build = threeTitles();
        build.planDigest = "plan-b";
        build.sceneDigest = "scene-b";
        build.rooms.front().layout.items.push_back(item(40, 9, 3.0f));
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Rebuild);
        CHECK_FALSE(plan.reason.empty());
        CHECK(plan.added == std::vector<rv::catalog::GameId>{gameId(40)});
    }
    SECTION("an item elsewhere (never by the incremental rules)") {
        WorldBuild build = threeTitles();
        build.planDigest = "plan-b";
        build.rooms.front().layout.items[0].transform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 1.0f, 0.0f));
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Rebuild);
        CHECK(plan.difference.moved == 1);
    }
    SECTION("another media family") {
        WorldBuild build = built("plan-b", "scene-a", layoutOf({item(10, 1), item(20, 2, 0.1f)}, "disc"), {});
        CHECK(rv::app::planLiveUpdate(shown, build).kind == LiveUpdateKind::Rebuild);
    }
    SECTION("a room shown whose static part is unknown") {
        ShownFloor unknown = shown;
        unknown.sceneDigest.clear();
        WorldBuild build = threeTitles();
        build.planDigest = "plan-b";
        build.rooms.front().layout.items.pop_back();
        CHECK(rv::app::planLiveUpdate(unknown, build).kind == LiveUpdateKind::Rebuild);
    }
    SECTION("another floor") {
        WorldBuild build = threeTitles();
        build.floor = "gen5";
        CHECK(rv::app::planLiveUpdate(shown, build).kind == LiveUpdateKind::Rebuild);
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

/// The building job and the floor's build on the imported fixture's catalog as it is now (every system's snapshot on a
/// reader of its own, then buildWorld), starting in the snes room.
WorldBuild roomJob(const ImportedFixture& imported) {
    auto database = imported.openDatabase();
    rv::app::WorldCatalogReader reader;
    reader.query = rv::catalog_db::SqliteCatalogQuery::open(database.openReader().value()).value();
    reader.profile = rv::test::fixtureLibrary(imported.fixture);
    std::vector<rv::catalog::GameId> placed;
    if (auto stored = rv::world::readStoredPlan(
            rv::world::worldFiles(imported.paths.userDataRoot(), reader.profile.id).plan)) {
        for (const rv::world::Placement& placement : stored->plan.placements) {
            placed.push_back(placement.game);
        }
    }
    rv::app::WorldBuildInput input;
    input.paths = rv::app::WorldPaths{imported.paths.assetRoot(), imported.paths.userDataRoot(),
                                      imported.paths.configDir(), imported.paths.cacheDir()};
    input.library = reader.profile.id;
    input.snapshot = rv::app::takeBuildingSnapshot(reader, placed, rv::app::roomSystemsFilter(input.paths.assetRoot));
    input.writer = "retroverse (test)";
    input.startRoom = kSnesRoom;
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
    const ShownFloor shown = shownOf(first);

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
    const ShownFloor fiftyOne = shownOf(added);
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
    const ShownFloor shown = shownOf(first);
    REQUIRE(fs::remove(imported.fixture.path("roms/snes/Cobalt Compass (USA).sfc")));
    imported.importLibrary();
    const WorldBuild after = roomJob(imported);
    REQUIRE(after.plan.has_value());
    REQUIRE(after.findRoom(kSnesRoom) != nullptr);
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

TEST_CASE("app/world_live_plan: a floor of two rooms is compared room by room", "[app][world_live]") {
    const rv::world::RoomId gb = rv::world::RoomId::parse("room:gb").value();
    const auto twoRooms = [&](std::vector<rv::world::CollectionItem> gbItems, std::string sceneDigest) {
        WorldBuild build = threeTitles();
        build.sceneDigest = std::move(sceneDigest);
        std::vector<rv::app::CoverItem> covers;
        for (const rv::world::CollectionItem& made : gbItems) {
            covers.push_back(cover(made.game.value(), "Pocket " + std::to_string(made.game.value())));
        }
        build.rooms.push_back(collectionOf(layoutOf(std::move(gbItems), "cartridge", gb), std::move(covers)));
        return build;
    };
    // The same pick ids in both rooms: each room's items are compared with its own.
    const ShownFloor shown = shownOf(twoRooms({item(110, 1), item(120, 2, 0.1f)}, "scene-a"));
    CHECK(shown.rooms.size() == 2);
    SECTION("the same floor") {
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, twoRooms({item(110, 1), item(120, 2, 0.1f)},
                                                                            "scene-a"));
        CHECK(plan.kind == LiveUpdateKind::Unchanged);
        CHECK(plan.difference == rv::app::LayoutDifference{5, 0, 0, 0, 0});
    }
    SECTION("a title placed into a free slot of the second room") {
        WorldBuild build = twoRooms({item(110, 1), item(115, 5, 0.05f), item(120, 2, 0.1f)}, "scene-a");
        build.planDigest = "plan-b";
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Items);
        CHECK(plan.difference == rv::app::LayoutDifference{5, 0, 0, 1, 0});
        CHECK(plan.added == std::vector<rv::catalog::GameId>{gameId(115)});
    }
    SECTION("a room more on the floor: another static part") {
        WorldBuild build = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "scene-b");
        build.planDigest = "plan-b";
        build.rooms.push_back(collectionOf(layoutOf({item(210, 1)}, "cartridge",
                                                    rv::world::RoomId::parse("room:pokemini").value()),
                                           {cover(210, "Tiny")}));
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Rebuild);
        CHECK(plan.difference.added == 1);
    }
    SECTION("the second room's family changed") {
        WorldBuild build = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "scene-a");
        build.planDigest = "plan-b";
        build.rooms.back().layout.groups.front().mediaFamily = "disc";
        build.rooms.back().layout.items.pop_back();
        CHECK(rv::app::planLiveUpdate(shown, build).kind == LiveUpdateKind::Rebuild);
    }
    // The reconciliation of the persisted start compares the floor the same way.
    const WorldBuild before = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "scene-a");
    const WorldBuild same = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "scene-a");
    const rv::app::ReconcilePlan reconciled = rv::app::planReconciliation(before, same);
    CHECK(reconciled.kind == rv::app::Reconciliation::Textures);
    CHECK(reconciled.difference == rv::app::LayoutDifference{5, 0, 0, 0, 0});
    WorldBuild added = twoRooms({item(110, 1), item(115, 5, 0.05f), item(120, 2, 0.1f)}, "scene-a");
    added.planDigest = "plan-b";
    CHECK(rv::app::planReconciliation(before, added).kind == rv::app::Reconciliation::Rebuild);
}

TEST_CASE("app/world_live_plan: a room moved as a whole on its floor moves none of its items; an item moved in it does",
          "[app][world_live]") {
    // ADR-0013 §7: a room that grows moves away from the hall as a whole (followDepth), its items keeping their places
    // in its own frame; in the scene's frame (floorCollectionLayout) every item's transform changes.
    const rv::world::RoomId pocketlab = rv::world::RoomId::parse("room:pocketlab").value();
    const rv::world::CollectionLayout inRoom =
        layoutOf({item(110, 1, 0.5f), item(120, 2, 1.25f), item(130, 3, 2.0f)}, "cartridge", pocketlab);
    const auto placedAt = [&](const rv::world::CollectionLayout& layout, const rv::world::RoomPlacement& at) {
        WorldBuild build = threeTitles();
        rv::app::RoomCollection room = collectionOf(rv::world::floorCollectionLayout(layout, at),
                                                    {cover(110, "A"), cover(120, "B"), cover(130, "C")});
        room.scenePlacement = at;
        room.placement = at;
        build.rooms.push_back(std::move(room));
        return build;
    };
    // Across the hall (half-turned), then grown: its placement 1.2 m further from the hall.
    const rv::world::RoomPlacement before{14'300, 5'750, 1};
    const rv::world::RoomPlacement grown{14'300, 6'950, 1};
    const ShownFloor shown = shownOf(placedAt(inRoom, before));
    REQUIRE(shown.rooms.size() == 2);
    CHECK(shown.rooms.back().scenePlacement == before);
    WorldBuild moved = placedAt(inRoom, grown);
    moved.planDigest = "plan-b";
    moved.sceneDigest = "scene-b";
    REQUIRE(moved.rooms.back().layout.items.front().transform != shown.rooms.back().layout.items.front().transform);
    const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, moved);
    CHECK(plan.difference == rv::app::LayoutDifference{6, 0, 0, 0, 0});
    // Its static part changed (the room's walls, the hall): the floor committed again, for that reason.
    CHECK(plan.kind == LiveUpdateKind::Rebuild);
    CHECK(plan.reason.find("static part") != std::string::npos);
    const rv::app::ReconcilePlan reconciled = rv::app::planReconciliation(placedAt(inRoom, before), moved);
    CHECK(reconciled.kind == rv::app::Reconciliation::Rebuild);
    CHECK(reconciled.difference == rv::app::LayoutDifference{6, 0, 0, 0, 0});

    // An item of the room at another place in its own frame (1 mm along its level) is moved, whatever the placement.
    rv::world::CollectionLayout shifted = inRoom;
    shifted.items[1].transform = glm::translate(shifted.items[1].transform, glm::vec3(0.001f, 0.0f, 0.0f));
    CHECK(rv::app::planLiveUpdate(shown, placedAt(shifted, grown)).difference ==
          rv::app::LayoutDifference{5, 1, 0, 0, 0});
    CHECK(rv::app::planLiveUpdate(shown, placedAt(shifted, before)).difference ==
          rv::app::LayoutDifference{5, 1, 0, 0, 0});
    // The comparison itself: the same placement is exact, another one is the room's own frame.
    CHECK(rv::app::compareLayouts(rv::world::floorCollectionLayout(inRoom, before), before,
                                  rv::world::floorCollectionLayout(inRoom, grown), grown) ==
          rv::app::LayoutDifference{3, 0, 0, 0, 0});
    CHECK(rv::app::compareLayouts(rv::world::floorCollectionLayout(inRoom, before),
                                  rv::world::floorCollectionLayout(inRoom, grown))
              .moved == 3);
}

TEST_CASE("app/world_live_plan: room by room: an unchanged room stays, another's items or covers are applied alone",
          "[app][world_live]") {
    // I4 step 6b (ADR-0013 §9): each collection room decided apart, by its content digest, its items and its covers.
    const rv::world::RoomId gb = rv::world::RoomId::parse("room:gb").value();
    const auto twoRooms = [&](std::vector<rv::world::CollectionItem> gbItems, std::string gbDigest) {
        WorldBuild build = threeTitles();
        build.rooms.front().contentDigest = "snes-a";
        std::vector<rv::app::CoverItem> covers;
        for (const rv::world::CollectionItem& made : gbItems) {
            covers.push_back(cover(made.game.value(), "Pocket " + std::to_string(made.game.value())));
        }
        build.rooms.push_back(collectionOf(layoutOf(std::move(gbItems), "cartridge", gb), std::move(covers)));
        build.rooms.back().contentDigest = std::move(gbDigest);
        return build;
    };
    const ShownFloor shown = shownOf(twoRooms({item(110, 1), item(120, 2, 0.1f)}, "gb-a"));
    REQUIRE(shown.rooms.size() == 2);
    CHECK(shown.rooms.back().contentDigest == "gb-a");
    SECTION("nothing changed: every room unchanged") {
        const LiveUpdatePlan plan =
            rv::app::planLiveUpdate(shown, twoRooms({item(110, 1), item(120, 2, 0.1f)}, "gb-a"));
        CHECK(plan.kind == LiveUpdateKind::Unchanged);
        REQUIRE(plan.rooms.size() == 2);
        for (const rv::app::RoomUpdate& room : plan.rooms) {
            CHECK(room.kind == rv::app::RoomUpdateKind::Unchanged);
            CHECK_FALSE(room.planChanged);
        }
    }
    SECTION("a title placed in the second room: its items in place, the first room unchanged") {
        WorldBuild build = twoRooms({item(110, 1), item(115, 5, 0.05f), item(120, 2, 0.1f)}, "gb-b");
        build.planDigest = "plan-b";
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Items);
        REQUIRE(plan.find(kSnesRoom) != nullptr);
        CHECK(plan.find(kSnesRoom)->kind == rv::app::RoomUpdateKind::Unchanged);
        CHECK(plan.find(kSnesRoom)->difference == rv::app::LayoutDifference{3, 0, 0, 0, 0});
        REQUIRE(plan.find(gb) != nullptr);
        CHECK(plan.find(gb)->kind == rv::app::RoomUpdateKind::Items);
        CHECK(plan.find(gb)->planChanged);
        CHECK(plan.find(gb)->difference == rv::app::LayoutDifference{2, 0, 0, 1, 0});
    }
    SECTION("a cover of the first room replaced: that room's textures alone") {
        WorldBuild build = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "gb-a");
        build.rooms.front().covers[0].coverPath = "snes/covers/Amber Lantern (Europe).png";
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Textures);
        CHECK(plan.find(kSnesRoom)->kind == rv::app::RoomUpdateKind::Textures);
        CHECK(plan.find(kSnesRoom)->coversChanged == 1);
        CHECK(plan.find(gb)->kind == rv::app::RoomUpdateKind::Unchanged);
    }
    SECTION("a game of the other floor's room unplaced: another plan, this floor unchanged") {
        WorldBuild build = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "gb-a");
        build.planDigest = "plan-c";
        CHECK(rv::app::planLiveUpdate(shown, build).kind == LiveUpdateKind::Unchanged);
    }
    SECTION("the second room's CRT games changed alone") {
        WorldBuild build = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "gb-a");
        build.rooms.back().screenGames.push_back(rv::app::ScreenGame{"x", {}, std::nullopt, true});
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Unchanged);
        CHECK(plan.screenGamesChanged);
        CHECK(plan.find(gb)->screenGamesChanged);
        CHECK_FALSE(plan.find(kSnesRoom)->screenGamesChanged);
    }
    SECTION("a room added on the floor: rebuilt, the room listed as added") {
        WorldBuild build = twoRooms({item(110, 1), item(120, 2, 0.1f)}, "gb-a");
        build.sceneDigest = "scene-b";
        build.rooms.push_back(collectionOf(
            layoutOf({item(210, 1)}, "cartridge", rv::world::RoomId::parse("area:shared:handheld").value()),
            {cover(210, "Tiny")}));
        const LiveUpdatePlan plan = rv::app::planLiveUpdate(shown, build);
        CHECK(plan.kind == LiveUpdateKind::Rebuild);
        REQUIRE(plan.rooms.size() == 3);
        CHECK(plan.rooms.back().kind == rv::app::RoomUpdateKind::Added);
        CHECK(rv::app::toString(plan.rooms.back().kind) == "added");
    }
}

TEST_CASE("app/world_live_plan: the walker kept in the frame of the area that holds it, moved with it",
          "[app][world_live]") {
    // ADR-0013 §7: a floor committed again moves the walker with the room (or the hall) that holds it.
    const rv::world::RoomId gb = rv::world::RoomId::parse("room:gb").value();
    const auto floorOf = [&](const rv::world::RoomPlacement& gbAt, const rv::world::RoomPlacement& floorAt) {
        WorldBuild build = threeTitles();
        build.floorPlacement = floorAt;
        rv::app::RoomCollection room = collectionOf(layoutOf({item(110, 1)}, "cartridge", gb), {cover(110, "A")});
        room.scenePlacement = gbAt;
        build.rooms.push_back(std::move(room));
        return build;
    };
    const rv::world::RoomPlacement gbBefore{14'300, -5'750, 0};
    const ShownFloor shown = shownOf(floorOf(gbBefore, {}));
    CHECK(shown.floorPlacement == rv::world::RoomPlacement{});
    // The room grown 1.2 m away from the hall (north row: towards -z): the walker in it moves 1.2 m with it.
    const WorldBuild grown = floorOf({14'300, -6'950, 0}, {});
    const auto inRoom = rv::app::walkerMove(shown, grown, "room:gb");
    REQUIRE(inRoom.has_value());
    CHECK_FALSE(inRoom->halfTurn);
    const glm::vec3 feet = glm::vec3(inRoom->matrix * glm::vec4(15.0f, 0.0f, -7.0f, 1.0f));
    CHECK(glm::length(feet - glm::vec3(15.0f, 0.0f, -8.2f)) < 1e-4f);
    // The hall and the other rooms stay: no move.
    CHECK_FALSE(rv::app::walkerMove(shown, grown, "hall:gen4").has_value());
    CHECK_FALSE(rv::app::walkerMove(shown, grown, "room:snes").has_value());
    // The start room grown (the scene's frame is its): the hall moves in the scene's frame, the start room does not.
    const WorldBuild startGrown = floorOf(gbBefore, {-2'000, 1'200, 0});
    const auto inHall = rv::app::walkerMove(shown, startGrown, "hall:gen4");
    REQUIRE(inHall.has_value());
    const glm::vec3 hallFeet = glm::vec3(inHall->matrix * glm::vec4(3.0f, 0.0f, 1.0f, 1.0f));
    CHECK(glm::length(hallFeet - glm::vec3(1.0f, 0.0f, 2.2f)) < 1e-4f);
    // A half-turned frame turns the walker too.
    const auto turned = rv::app::walkerMove(shown, floorOf({14'300, -5'750, 1}, {}), "room:gb");
    REQUIRE(turned.has_value());
    CHECK(turned->halfTurn);
    // An area neither floor has: none.
    CHECK_FALSE(rv::app::walkerMove(shown, grown, "room:nes").has_value());
}
