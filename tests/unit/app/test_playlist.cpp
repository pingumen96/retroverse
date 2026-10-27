// The screens' playlists (I5 step 1, ADR-0016 §3): each mode's order as a table, no repeat within a cycle, the
// fallbacks to shuffle, excluded clips. The shuffled orders come from scripts/world_seed_oracle.py's derive_seed and
// SplitMix64 (Python's hashlib, independent of rv_core): a cycle is the room's games with a clip, in their order,
// shuffled by Random(deriveSeed(room id, screen id, mode, cycle)), its first game swapped with its last when it is the
// previous cycle's last. `python -I scripts/world_seed_oracle.py --check ... tests/unit/app/test_playlist.cpp` finds the
// oracle's playlist table below.

#include <rv/app/playlist.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using rv::app::Playlist;
using rv::app::PlaylistMode;
using rv::app::PlaylistOptions;
using rv::app::ScreenGame;

namespace {

using Ids = std::vector<std::string>;

ScreenGame game(std::string id, bool clip = true) {
    ScreenGame g;
    g.id = std::move(id);
    if (clip) {
        g.media.video = "C:/media/videos/" + g.id + ".mp4";
    } else {
        g.media.cover = "C:/media/covers/" + g.id + ".png";
    }
    return g;
}

/// Five games with clips in the room's (featured) order: a002, a004 and a005 favourites; a003, a004 and a001 played,
/// the newest first; sort keys alpha (a002), bravo (a004), charlie (a005), delta (a001), echo (a003). Then a game
/// without a clip, which no playlist plays.
std::vector<ScreenGame> roomGames() {
    std::vector<ScreenGame> games;
    const struct {
        const char* id;
        const char* sortKey;
        bool favorite;
        std::optional<std::int64_t> lastPlayed;
    } rows[] = {
        {"g000000000000a001", "delta", false, 100},  {"g000000000000a002", "alpha", true, std::nullopt},
        {"g000000000000a003", "echo", false, 300},   {"g000000000000a004", "bravo", true, 200},
        {"g000000000000a005", "charlie", true, std::nullopt},
    };
    for (const auto& row : rows) {
        ScreenGame g = game(row.id);
        g.sortKey = row.sortKey;
        g.favorite = row.favorite;
        g.lastPlayed = row.lastPlayed;
        games.push_back(std::move(g));
    }
    ScreenGame stills = game("g000000000000a009", false);
    stills.sortKey = "aaa";
    stills.favorite = true;
    stills.lastPlayed = 900;
    games.push_back(std::move(stills));
    return games;
}

/// The games next() gives over `count` calls.
Ids take(Playlist& playlist, std::size_t count) {
    Ids ids;
    for (std::size_t i = 0; i < count; ++i) {
        const std::optional<std::string> id = playlist.next();
        REQUIRE(id.has_value());
        ids.push_back(*id);
    }
    return ids;
}

Ids flatten(const std::vector<Ids>& cycles) {
    Ids all;
    for (const Ids& cycle : cycles) {
        all.insert(all.end(), cycle.begin(), cycle.end());
    }
    return all;
}

} // namespace

TEST_CASE("app/playlist: the modes' names", "[app][playlist]") {
    for (const PlaylistMode mode : {PlaylistMode::Shuffle, PlaylistMode::Sequential, PlaylistMode::Favorites,
                                    PlaylistMode::Recent, PlaylistMode::Explicit}) {
        CHECK(rv::app::parsePlaylistMode(rv::app::toString(mode)) == mode);
    }
    CHECK(rv::app::toString(PlaylistMode::Favorites) == "favorites");
    CHECK_FALSE(rv::app::parsePlaylistMode("Shuffle").has_value());
    CHECK_FALSE(rv::app::parsePlaylistMode("random").has_value());
}

TEST_CASE("app/playlist: each mode's order, cycle after cycle", "[app][playlist]") {
    const std::vector<ScreenGame> games = roomGames();
    struct Row {
        const char* name;
        PlaylistMode mode;
        const char* room;
        Ids explicitGames;
        PlaylistMode plays;
        std::vector<Ids> cycles;
    };
    const std::vector<Row> rows{
        // The oracle's shuffles of the five (the third cycle's raw order started with the second's last: swapped).
        {"shuffle",
         PlaylistMode::Shuffle,
         "room:snes",
         {},
         PlaylistMode::Shuffle,
         {{"g000000000000a002", "g000000000000a005", "g000000000000a001", "g000000000000a004", "g000000000000a003"},
          {"g000000000000a002", "g000000000000a003", "g000000000000a001", "g000000000000a005", "g000000000000a004"},
          {"g000000000000a002", "g000000000000a003", "g000000000000a001", "g000000000000a005", "g000000000000a004"},
          {"g000000000000a005", "g000000000000a003", "g000000000000a002", "g000000000000a004", "g000000000000a001"}}},
        // Another room, another order.
        {"shuffle in another room",
         PlaylistMode::Shuffle,
         "room:nes",
         {},
         PlaylistMode::Shuffle,
         {{"g000000000000a004", "g000000000000a002", "g000000000000a003", "g000000000000a005", "g000000000000a001"}}},
        {"sequential",
         PlaylistMode::Sequential,
         "room:snes",
         {},
         PlaylistMode::Sequential,
         {{"g000000000000a002", "g000000000000a004", "g000000000000a005", "g000000000000a001", "g000000000000a003"},
          {"g000000000000a002", "g000000000000a004", "g000000000000a005", "g000000000000a001", "g000000000000a003"}}},
        // The favourites with a clip, shuffled with their own seeds.
        {"favorites",
         PlaylistMode::Favorites,
         "room:snes",
         {},
         PlaylistMode::Favorites,
         {{"g000000000000a002", "g000000000000a004", "g000000000000a005"},
          {"g000000000000a004", "g000000000000a002", "g000000000000a005"}}},
        // The games played with a clip, the newest first (a009 has no clip).
        {"recent",
         PlaylistMode::Recent,
         "room:snes",
         {},
         PlaylistMode::Recent,
         {{"g000000000000a003", "g000000000000a004", "g000000000000a001"},
          {"g000000000000a003", "g000000000000a004", "g000000000000a001"}}},
        // The listed ids in their order, each once; unknown ones and one without a clip skipped.
        {"explicit",
         PlaylistMode::Explicit,
         "room:snes",
         {"g000000000000a004", "g00000000000ffff1", "g000000000000a001", "g000000000000a004", "g000000000000a009"},
         PlaylistMode::Explicit,
         {{"g000000000000a004", "g000000000000a001"}, {"g000000000000a004", "g000000000000a001"}}},
    };
    for (const Row& row : rows) {
        DYNAMIC_SECTION(row.name) {
            PlaylistOptions options;
            options.mode = row.mode;
            options.roomId = row.room;
            options.explicitGames = row.explicitGames;
            for (std::size_t cycle = 0; cycle < row.cycles.size(); ++cycle) {
                const std::optional<std::string_view> previous =
                    cycle == 0 ? std::nullopt : std::optional<std::string_view>(row.cycles[cycle - 1].back());
                CHECK(rv::app::playlistCycle(games, options, cycle, previous) == row.cycles[cycle]);
            }
            Playlist playlist(games, options);
            CHECK(playlist.mode() == row.plays);
            CHECK(playlist.order() == row.cycles.front());
            const Ids expected = flatten(row.cycles);
            CHECK(take(playlist, expected.size()) == expected);
            CHECK(playlist.cycle() == row.cycles.size() - 1);
            CHECK(playlist.current() == expected.back());
        }
    }
}

TEST_CASE("app/playlist: the shuffled cycles are the oracle's", "[app][playlist]") {
    // `python -I scripts/world_seed_oracle.py`'s playlist table, pasted as printed (its --check finds these lines
    // here): room id, screen id, mode, cycle, the previous cycle's last, the cycle's order. The rooms are roomGames()
    // and the test room's three clips (test_screen_policy.cpp's playlistRoom(), its order e002, e003, e001).
    struct OracleCycle {
        const char* room;
        const char* screen;
        const char* mode;
        std::uint64_t cycle;
        const char* previousLast;
        Ids order;
    };
    // clang-format off
    const std::vector<OracleCycle> cycles{
{"room:snes", "hero", "shuffle", 0, "", {"g000000000000a002", "g000000000000a005", "g000000000000a001", "g000000000000a004", "g000000000000a003"}},
{"room:snes", "hero", "shuffle", 1, "g000000000000a003", {"g000000000000a002", "g000000000000a003", "g000000000000a001", "g000000000000a005", "g000000000000a004"}},
{"room:snes", "hero", "shuffle", 2, "g000000000000a004", {"g000000000000a002", "g000000000000a003", "g000000000000a001", "g000000000000a005", "g000000000000a004"}},
{"room:snes", "hero", "shuffle", 3, "g000000000000a004", {"g000000000000a005", "g000000000000a003", "g000000000000a002", "g000000000000a004", "g000000000000a001"}},
{"room:nes", "hero", "shuffle", 0, "", {"g000000000000a004", "g000000000000a002", "g000000000000a003", "g000000000000a005", "g000000000000a001"}},
{"room:snes", "hero", "favorites", 0, "", {"g000000000000a002", "g000000000000a004", "g000000000000a005"}},
{"room:snes", "hero", "favorites", 1, "g000000000000a005", {"g000000000000a004", "g000000000000a002", "g000000000000a005"}},
{"m0_testroom", "hero", "shuffle", 0, "", {"g000000000000e002", "g000000000000e003", "g000000000000e001"}},
{"m0_testroom", "hero", "shuffle", 1, "g000000000000e001", {"g000000000000e002", "g000000000000e001", "g000000000000e003"}},
    };
    // clang-format on
    const std::vector<ScreenGame> testRoom{game("g000000000000e001"), game("g000000000000e002"),
                                           game("g000000000000e003"), game("g000000000000e009", false)};
    for (const OracleCycle& row : cycles) {
        INFO(row.room << " " << row.mode << " cycle " << row.cycle);
        PlaylistOptions options;
        options.roomId = row.room;
        options.screenId = row.screen;
        const std::optional<PlaylistMode> mode = rv::app::parsePlaylistMode(row.mode);
        REQUIRE(mode.has_value());
        options.mode = *mode;
        const std::string_view previous = row.previousLast;
        const std::optional<std::string_view> last =
            previous.empty() ? std::nullopt : std::optional<std::string_view>(previous);
        const std::vector<ScreenGame> games = row.room == std::string_view("m0_testroom") ? testRoom : roomGames();
        CHECK(rv::app::playlistCycle(games, options, row.cycle, last) == row.order);
    }
}

TEST_CASE("app/playlist: the explicit mode skips unknown ids and games without a clip, and says which",
          "[app][playlist]") {
    PlaylistOptions options;
    options.mode = PlaylistMode::Explicit;
    options.roomId = "room:snes";
    options.explicitGames = {"g00000000000ffff1", "g000000000000a005", "g000000000000a009", "g00000000000ffff2",
                             "g00000000000ffff1"};
    const Playlist playlist(roomGames(), options);
    CHECK(playlist.order() == Ids{"g000000000000a005"});
    CHECK(playlist.unknownIds() == Ids{"g00000000000ffff1", "g00000000000ffff2"});
    CHECK(playlist.unplayableIds() == Ids{"g000000000000a009"});

    // None left: shuffle.
    options.explicitGames = {"g00000000000ffff1", "g000000000000a009"};
    const Playlist none(roomGames(), options);
    CHECK(none.mode() == PlaylistMode::Shuffle);
    CHECK(none.size() == 5);
    CHECK(none.unknownIds() == Ids{"g00000000000ffff1"});
    CHECK(none.unplayableIds() == Ids{"g000000000000a009"});
    // The other modes read no ids.
    options.mode = PlaylistMode::Shuffle;
    CHECK(Playlist(roomGames(), options).unknownIds().empty());
}

TEST_CASE("app/playlist: favorites and recent fall back to shuffle without their games", "[app][playlist]") {
    std::vector<ScreenGame> games = roomGames();
    for (ScreenGame& g : games) {
        if (g.media.video.empty()) {
            continue; // the game without a clip keeps its flag and play: it never counts
        }
        g.favorite = false;
        g.lastPlayed.reset();
    }
    PlaylistOptions options;
    options.roomId = "room:snes";
    PlaylistOptions shuffle = options;
    for (const PlaylistMode mode : {PlaylistMode::Favorites, PlaylistMode::Recent}) {
        options.mode = mode;
        INFO(rv::app::toString(mode));
        CHECK(rv::app::effectivePlaylistMode(games, options) == PlaylistMode::Shuffle);
        // The shuffle's own order (its seeds name the mode played).
        CHECK(rv::app::playlistCycle(games, options, 0) == rv::app::playlistCycle(games, shuffle, 0));
        CHECK(Playlist(games, options).mode() == PlaylistMode::Shuffle);
    }
}

TEST_CASE("app/playlist: recent keeps the ten newest", "[app][playlist]") {
    std::vector<ScreenGame> games;
    for (int i = 0; i < 12; ++i) {
        ScreenGame g = game("g00000000000000" + std::string(i < 10 ? "0" : "") + std::to_string(i));
        g.lastPlayed = 1000 + i;
        games.push_back(std::move(g));
    }
    PlaylistOptions options;
    options.mode = PlaylistMode::Recent;
    const Ids order = rv::app::playlistCycle(games, options, 0);
    REQUIRE(order.size() == rv::app::kRecentPlaylistGames);
    CHECK(order.front() == "g0000000000000011");
    CHECK(order.back() == "g0000000000000002");
}

TEST_CASE("app/playlist: no game repeats within a cycle, nor across a cycle's boundary", "[app][playlist]") {
    std::vector<ScreenGame> games;
    for (int i = 0; i < 7; ++i) {
        games.push_back(game("g00000000000000" + std::to_string(i) + "0"));
    }
    games.push_back(game("g0000000000000099", false)); // no clip: never played
    std::set<std::string> all;
    for (const ScreenGame& g : games) {
        if (!g.media.video.empty()) {
            all.insert(g.id);
        }
    }
    for (const char* room : {"room:a", "room:b", "room:c", "room:d", "room:e", "room:f", "area:arcade"}) {
        for (const PlaylistMode mode : {PlaylistMode::Shuffle, PlaylistMode::Sequential}) {
            PlaylistOptions options;
            options.mode = mode;
            options.roomId = room;
            Playlist playlist(games, options);
            std::optional<std::string> previous;
            for (int cycle = 0; cycle < 6; ++cycle) {
                INFO(room << " " << rv::app::toString(mode) << " cycle " << cycle);
                const Ids order = take(playlist, all.size());
                CHECK(std::set<std::string>(order.begin(), order.end()) == all);
                if (previous && mode == PlaylistMode::Shuffle) {
                    CHECK(order.front() != *previous);
                }
                previous = order.back();
            }
        }
    }
}

TEST_CASE("app/playlist: a game excluded (its clip failed) is skipped from then on", "[app][playlist]") {
    PlaylistOptions options;
    options.mode = PlaylistMode::Sequential;
    Playlist playlist(roomGames(), options);
    CHECK(take(playlist, 1) == Ids{"g000000000000a002"});
    playlist.exclude("g000000000000a005"); // ahead in this cycle
    CHECK(take(playlist, 5) ==
          Ids{"g000000000000a004", "g000000000000a001", "g000000000000a003", "g000000000000a002", "g000000000000a004"});
    CHECK(playlist.order().size() == 4);
    CHECK(playlist.excluded().contains("g000000000000a005"));
    // Excluded from the start (a clip that failed before the room was bound again).
    const Playlist again(roomGames(), options, {"g000000000000a002"});
    CHECK(again.order() == Ids{"g000000000000a004", "g000000000000a005", "g000000000000a001", "g000000000000a003"});
    // Every game excluded: nothing to play.
    Playlist empty(roomGames(), options);
    for (const char* id :
         {"g000000000000a001", "g000000000000a002", "g000000000000a003", "g000000000000a004", "g000000000000a005"}) {
        empty.exclude(id);
    }
    CHECK_FALSE(empty.next().has_value());
    CHECK(Playlist({}, options).empty());
    CHECK_FALSE(Playlist({}, options).current().has_value());
}
