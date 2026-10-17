// retroverse-cli identities (I2 step 11b) through the dispatcher, on the standard ES-DE fixture: the quick-hash pass
// (figures, resumed with nothing left, an unreadable file reported with exit 1), SC-07 on disk (a file renamed and one
// moved, their gamelist entries left behind: the import pairs them by quick hash, `list` and `show` give the history),
// review rows (identical copies of one name: `reviews`, `resolve --with`, `resolve --keep-separate`, then an import that
// changes nothing), usage errors and exit codes, and the import lock: the pass and a resolution refused while an import
// holds it, also from a second process, and an import of a second process refused while the pass runs.

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
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>

#include "../catalog_db/import_test_support.hpp"
#include "support/child_process.hpp"
#include "support/file_system.hpp"
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

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// The standard fixture materialized with a data directory whose library.json records it.
class Session {
public:
    explicit Session(std::string_view prefix) : m_dir(prefix), m_data(m_dir.file("data")) {
        auto fixture = rv::test::Fixture::materialize("esde/standard");
        REQUIRE(fixture.has_value());
        m_fixture.emplace(std::move(*fixture));
        rv::test::settleFolderTimes(m_fixture->path("roms")); // imports a moment apart compare exactly
        rv::catalog::LibraryProfile profile;
        profile.esdeHome = m_fixture->path("ES-DE");
        profile.installFolder = m_fixture->path("Programs/ES-DE");
        REQUIRE(rv::esde::writeLibraryFile(m_data / "config" / "library.json", profile,
                                           rv::esde::ExistingFile::Replace));
    }

    [[nodiscard]] RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(m_data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(m_dir.path()));
        return RunResult{code, out.text(), err.text()};
    }
    /// run() REQUIREd to exit `code`, its JSON output parsed.
    [[nodiscard]] nlohmann::json json(std::vector<std::string> arguments, ExitCode code = ExitCode::Ok) const {
        const RunResult result = run(std::move(arguments));
        INFO(result.err);
        INFO(result.out);
        REQUIRE(result.code == code);
        auto json = rv::core::parseJson(result.out, "stdout");
        REQUIRE(json.has_value());
        return std::move(*json);
    }

    [[nodiscard]] fs::path rom(std::string_view relative) const {
        return m_fixture->path("roms") / rv::core::pathFromUtf8(relative);
    }
    [[nodiscard]] const fs::path& data() const { return m_data; }
    [[nodiscard]] const rv::test::TempDir& dir() const { return m_dir; }
    [[nodiscard]] fs::path database() const { return m_data / "database" / "retroverse.sqlite"; }
    void settle() const { rv::test::settleFolderTimes(m_fixture->path("roms")); }

private:
    rv::test::TempDir m_dir;
    fs::path m_data;
    std::optional<rv::test::Fixture> m_fixture;
};

/// The GameId text of a path of a system, as a first import gives it.
std::string gameIdOf(std::string_view system, std::string_view relativePath, std::uint32_t counter = 0) {
    const auto id = rv::catalog::stableGameId(rv::catalog::LibraryId(), rv::catalog::SystemName::parse(system).value(),
                                              rv::catalog::makeLookupKey(relativePath).value().key(), counter);
    REQUIRE(id.has_value());
    return id->toString();
}

/// The identity of `relativePath` in a `list --json`.
nlohmann::json identityIn(const nlohmann::json& list, std::string_view relativePath) {
    const auto& identities = list["identities"];
    const auto found = std::find_if(identities.begin(), identities.end(), [&](const nlohmann::json& identity) {
        return identity["relativePath"] == std::string(relativePath);
    });
    INFO(relativePath);
    REQUIRE(found != identities.end());
    return *found;
}

/// Moves a ROM of the fixture, its last write time kept (as a move keeps it), the folders above it made.
void moveRom(const Session& session, std::string_view from, std::string_view to) {
    fs::create_directories(session.rom(to).parent_path());
    fs::rename(session.rom(from), session.rom(to));
}

/// Copies a ROM of the fixture with its last write time.
void copyRom(const Session& session, std::string_view from, std::string_view to) {
    fs::create_directories(session.rom(to).parent_path());
    fs::copy_file(session.rom(from), session.rom(to));
    REQUIRE(rv::test::setLastWriteTimeAnyLength(session.rom(to), fs::last_write_time(session.rom(from))).has_value());
}

} // namespace

TEST_CASE("cli/identities: usage errors exit 2 and name the problem", "[cli][identities]") {
    const LoggingReset loggingReset;
    const Session session("rv-cli-identities-usage");
    const RunResult none = session.run({"identities"});
    CHECK(none.code == ExitCode::UsageError);
    CHECK(contains(none.err, "Usage: retroverse-cli"));
    const RunResult help = session.run({"identities", "--help"});
    CHECK(help.code == ExitCode::Ok);
    CHECK(contains(help.out, "resolve <review> --with <gameId>"));
    CHECK(session.run({"identities", "frobnicate"}).code == ExitCode::UsageError);
    CHECK(session.run({"identities", "list", "--bogus"}).code == ExitCode::UsageError);
    CHECK(contains(session.run({"identities", "list"}).err, "--system <name> is required"));
    CHECK(session.run({"identities", "show", "g12"}).code == ExitCode::UsageError);
    CHECK(contains(session.run({"identities", "resolve", "3"}).err, "--with <gameId> or --keep-separate"));
    CHECK(session.run({"identities", "resolve", "3", "--with", "g0000000000000001", "--keep-separate"}).code ==
          ExitCode::UsageError);
    CHECK(session.run({"identities", "resolve", "-4", "--keep-separate"}).code == ExitCode::UsageError);
    CHECK(session.run({"identities", "hash", "--limit", "0"}).code == ExitCode::UsageError);
    // No catalog yet: nothing to read (exit 3), and nothing made.
    const RunResult noCatalog = session.run({"identities", "hash"});
    CHECK(noCatalog.code == ExitCode::RuntimeFailure);
    CHECK(contains(noCatalog.err, "'retroverse-cli import' makes it"));
    CHECK_FALSE(fs::exists(session.database()));
    // A system without identities is a mistake worth naming.
    REQUIRE(session.run({"import"}).code == ExitCode::Ok);
    CHECK(session.run({"identities", "list", "--system", "nes"}).code == ExitCode::UsageError);
    CHECK(session.run({"identities", "hash", "--system", "nes"}).code == ExitCode::UsageError);
    // An identity or alias the catalog does not have: exit 1.
    CHECK(session.run({"identities", "show", "g0000000000000001"}).code == ExitCode::ProblemsFound);
}

TEST_CASE("cli/identities: the quick-hash pass, then SC-07 on disk: a file renamed and one moved keep their GameIds",
          "[cli][identities]") {
    const LoggingReset loggingReset;
    const Session session("rv-cli-identities-sc07");
    REQUIRE(session.run({"import"}).code == ExitCode::Ok);

    // The pass: every present file of the catalog (18), folders (3) and the missing file (1) left out.
    const nlohmann::json pass = session.json({"identities", "hash", "--json"});
    CHECK(pass["identities"] == 22);
    CHECK(pass["candidates"] == 18);
    CHECK(pass["hashed"] == 18);
    CHECK(pass["folders"] == 3);
    CHECK(pass["notPresent"] == 1);
    CHECK(pass["unreadable"] == 0);
    CHECK(pass["remaining"] == 0);
    CHECK(pass["batches"] == 1);
    // At most 128 KiB per file: Bottom Dweller's 192 KiB count 128 KiB.
    CHECK(pass["bytesRead"].get<std::uint64_t>() <= 18u * 131072u);
    // Again: nothing left.
    const RunResult again = session.run({"identities", "hash"});
    INFO(again.err);
    CHECK(again.code == ExitCode::Ok);
    CHECK(contains(again.out, "18 with a current quick hash, 0 to hash"));
    CHECK(contains(again.out, "Hashed 0 file(s)"));
    const nlohmann::json listed = session.json({"identities", "list", "--system", "gb", "--json"});
    CHECK(listed["count"] == 10);
    CHECK(listed["quickHashes"]["current"] == 8);
    CHECK(identityIn(listed, "Tiny Tennis (Europe).gb")["quickHashState"] == "current");

    // Renamed in place and moved to a new folder; Tiny Tennis's gamelist entry stays at its old path.
    moveRom(session, "gb/Tiny Tennis (Europe).gb", "gb/Tiny Tennis (Europe) (Rev 1).gb");
    moveRom(session, "gb/LOUD CART (USA).GB", "gb/Moved/LOUD CART (USA).GB");
    session.settle();
    const nlohmann::json imported = session.json({"import", "--json"});
    REQUIRE(imported["moved"].size() == 2);
    const std::string tiny = gameIdOf("gb", "Tiny Tennis (Europe).gb");
    const std::string loud = gameIdOf("gb", "LOUD CART (USA).GB");
    for (const nlohmann::json& move : imported["moved"]) {
        CHECK(move["reason"] == "quick-hash");
        CHECK(move["alias"].is_null());
        if (move["id"] == tiny) {
            CHECK(move["from"] == "Tiny Tennis (Europe).gb");
            CHECK(move["to"] == "Tiny Tennis (Europe) (Rev 1).gb");
        } else {
            CHECK(move["id"] == loud);
            CHECK(move["to"] == "Moved/LOUD CART (USA).GB");
        }
    }
    CHECK(imported["statistics"]["quickHashesRead"] == 2);
    CHECK(imported["statistics"]["historyWritten"] == 2);
    CHECK(imported["reviews"].empty());
    const nlohmann::json after = session.json({"identities", "list", "--system", "gb", "--json"});
    CHECK(identityIn(after, "Tiny Tennis (Europe) (Rev 1).gb")["id"] == tiny);
    CHECK(identityIn(after, "Moved/LOUD CART (USA).GB")["id"] == loud);
    // The gamelist entry left at the old path: a new GameId, its file unseen.
    const nlohmann::json left = identityIn(after, "Tiny Tennis (Europe).gb");
    CHECK(left["id"] == gameIdOf("gb", "Tiny Tennis (Europe).gb", 1));
    CHECK(left["state"] == "unseen");

    // `show`: the history of the move.
    const nlohmann::json shown = session.json({"identities", "show", tiny, "--json"});
    CHECK(shown["alias"].is_null());
    CHECK(shown["identity"]["relativePath"] == "Tiny Tennis (Europe) (Rev 1).gb");
    CHECK(shown["identity"]["quickHashState"] == "current");
    REQUIRE(shown["history"].size() == 1);
    CHECK(shown["history"][0]["event"] == "moved");
    CHECK(shown["history"][0]["reason"] == "quick-hash");
    CHECK(shown["history"][0]["fromPath"] == "Tiny Tennis (Europe).gb");
    const RunResult text = session.run({"identities", "show", tiny});
    CHECK(text.code == ExitCode::Ok);
    CHECK(contains(text.out, "moved (quick-hash): Tiny Tennis (Europe).gb -> Tiny Tennis (Europe) (Rev 1).gb"));
    CHECK(contains(text.out, "State:        present"));
    // The import after changes nothing.
    const nlohmann::json steady = session.json({"import", "--json"});
    CHECK(steady["statistics"]["identitiesWritten"] == 0);
    CHECK(steady["statistics"]["entriesWritten"] == 0);
    CHECK(steady["statistics"]["historyWritten"] == 0);
    CHECK(steady["statistics"]["quickHashesRead"] == 0);
}

TEST_CASE("cli/identities: identical copies of one name become a review row, resolved by pairing or kept separate",
          "[cli][identities]") {
    const LoggingReset loggingReset;
    const Session session("rv-cli-identities-review");
    REQUIRE(session.run({"import"}).code == ExitCode::Ok); // no quick hash stored
    // Collection A's Pocket Puzzle copied into two new folders, then deleted: two files of its size, time and name.
    copyRom(session, "gb/Collection A/Pocket Puzzle (World).gb", "gb/Collection C/Pocket Puzzle (World).gb");
    copyRom(session, "gb/Collection A/Pocket Puzzle (World).gb", "gb/Collection D/Pocket Puzzle (World).gb");
    fs::remove(session.rom("gb/Collection A/Pocket Puzzle (World).gb"));
    session.settle();
    const RunResult imported = session.run({"import"});
    INFO(imported.err);
    CHECK(imported.code == ExitCode::Ok);
    CHECK(contains(imported.out, "1 to review"));
    CHECK(contains(imported.out, "2 new files have its size, time and name"));

    const nlohmann::json reviews = session.json({"identities", "reviews", "--json"});
    CHECK(reviews["open"] == 1);
    REQUIRE(reviews["reviews"].size() == 1);
    const nlohmann::json& review = reviews["reviews"][0];
    const std::string vanished = gameIdOf("gb", "Collection A/Pocket Puzzle (World).gb");
    CHECK(review["game"] == vanished);
    CHECK(review["vanished"] == "Collection A/Pocket Puzzle (World).gb");
    REQUIRE(review["candidates"].size() == 2);
    CHECK(review["candidates"][0]["relativePath"] == "Collection C/Pocket Puzzle (World).gb");
    CHECK(review["candidates"][1]["relativePath"] == "Collection D/Pocket Puzzle (World).gb");
    const std::string chosen = review["candidates"][0]["game"].get<std::string>();
    const std::string id = std::to_string(review["review"].get<std::int64_t>());
    const RunResult text = session.run({"identities", "reviews"});
    CHECK(contains(text.out, fmt::format("Review {} (open", id)));
    CHECK(contains(text.out, fmt::format("candidate {}: Collection C/Pocket Puzzle (World).gb", chosen)));

    SECTION("paired with a candidate") {
        // Refusals first: an unknown row (1), an identity of another system (2).
        CHECK(session.run({"identities", "resolve", "999", "--keep-separate"}).code == ExitCode::ProblemsFound);
        CHECK(session.run({"identities", "resolve", id, "--with", gameIdOf("snes", "Alpha Quest (USA).sfc")}).code ==
              ExitCode::UsageError);
        const RunResult resolved = session.run({"identities", "resolve", id, "--with", chosen});
        INFO(resolved.err);
        CHECK(resolved.code == ExitCode::Ok);
        CHECK(contains(resolved.out, fmt::format("{} paired with the file of {}, which is its alias now", vanished,
                                                 chosen)));
        CHECK(contains(resolved.out, "The gamelist entry left at Collection A/Pocket Puzzle (World).gb has a new "
                                     "identity"));
        CHECK(session.json({"identities", "reviews", "--json"})["open"] == 0);
        const nlohmann::json shown = session.json({"identities", "show", chosen, "--json"});
        CHECK(shown["alias"]["game"] == vanished);
        CHECK(shown["identity"]["id"] == vanished);
        CHECK(shown["identity"]["relativePath"] == "Collection C/Pocket Puzzle (World).gb");
        CHECK(contains(session.run({"identities", "show", chosen}).out,
                       fmt::format("{} is an alias of {}", chosen, vanished)));
        // Resolved once; the import after changes nothing.
        CHECK(session.run({"identities", "resolve", id, "--keep-separate"}).code == ExitCode::ProblemsFound);
        const nlohmann::json steady = session.json({"import", "--json"});
        CHECK(steady["statistics"]["identitiesWritten"] == 0);
        CHECK(steady["statistics"]["entriesWritten"] == 0);
        CHECK(steady["moved"].empty());
        CHECK(steady["reviews"].empty());
    }
    SECTION("kept separate") {
        const nlohmann::json resolved = session.json({"identities", "resolve", id, "--keep-separate", "--json"});
        CHECK(resolved["review"]["resolution"] == "kept-separate");
        CHECK(resolved["moved"].is_null());
        const nlohmann::json all = session.json({"identities", "reviews", "--all", "--json"});
        CHECK(all["open"] == 0);
        REQUIRE(all["reviews"].size() == 1);
        CHECK(all["reviews"][0]["open"] == false);
        const nlohmann::json steady = session.json({"import", "--json"});
        CHECK(steady["statistics"]["identitiesWritten"] == 0);
        CHECK(steady["statistics"]["entriesWritten"] == 0);
    }
}

TEST_CASE("cli/identities: a file that cannot be read is reported and exits 1; the next pass hashes it",
          "[cli][identities]") {
    const LoggingReset loggingReset;
    const Session session("rv-cli-identities-denied");
    REQUIRE(session.run({"import"}).code == ExitCode::Ok);
    {
        auto denied = rv::test::DataReadDenied::apply(session.rom("snes/Beta Run (Europe).SMC"));
        REQUIRE(denied.has_value());
        const RunResult pass = session.run({"identities", "hash"});
        INFO(pass.err);
        CHECK(pass.code == ExitCode::ProblemsFound);
        CHECK(contains(pass.out, "Hashed 17 file(s)"));
        CHECK(contains(pass.out, "1 unreadable"));
        CHECK(contains(pass.out, "[warning] quick-hash-unreadable snes"));
    }
    const nlohmann::json rest = session.json({"identities", "hash", "--system", "snes", "--json"});
    CHECK(rest["hashed"] == 1);
    CHECK(rest["current"] == 5);
}

TEST_CASE("cli/identities: the pass and a resolution never run beside an import, also across processes",
          "[cli][identities]") {
    const LoggingReset loggingReset;
    const Session session("rv-cli-identities-lock");
    REQUIRE(session.run({"import"}).code == ExitCode::Ok);
    const fs::path cli = rv::test::testProgramsDir() / "retroverse-cli.exe";
    const auto child = [&](std::vector<std::string> arguments, std::string_view name) {
        rv::test::ChildRequest request;
        request.executable = cli;
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(session.data())});
        request.arguments = std::move(arguments);
        request.stdoutFile = session.dir().file(fmt::format("{}-stdout.txt", name));
        request.stderrFile = session.dir().file(fmt::format("{}-stderr.txt", name));
        request.timeout = std::chrono::minutes(2);
        return rv::test::runChild(request);
    };

    auto database = rv::catalog_db::Database::open(session.database());
    REQUIRE(database.has_value());
    auto store = rv::catalog_db::SqliteCatalogStore::open(*database);
    REQUIRE(store.has_value());
    {
        // Another holder imports.
        auto lock = (*store)->lockImport(rv::catalog::LibraryId());
        REQUIRE(lock.has_value());
        const RunResult refused = session.run({"identities", "hash"});
        CHECK(refused.code == ExitCode::RuntimeFailure);
        CHECK(contains(refused.err, "another import runs"));
        const RunResult resolve = session.run({"identities", "resolve", "1", "--keep-separate"});
        CHECK(resolve.code == ExitCode::RuntimeFailure);
        CHECK(contains(resolve.err, "another import runs"));
        if (fs::exists(cli)) {
            const auto pass = child({"identities", "hash"}, "pass");
            REQUIRE(pass.has_value());
            CHECK_FALSE(pass->timedOut);
            CHECK(pass->exitCode == 3);
            CHECK(contains(pass->standardError, "another import runs"));
        } else {
            WARN("retroverse-cli.exe is not built beside rv_tests.exe: the second process is not run");
        }
    }
    // The pass of this process holds the lock while it reads: the import of a second process is refused meanwhile.
    std::optional<int> importExit;
    rv::catalog::QuickHashPassOptions options;
    options.reader = [&](const fs::path& folder, std::string_view relativePath) {
        if (!importExit && fs::exists(cli)) {
            const auto import = child({"import"}, "import");
            REQUIRE(import.has_value());
            importExit = import->exitCode;
            CHECK(contains(import->standardError, "another import runs"));
        }
        return rv::catalog::readQuickHashBelow(folder, relativePath);
    };
    const auto pass = rv::catalog::runQuickHashPass(**store, rv::catalog::LibraryId(), options);
    REQUIRE(pass.has_value());
    CHECK(pass->hashed == 18);
    if (fs::exists(cli)) {
        CHECK(importExit == 3);
    }
    // Once the pass is done, the import runs.
    CHECK(session.run({"import"}).code == ExitCode::Ok);
}
