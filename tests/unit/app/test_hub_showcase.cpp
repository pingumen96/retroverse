// The hub's favourites showcase, GPU-free (I6 step 5, rv/app/hub_showcase.hpp): its proxies' data (one family, its item
// prefab), the games from the catalog's favourite and recent list completed by the rooms' items files, their layout on
// the showcase's slots (face-out, pick ids from 1, the layer keys), their cover requests.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/app/hub_showcase.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/scene/instance_data.hpp>

namespace {

const std::filesystem::path& assetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

rv::catalog::GameId gameId(std::uint64_t value) {
    return *rv::catalog::GameId::parse(std::string("g") + std::string(16 - std::to_string(value).size(), '0') +
                                       std::to_string(value));
}

rv::catalog::RecentGame recent(std::uint64_t id, std::string_view system, bool favorite) {
    rv::catalog::RecentGame game;
    game.id = gameId(id);
    game.system = *rv::catalog::SystemName::parse(system);
    game.title = "Game " + std::to_string(id);
    game.favorite = favorite;
    return game;
}

/// A showcase of `count` slots in a row along +X, 0.3 m apart, their faces towards +Z.
rv::app::HubShowcase showcaseOf(std::size_t count) {
    rv::app::HubShowcase showcase;
    auto data = rv::app::loadShowcaseData(assetRoot());
    REQUIRE(data.has_value());
    showcase.data = std::move(*data);
    showcase.slotSize = glm::vec3(0.135f, 0.19f, 0.014f);
    for (std::size_t i = 0; i < count; ++i) {
        showcase.slots.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(0.3f * static_cast<float>(i), 0.6f, 6.6f)));
    }
    return showcase;
}

} // namespace

TEST_CASE("app/hub_showcase: the proxies' data: one keep-case family and its item prefab", "[app][hub_showcase]") {
    auto data = rv::app::loadShowcaseData(assetRoot());
    REQUIRE(data.has_value());
    CHECK(data->mediaFamily.id == rv::app::kShowcaseFamily);
    CHECK(data->mediaFamily.itemWidthMm == 135);
    CHECK(data->mediaFamily.itemHeightMm == 190);
    const rv::world::ScenePrefab* item = data->prefabs.find(data->mediaFamily.item);
    REQUIRE(item != nullptr);
    REQUIRE(item->prefab.has_value());
    CHECK(rv::geometry::findPart(*item->prefab, "plastic_shell") != nullptr);
    CHECK(rv::app::loadShowcaseData(assetRoot(), "no_such_family").error().code == rv::ErrorCode::NotFound);
}

TEST_CASE("app/hub_showcase: the games: the catalog's order, at most one per slot, covers from the items files",
          "[app][hub_showcase]") {
    const std::vector<rv::catalog::RecentGame> list{recent(1, "snes", true), recent(2, "psx", true),
                                                    recent(3, "snes", false), recent(4, "gb", false)};
    rv::app::RoomItems snes;
    snes.systemName = "Lumen 16 Home System";
    rv::app::RoomItemText text;
    text.game = gameId(3);
    text.title = "Game 3";
    text.fallbackTitle = "game three (europe)";
    text.year = "1994";
    text.coverPath = "snes/covers/game three.png";
    snes.items.push_back(text);
    const std::map<rv::catalog::SystemName, rv::app::RoomItems> items{{*rv::catalog::SystemName::parse("snes"), snes}};
    const std::vector<rv::app::ShowcaseGame> games = rv::app::showcaseGamesOf(list, items, 3);
    REQUIRE(games.size() == 3);
    CHECK(games[0].game == gameId(1));
    CHECK(games[0].favorite);
    CHECK(games[0].systemName == "Lumen 16 Home System"); // the items file's, the catalog's name unknown
    CHECK(games[0].coverPath.empty());                     // its room's file does not know it
    CHECK(games[1].game == gameId(2));
    CHECK(games[1].systemName.empty());                    // no file of its system
    CHECK(games[2].game == gameId(3));
    CHECK(games[2].coverPath == "snes/covers/game three.png");
    CHECK(games[2].fallbackTitle == "game three (europe)");
    CHECK(games[2].year == "1994");
    CHECK(rv::app::showcaseGamesOf(list, items, 0).empty());
}

TEST_CASE("app/hub_showcase: the layout: game i face-out on slot i, pick ids from 1, its layers' keys",
          "[app][hub_showcase]") {
    const rv::app::HubShowcase showcase = showcaseOf(4);
    const std::vector<rv::catalog::RecentGame> list{recent(7, "snes", true), recent(8, "psx", false)};
    const std::vector<rv::app::ShowcaseGame> games = rv::app::showcaseGamesOf(list, {}, showcase.slots.size());
    const rv::world::CollectionLayout layout = rv::app::showcaseLayout(showcase, games);
    REQUIRE(layout.items.size() == 2); // fewer games than slots: the rest empty
    REQUIRE(layout.groups.size() == 1);
    CHECK(layout.groups.front().mediaFamily == rv::app::kShowcaseFamily);
    CHECK(layout.groups.front().itemCount == 2);
    REQUIRE(layout.clusters.size() == 1);
    CHECK(layout.clusters.front().itemCount == 2);
    const glm::vec3 itemSize(0.135f, 0.19f, 0.014f);
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        const rv::world::CollectionItem& item = layout.items[i];
        INFO(i);
        CHECK(item.pickId == static_cast<rv::world::PickId>(i + 1));
        CHECK(item.game == games[i].game);
        CHECK(item.stance == rv::world::ItemStance::FaceOut);
        CHECK(item.slotFrame == showcase.slots[i]);
        CHECK(item.transform ==
              showcase.slots[i] * rv::geometry::slotItemTransform(rv::geometry::SlotFacing::FaceOut, itemSize));
        CHECK(item.frontLayerKey == games[i].game.toString() + "/front");
        CHECK(item.spineLayerKey == games[i].game.toString() + "/spine");
        CHECK((item.attributes().flags & rv::scene::instance_flags::kNoSpineLabel) != 0);
        CHECK(layout.clusters.front().bounds.contains(item.worldBounds()));
    }
    // Picked by their faces.
    const std::vector<rv::world::PickTarget> targets = layout.pickTargets();
    CHECK(targets.size() == 2);
    // The covers: one per item, the family's shapes, the game's text and seed.
    const std::vector<rv::app::CoverItem> covers = rv::app::showcaseCovers(showcase, layout, games);
    REQUIRE(covers.size() == 2);
    CHECK(covers[0].game == games[0].game);
    CHECK(covers[0].frontKey == layout.items[0].frontLayerKey);
    CHECK(covers[0].label == showcase.data.mediaFamily.coverAspect());
    CHECK(covers[0].spine == showcase.data.mediaFamily.spineAspect());
    CHECK(covers[0].title == "Game 7");
    CHECK(covers[0].seed != covers[1].seed);
    // No game: no item, the group still there (the collection draws nothing).
    CHECK(rv::app::showcaseLayout(showcase, {}).items.empty());
}
