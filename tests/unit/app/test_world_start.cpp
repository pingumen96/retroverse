// The persisted start's GPU-free parts (I3 step 10c): the room's items kept for it (rv/app/room_items.hpp: the
// canonical file, its reading and its rules) and the reconciliation's decision (rv/app/world_reconcile.hpp) on
// hand-made collections. The fixture-based cases (the preview of a stored plan, the items written by the room's job,
// the hero CRT's games) are in test_world_build.cpp.

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/app/room_items.hpp>
#include <rv/app/world_reconcile.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/file_io.hpp>
#include <rv/scene/instance_data.hpp>

#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::app::LayoutDifference;
using rv::app::Reconciliation;
using rv::app::RoomItems;
using rv::app::RoomItemText;
using rv::app::WorldBuild;

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

rv::world::CollectionLayout layoutOf(std::vector<rv::world::CollectionItem> items) {
    rv::world::CollectionLayout layout;
    layout.items = std::move(items);
    return layout;
}

rv::app::CoverItem cover(std::uint64_t game, std::string title) {
    rv::app::CoverItem made;
    made.game = gameId(game);
    made.title = std::move(title);
    made.fallbackTitle = made.title + " (USA)";
    made.systemName = "Lumen 16 Home System";
    made.year = "1993";
    made.coverPath = "snes/covers/" + made.fallbackTitle + ".png";
    return made;
}

/// A build with a plan (its digest), a scene, its data and `layout`.
WorldBuild built(std::string digest, rv::world::CollectionLayout layout, std::vector<rv::app::CoverItem> covers = {}) {
    WorldBuild build;
    build.plan = rv::world::WorldPlan{};
    build.planDigest = std::move(digest);
    build.scene = rv::scene::SceneDescription{};
    build.sceneData = rv::world::SceneData{};
    build.layout = std::move(layout);
    build.covers = std::move(covers);
    return build;
}

} // namespace

TEST_CASE("app/room_items: the room's items round trip as canonical JSON, by GameId, without absolute paths",
          "[app][world_start]") {
    const std::vector<rv::app::CoverItem> covers{cover(30, "Cobalt Compass"), cover(10, "Amber Lantern"),
                                                 cover(20, "Kiln Keeper: The Dappled Almanac")};
    const rv::world::CollectionLayout layout =
        layoutOf({item(10, 1), item(20, 2, 0.1f, rv::scene::instance_flags::kUnavailable), item(30, 3, 0.2f)});
    const RoomItems items = rv::app::roomItemsOf(covers, layout);
    CHECK(items.systemName == "Lumen 16 Home System");
    REQUIRE(items.items.size() == 3);
    CHECK(items.items[0].game == gameId(10));
    CHECK(items.items[1].game == gameId(20));
    CHECK_FALSE(items.items[1].available);
    CHECK(items.items[2].available);
    REQUIRE(items.find(gameId(20)) != nullptr);
    CHECK(items.find(gameId(20))->title == "Kiln Keeper: The Dappled Almanac");
    CHECK(items.find(gameId(40)) == nullptr);

    const auto text = rv::core::writeCanonicalJson(rv::app::roomItemsToJson(items));
    REQUIRE(text.has_value());
    const auto parsed = rv::app::parseRoomItems(nlohmann::json::parse(*text), "items.json");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == items);

    // The titles the preview's layout takes: every placed game, unknown ones without a title and available.
    rv::world::WorldPlan plan;
    for (const std::uint64_t game : {30, 10, 50}) {
        rv::world::Placement placement;
        placement.game = gameId(game);
        plan.placements.push_back(placement);
    }
    const std::vector<rv::world::CollectionTitle> titles = rv::app::collectionTitlesOf(items, plan);
    REQUIRE(titles.size() == 3);
    CHECK(titles[0] == rv::world::CollectionTitle{gameId(10), "Amber Lantern", true});
    CHECK(titles[1] == rv::world::CollectionTitle{gameId(30), "Cobalt Compass", true});
    CHECK(titles[2] == rv::world::CollectionTitle{gameId(50), "", true});

    // Written once; the same items again write nothing.
    const rv::test::TempDir dir("room-items");
    const fs::path file = rv::app::roomItemsFile(dir.file("cache"), rv::catalog::LibraryId::parse("esde").value(),
                                                 rv::catalog::SystemName::parse("snes").value());
    CHECK(file == dir.file("cache") / "world" / "esde" / "room-snes.json");
    const auto missing = rv::app::readRoomItems(file);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
    CHECK(rv::app::writeRoomItems(file, items).value());
    CHECK(rv::core::readTextFile(file).value() == *text);
    CHECK_FALSE(rv::app::writeRoomItems(file, items).value());
    CHECK(rv::app::readRoomItems(file).value() == items);
    RoomItems changed = items;
    changed.items[0].title = "Amber Lantern Deluxe";
    CHECK(rv::app::writeRoomItems(file, changed).value());
    CHECK(rv::app::readRoomItems(file).value() == changed);

    // The system's platform tags (I4 step 2: the preview classifies a custom system by them): kept when there are any,
    // a file without them (I3's) reads as none.
    CHECK_FALSE(rv::app::roomItemsToJson(items).contains("platforms"));
    RoomItems tagged = changed;
    tagged.platforms = {"gb", "gbc"};
    const nlohmann::json taggedJson = rv::app::roomItemsToJson(tagged);
    CHECK(taggedJson["platforms"] == nlohmann::json::array({"gb", "gbc"}));
    CHECK(rv::app::parseRoomItems(taggedJson, "items.json").value() == tagged);
    CHECK(rv::app::writeRoomItems(file, tagged).value());
    CHECK(rv::app::readRoomItems(file).value() == tagged);
}

TEST_CASE("app/room_items: a file of another shape is refused with its JSON pointer", "[app][world_start]") {
    const nlohmann::json good = rv::app::roomItemsToJson(
        rv::app::roomItemsOf(std::vector<rv::app::CoverItem>{cover(10, "Amber Lantern"), cover(20, "Bramble")},
                             layoutOf({item(10, 1), item(20, 2)})));
    struct Case {
        const char* name;
        nlohmann::json document;
        rv::ErrorCode code;
        const char* context;
    };
    nlohmann::json newer = good;
    newer["schemaVersion"] = 2;
    nlohmann::json badGame = good;
    badGame["items"][1]["game"] = "not a game";
    nlohmann::json unordered = good;
    std::swap(unordered["items"][0], unordered["items"][1]);
    nlohmann::json twice = good;
    twice["items"][1] = twice["items"][0];
    nlohmann::json noAvailability = good;
    noAvailability["items"][0].erase("available");
    nlohmann::json numberTitle = good;
    numberTitle["items"][0]["title"] = 7;
    nlohmann::json platformsObject = good;
    platformsObject["platforms"] = nlohmann::json::object();
    nlohmann::json platformNumber = good;
    platformNumber["platforms"] = nlohmann::json::array({"gb", 7});
    const std::vector<Case> cases{
        {"not an object", nlohmann::json::array(), rv::ErrorCode::ParseError, "items.json:"},
        {"a newer schema", newer, rv::ErrorCode::Unsupported, "items.json:/schemaVersion"},
        {"a game that is no GameId", badGame, rv::ErrorCode::ParseError, "items.json:/items/1/game"},
        {"games out of order", unordered, rv::ErrorCode::ParseError, "items.json:/items/1/game"},
        {"a game twice", twice, rv::ErrorCode::ParseError, "items.json:/items/1/game"},
        {"no availability", noAvailability, rv::ErrorCode::ParseError, "items.json:/items/0/available"},
        {"a title that is no string", numberTitle, rv::ErrorCode::ParseError, "items.json:/items/0/title"},
        {"platforms that are no array", platformsObject, rv::ErrorCode::ParseError, "items.json:/platforms"},
        {"a platform that is no string", platformNumber, rv::ErrorCode::ParseError, "items.json:/platforms/1"},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const auto parsed = rv::app::parseRoomItems(c.document, "items.json");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == c.code);
        CHECK(parsed.error().context == c.context);
    }
}

TEST_CASE("app/world_reconcile: two collections compared by game", "[app][world_start]") {
    const rv::world::CollectionLayout before = layoutOf({item(1, 10), item(2, 11), item(3, 12), item(4, 13)});
    CHECK(rv::app::compareLayouts(before, before) == LayoutDifference{4, 0, 0, 0, 0});
    CHECK(rv::app::compareLayouts(before, before).same());
    // Game 2 moved (another transform), game 3 flagged unavailable, game 4 gone, game 5 new.
    const rv::world::CollectionLayout after = layoutOf(
        {item(1, 10), item(2, 11, 0.5f), item(3, 12, 0.0f, rv::scene::instance_flags::kUnavailable), item(5, 14)});
    const LayoutDifference difference = rv::app::compareLayouts(before, after);
    CHECK(difference == LayoutDifference{1, 1, 1, 1, 1});
    CHECK_FALSE(difference.same());
    // Another pick id is a move too.
    CHECK(rv::app::compareLayouts(before, layoutOf({item(1, 99), item(2, 11), item(3, 12), item(4, 13)})).moved == 1);
}

TEST_CASE("app/world_reconcile: the stored room kept, rebuilt or replaced by the shell", "[app][world_start]") {
    const std::vector<rv::app::CoverItem> covers{cover(1, "Amber Lantern"), cover(2, "Bramble Burrow Rally")};
    const WorldBuild shown = built("digest-a", layoutOf({item(1, 10), item(2, 11)}), covers);

    SECTION("the same plan and collection, the same covers, the media unknown: nothing to do but the CRT") {
        const WorldBuild build = built("digest-a", layoutOf({item(1, 10), item(2, 11)}), covers);
        const rv::app::ReconcilePlan plan = rv::app::planReconciliation(shown, build);
        CHECK(plan.kind == Reconciliation::Textures);
        CHECK(plan.difference == LayoutDifference{2, 0, 0, 0, 0});
        CHECK_FALSE(plan.reloadCovers);
    }
    SECTION("the media root known: the covers served again, their sources checked") {
        WorldBuild build = built("digest-a", layoutOf({item(1, 10), item(2, 11)}), covers);
        build.mediaRoot = fs::path("M:/media");
        const rv::app::ReconcilePlan plan = rv::app::planReconciliation(shown, build);
        CHECK(plan.kind == Reconciliation::Textures);
        CHECK(plan.reloadCovers);
    }
    SECTION("a title changed: the covers and spines served again, nothing rebuilt") {
        std::vector<rv::app::CoverItem> renamed = covers;
        renamed[1].title = "Bramble Burrow Rally Deluxe";
        const WorldBuild build = built("digest-a", layoutOf({item(1, 10), item(2, 11)}), renamed);
        const rv::app::ReconcilePlan plan = rv::app::planReconciliation(shown, build);
        CHECK(plan.kind == Reconciliation::Textures);
        CHECK(plan.reloadCovers);
    }
    SECTION("another plan, a game added: rebuilt, nothing moved") {
        const WorldBuild build = built("digest-b", layoutOf({item(1, 10), item(2, 11), item(3, 12)}));
        const rv::app::ReconcilePlan plan = rv::app::planReconciliation(shown, build);
        CHECK(plan.kind == Reconciliation::Rebuild);
        CHECK(plan.difference == LayoutDifference{2, 0, 0, 1, 0});
    }
    SECTION("the same plan, an availability changed: rebuilt") {
        const WorldBuild build = built(
            "digest-a", layoutOf({item(1, 10), item(2, 11, 0.0f, rv::scene::instance_flags::kUnavailable)}), covers);
        const rv::app::ReconcilePlan plan = rv::app::planReconciliation(shown, build);
        CHECK(plan.kind == Reconciliation::Rebuild);
        CHECK(plan.difference.flagsChanged == 1);
    }
    SECTION("no plan: the shell") {
        WorldBuild build;
        build.outcome = rv::app::WorldOutcome::NoPlan;
        CHECK(rv::app::planReconciliation(shown, build).kind == Reconciliation::Shell);
        // A plan without its scene is no room either.
        WorldBuild sceneless = built("digest-a", layoutOf({item(1, 10)}));
        sceneless.scene.reset();
        CHECK(rv::app::planReconciliation(shown, sceneless).kind == Reconciliation::Shell);
    }
    CHECK(rv::app::toString(Reconciliation::Textures) == "textures");
    CHECK(rv::app::toString(Reconciliation::Rebuild) == "rebuild");
    CHECK(rv::app::toString(Reconciliation::Shell) == "shell");
}
