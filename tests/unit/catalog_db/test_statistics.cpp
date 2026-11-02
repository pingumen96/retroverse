// Statistics and the game's status (I7 step 6, ADR-0018 §9): the hours by outcome, per system and status, the derived
// status, what the store refuses, ES-DE's counts left out, an empty catalog, a game the library lost; the cost on a
// generated library (the p95 of the plan's row; the 40,000 games of the reference library's size, by hand).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/game_status.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog/statistics.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/time.hpp>

#include "catalog_db_test_support.hpp"
#include "support/temp_dir.hpp"

using rv::ErrorCode;
using rv::catalog::CatalogEntry;
using rv::catalog::ChoiceSource;
using rv::catalog::GameId;
using rv::catalog::GameStatus;
using rv::catalog::LibraryProfile;
using rv::catalog::PlaySession;
using rv::catalog::PlaySessionOutcome;
using rv::catalog::PlayStatistics;
using rv::catalog::PlayStatisticsQuery;
using rv::catalog_db::Connection;
using rv::catalog_db::Database;
using rv::catalog_db::SessionStore;
using rv::catalog_db::SqliteCatalogQuery;
using rv::catalog_db::SqliteCatalogStore;
using rv::launcher::LaunchChoice;
using rv::launcher::LaunchChoiceKind;

namespace {

std::chrono::system_clock::time_point at(std::string_view iso) {
    return rv::core::parseUtcIso8601(iso).value();
}

GameId id(std::uint64_t value) {
    return GameId::fromValue(value).value();
}

const LaunchChoice kFake{LaunchChoiceKind::EsdeCommand, "Fake Emulator"};

/// A catalog with the library "esde", `games` games of gb and `others` of snes (ids from `games` + 1).
struct StatsFixture {
    explicit StatsFixture(std::string_view prefix, std::uint64_t games = 4, std::uint64_t others = 0)
        : dir(prefix), database(rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir)).value()),
          catalogStore(SqliteCatalogStore::open(database).value()) {
        const LibraryProfile library = rv::test::testLibrary();
        auto transaction = catalogStore->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putLibrary(library).has_value());
        for (std::uint64_t value = 1; value <= games + others; ++value) {
            CatalogEntry entry = rv::test::bareEntry(value <= games ? "gb" : "snes", fmt::format("Game {}.gb", value));
            entry.title = fmt::format("Game {}", value);
            REQUIRE((*transaction)->putIdentity(rv::test::identityOf(id(value), library.id, entry)).has_value());
            REQUIRE((*transaction)->putEntry(rv::test::indexedEntry(id(value), entry, false)).has_value());
        }
        REQUIRE((*transaction)->commit().has_value());
        store = SessionStore::open(database.writer()).value();
        auto reader = database.openReader();
        REQUIRE(reader.has_value());
        query = SqliteCatalogQuery::open(std::move(*reader)).value();
    }

    /// A finished session of `game`: `minutes` long from noon, with `outcome`.
    void played(std::uint64_t game, PlaySessionOutcome outcome, int minutes, bool test = false) {
        PlaySession session;
        session.game = id(game);
        session.choice = kFake;
        session.choiceSource = ChoiceSource::FirstCommand;
        session.test = test;
        session.started = at("2026-10-17T12:00:00.000Z");
        session.ended = session.started + std::chrono::minutes(minutes);
        session.outcome = outcome;
        session.owner = rv::catalog::SessionOwner{4242, 133'000'000'000'000'000ull};
        if (outcome == PlaySessionOutcome::Crashed) {
            session.exitCode = 3221225477u;
            session.exitKind = rv::launcher::ExitKind::Crash;
        } else if (outcome == PlaySessionOutcome::LeftRunning) {
            session.leftRunning = rv::catalog::LeftRunningReason::Quit;
        } else if (outcome == PlaySessionOutcome::Blocked) {
            session.readiness = rv::catalog::LaunchReadiness::RomMissing;
        } else if (outcome == PlaySessionOutcome::Running || outcome == PlaySessionOutcome::Interrupted) {
            session.ended.reset();
        }
        if (outcome == PlaySessionOutcome::Interrupted) {
            session.closed = session.started + std::chrono::hours(1);
        }
        REQUIRE(store->insertSession(session).has_value());
    }

    [[nodiscard]] PlayStatistics statistics(std::size_t limit = 10) {
        PlayStatisticsQuery options;
        options.limit = limit;
        options.localToUtc = rv::catalog::fixedOffsetLocalTimeToUtc(std::chrono::minutes(60));
        return query->playStatistics(options).value();
    }

    rv::test::TempDir dir;
    Database database;
    std::unique_ptr<SqliteCatalogStore> catalogStore;
    std::unique_ptr<SessionStore> store;
    std::unique_ptr<SqliteCatalogQuery> query;
};

} // namespace

TEST_CASE("catalog/status: a game with no row derives its status, and a row wins", "[catalog][statistics]") {
    using rv::catalog::derivedGameStatus;
    using rv::catalog::gameStatusOf;
    using rv::catalog::steppedGameStatus;
    CHECK(derivedGameStatus(false, false) == GameStatus::NotStarted);
    CHECK(derivedGameStatus(false, true) == GameStatus::InProgress);
    CHECK(derivedGameStatus(true, false) == GameStatus::Completed);
    CHECK(derivedGameStatus(true, true) == GameStatus::Completed);
    CHECK(gameStatusOf(GameStatus::Abandoned, true, true) == GameStatus::Abandoned);
    CHECK(gameStatusOf(std::nullopt, false, true) == GameStatus::InProgress);
    CHECK(steppedGameStatus(GameStatus::NotStarted) == GameStatus::InProgress);
    CHECK(steppedGameStatus(GameStatus::Abandoned) == GameStatus::NotStarted);
    CHECK(steppedGameStatus(GameStatus::NotStarted, false) == GameStatus::Abandoned);
    for (const GameStatus status : {GameStatus::NotStarted, GameStatus::InProgress, GameStatus::Completed,
                                    GameStatus::Abandoned}) {
        CHECK(rv::catalog::parseGameStatus(rv::catalog::toString(status)) == status);
    }
    CHECK_FALSE(rv::catalog::parseGameStatus("played"));
}

TEST_CASE("catalog_db/statistics: only Returned sessions make hours; tests never count; crashes and left-running are "
          "attempts",
          "[catalog_db][statistics]") {
    StatsFixture f("rv-stats-outcomes");
    f.played(1, PlaySessionOutcome::Returned, 60);
    f.played(1, PlaySessionOutcome::Returned, 30);
    f.played(1, PlaySessionOutcome::Returned, 600, true); // a launch test
    f.played(2, PlaySessionOutcome::Crashed, 90);
    f.played(2, PlaySessionOutcome::LeftRunning, 90);
    f.played(2, PlaySessionOutcome::Crashed, 90, true); // a test's crash is no attempt
    f.played(3, PlaySessionOutcome::FailedToStart, 1);
    f.played(3, PlaySessionOutcome::Blocked, 0);
    f.played(3, PlaySessionOutcome::Interrupted, 0);
    f.played(3, PlaySessionOutcome::Running, 0);

    const PlayStatistics statistics = f.statistics();
    CHECK(statistics.sessions == 2);
    CHECK(statistics.played == std::chrono::minutes(90));
    CHECK(statistics.gamesPlayed == 1);
    CHECK(statistics.crashed == 1);
    CHECK(statistics.leftRunning == 1);
    CHECK(statistics.attempts() == 2);
    CHECK(statistics.games == 4);
    REQUIRE(statistics.mostPlayed.size() == 1);
    CHECK(statistics.mostPlayed[0].id == id(1));
    CHECK(statistics.mostPlayed[0].title == "Game 1");
    CHECK(statistics.mostPlayed[0].sessions == 2);
    CHECK(statistics.mostPlayed[0].played == std::chrono::minutes(90));
    REQUIRE(statistics.systems.size() == 1);
    CHECK(statistics.systems[0].sessions == 2);
    CHECK(statistics.systems[0].attempts == 2);
    CHECK(statistics.systems[0].games == 1);
}

TEST_CASE("catalog_db/statistics: a clock moved back counts for nothing, never for less", "[catalog_db][statistics]") {
    StatsFixture f("rv-stats-clock");
    f.played(1, PlaySessionOutcome::Returned, 60);
    f.played(1, PlaySessionOutcome::Returned, -30);
    const PlayStatistics statistics = f.statistics();
    CHECK(statistics.sessions == 2);
    CHECK(statistics.played == std::chrono::minutes(60));
}

TEST_CASE("catalog_db/statistics: an empty catalog, and one where nothing was played", "[catalog_db][statistics]") {
    StatsFixture none("rv-stats-none", 0);
    const PlayStatistics empty = none.statistics();
    CHECK(empty.empty());
    CHECK(empty.games == 0);
    CHECK(empty.statuses.size() == 4);

    StatsFixture unplayed("rv-stats-unplayed");
    const PlayStatistics statistics = unplayed.statistics();
    CHECK(statistics.empty());
    CHECK(statistics.games == 4);
    CHECK(statistics.statuses[0].status == GameStatus::NotStarted);
    CHECK(statistics.statuses[0].games == 4);
    CHECK(statistics.mostPlayed.empty());
    CHECK(statistics.systems.empty());

    // The query needs its converter, as the directory's.
    CHECK(unplayed.query->playStatistics(PlayStatisticsQuery{}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("catalog_db/statistics: per system, per status, the derived status and ES-DE's counts",
          "[catalog_db][statistics]") {
    StatsFixture f("rv-stats-groups", 3, 2);
    Connection& writer = f.database.writer();
    // ES-DE's numbers for game 4: its <completed>, a large <playcount> and <playtime> that must not show.
    REQUIRE(writer.execute("UPDATE idx_games SET completed = 1, play_count = 99, play_time = 360000 WHERE id = 4")
                .has_value());
    // A hidden game is no game of the library's lists.
    REQUIRE(writer.execute("UPDATE idx_games SET hidden = 1 WHERE id = 3").has_value());
    f.played(1, PlaySessionOutcome::Returned, 120);
    f.played(2, PlaySessionOutcome::Returned, 30);
    f.played(5, PlaySessionOutcome::Returned, 240);
    REQUIRE(f.store->setGameStatus(id(2), GameStatus::Abandoned, at("2026-10-17T13:00:00Z")).has_value());

    const PlayStatistics statistics = f.statistics();
    CHECK(statistics.sessions == 3);
    CHECK(statistics.played == std::chrono::minutes(390));
    CHECK(statistics.games == 4); // the hidden game out
    REQUIRE(statistics.systems.size() == 2);
    CHECK(statistics.systems[0].system.str() == "snes"); // by time played
    CHECK(statistics.systems[0].played == std::chrono::minutes(240));
    CHECK(statistics.systems[1].system.str() == "gb");
    CHECK(statistics.systems[1].played == std::chrono::minutes(150));
    REQUIRE(statistics.mostPlayed.size() == 3);
    CHECK(statistics.mostPlayed[0].id == id(5));
    CHECK(statistics.mostPlayed[1].id == id(1));
    CHECK(statistics.mostPlayed[2].id == id(2));

    const auto status = [&](GameStatus wanted) {
        return statistics.statuses[static_cast<std::size_t>(wanted)];
    };
    CHECK(status(GameStatus::NotStarted).games == 0);
    CHECK(status(GameStatus::InProgress).games == 2); // 1 and 5: a Returned session
    CHECK(status(GameStatus::InProgress).played == std::chrono::minutes(360));
    CHECK(status(GameStatus::Completed).games == 1); // 4: ES-DE's <completed>, its hours none of ours
    CHECK(status(GameStatus::Completed).played == std::chrono::minutes(0));
    CHECK(status(GameStatus::Completed).sessions == 0);
    CHECK(status(GameStatus::Abandoned).games == 1); // 2: set, over its session
    CHECK(status(GameStatus::Abandoned).sessions == 1);
}

TEST_CASE("catalog_db/statistics: the recent games take ES-DE's <lastplayed> as it is, favourites not first",
          "[catalog_db][statistics]") {
    StatsFixture f("rv-stats-recent");
    Connection& writer = f.database.writer();
    REQUIRE(writer.execute("UPDATE idx_games SET last_played = '20261018T100000' WHERE id = 3").has_value());
    REQUIRE(writer.execute("UPDATE idx_games SET favorite = 1 WHERE id = 4").has_value());
    REQUIRE(writer.execute("UPDATE idx_games SET last_played = '20200101T100000' WHERE id = 4").has_value());
    f.played(1, PlaySessionOutcome::Returned, 10);

    const PlayStatistics statistics = f.statistics();
    REQUIRE(statistics.mostRecent.size() == 3);
    CHECK(statistics.mostRecent[0].id == id(3)); // ES-DE's, the latest: UTC+1, 09:00
    CHECK(statistics.mostRecent[0].lastPlayed == at("2026-10-18T09:00:00Z"));
    CHECK(statistics.mostRecent[0].sessions == 0);
    CHECK(statistics.mostRecent[1].id == id(1));
    CHECK(statistics.mostRecent[2].id == id(4)); // a favourite, not first
    CHECK_FALSE(statistics.empty());             // a game played in ES-DE shows, with no session of ours
    CHECK(statistics.sessions == 1);

    // A limit keeps the head of each list.
    const PlayStatistics one = f.statistics(1);
    CHECK(one.mostRecent.size() == 1);
    CHECK(one.mostPlayed.size() == 1);
}

TEST_CASE("catalog_db/statistics: a game the library lost keeps its hours, out of the statuses and the recent games",
          "[catalog_db][statistics]") {
    StatsFixture f("rv-stats-lost");
    f.played(1, PlaySessionOutcome::Returned, 60);
    Connection& writer = f.database.writer();
    REQUIRE(writer.execute("DELETE FROM idx_media WHERE game = 1; DELETE FROM idx_games WHERE id = 1").has_value());
    const PlayStatistics statistics = f.statistics();
    CHECK(statistics.played == std::chrono::minutes(60));
    CHECK(statistics.games == 3);
    REQUIRE(statistics.mostPlayed.size() == 1);
    CHECK(statistics.mostPlayed[0].title == "Game 1.gb"); // its path, for a title
    CHECK(statistics.mostRecent.empty());
    CHECK(statistics.statuses[1].games == 0);
}

TEST_CASE("catalog_db/statistics: a status is stored for an identity, read through an alias, refused for a stranger",
          "[catalog_db][statistics][sessions]") {
    StatsFixture f("rv-status-store");
    SessionStore& store = *f.store;
    CHECK_FALSE(store.gameStatus(id(1)).value().has_value());
    CHECK(f.query->gameStatus(id(1)).value()->status == GameStatus::NotStarted);
    CHECK_FALSE(f.query->gameStatus(id(1)).value()->stored);
    REQUIRE(store.setGameStatus(id(1), GameStatus::Completed, at("2026-10-17T13:00:00Z")).has_value());
    CHECK(store.gameStatus(id(1)).value() == GameStatus::Completed);
    REQUIRE(store.setGameStatus(id(1), GameStatus::Abandoned, at("2026-10-17T14:00:00Z")).has_value());
    CHECK(store.gameStatus(id(1)).value() == GameStatus::Abandoned);
    CHECK(f.query->gameStatus(id(1)).value()->status == GameStatus::Abandoned);
    CHECK(f.query->gameStatus(id(1)).value()->stored);
    CHECK(store.setGameStatus(id(77), GameStatus::Completed, at("2026-10-17T13:00:00Z")).error().code ==
          ErrorCode::NotFound);
    CHECK_FALSE(f.query->gameStatus(id(77)).value().has_value());
    f.played(2, PlaySessionOutcome::Returned, 5);
    f.played(3, PlaySessionOutcome::Returned, 5, true);
    CHECK(f.query->gameStatus(id(2)).value()->status == GameStatus::InProgress);
    CHECK(f.query->gameStatus(id(3)).value()->status == GameStatus::NotStarted); // a test starts nothing
}

TEST_CASE("catalog_db/statistics: the read's cost on 10,000 sessions (p95 at most 100 ms)", "[catalog_db][statistics]") {
    constexpr std::uint64_t kGames = 2000;
    StatsFixture f("rv-stats-cost", kGames);
    Connection& writer = f.database.writer();
    auto session = rv::catalog_db::Transaction::begin(writer, rv::catalog_db::Transaction::Mode::Immediate);
    REQUIRE(session.has_value());
    REQUIRE(writer
                .execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 10000) "
                         "INSERT INTO rv_play_sessions (game, choice_kind, choice, choice_source, test, started_at, "
                         "ended_at, run_ms, outcome, left_running, failure, quick_exit, confirmed_by_user, "
                         "processes_remained, refused_by_guard, message, process_id, owner_pid, owner_created) "
                         "SELECT 1 + i % 2000, 'command', 'Fake', 'first-command', 0, '2026-10-17T12:00:00.000Z', "
                         "'2026-10-17T12:30:00.000Z', 1800000, 'returned', 'none', 'none', 0, 0, 0, 0, '', 1, 1, 1 "
                         "FROM n")
                .has_value());
    REQUIRE(session->commit().has_value());
    std::vector<double> times;
    for (int run = 0; run < 20; ++run) {
        const auto start = std::chrono::steady_clock::now();
        const PlayStatistics statistics = f.statistics();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        CHECK(statistics.sessions == 10000);
        CHECK(statistics.played == std::chrono::minutes(5000 * 60));
    }
    std::sort(times.begin(), times.end());
    const double p95 = times[times.size() * 95 / 100];
    WARN("the statistics of 10,000 sessions over " << kGames << " games: p95 " << p95 << " ms");
    CHECK(p95 <= 100.0);
}

TEST_CASE("catalog_db/statistics: 40,000 games, 10,000 sessions", "[.bench][statistics_bench]") {
    constexpr std::uint64_t kGames = 40000;
    StatsFixture f("rv-stats-bench", kGames);
    Connection& writer = f.database.writer();
    REQUIRE(writer
                .execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 10000) "
                         "INSERT INTO rv_play_sessions (game, choice_kind, choice, choice_source, test, started_at, "
                         "ended_at, run_ms, outcome, left_running, failure, quick_exit, confirmed_by_user, "
                         "processes_remained, refused_by_guard, message, process_id, owner_pid, owner_created) "
                         "SELECT 1 + i * 7 % 40000, 'command', 'Fake', 'first-command', 0, '2026-10-17T12:00:00.000Z', "
                         "'2026-10-17T12:30:00.000Z', 1800000, 'returned', 'none', 'none', 0, 0, 0, 0, '', 1, 1, 1 "
                         "FROM n; UPDATE idx_games SET last_played = '20261018T100000' WHERE id % 3 = 0")
                .has_value());
    std::vector<double> times;
    for (int run = 0; run < 20; ++run) {
        const auto start = std::chrono::steady_clock::now();
        const PlayStatistics statistics = f.statistics();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        CHECK(statistics.games == kGames);
    }
    std::sort(times.begin(), times.end());
    WARN("the statistics of 10,000 sessions over " << kGames << " games: median " << times[10] << " ms, p95 "
                                                   << times[times.size() * 95 / 100] << " ms");
}
