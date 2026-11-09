// I12 step 3 (ADR-0021 §1): the game a console room played last, standing in its console (room_last_played.hpp):
// the choice from the items' sessions and the catalog's, the pose in the socket, the proxy's one-item layout, its
// cover.

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/app/room_last_played.hpp>

namespace {

using rv::app::RoomItemScreen;
using rv::app::RoomItemText;
using rv::catalog::GameId;

GameId gameId(std::uint64_t value) {
    return GameId::fromValue(value).value();
}

/// A room of cartridges standing spine-out on a shelf: game i in slot i, two groups (a module, a display case).
rv::world::CollectionLayout roomOf(std::initializer_list<std::uint64_t> games) {
    rv::world::CollectionLayout layout;
    layout.room = rv::world::RoomId::parse("room:snes").value();
    rv::world::PickId pick = 1;
    float x = -1.0f;
    for (const std::uint64_t game : games) {
        rv::world::CollectionItem item;
        item.game = gameId(game);
        item.pickId = pick++;
        item.slotFrame = glm::translate(glm::mat4(1.0f), glm::vec3(x, 1.2f, -4.5f));
        item.slotBox = rv::core::Aabb::fromMinMax(glm::vec3(-0.01f, 0.0f, -0.0625f), glm::vec3(0.01f, 0.085f, 0.0625f));
        // Spine-out: the item turned a quarter about +Y, its centre half its height up.
        item.transform = item.slotFrame * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0425f, 0.0f)) *
                         glm::rotate(glm::mat4(1.0f), 0.5f * 3.14159265f, glm::vec3(0.0f, 1.0f, 0.0f));
        item.frontLayerKey = item.game.toString() + "/front";
        item.spineLayerKey = item.game.toString() + "/spine";
        item.title = "Game " + std::to_string(game);
        layout.items.push_back(item);
        x += 0.03f;
    }
    rv::world::CollectionGroup shelf;
    shelf.mediaFamily = "cartridge_home16";
    shelf.itemSize = glm::vec3(0.125f, 0.085f, 0.02f);
    shelf.firstItem = 0;
    shelf.itemCount = static_cast<std::uint32_t>(layout.items.size() - 1);
    shelf.clusterCount = 3;
    rv::world::CollectionGroup display = shelf;
    display.mediaFamily = "cartridge_home16_display";
    display.firstItem = shelf.itemCount;
    display.itemCount = 1;
    display.firstCluster = 3;
    layout.groups = {shelf, display};
    return layout;
}

RoomItemText played(std::uint64_t game, std::optional<std::int64_t> when) {
    RoomItemText text;
    text.game = gameId(game);
    text.screen = RoomItemScreen{};
    text.screen->lastPlayed = when;
    return text;
}

/// A console's socket on a cabinet 0.55 m high, turned a little.
rv::app::ConsoleSocket socketAt(const glm::vec3& at) {
    rv::app::ConsoleSocket socket;
    socket.transform = glm::rotate(glm::translate(glm::mat4(1.0f), at), -0.14f, glm::vec3(0.0f, 1.0f, 0.0f));
    socket.opening = glm::vec2(0.15f, 0.03f);
    return socket;
}

bool near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-5f) {
    return glm::length(a - b) <= epsilon;
}

} // namespace

TEST_CASE("app/room_last_played: the game played last, the catalog's sessions over the items'", "[app][last_played]") {
    const rv::world::CollectionLayout room = roomOf({11, 12, 13, 14});
    std::map<GameId, RoomItemText> texts{{gameId(11), played(11, 1000)},
                                         {gameId(12), played(12, 3000)},
                                         {gameId(13), played(13, 3000)},
                                         {gameId(14), played(14, std::nullopt)}};
    const auto textOf = [&](const GameId& game) -> const RoomItemText* {
        const auto found = texts.find(game);
        return found != texts.end() ? &found->second : nullptr;
    };
    // The latest, the smaller GameId on a tie.
    CHECK(rv::app::lastPlayedGame(room, textOf, nullptr) == gameId(12));
    // None ever played: none.
    for (auto& [game, text] : texts) {
        text.screen->lastPlayed.reset();
    }
    CHECK_FALSE(rv::app::lastPlayedGame(room, textOf, nullptr).has_value());
    // A game the room no longer places counts for nothing; the catalog's sessions stand in for the items'.
    const std::map<GameId, std::int64_t> sessions{{gameId(14), 5000}, {gameId(99), 9000}, {gameId(11), 4000}};
    CHECK(rv::app::lastPlayedGame(room, textOf, &sessions) == gameId(14));
    CHECK_FALSE(rv::app::lastPlayedGame(room, {}, nullptr).has_value());
    CHECK_FALSE(rv::app::lastPlayedGame(rv::world::CollectionLayout{}, textOf, &sessions).has_value());
}

TEST_CASE("app/room_last_played: the proxy stands where the insertion leaves its item, alone in its layout",
          "[app][last_played]") {
    const rv::world::CollectionLayout room = roomOf({21, 22, 23});
    const rv::app::ConsoleSocket socket = socketAt(glm::vec3(-0.2f, 0.62f, -4.7f));
    // The item at the insertion's end: its centre at the socket, the socket's axes, shrunk into the opening.
    const rv::world::CollectionItem& source = room.items[1];
    const glm::mat4 pose = rv::app::lastPlayedTransform(source, socket);
    const float scale = rv::app::insertedScale(rv::app::itemExtents(source), socket.opening);
    CHECK(pose == rv::app::insertedItemTransform(source.transform, socket, scale, 1.0f));
    CHECK(near(glm::vec3(pose[3]), socket.position()));
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(near(glm::normalize(glm::vec3(pose[axis])), glm::normalize(glm::vec3(socket.transform[axis]))));
    }

    const auto layout = rv::app::lastPlayedLayout(room, gameId(22), socket);
    REQUIRE(layout.has_value());
    CHECK(layout->room == room.room);
    REQUIRE(layout->items.size() == 1);
    const rv::world::CollectionItem& item = layout->items.front();
    CHECK(item.game == gameId(22));
    CHECK(item.pickId == 1);
    CHECK(item.stance == rv::world::ItemStance::FaceOut);
    CHECK(item.inFixture);
    CHECK(item.transform == pose);
    CHECK(item.frontLayerKey == source.frontLayerKey);
    CHECK(item.title == source.title);
    // Its box follows it: the same extents as the room's item, around the socket.
    CHECK(near(rv::app::itemExtents(item), rv::app::itemExtents(source)));
    CHECK(item.worldBounds().contains(socket.position()));
    REQUIRE(layout->clusters.size() == 1);
    CHECK(layout->clusters.front().itemCount == 1);
    CHECK(layout->clusters.front().bounds == item.worldBounds());
    REQUIRE(layout->groups.size() == 1);
    CHECK(layout->groups.front().mediaFamily == "cartridge_home16");
    CHECK(layout->groups.front().firstItem == 0);
    CHECK(layout->groups.front().itemCount == 1);
    CHECK(layout->groups.front().clusterCount == 1);
    // The display case's item takes its own group; a game the room does not place gives none.
    const auto display = rv::app::lastPlayedLayout(room, gameId(23), socket);
    REQUIRE(display.has_value());
    CHECK(display->groups.front().mediaFamily == "cartridge_home16_display");
    CHECK_FALSE(rv::app::lastPlayedLayout(room, gameId(99), socket).has_value());
}

TEST_CASE("app/room_last_played: the proxy's cover is the room's", "[app][last_played]") {
    std::vector<rv::app::CoverItem> covers(2);
    covers[0].game = gameId(31);
    covers[0].coverPath = "snes/covers/a.png";
    covers[1].game = gameId(32);
    covers[1].coverPath = "snes/covers/b.png";
    const auto cover = rv::app::lastPlayedCover(covers, gameId(32));
    REQUIRE(cover.has_value());
    CHECK(*cover == covers[1]);
    CHECK_FALSE(rv::app::lastPlayedCover(covers, gameId(33)).has_value());
}
