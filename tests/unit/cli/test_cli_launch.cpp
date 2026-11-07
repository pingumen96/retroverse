// retroverse-cli profiles and launch --dry-run (I2 step 10) through the dispatcher, on the standard ES-DE fixture
// recorded in library.json: each system's command and readiness, why it was chosen, the rule and entry that found the
// program; a dry run's argv, working folder, readiness and exit code, the path spelled as the folder lists it, the
// automation guard's verdict; a launch names a game of the catalog (--game, I2 step 12: test_cli_sessions.cpp);
// nothing is started or written in the library's folders.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/discovery.hpp>

#include "support/fixture.hpp"
#include "support/program_stub.hpp"
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

/// Sets an environment variable for one scope (nothing: unset), and puts the previous value back.
class ScopedVariable {
public:
    ScopedVariable(const wchar_t* name, std::optional<std::wstring> value) : m_name(name) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size > 0) {
            std::wstring previous(size, L'\0');
            previous.resize(GetEnvironmentVariableW(name, previous.data(), size));
            m_previous = std::move(previous);
        }
        SetEnvironmentVariableW(name, value ? value->c_str() : nullptr);
    }
    ~ScopedVariable() { SetEnvironmentVariableW(m_name, m_previous ? m_previous->c_str() : nullptr); }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    const wchar_t* m_name;
    std::optional<std::wstring> m_previous;
};

struct RunResult {
    ExitCode code = ExitCode::Ok;
    std::string out;
    std::string err;
};

/// A path's UTF-8 text with '\' separators, as the results print it.
std::string utf8(const fs::path& path) {
    return rv::core::pathToUtf8(fs::path(path).make_preferred());
}

/// A path from UTF-8 text (std::filesystem reads a narrow literal in the ANSI code page).
fs::path u8path(std::string_view text) {
    return rv::core::pathFromUtf8(text);
}

/// The standard fixture recorded as the library of a user data directory outside it.
struct Setup {
    rv::test::TempDir dir{"rv-cli-launch"};
    rv::test::Fixture fixture;
    fs::path data;

    explicit Setup(rv::test::Fixture materialized) : fixture(std::move(materialized)), data(dir.file("data")) {
        rv::catalog::LibraryProfile profile;
        profile.esdeHome = fixture.path("ES-DE");
        profile.installFolder = fixture.path("Programs/ES-DE");
        REQUIRE(rv::esde::writeLibraryFile(data / "config" / "library.json", profile, rv::esde::ExistingFile::Keep)
                    .has_value());
    }

    RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", utf8(data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(dir.path()));
        return RunResult{code, out.text(), err.text()};
    }
};

Setup standardSetup() {
    auto fixture = rv::test::Fixture::materialize("esde/standard");
    REQUIRE(fixture.has_value());
    return Setup(std::move(*fixture));
}

/// The fixture's folders by name and its files by name, size and content (folder times follow their content lazily on
/// NTFS, so they are not compared): what a run must leave as it was.
std::vector<std::string> treeOf(const fs::path& root) {
    const auto listed = rv::core::listTree(root);
    REQUIRE(listed.has_value());
    std::vector<std::string> entries;
    for (const rv::core::TreeEntry& entry : *listed) {
        if (entry.kind != rv::core::TreeEntryKind::File) {
            entries.push_back(entry.relativePath + "/");
            continue;
        }
        const auto path = rv::core::extendedLengthPathBelow(root, entry.relativePath);
        REQUIRE(path.has_value());
        const auto bytes = rv::core::readFileShared(*path, 16ull * 1024ull * 1024ull);
        REQUIRE(bytes.has_value());
        entries.push_back(entry.relativePath + " " + std::to_string(entry.size) + " " + rv::core::sha256Hex(*bytes));
    }
    return entries;
}

const nlohmann::json& systemNamed(const nlohmann::json& json, std::string_view name) {
    const nlohmann::json* found = nullptr;
    for (const nlohmann::json& system : json.at("systems")) {
        if (system.at("name").get<std::string>() == name) {
            found = &system;
        }
    }
    INFO("system " << name);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("cli/profiles: the systems of the standard fixture, their commands and readiness", "[cli][launch]") {
    const LoggingReset loggingReset;
    const Setup setup = standardSetup();
    const auto before = treeOf(setup.fixture.root());
    const fs::path emulator = setup.fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe");

    const RunResult json = setup.run({"profiles", "--json"});
    INFO(json.err);
    REQUIRE(json.code == ExitCode::Ok);
    const nlohmann::json report = nlohmann::json::parse(json.out);
    CHECK(report.at("systems").size() == 3);
    const nlohmann::json& gb = systemNamed(report, "gb");
    CHECK(gb.at("readiness") == "ready");
    CHECK(gb.at("chosen").at("source") == "first-command");
    CHECK(gb.at("chosen").at("value") == "Fake Emulator");
    CHECK(gb.at("program").at("path") == utf8(emulator));
    CHECK(gb.at("program").at("rule") == "staticpath");
    CHECK(gb.at("program").at("entry") == "%ESPATH%\\Emulators\\FakeEmulator\\fake-emulator.exe");
    CHECK(gb.at("core").at("rule") == "corepath");
    REQUIRE(gb.at("commands").size() == 4);
    CHECK(gb.at("commands")[2].at("label") == "Missing Emulator (Standalone)");
    CHECK(gb.at("commands")[2].at("readiness") == "emulator-not-found");
    CHECK(gb.at("commands")[3].at("readiness") == "ready");
    const nlohmann::json& snes = systemNamed(report, "snes");
    CHECK(snes.at("chosen").at("source") == "esde-system");
    CHECK(snes.at("alternativeEmulator") == "Fake Emulator (Hidden Console)");
    CHECK(snes.at("commands")[2].at("readiness") == "requires-configuration");
    CHECK(snes.at("commands")[2].at("blockers").size() == 2);
    const nlohmann::json& psx = systemNamed(report, "psx");
    CHECK(psx.at("commands")[1].at("notes").size() == 1); // the %INJECT% file is each game's
    CHECK(psx.at("commands")[2].at("readiness") == "core-not-found");
    // The shipped RetroArch profile serves gb and psx; its readiness depends on this machine.
    CHECK(gb.at("profiles").size() == 1);
    CHECK(report.at("profiles").at(0).at("id") == "retroarch");

    const RunResult text = setup.run({"profiles", "--system", "snes"});
    INFO(text.err);
    CHECK(text.code == ExitCode::Ok);
    CHECK(text.out.find("snes (Nintendo SNES (Super Nintendo))") != std::string::npos);
    CHECK(text.out.find("ready: command \"Fake Emulator (Hidden Console)\" (the gamelist's <alternativeEmulator>)") !=
          std::string::npos);
    CHECK(text.out.find("gb (") == std::string::npos);

    CHECK(setup.run({"profiles", "--system", "vectrex"}).code == ExitCode::UsageError);
    CHECK(setup.run({"profiles", "--bogus"}).code == ExitCode::UsageError);
    CHECK(treeOf(setup.fixture.root()) == before);
}

TEST_CASE("cli/launch: --dry-run on the standard fixture", "[cli][launch]") {
    const LoggingReset loggingReset;
    const Setup setup = standardSetup();
    const auto before = treeOf(setup.fixture.root());
    const fs::path emulator = setup.fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe");
    const fs::path roms = setup.fixture.path("roms");

    SECTION("a ready game: argv, working folder, the path as the folder lists it") {
        const RunResult run = setup.run({"launch", "--dry-run", "--system", "gb", "--path", "café racer (europe).GB",
                                         "--json"});
        INFO(run.err);
        REQUIRE(run.code == ExitCode::Ok);
        const nlohmann::json json = nlohmann::json::parse(run.out);
        CHECK(json.at("ready") == true);
        CHECK(json.at("listedPath") == "Café Racer (Europe).gb");
        CHECK(json.at("title") == "Café Racer");
        CHECK(json.at("chosen").at("source") == "esde-game");
        CHECK(json.at("chosen").at("value") == "Fake Emulator (Game Folder)");
        CHECK(json.at("command") == "%STARTDIR%=%GAMEDIR% %EMULATOR_FAKE-EMULATOR% -- %ROM%");
        CHECK(json.at("request").at("argv") ==
              nlohmann::json::array({utf8(emulator), "--", utf8(roms / "gb" / u8path("Café Racer (Europe).gb"))}));
        CHECK(json.at("request").at("workingDirectory") == utf8(roms / "gb"));
        CHECK(json.at("request").at("environment").empty());
        CHECK(json.at("started") == false);
        CHECK_FALSE(json.at("commandLine").is_null());
    }
    SECTION("the text form") {
        const RunResult run = setup.run({"launch", "--dry-run", "--system", "snes", "--path", "Alpha Quest (USA).sfc"});
        INFO(run.err);
        CHECK(run.code == ExitCode::Ok);
        CHECK(run.out.find("Readiness           ready") != std::string::npos);
        CHECK(run.out.find("  [1] --") != std::string::npos);
        CHECK(run.out.find("Console window      hidden (%HIDEWINDOW%)") != std::string::npos);
        CHECK(run.out.find("Dry run: nothing was started.") != std::string::npos);
    }
    SECTION("not ready: exit 1, the readiness and the blockers") {
        RunResult run = setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Gone Fishing (USA).gb", "--json"});
        CHECK(run.code == ExitCode::ProblemsFound);
        nlohmann::json json = nlohmann::json::parse(run.out);
        CHECK(json.at("readiness") == "rom-missing");
        CHECK(json.at("availability") == "missing");
        CHECK(json.at("title") == "Gone Fishing"); // from its gamelist entry
        run = setup.run({"launch", "--dry-run", "--system", "psx", "--path", "Loud Disc (Japan).CUE", "--json"});
        CHECK(run.code == ExitCode::Ok); // not a game for ES-DE, but the dry run resolves what it is asked
    }
    SECTION("the automation guard's verdict, nothing started") {
        {
            const ScopedVariable allowList(L"RV_LAUNCH_ALLOWLIST", std::wstring());
            const RunResult run =
                setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Tiny Tennis (Europe).gb", "--json"});
            const nlohmann::json json = nlohmann::json::parse(run.out);
            CHECK(json.at("guard").at("allowed") == false);
            CHECK(json.at("guard").at("rule") == "not listed");
            CHECK(run.code == ExitCode::Ok); // readiness is the command's, the guard only says what automation would do
        }
        {
            const ScopedVariable allowList(L"RV_LAUNCH_ALLOWLIST", emulator.wstring());
            const RunResult run =
                setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Tiny Tennis (Europe).gb", "--json"});
            CHECK(nlohmann::json::parse(run.out).at("guard").at("rule") == "allowed");
        }
        {
            const ScopedVariable allowList(L"RV_LAUNCH_ALLOWLIST", std::nullopt);
            const RunResult run =
                setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Tiny Tennis (Europe).gb"});
            CHECK(run.out.find("Automation guard    inactive") != std::string::npos);
        }
    }
    SECTION("usage errors") {
        // A launch names a game of the catalog (I2 step 12): --system and --path are the dry run's.
        RunResult run = setup.run({"launch", "--system", "gb", "--path", "Tiny Tennis (Europe).gb"});
        CHECK(run.code == ExitCode::UsageError);
        CHECK(run.err.find("for --dry-run only") != std::string::npos);
        CHECK(setup.run({"launch"}).code == ExitCode::UsageError);
        CHECK(setup.run({"launch", "--game", "g0000000000000001", "--system", "gb"}).code == ExitCode::UsageError);
        CHECK(setup.run({"launch", "--game", "x"}).code == ExitCode::UsageError);
        CHECK(setup.run({"launch", "--dry-run", "--game", "g0000000000000001", "--wait"}).code == ExitCode::UsageError);
        CHECK(setup.run({"launch", "--game", "g0000000000000001", "--wait-limit", "5"}).code == ExitCode::UsageError);
        CHECK(setup.run({"launch", "--game", "g0000000000000001", "--wait", "--wait-limit", "0"}).code ==
              ExitCode::UsageError);
        CHECK(setup.run({"launch", "--dry-run", "--system", "gb"}).code == ExitCode::UsageError);
        CHECK(setup.run({"launch", "--dry-run", "--system", "vectrex", "--path", "x.vec"}).code ==
              ExitCode::UsageError);
        run = setup.run({"launch", "--dry-run", "--system", "gb", "--path", "../snes/Alpha Quest (USA).sfc"});
        CHECK(run.code == ExitCode::UsageError);
        CHECK(run.err.find("--path") != std::string::npos);
        CHECK(setup.run({"launch", "--dry-run", "--system", "gb", "--path", "a/../b.gb"}).code == ExitCode::UsageError);
        CHECK(setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Tiny*.gb"}).code == ExitCode::UsageError);
    }
    CHECK(treeOf(setup.fixture.root()) == before);
}

TEST_CASE("cli/launch: --dry-run --vr adapts the launch to its program (I11 step 12)", "[cli][launch]") {
    const LoggingReset loggingReset;
    const Setup setup = standardSetup();
    const auto before = treeOf(setup.fixture.root());
    const fs::path emulator = setup.fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe");
    const fs::path rom = setup.fixture.path("roms/gb/Tiny Tennis (Europe).gb");
    // The fixture's emulator as a program of the user's emulators.json (ADR-0020 §1).
    REQUIRE(rv::core::writeTextFileAtomic(setup.data / "config" / "emulators.json", R"({
  "schemaVersion": 2,
  "profiles": [],
  "programs": [{
    "id": "fake-emulator", "displayName": "Fake Emulator", "files": ["fake-emulator.exe"],
    "window": {"prefer": [{"classContains": "FakeRender"}]},
    "vr": {"addArguments": ["--windowed"], "dropArguments": ["--fullscreen"],
           "appendConfig": {"option": "--config", "file": "fake.cfg", "lines": ["a = 1", "b = \"two\""]}}
  }]
})").has_value());
    const fs::path vrFolder = setup.data / "launcher" / "vr";

    SECTION("JSON: the argv in VR, the program, the append-file shown and not written") {
        const RunResult run = setup.run(
            {"launch", "--dry-run", "--system", "gb", "--path", "Tiny Tennis (Europe).gb", "--vr", "--json"});
        INFO(run.err);
        REQUIRE(run.code == ExitCode::Ok);
        const nlohmann::json json = nlohmann::json::parse(run.out);
        // The desktop's argv with the program's arguments right after the program.
        const RunResult desktop =
            setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Tiny Tennis (Europe).gb", "--json"});
        REQUIRE(desktop.code == ExitCode::Ok);
        nlohmann::json expected = nlohmann::json::parse(desktop.out).at("request").at("argv");
        REQUIRE(expected.size() >= 3);
        CHECK(expected.at(0) == utf8(emulator));
        CHECK(expected.back() == utf8(rom));
        expected.insert(expected.begin() + 1, {"--windowed", "--config", utf8(vrFolder / "fake.cfg")});
        CHECK(json.at("request").at("argv") == expected);
        const nlohmann::json& vr = json.at("vr");
        CHECK(vr.at("program") == "fake-emulator");
        CHECK(vr.at("programOrigin") == "user");
        CHECK(vr.at("added") == nlohmann::json::array({"--windowed", "--config", utf8(vrFolder / "fake.cfg")}));
        CHECK(vr.at("dropped").empty());
        CHECK(vr.at("appendFile").at("path") == utf8(vrFolder / "fake.cfg"));
        CHECK(vr.at("appendFile").at("bytes") == "a = 1\nb = \"two\"\n");
        CHECK(vr.at("inGame") == "light");
        CHECK(vr.at("refused").is_null());
        CHECK_FALSE(json.at("commandLine").is_null());
        CHECK(json.at("commandLine").get<std::string>().find("--windowed") != std::string::npos);
        CHECK_FALSE(fs::exists(vrFolder));
    }
    SECTION("without --vr the launch is as asked") {
        const RunResult run =
            setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Tiny Tennis (Europe).gb", "--json"});
        REQUIRE(run.code == ExitCode::Ok);
        const nlohmann::json json = nlohmann::json::parse(run.out);
        const nlohmann::json& argv = json.at("request").at("argv");
        CHECK(std::find(argv.begin(), argv.end(), "--windowed") == argv.end());
        CHECK(std::find(argv.begin(), argv.end(), "--config") == argv.end());
        CHECK_FALSE(json.contains("vr"));
    }
    SECTION("the text form") {
        const RunResult run =
            setup.run({"launch", "--dry-run", "--system", "gb", "--path", "Tiny Tennis (Europe).gb", "--vr"});
        INFO(run.err);
        CHECK(run.code == ExitCode::Ok);
        CHECK(run.out.find("Fake Emulator (program 'fake-emulator', user): 3 argument(s) added, 0 dropped, load light") !=
              std::string::npos);
        CHECK(run.out.find("VR append-file") != std::string::npos);
        CHECK(run.out.find("  b = \"two\"") != std::string::npos);
        CHECK(run.out.find("Argv (in VR):") != std::string::npos);
        CHECK(run.out.find("  [1] --windowed") != std::string::npos);
        CHECK_FALSE(fs::exists(vrFolder));
    }
    SECTION("--vr is a dry run's") {
        const RunResult run = setup.run({"launch", "--game", "g0000000000000001", "--vr"});
        CHECK(run.code == ExitCode::UsageError);
        CHECK(run.err.find("--vr") != std::string::npos);
    }
    SECTION("profiles: the programs, and the one each system's launch starts") {
        const RunResult run = setup.run({"profiles", "--json"});
        INFO(run.err);
        REQUIRE(run.code == ExitCode::Ok);
        const nlohmann::json report = nlohmann::json::parse(run.out);
        bool shippedRetroArch = false;
        bool userFake = false;
        for (const nlohmann::json& program : report.at("programs")) {
            shippedRetroArch |= program.at("id") == "retroarch" && program.at("origin") == "shipped";
            userFake |= program.at("id") == "fake-emulator" && program.at("origin") == "user";
        }
        CHECK(shippedRetroArch);
        CHECK(userFake);
        CHECK(systemNamed(report, "gb").at("programProfile") == "fake-emulator");
        const RunResult text = setup.run({"profiles"});
        CHECK(text.out.find("Programs in use") != std::string::npos);
        CHECK(text.out.find("program           Fake Emulator ('fake-emulator')") != std::string::npos);
    }
    CHECK(treeOf(setup.fixture.root()) == before);
}

TEST_CASE("cli/launch: no library.json", "[cli][launch]") {
    const LoggingReset loggingReset;
    const rv::test::TempDir dir("rv-cli-launch-nolibrary");
    Output out = Output::memory();
    Output err = Output::memory();
    const std::vector<std::string> arguments{"--data-dir", utf8(dir.file("data")), "profiles"};
    CHECK(rv::cli::run(arguments, out, err, testEnvironment(dir.path())) == ExitCode::UsageError);
    CHECK(err.text().find("library create") != std::string::npos);
}

TEST_CASE("cli/assign --emulator: a program for a command that finds none", "[cli][launch]") {
    const LoggingReset loggingReset;
    Setup setup = standardSetup();
    REQUIRE(setup.run({"import"}).code == ExitCode::Ok);
    const auto gbMissing = [&] {
        const auto report = nlohmann::json::parse(setup.run({"profiles", "--json", "--system", "gb"}).out);
        for (const auto& command : report.at("systems").at(0).at("commands")) {
            if (command.at("label") == "Missing Emulator (Standalone)") {
                return command;
            }
        }
        return nlohmann::json();
    };
    CHECK(gbMissing().at("readiness") == "emulator-not-found");

    const rv::test::TempDir programs("rv-cli-program");
    const fs::path program = programs.file("standalone.exe");
    rv::test::writeProgramStub(program);
    const RunResult set = setup.run({"assign", "--emulator", "MISSING-EMULATOR", "--program", utf8(program)});
    INFO(set.err);
    REQUIRE(set.code == ExitCode::Ok);
    CHECK(set.out.find("standalone.exe") != std::string::npos);
    CHECK(gbMissing().at("readiness") == "ready");
    CHECK(gbMissing().at("program").at("rule") == "retroverse");

    const RunResult shown = setup.run({"assign", "--emulator", "MISSING-EMULATOR", "--json"});
    REQUIRE(shown.code == ExitCode::Ok);
    CHECK(nlohmann::json::parse(shown.out).at("program") == utf8(program));

    // Refused by its name; nothing stored.
    const fs::path shell = programs.file("powershell.exe");
    rv::test::writeProgramStub(shell);
    const RunResult refused = setup.run({"assign", "--emulator", "MISSING-EMULATOR", "--program", utf8(shell)});
    CHECK(refused.code == ExitCode::UsageError);
    CHECK(refused.err.find("command interpreter") != std::string::npos);
    CHECK(setup.run({"assign", "--emulator", "MISSING-EMULATOR", "--program", utf8(programs.file("gone.exe"))}).code ==
          ExitCode::UsageError);
    CHECK(nlohmann::json::parse(setup.run({"assign", "--emulator", "MISSING-EMULATOR", "--json"}).out).at("program") ==
          utf8(program));

    REQUIRE(setup.run({"assign", "--emulator", "MISSING-EMULATOR", "--clear"}).code == ExitCode::Ok);
    CHECK(gbMissing().at("readiness") == "emulator-not-found");

    CHECK(setup.run({"assign", "gb", "--program", utf8(program)}).code == ExitCode::UsageError);
    CHECK(setup.run({"assign", "--emulator", "X", "--game", "g0000000000000001"}).code == ExitCode::UsageError);
    CHECK(setup.run({"assign", "--emulator", "X", "--clear", "--program", utf8(program)}).code == ExitCode::UsageError);
}

TEST_CASE("cli/launch --test fails closed in an unattended run without the allow-list", "[cli][launch]") {
    const LoggingReset loggingReset;
    Setup setup = standardSetup();
    const ScopedVariable background(L"RV_LAUNCH_BACKGROUND", L"1");
    const ScopedVariable allowList(L"RV_LAUNCH_ALLOWLIST", std::nullopt);
    // Refused before the library or the game is looked at: this one does not exist.
    const RunResult refused = setup.run({"launch", "--game", "g0000000000000001", "--test"});
    CHECK(refused.code == ExitCode::ProblemsFound);
    CHECK(refused.err.find("RV_LAUNCH_ALLOWLIST") != std::string::npos);
}
