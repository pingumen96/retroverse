// I2 step 4: the catalog database's connections (ADR-0008 §5): the file and its lock file, the writer's and the
// readers' settings, FTS5, statements and SQLite's errors as Expected values, transactions, a reader during a write,
// interrupting the writer, files that are not a catalog database, long and non-ASCII paths, and the process-level lock
// between holders (two holders in one process stand for two processes: LockFileEx locks belong to handles).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "catalog_db_test_support.hpp"
#include "support/file_system.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog_db;
using namespace std::chrono_literals;
using rv::ErrorCode;

namespace {

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// The error code of a failed result (Unknown when it did not fail, which the checks then report).
template <class T>
ErrorCode codeOf(const rv::Expected<T>& result) {
    return result.has_value() ? ErrorCode::Unknown : result.error().code;
}

/// Writes a database file with `sql` run in a private database (a rollback-journal file, as another application might
/// leave one), through VACUUM INTO.
void writeForeignDatabase(const std::filesystem::path& file, std::string_view sql) {
    auto memory = Connection::openMemory();
    REQUIRE(memory.has_value());
    REQUIRE(memory->execute(sql).has_value());
    auto vacuum = memory->prepare("VACUUM INTO ?1");
    REQUIRE(vacuum.has_value());
    vacuum->bindText(1, rv::core::pathToUtf8(file));
    REQUIRE(vacuum->run().has_value());
}

} // namespace

TEST_CASE("catalog_db/database: the file is <user data>/database/retroverse.sqlite, its lock file beside it",
          "[catalog_db]") {
    rv::core::PathEnvironment environment;
    environment.executableDir = std::filesystem::path("C:/Programs/Retroverse");
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    rv::core::PathOptions options;
    options.userDataOverride = std::filesystem::path("D:/Retroverse data");
    const auto paths = rv::core::Paths::resolve(options, environment);
    REQUIRE(paths.has_value());
    CHECK(databaseFile(*paths) == paths->databaseDir() / "retroverse.sqlite");
    CHECK(kDatabaseFileName == "retroverse.sqlite");
    CHECK(lockFileOf(std::filesystem::path("D:/data/database/retroverse.sqlite")) ==
          std::filesystem::path("D:/data/database/retroverse.sqlite.lock"));
}

TEST_CASE("catalog_db/database: the writer runs in WAL with foreign keys, synchronous FULL, a journal size limit and a "
          "busy timeout; readers are read-only and never wait",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-pragmas");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    auto database = Database::open(file); // the folders above it are made
    REQUIRE(database.has_value());
    CHECK(database->path() == file);
    CHECK(std::filesystem::exists(file));
    CHECK(std::filesystem::exists(lockFileOf(file)));
    CHECK(database->lock().mode() == LockMode::Shared);

    Connection& writer = database->writer();
    CHECK(writer.role() == ConnectionRole::Writer);
    CHECK(writer.path() == file);
    CHECK(writer.queryText("PRAGMA journal_mode").value() == "wal");
    CHECK(writer.queryInt("PRAGMA foreign_keys").value() == 1);
    CHECK(writer.queryInt("PRAGMA synchronous").value() == 2); // FULL
    CHECK(writer.queryInt("PRAGMA journal_size_limit").value() == kJournalSizeLimitBytes);
    CHECK(writer.queryInt("PRAGMA busy_timeout").value() == 5000);
    CHECK(writer.busyTimeout() == 5000ms);

    auto reader = database->openReader();
    REQUIRE(reader.has_value());
    CHECK(reader->role() == ConnectionRole::Reader);
    CHECK(reader->queryInt("PRAGMA busy_timeout").value() == 0);
    CHECK(reader->queryInt("PRAGMA foreign_keys").value() == 1);
    const auto write = reader->execute("CREATE TABLE notes (text TEXT)");
    REQUIRE_FALSE(write.has_value());
    CHECK(contains(write.error().message, "readonly"));
}

TEST_CASE("catalog_db/database: SQLite has FTS5, whose unicode61 tokenizer folds case and diacritics", "[catalog_db]") {
    CHECK(sqliteHasFts5());
    CHECK(sqliteVersion().starts_with("3."));
    auto memory = Connection::openMemory();
    REQUIRE(memory.has_value());
    REQUIRE(memory
                ->execute("CREATE VIRTUAL TABLE search USING fts5(title, tokenize = 'unicode61 remove_diacritics 2', "
                          "prefix = '2 3');"
                          "INSERT INTO search (title) VALUES ('Pokémon Cristallo'), ('Super Mario World')")
                .has_value());
    CHECK(memory->queryInt("SELECT count(*) FROM search WHERE search MATCH 'pokemon'").value() == 1);
    CHECK(memory->queryInt("SELECT count(*) FROM search WHERE search MATCH 'POKÉMON'").value() == 1);
    CHECK(memory->queryInt("SELECT count(*) FROM search WHERE search MATCH 'wor*'").value() == 1);
    CHECK(memory->queryInt("SELECT count(*) FROM search WHERE search MATCH 'zelda'").value() == 0);
}

TEST_CASE("catalog_db/database: statements bind, step and read columns; SQLite's errors come back with its message",
          "[catalog_db]") {
    auto opened = Connection::openMemory();
    REQUIRE(opened.has_value());
    Connection& memory = *opened;
    CHECK(memory.role() == ConnectionRole::Memory);
    CHECK(memory.path().empty());
    REQUIRE(memory
                .execute(R"sql(
                    -- comments and blank statements are skipped
                    CREATE TABLE parent (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE) STRICT;
                    CREATE TABLE child (
                        id     INTEGER PRIMARY KEY,
                        parent INTEGER NOT NULL REFERENCES parent (id),
                        score  INTEGER CHECK (score >= 0),
                        data   BLOB
                    ) STRICT;
                )sql")
                .has_value());

    auto insert = memory.prepare("INSERT INTO parent (id, name) VALUES (?1, ?2)");
    REQUIRE(insert.has_value());
    CHECK(insert->sql() == "INSERT INTO parent (id, name) VALUES (?1, ?2)");
    insert->bindInt(1, 1).bindText(2, "Café");
    REQUIRE(insert->run().has_value());
    CHECK(memory.lastInsertRowId() == 1);
    CHECK(memory.changes() == 1);
    insert->bindInt(1, 2).bindText(2, ""); // an empty text is '', never NULL
    REQUIRE(insert->run().has_value());

    auto select = memory.prepare("SELECT id, name, length(name), NULL, 1.5 FROM parent ORDER BY id");
    REQUIRE(select.has_value());
    CHECK(select->columnCount() == 5);
    {
        const ScopedReset reset(*select);
        REQUIRE(select->step().value());
        CHECK(select->columnInt(0) == 1);
        CHECK(select->columnText(1) == "Café");
        CHECK(select->columnInt(2) == 4); // characters
        CHECK(select->columnType(3) == ColumnType::Null);
        CHECK(select->isNull(3));
        CHECK_FALSE(select->columnOptionalInt(3).has_value());
        CHECK(select->columnType(4) == ColumnType::Real);
        CHECK(select->columnReal(4) == 1.5);
        REQUIRE(select->step().value());
        CHECK(select->columnType(1) == ColumnType::Text);
        CHECK(select->columnText(1).empty());
        CHECK_FALSE(select->step().value());
    }
    // Reset: the statement runs again from its first row.
    REQUIRE(select->step().value());
    CHECK(select->columnInt(0) == 1);
    select->reset();

    const std::array<std::byte, 4> bytes{std::byte{0}, std::byte{0xFF}, std::byte{7}, std::byte{0}};
    auto child = memory.prepare("INSERT INTO child (id, parent, score, data) VALUES (?1, ?2, ?3, ?4)");
    REQUIRE(child.has_value());
    child->bindInt(1, 10).bindInt(2, 1).bindInt(3, 5).bindBlob(4, bytes);
    REQUIRE(child->run().has_value());
    child->bindInt(1, 11).bindInt(2, 1).bindNull(3).bindBlob(4, {}); // an empty blob is a zero-length one
    REQUIRE(child->run().has_value());
    auto blobs = memory.prepare("SELECT data FROM child ORDER BY id");
    REQUIRE(blobs.has_value());
    REQUIRE(blobs->step().value());
    const auto read = blobs->columnBlob(0);
    CHECK(std::equal(read.begin(), read.end(), bytes.begin(), bytes.end()));
    REQUIRE(blobs->step().value());
    CHECK(blobs->columnType(0) == ColumnType::Blob);
    CHECK(blobs->columnBlob(0).empty());
    blobs->reset();

    // SQLite's errors as ErrorCodes, SQLite's message kept, the statement's SQL as context.
    insert->bindInt(1, 3).bindText(2, "Café");
    const auto unique = insert->run();
    REQUIRE_FALSE(unique.has_value());
    CHECK(unique.error().code == ErrorCode::Conflict);
    CHECK(contains(unique.error().message, "UNIQUE constraint failed: parent.name"));
    CHECK(contains(unique.error().context, "INSERT INTO parent"));

    child->bindInt(1, 12).bindInt(2, 99).bindInt(3, 1).bindNull(4);
    const auto foreignKey = child->run();
    REQUIRE_FALSE(foreignKey.has_value());
    CHECK(foreignKey.error().code == ErrorCode::InvalidArgument);
    CHECK(contains(foreignKey.error().message, "FOREIGN KEY constraint failed"));

    child->bindInt(1, 12).bindInt(2, 1).bindInt(3, -1).bindNull(4);
    const auto check = child->run();
    REQUIRE_FALSE(check.has_value());
    CHECK(check.error().code == ErrorCode::ValidationFailed);
    CHECK(contains(check.error().message, "CHECK constraint failed"));

    child->bindInt(1, 12).bindInt(2, 1).bindText(3, "many").bindNull(4); // STRICT: no text in an INTEGER column
    CHECK(codeOf<void>(child->run()) == ErrorCode::ValidationFailed);

    const auto syntax = memory.prepare("SELEC 1");
    REQUIRE_FALSE(syntax.has_value());
    CHECK(syntax.error().code == ErrorCode::InvalidArgument);
    CHECK(contains(syntax.error().message, "syntax error"));
    CHECK(codeOf(memory.prepare("SELECT 1; SELECT 2")) == ErrorCode::InvalidArgument);
    CHECK(codeOf(memory.prepare("  -- nothing but a comment\n")) == ErrorCode::InvalidArgument);
    CHECK(memory.prepare("SELECT 1; -- a comment after it").has_value());

    // execute() names the statement that failed, after running the ones before it.
    const auto second =
        memory.execute("INSERT INTO parent (id, name) VALUES (5, 'five'); INSERT INTO nowhere VALUES (1)");
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == ErrorCode::InvalidArgument);
    CHECK(contains(second.error().message, "no such table: nowhere"));
    CHECK(second.error().context.starts_with("INSERT INTO nowhere"));
    CHECK(memory.queryInt("SELECT count(*) FROM parent WHERE id = 5").value() == 1);

    // A binding that fails is reported by the next step, until the statement is reset.
    auto parameter = memory.prepare("SELECT ?1");
    REQUIRE(parameter.has_value());
    parameter->bindInt(2, 5);
    CHECK(codeOf(parameter->step()) == ErrorCode::InvalidArgument);
    parameter->reset();
    parameter->bindInt(1, 5);
    REQUIRE(parameter->step().value());
    CHECK(parameter->columnInt(0) == 5);

    CHECK_FALSE(memory.queryInt("SELECT 1 WHERE 0").value().has_value());
    CHECK_FALSE(memory.queryText("SELECT NULL").value().has_value());
    CHECK(memory.userVersion().value() == 0);
    CHECK(codeOf<void>(Statement().run()) == ErrorCode::InvalidArgument);
}

TEST_CASE("catalog_db/database: a transaction rolls back unless committed", "[catalog_db]") {
    auto opened = Connection::openMemory();
    REQUIRE(opened.has_value());
    Connection& memory = *opened;
    REQUIRE(memory.execute("CREATE TABLE notes (text TEXT NOT NULL) STRICT").has_value());
    {
        auto transaction = Transaction::begin(memory, Transaction::Mode::Immediate);
        REQUIRE(transaction.has_value());
        CHECK(transaction->active());
        CHECK(memory.inTransaction());
        REQUIRE(memory.execute("INSERT INTO notes VALUES ('dropped')").has_value());
        CHECK(codeOf(Transaction::begin(memory, Transaction::Mode::Deferred)) == ErrorCode::InvalidArgument);
    }
    CHECK_FALSE(memory.inTransaction());
    CHECK(memory.queryInt("SELECT count(*) FROM notes").value() == 0);

    auto transaction = Transaction::begin(memory, Transaction::Mode::Deferred);
    REQUIRE(transaction.has_value());
    REQUIRE(memory.execute("INSERT INTO notes VALUES ('kept')").has_value());
    REQUIRE(transaction->commit().has_value());
    CHECK_FALSE(transaction->active());
    CHECK(codeOf<void>(transaction->commit()) == ErrorCode::InvalidArgument);
    transaction->rollback(); // nothing: it has ended
    CHECK(memory.queryInt("SELECT count(*) FROM notes").value() == 1);
}

TEST_CASE("catalog_db/database: a reader during a write sees the previous snapshot and does not wait", "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-reader");
    auto database = openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    Connection& writer = database->writer();
    REQUIRE(writer.execute("INSERT INTO rv_meta (key, value) VALUES ('test', 'before')").has_value());
    auto reader = database->openReader();
    REQUIRE(reader.has_value());
    constexpr std::string_view kRead = "SELECT value FROM rv_meta WHERE key = 'test'";
    CHECK(reader->queryText(kRead).value() == "before");
    {
        auto write = Transaction::begin(writer, Transaction::Mode::Immediate);
        REQUIRE(write.has_value());
        REQUIRE(writer
                    .execute("UPDATE rv_meta SET value = 'during' WHERE key = 'test';"
                             "INSERT INTO rv_meta (key, value) VALUES ('test-new', 'new')")
                    .has_value());
        CHECK(writer.queryText(kRead).value() == "during"); // the writer reads its own writes

        // The write lock is taken: the reader neither waits (no busy timeout: it would fail at once) nor sees them.
        const auto started = std::chrono::steady_clock::now();
        const auto seen = reader->queryText(kRead);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        REQUIRE(seen.has_value());
        CHECK(seen->value() == "before");
        CHECK(reader->queryInt("SELECT count(*) FROM rv_meta WHERE key = 'test-new'").value() == 0);
        CHECK(elapsed < 1s);
        // A reader opened during the write, and another process's connection, see the same snapshot.
        auto late = database->openReader();
        REQUIRE(late.has_value());
        CHECK(late->queryText(kRead).value() == "before");
        DatabaseOptions noWait;
        noWait.busyTimeout = 0ms;
        auto other = Database::open(database->path(), noWait);
        REQUIRE(other.has_value());
        CHECK(other->writer().queryText(kRead).value() == "before");
        // ... which cannot write meanwhile: the writer's lock is SQLite's, and without a busy timeout it fails at once.
        CHECK(codeOf<void>(other->writer().execute("DELETE FROM rv_meta WHERE key = 'test'")) == ErrorCode::Conflict);
        REQUIRE(write->commit().has_value());
    }
    CHECK(reader->queryText(kRead).value() == "during");
    CHECK(reader->queryInt("SELECT count(*) FROM rv_meta WHERE key = 'test-new'").value() == 1);
}

TEST_CASE("catalog_db/database: interrupt() stops the writer's running statement with Cancelled", "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-interrupt");
    auto database = openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    std::atomic<bool> done{false};
    std::optional<rv::Expected<std::optional<std::int64_t>>> result;
    // Long but bounded, so that a broken interrupt fails the test instead of hanging it.
    std::thread worker([&] {
        result = database->writer().queryInt(
            "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM n LIMIT 200000000) SELECT count(*) FROM n");
        done = true;
    });
    // sqlite3_interrupt() stops what runs when it is called: called until the statement has stopped.
    while (!done) {
        database->interrupt();
        std::this_thread::sleep_for(5ms);
    }
    worker.join();
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(result->error().code == ErrorCode::Cancelled);
    // The connection works on.
    CHECK(database->writer().queryInt("SELECT 1").value() == 1);
}

TEST_CASE("catalog_db/database: opening refuses what is not a catalog database, and makes nothing without create",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-refuse");
    DatabaseOptions noCreate;
    noCreate.create = false;

    const auto missing = Database::open(dir.file("none/retroverse.sqlite"), noCreate);
    CHECK(codeOf(missing) == ErrorCode::NotFound);
    CHECK_FALSE(std::filesystem::exists(dir.file("none")));
    CHECK(codeOf(Database::open(dir.path())) == ErrorCode::InvalidArgument); // a folder

    // A text file: not a database (ParseError), and left as it was.
    const std::filesystem::path text = dir.file("text.sqlite");
    const std::string content(4096, 'x');
    REQUIRE(rv::core::writeTextFileAtomic(text, content).has_value());
    CHECK(codeOf(Database::open(text)) == ErrorCode::ParseError);
    CHECK(rv::core::readTextFile(text).value() == content);

    // Another application's database: refused, never switched to WAL.
    const std::filesystem::path foreign = dir.file("foreign.sqlite");
    writeForeignDatabase(foreign, "PRAGMA application_id = 1234; CREATE TABLE theirs (x INTEGER)");
    const auto theirs = Database::open(foreign);
    REQUIRE_FALSE(theirs.has_value());
    CHECK(theirs.error().code == ErrorCode::Conflict);
    CHECK(contains(theirs.error().message, "not a Retroverse catalog database"));
    REQUIRE(rv::test::readDatabaseHeader(foreign).has_value());
    CHECK(rv::test::readDatabaseHeader(foreign)->writeVersion == 1); // still a rollback journal

    // Tables without Retroverse's application id: refused too.
    const std::filesystem::path unmarked = dir.file("unmarked.sqlite");
    writeForeignDatabase(unmarked, "CREATE TABLE notes (x INTEGER)");
    CHECK(codeOf(Database::open(unmarked)) == ErrorCode::Conflict);

    // An empty file is a new database.
    const std::filesystem::path empty = dir.file("empty.sqlite");
    REQUIRE(rv::core::writeTextFileAtomic(empty, "").has_value());
    auto fresh = Database::open(empty, noCreate);
    REQUIRE(fresh.has_value());
    CHECK(fresh->schemaVersion().value() == 0);
}

TEST_CASE("catalog_db/database: a database past MAX_PATH, in folders named outside ASCII, opens, migrates and backs up",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-long");
    // Removed through its \\?\ form after the database closes (declared before it), whatever the long path setting.
    struct RemoveTree {
        std::filesystem::path path;
        ~RemoveTree() { rv::test::removeTreeAnyLength(path); }
    } removeTree{dir.path() / L"deep"};
    const std::filesystem::path folder =
        dir.path() / L"deep" / std::wstring(100, L'a') / std::wstring(100, L'b') / L"dàta \u65E5\u672C";
    const std::filesystem::path file = folder / L"retroverse.sqlite";
    REQUIRE(file.native().size() > 260);

    auto database = openCatalogDatabase(file);
    REQUIRE(database.has_value());
    CHECK(database->schemaVersion().value() == programSchemaVersion());
    CHECK(rv::test::existsAnyLength(file));
    CHECK(rv::test::existsAnyLength(lockFileOf(file)));
    auto reader = database->openReader();
    REQUIRE(reader.has_value());
    CHECK(reader->queryInt("SELECT count(*) FROM rv_meta").value() > 0);

    MigrationOptions options;
    options.migrations = rv::test::migrationsUpTo(programSchemaVersion() + 1);
    const auto report = migrate(*database, options);
    REQUIRE(report.has_value());
    REQUIRE(report->backup.has_value());
    CHECK(report->backup->parent_path() == backupFolderOf(file));
    CHECK(rv::test::existsAnyLength(*report->backup));
    CHECK(listBackups(file) == std::vector<std::filesystem::path>{*report->backup});
    const auto header = rv::test::readDatabaseHeader(*report->backup);
    REQUIRE(header.has_value());
    CHECK(header->userVersion == static_cast<std::uint32_t>(programSchemaVersion()));
}

TEST_CASE("catalog_db/lock: holders share the lock; a second holder blocks the exclusive migration lock",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-lock");
    const std::filesystem::path lockFile = dir.file("retroverse.sqlite.lock");
    std::optional<DatabaseLock> first(DatabaseLock::acquireShared(lockFile, 0ms).value());
    auto second = DatabaseLock::acquireShared(lockFile, 0ms);
    REQUIRE(second.has_value());
    CHECK(first->mode() == LockMode::Shared);
    CHECK(second->path() == lockFile);

    // Another holder has the database open: no exclusive lock after the wait, and the holder still holds it shared.
    const auto refused = second->upgrade(50ms);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::Conflict);
    CHECK(second->mode() == LockMode::Shared);
    CHECK(second->held());
    CHECK(DatabaseLock::acquireShared(lockFile, 0ms).has_value()); // others join again: the gate is released

    // The other holder leaves within the wait: the lock becomes exclusive, and nobody joins until it is shared again.
    std::thread leaving([&] {
        std::this_thread::sleep_for(100ms);
        first.reset();
    });
    const auto upgraded = second->upgrade(10s);
    leaving.join();
    REQUIRE(upgraded.has_value());
    CHECK(*upgraded == DatabaseLock::Upgrade::Exclusive);
    CHECK(second->mode() == LockMode::Exclusive);
    CHECK(second->upgrade(0ms).value() == DatabaseLock::Upgrade::Exclusive); // already exclusive
    const auto joining = DatabaseLock::acquireShared(lockFile, 50ms);
    REQUIRE_FALSE(joining.has_value());
    CHECK(joining.error().code == ErrorCode::Conflict);
    CHECK(contains(joining.error().message, "migrating"));

    // A holder that waits to join gets in once the migration ends, within its wait.
    std::optional<rv::Expected<DatabaseLock>> waited;
    std::thread joiner([&] { waited.emplace(DatabaseLock::acquireShared(lockFile, 10s)); });
    std::this_thread::sleep_for(100ms);
    second->downgrade();
    joiner.join();
    REQUIRE(waited.has_value());
    CHECK(waited->has_value());
    CHECK(second->mode() == LockMode::Shared);
    second->downgrade();                                        // already shared: nothing
    CHECK(codeOf(second->upgrade(0ms)) == ErrorCode::Conflict); // the joiner holds it now
    CHECK(second->rejoin(0ms).has_value());                     // leaves and joins again at once
    CHECK(second->held());
}

TEST_CASE("catalog_db/lock: of two holders that want to migrate, one waits with the gate and the other lets it go "
          "first",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-lock-two");
    const std::filesystem::path lockFile = dir.file("retroverse.sqlite.lock");
    auto first = DatabaseLock::acquireShared(lockFile, 0ms);
    auto second = DatabaseLock::acquireShared(lockFile, 0ms);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    // The first takes the gate (again and again while the second's attempts hold it for an instant) and waits for the
    // second to leave; it migrates, then shares the lock again.
    std::optional<rv::Expected<DatabaseLock::Upgrade>> upgraded;
    std::thread migrating([&] {
        do {
            upgraded.emplace(first->upgrade(10s));
        } while (upgraded->has_value() && **upgraded == DatabaseLock::Upgrade::GateTaken);
        std::this_thread::sleep_for(100ms); // the migration
        first->downgrade();
    });
    // The second wants to migrate too. Once the first waits with the gate, the second finds it taken: it would wait on
    // the first as the first waits on it.
    bool gateTaken = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!gateTaken && std::chrono::steady_clock::now() < deadline) {
        const auto attempt = second->upgrade(0ms);
        gateTaken = attempt.has_value() && *attempt == DatabaseLock::Upgrade::GateTaken;
        if (!gateTaken) {
            CHECK(codeOf(attempt) == ErrorCode::Conflict); // the first holds it shared, not yet waiting
            std::this_thread::sleep_for(5ms);
        }
    }
    CHECK(gateTaken);
    CHECK(second->mode() == LockMode::Shared);
    // It leaves and joins again: the first migrates meanwhile, and the second is back once it is done.
    REQUIRE(second->rejoin(10s).has_value());
    migrating.join();
    REQUIRE(upgraded.has_value());
    REQUIRE(upgraded->has_value());
    CHECK(**upgraded == DatabaseLock::Upgrade::Exclusive);
    CHECK(first->mode() == LockMode::Shared);
    CHECK(second->held());
    CHECK(second->mode() == LockMode::Shared);
}

TEST_CASE("catalog_db/lock: the import lock is one holder's at a time and goes with its holder", "[catalog_db]") {
    const rv::test::TempDir dir("rv-db-import-lock");
    const std::filesystem::path lockFile = dir.file("retroverse.sqlite.lock");
    auto first = DatabaseLock::acquireShared(lockFile, 0ms);
    std::optional<DatabaseLock> second(DatabaseLock::acquireShared(lockFile, 0ms).value());
    REQUIRE(first.has_value());

    REQUIRE(second->lockImport().has_value());
    CHECK(second->holdsImport());
    CHECK(codeOf<void>(second->lockImport()) == ErrorCode::Conflict); // held already
    CHECK(codeOf<void>(first->lockImport()) == ErrorCode::Conflict);  // another holder's import
    // The import lock is a byte of its own: the database's lock stays shared, a migration's needs every holder gone.
    CHECK(second->mode() == LockMode::Shared);
    second->unlockImport();
    CHECK_FALSE(second->holdsImport());
    REQUIRE(first->lockImport().has_value());
    first->unlockImport();

    REQUIRE(second->lockImport().has_value());
    second.reset(); // a holder that goes releases it
    CHECK(first->lockImport().has_value());
}
