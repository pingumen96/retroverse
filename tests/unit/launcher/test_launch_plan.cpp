#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_plan.hpp>
#include <rv/launcher/launch_session.hpp>
#include <rv/platform_win/win32_process_runner.hpp>

#include "support/fake_system_probe.hpp"
#include "support/file_system.hpp"
#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")
#endif

using namespace std::chrono_literals;
using rv::core::RegistryRoot;
using rv::launcher::BlockerKind;
using rv::launcher::EmulatorProfile;
using rv::launcher::EnvironmentOverride;
using rv::launcher::isRefusedProgram;
using rv::launcher::LaunchPlan;
using rv::launcher::LaunchTarget;
using rv::launcher::LocationRule;
using rv::launcher::planLaunch;
using rv::launcher::PlanOptions;
using rv::launcher::WaitStrategy;

namespace {

constexpr std::string_view kAppPaths = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";

/// A machine with RetroArch registered in App Paths (a program stub on disk, so that requests validate) and the
/// gambatte core in its cores folder (known to the probe only), and a folder for ROMs.
struct Machine {
    rv::test::TempDir dir{"rv-launch-plan"};
    rv::test::FakeSystemProbe probe;
    std::filesystem::path emulator;
    std::filesystem::path cores;

    Machine() {
        emulator = rv::test::writeProgramStub(dir.file("Emulators/Retro Arch/retroarch.exe").lexically_normal());
        registerAppPath("retroarch.exe", emulator);
        cores = emulator.parent_path() / "cores";
        probe.addFile(cores / "gambatte_libretro.dll");
    }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    ~Machine() {
        rv::test::removeTreeAnyLength(dir.path() / "roms"); // the plain path of TempDir's clean-up misses long names
    }

    void registerAppPath(std::string_view name, const std::filesystem::path& path) {
        probe.setRegistryString(RegistryRoot::LocalMachine, std::string(kAppPaths) + std::string(name), "",
                                rv::core::pathToUtf8(path));
        probe.addFile(path);
    }

    /// A ROM file under <dir>/roms (any length).
    std::filesystem::path rom(std::string_view relativeUtf8) {
        const std::filesystem::path path = dir.path() / "roms" / rv::core::pathFromUtf8(relativeUtf8);
        REQUIRE(rv::test::writeFileAnyLength(path, "rom").has_value());
        return path.lexically_normal();
    }
};

/// RetroArch as the shipped profile describes it, for gb (whose core the machine has) and snes (whose core it lacks).
EmulatorProfile retroArch() {
    EmulatorProfile profile;
    profile.id = "retroarch";
    profile.displayName = "RetroArch";
    profile.executable.appPathsName = "retroarch.exe";
    profile.arguments = {"-L", "{var:core}", "{rom}"};
    profile.variables = {{"cores", "{emulator_dir}\\cores"}};
    profile.requiredFiles = {"{var:core}"};
    profile.systems["gb"].variables = {{"core", "{var:cores}\\gambatte_libretro.dll"}};
    profile.systems["snes"].variables = {{"core", "{var:cores}\\snes9x_libretro.dll"}};
    return profile;
}

LaunchTarget target(std::filesystem::path rom, std::string system = "gb", std::string title = {}) {
    LaunchTarget target;
    target.rom = std::move(rom);
    target.system = std::move(system);
    target.title = std::move(title);
    return target;
}

/// The plan's blocker, checked to be of `kind` and to name every one of `named`.
void requireBlocked(const LaunchPlan& plan, BlockerKind kind, std::initializer_list<std::string_view> named = {}) {
    REQUIRE_FALSE(plan.ready());
    REQUIRE(plan.blocker.has_value());
    INFO(rv::launcher::toString(plan.blocker->kind) << ": " << plan.blocker->message);
    CHECK(plan.blocker->kind == kind);
    for (const std::string_view text : named) {
        CHECK(plan.blocker->message.find(text) != std::string::npos);
    }
}

const rv::launcher::LaunchRequest& requireReady(const LaunchPlan& plan) {
    INFO((plan.blocker ? plan.blocker->message : std::string()));
    REQUIRE(plan.ready());
    CHECK_FALSE(plan.blocker.has_value());
    return *plan.request;
}

#if defined(_WIN32)
std::vector<std::string> commandLineToArgv(const std::string& commandLine) {
    const auto wide = rv::core::utf8ToWide(commandLine);
    REQUIRE(wide.has_value());
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(wide->c_str(), &count);
    REQUIRE(argv != nullptr);
    std::vector<std::string> arguments;
    for (int i = 0; i < count; ++i) {
        arguments.push_back(rv::core::wideToUtf8Lossy(argv[i]));
    }
    LocalFree(argv);
    return arguments;
}
#endif

/// A name holding everything a ROM path may hold: spaces, quotes, shell characters, non-ASCII text.
const std::vector<std::string>& hostileNames() {
    static const std::vector<std::string> names{
        "Mole Mania (USA, Europe) (SGB Enhanced).gb",
        "it's  two spaces.gb",
        "R&D & more.gb",
        "100% ^caret^ !bang!.gb",
        "semi;colon=equals,comma.gb",
        "Pok\xC3\xA9mon \xE2\x80\x93 \xC3\x89" "dition Bleue.gb",
        "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3 \xF0\x9F\x8E\xAE.gb",
        "trailing dot.and space .gb",
    };
    return names;
}

/// A relative path whose full path under the machine's folder passes 260 characters.
std::string longRelativePath() {
    std::string relative;
    for (int i = 0; i < 4; ++i) {
        relative += "A rather long folder name with spaces, & and % (" + std::to_string(i) + ")/";
    }
    return relative + "Game (Europe) (En,Fr,De,Es,It) - Long Title; Special Edition = Deluxe.gb";
}

} // namespace

TEST_CASE("launcher/launch_plan: a RetroArch launch becomes a valid request", "[launcher][launch_plan]") {
    Machine machine;
    const std::filesystem::path rom = machine.rom("gb/Mole Mania (USA, Europe) (SGB Enhanced).gb");
    const LaunchPlan plan = planLaunch(target(rom, "gb", "Mole Mania"), retroArch(), machine.probe);
    const auto& request = requireReady(plan);
    CHECK(request.executable == machine.emulator.lexically_normal());
    const std::vector<std::string> arguments{"-L", rv::core::pathToUtf8(machine.cores / "gambatte_libretro.dll"),
                                             rv::core::pathToUtf8(rom)};
    CHECK(request.arguments == arguments);
    CHECK(request.workingDirectory.empty()); // the executable's folder (ADR-0008 §8)
    CHECK(rv::launcher::workingDirectoryOf(request) == machine.emulator.parent_path());
    CHECK(request.environment.empty());
    CHECK(request.waitStrategy == WaitStrategy::DirectProcess);
    CHECK(request.timeouts.windowTimeout == 30s);
    CHECK_FALSE(request.hideConsoleWindow);
    CHECK(request.label == "Mole Mania with RetroArch");
    CHECK(rv::launcher::validateLaunchRequest(request).has_value());
    REQUIRE(plan.executable.has_value());
    CHECK(plan.executable->rule == LocationRule::AppPaths);
    CHECK(plan.executable->entry == "retroarch.exe");

    // Without a title, the ROM's file name names the launch.
    const LaunchPlan untitled = planLaunch(target(rom), retroArch(), machine.probe);
    CHECK(requireReady(untitled).label == "Mole Mania (USA, Europe) (SGB Enhanced).gb with RetroArch");
}

TEST_CASE("launcher/launch_plan: hostile ROM paths stay one argument, also past 260 characters",
          "[launcher][launch_plan]") {
    Machine machine;
    std::vector<std::string> relatives;
    for (const std::string& name : hostileNames()) {
        relatives.push_back("gb/" + name);
    }
    relatives.push_back("gb/" + longRelativePath());
    for (const std::string& relative : relatives) {
        const std::filesystem::path rom = machine.rom(relative);
        INFO(rv::core::pathToUtf8(rom));
        EmulatorProfile profile = retroArch();
        profile.arguments = {"-L", "{var:core}", "{rom}", "-image={rom}", "{rom_name}", "--", "{rom_dir}"};
        const LaunchPlan plan = planLaunch(target(rom), profile, machine.probe);
        const auto& request = requireReady(plan);
        REQUIRE(request.arguments.size() == profile.arguments.size());
        const std::string romText = rv::core::pathToUtf8(rom);
        CHECK(request.arguments[2] == romText);
        CHECK(request.arguments[3] == "-image=" + romText);
        CHECK(request.arguments[4] == rv::core::pathToUtf8(rom.stem()));
        CHECK(request.arguments[6] == rv::core::pathToUtf8(rom.parent_path()));
        for (const std::string& argument : request.arguments) {
            CHECK(rv::launcher::isValidUtf8(argument));
        }
#if defined(_WIN32)
        // What the emulator's C runtime gives back, argument for argument.
        const auto line = rv::launcher::commandLineOf(request);
        REQUIRE(line.has_value());
        std::vector<std::string> expected{rv::core::pathToUtf8(request.executable)};
        expected.insert(expected.end(), request.arguments.begin(), request.arguments.end());
        CHECK(commandLineToArgv(*line) == expected);
#endif
    }
    CHECK(rv::core::pathToUtf8(machine.dir.path() / "roms" / "gb" / rv::core::pathFromUtf8(longRelativePath()))
              .size() > 260);
}

TEST_CASE("launcher/launch_plan: what the request carries from the profile", "[launcher][launch_plan]") {
    Machine machine;
    const std::filesystem::path rom = machine.rom("gb/Game.gb");
    machine.probe.addFile(machine.cores / "custom_libretro.dll");
    EmulatorProfile profile = retroArch();
    profile.displayName = "RetroArch (tree)";
    profile.workingDirectory = "{rom_dir}";
    profile.environment = {{"EMU_HOME", "{emulator_dir}\\home {system}"}, {"GONE", std::nullopt}};
    profile.waitStrategy = WaitStrategy::ProcessTree;
    profile.windowTimeout = 0ms;
    profile.hideConsoleWindow = true;
    LaunchTarget launch = target(rom, "gb", "Game");
    launch.variables = {{"core", "{var:cores}\\custom_libretro.dll"}}; // the launch's own variable wins
    const LaunchPlan plan = planLaunch(launch, profile, machine.probe);
    const auto& request = requireReady(plan);
    CHECK(request.arguments[1] == rv::core::pathToUtf8(machine.cores / "custom_libretro.dll"));
    CHECK(request.workingDirectory == rom.parent_path());
    const std::vector<EnvironmentOverride> environment{
        {"EMU_HOME", rv::core::pathToUtf8(machine.emulator.parent_path()) + "\\home gb"}, {"GONE", std::nullopt}};
    CHECK(request.environment == environment);
    CHECK(request.waitStrategy == WaitStrategy::ProcessTree);
    CHECK(request.timeouts.windowTimeout == 0ms);
    CHECK(request.timeouts.startupGrace == rv::launcher::LaunchTimeouts{}.startupGrace);
    CHECK(request.hideConsoleWindow);
    CHECK(request.label == "Game with RetroArch (tree)");
}

TEST_CASE("launcher/launch_plan: the ROM", "[launcher][launch_plan]") {
    Machine machine;
    requireBlocked(planLaunch(target("gb/Game.gb"), retroArch(), machine.probe), BlockerKind::RomMissing,
                   {"not absolute", "gb/Game.gb"});
    requireBlocked(planLaunch(target({}), retroArch(), machine.probe), BlockerKind::RomMissing);
    const std::filesystem::path missing = machine.dir.file("roms/gb/Gone.gb");
    requireBlocked(planLaunch(target(missing), retroArch(), machine.probe), BlockerKind::RomMissing,
                   {"does not exist", "Gone.gb"});
    // Without the check (a system's readiness), a ROM that is not there is planned.
    PlanOptions options;
    options.checkRom = false;
    CHECK(planLaunch(target(missing), retroArch(), machine.probe, options).ready());
    // Games that are folders.
    const std::filesystem::path game = machine.dir.file("roms/ps3/Some Game.ps3").lexically_normal();
    std::filesystem::create_directories(game);
    EmulatorProfile folderGame = retroArch();
    folderGame.systems["ps3"].variables = {{"core", "{var:cores}\\gambatte_libretro.dll"}};
    const LaunchPlan folder = planLaunch(target(game, "ps3"), folderGame, machine.probe);
    CHECK(requireReady(folder).arguments[2] == rv::core::pathToUtf8(game));
}

TEST_CASE("launcher/launch_plan: configuration problems come before a missing emulator", "[launcher][launch_plan]") {
    Machine machine;
    const std::filesystem::path rom = machine.rom("psx/Game.cue");
    // psx is not one of the profile's systems: no core variable.
    const LaunchPlan noCore = planLaunch(target(rom, "psx"), retroArch(), machine.probe);
    requireBlocked(noCore, BlockerKind::VariableMissing,
                   {"unknown variable {var:core}", "profile 'retroarch'", "arguments[1]", "system \"psx\""});
    CHECK_FALSE(noCore.executable.has_value()); // decided before the executable was looked for

    // The same with no RetroArch on the machine: still the configuration.
    rv::test::FakeSystemProbe empty;
    requireBlocked(planLaunch(target(rom, "psx"), retroArch(), empty), BlockerKind::VariableMissing);

    SECTION("an unknown token") {
        EmulatorProfile profile = retroArch();
        profile.arguments = {"-L", "{var:core}", "{rom_path}"};
        requireBlocked(planLaunch(target(rom, "gb"), profile, empty), BlockerKind::TemplateError,
                       {"unknown token {rom_path}", "arguments[2]"});
    }
    SECTION("variables that refer to each other") {
        EmulatorProfile profile = retroArch();
        profile.variables["cores"] = "{var:core}\\..";
        requireBlocked(planLaunch(target(rom, "gb"), profile, machine.probe), BlockerKind::TemplateError,
                       {"refer to each other"});
    }
    SECTION("an environment variable outside the allow-list, one that is not set") {
        EmulatorProfile profile = retroArch();
        profile.environment = {{"X", "{env:PATH}"}};
        requireBlocked(planLaunch(target(rom, "gb"), profile, machine.probe), BlockerKind::TemplateError,
                       {"{env:PATH}", "environment.X"});
        profile.environment = {{"X", "{env:LOCALAPPDATA}\\x"}};
        PlanOptions options;
        options.environment = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
        requireBlocked(planLaunch(target(rom, "gb"), profile, machine.probe, options), BlockerKind::VariableMissing,
                       {"{env:LOCALAPPDATA} is not set"});
    }
    SECTION("a required file's template") {
        EmulatorProfile profile = retroArch();
        profile.requiredFiles = {"{var:bios}"};
        requireBlocked(planLaunch(target(rom, "gb"), profile, machine.probe), BlockerKind::VariableMissing,
                       {"{var:bios}", "requiredFiles[0]"});
    }
    SECTION("a working directory that is not absolute") {
        EmulatorProfile profile = retroArch();
        profile.workingDirectory = "{rom_name}";
        const std::filesystem::path gb = machine.rom("gb/Game.gb");
        requireBlocked(planLaunch(target(gb, "gb"), profile, machine.probe), BlockerKind::TemplateError,
                       {"not an absolute path"});
    }
}

TEST_CASE("launcher/launch_plan: the executable", "[launcher][launch_plan]") {
    Machine machine;
    const std::filesystem::path rom = machine.rom("gb/Game.gb");
    SECTION("not found") {
        rv::test::FakeSystemProbe empty;
        const LaunchPlan plan = planLaunch(target(rom), retroArch(), empty);
        requireBlocked(plan, BlockerKind::ExecutableNotFound,
                       {"RetroArch (profile 'retroarch')", "App Paths \"retroarch.exe\" is not registered"});
        CHECK_FALSE(plan.executable.has_value());
    }
    SECTION("a command interpreter is never started") {
        EmulatorProfile profile = retroArch();
        profile.executable.appPathsName = "cmd.exe";
        machine.registerAppPath("cmd.exe", rv::test::writeProgramStub(machine.dir.file("System/CMD.EXE")));
        const LaunchPlan plan = planLaunch(target(rom), profile, machine.probe);
        requireBlocked(plan, BlockerKind::ProgramRefused, {"CMD.EXE", "command interpreter"});
        REQUIRE(plan.executable.has_value()); // found, then refused
    }
    SECTION("a file that is not a program") {
        EmulatorProfile profile = retroArch();
        profile.executable.appPathsName = "emu.exe";
        const std::filesystem::path text = machine.dir.file("Emulators/Retro Arch/emu.exe");
        std::ofstream(text) << "not a program";
        machine.registerAppPath("emu.exe", text);
        requireBlocked(planLaunch(target(rom), profile, machine.probe), BlockerKind::InvalidRequest,
                       {"not a Windows program"});
    }
    SECTION("a batch file") {
        EmulatorProfile profile = retroArch();
        profile.executable.appPathsName = "emu.bat";
        const std::filesystem::path batch = machine.dir.file("Emulators/Retro Arch/emu.bat"); // beside the cores
        std::ofstream(batch) << "@echo off\n";
        machine.registerAppPath("emu.bat", batch);
        requireBlocked(planLaunch(target(rom), profile, machine.probe), BlockerKind::InvalidRequest, {"batch"});
    }
    SECTION("a working directory that does not exist") {
        EmulatorProfile profile = retroArch();
        profile.workingDirectory = "{rom_dir}\\missing";
        requireBlocked(planLaunch(target(rom), profile, machine.probe), BlockerKind::InvalidRequest,
                       {"working directory"});
    }
}

TEST_CASE("launcher/launch_plan: a required file that is missing", "[launcher][launch_plan]") {
    Machine machine;
    const std::filesystem::path rom = machine.rom("snes/Game.sfc");
    const LaunchPlan plan = planLaunch(target(rom, "snes"), retroArch(), machine.probe);
    requireBlocked(plan, BlockerKind::RequiredFileMissing,
                   {"snes9x_libretro.dll", "profile 'retroarch'", "requiredFiles[0]", "system \"snes\""});
    REQUIRE(plan.executable.has_value());
    CHECK(plan.executable->path == machine.emulator.lexically_normal());
}

TEST_CASE("launcher/launch_plan: command interpreters and launchers by name", "[launcher][launch_plan]") {
    for (const std::string_view name :
         {R"(C:\Windows\System32\cmd.exe)", R"(C:\Windows\System32\CMD.EXE)", "powershell.exe", "PowerShell.exe",
          "pwsh.exe", "wscript.exe", "cscript.exe", "mshta.exe", "rundll32.exe", "explorer.exe", "conhost.exe",
          "wt.exe", "wsl.exe", "bash.exe", "msiexec.exe", "regsvr32.exe", "cmd.com", "cmd"}) {
        INFO(name);
        CHECK(isRefusedProgram(rv::core::pathFromUtf8(name)));
    }
    for (const std::string_view name : {"retroarch.exe", "cmdline.exe", "commander.exe", "bash-emulator.exe",
                                        "duckstation-qt-x64-ReleaseLTCG.exe", ""}) {
        INFO(name);
        CHECK_FALSE(isRefusedProgram(rv::core::pathFromUtf8(name)));
    }
}

TEST_CASE("launcher/launch_plan: the shipped RetroArch profile on a machine", "[launcher][launch_plan]") {
    const auto document = rv::launcher::loadProfileDocument(
        std::filesystem::path(RV_TEST_ASSET_DIR) / "launcher" / "emulators.json", rv::launcher::ProfileOrigin::Shipped);
    REQUIRE(document.issues.empty());
    const auto shipped = std::find_if(document.profiles.begin(), document.profiles.end(),
                                      [](const EmulatorProfile& profile) { return profile.id == "retroarch"; });
    REQUIRE(shipped != document.profiles.end());

    Machine machine;
    const std::filesystem::path gb = machine.rom("gb/Game.gb");
    const LaunchPlan plan = planLaunch(target(gb), *shipped, machine.probe);
    const auto& request = requireReady(plan);
    CHECK(request.arguments == std::vector<std::string>{"-L",
                                                        rv::core::pathToUtf8(machine.cores / "gambatte_libretro.dll"),
                                                        rv::core::pathToUtf8(gb)});
    // A system whose core is not installed, one the profile does not serve.
    requireBlocked(planLaunch(target(machine.rom("psx/Game.cue"), "psx"), *shipped, machine.probe),
                   BlockerKind::RequiredFileMissing, {"mednafen_psx_libretro.dll"});
    requireBlocked(planLaunch(target(machine.rom("ps3/Game.iso"), "ps3"), *shipped, machine.probe),
                   BlockerKind::VariableMissing, {"{var:core}"});

    // Located through the user's home folder when App Paths has nothing.
    rv::test::FakeSystemProbe home;
    home.setHomeFolder(machine.dir.path());
    const std::filesystem::path roaming =
        rv::test::writeProgramStub(machine.dir.file("AppData/Roaming/RetroArch/retroarch.exe"));
    home.addFile(roaming);
    home.addFile(roaming.parent_path() / "cores" / "gambatte_libretro.dll");
    const LaunchPlan fromHome = planLaunch(target(gb), *shipped, home);
    CHECK(requireReady(fromHome).executable == roaming.lexically_normal());
    REQUIRE(fromHome.executable.has_value());
    CHECK(fromHome.executable->rule == LocationRule::Path);
}

#if defined(_WIN32)
TEST_CASE("launcher/launch_plan: a planned launch reaches the program argument for argument",
          "[launcher][launch_plan]") {
    const std::filesystem::path fake = rv::core::executablePath().parent_path() / "rv_fake_emulator.exe";
    if (!std::filesystem::exists(fake)) {
        SKIP("rv_fake_emulator.exe is not next to the tests");
    }
    Machine machine;
    machine.registerAppPath("rv_fake_emulator.exe", fake);
    const std::filesystem::path rom = machine.rom("gb/" + longRelativePath());
    const std::filesystem::path printed = machine.dir.file("out/args.json");
    const std::filesystem::path cwd = machine.dir.file("out/cwd.json");
    std::filesystem::create_directories(printed.parent_path());

    EmulatorProfile profile;
    profile.id = "fake";
    profile.displayName = "Fake";
    profile.executable.appPathsName = "rv_fake_emulator.exe";
    profile.arguments = {"--print-args", "{var:args}", "--print-cwd", "{var:cwd}", "--exit-timeout-ms", "60000",
                         "--",           "{rom}",      "-image={rom}", "{rom_name}", "{rom_ext}",        "{system}"};
    profile.variables = {{"args", rv::core::pathToUtf8(printed)}, {"cwd", rv::core::pathToUtf8(cwd)}};
    profile.hideConsoleWindow = true;
    const LaunchPlan plan = planLaunch(target(rom), profile, machine.probe);
    const auto& request = requireReady(plan);

    rv::platform_win::Win32ProcessRunner runner;
    rv::launcher::LaunchSession session(request, runner);
    session.start(rv::launcher::LaunchClock::now());
    REQUIRE(session.state() == rv::launcher::SessionState::Running);
    const auto deadline = rv::launcher::LaunchClock::now() + 60s;
    while (!session.finished() && rv::launcher::LaunchClock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        session.poll(rv::launcher::LaunchClock::now());
    }
    REQUIRE(session.state() == rv::launcher::SessionState::Returned);

    const auto echoed = rv::core::readJsonFile(printed);
    REQUIRE(echoed.has_value());
    const std::string romText = rv::core::pathToUtf8(rom);
    const std::vector<std::string> payload{romText, "-image=" + romText, rv::core::pathToUtf8(rom.stem()), ".gb", "gb"};
    CHECK(echoed->at("payload").get<std::vector<std::string>>() == payload);
    CHECK(romText.size() > 260);
    // The emulator ran in its own folder (ADR-0008 §8).
    const auto directory = rv::core::readJsonFile(cwd);
    REQUIRE(directory.has_value());
    INFO(directory->dump());
    CHECK(std::filesystem::equivalent(rv::core::pathFromUtf8(directory->at("cwd").get<std::string>()),
                                      fake.parent_path()));
}
#endif
