// rv_platform_win's process identity (I2 step 12): a play session's owner is known by its process id and creation time,
// and a later start closes the session only when that process is gone: this process runs; the same id with another
// creation time (a later process that reuses it) is gone; a process that exited is gone, while a handle to it is still
// open and after; a process that runs is running.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/platform_win/win32_util.hpp>

#include "support/fixture.hpp"

using rv::platform_win::ProcessIdentity;
using rv::platform_win::ProcessLiveness;
using rv::platform_win::processLiveness;

namespace {

/// The fake emulator started directly (not through the runner), its handle kept.
struct StartedFake {
    rv::platform_win::UniqueHandle process;
    ProcessIdentity identity;
};

StartedFake startFake(const std::vector<std::string>& arguments) {
    const std::filesystem::path fake = rv::test::testProgramsDir() / "rv_fake_emulator.exe";
    REQUIRE(std::filesystem::exists(fake));
    auto commandLine = rv::launcher::buildCommandLine(rv::core::pathToUtf8(fake), arguments);
    REQUIRE(commandLine.has_value());
    std::wstring wide = rv::core::utf8ToWide(*commandLine).value();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    REQUIRE(CreateProcessW(fake.c_str(), wide.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           fake.parent_path().c_str(), &startup, &information) != FALSE);
    CloseHandle(information.hThread);
    StartedFake started;
    started.process.reset(information.hProcess);
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    REQUIRE(GetProcessTimes(information.hProcess, &creation, &exit, &kernel, &user) != FALSE);
    started.identity = ProcessIdentity{information.dwProcessId, rv::platform_win::ticks(creation)};
    return started;
}

} // namespace

TEST_CASE("platform_win/process: a session owner's identity and whether it still runs", "[platform_win][sessions]") {
    const ProcessIdentity self = rv::platform_win::currentProcessIdentity();
    CHECK(self.processId == GetCurrentProcessId());
    CHECK(self.creationTime > 0);
    CHECK(processLiveness(self) == ProcessLiveness::Running);
    // The same id with another creation time: a later process that reuses the id, not the owner.
    CHECK(processLiveness(ProcessIdentity{self.processId, self.creationTime + 1}) == ProcessLiveness::Gone);
    CHECK(rv::platform_win::toString(ProcessLiveness::Unknown) == "unknown");

    SECTION("a process that runs, then exits") {
        StartedFake fake = startFake({"--sleep-ms", "1500", "--exit-timeout-ms", "20000"});
        CHECK(processLiveness(fake.identity) == ProcessLiveness::Running);
        REQUIRE(WaitForSingleObject(fake.process.get(), 20000) == WAIT_OBJECT_0);
        // Exited, its process object kept by the handle still open: gone.
        CHECK(processLiveness(fake.identity) == ProcessLiveness::Gone);
        fake.process.reset();
        CHECK(processLiveness(fake.identity) == ProcessLiveness::Gone);
    }
}
