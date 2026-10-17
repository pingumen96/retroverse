// The M1 flow of retroverse-cli with every command a process of its own (I2 step 14, Gate M1): the standard ES-DE
// fixture recorded with `library create`, imported, imported again (which writes nothing), its index rebuilt and the
// database checked, a search, `show`, `assign` and `launch --game --wait` into the build's fake emulator, then
// `sessions` and another check. Each command runs as retroverse-cli.exe, its results on stdout as JSON, its logs on
// stderr, with RV_LAUNCH_BACKGROUND=1 (nothing it starts takes the foreground) and RV_LAUNCH_ALLOWLIST naming the
// build's fake emulator alone, so the fixture's own copies could never start. The fixture is the library: its files,
// their sizes, times and contents, are the same after the flow as before it (NFR-10 on the fixture). scripts/gate.ps1
// -Gate M1 runs this case as a step of its own ("cli/process"); the dispatcher-level tests of each command are in the
// other test_cli_*.cpp files.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>

#include "support/child_process.hpp"
#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string utf8(const fs::path& path) {
    return rv::core::pathToUtf8(fs::path(path).make_preferred());
}

/// What a file of the fixture is: its size, last write time and SHA-256, or "folder".
using TreeState = std::map<std::string, std::string>;

TreeState treeState(const fs::path& root) {
    TreeState state;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const std::string relative = utf8(fs::relative(it->path(), root));
        if (it->is_directory()) {
            state[relative] = "folder";
            continue;
        }
        const auto bytes = rv::core::readBinaryFile(it->path());
        REQUIRE(bytes.has_value());
        const auto time = rv::core::lastWriteTime(it->path());
        REQUIRE(time.has_value());
        state[relative] = fmt::format("{} bytes, time {}, sha256 {}", bytes->size(), time->time_since_epoch().count(),
                                      rv::core::sha256Hex(*bytes));
    }
    REQUIRE_FALSE(error);
    return state;
}

/// One run of retroverse-cli.exe: its exit code and its standard output parsed as JSON (null when it wrote none).
struct CliRun {
    int exitCode = -1;
    json result;
    std::string standardError;
};

/// The flow's folders: the materialized fixture (the library) and a user data directory outside it.
struct Flow {
    rv::test::TempDir dir{"rv-cli-m1-flow"};
    rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/standard").value();
    fs::path data = dir.file("data");
    fs::path fake = rv::test::testProgramsDir() / "rv_fake_emulator.exe";
    int runs = 0;

    /// retroverse-cli --data-dir <data> `arguments` as a process; its stdout and stderr kept apart in files.
    CliRun cli(std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(data)});
        rv::test::ChildRequest request;
        request.executable = rv::test::testProgramsDir() / "retroverse-cli.exe";
        request.arguments = arguments;
        request.workingDirectory = dir.path();
        ++runs;
        request.stdoutFile = dir.file(fmt::format("run{:02}.out", runs));
        request.stderrFile = dir.file(fmt::format("run{:02}.err", runs));
        // Automated: whatever it starts runs in the background; only the build's fake may start.
        request.environment = {{"RV_LAUNCH_BACKGROUND", std::string("1")},
                               {"RV_LAUNCH_ALLOWLIST", utf8(fs::absolute(fake))}};
        request.timeout = std::chrono::minutes(2);
        const auto outcome = rv::test::runChild(request);
        REQUIRE(outcome.has_value());
        REQUIRE_FALSE(outcome->timedOut);
        CliRun run;
        run.exitCode = outcome->exitCode;
        run.standardError = outcome->standardError;
        if (!outcome->standardOutput.empty()) {
            run.result = json::parse(outcome->standardOutput, nullptr, false);
            INFO(outcome->standardOutput);
            REQUIRE_FALSE(run.result.is_discarded());
        }
        return run;
    }

    /// `arguments` with --json, which must exit 0.
    json ok(std::vector<std::string> arguments) {
        const std::string name = arguments.front();
        arguments.push_back("--json");
        const CliRun run = cli(std::move(arguments));
        INFO(name << ": " << run.standardError);
        REQUIRE(run.exitCode == 0);
        return run.result;
    }
};

} // namespace

TEST_CASE("cli/process: the M1 flow on the standard fixture, every command a process of its own", "[cli][process]") {
    Flow flow;
    REQUIRE(fs::exists(flow.fake));
    if (!fs::exists(rv::test::testProgramsDir() / "retroverse-cli.exe")) {
        FAIL("retroverse-cli.exe is not built beside rv_tests.exe");
    }
    const TreeState before = treeState(flow.fixture.root());

    // library create: the fixture's ES-DE home under its home folder, the install folder given; the roots from its
    // settings.
    const json library = flow.ok({"library", "create", "--home", utf8(flow.fixture.root()), "--install",
                                  utf8(flow.fixture.path("Programs/ES-DE"))});
    CHECK(library.at("id") == "esde");
    REQUIRE(fs::exists(flow.data / "config" / "library.json"));

    // The emulators.json of the data directory: a profile of the build's fake, which returns after 2.6 s.
    const json profile{{"id", "fake-return"},
                       {"executable", utf8(flow.fake)},
                       {"arguments", {"--sleep-ms", "2600", "--exit-timeout-ms", "30000", "--", "{rom}"}},
                       {"windowTimeoutSeconds", 0},
                       {"hideConsoleWindow", true},
                       {"systems", {{"gb", json::object()}}}};
    const json emulators{{"schemaVersion", 1}, {"profiles", json::array({profile})}};
    REQUIRE(rv::core::writeTextFileAtomic(flow.data / "config" / "emulators.json", emulators.dump(2) + "\n")
                .has_value());

    // The first import: every indexed entry of the fixture a new identity (its README: 22, the entry outside its
    // system folder is never indexed).
    const json first = flow.ok({"import"});
    CHECK(first.at("kind") == "import");
    CHECK(first.at("firstImport") == true);
    CHECK(first.at("totals").at("newIdentities") == 22);
    CHECK(first.at("statistics").at("identitiesWritten") == 22);
    CHECK(first.at("statistics").at("entriesWritten") == 22);

    // The second import writes nothing: no identity, entry, disc group, history or review row.
    const json second = flow.ok({"import"});
    CHECK(second.at("firstImport") == false);
    CHECK(second.at("totals").at("newIdentities") == 0);
    CHECK(second.at("totals").at("unchanged") == 22);
    const json& written = second.at("statistics");
    for (const char* key : {"identitiesWritten", "entriesWritten", "entriesRemoved", "discGroupsWritten",
                            "discGroupsRemoved", "historyWritten", "reviewsWritten"}) {
        INFO(key);
        CHECK(written.at(key) == 0);
    }

    // db rebuild-index: every row written again, no identity.
    const json rebuilt = flow.ok({"db", "rebuild-index"});
    CHECK(rebuilt.at("kind") == "rebuild-index");
    CHECK(rebuilt.at("statistics").at("identitiesWritten") == 0);
    CHECK(rebuilt.at("statistics").at("entriesWritten") == 22);
    flow.ok({"db", "check"});

    // search: the systems' full names are searched too ("Nintendo Game Boy", "Nintendo SNES"): the gb and snes games.
    const json nintendo = flow.ok({"search", "nintendo"});
    CHECK(nintendo.at("count") == 13);
    const json tennis = flow.ok({"search", "tiny tennis"});
    REQUIRE(tennis.at("count") == 1);
    const std::string game = tennis.at("games").at(0).at("id").get<std::string>();
    const json shown = flow.ok({"show", game});
    CHECK(shown.dump().find("Tiny Tennis") != std::string::npos);

    // assign, then the launch waited for: the fake returns normally, the session is recorded and verified.
    flow.ok({"assign", "gb", "--profile", "fake-return"});
    const json launched = flow.ok({"launch", "--game", game, "--wait"});
    CHECK(launched.at("sessionRecorded") == true);
    CHECK(launched.at("verified") == true);
    const json& session = launched.at("session");
    CHECK(session.at("game") == game);
    CHECK(session.at("outcome") == "returned");
    CHECK(session.at("exitCode") == 0);
    CHECK(session.at("choice").at("value") == "fake-return");
    CHECK(session.at("durationMs").get<std::int64_t>() >= 2500);

    // sessions lists it; the database is still sound.
    const json sessions = flow.ok({"sessions"});
    CHECK(sessions.dump().find(game) != std::string::npos);
    CHECK(sessions.dump().find("returned") != std::string::npos);
    flow.ok({"db", "check"});

    // Nothing of the library was written: the fixture's files as they were.
    CHECK(treeState(flow.fixture.root()) == before);
}
