// The manual's plan (I5 step 8, ADR-0016 §10; rv/launcher/manual_open.hpp): explorer.exe of the Windows folder with
// the manual's plain path as its one argument, whatever the name holds (a hostile name stays one argument as the shell's
// parser and explorer.exe read it); the paths explorer.exe would misread refused; and the automation guard refusing
// it under RV_LAUNCH_ALLOWLIST, so that no automated run ever opens a window. Nothing is started here: the runner's
// test only shows the guard's refusal before any process exists.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW

#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/launcher/manual_open.hpp>
#include <rv/platform_win/system_probe.hpp>
#include <rv/platform_win/win32_process_runner.hpp>

#include "support/fake_system_probe.hpp"
#include "support/scoped_environment.hpp"

using rv::launcher::manualArgument;
using rv::launcher::planManualOpen;

namespace {

std::filesystem::path path(std::string_view utf8) {
    auto converted = rv::core::tryPathFromUtf8(utf8);
    REQUIRE(converted.has_value());
    return *converted;
}

/// What a started program sees: the Windows shell's own parser.
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

/// This machine's Windows folder (its explorer.exe is only validated, never started).
const rv::platform_win::Win32SystemProbe& machine() {
    static const rv::platform_win::Win32SystemProbe probe;
    return probe;
}

std::filesystem::path fakeEmulator() {
    return rv::core::executablePath().parent_path() / "rv_fake_emulator.exe";
}

} // namespace

TEST_CASE("launcher/manual_open: explorer.exe with the manual as its one argument, hostile names included",
          "[launcher][manual_open]") {
    const std::filesystem::path windows = machine().windowsFolder();
    REQUIRE(std::filesystem::is_regular_file(windows / "explorer.exe"));
    for (const std::string_view manual : {
             "C:\\Media\\snes\\manuals\\Amber Lantern (USA).pdf",
             "C:\\Media\\snes\\manuals\\Plain.pdf",
             "C:\\Media\\snes\\manuals\\a & b | c > d ^e %PATH% !x! (World).pdf",
             "C:\\Media\\snes\\manuals\\Legend, The - Quest (USA).PDF",
             "C:\\Media\\snes\\manuals\\\xC3\xA8 un caf\xC3\xA8 \xE6\x97\xA5\xE6\x9C\xAC.pdf",
             "C:\\Media\\snes\\manuals\\end with backslash\\x.pdf",
             "C:\\Media\\snes\\manuals\\'quotes'; and $(subst) `ticks`.pdf",
             "\\\\server\\share\\manuals\\Opal Oriel (Europe).pdf",
         }) {
        INFO(manual);
        const auto request = planManualOpen(path(manual), machine(), "the manual of g0000000000000001");
        REQUIRE(request.has_value());
        CHECK(request->executable == windows / "explorer.exe");
        CHECK(request->workingDirectory == windows);
        REQUIRE(request->arguments.size() == 1);
        CHECK(request->arguments.front() == manual);
        CHECK(request->waitStrategy == rv::launcher::WaitStrategy::DirectProcess);
        CHECK(request->timeouts.startupGrace.count() == 0);
        CHECK(request->timeouts.windowTimeout.count() == 0);
        CHECK(request->environment.empty());
        CHECK(request->label == "the manual of g0000000000000001");
        // The command line gives it back as one argument after the program, as explorer.exe's own parser reads it.
        const auto commandLine = rv::launcher::commandLineOf(*request);
        REQUIRE(commandLine.has_value());
        const std::vector<std::string> argv = commandLineToArgv(*commandLine);
        REQUIRE(argv.size() == 2);
        CHECK(path(argv[0]) == windows / "explorer.exe");
        CHECK(argv[1] == manual);
    }
}

TEST_CASE("launcher/manual_open: the \\\\?\\ forms are given back as the paths they stand for",
          "[launcher][manual_open]") {
    const auto drive = manualArgument(path("\\\\?\\C:\\Media\\snes\\manuals\\Amber Lantern (USA).pdf"));
    REQUIRE(drive.has_value());
    CHECK(*drive == "C:\\Media\\snes\\manuals\\Amber Lantern (USA).pdf");
    const auto unc = manualArgument(path("\\\\?\\UNC\\server\\share\\manuals\\B.pdf"));
    REQUIRE(unc.has_value());
    CHECK(*unc == "\\\\server\\share\\manuals\\B.pdf");
}

TEST_CASE("launcher/manual_open: what explorer.exe would misread is refused", "[launcher][manual_open]") {
    const std::string longName(260, 'a');
    for (const std::string_view manual : {
             "manuals\\Relative.pdf",
             "C:Relative.pdf",
             "\\Rooted\\NoDrive.pdf",
             "\\\\.\\C:\\Device.pdf",
             "\\\\?\\GLOBALROOT\\Device\\x.pdf",
             "C:\\Media\\manual.txt",
             "C:\\Media\\manual.pdf.exe",
             "C:\\Media\\manual",
             "C:\\Media\\trailing dot.\\x.pdf",
             "C:\\Media\\trailing space \\x.pdf",
             "C:\\Media\\\\double.pdf",
             "C:\\Media\\a,b.pdf",
         }) {
        INFO(manual);
        const auto argument = manualArgument(path(manual));
        REQUIRE_FALSE(argument.has_value());
        CHECK(argument.error().code == rv::ErrorCode::Unsupported);
        CHECK_FALSE(planManualOpen(path(manual), machine()).has_value());
    }
    const auto tooLong = manualArgument(path("C:\\" + longName + ".pdf"));
    REQUIRE_FALSE(tooLong.has_value());
    CHECK(tooLong.error().code == rv::ErrorCode::Unsupported);
    // A Windows folder without explorer.exe: the request does not validate.
    rv::test::FakeSystemProbe probe;
    probe.setWindowsFolder(std::filesystem::temp_directory_path() / "rv-no-windows-here");
    CHECK_FALSE(planManualOpen(path("C:\\Media\\manual.pdf"), probe).has_value());
    probe.setWindowsFolder({});
    CHECK_FALSE(planManualOpen(path("C:\\Media\\manual.pdf"), probe).has_value());
}

TEST_CASE("launcher/manual_open: the automation guard refuses it under RV_LAUNCH_ALLOWLIST, nothing started",
          "[launcher][manual_open]") {
    const std::vector<std::filesystem::path> allowed{fakeEmulator()};
    const rv::test::ScopedLaunchAllowList allowList(allowed);
    const auto request = planManualOpen(path("C:\\Media\\snes\\manuals\\Amber Lantern (USA).pdf"), machine());
    REQUIRE(request.has_value());
    const rv::launcher::GuardVerdict verdict = rv::launcher::LaunchGuard::fromEnvironment().check(*request);
    CHECK(verdict.rule == rv::launcher::GuardRule::NotListed);
    REQUIRE_FALSE(verdict.allowed()); // never past this line with a guard that would let explorer.exe start
    const rv::Status checked = rv::launcher::checkLaunchGuard(*request);
    REQUIRE_FALSE(checked.has_value());
    CHECK(rv::launcher::isGuardRefusal(checked.error()));
    // The runner refuses it before any process exists (the guard is its first check).
    rv::platform_win::Win32ProcessRunner runner;
    const auto started = runner.start(*request, {});
    REQUIRE_FALSE(started.has_value());
    CHECK(rv::launcher::isGuardRefusal(started.error()));
    CHECK(runner.lastStart().processId == 0);
}
