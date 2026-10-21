// I4 step 7a: SqliteCatalogQuery::favoritesAndRecent, the directory's favourites and recently played games
// (rv/catalog/recent_games.hpp's rule) on the catalog database: favourites first, then the games played, across
// systems with their full names; ES-DE's local <lastplayed> converted to UTC before it is compared with Retroverse's
// sessions; the sessions that count (a blocked attempt, a failure to start and a launch test do not); the entries left
// out (hidden, not a game, a missing file, a disc member, a folder, an unavailable game unless asked); ties by sort key
// then GameId; the limit; a generated catalog against an oracle. The benchmark on 100,000 games is in
// test_recent_query_bench.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/time.hpp>

#include "catalog_db_test_support.hpp"
#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using namespace std::chrono_literals;
using rv::catalog_db::SqliteCatalogQuery;
using rv::catalog_db::SqliteCatalogStore;

/// GameIds in failure messages by their text form.
template <>
struct Catch::StringMaker<rv::catalog::GameId> {
    static std::string convert(rv::catalog::GameId id) { return id.toString(); }
};

namespace {

UtcTime utc(std::string_view text) {
    return rv::core::parseUtcIso8601(text).value();
}

/// A game of `system`: a listed file of `relativePath` titled `title` by its gamelist entry, nothing else set.
IndexedEntry game(const LibraryId& library, std::string_view system, std::string_view relativePath,
                  std::string_view title, const std::function<void(CatalogEntry&)>& edit = {}) {
    CatalogEntry entry = rv::test::bareEntry(system, relativePath);
    entry.gamelistPath = "./" + std::string(relativePath);
    entry.title = std::string(title);
    entry.metadata.name = std::string(title);
    if (edit) {
        edit(entry);
    }
    const GameId id = rv::test::gameIdOf(library, entry);
    return rv::test::indexedEntry(id, std::move(entry), false);
}

SystemInfo system(std::string_view name, std::string_view fullName) {
    SystemInfo info = rv::test::testSystem(name, std::filesystem::path(L"E:\\roms") / std::string(name));
    info.fullName = std::string(fullName);
    return info;
}

/// A session of `game` that started at `started` and ended with `outcome` (the room query test's).
PlaySession sessionOf(GameId game, std::string_view started, PlaySessionOutcome outcome, bool test = false) {
    PlaySession session;
    session.game = game;
    session.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::EsdeCommand, "Fake Emulator"};
    session.choiceSource = ChoiceSource::FirstCommand;
    session.test = test;
    session.started = utc(started);
    session.outcome = outcome;
    session.owner = SessionOwner{4242, 133'000'000'000'000'000ull};
    if (outcome != PlaySessionOutcome::Running) {
        session.ended = session.started + 30min;
    }
    if (outcome == PlaySessionOutcome::Blocked) {
        session.readiness = LaunchReadiness::EmulatorNotFound;
    }
    if (outcome == PlaySessionOutcome::FailedToStart) {
        session.failure = rv::launcher::FailureReason::StartFailed;
    }
    return session;
}

/// A migrated database in a test's folder, its store on the writer and its queries on a reader.
struct RecentFixture {
    explicit RecentFixture(std::string_view prefix)
        : dir(prefix), database(rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir)).value()),
          store(SqliteCatalogStore::open(database).value()),
          query(SqliteCatalogQuery::open(database.openReader().value()).value()) {}

    void import(const std::vector<SystemInfo>& systems, const std::vector<IndexedEntry>& entries) {
        auto transaction = store->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putLibrary(library).has_value());
        for (const SystemInfo& info : systems) {
            REQUIRE((*transaction)->putSystem(library.id, info).has_value());
        }
        for (const IndexedEntry& entry : entries) {
            REQUIRE((*transaction)->putIdentity(rv::test::identityOf(entry.id, library.id, entry.entry)).has_value());
            REQUIRE((*transaction)->putEntry(entry).has_value());
        }
        REQUIRE((*transaction)->commit().has_value());
    }

    void record(const std::vector<PlaySession>& sessions) {
        auto sessionStore = rv::catalog_db::SessionStore::open(database.writer());
        REQUIRE(sessionStore.has_value());
        for (const PlaySession& session : sessions) {
            REQUIRE((*sessionStore)->insertSession(session).has_value());
        }
    }

    std::vector<RecentGame> recent(std::size_t limit = kDefaultRecentGames,
                                   LocalTimeToUtc localToUtc = fixedOffsetLocalTimeToUtc(120min),
                                   bool includeUnavailable = false) {
        RecentGamesQuery request;
        request.limit = limit;
        request.localToUtc = std::move(localToUtc);
        request.includeUnavailable = includeUnavailable;
        auto games = query->favoritesAndRecent(request);
        REQUIRE(games.has_value());
        return *games;
    }

    rv::test::TempDir dir;
    LibraryProfile library = rv::test::testLibrary();
    rv::catalog_db::Database database;
    std::unique_ptr<SqliteCatalogStore> store;
    std::unique_ptr<SqliteCatalogQuery> query;
};

std::vector<GameId> idsOf(const std::vector<RecentGame>& games) {
    std::vector<GameId> ids;
    for (const RecentGame& recent : games) {
        ids.push_back(recent.id);
    }
    return ids;
}

void favourite(CatalogEntry& entry) {
    entry.flags.favorite = true;
}

std::function<void(CatalogEntry&)> playedAt(LocalDateTime time, bool favorite = false) {
    return [time, favorite](CatalogEntry& entry) {
        entry.flags.lastPlayed = time;
        entry.flags.favorite = favorite;
    };
}

} // namespace

TEST_CASE("catalog_db/recent_query: favourites first, then the games played, most recent first, across systems",
          "[catalog_db][recent_query]") {
    RecentFixture fixture("rv-recent-query");
    const LibraryId& library = fixture.library.id;
    // At UTC+2: ES-DE's 20:00 of 1 October is 18:00 UTC.
    const IndexedEntry favouriteNever = game(library, "gb", "Amber Lantern.gb", "Amber Lantern", favourite);
    const IndexedEntry favouritePlayed =
        game(library, "snes", "Bramble Burrow.sfc", "Bramble Burrow", playedAt({2026, 10, 1, 20, 0, 0}, true));
    const IndexedEntry session = game(library, "snes", "Cobalt Comet.sfc", "Cobalt Comet");
    const IndexedEntry esde = game(library, "gb", "Dusk Harbor.gb", "Dusk Harbor", playedAt({2026, 10, 3, 8, 0, 0}));
    const IndexedEntry both = game(library, "gb", "Ember Vale.gb", "Ember Vale", playedAt({2026, 9, 1, 12, 0, 0}));
    const IndexedEntry untouched = game(library, "snes", "Fern Hollow.sfc", "Fern Hollow");
    fixture.import({system("gb", "Pocket Lumen"), system("snes", "Lumen 16 Home System")},
                   {favouriteNever, favouritePlayed, session, esde, both, untouched});
    fixture.record({
        sessionOf(session.id, "2026-10-02T10:00:00.000Z", PlaySessionOutcome::Returned),
        sessionOf(both.id, "2026-10-04T09:00:00.000Z", PlaySessionOutcome::Returned),
        sessionOf(both.id, "2026-09-02T09:00:00.000Z", PlaySessionOutcome::Returned),
    });

    const std::vector<RecentGame> games = fixture.recent();
    CHECK(idsOf(games) == std::vector{favouritePlayed.id, favouriteNever.id, both.id, esde.id, session.id});
    REQUIRE(games.size() == 5);
    const RecentGame& first = games[0];
    CHECK(first.system == SystemName::parse("snes").value());
    CHECK(first.systemFullName == "Lumen 16 Home System");
    CHECK(first.title == "Bramble Burrow");
    CHECK(first.sortKey == favouritePlayed.sortKey);
    CHECK(first.favorite);
    CHECK(first.esdeLastPlayed == LocalDateTime{2026, 10, 1, 20, 0, 0});
    CHECK_FALSE(first.lastSession.has_value());
    CHECK(first.lastPlayed == utc("2026-10-01T18:00:00Z"));
    CHECK(games[1].systemFullName == "Pocket Lumen");
    CHECK_FALSE(games[1].lastPlayed.has_value());
    // The later of the last session's start and <lastplayed>.
    CHECK(games[2].lastSession == utc("2026-10-04T09:00:00Z"));
    CHECK(games[2].lastPlayed == utc("2026-10-04T09:00:00Z"));
    CHECK(games[3].lastPlayed == utc("2026-10-03T06:00:00Z"));
    CHECK(games[4].lastSession == utc("2026-10-02T10:00:00Z"));
    CHECK_FALSE(games[4].favorite);

    // The limit cuts the same order.
    CHECK(idsOf(fixture.recent(2)) == std::vector{favouritePlayed.id, favouriteNever.id});
    CHECK(fixture.recent(0).empty());
    // A query without its converter is refused.
    RecentGamesQuery request;
    request.localToUtc = {};
    const auto refused = fixture.query->favoritesAndRecent(request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("catalog_db/recent_query: ES-DE's local time is compared with the sessions in UTC",
          "[catalog_db][recent_query]") {
    RecentFixture fixture("rv-recent-query-utc");
    const LibraryId& library = fixture.library.id;
    // A session at 23:45 UTC on the 4th; ES-DE's <lastplayed> of 01:30 on the 5th.
    const IndexedEntry session = game(library, "snes", "Glass Garden.sfc", "Glass Garden");
    const IndexedEntry esde = game(library, "snes", "Harbor Lights.sfc", "Harbor Lights", playedAt({2026, 10, 5, 1, 30, 0}));
    fixture.import({system("snes", "Lumen 16 Home System")}, {session, esde});
    fixture.record({sessionOf(session.id, "2026-10-04T23:45:00.000Z", PlaySessionOutcome::Returned)});
    // At UTC+2, 01:30 is 23:30 UTC: the session is later.
    CHECK(idsOf(fixture.recent(10, fixedOffsetLocalTimeToUtc(120min))) == std::vector{session.id, esde.id});
    // At UTC, 01:30 is later than the session.
    CHECK(idsOf(fixture.recent(10, fixedOffsetLocalTimeToUtc(0min))) == std::vector{esde.id, session.id});
}

TEST_CASE("catalog_db/recent_query: the sessions that started a game count, blocked, failed and test ones do not",
          "[catalog_db][recent_query]") {
    RecentFixture fixture("rv-recent-query-sessions");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry blocked = game(library, "gb", "Indigo Isle.gb", "Indigo Isle");
    const IndexedEntry failed = game(library, "gb", "Jade Jetty.gb", "Jade Jetty");
    const IndexedEntry tested = game(library, "gb", "Kestrel Keep.gb", "Kestrel Keep");
    const IndexedEntry running = game(library, "gb", "Lark Lagoon.gb", "Lark Lagoon");
    const IndexedEntry left = game(library, "gb", "Moss Mill.gb", "Moss Mill");
    fixture.import({system("gb", "Pocket Lumen")}, {blocked, failed, tested, running, left});
    PlaySession leftRunning = sessionOf(left.id, "2026-10-06T10:00:00.000Z", PlaySessionOutcome::LeftRunning);
    leftRunning.leftRunning = LeftRunningReason::Quit;
    fixture.record({
        sessionOf(blocked.id, "2026-10-09T10:00:00.000Z", PlaySessionOutcome::Blocked),
        sessionOf(failed.id, "2026-10-09T10:00:00.000Z", PlaySessionOutcome::FailedToStart),
        sessionOf(tested.id, "2026-10-09T10:00:00.000Z", PlaySessionOutcome::Returned, true),
        sessionOf(running.id, "2026-10-07T10:00:00.000Z", PlaySessionOutcome::Running),
        // A session that failed after a later one started does not hide it.
        sessionOf(running.id, "2026-10-08T10:00:00.000Z", PlaySessionOutcome::Blocked),
        leftRunning,
    });
    const std::vector<RecentGame> games = fixture.recent();
    CHECK(idsOf(games) == std::vector{running.id, left.id});
    REQUIRE(games.size() == 2);
    CHECK(games[0].lastSession == utc("2026-10-07T10:00:00Z"));
}

TEST_CASE("catalog_db/recent_query: hidden games, entries that are not games and unavailable games are left out",
          "[catalog_db][recent_query]") {
    RecentFixture fixture("rv-recent-query-filters");
    const LibraryId& library = fixture.library.id;
    const LocalDateTime played{2026, 10, 1, 12, 0, 0};
    const IndexedEntry shown = game(library, "gb", "Pixel Quest.gb", "Pixel Quest", playedAt(played, true));
    const IndexedEntry hidden = game(library, "gb", "Hidden.gb", "Hidden Pixel", [&](CatalogEntry& e) {
        playedAt(played, true)(e);
        e.flags.hidden = true;
    });
    const IndexedEntry missing = game(library, "gb", "Gone.gb", "Gone Pixel", [&](CatalogEntry& e) {
        playedAt(played, true)(e);
        e.state = EntryState::MissingFile;
        e.availability = Availability::Missing;
        e.file.reset();
    });
    const IndexedEntry unlisted = game(library, "snes", "Away.sfc", "Away Pixel", [&](CatalogEntry& e) {
        playedAt(played, true)(e);
        e.availability = Availability::SourceUnavailable;
        e.unavailableReason = SourceUnavailableReason::Unreachable;
        e.file.reset();
    });
    const IndexedEntry notAGame = game(library, "gb", "Readme.txt", "Readme Pixel", [&](CatalogEntry& e) {
        playedAt(played, true)(e);
        e.state = EntryState::NotAGame;
        e.notAGameReason = NotAGameReason::ExtensionNotConfigured;
    });
    const IndexedEntry member = game(library, "gb", "Set/Track 2.bin", "Track Pixel", [&](CatalogEntry& e) {
        playedAt(played, true)(e);
        e.state = EntryState::DiscMember;
        e.discHead = makeLookupKey("Set/Set.cue").value();
    });
    const IndexedEntry folder = game(library, "gb", "Collection", "Collection Pixel", [&](CatalogEntry& e) {
        playedAt(played, true)(e);
        e.state = EntryState::Folder;
        e.file = FileFacts{true, 0, 0, false};
    });
    fixture.import({system("gb", "Pocket Lumen"), system("snes", "Lumen 16 Home System")},
                   {shown, hidden, missing, unlisted, notAGame, member, folder});
    fixture.record({sessionOf(hidden.id, "2026-10-09T10:00:00.000Z", PlaySessionOutcome::Returned),
                    sessionOf(notAGame.id, "2026-10-09T10:00:00.000Z", PlaySessionOutcome::Returned)});

    CHECK(idsOf(fixture.recent()) == std::vector{shown.id});
    // Asked for, a game whose folder could not be listed comes too (FR-033); a missing file never does.
    const std::vector<RecentGame> withUnavailable = fixture.recent(10, fixedOffsetLocalTimeToUtc(120min), true);
    std::vector<GameId> expected{shown.id, unlisted.id};
    std::sort(expected.begin(), expected.end(), [&](GameId a, GameId b) {
        const std::string& keyA = a == shown.id ? shown.sortKey : unlisted.sortKey;
        const std::string& keyB = b == shown.id ? shown.sortKey : unlisted.sortKey;
        return keyA < keyB;
    });
    CHECK(idsOf(withUnavailable) == expected);
}

TEST_CASE("catalog_db/recent_query: ties by sort key, then by GameId", "[catalog_db][recent_query]") {
    RecentFixture fixture("rv-recent-query-ties");
    const LibraryId& library = fixture.library.id;
    const LocalDateTime played{2026, 10, 1, 12, 0, 0};
    // Namesakes in two systems played at the same time, and another title at that time.
    const IndexedEntry gbNamesake = game(library, "gb", "Nova Run.gb", "Nova Run", playedAt(played));
    const IndexedEntry snesNamesake = game(library, "snes", "Nova Run.sfc", "Nova Run", playedAt(played));
    const IndexedEntry other = game(library, "snes", "Amber Run.sfc", "Amber Run", playedAt(played));
    fixture.import({system("gb", "Pocket Lumen"), system("snes", "Lumen 16 Home System")},
                   {gbNamesake, snesNamesake, other});
    REQUIRE(gbNamesake.sortKey == snesNamesake.sortKey);
    REQUIRE(other.sortKey < gbNamesake.sortKey);
    const GameId lower = std::min(gbNamesake.id, snesNamesake.id);
    const GameId higher = std::max(gbNamesake.id, snesNamesake.id);
    CHECK(idsOf(fixture.recent()) == std::vector{other.id, lower, higher});
}

TEST_CASE("catalog_db/recent_query: a generated catalog against an oracle", "[catalog_db][recent_query]") {
    rv::test::TempDir dir("rv-recent-query-generated");
    rv::test::CatalogGeneratorOptions options;
    options.entries = 3'000;
    auto generated = rv::test::generateCatalogDatabase(rv::test::databaseFileIn(dir), options);
    REQUIRE(generated.has_value());
    auto database = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    // Sessions for every 7th entry, a minute apart (some of them favourites, some not games: those stay out).
    std::map<GameId, UtcTime> sessions;
    {
        auto store = rv::catalog_db::SessionStore::open(database->writer());
        REQUIRE(store.has_value());
        for (std::size_t index = 0; index < generated->entries.size(); index += 7) {
            const GameId id = generated->entries[index].id;
            const UtcTime started = utc("2026-01-01T00:00:00Z") + std::chrono::minutes(index);
            PlaySession session = sessionOf(id, "2026-01-01T00:00:00.000Z", PlaySessionOutcome::Returned);
            session.started = started;
            session.ended = started + 30min;
            REQUIRE((*store)->insertSession(session).has_value());
            sessions[id] = started;
        }
    }
    std::map<SystemName, std::string> fullNames;
    for (const SystemInfo& info : generated->systems) {
        fullNames[info.name] = info.fullName;
    }
    // The oracle: the rule applied to the entries as generated (the generator writes no <lastplayed>).
    std::vector<RecentGame> expected;
    for (const IndexedEntry& indexed : generated->entries) {
        const CatalogEntry& entry = indexed.entry;
        const auto session = sessions.find(indexed.id);
        const bool played = session != sessions.end();
        if (entry.state != EntryState::Game || entry.flags.hidden || entry.availability != Availability::Available ||
            (!entry.flags.favorite && !played && !entry.flags.lastPlayed)) {
            continue;
        }
        RecentGame recent;
        recent.id = indexed.id;
        recent.system = entry.system;
        recent.systemFullName = fullNames[entry.system];
        recent.title = entry.title;
        recent.sortKey = indexed.sortKey;
        recent.favorite = entry.flags.favorite;
        recent.esdeLastPlayed = entry.flags.lastPlayed;
        if (played) {
            recent.lastSession = session->second;
        }
        expected.push_back(recent);
    }
    rankRecentGames(expected, fixedOffsetLocalTimeToUtc(0min), 300);
    REQUIRE(expected.size() == 300);
    REQUIRE(expected.front().favorite);
    REQUIRE_FALSE(expected.back().favorite);

    auto query = SqliteCatalogQuery::open(database->openReader().value());
    REQUIRE(query.has_value());
    RecentGamesQuery request;
    request.limit = 300;
    request.localToUtc = fixedOffsetLocalTimeToUtc(0min);
    const auto games = (*query)->favoritesAndRecent(request);
    REQUIRE(games.has_value());
    REQUIRE(games->size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO(i << ": " << expected[i].id.toString() << " " << expected[i].title);
        CHECK((*games)[i] == expected[i]);
    }
}
