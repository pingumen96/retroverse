#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

#include <rv/core/case_fold.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/environment.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/launcher/launch_session.hpp>
#include <rv/platform_win/win32_process_runner.hpp>

#include "support/file_system.hpp"
#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

// The automation guard against real files: program stubs in a temporary folder, reached by other names (case, 8.3 short
// names, '..', trailing dots, links, junctions, hard links, streams, device paths) to show that none of them gets past
// it. The tests that set RV_LAUNCH_ALLOWLIST put back what was there.

using namespace std::chrono_literals;
using rv::launcher::checkLaunchGuard;
using rv::launcher::GuardRule;
using rv::launcher::kLaunchAllowListVariable;
using rv::launcher::LaunchGuard;
using rv::launcher::LaunchRequest;

namespace {

/// Sets (or, with nothing, removes) an environment variable of this process until the end of the scope.
class ScopedVariable {
public:
    ScopedVariable(std::string name, std::optional<std::string> value)
        : m_name(std::move(name)), m_previous(rv::launcher::processEnvironmentVariable(m_name)) {
        set(value);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ~ScopedVariable() { set(m_previous); }

private:
    void set(const std::optional<std::string>& value) const {
        const auto name = rv::core::utf8ToWide(m_name);
        const auto wide = rv::core::utf8ToWide(value.value_or(std::string()));
        SetEnvironmentVariableW(name->c_str(), value ? wide->c_str() : nullptr);
    }

    std::string m_name;
    std::optional<std::string> m_previous;
};

/// Program stubs, an appended configuration and a few links, in a temporary folder.
struct Files {
    rv::test::TempDir dir{"rv-launch-guard"};
    std::filesystem::path fake = stub("bin/rv_fake_emulator.exe");
    std::filesystem::path other = stub("bin/other.exe");
    std::filesystem::path duckstation = stub("DuckStation/duckstation-qt-x64-ReleaseLTCG.exe");
    std::filesystem::path retroarch = stub("RetroArch/retroarch.exe");
    std::filesystem::path config = text("scratch/retroverse-isolated.cfg");
    std::filesystem::path config2 = text("scratch/second.cfg");

    std::filesystem::path stub(std::string_view relative) {
        return rv::test::writeProgramStub(dir.file(relative).lexically_normal());
    }
    std::filesystem::path text(std::string_view relative) {
        const std::filesystem::path path = dir.file(relative).lexically_normal();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "config_save_on_exit = \"false\"\n";
        return path;
    }
    [[nodiscard]] std::string utf8(const std::filesystem::path& path) const { return rv::core::pathToUtf8(path); }
};

LaunchRequest request(const std::filesystem::path& executable, std::vector<std::string> arguments = {}) {
    LaunchRequest r;
    r.executable = executable;
    r.arguments = std::move(arguments);
    return r;
}

GuardRule ruleOf(const LaunchGuard& guard, const std::filesystem::path& executable,
                 std::vector<std::string> arguments = {}) {
    const auto verdict = guard.check(request(executable, std::move(arguments)));
    UNSCOPED_INFO(rv::core::pathToUtf8Lossy(executable) << " -> " << rv::launcher::toString(verdict.rule) << ": "
                                                        << verdict.message);
    CHECK(verdict.allowed() == (verdict.rule == GuardRule::Allowed || verdict.rule == GuardRule::Inactive));
    CHECK(verdict.allowed() == verdict.message.empty());
    return verdict.rule;
}

std::filesystem::path upperCased(const std::filesystem::path& path) {
    const auto upper = rv::core::fileSystemUpperCase(rv::core::pathToUtf8(path));
    REQUIRE(upper.has_value());
    return rv::core::pathFromUtf8(*upper);
}

/// The 8.3 short form of an existing path (names that are 8.3 names already stay as they are).
std::filesystem::path shortPath(const std::filesystem::path& path) {
    const DWORD size = GetShortPathNameW(path.c_str(), nullptr, 0);
    REQUIRE(size != 0);
    std::wstring buffer(size, L'\0');
    const DWORD length = GetShortPathNameW(path.c_str(), buffer.data(), size);
    REQUIRE((length != 0 && length < size));
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

/// True when `path`'s file name is a generated short name ("DUCKST~1.EXE").
bool isShortName(const std::filesystem::path& path) {
    return path.filename().native().find(L'~') != std::wstring::npos;
}

/// A symbolic link to a file; nothing when this user may not create one (no Developer Mode, no privilege).
std::optional<std::filesystem::path> fileSymlink(const std::filesystem::path& link,
                                                 const std::filesystem::path& target) {
    std::filesystem::create_directories(link.parent_path());
    if (!CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        return std::nullopt;
    }
    return link;
}

std::filesystem::path fakeEmulator() {
    return rv::core::executablePath().parent_path() / "rv_fake_emulator.exe";
}

} // namespace

TEST_CASE("launcher/launch_guard: an inactive guard allows everything", "[launcher][launch_guard]") {
    Files files;
    const LaunchGuard guard;
    CHECK_FALSE(guard.active());
    CHECK(ruleOf(guard, files.duckstation) == GuardRule::Inactive);
    CHECK(ruleOf(guard, files.retroarch) == GuardRule::Inactive);
    CHECK(ruleOf(guard, "relative.exe") == GuardRule::Inactive);
    CHECK(ruleOf(guard, files.dir.file("missing.exe")) == GuardRule::Inactive);
}

TEST_CASE("launcher/launch_guard: the allow-list's syntax", "[launcher][launch_guard]") {
    const LaunchGuard guard = LaunchGuard::fromAllowList(
        R"( "C:\Emu dir\emu.exe" ; ;relative.exe;D:\y.exe;\\.\C:\device.exe; C:\s.exe:stream ;\\nas\share\e.exe;)");
    CHECK(guard.active());
    REQUIRE(guard.entries().size() == 3);
    CHECK(guard.entries()[0] == std::filesystem::path(LR"(C:\Emu dir\emu.exe)"));
    CHECK(guard.entries()[1] == std::filesystem::path(LR"(D:\y.exe)"));
    CHECK(guard.entries()[2] == std::filesystem::path(LR"(\\nas\share\e.exe)"));
    const std::vector<std::string> ignored(guard.ignoredEntries().begin(), guard.ignoredEntries().end());
    CHECK(ignored == std::vector<std::string>{"relative.exe", R"(\\.\C:\device.exe)", R"(C:\s.exe:stream)"});

    // Set but empty or blank: active, and nothing may start.
    Files files;
    for (const std::string_view list : {"", "   ", ";;", "relative.exe"}) {
        INFO("[" << list << "]");
        const LaunchGuard nothing = LaunchGuard::fromAllowList(list);
        CHECK(nothing.active());
        CHECK(nothing.entries().empty());
        CHECK(ruleOf(nothing, files.fake) == GuardRule::NotListed);
    }
}

TEST_CASE("launcher/launch_guard: only listed executables start", "[launcher][launch_guard]") {
    Files files;
    const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.fake) + ";" + files.utf8(files.retroarch));
    CHECK(ruleOf(guard, files.fake) == GuardRule::Allowed);
    CHECK(ruleOf(guard, files.other) == GuardRule::NotListed);
    const auto refused = guard.check(request(files.other));
    CHECK(refused.message.find("RV_LAUNCH_ALLOWLIST") != std::string::npos);
    CHECK(refused.message.find("other.exe") != std::string::npos);
    // Missing: nothing to resolve.
    CHECK(ruleOf(guard, files.dir.file("bin/missing.exe")) == GuardRule::Unresolved);
}

TEST_CASE("launcher/launch_guard: DuckStation never starts, listed or not", "[launcher][launch_guard]") {
    Files files;
    const std::filesystem::path plain = files.stub("DuckStation/DuckStation.exe");
    const std::filesystem::path nogui = files.stub("DuckStation/duckstation-nogui-x64-ReleaseLTCG.exe");
    const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.duckstation) + ";" + files.utf8(plain) +
                                                         ";" + files.utf8(nogui));
    CHECK(ruleOf(guard, files.duckstation) == GuardRule::DuckStation);
    CHECK(ruleOf(guard, plain) == GuardRule::DuckStation);
    CHECK(ruleOf(guard, nogui) == GuardRule::DuckStation);
    CHECK(ruleOf(LaunchGuard::fromAllowList(""), files.duckstation) == GuardRule::DuckStation);
    // Refused by name before anything else, even when the file is gone.
    CHECK(ruleOf(guard, files.dir.file("elsewhere/duckstation.exe")) == GuardRule::DuckStation);
    CHECK(guard.check(request(files.duckstation)).message.find("DuckStation") != std::string::npos);
}

TEST_CASE("launcher/launch_guard: RetroArch starts only isolated", "[launcher][launch_guard]") {
    Files files;
    const std::filesystem::path angle = files.stub("RetroArch/retroarch_angle.exe");
    const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.retroarch) + ";" + files.utf8(angle));
    const std::string config = files.utf8(files.config);
    const std::string config2 = files.utf8(files.config2);
    const std::string missing = files.utf8(files.dir.file("scratch/missing.cfg"));
    struct Case {
        std::vector<std::string> arguments;
        GuardRule rule;
    };
    const std::vector<Case> cases{
        {{}, GuardRule::RetroArchNotIsolated},
        {{"-L", "core.dll", "rom.gb"}, GuardRule::RetroArchNotIsolated},
        {{"--appendconfig", config}, GuardRule::Allowed},
        {{"--appendconfig=" + config}, GuardRule::Allowed},
        {{"-L", "core.dll", "--appendconfig=" + config, "--verbose", "rom.gb"}, GuardRule::Allowed},
        {{"-L", "core.dll", "rom.gb", "--appendconfig", config}, GuardRule::Allowed}, // options are permuted
        {{"--appendconfig=" + config + "|" + config2}, GuardRule::Allowed},
        {{"--appendconfig", config, "--", "rom.gb"}, GuardRule::Allowed},
        // What does not isolate it.
        {{"--appendconfig=" + missing + "|" + config}, GuardRule::RetroArchNotIsolated}, // every file must exist
        {{"--appendconfig=" + missing}, GuardRule::RetroArchNotIsolated},
        {{"--appendconfig=" + config + "|" + missing}, GuardRule::RetroArchNotIsolated},
        {{"--appendconfig=scratch\\retroverse-isolated.cfg"}, GuardRule::RetroArchNotIsolated}, // relative
        {{"--appendconfig="}, GuardRule::RetroArchNotIsolated},
        {{"--appendconfig"}, GuardRule::RetroArchNotIsolated}, // no value
        {{"--APPENDCONFIG=" + config}, GuardRule::RetroArchNotIsolated}, // options are case-sensitive
        {{"--AppendConfig", config}, GuardRule::RetroArchNotIsolated},
        {{"-appendconfig", config}, GuardRule::RetroArchNotIsolated},
        {{"--", "--appendconfig=" + config}, GuardRule::RetroArchNotIsolated}, // content after "--"
        {{"rom.gb --appendconfig=" + config}, GuardRule::RetroArchNotIsolated}, // one argument, not the option
        {{"-L", "--appendconfig", config, "rom.gb"}, GuardRule::RetroArchNotIsolated}, // -L's value
        {{"-vL", "--appendconfig", config}, GuardRule::RetroArchNotIsolated},           // grouped options
        {{"--libretro", "--appendconfig", config}, GuardRule::RetroArchNotIsolated},
        {{"--libr", "--appendconfig", config}, GuardRule::RetroArchNotIsolated}, // an abbreviated --libretro
        {{"--appendconfig=" + config, "--appendconfig=" + missing}, GuardRule::RetroArchNotIsolated}, // the last wins
        {{"--appendconfig=" + config, "--appendc=" + config}, GuardRule::RetroArchNotIsolated},
        {{"--appendconfig=" + config, "--appendconfig"}, GuardRule::RetroArchNotIsolated},
        {{"--appendconfig=" + missing, "--appendconfig=" + config}, GuardRule::RetroArchNotIsolated}, // every one
        // A flag read as an option taking a value would hide what follows it: RetroArch's own table decides.
        {{"--max-frames-ss", "-L", "--appendconfig=" + config}, GuardRule::RetroArchNotIsolated},
        {{"--max-frames-ss", "--appendconfig=" + config}, GuardRule::Allowed},
        {{"--verbose", "--appendconfig", config, "-f", "rom.gb"}, GuardRule::Allowed},
        {{"--unknown-option", "--appendconfig=" + config}, GuardRule::RetroArchNotIsolated}, // RetroArch stops there
        {{"--max", "240", "--appendconfig=" + config}, GuardRule::RetroArchNotIsolated},    // ambiguous
        {{"-x", "--appendconfig=" + config}, GuardRule::RetroArchNotIsolated},
        {{"--ap=" + config}, GuardRule::RetroArchNotIsolated},
        // The I1 spike's isolated run (spikes/launch).
        {{"-L", "C:\\RetroArch\\cores\\gambatte_libretro.dll", "--appendconfig=" + config, "--max-frames=240",
          "--sram-mode=noload-nosave", "--log-file=C:\\scratch\\retroarch.log", "--verbose", "C:\\Games\\a b.gb"},
         GuardRule::Allowed},
    };
    for (const Case& c : cases) {
        std::string shown;
        for (const std::string& argument : c.arguments) {
            shown += "[" + argument + "] ";
        }
        INFO(shown);
        CHECK(ruleOf(guard, files.retroarch, c.arguments) == c.rule);
    }
    CHECK(ruleOf(guard, angle) == GuardRule::RetroArchNotIsolated); // every retroarch*.exe
    CHECK(ruleOf(guard, angle, {"--appendconfig", config}) == GuardRule::Allowed);
    // Isolated but not listed.
    CHECK(ruleOf(LaunchGuard::fromAllowList(files.utf8(files.fake)), files.retroarch, {"--appendconfig", config}) ==
          GuardRule::NotListed);
}

TEST_CASE("launcher/launch_guard: bypass attempts: another case", "[launcher][launch_guard]") {
    Files files;
    const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.fake) + ";" + files.utf8(files.retroarch) +
                                                         ";" + files.utf8(files.duckstation));
    CHECK(ruleOf(guard, upperCased(files.fake)) == GuardRule::Allowed); // the same file
    CHECK(ruleOf(guard, upperCased(files.duckstation)) == GuardRule::DuckStation);
    CHECK(ruleOf(guard, upperCased(files.retroarch)) == GuardRule::RetroArchNotIsolated);
    CHECK(ruleOf(guard, upperCased(files.retroarch), {"--appendconfig", files.utf8(files.config)}) ==
          GuardRule::Allowed);
    CHECK(ruleOf(guard, upperCased(files.other)) == GuardRule::NotListed);
    // A list written in another case.
    const LaunchGuard upper = LaunchGuard::fromAllowList(files.utf8(upperCased(files.fake)));
    CHECK(ruleOf(upper, files.fake) == GuardRule::Allowed);
}

TEST_CASE("launcher/launch_guard: bypass attempts: 8.3 short names", "[launcher][launch_guard]") {
    Files files;
    const std::filesystem::path shortDuck = shortPath(files.duckstation);
    const std::filesystem::path shortRetro = shortPath(files.retroarch);
    const std::filesystem::path shortFake = shortPath(files.fake);
    const std::filesystem::path shortOther = shortPath(files.other); // other.exe is an 8.3 name: its folders are short
    if (!isShortName(shortDuck) || !isShortName(shortRetro) || !isShortName(shortFake)) {
        SKIP("the temporary folder's volume keeps no 8.3 short names");
    }
    INFO(rv::core::pathToUtf8(shortDuck) << ", " << rv::core::pathToUtf8(shortRetro));
    const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.fake) + ";" + files.utf8(files.retroarch) +
                                                         ";" + files.utf8(shortDuck));
    CHECK(ruleOf(guard, shortDuck) == GuardRule::DuckStation);
    CHECK(ruleOf(guard, shortRetro) == GuardRule::RetroArchNotIsolated);
    CHECK(ruleOf(guard, shortRetro, {"--appendconfig", files.utf8(files.config)}) == GuardRule::Allowed);
    CHECK(ruleOf(guard, shortFake) == GuardRule::Allowed); // listed by its long name
    CHECK(ruleOf(guard, shortOther) == GuardRule::NotListed);
    // Listed by its short name, started by its long one.
    CHECK(ruleOf(LaunchGuard::fromAllowList(files.utf8(shortFake)), files.fake) == GuardRule::Allowed);
}

TEST_CASE("launcher/launch_guard: bypass attempts: relative paths", "[launcher][launch_guard]") {
    Files files;
    const LaunchGuard guard =
        LaunchGuard::fromAllowList(files.utf8(files.fake) + ";bin\\rv_fake_emulator.exe;rv_fake_emulator.exe");
    CHECK(guard.entries().size() == 1);
    for (const std::wstring_view relative : {L"rv_fake_emulator.exe", L".\\rv_fake_emulator.exe",
                                             L"bin\\rv_fake_emulator.exe", L"..\\bin\\rv_fake_emulator.exe",
                                             L"C:rv_fake_emulator.exe", L"\\rv_fake_emulator.exe",
                                             L"duckstation-qt-x64-ReleaseLTCG.exe"}) {
        CHECK(ruleOf(guard, std::filesystem::path(relative)) == GuardRule::UnsupportedPath);
    }
    CHECK(guard.check(request("rv_fake_emulator.exe")).message.find("relative path") != std::string::npos);
}

TEST_CASE("launcher/launch_guard: bypass attempts: '..', trailing dots and spaces", "[launcher][launch_guard]") {
    Files files;
    const std::wstring root = files.dir.path().native();
    const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.fake));
    CHECK(ruleOf(guard, root + LR"(\bin\nowhere\..\rv_fake_emulator.exe)") == GuardRule::Allowed);
    CHECK(ruleOf(guard, root + LR"(\bin\rv_fake_emulator.exe\..\other.exe)") == GuardRule::NotListed);
    CHECK(ruleOf(guard, root + LR"(\bin\..\DuckStation\duckstation-qt-x64-ReleaseLTCG.exe)") ==
          GuardRule::DuckStation);
    // Windows drops trailing dots and spaces before it opens a file: so does the guard before it reads the name.
    CHECK(ruleOf(guard, root + LR"(\DuckStation\duckstation-qt-x64-ReleaseLTCG.exe.)") == GuardRule::DuckStation);
    CHECK(ruleOf(guard, root + LR"(\DuckStation\duckstation-qt-x64-ReleaseLTCG.exe . )") == GuardRule::DuckStation);
    CHECK(ruleOf(guard, root + LR"(\bin\rv_fake_emulator.exe.)") == GuardRule::Allowed);
    // '/' separators and a list entry with '..'.
    CHECK(ruleOf(guard, rv::core::pathFromUtf8(rv::core::pathToGenericUtf8(files.fake))) == GuardRule::Allowed);
    CHECK(ruleOf(LaunchGuard::fromAllowList(files.utf8(files.dir.path()) + R"(\scratch\..\bin\rv_fake_emulator.exe)"),
                 files.fake) == GuardRule::Allowed);
}

TEST_CASE("launcher/launch_guard: bypass attempts: links and junctions", "[launcher][launch_guard]") {
    Files files;
    const std::string config = files.utf8(files.config);
    SECTION("junctions") {
        auto junction = rv::test::Junction::create(files.dir.file("through"), files.dir.file("bin"));
        REQUIRE(junction.has_value());
        const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.fake));
        CHECK(ruleOf(guard, files.dir.file("through/rv_fake_emulator.exe")) == GuardRule::Allowed); // the same file
        CHECK(ruleOf(guard, files.dir.file("through/other.exe")) == GuardRule::NotListed);
        // Listed through the junction, started by its real path.
        CHECK(ruleOf(LaunchGuard::fromAllowList(files.utf8(files.dir.file("through/rv_fake_emulator.exe"))),
                     files.fake) == GuardRule::Allowed);
        auto duck = rv::test::Junction::create(files.dir.file("innocent"), files.dir.file("DuckStation"));
        REQUIRE(duck.has_value());
        CHECK(ruleOf(guard, files.dir.file("innocent/duckstation-qt-x64-ReleaseLTCG.exe")) == GuardRule::DuckStation);
    }
    SECTION("symbolic links: the file they lead to decides") {
        const auto innocent = fileSymlink(files.dir.file("links/innocent.exe"), files.duckstation);
        if (!innocent) {
            SKIP("this user may not create symbolic links (Developer Mode is off)");
        }
        const auto retro = fileSymlink(files.dir.file("links/emu.exe"), files.retroarch);
        const auto toFake = fileSymlink(files.dir.file("links/fake-link.exe"), files.fake);
        const auto lookalike = fileSymlink(files.dir.file("links/rv_fake_emulator.exe"), files.other);
        REQUIRE(retro.has_value());
        REQUIRE(toFake.has_value());
        REQUIRE(lookalike.has_value());
        const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(*innocent) + ";" + files.utf8(*retro) + ";" +
                                                             files.utf8(files.fake));
        CHECK(ruleOf(guard, *innocent) == GuardRule::DuckStation); // listed by its innocent name
        CHECK(ruleOf(guard, *retro) == GuardRule::RetroArchNotIsolated);
        CHECK(ruleOf(guard, *retro, {"--appendconfig", config}) == GuardRule::Allowed);
        CHECK(ruleOf(guard, *toFake) == GuardRule::Allowed);       // leads to a listed file
        CHECK(ruleOf(guard, *lookalike) == GuardRule::NotListed); // named like a listed file, leads elsewhere
        // A listed link allows the file it leads to, under its own name too.
        CHECK(ruleOf(LaunchGuard::fromAllowList(files.utf8(*toFake)), files.fake) == GuardRule::Allowed);
    }
    SECTION("hard links are not recognized by name: only the list keeps them from starting") {
        const std::filesystem::path renamed = files.dir.file("links/renamed.exe");
        std::filesystem::create_directories(renamed.parent_path());
        REQUIRE(CreateHardLinkW(renamed.c_str(), files.duckstation.c_str(), nullptr));
        CHECK(ruleOf(LaunchGuard::fromAllowList(files.utf8(files.fake)), renamed) == GuardRule::NotListed);
        CHECK(ruleOf(LaunchGuard::fromAllowList(files.utf8(renamed)), renamed) == GuardRule::Allowed);
    }
}

TEST_CASE("launcher/launch_guard: bypass attempts: streams and device paths", "[launcher][launch_guard]") {
    Files files;
    const LaunchGuard guard = LaunchGuard::fromAllowList(files.utf8(files.fake));
    const std::wstring fake = files.fake.native();
    CHECK(ruleOf(guard, fake + L":hidden") == GuardRule::UnsupportedPath);
    CHECK(ruleOf(guard, fake + L"::$DATA") == GuardRule::UnsupportedPath);
    CHECK(ruleOf(guard, L"\\\\.\\" + fake) == GuardRule::UnsupportedPath);
    CHECK(ruleOf(guard, L"//./" + fake) == GuardRule::UnsupportedPath);
    CHECK(ruleOf(guard, LR"(\\?\GLOBALROOT\Device\HarddiskVolume1\x.exe)") == GuardRule::UnsupportedPath);
    CHECK(ruleOf(guard, LR"(\??\)" + fake) == GuardRule::UnsupportedPath);
    // The \\?\ form of a listed path is that path.
    CHECK(ruleOf(guard, L"\\\\?\\" + fake) == GuardRule::Allowed);
}

TEST_CASE("launcher/launch_guard: the process environment", "[launcher][launch_guard]") {
    Files files;
    const std::filesystem::path unicode = files.stub("\xC3\x89mulateurs \xE6\x97\xA5\xE6\x9C\xAC/emu.exe");
    {
        const ScopedVariable unset{std::string(kLaunchAllowListVariable), std::nullopt};
        CHECK_FALSE(LaunchGuard::fromEnvironment().active());
        CHECK(checkLaunchGuard(request(files.duckstation)).has_value()); // a no-op while unset
    }
    {
        const ScopedVariable set{std::string(kLaunchAllowListVariable),
                                 files.utf8(files.fake) + ";" + files.utf8(unicode)};
        const LaunchGuard guard = LaunchGuard::fromEnvironment();
        CHECK(guard.active());
        CHECK(guard.entries().size() == 2);
        CHECK(checkLaunchGuard(request(files.fake)).has_value());
        CHECK(checkLaunchGuard(request(unicode)).has_value());
        const auto refused = checkLaunchGuard(request(files.other));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == rv::ErrorCode::ValidationFailed);
        CHECK(refused.error().context == "RV_LAUNCH_ALLOWLIST");
        CHECK(refused.error().message.starts_with("refused by the automation guard: "));
    }
    {
        const ScopedVariable empty{std::string(kLaunchAllowListVariable), std::string()};
        CHECK(LaunchGuard::fromEnvironment().active()); // set, even to nothing
        CHECK_FALSE(checkLaunchGuard(request(files.fake)).has_value());
    }
}

TEST_CASE("launcher/environment: the process's variables as they are now", "[launcher][environment]") {
    const std::string name = "RV_TEST_PROCESS_VARIABLE";
    {
        const ScopedVariable unset{name, std::nullopt};
        CHECK_FALSE(rv::launcher::processEnvironmentVariable(name).has_value());
    }
    {
        const ScopedVariable empty{name, std::string()};
        CHECK(rv::launcher::processEnvironmentVariable(name) == std::string());
    }
    {
        const std::string value = "Pok\xC3\xA9mon \xF0\x9F\x8E\xAE " + std::string(300, 'x'); // past the first buffer
        const ScopedVariable set{name, value};
        CHECK(rv::launcher::processEnvironmentVariable(name) == value);
        CHECK(rv::launcher::processEnvironmentVariable("rv_test_process_variable") == value); // names ignore case
    }
    CHECK_FALSE(rv::launcher::processEnvironmentVariable("").has_value());
    CHECK_FALSE(rv::launcher::processEnvironmentVariable("A=B").has_value());
    CHECK_FALSE(rv::launcher::processEnvironmentVariable(std::string("A\0B", 3)).has_value());
}

TEST_CASE("launcher/launch_guard: the runner applies it before anything starts", "[launcher][launch_guard]") {
    if (!std::filesystem::exists(fakeEmulator())) {
        SKIP("rv_fake_emulator.exe is not next to the tests");
    }
    Files files;
    LaunchRequest fake = request(fakeEmulator(), {"--exit-code", "0"});
    fake.hideConsoleWindow = true;
    fake.label = "fake emulator";
    {
        const ScopedVariable set{std::string(kLaunchAllowListVariable), files.utf8(files.other)};
        rv::platform_win::Win32ProcessRunner runner;
        rv::launcher::LaunchSession session(fake, runner);
        bool handedOver = false;
        session.start(rv::launcher::LaunchClock::now(), {nullptr, [&](std::uint32_t) { handedOver = true; }});
        CHECK(session.state() == rv::launcher::SessionState::FailedToStart);
        CHECK(session.outcome().failure == rv::launcher::FailureReason::StartFailed);
        CHECK(session.outcome().message.find("RV_LAUNCH_ALLOWLIST") != std::string::npos);
        CHECK(session.process() == nullptr);
        CHECK(runner.lastStart().processId == 0);
        CHECK_FALSE(handedOver); // the window is never handed over for a launch that cannot happen
    }
    {
        const ScopedVariable set{std::string(kLaunchAllowListVariable), rv::core::pathToUtf8(fakeEmulator())};
        rv::platform_win::Win32ProcessRunner runner;
        rv::launcher::LaunchSession session(fake, runner);
        session.start(rv::launcher::LaunchClock::now());
        REQUIRE(session.state() == rv::launcher::SessionState::Running);
        const auto deadline = rv::launcher::LaunchClock::now() + 60s;
        while (!session.finished() && rv::launcher::LaunchClock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
            session.poll(rv::launcher::LaunchClock::now());
        }
        CHECK(session.state() == rv::launcher::SessionState::Returned);
    }
}
