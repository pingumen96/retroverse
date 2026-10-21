// I4 step 7a: the cost of the directory's favourites and recent games (SqliteCatalogQuery::favoritesAndRecent) on
// 100,000 games, hidden ([.bench]) and informative, meant for Release builds:
//   build/msvc-x64-release/bin/rv_tests.exe "[recent_query][bench]"
// A generated catalog (tests/support/catalog_generator.hpp: every state, about 5% favourites) is given ES-DE
// <lastplayed> times on one game in 20 (written into idx_games directly, as an import of played games would) and
// Retroverse sessions on one game in 50, then read on a reader as the catalog worker reads it with the machine's time
// zone: once to warm the page cache, then 20 times; the median and the slowest are printed (docs/i4/step-7a.md records
// them). No budget is checked: the query runs on the catalog worker when the directory's tab opens.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/play_session.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>

#include "catalog_db_test_support.hpp"
#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using Clock = std::chrono::steady_clock;

namespace {

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

TEST_CASE("catalog_db/recent_query: the favourites and recent games of 100,000 games (informative)",
          "[.bench][bench][recent_query]") {
    rv::test::TempDir dir("rv-recent-query-bench");
    rv::test::CatalogGeneratorOptions options;
    options.entries = 100'000;
    auto generated = rv::test::generateCatalogDatabase(rv::test::databaseFileIn(dir), options, false);
    REQUIRE(generated.has_value());
    auto database = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());

    // ES-DE's <lastplayed> on one game in 20, spread over a year of local times.
    rv::catalog_db::Connection& writer = database->writer();
    REQUIRE(writer
                .execute("UPDATE idx_games SET last_played = printf('2026%02d%02dT%02d%02d00', 1 + (id / 7) % 12, "
                         "1 + (id / 3) % 28, (id / 11) % 24, (id / 13) % 60) WHERE id % 20 = 0")
                .has_value());
    // Retroverse's sessions on one game in 50.
    std::vector<GameId> played;
    {
        auto ids = writer.prepare("SELECT id FROM idx_games WHERE id % 50 = 1 ORDER BY id");
        REQUIRE(ids.has_value());
        const rv::catalog_db::ScopedReset reset(*ids);
        while (true) {
            const auto row = ids->step();
            REQUIRE(row.has_value());
            if (!*row) {
                break;
            }
            played.push_back(GameId::fromValue(static_cast<std::uint64_t>(ids->columnInt(0))).value());
        }
    }
    {
        auto sessions = rv::catalog_db::SessionStore::open(writer);
        REQUIRE(sessions.has_value());
        const auto start = std::chrono::system_clock::time_point(std::chrono::sys_days{std::chrono::year{2026} / 1 / 1});
        for (std::size_t index = 0; index < played.size(); ++index) {
            PlaySession session;
            session.game = played[index];
            session.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::EsdeCommand, "Fake Emulator"};
            session.started = start + std::chrono::minutes(37 * index);
            session.ended = session.started + std::chrono::minutes(30);
            session.outcome = PlaySessionOutcome::Returned;
            session.owner = SessionOwner{4242, 133'000'000'000'000'000ull};
            REQUIRE((*sessions)->insertSession(session).has_value());
        }
    }

    auto query = rv::catalog_db::SqliteCatalogQuery::open(database->openReader().value());
    REQUIRE(query.has_value());
    auto machine = machineLocalTimeToUtc();
    RecentGamesQuery request;
    request.localToUtc = machine ? *machine : fixedOffsetLocalTimeToUtc(std::chrono::minutes(0));
    REQUIRE((*query)->favoritesAndRecent(request).has_value()); // warms the page cache, as the app's reader is
    std::vector<double> reads;
    std::size_t favourites = 0;
    std::size_t count = 0;
    for (int run = 0; run < 20; ++run) {
        // A new converter each time, as the worker would start cold on a new tab.
        request.localToUtc = machine ? machineLocalTimeToUtc().value() : request.localToUtc;
        const Clock::time_point start = Clock::now();
        auto games = (*query)->favoritesAndRecent(request);
        reads.push_back(millisecondsSince(start));
        REQUIRE(games.has_value());
        count = games->size();
        favourites = static_cast<std::size_t>(
            std::count_if(games->begin(), games->end(), [](const RecentGame& game) { return game.favorite; }));
    }
    std::cout << fmt::format("Favourites and recent games of 100,000 entries ({} sessions, {} zone): {} listed, {} "
                             "favourites; median {:.2f} ms, slowest {:.2f} ms\n",
                             played.size(), machine ? "the machine's" : "a UTC", count, favourites, median(reads),
                             *std::max_element(reads.begin(), reads.end()));
    CHECK(count == request.limit);
}
