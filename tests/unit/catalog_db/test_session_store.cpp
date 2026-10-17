// rv_catalog_db's session store (I2 step 12): the launch area of schema v1 (play sessions, per-game and per-system
// launch choices, launch verifications) and its rules; every session outcome written and read back unchanged; the
// refusals; only the sessions whose owner is gone closed as interrupted; the listing; choices and verifications; an
// alias standing for its game; a write that meets a busy database tried again, or kept by the recorder until the
// database is free; the state rows kept by `db rebuild-index` (clearIndex) and re-pointed as step 11b re-points an
// alias's rows.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/play_session.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/time.hpp>

#include "catalog_db_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using namespace std::chrono_literals;
using rv::ErrorCode;
using rv::catalog_db::BusyRetry;
using rv::catalog_db::Connection;
using rv::catalog_db::Database;
using rv::catalog_db::LaunchVerification;
using rv::catalog_db::SessionFilter;
using rv::catalog_db::SessionRecorder;
using rv::catalog_db::SessionStore;
using rv::catalog_db::SqliteCatalogStore;
using rv::catalog_db::Transaction;
using rv::launcher::LaunchChoice;
using rv::launcher::LaunchChoiceKind;

namespace {

template <class T>
ErrorCode codeOf(const rv::Expected<T>& result) {
    return result.has_value() ? ErrorCode::Unknown : result.error().code;
}

std::chrono::system_clock::time_point at(std::string_view iso) {
    return rv::core::parseUtcIso8601(iso).value();
}

GameId id(std::uint64_t value) {
    return GameId::fromValue(value).value();
}

const LaunchChoice kFake{LaunchChoiceKind::EsdeCommand, "Fake Emulator"};
const LaunchChoice kProfile{LaunchChoiceKind::Profile, "retroarch-isolated"};

/// A catalog with the library "esde" and identities 1 to 4 (gb), its session store on the writer.
struct SessionFixture {
    explicit SessionFixture(std::string_view prefix, const rv::catalog_db::DatabaseOptions& options = {})
        : dir(prefix), database(rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir), options).value()),
          catalogStore(SqliteCatalogStore::open(database).value()) {
        const LibraryProfile library = rv::test::testLibrary();
        auto transaction = catalogStore->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putLibrary(library).has_value());
        for (std::uint64_t value = 1; value <= 4; ++value) {
            const CatalogEntry entry = rv::test::bareEntry("gb", fmt::format("Game {}.gb", value));
            REQUIRE((*transaction)->putIdentity(rv::test::identityOf(id(value), library.id, entry)).has_value());
            REQUIRE((*transaction)->putEntry(rv::test::indexedEntry(id(value), entry, false)).has_value());
        }
        REQUIRE((*transaction)->commit().has_value());
        store = SessionStore::open(database.writer()).value();
    }

    rv::test::TempDir dir;
    Database database;
    std::unique_ptr<SqliteCatalogStore> catalogStore;
    std::unique_ptr<SessionStore> store;
};

/// A session of `game` begun at `started` by process 4242.
PlaySession sessionOf(GameId game, std::string_view started = "2026-10-17T10:00:00.000Z") {
    PlaySession session;
    session.game = game;
    session.choice = kFake;
    session.choiceSource = ChoiceSource::FirstCommand;
    session.started = at(started);
    session.owner = SessionOwner{4242, 133'000'000'000'000'000ull};
    return session;
}

/// `session` ended at `ended` with `outcome`.
PlaySession ended(PlaySession session, PlaySessionOutcome outcome, std::string_view endedAt) {
    session.outcome = outcome;
    session.ended = at(endedAt);
    return session;
}

std::vector<std::string> textColumn(Connection& connection, std::string_view sql) {
    auto statement = connection.prepare(sql);
    REQUIRE(statement.has_value());
    std::vector<std::string> values;
    while (statement->step().value()) {
        values.emplace_back(statement->columnText(0));
    }
    return values;
}

std::int64_t count(Connection& connection, std::string_view sql) {
    return connection.queryInt(sql).value().value_or(-1);
}

} // namespace

TEST_CASE("catalog_db/sessions: the launch area's tables keep the schema's rules", "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-schema");
    Connection& writer = f.database.writer();
    const auto tables = textColumn(writer, "SELECT name FROM sqlite_schema WHERE type = 'table' AND name GLOB "
                                           "'rv_launch_*' OR name = 'rv_play_sessions' ORDER BY name");
    CHECK(tables == std::vector<std::string>{"rv_launch_game_choices", "rv_launch_system_choices",
                                             "rv_launch_verifications", "rv_play_sessions"});
    // Ids shown to users are AUTOINCREMENT, never reused.
    for (const std::string_view table : {"rv_play_sessions", "rv_launch_verifications"}) {
        INFO(table);
        CHECK(writer.queryText(fmt::format("SELECT sql FROM sqlite_schema WHERE name = '{}'", table))
                  .value()
                  .value()
                  .find("AUTOINCREMENT") != std::string::npos);
    }
    // Every table that names a game refers to its identity, never to the index; a per-game choice is a game row.
    for (const std::string_view table : {"rv_play_sessions", "rv_launch_game_choices", "rv_launch_verifications"}) {
        INFO(table);
        CHECK(count(writer, fmt::format("SELECT count(*) FROM pragma_foreign_key_list('{}') WHERE \"table\" = "
                                        "'rv_game_identities' AND \"from\" = 'game'",
                                        table)) == 1);
    }
    CHECK(writer.queryText("SELECT type FROM pragma_table_info('rv_launch_game_choices') WHERE pk = 1").value() ==
          "INTEGER");
    CHECK(count(writer, "SELECT count(*) FROM pragma_foreign_key_list('rv_launch_system_choices') WHERE \"table\" = "
                        "'rv_libraries'") == 1);
}

TEST_CASE("catalog_db/sessions: every outcome is written and read back unchanged", "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-outcomes");
    SessionStore& store = *f.store;

    std::vector<PlaySession> sessions;
    PlaySession running = sessionOf(id(1));
    running.processId = 777;
    sessions.push_back(running);

    PlaySession returned = ended(sessionOf(id(1)), PlaySessionOutcome::Returned, "2026-10-17T10:05:00.250Z");
    returned.exitCode = 0;
    returned.runTime = 299'750ms;
    returned.test = true;
    returned.choice = kProfile;
    returned.choiceSource = ChoiceSource::RetroverseGame;
    returned.processId = 778;
    returned.message = "Tiny Tennis with Fake Emulator exited after 299.8 s (0 (success))";
    sessions.push_back(returned);

    PlaySession quick = ended(sessionOf(id(2)), PlaySessionOutcome::Returned, "2026-10-17T10:00:00.100Z");
    quick.exitCode = 0;
    quick.quickExit = true;
    sessions.push_back(quick);

    PlaySession crashed = ended(sessionOf(id(2)), PlaySessionOutcome::Crashed, "2026-10-17T10:00:03.000Z");
    crashed.exitCode = 0xC0000005u; // beyond a signed 32-bit integer
    crashed.exitKind = rv::launcher::ExitKind::Crash;
    crashed.message = "crashed: 0xC0000005 STATUS_ACCESS_VIOLATION";
    sessions.push_back(crashed);

    PlaySession refused = ended(sessionOf(id(3)), PlaySessionOutcome::FailedToStart, "2026-10-17T10:00:00.000Z");
    refused.failure = rv::launcher::FailureReason::StartFailed;
    refused.refusedByGuard = true;
    refused.message = "Café Racer could not be started: not listed — 日本";
    sessions.push_back(refused);

    PlaySession early = ended(sessionOf(id(3)), PlaySessionOutcome::FailedToStart, "2026-10-17T10:00:00.300Z");
    early.failure = rv::launcher::FailureReason::EarlyExit;
    early.exitCode = 3;
    early.exitKind = rv::launcher::ExitKind::ErrorCode;
    sessions.push_back(early);

    PlaySession blocked = ended(sessionOf(id(4)), PlaySessionOutcome::Blocked, "2026-10-17T10:00:00.000Z");
    blocked.readiness = LaunchReadiness::RomMissing;
    blocked.choiceSource = ChoiceSource::EsdeGame;
    blocked.message = "rom-missing: the game's file is gone";
    sessions.push_back(blocked);

    PlaySession left = ended(sessionOf(id(4)), PlaySessionOutcome::LeftRunning, "2026-10-17T10:00:02.500Z");
    left.leftRunning = LeftRunningReason::NotWaited;
    left.confirmedByUser = false;
    left.processesRemained = true;
    left.runTime = 2'500ms;
    sessions.push_back(left);

    for (PlaySession& session : sessions) {
        INFO(toString(session.outcome));
        const auto stored = store.insertSession(session);
        REQUIRE(stored.has_value());
        session.id = *stored;
        CHECK(store.session(*stored).value() == session);
    }
    // Ids increase; the running session ends as an update.
    CHECK(sessions[1].id > sessions[0].id);
    PlaySession finished = ended(sessions[0], PlaySessionOutcome::LeftRunning, "2026-10-17T10:30:00.000Z");
    finished.leftRunning = LeftRunningReason::WaitLimit;
    finished.processesRemained = true;
    finished.runTime = 1'800'000ms;
    REQUIRE(store.updateSession(finished).has_value());
    CHECK(store.session(finished.id).value() == finished);
    CHECK_FALSE(store.session(9999).value().has_value());
}

TEST_CASE("catalog_db/sessions: what the store refuses", "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-refusals");
    SessionStore& store = *f.store;
    const auto refusedAs = [&](PlaySession session, ErrorCode code) {
        CHECK(codeOf(store.insertSession(session)) == code);
    };
    PlaySession runningWithEnd = sessionOf(id(1));
    runningWithEnd.ended = at("2026-10-17T10:01:00Z");
    refusedAs(runningWithEnd, ErrorCode::InvalidArgument);
    refusedAs(ended(sessionOf(id(1)), PlaySessionOutcome::Blocked, "2026-10-17T10:00:00Z"), ErrorCode::InvalidArgument);
    PlaySession blockedReady = ended(sessionOf(id(1)), PlaySessionOutcome::Blocked, "2026-10-17T10:00:00Z");
    blockedReady.readiness = LaunchReadiness::Ready;
    refusedAs(blockedReady, ErrorCode::InvalidArgument);
    refusedAs(ended(sessionOf(id(1)), PlaySessionOutcome::LeftRunning, "2026-10-17T10:00:00Z"),
              ErrorCode::InvalidArgument);
    PlaySession finalWithoutEnd = sessionOf(id(1));
    finalWithoutEnd.outcome = PlaySessionOutcome::Returned;
    refusedAs(finalWithoutEnd, ErrorCode::InvalidArgument);
    PlaySession interruptedOpen = sessionOf(id(1));
    interruptedOpen.outcome = PlaySessionOutcome::Interrupted;
    refusedAs(interruptedOpen, ErrorCode::InvalidArgument); // no time it was closed
    PlaySession withId = sessionOf(id(1));
    withId.id = 5;
    refusedAs(withId, ErrorCode::InvalidArgument);
    PlaySession badText = sessionOf(id(1));
    badText.message = "\xC3\x28";
    refusedAs(badText, ErrorCode::InvalidArgument);
    refusedAs(sessionOf(GameId()), ErrorCode::InvalidArgument);
    refusedAs(sessionOf(id(99)), ErrorCode::NotFound); // neither an identity nor an alias
    PlaySession unknown = ended(sessionOf(id(1)), PlaySessionOutcome::Returned, "2026-10-17T10:00:01Z");
    unknown.id = 12345;
    CHECK(codeOf(store.updateSession(unknown)) == ErrorCode::NotFound);
    unknown.id = 0;
    CHECK(codeOf(store.updateSession(unknown)) == ErrorCode::InvalidArgument);
    CHECK(count(f.database.writer(), "SELECT count(*) FROM rv_play_sessions") == 0);
}

TEST_CASE("catalog_db/sessions: only the open sessions whose owner is gone are closed as interrupted",
          "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-interrupted");
    SessionStore& store = *f.store;
    PlaySession alive = sessionOf(id(1));
    alive.owner = SessionOwner{100, 1};
    PlaySession gone = sessionOf(id(2));
    gone.owner = SessionOwner{200, 2};
    PlaySession reused = sessionOf(id(3)); // the id of a live process, another creation time: gone
    reused.owner = SessionOwner{100, 3};
    PlaySession done = ended(sessionOf(id(4)), PlaySessionOutcome::Returned, "2026-10-17T10:01:00Z");
    done.owner = SessionOwner{200, 2};
    for (PlaySession* session : {&alive, &gone, &reused, &done}) {
        session->id = store.insertSession(*session).value();
    }
    std::vector<SessionOwner> asked;
    const auto ownerAlive = [&](const SessionOwner& owner) {
        asked.push_back(owner);
        return owner == SessionOwner{100, 1};
    };
    const auto now = at("2026-10-17T11:00:00.000Z");
    const auto closed = store.closeInterrupted(ownerAlive, now);
    REQUIRE(closed.has_value());
    REQUIRE(closed->size() == 2);
    CHECK(asked.size() == 3); // only the open sessions' owners
    CHECK((*closed)[0].id == gone.id);
    CHECK((*closed)[1].id == reused.id);
    const PlaySession interrupted = store.session(gone.id).value().value();
    CHECK(interrupted.outcome == PlaySessionOutcome::Interrupted);
    CHECK(interrupted.closed == now);
    CHECK_FALSE(interrupted.ended.has_value());
    CHECK(interrupted.message.find("process 200") != std::string::npos);
    CHECK(interrupted == (*closed)[0]);
    CHECK(store.session(alive.id).value()->outcome == PlaySessionOutcome::Running);
    CHECK(store.session(done.id).value() == done);
    // Nothing is left to close; a store without a probe takes every owner as gone.
    CHECK(store.closeInterrupted(ownerAlive, now).value().empty());
    CHECK(store.closeInterrupted(nullptr, now).value().size() == 1);
}

TEST_CASE("catalog_db/sessions: the listing, newest first, by game, limited, with the identity and the title",
          "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-list");
    SessionStore& store = *f.store;
    std::vector<std::int64_t> ids;
    for (const std::uint64_t game : {1u, 2u, 1u, 3u}) {
        ids.push_back(store.insertSession(sessionOf(id(game))).value());
    }
    const auto all = store.sessions(SessionFilter{std::nullopt, 0}).value();
    REQUIRE(all.size() == 4);
    CHECK(all[0].session.id == ids[3]);
    CHECK(all[3].session.id == ids[0]);
    CHECK(all[0].library.str() == "esde");
    CHECK(all[0].system.str() == "gb");
    CHECK(all[0].relativePath == "Game 3.gb");
    CHECK(all[0].title == "Game 3");
    const auto ofOne = store.sessions(SessionFilter{id(1), 50}).value();
    REQUIRE(ofOne.size() == 2);
    CHECK(ofOne[0].session.id == ids[2]);
    CHECK(ofOne[1].session.id == ids[0]);
    CHECK(store.sessions(SessionFilter{std::nullopt, 3}).value().size() == 3);
    CHECK(store.sessions(SessionFilter{id(4), 50}).value().empty());
    // A game without an index row (removed from the index, its identity kept) is listed without a title.
    {
        auto transaction = f.catalogStore->beginImport(LibraryId());
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->removeEntry(id(3)).has_value());
        REQUIRE((*transaction)->commit().has_value());
    }
    const auto untitled = store.sessions(SessionFilter{id(3), 50}).value();
    REQUIRE(untitled.size() == 1);
    CHECK_FALSE(untitled[0].title.has_value());
    CHECK(untitled[0].relativePath == "Game 3.gb");
}

TEST_CASE("catalog_db/sessions: launch choices per game and per system; an alias stands for its game",
          "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-choices");
    SessionStore& store = *f.store;
    const LibraryId esde;
    const SystemName gb = SystemName::parse("gb").value();
    const SystemName snes = SystemName::parse("snes").value();

    CHECK_FALSE(store.gameChoice(id(1)).value().has_value());
    REQUIRE(store.setGameChoice(id(1), kFake, at("2026-10-17T10:00:00Z")).has_value());
    CHECK(store.gameChoice(id(1)).value() == rv::catalog_db::StoredChoice{kFake, at("2026-10-17T10:00:00Z")});
    REQUIRE(store.setGameChoice(id(1), kProfile, at("2026-10-17T10:01:00Z")).has_value()); // replaces it
    CHECK(store.gameChoice(id(1)).value()->choice == kProfile);
    CHECK(codeOf(store.setGameChoice(id(99), kFake, at("2026-10-17T10:00:00Z"))) == ErrorCode::NotFound);
    CHECK(codeOf(store.setGameChoice(id(1), LaunchChoice{LaunchChoiceKind::EsdeCommand, ""}, at("2026-10-17T10:00:00Z"))) ==
          ErrorCode::InvalidArgument);
    CHECK(store.clearGameChoice(id(1)).value());
    CHECK_FALSE(store.clearGameChoice(id(1)).value());
    CHECK_FALSE(store.gameChoice(id(1)).value().has_value());

    REQUIRE(store.setSystemChoice(esde, snes, kFake, at("2026-10-17T10:00:00Z")).has_value());
    REQUIRE(store.setSystemChoice(esde, gb, kProfile, at("2026-10-17T10:02:00Z")).has_value());
    const auto systems = store.systemChoices(esde).value();
    REQUIRE(systems.size() == 2);
    CHECK(systems[0].system == gb); // by name
    CHECK(systems[0].stored.choice == kProfile);
    CHECK(store.systemChoice(esde, snes).value()->choice == kFake);
    CHECK(codeOf(store.setSystemChoice(LibraryId::parse("other").value(), gb, kFake, at("2026-10-17T10:00:00Z"))) ==
          ErrorCode::InvalidArgument); // a library the catalog does not hold
    CHECK(store.systemChoices(LibraryId::parse("other").value()).value().empty());
    CHECK(store.clearSystemChoice(esde, snes).value());
    CHECK_FALSE(store.systemChoice(esde, snes).value().has_value());

    // GameId 9 stands for game 2 (step 11b: a file moved back onto a key that had another identity meanwhile).
    REQUIRE(f.database.writer()
                .execute("INSERT INTO rv_identity_aliases (alias, game, lookup_key, relative_path, at, reason) "
                         "VALUES (9, 2, 'GAME 9.GB', 'Game 9.gb', '2026-10-17T10:00:00.000Z', 'moved-back')")
                .has_value());
    CHECK(store.resolveGame(id(9)).value() == id(2));
    CHECK(store.resolveGame(id(2)).value() == id(2));
    CHECK_FALSE(store.resolveGame(id(99)).value().has_value());
    REQUIRE(store.setGameChoice(id(9), kFake, at("2026-10-17T10:03:00Z")).has_value());
    CHECK(store.gameChoice(id(2)).value()->choice == kFake);
    CHECK(store.gameChoice(id(9)).value()->choice == kFake);
    const std::int64_t session = store.insertSession(sessionOf(id(9))).value();
    CHECK(store.session(session).value()->game == id(2)); // recorded under the identity
    CHECK(store.sessions(SessionFilter{id(9), 50}).value().size() == 1);
}

TEST_CASE("catalog_db/sessions: a verification is the latest per library, system and choice", "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-verified");
    SessionStore& store = *f.store;
    const std::int64_t first =
        store.insertSession(ended(sessionOf(id(1)), PlaySessionOutcome::Returned, "2026-10-17T10:05:00Z")).value();
    const std::int64_t second =
        store.insertSession(ended(sessionOf(id(2)), PlaySessionOutcome::Returned, "2026-10-17T11:05:00Z")).value();
    LaunchVerification verification;
    verification.system = SystemName::parse("gb").value();
    verification.choice = kFake;
    verification.game = id(1);
    verification.session = first;
    verification.test = true;
    verification.program = std::filesystem::path(L"C:\\Émulateurs\\fake-emulator.exe");
    verification.verifiedAt = at("2026-10-17T10:05:00Z");
    const std::int64_t row = store.recordVerification(verification).value();
    LaunchVerification later = verification;
    later.game = id(2);
    later.session = second;
    later.test = false;
    later.verifiedAt = at("2026-10-17T11:05:00Z");
    CHECK(store.recordVerification(later).value() == row); // the same row, updated
    LaunchVerification other = verification;
    other.choice = kProfile;
    CHECK(store.recordVerification(other).value() != row);
    const auto rows = store.verifications(LibraryId()).value();
    REQUIRE(rows.size() == 2);
    later.id = row;
    CHECK(rows[0] == later); // "command" sorts before "profile"
    CHECK(rows[1].choice == kProfile);
    LaunchVerification noSession = verification;
    noSession.session = 999;
    CHECK(codeOf(store.recordVerification(noSession)) == ErrorCode::InvalidArgument);
}

TEST_CASE("catalog_db/sessions: a write that meets a busy database is tried again, or kept until it is free",
          "[catalog_db][sessions]") {
    // Two Database objects on one file stand for two processes (docs/i2/step-4.md): the first holds the write lock as
    // another process's bulk import holds it (its write transaction), the second writes sessions with a short busy
    // timeout.
    rv::catalog_db::DatabaseOptions quick;
    quick.busyTimeout = 100ms;
    SessionFixture f("rv-sessions-busy", quick);
    auto importer = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(f.dir));
    REQUIRE(importer.has_value());
    SessionStore& store = *f.store;
    std::optional<Transaction> held = Transaction::begin(importer->writer(), Transaction::Mode::Immediate).value();

    // One attempt: Conflict, a busy database.
    const auto refused = store.insertSession(sessionOf(id(1)), BusyRetry{0ms, 10ms});
    REQUIRE_FALSE(refused.has_value());
    CHECK(rv::catalog_db::isBusyError(refused.error()));
    // The recorder keeps what the database refused: begin() is no failure, the session is pending.
    SessionRecorder recorder(store, sessionOf(id(2)));
    REQUIRE(recorder.begin().has_value());
    CHECK(recorder.pending());
    CHECK_FALSE(recorder.stored());
    PlaySession end = ended(sessionOf(id(2)), PlaySessionOutcome::Returned, "2026-10-17T10:00:05Z");
    end.exitCode = 0;
    recorder.update(end);
    // finish() with too little patience gives the busy error back and keeps it pending.
    const auto impatient = recorder.finish(BusyRetry{200ms, 50ms});
    REQUIRE_FALSE(impatient.has_value());
    CHECK(rv::catalog_db::isBusyError(impatient.error()));
    CHECK(recorder.pending());

    // The import commits after 0.6 s: a write tried again for long enough gets in.
    bool committed = false;
    std::thread release([&] {
        std::this_thread::sleep_for(600ms);
        committed = held->commit().has_value();
    });
    const auto before = std::chrono::steady_clock::now();
    const auto patient = store.insertSession(sessionOf(id(3)), BusyRetry{10s, 50ms});
    const auto waited = std::chrono::steady_clock::now() - before;
    release.join();
    CHECK(committed);
    REQUIRE(patient.has_value());
    CHECK(waited >= 500ms);
    CHECK(store.busyRetries() > 0);
    REQUIRE(recorder.flush().has_value());
    CHECK_FALSE(recorder.pending());
    CHECK(recorder.stored());
    CHECK(store.session(recorder.session().id).value() == recorder.session());
    CHECK(store.session(recorder.session().id).value()->outcome == PlaySessionOutcome::Returned);
    // A UNIQUE conflict is no busy database: never tried again.
    CHECK_FALSE(rv::catalog_db::isBusyError(rv::Error{ErrorCode::Conflict, "UNIQUE constraint failed: x (SQLite "
                                                                            "result code 2067)",
                                                      {}}));
    CHECK(rv::catalog_db::isBusyError(rv::Error{ErrorCode::Conflict, "database is locked (SQLite result code 5)", {}}));
    CHECK(rv::catalog_db::isBusyError(rv::Error{ErrorCode::Conflict, "x (SQLite result code 517)", {}}));
}

TEST_CASE("catalog_db/sessions: the state rows outlive a rebuild of the index and follow an alias as step 11b moves "
          "them",
          "[catalog_db][sessions]") {
    SessionFixture f("rv-sessions-state");
    SessionStore& store = *f.store;
    const LibraryId esde;
    const SystemName gb = SystemName::parse("gb").value();
    const std::int64_t s1 = store.insertSession(sessionOf(id(1))).value();
    const std::int64_t s2 =
        store.insertSession(ended(sessionOf(id(2)), PlaySessionOutcome::Returned, "2026-10-17T10:05:00Z")).value();
    REQUIRE(store.setGameChoice(id(1), kFake, at("2026-10-17T10:00:00Z")).has_value());
    REQUIRE(store.setGameChoice(id(2), kProfile, at("2026-10-17T10:00:00Z")).has_value());
    REQUIRE(store.setSystemChoice(esde, gb, kProfile, at("2026-10-17T10:00:00Z")).has_value());
    LaunchVerification verification;
    verification.system = gb;
    verification.choice = kFake;
    verification.game = id(2);
    verification.session = s2;
    verification.program = "C:\\fake.exe";
    verification.verifiedAt = at("2026-10-17T10:05:00Z");
    REQUIRE(store.recordVerification(verification).has_value());

    // db rebuild-index clears the index tables (clearIndex()) and writes them again: the state stays.
    {
        auto transaction = f.catalogStore->beginImport(esde);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->clearIndex(esde).has_value());
        REQUIRE((*transaction)->commit().has_value());
    }
    Connection& writer = f.database.writer();
    CHECK(count(writer, "SELECT count(*) FROM idx_games") == 0);
    CHECK(store.session(s1).value().has_value());
    CHECK(store.gameChoice(id(1)).value()->choice == kFake);
    CHECK(store.systemChoice(esde, gb).value()->choice == kProfile);
    CHECK(store.verifications(esde).value().size() == 1);

    // Identity 1 becomes an alias of identity 2 (step 11b's re-pointing): every rv_* row that refers to it through a
    // foreign key into rv_game_identities, found from the schema, is moved with UPDATE OR IGNORE and what is left
    // deleted; then the identity row goes. The launch tables need nothing else.
    const auto references = textColumn(
        writer, "SELECT m.name || '|' || k.\"from\" FROM sqlite_schema AS m, pragma_foreign_key_list(m.name) AS k "
                "WHERE m.type = 'table' AND m.name GLOB 'rv_*' AND k.\"table\" = 'rv_game_identities' ORDER BY 1");
    CHECK(std::find(references.begin(), references.end(), "rv_play_sessions|game") != references.end());
    CHECK(std::find(references.begin(), references.end(), "rv_launch_game_choices|game") != references.end());
    CHECK(std::find(references.begin(), references.end(), "rv_launch_verifications|game") != references.end());
    auto transaction = Transaction::begin(writer, Transaction::Mode::Immediate);
    REQUIRE(transaction.has_value());
    for (const std::string& reference : references) {
        const std::string table = reference.substr(0, reference.find('|'));
        const std::string column = reference.substr(reference.find('|') + 1);
        REQUIRE(
            writer.execute(fmt::format("UPDATE OR IGNORE {0} SET {1} = 2 WHERE {1} = 1", table, column)).has_value());
        REQUIRE(writer.execute(fmt::format("DELETE FROM {0} WHERE {1} = 1", table, column)).has_value());
    }
    REQUIRE(writer
                .execute("INSERT INTO rv_identity_aliases (alias, game, lookup_key, relative_path, at, reason) "
                         "VALUES (1, 2, 'GAME 1.GB', 'Game 1.gb', '2026-10-17T11:00:00.000Z', 'moved-back'); "
                         "DELETE FROM rv_game_identities WHERE id = 1")
                .has_value());
    REQUIRE(transaction->commit().has_value());
    CHECK(store.session(s1).value()->game == id(2));               // the history moved
    CHECK(store.gameChoice(id(2)).value()->choice == kProfile);    // the moved identity's own choice kept
    CHECK(store.gameChoice(id(1)).value()->choice == kProfile);    // the alias stands for it
    CHECK(store.sessions(SessionFilter{id(1), 0}).value().size() == 2);
    CHECK(count(writer, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
}
