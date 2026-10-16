// I2 step 4: retroverse-cli db (ADR-0008 §5): migrate, check and info on the database of --data-dir, as text and as
// JSON, to stdout or --output; their exit codes, and the usage errors that touch no database.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog_db/database.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "support/temp_dir.hpp"

using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const std::filesystem::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

/// A run of the dispatcher logs to stderr; this puts back the plain console logger afterwards.
class LoggingReset {
public:
    LoggingReset() = default;
    ~LoggingReset() {
        rv::core::LogConfig config;
        config.console = false;
        rv::core::initLogging(config);
        rv::core::shutdownLogging();
    }
    LoggingReset(const LoggingReset&) = delete;
    LoggingReset& operator=(const LoggingReset&) = delete;
};

struct RunResult {
    ExitCode code = ExitCode::Ok;
    std::string out;
    std::string err;
};

/// retroverse-cli --data-dir <dir>/data db <arguments...>.
class DbRunner {
public:
    explicit DbRunner(std::string_view prefix) : m_dir(prefix) {}

    RunResult run(const std::vector<std::string>& arguments) const {
        std::vector<std::string> all{"--data-dir", rv::core::pathToUtf8(dataDir()), "db"};
        all.insert(all.end(), arguments.begin(), arguments.end());
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(all, out, err, testEnvironment(m_dir.path()));
        return RunResult{code, out.text(), err.text()};
    }

    [[nodiscard]] std::filesystem::path dataDir() const { return m_dir.file("data"); }
    [[nodiscard]] std::filesystem::path databaseFile() const { return dataDir() / "database" / "retroverse.sqlite"; }
    [[nodiscard]] const rv::test::TempDir& dir() const { return m_dir; }

private:
    rv::test::TempDir m_dir;
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

TEST_CASE("cli/db: migrate makes the database in <user data>/database and reports the versions; again it does nothing",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const DbRunner cli("rv-cli-db-migrate");
    const RunResult first = cli.run({"migrate"});
    CHECK(first.code == ExitCode::Ok);
    CHECK(first.err.empty());
    CHECK(first.out == "Database: " + rv::core::pathToUtf8(cli.databaseFile()) +
                           "\nSchema version: 0 -> 1\nBackup: none (the database held nothing)\n");
    CHECK(std::filesystem::exists(cli.databaseFile()));

    const RunResult again = cli.run({"migrate", "--json"});
    CHECK(again.code == ExitCode::Ok);
    const auto json = nlohmann::json::parse(again.out);
    CHECK(json.at("database") == rv::core::pathToUtf8(cli.databaseFile()));
    CHECK(json.at("fromVersion") == 1);
    CHECK(json.at("toVersion") == 1);
    CHECK(json.at("migrated") == false);
    CHECK(json.at("backup").is_null());
    CHECK(json.at("removedBackups").empty());
    CHECK(cli.run({"migrate"}).out.ends_with("Schema version: 1 (up to date)\n"));
}

TEST_CASE("cli/db: check passes on a migrated database (exit 0) and reports what it finds (exit 1)",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const DbRunner cli("rv-cli-db-check");

    // No database: nothing to check is a problem, and nothing is made.
    const RunResult missing = cli.run({"check"});
    CHECK(missing.code == ExitCode::ProblemsFound);
    CHECK(contains(missing.out, "the database does not exist"));
    CHECK_FALSE(std::filesystem::exists(cli.databaseFile()));
    const auto missingJson = nlohmann::json::parse(cli.run({"check", "--json"}).out);
    CHECK(missingJson.at("exists") == false);
    CHECK(missingJson.at("passed") == false);

    REQUIRE(cli.run({"migrate"}).code == ExitCode::Ok);
    const RunResult healthy = cli.run({"check"});
    CHECK(healthy.code == ExitCode::Ok);
    CHECK(healthy.err.empty());
    for (const std::string_view line :
         {"integrity: ok\n", "foreign-keys: ok\n", "fts5: ok\n", "schema-version: ok\n", "schema: ok\n"}) {
        CHECK(contains(healthy.out, line));
    }
    CHECK(healthy.out.ends_with("No problems found.\n"));
    const auto healthyJson = nlohmann::json::parse(cli.run({"check", "--json"}).out);
    CHECK(healthyJson.at("passed") == true);
    CHECK(healthyJson.at("schemaVersion") == 1);
    CHECK(healthyJson.at("checks").size() == 5);
    CHECK(healthyJson.at("problems").empty());

    // A reference to nothing, as a damaged file might hold it.
    {
        auto database = rv::catalog_db::Database::open(cli.databaseFile());
        REQUIRE(database.has_value());
        REQUIRE(database->writer()
                    .execute("PRAGMA foreign_keys = OFF;"
                             "INSERT INTO idx_media (game, kind, extension) VALUES (42, 0, '.png')")
                    .has_value());
    }
    const RunResult broken = cli.run({"check"});
    CHECK(broken.code == ExitCode::ProblemsFound);
    CHECK(contains(broken.out, "foreign-keys: PROBLEMS\n"));
    CHECK(contains(broken.out, "idx_media"));
    CHECK(broken.out.ends_with("1 problem(s) found.\n"));
    const RunResult brokenJson = cli.run({"check", "--json"});
    CHECK(brokenJson.code == ExitCode::ProblemsFound);
    const auto brokenReport = nlohmann::json::parse(brokenJson.out);
    CHECK(brokenReport.at("problemCount") == 1);
    const auto problems = brokenReport.at("problems");
    REQUIRE(problems.size() == 1);
    CHECK(contains(problems[0].get<std::string>(), "foreign-keys: "));
}

TEST_CASE("cli/db: check reports a damaged database as a problem (exit 1) and writes nothing into it",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const DbRunner cli("rv-cli-db-damaged");
    REQUIRE(cli.run({"migrate"}).code == ExitCode::Ok);
    std::int64_t pageSize = 0;
    std::vector<std::int64_t> roots;
    {
        auto database = rv::catalog_db::Database::open(cli.databaseFile());
        REQUIRE(database.has_value());
        pageSize = database->writer().queryInt("PRAGMA page_size").value().value();
        auto select = database->writer().prepare("SELECT rootpage FROM sqlite_schema WHERE rootpage > 1");
        REQUIRE(select.has_value());
        while (select->step().value()) {
            roots.push_back(select->columnInt(0));
        }
    } // closed: the WAL is copied into the file and removed
    const auto overwrite = [&](std::int64_t offset, const std::string& bytes) {
        std::fstream file(cli.databaseFile(), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file.good());
    };

    // The tables' root pages damaged (the schema's own pages left as they are): the integrity check finds them.
    REQUIRE_FALSE(roots.empty());
    for (const std::int64_t root : roots) {
        overwrite((root - 1) * pageSize, std::string(static_cast<std::size_t>(pageSize), '\xAB'));
    }
    const RunResult pages = cli.run({"check"});
    CHECK(pages.code == ExitCode::ProblemsFound);
    CHECK(pages.err.empty());
    CHECK(contains(pages.out, "integrity: PROBLEMS\n"));
    CHECK(contains(pages.out, " problem(s) found.\n"));

    // The header damaged: SQLite does not take the file for a database; that is the problem found, and the file stays
    // as it is.
    const std::string header = "not a database!!";
    overwrite(0, header);
    const RunResult damaged = cli.run({"check", "--json"});
    CHECK(damaged.code == ExitCode::ProblemsFound);
    CHECK(damaged.err.empty());
    const auto report = nlohmann::json::parse(damaged.out);
    CHECK(report.at("passed") == false);
    CHECK(report.at("schemaVersion").is_null());
    CHECK(report.at("problemCount") == 1);
    REQUIRE(report.at("problems").size() == 1);
    CHECK(contains(report.at("problems")[0].get<std::string>(), "integrity: SQLite cannot read the file"));
    const auto bytes = rv::core::readTextFile(cli.databaseFile());
    REQUIRE(bytes.has_value());
    CHECK(bytes->starts_with(header));
    // Every other command fails on it (exit 3), migrate included: nothing is written over it.
    CHECK(cli.run({"migrate"}).code == ExitCode::RuntimeFailure);
    CHECK(cli.run({"info"}).code == ExitCode::RuntimeFailure);
    CHECK(rv::core::readTextFile(cli.databaseFile()).value() == *bytes);
}

TEST_CASE(
    "cli/db: info shows the path, versions, sizes, metadata and row counts; a missing database is shown, not made",
    "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const DbRunner cli("rv-cli-db-info");
    const RunResult missing = cli.run({"info"});
    CHECK(missing.code == ExitCode::Ok);
    CHECK(contains(missing.out, "Exists: no"));
    CHECK(nlohmann::json::parse(cli.run({"info", "--json"}).out).at("exists") == false);
    CHECK_FALSE(std::filesystem::exists(cli.databaseFile()));

    REQUIRE(cli.run({"migrate"}).code == ExitCode::Ok);
    const RunResult info = cli.run({"info"});
    CHECK(info.code == ExitCode::Ok);
    CHECK(info.out.starts_with("Database: " + rv::core::pathToUtf8(cli.databaseFile()) + "\n"));
    CHECK(contains(info.out, "Schema version: 1 (this program: 1)\n"));
    CHECK(contains(info.out, ", journal mode wal\n"));
    CHECK(contains(info.out, "  lookup_key_rule = 1\n"));
    CHECK(contains(info.out, "  rv_meta              5\n"));
    CHECK(contains(info.out, "  rv_game_identities   0\n"));
    CHECK(info.out.ends_with("Backups: none\n"));

    const auto json = nlohmann::json::parse(cli.run({"info", "--json"}).out);
    CHECK(json.at("exists") == true);
    CHECK(json.at("schemaVersion") == 1);
    CHECK(json.at("programSchemaVersion") == 1);
    CHECK(json.at("journalMode") == "wal");
    CHECK(json.at("sizes").at("database").get<std::uint64_t>() > 0);
    CHECK(json.at("tables").size() == 12);
    CHECK(json.at("meta").at("sort_key_version") == "1");
    CHECK(json.at("backups").empty());
}

TEST_CASE("cli/db: a database newer than the program: migrate fails (exit 3), check reports it (exit 1)",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const DbRunner cli("rv-cli-db-newer");
    REQUIRE(cli.run({"migrate"}).code == ExitCode::Ok);
    {
        auto database = rv::catalog_db::Database::open(cli.databaseFile());
        REQUIRE(database.has_value());
        REQUIRE(database->writer().execute("PRAGMA user_version = 99").has_value());
    }
    const RunResult migrate = cli.run({"migrate"});
    CHECK(migrate.code == ExitCode::RuntimeFailure);
    CHECK(migrate.out.empty());
    CHECK(contains(migrate.err, "retroverse-cli db migrate: conflict: the catalog database has schema version 99"));
    const RunResult check = cli.run({"check"});
    CHECK(check.code == ExitCode::ProblemsFound);
    CHECK(contains(check.out, "schema-version: PROBLEMS\n"));
    CHECK(contains(check.out, "newer than this program's 1"));
    CHECK(contains(cli.run({"info"}).out, "Schema version: 99 (this program: 1)\n"));
}

TEST_CASE("cli/db: --output writes the results to a file; usage errors exit 2 and touch no database",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const DbRunner cli("rv-cli-db-usage");

    const RunResult none = cli.run({});
    CHECK(none.code == ExitCode::UsageError);
    CHECK(none.out.empty());
    CHECK(none.err.starts_with("Usage: retroverse-cli [global options] db <subcommand>"));
    const RunResult help = cli.run({"--help"});
    CHECK(help.code == ExitCode::Ok);
    CHECK(help.out == none.err);
    CHECK(cli.run({"check", "--help"}).code == ExitCode::Ok);

    const std::vector<std::pair<std::vector<std::string>, std::string_view>> invalid{
        {{"frobnicate"}, "retroverse-cli db: unknown subcommand 'frobnicate'"},
        {{"--no-such-option"}, "retroverse-cli db: unknown option '--no-such-option'"},
        {{"migrate", "--bogus"}, "retroverse-cli db migrate: "},
        {{"info", "extra"}, "retroverse-cli db info: "},
        {{"check", "--output="}, "--output"},
        {{"rebuild-index"}, "retroverse-cli db rebuild-index: not implemented yet (I2 step 11)"},
    };
    for (const auto& [arguments, message] : invalid) {
        INFO(arguments.front());
        const RunResult result = cli.run(arguments);
        CHECK(result.code == ExitCode::UsageError);
        CHECK(result.out.empty());
        CHECK(contains(result.err, message));
    }
    CHECK_FALSE(std::filesystem::exists(cli.databaseFile()));

    const std::filesystem::path results = cli.dir().file("results/migrate.json");
    const RunResult migrate = cli.run({"migrate", "--json", "--output", rv::core::pathToUtf8(results)});
    CHECK(migrate.code == ExitCode::Ok);
    CHECK(migrate.out.empty());
    const auto written = rv::core::readTextFile(results);
    REQUIRE(written.has_value());
    CHECK(nlohmann::json::parse(*written).at("toVersion") == 1);
}
