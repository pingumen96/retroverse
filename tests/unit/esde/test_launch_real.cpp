// The launch translation on the real machine (ADR-0008 §8, §10, I2 step 10). Hidden ([.real-library]): ctest never runs
// it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the
// machine's folders in environment variables (every one needed, else the test is skipped):
//   RV_REAL_ESDE_HOME      the ES-DE home
//   RV_REAL_ESDE_INSTALL   its install folder
//   RV_REAL_RETROARCH_EXE  the RetroArch executable App Paths registers
// Both tests go through retroverse-cli, as a user would, with a library.json naming those folders in a temporary user
// data directory (outside the library):
//  - `profiles --json`: the readiness of the 48 systems that have games, as the survey of the reference machine expects
//    it: 41 ready with RetroArch found through App Paths (apple2, mame and snes through their gamelists'
//    <alternativeEmulator>), ps3 and windows requiring launcher configuration (cmd.exe, refused), atari800, gc, ps2,
//    switch and trs-80 with their emulator not found;
//  - `launch --dry-run --json`: the five launches ES-DE logged on the machine, frozen in
//    tests/real_library/local/esde_logged_launches.json (git-ignored, machine-local: ES-DE rotates its log): each game is
//    selected by the path its logged command names, and the dry run's argv must equal CommandLineToArgvW of the logged
//    command line, case-sensitively, argument for argument.
// Nothing is started, and nothing is written but the temporary user data directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")

#include <nlohmann/json.hpp>

#include <rv/catalog/library.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>

#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// The same file or folder, compared as the file system compares names.
bool samePlace(const fs::path& a, const fs::path& b) {
    return rv::core::pathIsWithin(a, b) && rv::core::pathIsWithin(b, a);
}

std::wstring widen(std::string_view utf8) {
    const int units = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(units), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), units);
    return wide;
}

std::string narrow(std::wstring_view wide) {
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::vector<std::string> shellSplit(const std::string& commandLine) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(widen(commandLine).c_str(), &count);
    REQUIRE(argv != nullptr);
    std::vector<std::string> elements;
    for (int i = 0; i < count; ++i) {
        elements.push_back(narrow(argv[i]));
    }
    LocalFree(argv);
    return elements;
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

/// retroverse-cli with a user data directory whose library.json names the real ES-DE home and install folder.
class RealCli {
public:
    RealCli(const fs::path& esdeHome, const fs::path& install) {
        rv::catalog::LibraryProfile profile;
        profile.esdeHome = esdeHome;
        profile.installFolder = install;
        REQUIRE(rv::esde::writeLibraryFile(m_dir.file("data/config/library.json"), profile,
                                           rv::esde::ExistingFile::Keep)
                    .has_value());
    }

    struct Result {
        ExitCode code = ExitCode::Ok;
        nlohmann::json json;
        std::string err;
    };

    Result run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(m_dir.file("data"))});
        rv::cli::Output out = rv::cli::Output::memory();
        rv::cli::Output err = rv::cli::Output::memory();
        rv::core::PathEnvironment environment;
        environment.executableDir = m_dir.path();
        environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
        Result result;
        result.code = rv::cli::run(arguments, out, err, environment);
        result.err = err.text();
        INFO(result.err);
        REQUIRE_NOTHROW(result.json = nlohmann::json::parse(out.text()));
        return result;
    }

private:
    rv::test::TempDir m_dir{"rv-real-launch"};
};

struct RealMachine {
    fs::path esdeHome;
    fs::path install;
    fs::path retroArch;
};

std::optional<RealMachine> realMachine() {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto retroArch = pathVariable(L"RV_REAL_RETROARCH_EXE");
    if (!home || !install || !retroArch) {
        return std::nullopt;
    }
    return RealMachine{*home, *install, *retroArch};
}

} // namespace

TEST_CASE("esde/real: the readiness of the real systems that have games", "[esde][launch][.real-library]") {
    const auto machine = realMachine();
    if (!machine) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL and RV_REAL_RETROARCH_EXE (scripts/real-library.ps1 does)");
    }
    const LoggingReset loggingReset;
    const RealCli cli(machine->esdeHome, machine->install);
    const auto profiles = cli.run({"profiles", "--json"});
    REQUIRE(profiles.code == ExitCode::Ok);

    // The 48 systems ES-DE loads on the reference machine (a ROM folder with at least one game), by readiness.
    const std::set<std::string> ready{
        "amiga",   "amigacd32",  "amstradcpc", "apple2",     "atari2600",   "atari5200", "atari7800",   "atarijaguar",
        "atarilynx", "atarist",  "c64",        "channelf",   "colecovision", "dos",      "dreamcast",   "fds",
        "gb",      "gba",        "gbc",        "intellivision", "mame",     "mastersystem", "megadrive", "n64",
        "nds",     "neogeo",     "nes",        "ngpc",       "odyssey2",    "pc",        "pcengine",    "pcfx",
        "pokemini", "psp",       "psx",        "saturn",     "segacd",      "snes",      "vic20",       "virtualboy",
        "wii"};
    const std::set<std::string> requiresConfiguration{"ps3", "windows"};
    const std::set<std::string> emulatorNotFound{"atari800", "gc", "ps2", "switch", "trs-80"};
    REQUIRE(ready.size() == 41);
    const std::map<std::string, std::string> alternative{{"apple2", "MAME - Current"},
                                                         {"mame", "MAME 2003-Plus"},
                                                         {"snes", "bsnes"},
                                                         {"atari800", "Altirra (Standalone)"},
                                                         {"gc", "Dolphin (Standalone)"},
                                                         {"ps2", "PCSX2 (Standalone)"}};

    std::map<std::string, std::size_t> counts;
    std::size_t checked = 0;
    for (const nlohmann::json& system : profiles.json.at("systems")) {
        const std::string name = system.at("name");
        const std::string readiness = system.at("readiness");
        ++counts[readiness];
        const bool isReady = ready.count(name) > 0;
        const bool isConfiguration = requiresConfiguration.count(name) > 0;
        const bool isNotFound = emulatorNotFound.count(name) > 0;
        if (!isReady && !isConfiguration && !isNotFound) {
            continue;
        }
        ++checked;
        INFO(name << ": " << system.dump(2));
        CHECK(system.at("availability") == "available");
        if (const auto expected = alternative.find(name); expected != alternative.end()) {
            CHECK(system.at("chosen").at("source") == "esde-system");
            CHECK(system.at("chosen").at("value") == expected->second);
        } else {
            CHECK(system.at("chosen").at("source") == "first-command");
        }
        if (isReady) {
            CHECK(readiness == "ready");
            REQUIRE(system.at("program").is_object());
            CHECK(system.at("program").at("rule") == "winregistrypath");
            CHECK(system.at("program").at("entry") == "retroarch.exe");
            CHECK(samePlace(rv::core::pathFromUtf8(system.at("program").at("path").get<std::string>()),
                            machine->retroArch));
            REQUIRE(system.at("core").is_object());
            CHECK(system.at("core").at("entry") == "%EMUPATH%\\cores");
        } else if (isConfiguration) {
            CHECK(readiness == "requires-configuration");
            bool refused = false;
            for (const nlohmann::json& blocker : system.at("blockers")) {
                refused = refused || blocker.at("code") == "program-refused";
            }
            CHECK(refused);
        } else {
            CHECK(readiness == "emulator-not-found");
        }
    }
    CHECK(checked == 48);
    for (const auto& [readiness, count] : counts) {
        WARN(readiness << ": " << count << " of " << profiles.json.at("systems").size() << " configured systems");
    }
}

TEST_CASE("esde/real: the launches ES-DE logged, reproduced argument for argument", "[esde][launch][.real-library]") {
    const auto machine = realMachine();
    if (!machine) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL and RV_REAL_RETROARCH_EXE (scripts/real-library.ps1 does)");
    }
    const fs::path frozen = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "real_library" / "local" /
                            "esde_logged_launches.json";
    const auto document = rv::core::readJsonFile(frozen);
    if (!document) {
        SKIP("no frozen ES-DE launches on this machine: " << document.error().toString());
    }
    const LoggingReset loggingReset;
    const RealCli cli(machine->esdeHome, machine->install);
    const auto profiles = cli.run({"profiles", "--json"});
    REQUIRE(profiles.code == ExitCode::Ok);
    std::map<std::string, std::string> romFolders;
    for (const nlohmann::json& system : profiles.json.at("systems")) {
        if (system.at("romFolder").is_string()) {
            romFolders[system.at("name").get<std::string>()] = system.at("romFolder").get<std::string>();
        }
    }

    const nlohmann::json& launches = document->at("launches");
    REQUIRE(launches.size() == 5);
    std::size_t differences = 0;
    for (const nlohmann::json& launch : launches) {
        const std::string system = launch.at("system");
        const std::string command = launch.at("command");
        INFO(launch.at("title").get<std::string>() << " (" << system << "): " << command);
        const std::vector<std::string> expected = shellSplit(command);
        REQUIRE(expected.size() >= 2);
        // The game, by the path the logged command names: the system's ROM folder as ES-DE builds it, then the path.
        const std::string rom = expected.back();
        const std::string folder = romFolders.at(system) + "\\";
        REQUIRE(rom.starts_with(folder));
        std::string relative = rom.substr(folder.size());
        std::replace(relative.begin(), relative.end(), '\\', '/');

        const auto dryRun = cli.run({"launch", "--dry-run", "--system", system, "--path", relative, "--json"});
        INFO(dryRun.json.dump(2));
        CHECK(dryRun.code == ExitCode::Ok);
        CHECK(dryRun.json.at("readiness") == "ready");
        CHECK(dryRun.json.at("listedPath") == relative);
        CHECK(dryRun.json.at("started") == false);
        REQUIRE(dryRun.json.at("request").is_object());
        const std::vector<std::string> argv = dryRun.json.at("request").at("argv");
        CHECK(argv == expected);
        if (argv != expected) {
            ++differences;
        }
    }
    CHECK(differences == 0);
}
