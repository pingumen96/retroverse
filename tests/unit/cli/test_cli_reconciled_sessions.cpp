// Reconciliation (I2 step 11b) meeting launches and sessions (I2 step 12), on the real code paths: retroverse-cli import,
// identities, launch, assign, sessions and db through the dispatcher, on the standard ES-DE fixture imported into a user
// data directory whose emulators.json holds profiles of the fake emulator (tests/fake_emulator; RV_LAUNCH_ALLOWLIST
// lists it for each test's duration, support/scoped_environment.hpp). Step 12's launch tables are the state rows that
// step 11b's putAlias() re-points, found from the schema: the moved-back case of ADR-0008 §4 (the holder's session and
// per-game choice follow the moved identity, whose own choice wins; the holder's GameId launches, dry-runs and lists
// sessions as the moved identity's; `identities show` reports the alias), then `db rebuild-index` keeping every state
// row with `db check` passing; SC-07 (a moved identity keeps its GameId, so its sessions, choice and verification need no
// re-pointing and follow it to its new path); a review resolved with `identities resolve --with` whose chosen identity
// has sessions, a choice and a verification (they follow, the other candidate keeps its own). Sessions that never
// started (blocked) report no exit kind (I2 step 11b-12i).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/identity.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>

#include "../catalog_db/import_test_support.hpp"
#include "support/file_system.hpp"
#include "support/fixture.hpp"
#include "support/scoped_environment.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const fs::path& executableDir) {
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

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::string utf8(const fs::path& path) {
    return rv::core::pathToUtf8(fs::path(path).make_preferred());
}

/// The build's fake emulator, with its DLLs beside it.
fs::path buildFake() {
    return rv::test::testProgramsDir() / "rv_fake_emulator.exe";
}

/// An emulator profile serving gb, without a window timeout (the fake runs windowless).
json profile(std::string_view id, const fs::path& executable, std::vector<std::string> arguments) {
    return json{{"id", id},
                {"executable", utf8(executable)},
                {"arguments", std::move(arguments)},
                {"windowTimeoutSeconds", 0},
                {"systems", {{"gb", json::object()}}}};
}

/// The GameId text of a path of gb as a first import gives it (`counter`: the tuple's counter of ADR-0008 §4).
std::string gameIdOf(std::string_view relativePath, std::uint32_t counter = 0) {
    const auto id = rv::catalog::stableGameId(rv::catalog::LibraryId(), rv::catalog::SystemName::parse("gb").value(),
                                              rv::catalog::makeLookupKey(relativePath).value().key(), counter);
    REQUIRE(id.has_value());
    return id->toString();
}

/// The standard fixture recorded as the library of a user data directory outside it, imported, with profiles of the
/// fake emulator in its emulators.json:
///  - fake-return: runs 2.6 s, past the startup grace, then exits 0 (returned normally: verified);
///  - fake-quick: exits 0 at once (never launched here: a choice to tell apart from fake-return);
///  - fake-missing: an executable that is not there (a launch is blocked before it starts: emulator not found).
class Setup {
public:
    explicit Setup(std::string_view prefix)
        : m_dir(prefix), m_fixture(rv::test::Fixture::materialize("esde/standard").value()), m_data(m_dir.file("data")) {
        rv::test::settleFolderTimes(m_fixture.path("roms")); // imports a moment apart compare exactly
        rv::catalog::LibraryProfile library;
        library.esdeHome = m_fixture.path("ES-DE");
        library.installFolder = m_fixture.path("Programs/ES-DE");
        REQUIRE(rv::esde::writeLibraryFile(m_data / "config" / "library.json", library, rv::esde::ExistingFile::Keep)
                    .has_value());
        REQUIRE(fs::exists(buildFake()));
        json profiles = json::array();
        profiles.push_back(profile("fake-return", buildFake(),
                                   {"--sleep-ms", "2600", "--exit-timeout-ms", "30000", "--", "{rom}"}));
        profiles.push_back(profile("fake-quick", buildFake(), {"--", "{rom}"}));
        profiles.push_back(profile("fake-missing", m_dir.file("missing/fake-missing.exe"), {"--", "{rom}"}));
        const json document{{"schemaVersion", 1}, {"profiles", std::move(profiles)}};
        REQUIRE(rv::core::writeTextFileAtomic(m_data / "config" / "emulators.json", document.dump(2)).has_value());
        const RunResult imported = run({"import"});
        INFO(imported.err);
        REQUIRE(imported.code == ExitCode::Ok);
    }

    [[nodiscard]] RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(m_data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(m_dir.path()));
        return RunResult{code, out.text(), err.text()};
    }
    /// run() REQUIREd to exit `code`.
    void ok(std::vector<std::string> arguments, ExitCode code = ExitCode::Ok) const {
        const RunResult result = run(arguments);
        INFO(fmt::format("{}\n{}", result.out, result.err));
        REQUIRE(result.code == code);
    }
    /// run() with --json, REQUIREd to exit `code` (nothing: any code), its output parsed.
    [[nodiscard]] json runJson(std::vector<std::string> arguments,
                               std::optional<ExitCode> code = ExitCode::Ok) const {
        arguments.push_back("--json");
        const RunResult result = run(arguments);
        INFO(fmt::format("{}\n{}", result.out, result.err));
        if (code) {
            REQUIRE(result.code == *code);
        }
        REQUIRE_FALSE(result.out.empty());
        return json::parse(result.out);
    }

    [[nodiscard]] fs::path rom(std::string_view relative) const {
        return m_fixture.path("roms") / rv::core::pathFromUtf8(relative);
    }
    /// Moves a ROM of the fixture, its last write time kept (as a move keeps it), the folders above it made.
    void moveRom(std::string_view from, std::string_view to) const {
        fs::create_directories(rom(to).parent_path());
        fs::rename(rom(from), rom(to));
        rv::test::settleFolderTimes(m_fixture.path("roms"));
    }
    /// Copies a ROM of the fixture with its last write time.
    void copyRom(std::string_view from, std::string_view to) const {
        fs::create_directories(rom(to).parent_path());
        fs::copy_file(rom(from), rom(to));
        REQUIRE(rv::test::setLastWriteTimeAnyLength(rom(to), fs::last_write_time(rom(from))).has_value());
        rv::test::settleFolderTimes(m_fixture.path("roms"));
    }
    /// Deletes a ROM of the fixture.
    void removeRom(std::string_view relative) const {
        REQUIRE(fs::remove(rom(relative)));
        rv::test::settleFolderTimes(m_fixture.path("roms")); // the folder's time is a Folder entry's file fact
    }

    /// Each row of `sql` as one line of text (columns separated by " | ", NULL as "NULL", blobs as their size), read
    /// through a reader of the database.
    [[nodiscard]] std::vector<std::string> rows(std::string_view sql) const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(m_data / "database" / "retroverse.sqlite", options);
        REQUIRE(database.has_value());
        auto reader = database->openReader();
        REQUIRE(reader.has_value());
        auto statement = reader->prepare(sql);
        REQUIRE(statement.has_value());
        std::vector<std::string> lines;
        while (statement->step().value()) {
            std::string line;
            for (int column = 0; column < statement->columnCount(); ++column) {
                if (column > 0) {
                    line += " | ";
                }
                switch (statement->columnType(column)) {
                case rv::catalog_db::ColumnType::Null:
                    line += "NULL";
                    break;
                case rv::catalog_db::ColumnType::Integer:
                    line += std::to_string(statement->columnInt(column));
                    break;
                case rv::catalog_db::ColumnType::Blob:
                    line += fmt::format("blob of {} bytes", statement->columnBlob(column).size());
                    break;
                default:
                    line += std::string(statement->columnText(column));
                    break;
                }
            }
            lines.push_back(std::move(line));
        }
        return lines;
    }
    /// The first column of the only row of `sql`.
    [[nodiscard]] std::string value(std::string_view sql) const {
        const std::vector<std::string> found = rows(sql);
        INFO(sql);
        REQUIRE(found.size() == 1);
        return found.front();
    }
    /// The GameId's integer value, as the tables store it.
    [[nodiscard]] static std::string stored(std::string_view gameId) {
        return std::to_string(rv::catalog::GameId::parse(gameId).value().value());
    }
    /// The games (GameId texts) the play sessions name, by session id.
    [[nodiscard]] std::vector<std::string> sessionGames() const {
        std::vector<std::string> games;
        for (const std::string& value : rows("SELECT game FROM rv_play_sessions ORDER BY id")) {
            games.push_back(rv::catalog::GameId::fromValue(std::stoull(value)).value().toString());
        }
        return games;
    }
    /// Every state table (rv_*) but the import records (rv_imports, where `db rebuild-index` records itself), each row
    /// as text in a stable order: what `db rebuild-index` must leave alone.
    [[nodiscard]] std::vector<std::string> stateRows() const {
        std::vector<std::string> all;
        for (const std::string& table : rows("SELECT name FROM sqlite_schema WHERE type = 'table' AND name GLOB 'rv_*' "
                                             "AND name <> 'rv_imports' ORDER BY name")) {
            for (std::string& row : rows(fmt::format("SELECT * FROM \"{}\" ORDER BY 1, 2", table))) {
                all.push_back(table + ": " + row);
            }
        }
        return all;
    }

private:
    rv::test::TempDir m_dir;
    rv::test::Fixture m_fixture;
    fs::path m_data;
};

/// The GameIds of a `sessions --json` listing, newest first.
std::vector<std::string> listedGames(const json& listing) {
    std::vector<std::string> games;
    for (const json& session : listing.at("sessions")) {
        games.push_back(session.at("game").get<std::string>());
    }
    return games;
}

/// The session ids of a `sessions --json` listing, newest first.
std::vector<std::int64_t> listedIds(const json& listing) {
    std::vector<std::int64_t> ids;
    for (const json& session : listing.at("sessions")) {
        ids.push_back(session.at("id").get<std::int64_t>());
    }
    return ids;
}

} // namespace

TEST_CASE("cli/reconciled sessions: a file moved back makes the holder an alias; its session and choice follow the "
          "moved identity, whose own choice wins; db rebuild-index keeps every state row",
          "[cli][sessions][identities]") {
    const LoggingReset loggingReset;
    const Setup setup("rv-cli-reconciled-back");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    setup.ok({"identities", "hash"}); // quick hashes stored: the moves pair by them
    setup.ok({"assign", "gb", "fake-quick"}); // a system choice too, a state row of its own
    // What putAlias() re-points, read from the schema as the store reads it: every state table's single-column foreign
    // key into the identities, step 12's launch tables among them.
    CHECK(setup.rows("SELECT m.name || '.' || f.\"from\" FROM sqlite_schema AS m JOIN pragma_foreign_key_list(m.name) "
                     "AS f WHERE m.type = 'table' AND substr(m.name, 1, 3) = 'rv_' AND f.\"table\" = "
                     "'rv_game_identities' AND f.seq = 0 AND NOT EXISTS (SELECT 1 FROM pragma_foreign_key_list(m.name) "
                     "AS g WHERE g.id = f.id AND g.seq > 0) ORDER BY 1") ==
          std::vector<std::string>{"rv_identity_aliases.game", "rv_identity_history.game", "rv_identity_reviews.game",
                                   "rv_launch_game_choices.game", "rv_launch_verifications.game",
                                   "rv_play_sessions.game"});

    // Tiny Tennis moved to a sub-folder, its gamelist entry left at the old path: the entry gets a new identity, the
    // holder of that path (never seen with a file).
    const std::string tiny = gameIdOf("Tiny Tennis (Europe).gb");
    const std::string holder = gameIdOf("Tiny Tennis (Europe).gb", 1);
    setup.moveRom("gb/Tiny Tennis (Europe).gb", "gb/Moved/Tiny Tennis (Europe).gb");
    const json away = setup.runJson({"import"});
    REQUIRE(away.at("moved").size() == 1);
    CHECK(away.at("moved")[0].at("id") == tiny);
    CHECK(away.at("moved")[0].at("alias").is_null());
    CHECK(setup.value(fmt::format("SELECT relative_path FROM rv_game_identities WHERE id = {}",
                                  Setup::stored(holder))) == "Tiny Tennis (Europe).gb");

    // The holder: a per-game choice and a session (its file is not there: blocked before the launch, no exit).
    setup.ok({"assign", "--game", holder, "fake-missing"});
    const json blocked = setup.runJson({"launch", "--game", holder, "--wait"}, ExitCode::ProblemsFound);
    CHECK(blocked.at("session").at("game") == holder);
    CHECK(blocked.at("session").at("outcome") == "blocked");
    CHECK(blocked.at("session").at("readiness") == "rom-missing");
    CHECK(blocked.at("session").at("exitCode").is_null());
    CHECK(blocked.at("session").at("exitKind").is_null());
    const std::int64_t holderSession = blocked.at("session").at("id").get<std::int64_t>();
    // The moved identity: a choice of its own and a session that returns normally (a verification).
    setup.ok({"assign", "--game", tiny, "fake-return"});
    const json played = setup.runJson({"launch", "--game", tiny, "--wait"});
    CHECK(played.at("session").at("outcome") == "returned");
    CHECK(played.at("session").at("exitKind") == "success");
    CHECK(played.at("verified") == true);
    CHECK(played.at("path") == "Moved/Tiny Tennis (Europe).gb");
    const std::int64_t tinySession = played.at("session").at("id").get<std::int64_t>();
    CHECK(setup.rows("SELECT count(*) FROM rv_launch_game_choices") == std::vector<std::string>{"2"});

    // Moved back where its gamelist entry stayed: the file pairs with its identity (quick hash, same name), and the
    // holder becomes its alias (ADR-0008 §4's moved-back rule).
    setup.moveRom("gb/Moved/Tiny Tennis (Europe).gb", "gb/Tiny Tennis (Europe).gb");
    const json back = setup.runJson({"import"});
    REQUIRE(back.at("moved").size() == 1);
    CHECK(back.at("moved")[0].at("id") == tiny);
    CHECK(back.at("moved")[0].at("alias") == holder);
    CHECK(back.at("moved")[0].at("to") == "Tiny Tennis (Europe).gb");
    CHECK(setup.rows(fmt::format("SELECT id FROM rv_game_identities WHERE id = {}", Setup::stored(holder))).empty());

    // Step 12's rows that named the holder name the moved identity: its session moved; the per-game choice table, one
    // row per game, kept the moved identity's own row and dropped the holder's.
    CHECK(setup.sessionGames() == std::vector<std::string>{tiny, tiny});
    CHECK(setup.rows("SELECT game, kind, value FROM rv_launch_game_choices") ==
          std::vector<std::string>{fmt::format("{} | profile | fake-return", Setup::stored(tiny))});
    CHECK(setup.rows("SELECT game, session FROM rv_launch_verifications") ==
          std::vector<std::string>{fmt::format("{} | {}", Setup::stored(tiny), tinySession)});
    // The blocked session still reports no exit.
    CHECK(setup.value(fmt::format("SELECT exit_kind IS NULL AND exit_code IS NULL FROM rv_play_sessions WHERE id = {}",
                                  holderSession)) == "1");

    // `identities show <holder>`: the alias, then the identity it stands for.
    const json shown = setup.runJson({"identities", "show", holder});
    CHECK(shown.at("asked") == holder);
    REQUIRE(shown.at("alias").is_object());
    CHECK(shown.at("alias").at("game") == tiny);
    CHECK(shown.at("alias").at("reason") == "moved-back");
    CHECK(shown.at("identity").at("id") == tiny);
    CHECK(shown.at("identity").at("relativePath") == "Tiny Tennis (Europe).gb");
    CHECK(contains(setup.run({"identities", "show", holder}).out, fmt::format("{} is an alias of {}", holder, tiny)));
    // `assign --game <holder>` shows the moved identity's choice.
    const json assigned = setup.runJson({"assign", "--game", holder});
    CHECK(assigned.at("game").at("id") == tiny);
    CHECK(assigned.at("game").at("requested") == holder);
    CHECK(assigned.at("choice").at("value") == "fake-return");

    // `launch --dry-run --game <holder>` and `launch --game <holder>` resolve to the moved identity, with its choice.
    const json dryRun = setup.runJson({"launch", "--dry-run", "--game", holder});
    CHECK(dryRun.at("game") == tiny);
    CHECK(dryRun.at("requestedGame") == holder);
    CHECK(dryRun.at("path") == "Tiny Tennis (Europe).gb");
    CHECK(dryRun.at("chosen").at("value") == "fake-return");
    CHECK(dryRun.at("chosen").at("source") == "retroverse-game");
    CHECK(dryRun.at("chosen").at("origin") == "assign");
    CHECK(dryRun.at("ready") == true);
    CHECK(contains(setup.run({"launch", "--dry-run", "--game", holder}).out, fmt::format("(for {}, an alias)", holder)));
    const json launched = setup.runJson({"launch", "--game", holder, "--wait"});
    CHECK(launched.at("game") == tiny);
    CHECK(launched.at("requestedGame") == holder);
    CHECK(launched.at("session").at("game") == tiny);
    CHECK(launched.at("session").at("choice").at("value") == "fake-return");
    CHECK(launched.at("session").at("outcome") == "returned");
    CHECK(launched.at("sessionRecorded") == true);
    const std::int64_t aliasSession = launched.at("session").at("id").get<std::int64_t>();

    // `sessions --game <holder>` lists the holder's session and the moved identity's under the moved identity, as
    // `sessions --game <moved identity>` does.
    const json byHolder = setup.runJson({"sessions", "--game", holder});
    CHECK(listedIds(byHolder) == std::vector<std::int64_t>{aliasSession, tinySession, holderSession});
    CHECK(listedGames(byHolder) == std::vector<std::string>{tiny, tiny, tiny});
    CHECK(byHolder.at("sessions")[2].at("outcome") == "blocked");
    CHECK(byHolder.at("sessions")[2].at("exitKind").is_null());
    CHECK(byHolder.at("sessions")[2].at("path") == "Tiny Tennis (Europe).gb");
    CHECK(byHolder.at("sessions")[2].at("title") == "Tiny Tennis");
    CHECK(listedIds(setup.runJson({"sessions", "--game", tiny})) == listedIds(byHolder));
    setup.ok({"db", "check"});

    // `db rebuild-index` after it: every state row kept as it was (identities, aliases, history, reviews, sessions,
    // choices, verifications), the check passing, the alias still standing for the game.
    const std::vector<std::string> before = setup.stateRows();
    REQUIRE(std::any_of(before.begin(), before.end(),
                        [](const std::string& row) { return row.starts_with("rv_identity_aliases: "); }));
    const std::string imports = setup.value("SELECT count(*) FROM rv_imports");
    const json rebuilt = setup.runJson({"db", "rebuild-index"});
    CHECK(rebuilt.at("kind") == "rebuild-index");
    CHECK(setup.stateRows() == before);
    CHECK(std::stoi(setup.value("SELECT count(*) FROM rv_imports")) == std::stoi(imports) + 1); // its own record
    CHECK(setup.value("SELECT kind FROM rv_imports ORDER BY id DESC LIMIT 1") == "rebuild-index");
    setup.ok({"db", "check"});
    const json afterRebuild = setup.runJson({"sessions", "--game", holder});
    CHECK(listedIds(afterRebuild) == listedIds(byHolder));
    CHECK(afterRebuild.at("sessions")[0].at("title") == "Tiny Tennis");
    CHECK(setup.runJson({"launch", "--dry-run", "--game", holder}).at("game") == tiny);
    // The import after changes nothing.
    const json steady = setup.runJson({"import"});
    CHECK(steady.at("moved").empty());
    CHECK(steady.at("statistics").at("identitiesWritten") == 0);
    CHECK(setup.stateRows() == before);
    CHECK(setup.sessionGames() == std::vector<std::string>{tiny, tiny, tiny});
}

TEST_CASE("cli/reconciled sessions: SC-07, moved identities keep their GameIds: their sessions, choices and "
          "verifications need no re-pointing and still show",
          "[cli][sessions][identities]") {
    const LoggingReset loggingReset;
    const Setup setup("rv-cli-reconciled-sc07");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    setup.ok({"identities", "hash"});
    const std::string loud = gameIdOf("LOUD CART (USA).GB");
    const std::string tiny = gameIdOf("Tiny Tennis (Europe).gb");

    // LOUD CART: a choice and a launch that returns normally (a session and a verification); Tiny Tennis: a choice and
    // a blocked attempt (fake-missing: no exit).
    setup.ok({"assign", "--game", loud, "fake-return"});
    const json played = setup.runJson({"launch", "--game", loud, "--wait"});
    CHECK(played.at("session").at("outcome") == "returned");
    CHECK(played.at("verified") == true);
    setup.ok({"assign", "--game", tiny, "fake-missing"});
    const json blocked = setup.runJson({"launch", "--game", tiny}, ExitCode::ProblemsFound);
    CHECK(blocked.at("session").at("outcome") == "blocked");
    CHECK(blocked.at("session").at("readiness") == "emulator-not-found");
    CHECK(blocked.at("session").at("exitKind").is_null());
    const auto launchRows = [&] {
        std::vector<std::string> all = setup.rows("SELECT * FROM rv_play_sessions ORDER BY id");
        for (const char* table : {"rv_launch_game_choices", "rv_launch_system_choices", "rv_launch_verifications"}) {
            for (std::string& row : setup.rows(fmt::format("SELECT * FROM {} ORDER BY 1", table))) {
                all.push_back(std::string(table) + ": " + row);
            }
        }
        return all;
    };
    const std::vector<std::string> before = launchRows();

    // Tiny Tennis renamed in place (its gamelist entry left at the old path), LOUD CART moved into a new folder: both
    // pair by their quick hash and keep their GameIds; no alias, so no row is re-pointed.
    setup.moveRom("gb/Tiny Tennis (Europe).gb", "gb/Tiny Tennis (Europe) (Rev 1).gb");
    setup.moveRom("gb/LOUD CART (USA).GB", "gb/Moved/LOUD CART (USA).GB");
    const json imported = setup.runJson({"import"});
    REQUIRE(imported.at("moved").size() == 2);
    for (const json& move : imported.at("moved")) {
        CHECK(move.at("reason") == "quick-hash");
        CHECK(move.at("alias").is_null());
    }
    CHECK(launchRows() == before); // the launch tables untouched, row for row

    // The sessions show at the new paths, under the same GameIds.
    const json loudSessions = setup.runJson({"sessions", "--game", loud});
    REQUIRE(loudSessions.at("sessions").size() == 1);
    CHECK(loudSessions.at("sessions")[0].at("id") == played.at("session").at("id"));
    CHECK(loudSessions.at("sessions")[0].at("path") == "Moved/LOUD CART (USA).GB");
    const json tinySessions = setup.runJson({"sessions", "--game", tiny});
    REQUIRE(tinySessions.at("sessions").size() == 1);
    CHECK(tinySessions.at("sessions")[0].at("path") == "Tiny Tennis (Europe) (Rev 1).gb");
    // Renamed away from its gamelist entry, the file is titled from its name now.
    CHECK(tinySessions.at("sessions")[0].at("title") == "Tiny Tennis (Europe) (Rev 1)");
    CHECK(tinySessions.at("sessions")[0].at("exitKind").is_null());
    // The gamelist entry left at Tiny Tennis's old path is a new identity, with none of it.
    const std::string left = gameIdOf("Tiny Tennis (Europe).gb", 1);
    CHECK(setup.runJson({"sessions", "--game", left}).at("sessions").empty());
    CHECK(setup.runJson({"assign", "--game", left}).at("choice").is_null());

    // The choices still apply: LOUD CART launches from its new folder with its choice, and its verification is now
    // that launch's.
    const json dryRun = setup.runJson({"launch", "--dry-run", "--game", loud});
    CHECK(dryRun.at("game") == loud);
    CHECK(dryRun.at("chosen").at("value") == "fake-return");
    CHECK(dryRun.at("chosen").at("source") == "retroverse-game");
    CHECK(dryRun.at("path") == "Moved/LOUD CART (USA).GB");
    CHECK(dryRun.at("ready") == true);
    const json again = setup.runJson({"launch", "--game", loud, "--wait"});
    CHECK(again.at("session").at("game") == loud);
    CHECK(again.at("session").at("outcome") == "returned");
    CHECK(again.at("verified") == true);
    CHECK(setup.rows("SELECT game, session FROM rv_launch_verifications") ==
          std::vector<std::string>{fmt::format("{} | {}", Setup::stored(loud),
                                               again.at("session").at("id").get<std::int64_t>())});
    CHECK(setup.runJson({"launch", "--dry-run", "--game", tiny}, ExitCode::ProblemsFound).at("chosen").at("value") ==
          "fake-missing");
    CHECK(setup.runJson({"sessions", "--game", loud}).at("sessions").size() == 2);
    setup.ok({"db", "check"});
}

TEST_CASE("cli/reconciled sessions: a review resolved with --with moves the chosen identity's sessions, choice and "
          "verification to the vanished identity",
          "[cli][sessions][identities]") {
    const LoggingReset loggingReset;
    const Setup setup("rv-cli-reconciled-review");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    const std::string vanished = gameIdOf("Collection A/Pocket Puzzle (World).gb");

    // The vanished-to-be identity: a session (blocked), its choice cleared afterwards.
    setup.ok({"assign", "--game", vanished, "fake-missing"});
    const json first = setup.runJson({"launch", "--game", vanished}, ExitCode::ProblemsFound);
    CHECK(first.at("session").at("outcome") == "blocked");
    CHECK(first.at("session").at("exitKind").is_null());
    setup.ok({"assign", "--game", vanished, "--clear"});

    // Collection A's Pocket Puzzle copied into two new folders, then deleted (no quick hash stored): a review row with
    // both copies as candidates, each a new identity.
    setup.copyRom("gb/Collection A/Pocket Puzzle (World).gb", "gb/Collection C/Pocket Puzzle (World).gb");
    setup.copyRom("gb/Collection A/Pocket Puzzle (World).gb", "gb/Collection D/Pocket Puzzle (World).gb");
    setup.removeRom("gb/Collection A/Pocket Puzzle (World).gb");
    const json imported = setup.runJson({"import"});
    REQUIRE(imported.at("reviews").size() == 1);
    const std::string review = std::to_string(imported.at("reviews")[0].at("review").get<std::int64_t>());
    const json reviews = setup.runJson({"identities", "reviews"});
    REQUIRE(reviews.at("reviews").size() == 1);
    REQUIRE(reviews.at("reviews")[0].at("candidates").size() == 2);
    const std::string chosen = reviews.at("reviews")[0].at("candidates")[0].at("game").get<std::string>();
    const std::string other = reviews.at("reviews")[0].at("candidates")[1].at("game").get<std::string>();
    CHECK(reviews.at("reviews")[0].at("candidates")[0].at("relativePath") == "Collection C/Pocket Puzzle (World).gb");

    // The chosen copy is played meanwhile: a choice, a session that returns normally, a verification. The other copy
    // gets a choice and a blocked session of its own.
    setup.ok({"assign", "--game", chosen, "fake-return"});
    const json played = setup.runJson({"launch", "--game", chosen, "--wait"});
    CHECK(played.at("session").at("outcome") == "returned");
    CHECK(played.at("verified") == true);
    const std::int64_t chosenSession = played.at("session").at("id").get<std::int64_t>();
    setup.ok({"assign", "--game", other, "fake-missing"});
    const json otherBlocked = setup.runJson({"launch", "--game", other}, ExitCode::ProblemsFound);
    const std::int64_t otherSession = otherBlocked.at("session").at("id").get<std::int64_t>();
    CHECK(setup.sessionGames() == std::vector<std::string>{vanished, chosen, other});

    // Resolved with the chosen copy: it becomes an alias of the vanished identity, which takes its file; its session,
    // choice and verification follow (the vanished identity had no choice left, so the chosen one's moves), the other
    // copy keeps its own.
    const RunResult resolved = setup.run({"identities", "resolve", review, "--with", chosen});
    INFO(resolved.err);
    REQUIRE(resolved.code == ExitCode::Ok);
    CHECK(setup.sessionGames() == std::vector<std::string>{vanished, vanished, other});
    CHECK(setup.rows("SELECT game, value FROM rv_launch_game_choices ORDER BY value") ==
          std::vector<std::string>{fmt::format("{} | fake-missing", Setup::stored(other)),
                                   fmt::format("{} | fake-return", Setup::stored(vanished))});
    CHECK(setup.rows("SELECT game, session FROM rv_launch_verifications") ==
          std::vector<std::string>{fmt::format("{} | {}", Setup::stored(vanished), chosenSession)});

    // The chosen copy's GameId stands for the vanished identity everywhere: show, sessions, the dry run.
    const json shown = setup.runJson({"identities", "show", chosen});
    CHECK(shown.at("alias").at("game") == vanished);
    CHECK(shown.at("alias").at("reason") == "review-resolved");
    CHECK(shown.at("identity").at("relativePath") == "Collection C/Pocket Puzzle (World).gb");
    const json sessions = setup.runJson({"sessions", "--game", chosen});
    CHECK(listedIds(sessions) == std::vector<std::int64_t>{chosenSession, first.at("session").at("id")});
    CHECK(listedGames(sessions) == std::vector<std::string>{vanished, vanished});
    CHECK(sessions.at("sessions")[0].at("path") == "Collection C/Pocket Puzzle (World).gb");
    CHECK(listedIds(setup.runJson({"sessions", "--game", other})) == std::vector<std::int64_t>{otherSession});
    const json dryRun = setup.runJson({"launch", "--dry-run", "--game", chosen});
    CHECK(dryRun.at("game") == vanished);
    CHECK(dryRun.at("requestedGame") == chosen);
    CHECK(dryRun.at("chosen").at("value") == "fake-return");
    CHECK(dryRun.at("chosen").at("source") == "retroverse-game");
    CHECK(dryRun.at("ready") == true);
    setup.ok({"db", "check"});
    // The import after changes nothing.
    const json steady = setup.runJson({"import"});
    CHECK(steady.at("moved").empty());
    CHECK(steady.at("reviews").empty());
    CHECK(steady.at("statistics").at("identitiesWritten") == 0);
}
