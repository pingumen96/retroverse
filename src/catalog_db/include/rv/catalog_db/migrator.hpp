#pragma once

// Schema migrations of the catalog database (ADR-0008 §5), and the checks and facts `retroverse-cli db` shows.
//
// PRAGMA user_version counts the migrations a database has run. They run forward only, each in one transaction that
// also sets the version, so that a migration that fails leaves the database at the version before it. They run only
// under the exclusive process-level lock (no other Retroverse process has the database open: DatabaseLock), and only
// after a backup of the database as it was (VACUUM INTO a new file in <database folder>/backups whose name holds the
// UTC time and the version it holds; the free space is checked first and the last 3 backups are kept). A run that
// applies several migrations backs up once, before the first: the states after it are the program's own work, which
// that backup reproduces. A database that holds nothing yet is not backed up. A database whose version is newer than
// the program's is refused.
//
// A later migration that changes a table follows SQLite's table rebuild procedure ("Making Other Kinds Of Table Schema
// Changes", https://www.sqlite.org/lang_altertable.html#otheralter), with Migration::rebuildsTables set:
//   1. the migrator turns foreign keys off before the migration's BEGIN (PRAGMA foreign_keys does nothing inside a
//      transaction) and on again after it ends;
//   2. the migration remembers the indexes, triggers and views of table X (sqlite_schema);
//   3. CREATE TABLE new_X with the new definition; INSERT INTO new_X SELECT ... FROM X;
//   4. DROP TABLE X; ALTER TABLE new_X RENAME TO X;
//   5. creates X's indexes, triggers and views again (and views that refer to X, dropped first);
//   6. the migrator runs PRAGMA foreign_key_check before COMMIT (after every migration): a violation rolls it back.
// Never PRAGMA writable_schema: every connection runs in SQLite's defensive mode. Until the M1 gate, schema v1 stays
// open: I2 steps add their tables to v1 (a database made by an earlier build is deleted, never migrated; `db check`
// tells it from the program's schema); from the gate on, every change is a new version.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rv/catalog_db/database.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog_db {

/// One migration: what brings a database from version `version - 1` to `version`.
struct Migration {
    int version = 0;
    /// For logs and errors ("schema v1: identity, index").
    std::string name;
    /// Runs inside the migration's transaction, on the writer; an error rolls the migration back.
    std::function<Status(Connection& connection)> apply;
    /// It rebuilds tables (the procedure above): foreign keys are off around its transaction.
    bool rebuildsTables = false;
};

/// The program's migrations, versions 1 to programSchemaVersion(), from the schema areas: identity and index (I2
/// step 4), search (step 9), launch (step 12).
[[nodiscard]] std::vector<Migration> programMigrations();

/// The schema version this program reads and writes: the last of programMigrations().
[[nodiscard]] int programSchemaVersion();

/// "<database folder>/backups".
[[nodiscard]] std::filesystem::path backupFolderOf(const std::filesystem::path& databaseFile);

/// The backups of `databaseFile` in its backup folder, newest first: the files named
/// "<database file stem>-<UTC time, YYYYMMDDTHHMMSS.mmmZ>-v<version>[-<n>].sqlite" (n from 2 when the time repeats).
[[nodiscard]] std::vector<std::filesystem::path> listBackups(const std::filesystem::path& databaseFile);

/// How migrate() runs; the defaults are the product's.
struct MigrationOptions {
    /// The migrations, versions 1, 2, ... without gaps; empty: programMigrations().
    std::vector<Migration> migrations;
    /// How many backups stay in the backup folder, the newest.
    std::size_t backupsKept = 3;
    /// How long the migration waits for the other processes that have the database open to close it (their holders
    /// of the lock) before Conflict.
    std::chrono::milliseconds lockWait{2000};
    /// How long it waits for another process that migrates the database at the same time to be done (it then finds the
    /// database migrated) before Conflict.
    std::chrono::milliseconds otherMigrationWait{10000};
    /// The free bytes available to this user on the volume of a folder; empty: GetDiskFreeSpaceExW.
    std::function<Expected<std::uint64_t>(const std::filesystem::path& folder)> freeSpace;
    /// The time a backup's name holds; empty: the system clock.
    std::function<std::chrono::system_clock::time_point()> clock;
};

/// What migrate() did.
struct MigrationReport {
    /// The version the database had, and has (equal when nothing ran).
    int fromVersion = 0;
    int toVersion = 0;
    /// The backup written before the first migration; nothing when no migration ran or the database held nothing.
    std::optional<std::filesystem::path> backup;
    /// The older backups removed (backupsKept were kept).
    std::vector<std::filesystem::path> removedBackups;

    [[nodiscard]] bool migrated() const noexcept { return toVersion != fromVersion; }
};

/// Brings `database` to the last version of the migrations (MigrationOptions::migrations). Nothing happens when it is
/// there already. Otherwise the database's lock is made exclusive (waiting up to lockWait for the other holders to
/// close the database; when another holder wants to migrate it at the same time, this one leaves and joins again once
/// that one is done, waiting up to otherMigrationWait), the version read again (another process may have migrated it
/// meanwhile), the database backed up when it holds anything (the free space checked first: twice the database's size
/// and 32 MiB more, room for the backup and for the migration's WAL), the older backups beyond backupsKept removed
/// (never the one just written) with the copies cut short, then each migration runs in its own transaction; the lock
/// is shared again at the end, whatever happened. Errors: Conflict (a database newer than the migrations, written by a
/// newer Retroverse; another process keeps the database open, or migrates it past the wait), InvalidArgument
/// (migrations not numbered 1, 2, ... or without apply), IoError (not enough free space, the backup cannot be
/// written), and the error of a migration that failed (the database stays at the version before it), ValidationFailed
/// when a migration leaves rows that break a foreign key.
[[nodiscard]] Expected<MigrationReport> migrate(Database& database, const MigrationOptions& options = {});

/// Database::open() and then migrate() with the program's migrations, the migration logged: the database every product
/// path opens (the app, the store, retroverse-cli). Errors: those of both.
[[nodiscard]] Expected<Database> openCatalogDatabase(const std::filesystem::path& file,
                                                     const DatabaseOptions& options = {});

/// One check of checkDatabase().
struct DatabaseCheck {
    /// "integrity", "foreign-keys", "fts5", "schema-version", "schema".
    std::string name;
    bool passed = true;
    /// How many problems it found (0 when it passed); `details` lists the first 100 of them.
    std::size_t problems = 0;
    /// The problems found (at most 100), or what was checked when it passed.
    std::vector<std::string> details;
};

/// What checkDatabase() found.
struct DatabaseCheckReport {
    int schemaVersion = 0;
    int programSchemaVersion = 0;
    /// In the order of DatabaseCheck::name's list.
    std::vector<DatabaseCheck> checks;

    [[nodiscard]] bool passed() const noexcept {
        for (const DatabaseCheck& check : checks) {
            if (!check.passed) {
                return false;
            }
        }
        return true;
    }
};

/// Checks `database` without changing it: SQLite's integrity_check; foreign_key_check; FTS5 compiled in, and FTS5's
/// integrity-check of every FTS5 table of the schema; the schema version against the program's; and, at the program's
/// version, the schema itself against the one the program's migrations make (tables, indexes, triggers and views, by
/// their SQL), which tells a database made by an earlier build of an open schema version. A check that SQLite stops on
/// a damaged page (ParseError) found a problem: it is in the report, and the other checks still run. Errors: the checks
/// could not run (IoError and the others SQLite reports).
[[nodiscard]] Expected<DatabaseCheckReport> checkDatabase(Database& database);

/// A table of the database and its rows.
struct TableRows {
    std::string name;
    std::int64_t rows = 0;
};

/// What describeDatabase() reports.
struct DatabaseInfo {
    std::filesystem::path file;
    std::string sqliteVersion;
    int schemaVersion = 0;
    int programSchemaVersion = 0;
    /// PRAGMA journal_mode ("wal").
    std::string journalMode;
    std::int64_t pageSize = 0;
    std::int64_t pageCount = 0;
    std::int64_t freePages = 0;
    /// The files' sizes in bytes: the database, its WAL and its shared memory index (0 when absent).
    std::uint64_t databaseBytes = 0;
    std::uint64_t walBytes = 0;
    std::uint64_t sharedMemoryBytes = 0;
    /// rv_meta's rows, by key.
    std::vector<std::pair<std::string, std::string>> meta;
    /// The tables and virtual tables (FTS5's shadow tables and SQLite's own left out), by name.
    std::vector<TableRows> tables;
    /// listBackups().
    std::vector<std::filesystem::path> backups;
};

/// The facts of `database` (`retroverse-cli db info`), read without changing it. Errors: SQLite's.
[[nodiscard]] Expected<DatabaseInfo> describeDatabase(Database& database);

} // namespace rv::catalog_db
