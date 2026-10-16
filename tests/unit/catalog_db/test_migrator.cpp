// I2 step 4: migrations of the catalog database and schema v1 (ADR-0008 §5): a fresh database migrates to v1 with the
// tables of the identity and index areas; reopening changes nothing; a newer database is refused; a backup with a
// unique name precedes a migration and only the last 3 are kept; a failing migration rolls back and leaves the version
// it had; a migration waits for no other holder of the database's lock and checks the free space first; the schema's
// rules (foreign keys from the index to the state, never cascading; AUTOINCREMENT ids; STRICT tables) and its foreign
// keys enforced; `db check` and `db info`'s views of a database (FTS5 indexes against their content, a check that
// cannot run while another connection writes, files left as they are found). Schema v1 stays open until the M1 gate:
// the tests check step 4's tables and keys among those of later steps, and the schema's rules on every table.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/time.hpp>
#include <rv/core/version.hpp>

#include "catalog_db_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog_db;
using namespace std::chrono_literals;
using rv::ErrorCode;

namespace {

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

template <class T>
ErrorCode codeOf(const rv::Expected<T>& result) {
    return result.has_value() ? ErrorCode::Unknown : result.error().code;
}

/// The tables of schema v1 that I2 step 4 makes (the identity and index areas), by name. Later steps add theirs to v1
/// (search, launch): the tests check that these are among the schema's tables, and the schema's rules on all of them.
const std::vector<std::string> kStep4Tables{
    "idx_disc_groups",     "idx_disc_members",    "idx_games",          "idx_import_issues",
    "idx_media",           "idx_systems",         "rv_game_identities", "rv_identity_aliases",
    "rv_identity_history", "rv_identity_reviews", "rv_libraries",       "rv_meta",
};

/// The rv_meta keys the identity and index areas write, by name.
const std::vector<std::string> kStep4MetaKeys{"casing_table_version", "lookup_key_rule", "schema_created_at",
                                              "schema_created_by", "sort_key_version"};

/// Text rows of a query's first column.
std::vector<std::string> textColumn(Connection& connection, std::string_view sql) {
    auto statement = connection.prepare(sql);
    REQUIRE(statement.has_value());
    std::vector<std::string> values;
    while (statement->step().value()) {
        values.emplace_back(statement->columnText(0));
    }
    return values;
}

/// The schema's ordinary tables, sorted: SQLite's own, virtual tables and their shadow tables (FTS5's) left out.
std::vector<std::string> ordinaryTables(Connection& connection) {
    std::vector<std::string> tables = textColumn(connection, "SELECT name FROM pragma_table_list WHERE schema = 'main' "
                                                             "AND type = 'table' AND name NOT GLOB 'sqlite_*'");
    std::sort(tables.begin(), tables.end());
    return tables;
}

/// True when every value of `part` (sorted) is in `whole` (sorted).
bool includesAll(const std::vector<std::string>& whole, const std::vector<std::string>& part) {
    return std::includes(whole.begin(), whole.end(), part.begin(), part.end());
}

/// The schema as SQLite stores it: type, name and SQL of every object.
std::vector<std::string> schemaDump(Connection& connection) {
    return textColumn(connection,
                      "SELECT type || ' ' || name || ': ' || coalesce(sql, '') FROM sqlite_schema ORDER BY name");
}

/// Migration options for the tests: test migrations up to `last`, no wait for other holders, a fixed clock.
MigrationOptions optionsUpTo(int last, std::chrono::system_clock::time_point now) {
    MigrationOptions options;
    options.migrations = rv::test::migrationsUpTo(last);
    options.lockWait = 0ms;
    options.clock = [now] {
        return now;
    };
    return options;
}

std::chrono::system_clock::time_point at(std::string_view iso) {
    return rv::core::parseUtcIso8601(iso).value();
}

std::string metaValue(Connection& connection, std::string_view key) {
    auto select = connection.prepare("SELECT value FROM rv_meta WHERE key = ?1");
    REQUIRE(select.has_value());
    select->bindText(1, key);
    REQUIRE(select->step().value());
    return std::string(select->columnText(0));
}

} // namespace

TEST_CASE("catalog_db/migrator: a fresh database migrates to schema v1, the identity and index areas, without a backup",
          "[catalog_db]") {
    CHECK(programSchemaVersion() == 1);
    const auto migrations = programMigrations();
    REQUIRE(migrations.size() == 1);
    CHECK(migrations[0].version == 1);
    CHECK(migrations[0].name.starts_with("schema v1: identity, index")); // later areas add their names
    CHECK_FALSE(migrations[0].rebuildsTables);

    const rv::test::TempDir dir("rv-migrate-fresh");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    auto database = Database::open(file);
    REQUIRE(database.has_value());
    CHECK(database->schemaVersion().value() == 0);
    const auto report = migrate(*database);
    REQUIRE(report.has_value());
    CHECK(report->fromVersion == 0);
    CHECK(report->toVersion == 1);
    CHECK(report->migrated());
    CHECK_FALSE(report->backup.has_value()); // nothing to back up yet
    CHECK(report->removedBackups.empty());
    CHECK(listBackups(file).empty());
    CHECK(database->lock().mode() == LockMode::Shared); // shared again after the migration

    Connection& writer = database->writer();
    CHECK(writer.queryInt("PRAGMA application_id").value() == kApplicationId);
    CHECK(includesAll(ordinaryTables(writer), kStep4Tables));
    CHECK(metaValue(writer, "lookup_key_rule") == std::to_string(rv::catalog::kLookupKeyRule));
    CHECK(metaValue(writer, "casing_table_version") == rv::core::casingTableVersion());
    CHECK(metaValue(writer, "sort_key_version") == std::to_string(rv::catalog::kSortKeyVersion));
    CHECK(metaValue(writer, "schema_created_by") == rv::core::versionBanner());
    CHECK(rv::core::parseUtcIso8601(metaValue(writer, "schema_created_at")).has_value());
}

TEST_CASE("catalog_db/migrator: opening a migrated database again changes nothing", "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-again");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    std::vector<std::string> schema;
    std::string created;
    {
        auto database = openCatalogDatabase(file);
        REQUIRE(database.has_value());
        schema = schemaDump(database->writer());
        created = metaValue(database->writer(), "schema_created_at");
    }
    for (int run = 0; run < 2; ++run) {
        auto database = Database::open(file);
        REQUIRE(database.has_value());
        const auto report = migrate(*database);
        REQUIRE(report.has_value());
        CHECK(report->fromVersion == 1);
        CHECK(report->toVersion == 1);
        CHECK_FALSE(report->migrated());
        CHECK_FALSE(report->backup.has_value());
        CHECK(schemaDump(database->writer()) == schema);
        CHECK(metaValue(database->writer(), "schema_created_at") == created);
    }
    CHECK(listBackups(file).empty());
    const auto header = rv::test::readDatabaseHeader(file); // closed: the file holds everything
    REQUIRE(header.has_value());
    CHECK(header->userVersion == 1);
    CHECK(header->applicationId == kApplicationId);
    CHECK(header->writeVersion == 2); // WAL
}

TEST_CASE("catalog_db/migrator: a database newer than the program is refused and left as it is", "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-newer");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    {
        auto database = openCatalogDatabase(file);
        REQUIRE(database.has_value());
        REQUIRE(database->writer().execute("PRAGMA user_version = 99").has_value());
    }
    auto database = Database::open(file); // opened as it is: `db check` and `db info` show it
    REQUIRE(database.has_value());
    const auto refused = migrate(*database);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::Conflict);
    CHECK(contains(refused.error().message, "schema version 99, newer than this program's 1"));
    CHECK(database->schemaVersion().value() == 99);
    CHECK(listBackups(file).empty());
    CHECK(codeOf(openCatalogDatabase(file)) == ErrorCode::Conflict);
    CHECK(codeOf(SqliteCatalogStore::open(*database)) == ErrorCode::Conflict);
}

TEST_CASE("catalog_db/migrator: a backup with a unique name precedes each migration, and only the last 3 are kept",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-backups");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    {
        auto database = openCatalogDatabase(file);
        REQUIRE(database.has_value());
        REQUIRE(database->writer().execute("INSERT INTO rv_meta (key, value) VALUES ('test', 'data')").has_value());
    }
    // Four migrations, an hour apart: each writes a backup of the database as it was, named with the time and the
    // version it holds; the fourth removes the first.
    std::vector<std::filesystem::path> backups;
    for (int target = 2; target <= 5; ++target) {
        auto database = Database::open(file);
        REQUIRE(database.has_value());
        const auto now = at(fmt::format("2026-10-16T{:02}:00:00.000Z", 8 + target));
        const auto report = migrate(*database, optionsUpTo(target, now));
        REQUIRE(report.has_value());
        CHECK(report->fromVersion == target - 1);
        CHECK(report->toVersion == target);
        REQUIRE(report->backup.has_value());
        CHECK(report->backup->filename() ==
              std::filesystem::path(
                  fmt::format("retroverse-20261016T{:02}0000.000Z-v{}.sqlite", 8 + target, target - 1)));
        CHECK(report->removedBackups ==
              (target == 5 ? std::vector<std::filesystem::path>{backups[0]} : std::vector<std::filesystem::path>{}));
        backups.push_back(*report->backup);
        CHECK(database->writer().queryInt(fmt::format("SELECT count(*) FROM test_v{}", target)).value() == 0);
    }
    CHECK(listBackups(file) == std::vector<std::filesystem::path>{backups[3], backups[2], backups[1]});
    CHECK_FALSE(std::filesystem::exists(backups[0]));
    for (int i = 1; i <= 3; ++i) {
        // A backup is the database as it was before its migration, and complete: no copy cut short is left.
        const auto header = rv::test::readDatabaseHeader(backups[i]);
        REQUIRE(header.has_value());
        CHECK(header->userVersion == static_cast<std::uint32_t>(i + 1));
        CHECK(header->applicationId == kApplicationId);
        auto partial = backups[i];
        partial += L".partial";
        CHECK_FALSE(std::filesystem::exists(partial));
    }

    // Two backups at one instant of one version (a migration that fails, run twice): the second name gets a counter.
    MigrationOptions failing = optionsUpTo(5, at("2026-10-16T20:00:00.000Z"));
    failing.migrations.push_back(Migration{
        6, "fails", [](Connection& connection) { return connection.execute("INSERT INTO nowhere VALUES (1)"); }});
    for (int run = 0; run < 2; ++run) {
        auto database = Database::open(file);
        REQUIRE(database.has_value());
        CHECK_FALSE(migrate(*database, failing).has_value());
    }
    const auto latest = listBackups(file);
    REQUIRE(latest.size() == 3);
    CHECK(latest[0].filename() == "retroverse-20261016T200000.000Z-v5-2.sqlite");
    CHECK(latest[1].filename() == "retroverse-20261016T200000.000Z-v5.sqlite");
    CHECK(latest[2] == backups[3]);

    // A copy cut short earlier goes; a backup made while the clock was set back is kept all the same (it was the
    // oldest by its name), and the oldest of the others goes instead.
    const std::filesystem::path partial = backupFolderOf(file) / "retroverse-20261016T070000.000Z-v1.sqlite.partial";
    const std::filesystem::path unrelated = backupFolderOf(file) / "notes.txt";
    REQUIRE(rv::core::writeTextFileAtomic(partial, "cut short").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(unrelated, "the user's").has_value());
    auto database = Database::open(file);
    REQUIRE(database.has_value());
    const auto setBack = migrate(*database, optionsUpTo(6, at("2026-01-01T00:00:00.000Z")));
    REQUIRE(setBack.has_value());
    REQUIRE(setBack->backup.has_value());
    CHECK(std::filesystem::exists(*setBack->backup));
    CHECK(setBack->removedBackups == std::vector<std::filesystem::path>{backups[3]});
    CHECK(listBackups(file) == std::vector<std::filesystem::path>{latest[0], latest[1], *setBack->backup});
    CHECK_FALSE(std::filesystem::exists(partial));
    CHECK(std::filesystem::exists(unrelated)); // only backups of this database are touched
}

TEST_CASE("catalog_db/migrator: a failing migration rolls back and leaves the previous version", "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-rollback");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    auto database = openCatalogDatabase(file);
    REQUIRE(database.has_value());
    Connection& writer = database->writer();

    MigrationOptions options = optionsUpTo(1, at("2026-10-16T12:00:00.000Z"));
    options.migrations.push_back(
        Migration{2, "half done", [](Connection& connection) -> rv::Status {
                      if (auto made = connection.execute("CREATE TABLE half_done (x INTEGER) STRICT;"
                                                         "INSERT INTO rv_meta (key, value) VALUES ('half', 'done')");
                          !made) {
                          return made;
                      }
                      return connection.execute("INSERT INTO nowhere VALUES (1)");
                  }});
    const auto failed = migrate(*database, options);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ErrorCode::InvalidArgument);
    CHECK(contains(failed.error().message, "migration to schema version 2 (half done) failed"));
    CHECK(contains(failed.error().message, "no such table: nowhere"));
    CHECK(database->schemaVersion().value() == 1);
    CHECK(writer.queryInt("SELECT count(*) FROM sqlite_schema WHERE name = 'half_done'").value() == 0);
    CHECK(writer.queryInt("SELECT count(*) FROM rv_meta WHERE key = 'half'").value() == 0);
    CHECK(writer.queryInt("PRAGMA application_id").value() == kApplicationId);
    CHECK(listBackups(file).size() == 1); // the backup preceded it, and stays
    CHECK(database->lock().mode() == LockMode::Shared);
    CHECK_FALSE(writer.inTransaction());

    // A migration that rebuilds tables runs with foreign keys off; rows that break one roll it back all the same.
    MigrationOptions orphans = optionsUpTo(1, at("2026-10-16T13:00:00.000Z"));
    orphans.migrations.push_back(Migration{2, "orphans",
                                           [](Connection& connection) {
                                               return connection.execute(
                                                   "INSERT INTO rv_identity_history (game, at, event) "
                                                   "VALUES (12345, '2026-10-16T13:00:00.000Z', 'test')");
                                           },
                                           true});
    const auto broken = migrate(*database, orphans);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == ErrorCode::ValidationFailed);
    CHECK(contains(broken.error().message, "rv_identity_history"));
    CHECK(database->schemaVersion().value() == 1);
    CHECK(writer.queryInt("SELECT count(*) FROM rv_identity_history").value() == 0);
    CHECK(writer.queryInt("PRAGMA foreign_keys").value() == 1); // on again

    // The migration itself then runs.
    const auto migrated = migrate(*database, optionsUpTo(2, at("2026-10-16T14:00:00.000Z")));
    REQUIRE(migrated.has_value());
    CHECK(migrated->toVersion == 2);
}

TEST_CASE("catalog_db/migrator: no migration while another holder has the database open (another process)",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-lock");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    std::optional<Database> first(openCatalogDatabase(file).value());
    auto second = Database::open(file);
    REQUIRE(second.has_value());

    const auto refused = migrate(*second, optionsUpTo(2, at("2026-10-16T12:00:00.000Z")));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::Conflict);
    CHECK(contains(refused.error().message, "while another Retroverse process has it open"));
    CHECK(second->schemaVersion().value() == 1);
    CHECK(listBackups(file).empty()); // refused before anything was written
    CHECK(second->lock().mode() == LockMode::Shared);

    // A holder that leaves within the migration's wait lets it run.
    MigrationOptions waiting = optionsUpTo(2, at("2026-10-16T12:00:00.000Z"));
    waiting.lockWait = 10s;
    std::thread leaving([&] {
        std::this_thread::sleep_for(200ms);
        first.reset();
    });
    const auto migrated = migrate(*second, waiting);
    leaving.join();
    REQUIRE(migrated.has_value());
    CHECK(migrated->toVersion == 2);
    // The other holders see the new version once they open the database: the migration waited for them to close it.
    auto third = Database::open(file);
    REQUIRE(third.has_value());
    CHECK(third->schemaVersion().value() == 2);
}

TEST_CASE("catalog_db/migrator: two processes that migrate a new database at once: one migrates, the other then finds "
          "it migrated",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-two");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    auto first = Database::open(file);
    auto second = Database::open(file);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    MigrationOptions options;
    options.lockWait = 10s;
    std::optional<rv::Expected<MigrationReport>> firstReport;
    std::optional<rv::Expected<MigrationReport>> secondReport;
    std::thread firstProcess([&] { firstReport.emplace(migrate(*first, options)); });
    std::thread secondProcess([&] { secondReport.emplace(migrate(*second, options)); });
    firstProcess.join();
    secondProcess.join();
    REQUIRE(firstReport.has_value());
    REQUIRE(secondReport.has_value());
    REQUIRE(firstReport->has_value());
    REQUIRE(secondReport->has_value());
    CHECK((*firstReport)->toVersion == 1);
    CHECK((*secondReport)->toVersion == 1);
    CHECK((*firstReport)->migrated() != (*secondReport)->migrated()); // exactly one of them ran the migration
    CHECK(first->schemaVersion().value() == 1);
    CHECK(second->schemaVersion().value() == 1);
    CHECK(first->lock().mode() == LockMode::Shared);
    CHECK(second->lock().mode() == LockMode::Shared);
}

TEST_CASE("catalog_db/migrator: the free space is checked before the backup", "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-space");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    auto database = openCatalogDatabase(file);
    REQUIRE(database.has_value());

    std::filesystem::path asked;
    MigrationOptions options = optionsUpTo(2, at("2026-10-16T12:00:00.000Z"));
    options.freeSpace = [&](const std::filesystem::path& folder) -> rv::Expected<std::uint64_t> {
        asked = folder;
        return 1024;
    };
    const auto refused = migrate(*database, options);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::IoError);
    CHECK(contains(refused.error().message, "not enough free space"));
    CHECK(asked == file.parent_path());
    CHECK(database->schemaVersion().value() == 1);
    CHECK_FALSE(std::filesystem::exists(backupFolderOf(file)));

    options.freeSpace = [](const std::filesystem::path&) -> rv::Expected<std::uint64_t> {
        return rv::makeError(ErrorCode::Unavailable, "no such volume");
    };
    CHECK(codeOf(migrate(*database, options)) == ErrorCode::Unavailable);

    options.freeSpace = {}; // the system's: this volume has room
    CHECK(migrate(*database, options).has_value());
}

TEST_CASE("catalog_db/migrator: migrations are numbered 1, 2, ... without gaps, each with something to apply",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-migrate-numbers");
    auto database = Database::open(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    const auto apply = [](Connection&) {
        return rv::ok();
    };
    MigrationOptions options;
    options.migrations = {Migration{2, "two", apply}};
    CHECK(codeOf(migrate(*database, options)) == ErrorCode::InvalidArgument);
    options.migrations = {Migration{1, "one", apply}, Migration{3, "three", apply}};
    CHECK(codeOf(migrate(*database, options)) == ErrorCode::InvalidArgument);
    options.migrations = {Migration{1, "one", {}}};
    CHECK(codeOf(migrate(*database, options)) == ErrorCode::InvalidArgument);
    CHECK(database->schemaVersion().value() == 0);
}

TEST_CASE("catalog_db/schema: foreign keys point from the index to the state tables and never cascade; ids shown to "
          "users are AUTOINCREMENT; every table is STRICT",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-schema-rules");
    auto database = openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    Connection& writer = database->writer();
    // Every area's tables keep the rules, those of the later steps too.
    const std::vector<std::string> tables = ordinaryTables(writer);
    CHECK(includesAll(tables, kStep4Tables));
    for (const std::string& table : tables) {
        INFO(table);
        auto keys = writer.prepare("SELECT \"table\", on_update, on_delete FROM pragma_foreign_key_list(?1)");
        REQUIRE(keys.has_value());
        keys->bindText(1, table);
        int count = 0;
        while (keys->step().value()) {
            ++count;
            const std::string parent(keys->columnText(0));
            INFO("refers to " << parent);
            CHECK(parent.starts_with("rv_")); // nothing refers to the index: rebuilding it never touches the state
            CHECK(keys->columnText(1) == "NO ACTION");
            CHECK(keys->columnText(2) == "NO ACTION");
        }
        if (table.starts_with("idx_")) {
            CHECK(count > 0); // every index table hangs off the state
        }
        CHECK(writer.queryInt(fmt::format("SELECT strict FROM pragma_table_list WHERE name = '{}'", table)).value() ==
              1);
    }
    for (const std::string_view table : {"rv_identity_history", "rv_identity_aliases", "rv_identity_reviews"}) {
        INFO(table);
        CHECK(contains(
            writer.queryText(fmt::format("SELECT sql FROM sqlite_schema WHERE name = '{}'", table)).value().value(),
            "AUTOINCREMENT"));
    }
    // Game rows take the GameId as their INTEGER PRIMARY KEY (the rowid).
    for (const std::string_view table : {"rv_game_identities", "idx_games"}) {
        INFO(table);
        CHECK(writer.queryText(fmt::format("SELECT type FROM pragma_table_info('{}') WHERE pk = 1", table))
                  .value()
                  .value() == "INTEGER");
    }
}

TEST_CASE("catalog_db/schema: foreign keys are enforced, and a state row cannot go while index rows refer to it",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-schema-keys");
    auto database = openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    Connection& writer = database->writer();

    const auto orphan = writer.execute("INSERT INTO idx_media (game, kind, extension) VALUES (42, 0, '.png')");
    REQUIRE_FALSE(orphan.has_value());
    CHECK(orphan.error().code == ErrorCode::InvalidArgument);
    CHECK(contains(orphan.error().message, "FOREIGN KEY constraint failed"));

    REQUIRE(writer
                .execute("INSERT INTO rv_libraries (id, adapter, esde_home) VALUES ('esde', 'esde', 'C:/ES-DE');"
                         "INSERT INTO rv_game_identities (id, library, system, lookup_key, relative_path, key_rule, "
                         "key_casing, present, first_seen) VALUES (42, 'esde', 'gb', 'A.GB', 'a.gb', 1, 'x', 1, 'now');"
                         "INSERT INTO idx_media (game, kind, extension) VALUES (42, 0, '.png')")
                .has_value());
    CHECK(codeOf<void>(writer.execute("INSERT INTO rv_game_identities (id, library, system, lookup_key, relative_path, "
                                      "key_rule, key_casing, present, first_seen) "
                                      "VALUES (43, 'nolibrary', 'gb', 'B.GB', 'b.gb', 1, 'x', 1, 'now')")) ==
          ErrorCode::InvalidArgument);
    // No cascade: the identity stays while an index row refers to it, and the index row with it.
    CHECK(codeOf<void>(writer.execute("DELETE FROM rv_game_identities WHERE id = 42")) == ErrorCode::InvalidArgument);
    CHECK(writer.queryInt("SELECT count(*) FROM rv_game_identities").value() == 1);
    CHECK(writer.queryInt("SELECT count(*) FROM idx_media").value() == 1);
    // Index rows go freely.
    REQUIRE(writer.execute("DELETE FROM idx_media").has_value());
    CHECK(writer.execute("DELETE FROM rv_game_identities WHERE id = 42").has_value());
    // The schema's checks hold too.
    CHECK(codeOf<void>(writer.execute("INSERT INTO rv_libraries (id, adapter, esde_home) VALUES ('Not A Slug', 'esde', "
                                      "'C:/ES-DE')")) == ErrorCode::ValidationFailed);
}

TEST_CASE("catalog_db/check: a migrated database passes every check; broken references, a schema that is not the "
          "program's and an older version are problems",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-check");
    auto database = openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    Connection& writer = database->writer();

    const auto healthy = checkDatabase(*database);
    REQUIRE(healthy.has_value());
    CHECK(healthy->passed());
    CHECK(healthy->schemaVersion == 1);
    CHECK(healthy->programSchemaVersion == 1);
    std::vector<std::string> names;
    for (const DatabaseCheck& check : healthy->checks) {
        names.push_back(check.name);
        CHECK_FALSE(check.details.empty());
    }
    CHECK(names == std::vector<std::string>{"integrity", "foreign-keys", "fts5", "schema-version", "schema"});

    const auto failedCheck = [&](std::string_view name) -> std::optional<DatabaseCheck> {
        const auto report = checkDatabase(*database);
        REQUIRE(report.has_value());
        CHECK_FALSE(report->passed());
        for (const DatabaseCheck& check : report->checks) {
            if (check.name == name && !check.passed) {
                return check;
            }
        }
        return std::nullopt;
    };

    // A reference to nothing, written with foreign keys off as a damaged file might hold it.
    REQUIRE(writer
                .execute("PRAGMA foreign_keys = OFF;"
                         "INSERT INTO idx_media (game, kind, extension) VALUES (42, 0, '.png');"
                         "PRAGMA foreign_keys = ON")
                .has_value());
    const auto keys = failedCheck("foreign-keys");
    REQUIRE(keys.has_value());
    REQUIRE(keys->details.size() == 1);
    CHECK(contains(keys->details[0], "idx_media"));
    CHECK(contains(keys->details[0], "rv_game_identities"));
    REQUIRE(writer.execute("DELETE FROM idx_media").has_value());

    // Objects the program does not make, or no longer: a database made by an earlier build of an open schema version.
    REQUIRE(writer.execute("CREATE TABLE leftover (x INTEGER) STRICT; DROP INDEX idx_games_system").has_value());
    const auto schema = failedCheck("schema");
    REQUIRE(schema.has_value());
    CHECK(std::find(schema->details.begin(), schema->details.end(),
                    "table leftover is not part of this program's schema version 1") != schema->details.end());
    CHECK(std::find(schema->details.begin(), schema->details.end(), "index idx_games_system is missing") !=
          schema->details.end());
    REQUIRE(writer.execute("DROP TABLE leftover; CREATE INDEX idx_games_system ON idx_games (library, system)")
                .has_value());
    CHECK(checkDatabase(*database).value().passed());

    // An FTS5 table is checked by FTS5's own integrity-check (one more than the schema's own, if any).
    const auto schemaFts5Tables = writer
                                      .queryInt("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND "
                                                "sql LIKE 'CREATE VIRTUAL TABLE % USING fts5%'")
                                      .value()
                                      .value();
    REQUIRE(writer
                .execute("CREATE VIRTUAL TABLE search_test USING fts5(title);"
                         "INSERT INTO search_test (title) VALUES ('Pocket Puzzle')")
                .has_value());
    const auto withFts = checkDatabase(*database);
    REQUIRE(withFts.has_value());
    CHECK(withFts->checks[2].passed);
    CHECK(withFts->checks[2].details ==
          std::vector<std::string>{fmt::format("compiled in; {} FTS5 table(s) checked", schemaFts5Tables + 1)});
    REQUIRE(writer.execute("DROP TABLE search_test").has_value());

    // An index over external content (as the search index over the index tables) whose content changed behind its
    // back: FTS5's check with rank 1 compares the two.
    REQUIRE(writer
                .execute("CREATE TABLE search_content (id INTEGER PRIMARY KEY, title TEXT NOT NULL) STRICT;"
                         "CREATE VIRTUAL TABLE search_drift USING fts5(title, content = 'search_content', "
                         "content_rowid = 'id');"
                         "INSERT INTO search_content (id, title) VALUES (1, 'Pocket Puzzle');"
                         "INSERT INTO search_drift (rowid, title) VALUES (1, 'Pocket Puzzle')")
                .has_value());
    CHECK(checkDatabase(*database).value().checks[2].passed);
    REQUIRE(writer.execute("UPDATE search_content SET title = 'Harbor Racer' WHERE id = 1").has_value());
    const auto drift = failedCheck("fts5");
    REQUIRE(drift.has_value());
    CHECK(drift->problems == 1);
    REQUIRE(drift->details.size() == 1);
    CHECK(contains(drift->details[0], "search_drift: the index is damaged or differs from its content"));
    REQUIRE(writer.execute("DROP TABLE search_drift; DROP TABLE search_content").has_value());

    // An older schema version: migrate it; its schema is not compared.
    REQUIRE(writer.execute("PRAGMA user_version = 0").has_value());
    const auto version = failedCheck("schema-version");
    REQUIRE(version.has_value());
    CHECK(contains(version->details[0], "older than this program's 1"));
    const auto older = checkDatabase(*database).value();
    CHECK(older.checks[4].passed);
    CHECK(contains(older.checks[4].details[0], "not compared"));
}

TEST_CASE("catalog_db/check: pages SQLite cannot read are problems the checks find, and every check still runs",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-check-damaged");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    std::int64_t pageSize = 0;
    std::vector<std::int64_t> roots;
    {
        auto database = openCatalogDatabase(file);
        REQUIRE(database.has_value());
        pageSize = database->writer().queryInt("PRAGMA page_size").value().value();
        auto select = database->writer().prepare("SELECT rootpage FROM sqlite_schema WHERE rootpage > 1");
        REQUIRE(select.has_value());
        while (select->step().value()) {
            roots.push_back(select->columnInt(0));
        }
    } // closed: the WAL is copied into the file and removed
    REQUIRE_FALSE(roots.empty());

    // The root page of every table and index overwritten, the schema's own pages left as they are: the tables' B-trees
    // are garbage.
    {
        std::fstream bytes(file, std::ios::in | std::ios::out | std::ios::binary);
        const std::string garbage(static_cast<std::size_t>(pageSize), '\xAB');
        for (const std::int64_t root : roots) {
            bytes.seekp(static_cast<std::streamoff>((root - 1) * pageSize));
            bytes.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
        }
        REQUIRE(bytes.good());
    }
    auto database = Database::open(file);
    REQUIRE(database.has_value());
    const auto report = checkDatabase(*database);
    REQUIRE(report.has_value());
    CHECK_FALSE(report->passed());
    REQUIRE(report->checks.size() == 5);
    CHECK(report->checks[0].name == "integrity");
    CHECK_FALSE(report->checks[0].passed);
    // A problem per damaged B-tree at least, a line each (SQLite's rows hold several, after a "*** in database" line).
    CHECK(report->checks[0].problems >= roots.size());
    CHECK(report->checks[0].problems >= report->checks[0].details.size());
    for (const std::string& detail : report->checks[0].details) {
        INFO(detail);
        CHECK_FALSE(detail.starts_with("***"));
        CHECK_FALSE(contains(detail, "\n"));
    }
}

TEST_CASE("catalog_db/check: a check that cannot run while another connection writes fails (Conflict) instead of "
          "reporting a damaged FTS5 index",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-check-busy");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    {
        auto database = openCatalogDatabase(file);
        REQUIRE(database.has_value());
        REQUIRE(database->writer()
                    .execute("CREATE VIRTUAL TABLE search_test USING fts5(title);"
                             "INSERT INTO search_test (title) VALUES ('Pocket Puzzle')")
                    .has_value());
    }
    DatabaseOptions briefWait;
    briefWait.busyTimeout = 100ms;
    auto checking = Database::open(file, briefWait);
    auto writing = Database::open(file); // another process, in a write transaction
    REQUIRE(checking.has_value());
    REQUIRE(writing.has_value());
    auto transaction = Transaction::begin(writing->writer(), Transaction::Mode::Immediate);
    REQUIRE(transaction.has_value());

    // FTS5's check takes the write lock for an instant: past the busy timeout it could not run, which is no problem
    // found (`db check` exits 3, not 1).
    const auto busy = checkDatabase(*checking);
    REQUIRE_FALSE(busy.has_value());
    CHECK(busy.error().code == ErrorCode::Conflict);
    CHECK(contains(busy.error().message, "cannot run FTS5's integrity-check of search_test"));
    transaction->rollback();
    const auto checked = checkDatabase(*checking);
    REQUIRE(checked.has_value());
    CHECK(checked->checks[2].passed);
}

TEST_CASE(
    "catalog_db/check: check and info opened without the switch to WAL leave a database in rollback journal mode, "
    "and an empty file, as they find them",
    "[catalog_db]") {
    const rv::test::TempDir dir("rv-check-unchanged");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    const std::filesystem::path restored = dir.file("restored/retroverse.sqlite");
    {
        auto database = openCatalogDatabase(file);
        REQUIRE(database.has_value());
        REQUIRE(database->writer()
                    .execute("INSERT INTO rv_libraries (id, adapter, esde_home) VALUES ('esde', 'esde', 'C:/ES-DE')")
                    .has_value());
        REQUIRE(database->backupTo(restored).has_value()); // a backup, as a user would put it back: rollback journal
    }
    REQUIRE(rv::test::readDatabaseHeader(restored).has_value());
    REQUIRE(rv::test::readDatabaseHeader(restored)->writeVersion == 1);
    const std::string bytes = rv::core::readTextFile(restored).value();

    DatabaseOptions asFound;
    asFound.create = false;
    asFound.switchToWal = false;
    {
        auto database = Database::open(restored, asFound);
        REQUIRE(database.has_value());
        const auto report = checkDatabase(*database);
        REQUIRE(report.has_value());
        CHECK(report->passed());
        const auto info = describeDatabase(*database);
        REQUIRE(info.has_value());
        CHECK(info->journalMode == "delete");
        CHECK(info->walBytes == 0);
    }
    CHECK(rv::core::readTextFile(restored).value() == bytes);
    CHECK_FALSE(std::filesystem::exists(dir.file("restored/retroverse.sqlite-wal")));
    CHECK_FALSE(std::filesystem::exists(dir.file("restored/retroverse.sqlite-journal")));
    // The product's paths switch it to WAL when they open it.
    REQUIRE(openCatalogDatabase(restored).has_value());
    CHECK(rv::test::readDatabaseHeader(restored)->writeVersion == 2);

    // An empty file is no database yet: its version is older than the program's, and it stays empty.
    const std::filesystem::path empty = dir.file("empty/retroverse.sqlite");
    REQUIRE(rv::core::writeTextFileAtomic(empty, "").has_value());
    {
        auto database = Database::open(empty, asFound);
        REQUIRE(database.has_value());
        const auto report = checkDatabase(*database);
        REQUIRE(report.has_value());
        CHECK_FALSE(report->passed());
        CHECK(report->schemaVersion == 0);
        REQUIRE(describeDatabase(*database).has_value());
    }
    CHECK(std::filesystem::file_size(empty) == 0);
}

TEST_CASE("catalog_db/info: the file, versions, sizes, metadata, row counts and backups", "[catalog_db]") {
    const rv::test::TempDir dir("rv-info");
    const std::filesystem::path file = rv::test::databaseFileIn(dir);
    auto database = openCatalogDatabase(file);
    REQUIRE(database.has_value());
    REQUIRE(database->writer()
                .execute("INSERT INTO rv_libraries (id, adapter, esde_home) VALUES ('esde', 'esde', "
                         "'C:/ES-DE')")
                .has_value());

    const auto info = describeDatabase(*database);
    REQUIRE(info.has_value());
    CHECK(info->file == file);
    CHECK(info->sqliteVersion == sqliteVersion());
    CHECK(info->schemaVersion == 1);
    CHECK(info->programSchemaVersion == 1);
    CHECK(info->journalMode == "wal");
    CHECK(info->pageSize > 0);
    CHECK(info->pageCount > 0);
    CHECK(info->freePages >= 0);
    CHECK(info->databaseBytes > 0);
    CHECK(info->sharedMemoryBytes > 0); // open: the WAL index exists
    std::vector<std::string> tables;
    for (const TableRows& table : info->tables) {
        tables.push_back(table.name);
        if (std::find(kStep4Tables.begin(), kStep4Tables.end(), table.name) == kStep4Tables.end()) {
            continue; // a later step's
        }
        INFO(table.name);
        if (table.name == "rv_meta") {
            CHECK(table.rows >= static_cast<std::int64_t>(kStep4MetaKeys.size()));
        } else {
            CHECK(table.rows == (table.name == "rv_libraries" ? 1 : 0));
        }
    }
    CHECK(std::is_sorted(tables.begin(), tables.end()));
    CHECK(includesAll(tables, kStep4Tables));
    std::vector<std::string> keys;
    for (const auto& [key, value] : info->meta) {
        keys.push_back(key);
    }
    CHECK(std::is_sorted(keys.begin(), keys.end()));
    CHECK(includesAll(keys, kStep4MetaKeys));
    CHECK(info->backups.empty());

    const auto report = migrate(*database, optionsUpTo(2, at("2026-10-16T12:00:00.000Z")));
    REQUIRE(report.has_value());
    const auto after = describeDatabase(*database);
    REQUIRE(after.has_value());
    CHECK(after->schemaVersion == 2);
    CHECK(after->backups == std::vector<std::filesystem::path>{*report->backup});
}
