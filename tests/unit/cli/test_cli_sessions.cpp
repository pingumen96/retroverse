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
///  - fake-snes: like fake-quick, serving snes only.
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

    RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(dir.path()));
        return RunResult{code, out.text(), err.text()};
    }

    /// Runs `arguments` with --json and parses the result, checking the exit code.
    json runJson(std::vector<std::string> arguments, ExitCode expected) const {
        arguments.push_back("--json");
        const RunResult result = run(arguments);
        INFO(arguments[0] << ": " << result.err);
        CHECK(result.code == expected);
        REQUIRE_FALSE(result.out.empty());
        return json::parse(result.out);
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
    const fs::path fake = buildFake();
    const std::vector<fs::path> listed{fake};
    const rv::test::ScopedLaunchAllowList allowList(listed);
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();

    struct Case {
        std::string_view profile;
        ExitCode code;
        std::string_view outcome;
    };
    // Each launch of Tiny Tennis with the profile assigned to it.
    const std::vector<Case> cases{
        {"fake-return", ExitCode::Ok, "returned"},
        {"fake-quick", ExitCode::ProblemsFound, "returned"},
        {"fake-error", ExitCode::ProblemsFound, "failed-to-start"},
        {"fake-loader", ExitCode::ProblemsFound, "failed-to-start"},
        {"fake-crash", ExitCode::ProblemsFound, "crashed"},
        {"fake-unlisted", ExitCode::ProblemsFound, "failed-to-start"},
        {"fake-missing", ExitCode::ProblemsFound, "blocked"},
    };
    std::vector<json> sessions;
    for (const Case& launch : cases) {
        INFO(launch.profile);
        REQUIRE(setup.run({"assign", "--game", tiny, std::string(launch.profile)}).code == ExitCode::Ok);
        const json result = setup.runJson({"launch", "--game", tiny, "--wait"}, launch.code);
        CHECK(result.at("sessionRecorded") == true);
        const json& session = result.at("session");
        CHECK(session.at("outcome") == std::string(launch.outcome));
        CHECK(session.at("game") == tiny);
        CHECK(session.at("choice").at("kind") == "profile");
        CHECK(session.at("choice").at("value") == std::string(launch.profile));
        CHECK(session.at("choice").at("source") == "retroverse-game");
        CHECK(session.at("owner").at("processId") == GetCurrentProcessId());
        CHECK_FALSE(session.at("startedAt").is_null());
        CHECK_FALSE(session.at("endedAt").is_null());
        sessions.push_back(session);
    }
    // Returned normally: a clean exit after the startup grace, verified.
    CHECK(sessions[0].at("exitCode") == 0);
    CHECK(sessions[0].at("quickExit") == false);
    CHECK(sessions[0].at("durationMs").get<std::int64_t>() >= 2500);
    CHECK(sessions[0].at("processId").get<std::uint32_t>() != 0);
    // A quick exit: returned at once, with the hint.
    CHECK(sessions[1].at("quickExit") == true);
    CHECK(contains(sessions[1].at("outcomeText").get<std::string>(), "quick exit"));
    // An error exit within the grace, and a loader error: failed to start, with the exit code.
    CHECK(sessions[2].at("failure") == "early exit");
    CHECK(sessions[2].at("exitCode") == 3);
    CHECK(sessions[3].at("failure") == "loader error");
    CHECK(sessions[3].at("exitKind") == "loader error");
    CHECK(sessions[3].at("exitCode") == 0xC0000135u);
    // A crash.
    CHECK(sessions[4].at("exitCode") == 0xC0000005u);
    CHECK(contains(sessions[4].at("exitDescription").get<std::string>(), "STATUS_ACCESS_VIOLATION"));
    // Refused by the automation guard: nothing created.
    CHECK(sessions[5].at("refusedByGuard") == true);
    CHECK(sessions[5].at("failure") == "start failed");
    CHECK(sessions[5].at("processId") == 0);
    CHECK(sessions[5].at("durationMs").is_null());
    // The executable not there: blocked before the launch, with its readiness.
    CHECK(sessions[6].at("readiness") == "emulator-not-found");
    CHECK(sessions[6].at("processId") == 0);

    // A game whose file is gone: blocked (ROM missing) with its first command, recorded too.
    const std::string gone = setup.gameAt("gb", "Gone Fishing (USA).gb").toString();
    const json missing = setup.runJson({"launch", "--game", gone, "--wait"}, ExitCode::ProblemsFound);
    CHECK(missing.at("started") == false);
    CHECK(missing.at("readiness") == "rom-missing");
    CHECK(missing.at("session").at("outcome") == "blocked");
    CHECK(missing.at("session").at("readiness") == "rom-missing");
    CHECK(missing.at("session").at("choice").at("value") == "Fake Emulator");
    CHECK(missing.at("session").at("choice").at("source") == "first-command");
    CHECK(contains(missing.at("session").at("message").get<std::string>(), "rom-missing"));

    // The sessions, newest first, as recorded.
    CHECK(setup.sessionRows() == cases.size() + 1);
    const json listedSessions = setup.runJson({"sessions"}, ExitCode::Ok);
    REQUIRE(listedSessions.at("sessions").size() == cases.size() + 1);
    CHECK(listedSessions.at("sessions")[0].at("game") == gone);
    CHECK(listedSessions.at("sessions")[0].at("title") == "Gone Fishing");
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const json& row = listedSessions.at("sessions")[cases.size() - i];
        CHECK(row.at("id") == sessions[i].at("id"));
        CHECK(row.at("outcome") == sessions[i].at("outcome"));
        CHECK(row.at("system") == "gb");
        CHECK(row.at("path") == "Tiny Tennis (Europe).gb");
    }
    const json limited = setup.runJson({"sessions", "--game", tiny, "--limit", "2"}, ExitCode::Ok);
    REQUIRE(limited.at("sessions").size() == 2);
    CHECK(limited.at("sessions")[0].at("outcome") == "blocked");
    const RunResult textList = setup.run({"sessions"});
    CHECK(textList.code == ExitCode::Ok);
    CHECK(contains(textList.out, "failed to start: refused by the automation guard"));
    CHECK(contains(textList.out, "blocked before the launch: emulator not found"));
    CHECK(contains(textList.out, "8 sessions."));
    CHECK(setup.run({"sessions", "--game", "g0000000000000001"}).code == ExitCode::ProblemsFound);
    CHECK(setup.run({"sessions", "--limit", "-1"}).code == ExitCode::UsageError);

    // Only the clean return verified its choice for gb.
    CHECK(setup.query("SELECT kind || ' ' || value || ' ' || system || ' ' || test FROM rv_launch_verifications") ==
          std::vector<std::string>{"profile fake-return gb 0"});

    SECTION("a launch test verifies the choice when the game returns normally, and only then") {
        REQUIRE(setup.run({"assign", "--game", tiny, "fake-return"}).code == ExitCode::Ok);
        const json verified = setup.runJson({"launch", "--game", tiny, "--test"}, ExitCode::Ok);
        CHECK(verified.at("waited") == true);
        CHECK(verified.at("verified") == true);
        CHECK(verified.at("session").at("test") == true);
        CHECK(setup.query("SELECT test FROM rv_launch_verifications WHERE value = 'fake-return'") ==
              std::vector<std::string>{"1"});
        REQUIRE(setup.run({"assign", "--game", tiny, "fake-quick"}).code == ExitCode::Ok);
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
        const json launched = setup.runJson({"launch", "--game", game, "--wait"}, ExitCode::ProblemsFound);
        CHECK(launched.at("session").at("quickExit") == true); // it exits at once
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
        const json launched = setup.runJson({"launch", "--game", game, "--wait"}, ExitCode::ProblemsFound);
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

TEST_CASE("cli/launch: a CLI ended mid-game leaves its session to the next start, which closes it as interrupted; "
          "a session whose owner runs stays open",
          "[cli][launch][sessions]") {
    const LoggingReset loggingReset;
    const SessionSetup setup("rv-cli-recovery");
    const std::vector<fs::path> listed{buildFake()};
    const rv::test::ScopedLaunchAllowList allowList(listed); // inherited by the CLI processes
    const std::string tiny = setup.gameAt("gb", "Tiny Tennis (Europe).gb").toString();
    const std::string cafe = setup.gameAt("gb", "Café Racer (Europe).gb").toString();
    REQUIRE(setup.run({"assign", "gb", "fake-long"}).code == ExitCode::Ok);

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
    CHECK(result.code == ExitCode::ProblemsFound); // a quick exit
    const json launched = json::parse(result.out);
    CHECK(launched.at("started") == true);
    CHECK(launched.at("sessionRecorded") == true);
    CHECK(took >= 3500ms); // the session's end waited for the import's commit, rather than being lost
    const std::int64_t id = launched.at("session").at("id").get<std::int64_t>();
    CHECK(setup.query(fmt::format("SELECT outcome || ' ' || quick_exit FROM rv_play_sessions WHERE id = {}", id)) ==
          std::vector<std::string>{"returned 1"});
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
