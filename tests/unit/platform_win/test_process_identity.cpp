// rv_platform_win's process identity (I2 step 12): a play session's owner is known by its process id and creation time,
// and a later start closes the session only when that process is gone: this process runs; the same id with another
// creation time (a later process that reuses it) is gone; a process that exited is gone, while a handle to it is still
// open and after; a process that runs is running. A process this user may not open (a service, another user's) is
// compared through the system's process list: the list agrees with GetProcessTimes, and such a process that holds an
// owner's id with another creation time is gone, not running.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h.
#include <tlhelp32.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/platform_win/win32_util.hpp>

#include "support/fixture.hpp"

using rv::platform_win::listedProcess;
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

/// The ids of the processes that run now that this user may not open (services, other users' processes): none in an
/// elevated run.
std::vector<std::uint32_t> processesDenyingAccess() {
    std::vector<std::uint32_t> denied;
    const rv::platform_win::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    REQUIRE(snapshot.valid());
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != FALSE;
         more = Process32NextW(snapshot.get(), &entry)) {
        if (entry.th32ProcessID == 0) {
            continue;
        }
        const rv::platform_win::UniqueHandle process(
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process && GetLastError() == ERROR_ACCESS_DENIED) {
            denied.push_back(entry.th32ProcessID);
        }
    }
    return denied;
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

TEST_CASE("platform_win/process: a process this user may not open is compared through the system's process list",
          "[platform_win][sessions]") {
    // The list agrees with GetProcessTimes: this process, and a process that exited while its handle stays open (listed
    // without threads, or no longer listed).
    const ProcessIdentity self = rv::platform_win::currentProcessIdentity();
    const auto listedSelf = listedProcess(self.processId);
    REQUIRE(listedSelf.has_value());
    REQUIRE(listedSelf->has_value());
    CHECK((*listedSelf)->creationTime == self.creationTime);
    CHECK((*listedSelf)->threadCount >= 1);
    {
        StartedFake fake = startFake({"--exit-timeout-ms", "20000"});
        const auto running = listedProcess(fake.identity.processId);
        REQUIRE(running.has_value());
        if (running->has_value()) { // unless it already ended and its object went
            CHECK((*running)->creationTime == fake.identity.creationTime);
        }
        REQUIRE(WaitForSingleObject(fake.process.get(), 20000) == WAIT_OBJECT_0);
        const auto exited = listedProcess(fake.identity.processId);
        REQUIRE(exited.has_value());
        CHECK((!exited->has_value() || (*exited)->threadCount == 0));
    }
    // An id no process has (Windows hands out multiples of 4).
    const auto none = listedProcess(self.processId + 1);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->has_value());

    const std::vector<std::uint32_t> denied = processesDenyingAccess();
    if (denied.empty()) {
        SKIP("every running process can be opened by this user (an elevated run)");
    }
    bool compared = false;
    for (const std::uint32_t processId : denied) {
        const auto before = listedProcess(processId);
        REQUIRE(before.has_value());
        if (!before->has_value()) {
            continue; // ended since the snapshot
        }
        const std::uint64_t creationTime = (*before)->creationTime;
        // A dead owner whose id this process took later (a reboot, years ago): gone, although the id is in use.
        CHECK(processLiveness(ProcessIdentity{processId, creationTime + 1}) == ProcessLiveness::Gone);
        CHECK(processLiveness(ProcessIdentity{processId, 133'000'000'000'000'000ULL}) == ProcessLiveness::Gone);
        // The process itself, as an owner this user may not open (an elevated Retroverse): running, while it runs.
        const ProcessLiveness same = processLiveness(ProcessIdentity{processId, creationTime});
        const auto after = listedProcess(processId);
        REQUIRE(after.has_value());
        if (after->has_value() && (*after)->creationTime == creationTime) {
            CHECK(same == ProcessLiveness::Running);
            compared = true;
            break;
        }
    }
    CHECK(compared);
}
