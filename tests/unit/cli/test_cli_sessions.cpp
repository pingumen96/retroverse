// retroverse-cli launch --game, assign and sessions (I2 step 12), through the dispatcher and as processes, on the
// standard ES-DE fixture imported into a user data directory whose emulators.json holds profiles of the fake emulator
// (tests/fake_emulator). RV_LAUNCH_ALLOWLIST lists the fakes each test starts, for its own duration
// (support/scoped_environment.hpp), so the automation guard is exercised. Every outcome is recorded with its session
// row: returned, a quick exit, an error exit, a loader error, a crash, a refusal by the guard, an executable that is not
// there and a game whose file is gone, both blocked before the launch; hostile and long-path arguments reach the fake
// byte-identical, through a profile and through an ES-DE command; nothing is killed (the wait limit, a launch not waited
// for, a CLI ended mid-game); a CLI process killed while its session is open leaves it to the next start, which closes it
// as interrupted, while the session of a CLI that still runs stays open; assign is checked against what exists, ranks
// before emulators.json and survives a restart and `db rebuild-index`; a session is recorded while another process holds
// the database's write lock (as a bulk import's write transaction holds it); the dry run of --game writes nothing.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/platform_win/win32_util.hpp>

#include "support/file_system.hpp"
#include "support/fixture.hpp"
#include "support/program_stub.hpp"
#include "support/scoped_environment.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using nlohmann::json;
using rv::catalog::GameId;
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

std::string utf8(const fs::path& path) {
    return rv::core::pathToUtf8(fs::path(path).make_preferred());
}

fs::path u8path(std::string_view text) {
    return rv::core::pathFromUtf8(text);
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// The build's fake emulator, with its DLLs beside it.
fs::path buildFake() {
    return rv::test::testProgramsDir() / "rv_fake_emulator.exe";
}

/// An emulator profile of the fake (or another executable) serving gb, snes and psx, without a window timeout (the
/// fake runs windowless).
json profile(std::string_view id, const fs::path& executable, std::vector<std::string> arguments,
             json variables = json::object()) {
    return json{{"id", id},
                {"executable", utf8(executable)},
                {"arguments", std::move(arguments)},
                {"windowTimeoutSeconds", 0},
                {"variables", std::move(variables)},
                {"systems", {{"gb", json::object()}, {"snes", json::object()}, {"psx", json::object()}}}};
}

/// The standard fixture recorded as the library of a user data directory outside it, imported, with fake emulator
/// profiles in its emulators.json:
///  - fake-return: runs 2.6 s, past the startup grace, then exits 0 (returned normally);
///  - fake-quick: exits 0 at once (a quick exit);
///  - fake-error: exits 3 at once (an early exit: failed to start);
///  - fake-loader: exits 0xC0000135 at once (STATUS_DLL_NOT_FOUND: a loader error, without the loader's dialog);
///  - fake-crash: an access violation (Windows' dialogs are off for it);
///  - fake-long: runs 6 s (waits that give up, launches not waited for);
///  - fake-unlisted: the fixture's copy of the fake, which the allow-list never names (refused by the guard);
///  - fake-missing: an executable that is not there (blocked: emulator not found);
///  - fake-args: writes what it got (--print-args) to args.json, the ROM and hostile literal arguments after "--";
///  - fake-snes: like fake-quick, serving snes only;
///  - fake-tree: a wrapper that starts a copy of itself running 2.6 s and exits at once, waited for as a process tree;
///  - fake-confirm: exits at once, its end the user's confirmation (a launcher stub whose emulator runs elsewhere);
///  - fake-broken: a program stub whose headers pass validateLaunchRequest but which Windows cannot load: CreateProcessW
///    fails (failed to start, start failed), whatever the machine's load.
/// The startup grace (2 s, counted from the moment the process runs, its DLLs loaded within it) decides between a quick
/// exit and a return, and between an early exit and a return with an error code: on a machine busy enough, the Debug
/// fake needs more than that to load. The tests that need a quick exit judge it by the session's own run time
/// (launchWithinGrace()).
struct SessionSetup {
    rv::test::TempDir dir;
    rv::test::Fixture fixture;
    fs::path data;
    fs::path argsFile;

    explicit SessionSetup(std::string_view prefix, const json& systemChoices = json::object())
        : dir(prefix), fixture(rv::test::Fixture::materialize("esde/standard").value()), data(dir.file("data")),
          argsFile(dir.file("args.json")) {
        rv::catalog::LibraryProfile library;
        library.esdeHome = fixture.path("ES-DE");
        library.installFolder = fixture.path("Programs/ES-DE");
        REQUIRE(rv::esde::writeLibraryFile(data / "config" / "library.json", library, rv::esde::ExistingFile::Keep)
                    .has_value());
        const fs::path fake = buildFake();
        REQUIRE(fs::exists(fake));
        json profiles = json::array();
        profiles.push_back(profile("fake-return", fake, {"--sleep-ms", "2600", "--exit-timeout-ms", "30000", "--",
                                                         "{rom}"}));
        profiles.push_back(profile("fake-quick", fake, {"--", "{rom}"}));
        profiles.push_back(profile("fake-error", fake, {"--exit-code", "3", "--", "{rom}"}));
        profiles.push_back(profile("fake-loader", fake, {"--exit-code", "0xC0000135", "--", "{rom}"}));
        profiles.push_back(profile("fake-crash", fake, {"--crash", "--", "{rom}"}));
        profiles.push_back(profile("fake-long", fake, {"--sleep-ms", "6000", "--exit-timeout-ms", "30000", "--",
                                                       "{rom}"}));
        profiles.push_back(profile("fake-unlisted",
                                   fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe"),
                                   {"--", "{rom}"}));
        profiles.push_back(profile("fake-missing", dir.file("missing/fake-missing.exe"), {"--", "{rom}"}));
        profiles.push_back(profile("fake-args", fake,
                                   {"--print-args", "{var:out}", "--", "{rom}", "say \"hi\"", "C:\\dir with space\\",
                                    "100% ^caret^ & | < > %PATH%", "日本語 ünïcödé", "trailing\\\\", "",
                                    "{{braces}}"},
                                   json{{"out", utf8(argsFile)}}));
        json snesOnly = profile("fake-snes", fake, {"--", "{rom}"});
        snesOnly["systems"] = json{{"snes", json::object()}};
        profiles.push_back(std::move(snesOnly));
        json tree = profile("fake-tree", fake, {"--spawn-child-and-exit", "--sleep-ms", "2600", "--exit-timeout-ms",
                                                "30000", "--", "{rom}"});
        tree["wait"] = "tree";
        profiles.push_back(std::move(tree));
        json confirm = profile("fake-confirm", fake, {"--", "{rom}"});
        confirm["wait"] = "confirm";
        profiles.push_back(std::move(confirm));
        profiles.push_back(profile("fake-broken", rv::test::writeProgramStub(brokenProgram()), {"--", "{rom}"}));
        json document{{"schemaVersion", 1}, {"profiles", std::move(profiles)}};
        if (!systemChoices.empty()) {
            document["systemChoices"] = systemChoices;
        }
        REQUIRE(rv::core::writeTextFileAtomic(emulatorsFile(), document.dump(2)).has_value());
        const RunResult imported = run({"import"});
        INFO(imported.err);
        REQUIRE(imported.code == ExitCode::Ok);
    }

    [[nodiscard]] fs::path emulatorsFile() const { return data / "config" / "emulators.json"; }
    [[nodiscard]] fs::path databaseFile() const { return data / "database" / "retroverse.sqlite"; }
    [[nodiscard]] fs::path brokenProgram() const { return dir.file("broken/broken-emulator.exe"); }

    RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(dir.path()));
        return RunResult{code, out.text(), err.text()};
    }

    /// Runs `arguments` with --json and parses the result, checking the exit code when one is expected.
    json runJson(std::vector<std::string> arguments, std::optional<ExitCode> expected) const {
        arguments.push_back("--json");
        const RunResult result = run(arguments);
        INFO(arguments[0] << ": " << result.err);
        if (expected) {
            CHECK(result.code == *expected);
        }
        REQUIRE_FALSE(result.out.empty());
        json parsed = json::parse(result.out);
        parsed["exitCodeOfTheRun"] = toInt(result.code);
        return parsed;
    }

    /// `launch --game <game> --wait --json` (and `extra`) until the session's run time is within the startup grace
    /// (with a margin), at most 4 times: on a machine busy enough, the Debug fake's start alone can pass it. Every
    /// launch made is appended to `made`; the last result is returned.
    json launchWithinGrace(const std::string& game, std::vector<json>& made,
                           const std::vector<std::string>& extra = {"--wait"}) const {
        json result;
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::vector<std::string> arguments{"launch", "--game", game};
            arguments.insert(arguments.end(), extra.begin(), extra.end());
            result = runJson(arguments, std::nullopt);
            made.push_back(result.at("session"));
            const json& duration = result.at("session").at("durationMs");
            if (!duration.is_null() && duration.get<std::int64_t>() < 1500) {
                break;
            }
        }
        return result;
    }

    /// The GameId of the identity at `relativePath` of `system`.
    [[nodiscard]] GameId gameAt(std::string_view system, std::string_view relativePath) const {
        auto rows = query("SELECT id FROM rv_game_identities WHERE system = ?1 AND relative_path = ?2",
                          {std::string(system), std::string(relativePath)});
        REQUIRE(rows.size() == 1);
        return GameId::fromValue(std::stoull(rows.front())).value();
    }

    /// The first column of `sql`'s rows as text, read through a reader of the database.
    [[nodiscard]] std::vector<std::string> query(std::string_view sql, const std::vector<std::string>& texts = {}) const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(databaseFile(), options);
        REQUIRE(database.has_value());
        auto reader = database->openReader();
        REQUIRE(reader.has_value());
        auto statement = reader->prepare(sql);
        REQUIRE(statement.has_value());
        for (std::size_t i = 0; i < texts.size(); ++i) {
            statement->bindText(static_cast<int>(i + 1), texts[i]);
        }
        std::vector<std::string> values;
        while (statement->step().value()) {
            values.emplace_back(statement->columnType(0) == rv::catalog_db::ColumnType::Integer
                                    ? std::to_string(statement->columnInt(0))
                                    : std::string(statement->columnText(0)));
        }
        return values;
    }

    [[nodiscard]] std::size_t sessionRows() const {
        return std::stoull(query("SELECT count(*) FROM rv_play_sessions").front());
    }
};

/// A process handle opened by id, to watch a game the CLI started (and must never end).
struct Watched {
    rv::platform_win::UniqueHandle handle;

    explicit Watched(std::uint32_t processId)
        : handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)) {}

    [[nodiscard]] bool running() const { return handle && WaitForSingleObject(handle.get(), 0) == WAIT_TIMEOUT; }
    /// Waits up to `timeout` for its end; its exit code, or nothing when it still runs.
    [[nodiscard]] std::optional<DWORD> waitForExit(std::chrono::milliseconds timeout) const {
        if (!handle || WaitForSingleObject(handle.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(handle.get(), &code);
        return code;
    }
};

/// retroverse-cli.exe started as a process of its own, its standard output and error in a file, not waited for.
struct ChildCli {
    rv::platform_win::UniqueHandle process;
    std::uint32_t processId = 0;

    static std::optional<ChildCli> start(const std::vector<std::string>& arguments, const fs::path& outputFile) {
        const fs::path cli = rv::test::testProgramsDir() / "retroverse-cli.exe";
        if (!fs::exists(cli)) {
            return std::nullopt;
        }
        auto commandLine = rv::launcher::buildCommandLine(rv::core::pathToUtf8(cli), arguments);
        REQUIRE(commandLine.has_value());
        std::wstring wide = rv::core::utf8ToWide(*commandLine).value();
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        rv::platform_win::UniqueHandle output(CreateFileW(outputFile.c_str(), GENERIC_WRITE,
                                                          FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                                                          FILE_ATTRIBUTE_NORMAL, nullptr));
        rv::platform_win::UniqueHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                         &inherit, OPEN_EXISTING, 0, nullptr));
        REQUIRE(output.valid());
        REQUIRE(input.valid());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input.get();
        startup.hStdOutput = output.get();
        startup.hStdError = output.get();
        PROCESS_INFORMATION information{};
        REQUIRE(CreateProcessW(cli.c_str(), wide.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup, &information) != FALSE);
        CloseHandle(information.hThread);
        ChildCli child;
        child.process.reset(information.hProcess);
        child.processId = information.dwProcessId;
        return child;
    }

    [[nodiscard]] std::optional<DWORD> waitForExit(std::chrono::milliseconds timeout) const {
        if (WaitForSingleObject(process.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(process.get(), &code);
        return code;
    }
};

/// Polls the database until `sql` (one integer column) gives a row, at most `timeout`; its value.
std::optional<std::int64_t> pollFor(const SessionSetup& setup, std::string_view sql, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto rows = setup.query(sql);
        if (!rows.empty()) {
            return std::stoll(rows.front());
        }
        std::this_thread::sleep_for(100ms);
    }
    return std::nullopt;
}

std::string text(const fs::path& file) {
    return rv::core::readTextFile(file).value_or(std::string());
}

} // namespace

TEST_CASE("cli/launch: every outcome is recorded with its session row", "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-outcomes");
    const std::vector<fs::path> listed{buildFake(), setup.brokenProgram()};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();
    std::vector<json> made; // every session launched, in order
    const auto launchWith = [&](std::string_view profile, std::optional<ExitCode> code) {
        INFO(profile);
        REQUIRE(setup.run({"assign", "--game", tiny, std::string(profile)}).code == ExitCode::Ok);
        const json result = setup.runJson({"launch", "--game", tiny, "--wait"}, code);
        made.push_back(result.at("session"));
        return result;
    };
    const auto common = [&](const json& result, std::string_view profile) {
        INFO(profile);
        CHECK(result.at("sessionRecorded") == true);
        const json& session = result.at("session");
        CHECK(session.at("game") == tiny);
        CHECK(session.at("choice").at("kind") == "profile");
        CHECK(session.at("choice").at("value") == std::string(profile));
        CHECK(session.at("choice").at("source") == "retroverse-game");
        CHECK(session.at("owner").at("processId") == GetCurrentProcessId());
        CHECK_FALSE(session.at("startedAt").is_null());
        CHECK_FALSE(session.at("endedAt").is_null());
    };

    // Returned normally: a clean exit after the startup grace, verified.
    const json returned = launchWith("fake-return", ExitCode::Ok);
    common(returned, "fake-return");
    CHECK(returned.at("session").at("outcome") == "returned");
    CHECK(returned.at("session").at("exitCode") == 0);
    CHECK(returned.at("session").at("quickExit") == false);
    CHECK(returned.at("session").at("durationMs").get<std::int64_t>() >= 2500);
    CHECK(returned.at("session").at("processId").get<std::uint32_t>() != 0);
    CHECK(returned.at("verified") == true);

    // A quick exit (returned at once, with the hint, exit 1), and an error exit within the grace (failed to start, early
    // exit): judged by the session's own run time, which the grace is compared with.
    REQUIRE(setup.run({"assign", "--game", tiny, "fake-quick"}).code == ExitCode::Ok);
    const json quick = setup.launchWithinGrace(tiny, made);
    common(quick, "fake-quick");
    CHECK(quick.at("session").at("outcome") == "returned");
    if (quick.at("session").at("durationMs").get<std::int64_t>() < 2000) {
        CHECK(quick.at("session").at("quickExit") == true);
        CHECK(contains(quick.at("session").at("outcomeText").get<std::string>(), "quick exit"));
        CHECK(quick.at("exitCodeOfTheRun") == toInt(ExitCode::ProblemsFound));
        CHECK(quick.at("verified") == false);
    } else {
        WARN("the machine is too busy: the fake's quick exit took longer than the startup grace");
        CHECK(quick.at("session").at("quickExit") == false);
    }
    REQUIRE(setup.run({"assign", "--game", tiny, "fake-error"}).code == ExitCode::Ok);
    const json error = setup.launchWithinGrace(tiny, made);
    common(error, "fake-error");
    CHECK(error.at("session").at("exitCode") == 3);
    CHECK(error.at("exitCodeOfTheRun") == toInt(ExitCode::ProblemsFound));
    if (error.at("session").at("durationMs").get<std::int64_t>() < 2000) {
        CHECK(error.at("session").at("outcome") == "failed-to-start");
        CHECK(error.at("session").at("failure") == "early exit");
    } else {
        WARN("the machine is too busy: the fake's error exit took longer than the startup grace");
        CHECK(error.at("session").at("outcome") == "returned"); // the program's own error code, after the grace
        CHECK(error.at("session").at("exitKind") == "error code");
    }

    // A loader error, a crash: whatever the time they take.
    const json loader = launchWith("fake-loader", ExitCode::ProblemsFound);
    common(loader, "fake-loader");
    CHECK(loader.at("session").at("outcome") == "failed-to-start");
    CHECK(loader.at("session").at("failure") == "loader error");
    CHECK(loader.at("session").at("exitKind") == "loader error");
    CHECK(loader.at("session").at("exitCode") == 0xC0000135u);
    const json crash = launchWith("fake-crash", ExitCode::ProblemsFound);
    common(crash, "fake-crash");
    CHECK(crash.at("session").at("outcome") == "crashed");
    CHECK(crash.at("session").at("exitCode") == 0xC0000005u);
    CHECK(contains(crash.at("session").at("exitDescription").get<std::string>(), "STATUS_ACCESS_VIOLATION"));

    // Failed to start: a program Windows cannot load (CreateProcessW fails), and one the automation guard refuses
    // (nothing created).
    const json broken = launchWith("fake-broken", ExitCode::ProblemsFound);
    common(broken, "fake-broken");
    CHECK(broken.at("started") == false);
    CHECK(broken.at("session").at("outcome") == "failed-to-start");
    CHECK(broken.at("session").at("failure") == "start failed");
    CHECK(broken.at("session").at("refusedByGuard") == false);
    const json refused = launchWith("fake-unlisted", ExitCode::ProblemsFound);
    common(refused, "fake-unlisted");
    CHECK(refused.at("started") == false);
    CHECK(refused.at("session").at("outcome") == "failed-to-start");
    CHECK(refused.at("session").at("refusedByGuard") == true);
    CHECK(refused.at("session").at("failure") == "start failed");
    CHECK(refused.at("session").at("processId") == 0);
    CHECK(refused.at("session").at("durationMs").is_null());
    CHECK(refused.at("guard").at("rule") == "not listed");

    // The executable not there: blocked before the launch, with its readiness.
    const json missingProgram = launchWith("fake-missing", ExitCode::ProblemsFound);
    common(missingProgram, "fake-missing");
    CHECK(missingProgram.at("started") == false);
    CHECK(missingProgram.at("session").at("outcome") == "blocked");
    CHECK(missingProgram.at("session").at("readiness") == "emulator-not-found");
    CHECK(missingProgram.at("session").at("processId") == 0);

    // A game whose file is gone: blocked (ROM missing) with its first command, recorded too.
    const std::string gone = setup.gameAt("gb", "Gone Fishing (USA).gb").toString();
    const json missing = setup.runJson({"launch", "--game", gone, "--wait"}, ExitCode::ProblemsFound);
    made.push_back(missing.at("session"));
    CHECK(missing.at("started") == false);
    CHECK(missing.at("readiness") == "rom-missing");
    CHECK(missing.at("session").at("outcome") == "blocked");
    CHECK(missing.at("session").at("readiness") == "rom-missing");
    CHECK(missing.at("session").at("choice").at("value") == "Fake Emulator");
    CHECK(missing.at("session").at("choice").at("source") == "first-command");
    CHECK(contains(missing.at("session").at("message").get<std::string>(), "rom-missing"));

    // The sessions, newest first, as recorded.
    CHECK(setup.sessionRows() == made.size());
    const json listedSessions = setup.runJson({"sessions", "--limit", "0"}, ExitCode::Ok);
    REQUIRE(listedSessions.at("sessions").size() == made.size());
    for (std::size_t i = 0; i < made.size(); ++i) {
        const json& row = listedSessions.at("sessions")[made.size() - 1 - i];
        CHECK(row.at("id") == made[i].at("id"));
        CHECK(row.at("outcome") == made[i].at("outcome"));
        CHECK(row.at("system") == "gb");
    }
    CHECK(listedSessions.at("sessions")[0].at("game") == gone);
    CHECK(listedSessions.at("sessions")[0].at("title") == "Gone Fishing");
    CHECK(listedSessions.at("sessions")[1].at("path") == "Tiny Tennis (Europe).gb");
    const json limited = setup.runJson({"sessions", "--game", tiny, "--limit", "2"}, ExitCode::Ok);
    REQUIRE(limited.at("sessions").size() == 2);
    CHECK(limited.at("sessions")[0].at("readiness") == "emulator-not-found");
    const RunResult textList = setup.run({"sessions", "--limit", "0"});
    CHECK(textList.code == ExitCode::Ok);
    CHECK(contains(textList.out, "failed to start: refused by the automation guard"));
    CHECK(contains(textList.out, "blocked before the launch: emulator not found"));
    CHECK(contains(textList.out, fmt::format("{} sessions.", made.size())));
    CHECK(setup.run({"sessions", "--game", "g0000000000000001"}).code == ExitCode::ProblemsFound);
    CHECK(setup.run({"sessions", "--limit", "-1"}).code == ExitCode::UsageError);

    // Only the launches that returned normally verified their choice for gb.
    std::vector<std::string> verified;
    for (const json& session : made) {
        if (session.at("outcome") == "returned" && session.at("exitCode") == 0 && session.at("quickExit") == false) {
            const std::string row = "profile " + session.at("choice").at("value").get<std::string>() + " gb 0";
            if (std::find(verified.begin(), verified.end(), row) == verified.end()) {
                verified.push_back(row);
            }
        }
    }
    std::sort(verified.begin(), verified.end());
    CHECK(setup.query("SELECT kind || ' ' || value || ' ' || system || ' ' || test FROM rv_launch_verifications "
                      "ORDER BY kind, value") == verified);
    CHECK(std::find(verified.begin(), verified.end(), "profile fake-return gb 0") != verified.end());

    SECTION("a launch test verifies the choice when the game returns normally, and only then") {
        REQUIRE(setup.run({"assign", "--game", tiny, "fake-return"}).code == ExitCode::Ok);
        const json test = setup.runJson({"launch", "--game", tiny, "--test"}, ExitCode::Ok);
        CHECK(test.at("waited") == true);
        CHECK(test.at("verified") == true);
        CHECK(test.at("session").at("test") == true);
        CHECK(setup.query("SELECT test FROM rv_launch_verifications WHERE value = 'fake-return'") ==
              std::vector<std::string>{"1"});
        REQUIRE(setup.run({"assign", "--game", tiny, "fake-crash"}).code == ExitCode::Ok);
        const RunResult notVerified = setup.run({"launch", "--game", tiny, "--test"});
        CHECK(notVerified.code == ExitCode::ProblemsFound);
        CHECK(contains(notVerified.out, "Verified            no: the game did not return normally"));
    }
}

TEST_CASE("cli/launch: hostile and long-path arguments reach the emulator byte-identical", "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    SessionSetup setup("rv-cli-hostile");
    // A ROM below folders that take its path past MAX_PATH, named with what command lines and shells mistake.
    const std::string folder = std::string(90, 'D') + "/" + std::string(90, 'E');
    const std::string relative = folder + "/Hostile 100% ^caret^ & 'quote' (Ünïcödé 日本) [!].gb";
    const fs::path rom = setup.fixture.path("roms/gb/" + relative);
    REQUIRE(rv::test::writeFileAnyLength(rom, std::string(1024, 'x')).has_value());
    REQUIRE(utf8(rom).size() > 260);
    REQUIRE(setup.run({"import"}).code == ExitCode::Ok);
    const std::string game = setup.gameAt("gb", relative).toString();

    const fs::path homeEmulator = setup.fixture.path("Emulators/HomeEmulator/home-emulator.exe");
    const std::vector<fs::path> listed{buildFake(), homeEmulator};
    const rv::test::ScopedLaunchAllowList allowList(listed);

    SECTION("through a profile") {
        REQUIRE(setup.run({"assign", "--game", game, "fake-args"}).code == ExitCode::Ok);
        const json dryRun = setup.runJson({"launch", "--dry-run", "--game", game}, ExitCode::Ok);
        // A quick exit (exit 1), or a return on a machine busy enough.
        const json launched = setup.runJson({"launch", "--game", game, "--wait"}, std::nullopt);
        CHECK(launched.at("session").at("outcome") == "returned");
        const json received = json::parse(text(setup.argsFile));
        const std::vector<std::string> expected{utf8(rom), "say \"hi\"", "C:\\dir with space\\",
                                                "100% ^caret^ & | < > %PATH%", "日本語 ünïcödé", "trailing\\\\", "",
                                                "{braces}"};
        CHECK(received.at("payload").get<std::vector<std::string>>() == expected);
        // argv as the fake's C runtime read it, equal to the dry run's request (argv[0] aside).
        std::vector<std::string> argv = received.at("argv").get<std::vector<std::string>>();
        std::vector<std::string> requested = dryRun.at("request").at("argv").get<std::vector<std::string>>();
        REQUIRE_FALSE(argv.empty());
        REQUIRE_FALSE(requested.empty());
        argv.erase(argv.begin());
        requested.erase(requested.begin());
        CHECK(argv == requested);
    }
    SECTION("through an ES-DE command") {
        // The fixture's Home Emulator command with --print-args (a copy of the fake that finds its DLLs through PATH).
        const fs::path esdeArgs = setup.dir.file("esde-args.json");
        REQUIRE(setup.fixture
                    .replaceText("Programs/ES-DE/resources/systems/windows/es_systems.xml",
                                 "%EMULATOR_HOME-EMULATOR% -- %ROM%",
                                 "%EMULATOR_HOME-EMULATOR% --print-args \"" + utf8(esdeArgs) + "\" -- %ROM%")
                    .has_value());
        const rv::test::ScopedPathPrefix path(rv::test::testProgramsDir());
        REQUIRE(setup.run({"assign", "--game", game, "Home Emulator"}).code == ExitCode::Ok);
        const json launched = setup.runJson({"launch", "--game", game, "--wait"}, std::nullopt);
        CHECK(launched.at("session").at("choice").at("kind") == "command");
        CHECK(launched.at("session").at("choice").at("value") == "Home Emulator");
        CHECK(launched.at("session").at("outcome") == "returned");
        const json received = json::parse(text(esdeArgs));
        const std::vector<std::string> payload = received.at("payload").get<std::vector<std::string>>();
        REQUIRE(payload.size() == 1);
        CHECK(u8path(payload.front()) == rom); // ES-DE's %ROM%: the ROM folder as configured, the path as listed
        CHECK(payload.front() == launched.at("request").at("argv").back().get<std::string>());
    }
}

TEST_CASE("cli/launch: nothing is killed: a wait that gives up and a launch not waited for leave the game running",
          "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-nokill");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();
    REQUIRE(setup.run({"assign", "gb", "fake-long"}).code == ExitCode::Ok);

    const auto before = std::chrono::steady_clock::now();
    const json limited = setup.runJson({"launch", "--game", tiny, "--wait", "--wait-limit", "2"}, ExitCode::Ok);
    const auto waited = std::chrono::steady_clock::now() - before;
    const json& first = limited.at("session");
    CHECK(first.at("outcome") == "left-running");
    CHECK(first.at("leftRunning") == "wait-limit");
    CHECK(first.at("processesRemained") == true);
    CHECK(first.at("confirmedByUser") == false);
    CHECK(first.at("choice").at("source") == "retroverse-system");
    CHECK(waited < 5s);
    const Watched limitedGame(first.at("processId").get<std::uint32_t>());
    CHECK(limitedGame.running());

    const json notWaited = setup.runJson({"launch", "--game", tiny}, ExitCode::Ok);
    const json& second = notWaited.at("session");
    CHECK(notWaited.at("waited") == false);
    CHECK(second.at("outcome") == "left-running");
    CHECK(second.at("leftRunning") == "not-waited");
    CHECK(second.at("durationMs").get<std::int64_t>() >= 2000);
    const Watched notWaitedGame(second.at("processId").get<std::uint32_t>());
    CHECK(notWaitedGame.running());

    // Both run to their own end: exit code 0 after their 6 s, never 124 (their exit timeout) nor a termination.
    CHECK(limitedGame.waitForExit(20s) == 0u);
    CHECK(notWaitedGame.waitForExit(20s) == 0u);
    const json listedSessions = setup.runJson({"sessions"}, ExitCode::Ok);
    CHECK(listedSessions.at("sessions").size() == 2);
    CHECK(listedSessions.at("interrupted").empty()); // left running is no interruption
}

TEST_CASE("cli/launch: --wait follows each wait strategy to its end", "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-strategies");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();

    // A wrapper waited for as a process tree: its quick exit is no end; the tree's end is.
    REQUIRE(setup.run({"assign", "--game", tiny, "fake-tree"}).code == ExitCode::Ok);
    const json tree = setup.runJson({"launch", "--game", tiny, "--wait"}, ExitCode::Ok);
    CHECK(tree.at("request").at("waitStrategy") == "ProcessTree");
    CHECK(tree.at("session").at("outcome") == "returned");
    CHECK(tree.at("session").at("quickExit") == false);
    CHECK(tree.at("session").at("durationMs").get<std::int64_t>() >= 2500);
    CHECK(tree.at("verified") == true);

    // A launcher stub whose end is the user's confirmation: not waited for, or a wait that gives up, leaves it running
    // (the confirmation never came); nothing is terminated either way.
    REQUIRE(setup.run({"assign", "--game", tiny, "fake-confirm"}).code == ExitCode::Ok);
    const json notWaited = setup.runJson({"launch", "--game", tiny}, ExitCode::Ok);
    CHECK(notWaited.at("session").at("outcome") == "left-running");
    CHECK(notWaited.at("session").at("leftRunning") == "not-waited");
    CHECK(notWaited.at("session").at("quickExit") == false);
    CHECK(notWaited.at("session").at("confirmedByUser") == false);
    const json limited = setup.runJson({"launch", "--game", tiny, "--wait", "--wait-limit", "1"}, ExitCode::Ok);
    CHECK(limited.at("session").at("outcome") == "left-running");
    CHECK(limited.at("session").at("leftRunning") == "wait-limit");
    CHECK(limited.at("verified") == false);
}

TEST_CASE("cli/launch: a CLI ended mid-game leaves its session to the next start, which closes it as interrupted; "
          "a session whose owner runs stays open",
          "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-recovery");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed); // inherited by the CLI processes
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();
    const std::string cafe = setup.gameAt("gb", "Café Racer (Europe).gb").toString();
    // Per game: Café Racer's <altemulator> ranks before a choice for gb (ADR-0008 §8).
    REQUIRE(setup.run({"assign", "--game", tiny, "fake-long"}).code == ExitCode::Ok);
    REQUIRE(setup.run({"assign", "--game", cafe, "fake-long"}).code == ExitCode::Ok);

    // Two CLIs that wait for a game of 6 s: A ends normally, B is killed mid-game.
    const auto a = ChildCli::start({"--data-dir", utf8(setup.data), "launch", "--game", cafe, "--wait"},
                                   setup.dir.file("a.txt"));
    if (!a) {
        WARN("retroverse-cli.exe is not built beside rv_tests.exe: the CLI processes are not run");
        return;
    }
    const auto b = ChildCli::start({"--data-dir", utf8(setup.data), "launch", "--game", tiny, "--wait"},
                                   setup.dir.file("b.txt"));
    REQUIRE(b.has_value());
    const auto open = [&](const ChildCli& child) {
        return pollFor(setup,
                       fmt::format("SELECT id FROM rv_play_sessions WHERE owner_pid = {} AND outcome = 'running' AND "
                                   "process_id <> 0",
                                   child.processId),
                       30s);
    };
    const auto sessionA = open(*a);
    const auto sessionB = open(*b);
    INFO("A: " << text(setup.dir.file("a.txt")));
    INFO("B: " << text(setup.dir.file("b.txt")));
    REQUIRE(sessionA.has_value());
    REQUIRE(sessionB.has_value());
    const auto gameOf = [&](std::int64_t session) {
        return Watched(static_cast<std::uint32_t>(
            std::stoul(setup.query(fmt::format("SELECT process_id FROM rv_play_sessions WHERE id = {}", session))
                           .front())));
    };
    const Watched gameB = gameOf(*sessionB);
    REQUIRE(gameB.running());

    // B's CLI ends mid-game (killed): its game runs on; its session is still open, its owner gone.
    REQUIRE(TerminateProcess(b->process.get(), 1) != FALSE);
    REQUIRE(b->waitForExit(10s).has_value());
    CHECK(gameB.running());

    // The next start (sessions, here) closes B's session as interrupted, and only it: A's CLI still runs.
    const json listedSessions = setup.runJson({"sessions"}, ExitCode::Ok);
    REQUIRE(listedSessions.at("interrupted").size() == 1);
    CHECK(listedSessions.at("interrupted")[0].at("id") == *sessionB);
    CHECK(setup.query(fmt::format("SELECT outcome FROM rv_play_sessions WHERE id = {}", *sessionB)) ==
          std::vector<std::string>{"interrupted"});
    CHECK(setup.query(fmt::format("SELECT outcome FROM rv_play_sessions WHERE id = {}", *sessionA)) ==
          std::vector<std::string>{"running"});
    CHECK_FALSE(setup.query(fmt::format("SELECT closed_at FROM rv_play_sessions WHERE id = {}", *sessionB))
                    .front()
                    .empty());
    CHECK(gameB.running()); // closing the record ends nothing

    // A ends normally and records its game's return.
    const auto exitA = a->waitForExit(60s);
    REQUIRE(exitA.has_value());
    INFO(text(setup.dir.file("a.txt")));
    CHECK(*exitA == 0u);
    CHECK(setup.query(fmt::format("SELECT outcome FROM rv_play_sessions WHERE id = {}", *sessionA)) ==
          std::vector<std::string>{"returned"});
    CHECK(setup.runJson({"sessions"}, ExitCode::Ok).at("interrupted").empty());
    // B's game ends by itself.
    CHECK(gameB.waitForExit(20s) == 0u);
}

TEST_CASE("cli/assign: choices are checked against what exists, rank before emulators.json, and survive a restart "
          "and db rebuild-index",
          "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-assign", json{{"gb", {{"command", "Home Emulator"}}}});
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();
    const std::string track = setup.gameAt("psx", "Twin Tracks (Europe) (Track 1).bin").toString();
    const std::string userFile = text(setup.emulatorsFile());
    const std::string libraryFile = text(setup.data / "config" / "library.json");

    // Nothing assigned: emulators.json's choice for gb is in effect.
    json shown = setup.runJson({"assign", "gb"}, ExitCode::Ok);
    CHECK(shown.at("action") == "show");
    CHECK(shown.at("choice").is_null());
    CHECK(shown.at("emulatorsJsonChoice").at("value") == "Home Emulator");
    CHECK(shown.at("commands").size() == 4);
    json profiles = setup.runJson({"profiles", "--system", "gb"}, ExitCode::Ok);
    CHECK(profiles.at("systems")[0].at("chosen").at("value") == "Home Emulator");
    CHECK(profiles.at("systems")[0].at("chosen").at("origin") == "emulators.json");

    // Checked against what exists.
    CHECK(setup.run({"assign", "gb", "No Such Emulator"}).code == ExitCode::UsageError);
    CHECK(setup.run({"assign", "gb", "--profile", "nope"}).code == ExitCode::UsageError);
    CHECK(setup.run({"assign", "gb", "--command", "fake-return"}).code == ExitCode::UsageError);
    const RunResult notServing = setup.run({"assign", "gb", "fake-snes"});
    CHECK(notServing.code == ExitCode::UsageError);
    CHECK(contains(notServing.err, "neither a command of gb nor a profile serving it"));
    CHECK(contains(setup.run({"assign", "gb", "--profile", "fake-snes"}).err, "does not serve the system gb"));
    CHECK(setup.run({"assign", "vectrex", "fake-quick"}).code == ExitCode::UsageError);
    CHECK(setup.run({"assign", "--game", "g0000000000000001", "fake-quick"}).code == ExitCode::ProblemsFound);
    CHECK(setup.run({"assign", "--game", track, "fake-quick"}).code == ExitCode::UsageError); // a disc member
    CHECK(setup.run({"assign", "gb", "fake-quick", "--clear"}).code == ExitCode::UsageError);
    CHECK(setup.run({"assign"}).code == ExitCode::UsageError);
    CHECK(setup.query("SELECT count(*) FROM rv_launch_system_choices") == std::vector<std::string>{"0"});

    // A system's choice and a game's, stored in the catalog database.
    json set = setup.runJson({"assign", "gb", "fake-return"}, ExitCode::Ok);
    CHECK(set.at("action") == "set");
    CHECK(set.at("changed") == true);
    CHECK(set.at("choice").at("kind") == "profile");
    CHECK(set.at("readiness") == "ready");
    set = setup.runJson({"assign", "gb", "fake-return"}, ExitCode::Ok);
    CHECK(set.at("changed") == false);
    REQUIRE(setup.run({"assign", "--game", tiny, "Fake Emulator (Game Folder)"}).code == ExitCode::Ok);
    profiles = setup.runJson({"profiles", "--system", "gb"}, ExitCode::Ok);
    CHECK(profiles.at("systems")[0].at("chosen").at("value") == "fake-return");
    CHECK(profiles.at("systems")[0].at("chosen").at("origin") == "assign"); // before emulators.json's
    CHECK(profiles.at("assignedChoices").at("gb").at("value") == "fake-return");

    // A restart and a rebuild of the index keep them.
    REQUIRE(setup.run({"db", "rebuild-index"}).code == ExitCode::Ok);
    CHECK(setup.runJson({"assign", "gb"}, ExitCode::Ok).at("choice").at("value") == "fake-return");
    shown = setup.runJson({"assign", "--game", tiny}, ExitCode::Ok);
    CHECK(shown.at("choice").at("value") == "Fake Emulator (Game Folder)");
    CHECK(shown.at("game").at("path") == "Tiny Tennis (Europe).gb");
    json dryRun = setup.runJson({"launch", "--dry-run", "--game", tiny}, ExitCode::Ok);
    CHECK(dryRun.at("chosen").at("source") == "retroverse-game");
    CHECK(dryRun.at("chosen").at("origin") == "assign");
    CHECK(dryRun.at("chosen").at("value") == "Fake Emulator (Game Folder)");
    dryRun = setup.runJson({"launch", "--dry-run", "--system", "gb", "--path", "LOUD CART (USA).GB"}, ExitCode::Ok);
    CHECK(dryRun.at("chosen").at("source") == "retroverse-system");
    CHECK(dryRun.at("chosen").at("value") == "fake-return");
    if (const auto child = ChildCli::start({"--data-dir", utf8(setup.data), "assign", "gb", "--json"},
                                           setup.dir.file("restart.txt"))) {
        REQUIRE(child->waitForExit(60s) == 0u);
        CHECK(json::parse(text(setup.dir.file("restart.txt"))).at("choice").at("value") == "fake-return");
    } else {
        WARN("retroverse-cli.exe is not built beside rv_tests.exe: the restart is not run as a process");
    }

    // Cleared: emulators.json's choice applies again.
    json cleared = setup.runJson({"assign", "gb", "--clear"}, ExitCode::Ok);
    CHECK(cleared.at("changed") == true);
    CHECK(cleared.at("previous").at("value") == "fake-return");
    CHECK(setup.runJson({"assign", "gb", "--clear"}, ExitCode::Ok).at("changed") == false);
    profiles = setup.runJson({"profiles", "--system", "gb"}, ExitCode::Ok);
    CHECK(profiles.at("systems")[0].at("chosen").at("origin") == "emulators.json");
    REQUIRE(setup.run({"assign", "--game", tiny, "--clear"}).code == ExitCode::Ok);
    CHECK(setup.query("SELECT count(*) FROM rv_launch_game_choices") == std::vector<std::string>{"0"});

    // A value that is both a label of the system and a profile serving it: --command or --profile says which.
    REQUIRE(setup.fixture
                .replaceText("Programs/ES-DE/resources/systems/windows/es_systems.xml",
                             "<command label=\"Home Emulator\">", "<command label=\"fake-quick\">")
                .has_value());
    const RunResult ambiguous = setup.run({"assign", "gb", "fake-quick"});
    CHECK(ambiguous.code == ExitCode::UsageError);
    CHECK(contains(ambiguous.err, "--command or --profile"));
    CHECK(setup.runJson({"assign", "gb", "--command", "fake-quick"}, ExitCode::Ok).at("choice").at("kind") ==
          "command");
    CHECK(setup.runJson({"assign", "gb", "--profile", "fake-quick"}, ExitCode::Ok).at("choice").at("kind") ==
          "profile");
    // A label that disappears later blocks the launch, naming it (a Retroverse choice is never skipped).
    REQUIRE(setup.run({"assign", "gb", "--command", "fake-quick"}).code == ExitCode::Ok);
    REQUIRE(setup.fixture
                .replaceText("Programs/ES-DE/resources/systems/windows/es_systems.xml",
                             "<command label=\"fake-quick\">", "<command label=\"Home Emulator\">")
                .has_value());
    const json stale = setup.runJson({"launch", "--game", tiny}, ExitCode::ProblemsFound);
    CHECK(stale.at("readiness") == "requires-configuration");
    CHECK(stale.at("blockers")[0].at("code") == "choice-stale");
    CHECK(stale.at("session").at("outcome") == "blocked");

    // The user's files are never written.
    CHECK(text(setup.emulatorsFile()) == userFile);
    CHECK(text(setup.data / "config" / "library.json") == libraryFile);
}

TEST_CASE("cli/launch: a session is recorded while another process holds the database's write lock",
          "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-busy");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();
    REQUIRE(setup.run({"assign", "gb", "fake-quick"}).code == ExitCode::Ok);

    // Another process's import: its write transaction (BEGIN IMMEDIATE) held for 4 s, longer than the launch's busy
    // timeout (1 s) and than the game.
    auto importer = rv::catalog_db::openCatalogDatabase(setup.databaseFile());
    REQUIRE(importer.has_value());
    std::optional<rv::catalog_db::Transaction> held =
        rv::catalog_db::Transaction::begin(importer->writer(), rv::catalog_db::Transaction::Mode::Immediate).value();
    bool committed = false;
    std::thread release([&] {
        std::this_thread::sleep_for(4s);
        committed = held->commit().has_value();
    });
    const auto before = std::chrono::steady_clock::now();
    const RunResult result = setup.run({"launch", "--game", tiny, "--wait", "--json"});
    const auto took = std::chrono::steady_clock::now() - before;
    release.join();
    CHECK(committed);
    INFO(result.err);
    const json launched = json::parse(result.out);
    // A quick exit (exit 1), or a return on a machine busy enough (exit 0): never a session lost (exit 3).
    CHECK(result.code ==
          (launched.at("session").at("quickExit") == true ? ExitCode::ProblemsFound : ExitCode::Ok));
    CHECK(launched.at("started") == true);
    CHECK(launched.at("sessionRecorded") == true);
    CHECK(took >= 3500ms); // the session's end waited for the import's commit, rather than being lost
    const std::int64_t id = launched.at("session").at("id").get<std::int64_t>();
    CHECK(setup.query(fmt::format("SELECT outcome FROM rv_play_sessions WHERE id = {}", id)) ==
          std::vector<std::string>{"returned"});
    CHECK(setup.sessionRows() == 1);
}

TEST_CASE("cli/launch: --dry-run --game resolves a game of the catalog and writes nothing", "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-dryrun-game");
    const std::string cafe = setup.gameAt("gb", "Café Racer (Europe).gb").toString();
    const std::string gone = setup.gameAt("gb", "Gone Fishing (USA).gb").toString();
    const std::string folder = setup.gameAt("gb", "Collection A").toString();
    const std::string sha = rv::core::sha256Hex(rv::core::readFileShared(setup.databaseFile(), 1ull << 30).value());

    const json ready = setup.runJson({"launch", "--dry-run", "--game", cafe}, ExitCode::Ok);
    CHECK(ready.at("game") == cafe);
    CHECK(ready.at("listedPath") == "Café Racer (Europe).gb");
    CHECK(ready.at("title") == "Café Racer");
    CHECK(ready.at("chosen").at("source") == "esde-game"); // its <altemulator>, as the import read it
    CHECK(ready.at("started") == false);
    const json blocked = setup.runJson({"launch", "--dry-run", "--game", gone}, ExitCode::ProblemsFound);
    CHECK(blocked.at("readiness") == "rom-missing");
    CHECK(setup.run({"launch", "--dry-run", "--game", folder}).code == ExitCode::UsageError); // a folder
    CHECK(setup.run({"launch", "--dry-run", "--game", "g0000000000000001"}).code == ExitCode::ProblemsFound);
    const RunResult text = setup.run({"launch", "--dry-run", "--game", cafe});
    CHECK(contains(text.out, "Game                " + cafe + ": gb \"Café Racer (Europe).gb\""));
    CHECK(contains(text.out, "Dry run: nothing was started."));
    CHECK(setup.sessionRows() == 0);
    CHECK(rv::core::sha256Hex(rv::core::readFileShared(setup.databaseFile(), 1ull << 30).value()) == sha);
}
