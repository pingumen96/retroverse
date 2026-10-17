// retroverse-cli import through the dispatcher, on the standard ES-DE fixture. The dry run (I2 step 7): the counts by
// system and state as text and JSON, --system, the refusals (no library.json, an unknown system), and a dry run that
// writes nothing but its log file. The import (I2 step 11a): the ChangeSet per system as text and JSON, a second import
// that writes nothing, `show` and `media --game` with the media paths derived, `db rebuild-index`, nothing copied into
// the data directory; a library.json naming another library refused until it names the catalog's again, a re-created
// one and a new ROM root keeping every GameId; one import at a time, also from a second process; the
// mass-disappearance guard's exit code and --accept-removals.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
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
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/discovery.hpp>

#include "../catalog_db/import_test_support.hpp"
#include "support/child_process.hpp"
#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const fs::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

/// A run of the dispatcher logs to stderr; this puts back the plain console logger on stdout afterwards.
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

RunResult runCli(const std::vector<std::string>& arguments, const fs::path& executableDir) {
    Output out = Output::memory();
    Output err = Output::memory();
    const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(executableDir));
    return RunResult{code, out.text(), err.text()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// The files and folders under `root` (relative paths, files with their size).
std::vector<std::string> treeOf(const fs::path& root) {
    const auto listed = rv::core::listTree(root);
    REQUIRE(listed.has_value());
    std::vector<std::string> tree;
    for (const auto& entry : *listed) {
        tree.push_back(entry.relativePath +
                       (entry.kind == rv::core::TreeEntryKind::File ? " " + std::to_string(entry.size) : "/"));
    }
    return tree;
}

} // namespace

TEST_CASE("cli/import: --dry-run prints the standard fixture's entries by system and state", "[cli][import]") {
    const LoggingReset loggingReset;
    auto fixture = rv::test::Fixture::materialize("esde/standard");
    REQUIRE(fixture.has_value());
    const rv::test::TempDir dir("rv-cli-import");
    const fs::path data = dir.file("data");
    const std::vector<std::string> global{"--data-dir", rv::core::pathToUtf8(data)};
    const auto withGlobal = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), global.begin(), global.end());
        return arguments;
    };

    // Without a library.json there is nothing to read.
    const RunResult none = runCli(withGlobal({"import", "--dry-run"}), dir.path());
    CHECK(none.code == ExitCode::UsageError);
    CHECK(contains(none.err, "no library yet"));
    CHECK(none.out.empty());

    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture->path("ES-DE");
    profile.installFolder = fixture->path("Programs/ES-DE");
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / "library.json", profile, rv::esde::ExistingFile::Keep));
    const std::vector<std::string> before = treeOf(fixture->root());

    SECTION("text") {
        const RunResult result = runCli(withGlobal({"import", "--dry-run"}), dir.path());
        INFO(result.err);
        CHECK(result.code == ExitCode::Ok);
        CHECK(contains(result.out, "(dry run: nothing is written)"));
        // System, entries, games, missing, not game, members, folders, outside, unavailable, groups, issues.
        CHECK(contains(result.out, "gb                     11        8        1        0        0        1        1"
                                   "        0        0        1  available, read"));
        // psx's issue: the "CUE" quirk of its extensions (es_systems.xml).
        CHECK(contains(result.out, "psx                     4        2        0        0        2        0        0"
                                   "        0        1        1  available, read"));
        CHECK(contains(result.out, "snes                    8        5        0        1        0        2        0"
                                   "        0        0        0  available, read"));
        CHECK(contains(result.out, "Total                  23       15        1        1        2        3        1"));
        CHECK(contains(result.out, "games as ES-DE lists them (the games and disc members on disk): 17"));
        CHECK(contains(result.out, "  snes: extension-not-configured 1"));
        CHECK(contains(result.out, "[warning] gamelist-path-outside-folder gb:"));
        CHECK(contains(result.out, "Read in "));
        CHECK(treeOf(fixture->root()) == before);
        // It writes nothing but its log file.
        const std::vector<std::string> written = treeOf(data);
        for (const std::string& path : written) {
            INFO(path);
            CHECK((path.starts_with("config") || path.starts_with("logs")));
        }
        CHECK(fs::exists(data / "logs" / "retroverse-cli.log"));
    }
    SECTION("JSON, one system") {
        const RunResult result = runCli(withGlobal({"import", "--dry-run", "--system", "psx", "--json"}), dir.path());
        INFO(result.err);
        CHECK(result.code == ExitCode::Ok);
        auto json = rv::core::parseJson(result.out, "stdout");
        REQUIRE(json.has_value());
        CHECK((*json)["dryRun"] == true);
        REQUIRE((*json)["systems"].size() == 1);
        const auto& psx = (*json)["systems"][0];
        CHECK(psx["name"] == "psx");
        CHECK(psx["gamelist"] == "read");
        CHECK(psx["entries"] == 4);
        CHECK(psx["games"] == 2);
        CHECK(psx["discMembers"] == 2);
        CHECK(psx["discGroups"] == 1);
        CHECK((*json)["totals"]["entries"] == 4);
        CHECK((*json)["statistics"]["sheetsRead"] == 1);
    }
    SECTION("refusals") {
        const RunResult accepting = runCli(withGlobal({"import", "--dry-run", "--accept-removals"}), dir.path());
        CHECK(accepting.code == ExitCode::UsageError);
        CHECK(contains(accepting.err, "--accept-removals is for the import"));
        const RunResult unknown = runCli(withGlobal({"import", "--dry-run", "--system", "nosuch"}), dir.path());
        CHECK(unknown.code == ExitCode::UsageError);
        CHECK(contains(unknown.err, "nosuch"));
        CHECK(unknown.out.empty());
        const RunResult option = runCli(withGlobal({"import", "--dry-run", "--bogus"}), dir.path());
        CHECK(option.code == ExitCode::UsageError);
    }
}

namespace {

/// The standard fixture materialized with a data directory whose library.json records it (I2 step 11a's tests).
class ImportSession {
public:
    explicit ImportSession(std::string_view prefix) : m_dir(prefix), m_data(m_dir.file("data")) {
        auto fixture = rv::test::Fixture::materialize("esde/standard");
        REQUIRE(fixture.has_value());
        m_fixture.emplace(std::move(*fixture));
        rv::test::settleFolderTimes(m_fixture->path("roms")); // imports a moment apart compare exactly
        writeLibrary("esde");
    }

    /// Writes library.json naming `id`, the fixture's ES-DE home and install, and `romRoot` as the ROM root override.
    void writeLibrary(std::string_view id, const std::optional<fs::path>& romRoot = std::nullopt) const {
        rv::catalog::LibraryProfile profile;
        profile.id = rv::catalog::LibraryId::parse(id).value();
        profile.esdeHome = m_fixture->path("ES-DE");
        profile.installFolder = m_fixture->path("Programs/ES-DE");
        profile.romRoot = romRoot;
        REQUIRE(rv::esde::writeLibraryFile(m_data / "config" / "library.json", profile,
                                           rv::esde::ExistingFile::Replace));
    }

    [[nodiscard]] RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(m_data)});
        return runCli(arguments, m_dir.path());
    }
    /// run() REQUIREd to exit `code`, its JSON output parsed.
    [[nodiscard]] nlohmann::json json(std::vector<std::string> arguments, ExitCode code = ExitCode::Ok) const {
        const RunResult result = run(std::move(arguments));
        INFO(result.err);
        REQUIRE(result.code == code);
        auto json = rv::core::parseJson(result.out, "stdout");
        REQUIRE(json.has_value());
        return std::move(*json);
    }

    [[nodiscard]] const rv::test::Fixture& fixture() const { return *m_fixture; }
    [[nodiscard]] const fs::path& data() const { return m_data; }
    [[nodiscard]] const rv::test::TempDir& dir() const { return m_dir; }
    [[nodiscard]] fs::path database() const { return m_data / "database" / "retroverse.sqlite"; }

    /// Every GameId of the catalog with its system and path, in GameId order.
    [[nodiscard]] std::vector<std::string> ids() const {
        const nlohmann::json games =
            json({"games", "--state", "game,missing-file,not-a-game,disc-member,folder", "--include-unavailable",
                  "--json"});
        std::vector<std::string> ids;
        for (const auto& game : games["games"]) {
            ids.push_back(game["id"].get<std::string>() + " " + game["system"].get<std::string>() + " " +
                          game["title"].get<std::string>());
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

private:
    rv::test::TempDir m_dir;
    fs::path m_data;
    std::optional<rv::test::Fixture> m_fixture;
};

/// The GameId text of a path of a system, as a first import gives it.
std::string gameIdOf(std::string_view system, std::string_view relativePath) {
    const auto id = rv::catalog::stableGameId(rv::catalog::LibraryId(), rv::catalog::SystemName::parse(system).value(),
                                              rv::catalog::makeLookupKey(relativePath).value().key());
    REQUIRE(id.has_value());
    return id->toString();
}

/// A path as the CLI prints it ('\' between the elements).
std::string printed(const fs::path& path) {
    return rv::core::pathToUtf8(path.lexically_normal());
}

} // namespace

TEST_CASE("cli/import: the standard fixture imported, imported again, shown and rebuilt", "[cli][import]") {
    const LoggingReset loggingReset;
    const ImportSession session("rv-cli-import-db");
    const std::vector<std::string> before = treeOf(session.fixture().root());

    const RunResult first = session.run({"import"});
    INFO(first.err);
    CHECK(first.code == ExitCode::Ok);
    CHECK(contains(first.out, "(first import)"));
    // System, added, changed, removed, unchanged, unavailable, kept (then the issues and the outcome).
    const auto row = [](std::string_view system, int added) {
        return fmt::format("{:<16}{:>9}{:>9}{:>9}{:>10}{:>9}{:>9}", system, added, 0, 0, 0, 0, 0);
    };
    CHECK(contains(first.out, row("gb", 10)));
    CHECK(contains(first.out, row("snes", 8)));
    CHECK(contains(first.out, row("psx", 4)));
    CHECK(contains(first.out, "applied"));
    CHECK(contains(first.out, "Identities: 22 new, 0 vanished, 0 back, 0 re-keyed, 0 moved"));
    CHECK(contains(first.out, "the search index rebuilt"));
    CHECK(contains(first.out, "[warning] gamelist-path-outside-folder gb:"));

    // Again: nothing written, the cue sheet taken from the catalog.
    const nlohmann::json second = session.json({"import", "--json"});
    CHECK(second["kind"] == "import");
    CHECK(second["firstImport"] == false);
    CHECK(second["totals"]["added"] == 0);
    CHECK(second["totals"]["unchanged"] == 22);
    CHECK(second["totals"]["newIdentities"] == 0);
    CHECK(second["statistics"]["identitiesWritten"] == 0);
    CHECK(second["statistics"]["entriesWritten"] == 0);
    CHECK(second["statistics"]["sheetsRead"] == 0);
    CHECK(second["statistics"]["sheetsFromCache"] == 1);
    const auto gb = std::find_if(second["systems"].begin(), second["systems"].end(),
                                 [](const nlohmann::json& system) { return system["system"] == "gb"; });
    REQUIRE(gb != second["systems"].end());
    CHECK((*gb)["outcome"] == "applied");
    CHECK((*gb)["rows"] == 10);

    // A game's media: the stored presence and extension, the paths derived under the media root.
    const std::string tiny = gameIdOf("gb", "Tiny Tennis (Europe).gb");
    const fs::path media = session.fixture().path("media");
    const std::string cover = printed(media / L"gb/covers/Tiny Tennis (Europe).png");
    const nlohmann::json game = session.json({"media", "--game", tiny, "--json"});
    CHECK(game["system"] == "gb");
    CHECK(game["path"] == "Tiny Tennis (Europe).gb");
    CHECK(game["mediaName"] == "Tiny Tennis (Europe)");
    REQUIRE(game["media"].size() == 12);
    CHECK(game["media"][0] == nlohmann::json{{"kind", "covers"}, {"present", true}, {"extension", ".png"}, {"path", cover}});
    CHECK(game["media"][1]["present"] == false);
    CHECK(game["media"][1]["path"].is_null());
    CHECK(game["media"][4]["path"] == printed(media / L"gb/screenshots/Tiny Tennis (Europe).jpg"));
    CHECK(game["media"][10]["path"] == printed(media / L"gb/videos/Tiny Tennis (Europe).mp4"));
    const RunResult gameText = session.run({"media", "--game", tiny});
    CHECK(gameText.code == ExitCode::Ok);
    CHECK(contains(gameText.out, fmt::format("{:<14}3 of 12 kinds", "Media")));
    CHECK(contains(gameText.out, fmt::format("  {:<14}{}", "covers", cover)));
    CHECK(contains(gameText.out, fmt::format("  {:<14}none", "backcovers")));
    const RunResult folder = session.run({"media", "--game", gameIdOf("snes", "Sports Vol. 2"), "--json"});
    CHECK(contains(folder.out, "Sports Vol. 2.png")); // a folder's media keep its whole name
    const RunResult unknownGame = session.run({"media", "--game", "g0000000000000001"});
    CHECK(unknownGame.code == ExitCode::ProblemsFound);
    CHECK(contains(unknownGame.err, "the catalog has no game g0000000000000001"));
    // show prints them too.
    const RunResult show = session.run({"show", tiny});
    CHECK(show.code == ExitCode::Ok);
    CHECK(contains(show.out, fmt::format("  {:<20}{}", "covers", cover)));
    const nlohmann::json shown = session.json({"show", tiny, "--json"});
    CHECK(shown["media"][0]["path"] == cover);

    // The index rebuilt: every row written again, the same catalog.
    const std::vector<std::string> ids = session.ids();
    CHECK(ids.size() == 22); // every indexed entry
    const nlohmann::json rebuilt = session.json({"db", "rebuild-index", "--json"});
    CHECK(rebuilt["kind"] == "rebuild-index");
    CHECK(rebuilt["statistics"]["entriesWritten"] == 22);
    CHECK(rebuilt["statistics"]["identitiesWritten"] == 0);
    CHECK(rebuilt["totals"]["unchanged"] == 22);
    CHECK(session.ids() == ids);
    const RunResult check = session.run({"db", "check"});
    INFO(check.out);
    CHECK(check.code == ExitCode::Ok);

    // Nothing written in the library; nothing of it copied into the data directory.
    CHECK(treeOf(session.fixture().root()) == before);
    const auto listed = rv::core::listTree(session.data());
    REQUIRE(listed.has_value());
    std::uint64_t total = 0;
    for (const auto& entry : *listed) {
        if (entry.kind != rv::core::TreeEntryKind::File) {
            continue;
        }
        INFO(entry.relativePath);
        total += entry.size;
        CHECK((entry.relativePath == "config/library.json" || entry.relativePath == "logs/retroverse-cli.log" ||
               entry.relativePath.starts_with("database/retroverse.sqlite")));
    }
    CHECK(total < 4ull * 1024 * 1024);
}

TEST_CASE("cli/import: the catalog's library: another refused until re-associated; a re-created library.json and a new "
          "ROM root keep every id",
          "[cli][import]") {
    const LoggingReset loggingReset;
    const ImportSession session("rv-cli-import-library");
    REQUIRE(session.run({"import"}).code == ExitCode::Ok);
    const std::vector<std::string> ids = session.ids();

    // library.json naming another library: refused, with the re-association to run; nothing written.
    session.writeLibrary("other");
    const RunResult refused = session.run({"import"});
    CHECK(refused.code == ExitCode::UsageError);
    CHECK(refused.out.empty());
    CHECK(contains(refused.err, "names the library 'other', but the catalog holds 'esde'"));
    CHECK(contains(refused.err, "retroverse-cli library create --id esde --force"));
    CHECK(session.run({"db", "rebuild-index"}).code == ExitCode::UsageError);

    // Re-associated: library.json re-created by library create, naming the catalog's library.
    const RunResult created =
        session.run({"library", "create", "--id", "esde", "--force", "--home", printed(session.fixture().root()),
                     "--install", printed(session.fixture().path("Programs/ES-DE"))});
    INFO(created.err);
    REQUIRE(created.code == ExitCode::Ok);
    const nlohmann::json again = session.json({"import", "--json"});
    CHECK(again["totals"]["newIdentities"] == 0);
    CHECK(again["statistics"]["identitiesWritten"] == 0);
    CHECK(session.ids() == ids);

    // A new ROM root (the drive moved): every id kept, no entry row written.
    const fs::path moved = session.fixture().path("roms-moved");
    fs::rename(session.fixture().path("roms"), moved);
    session.writeLibrary("esde", moved);
    const nlohmann::json relocated = session.json({"import", "--json"});
    CHECK(relocated["totals"]["newIdentities"] == 0);
    CHECK(relocated["totals"]["added"] == 0);
    CHECK(relocated["totals"]["removed"] == 0);
    CHECK(relocated["totals"]["unavailable"] == 0);
    CHECK(relocated["totals"]["changed"] == 0);
    CHECK(relocated["statistics"]["identitiesWritten"] == 0);
    CHECK(relocated["statistics"]["entriesWritten"] == 0);
    CHECK(relocated["statistics"]["systemsWritten"] == 3); // their ROM folders
    CHECK(session.ids() == ids);
}

TEST_CASE("cli/import: one import at a time, also from another process", "[cli][import]") {
    const LoggingReset loggingReset;
    const ImportSession session("rv-cli-import-lock");
    REQUIRE(session.run({"import"}).code == ExitCode::Ok);

    // Another holder of the database imports (another process's Database and store).
    auto database = rv::catalog_db::Database::open(session.database());
    REQUIRE(database.has_value());
    auto store = rv::catalog_db::SqliteCatalogStore::open(*database);
    REQUIRE(store.has_value());
    {
        auto lock = (*store)->lockImport(rv::catalog::LibraryId());
        REQUIRE(lock.has_value());
        const RunResult refused = session.run({"import"});
        CHECK(refused.code == ExitCode::RuntimeFailure);
        CHECK(contains(refused.err, "another import runs"));
        CHECK(refused.out.empty());

        // retroverse-cli as a second process.
        const fs::path cli = rv::test::testProgramsDir() / "retroverse-cli.exe";
        if (fs::exists(cli)) {
            rv::test::ChildRequest request;
            request.executable = cli;
            request.arguments = {"--data-dir", rv::core::pathToUtf8(session.data()), "import"};
            request.stdoutFile = session.dir().file("child-stdout.txt");
            request.stderrFile = session.dir().file("child-stderr.txt");
            request.timeout = std::chrono::minutes(2);
            const auto child = rv::test::runChild(request);
            REQUIRE(child.has_value());
            CHECK_FALSE(child->timedOut);
            CHECK(child->exitCode == 3);
            CHECK(contains(child->standardError, "another import runs"));
        } else {
            WARN("retroverse-cli.exe is not built beside rv_tests.exe: the second process is not run");
        }
    }
    const RunResult after = session.run({"import"});
    INFO(after.err);
    CHECK(after.code == ExitCode::Ok);
}

TEST_CASE("cli/import: the mass-disappearance guard exits 1 until the removals are accepted", "[cli][import]") {
    const LoggingReset loggingReset;
    const ImportSession session("rv-cli-import-guard");
    REQUIRE(session.run({"import"}).code == ExitCode::Ok);
    // 5 of gb's 9 files present gone.
    for (const std::string_view file : {"Tiny Tennis (Europe).gb", "Collection A/Pocket Puzzle (World).gb",
                                        "Collection B/Pocket Puzzle (World).gb", "Café Racer (Europe).gb",
                                        ".Dot Hero (World).gb"}) {
        fs::remove(session.fixture().path("roms/gb") / rv::core::pathFromUtf8(file));
    }
    const RunResult held = session.run({"import"});
    INFO(held.err);
    CHECK(held.code == ExitCode::ProblemsFound);
    CHECK(contains(held.out, "kept-by-guard (5 of 9 present files gone)"));
    CHECK(contains(held.out, "--accept-removals"));
    CHECK(contains(held.out, "[warning] import-mass-disappearance gb:"));

    const nlohmann::json accepted = session.json({"import", "--accept-removals", "--json"});
    CHECK(accepted["acceptRemovals"] == true);
    const auto gb = std::find_if(accepted["systems"].begin(), accepted["systems"].end(),
                                 [](const nlohmann::json& system) { return system["system"] == "gb"; });
    REQUIRE(gb != accepted["systems"].end());
    CHECK((*gb)["outcome"] == "applied");
    // Tiny Tennis, Pocket Puzzle (Collection A) and Café Racer stay as missing files, their gamelist entries left.
    CHECK((*gb)["removed"] == 2);
    CHECK((*gb)["vanished"] == 5);
}
