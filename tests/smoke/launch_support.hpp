#pragma once

// Helpers of the launch smoke tests (tests/smoke/test_launch.cpp, I2 step A1): the mock catalog of the fake emulator
// written into a case directory, the automation guard's allow-list, retroverse.exe started and left running while the
// test acts on it (its window closed from outside), the processes it launched watched by id. Windows-only; Catch2
// assertions: call them from a test case.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "smoke_support.hpp"

namespace rv::smoke {

/// rv_fake_emulator.exe of this build (next to retroverse.exe).
[[nodiscard]] std::filesystem::path fakeEmulatorExe();

/// The game ids of tests/smoke/catalogs/fake_emulator_games.json, one behavior each.
namespace fake_games {
inline constexpr std::string_view kReturns = "g000000000000a001";       ///< runs 2.5 s, exits 0
inline constexpr std::string_view kQuickExit = "g000000000000a002";     ///< exits 0 at once
inline constexpr std::string_view kErrorExit = "g000000000000a003";     ///< exits 3 at once
inline constexpr std::string_view kCrashes = "g000000000000a004";       ///< an access violation at once
inline constexpr std::string_view kHangs = "g000000000000a005";         ///< runs until its exit timeout (12 s)
inline constexpr std::string_view kConfirm = "g000000000000a006";       ///< UserConfirmReturn, exits 0 after 2.5 s
inline constexpr std::string_view kStillRunning = "g000000000000a007";  ///< no window for 1 s, runs 6 s
inline constexpr std::string_view kWindow = "g000000000000a008";        ///< a fullscreen window for 150 frames
inline constexpr std::string_view kUnsetVariable = "g000000000000a009"; ///< its profile names {var:core}
inline constexpr std::string_view kNotListed = "g000000000000a010";     ///< a copy of the fake the guard refuses
inline constexpr std::string_view kMissingEmulator = "g000000000000a011"; ///< its profile's program does not exist
inline constexpr std::string_view kRomMissing = "g000000000000a012";    ///< its ROM does not exist
inline constexpr std::string_view kNoLaunch = "g000000000000a013";      ///< no launch target
} // namespace fake_games

/// Writes <caseDir>/catalog.json: tests/smoke/catalogs/fake_emulator_games.json with the executables of this build (the
/// fake, its copy <caseDir>/copied_emulator.exe, a missing program), the games `gameIds` alone, in their order, and the
/// ROM files they name under <caseDir>/roms (but kRomMissing's). Returns the catalog file. REQUIREs that it could.
[[nodiscard]] std::filesystem::path writeFakeCatalog(const std::filesystem::path& caseDir,
                                                     const std::vector<std::string_view>& gameIds);

/// Makes sure the automation guard is on for the runs the test starts (inherited by them): RV_LAUNCH_ALLOWLIST as ctest
/// sets it (the gpu tests' ENVIRONMENT property), or the fake of this build when the test runs outside ctest.
void ensureLaunchAllowList();

/// The --input-script files of the launch runs: tests/input_scripts/<name>.
[[nodiscard]] std::filesystem::path inputScript(std::string_view name);

/// True while the process `processId` runs and its executable's file name is `image` (compared ignoring case): a
/// process the run launched, not another that reuses its id.
[[nodiscard]] bool processRunning(std::uint32_t processId, std::string_view image);
/// Waits up to `timeout` for the process `processId` to end; its exit code, or nothing when it still runs (or cannot be
/// opened: it ended long ago).
[[nodiscard]] std::optional<std::uint32_t> waitForExit(std::uint32_t processId, std::chrono::milliseconds timeout);

/// retroverse.exe started like runApp() (--hidden unless `visible`, --data-dir, --run-report) but left running: the
/// test acts on it, then waits for it. A run still alive when the object goes away is terminated (the test harness
/// does that; the product never ends what it launched).
class StartedApp {
public:
    StartedApp(const std::filesystem::path& caseDir, std::string_view name, std::vector<std::string> arguments,
               bool visible = false);
    ~StartedApp();
    StartedApp(const StartedApp&) = delete;
    StartedApp& operator=(const StartedApp&) = delete;

    [[nodiscard]] std::uint32_t processId() const noexcept { return m_processId; }
    /// Posts WM_CLOSE to its window (found by process and title): what closing the window does. False when it has no
    /// window (yet).
    bool closeWindow();
    /// Waits for it to end (at most kRunTimeout) and reads its report: the run, as runApp() returns it.
    [[nodiscard]] AppRun finish();

private:
    AppRun m_run;
    void* m_process = nullptr;
    std::uint32_t m_processId = 0;
    void* m_output = nullptr;
};

/// Waits up to `timeout` for `file` to exist; true when it does.
[[nodiscard]] bool waitForFile(const std::filesystem::path& file, std::chrono::milliseconds timeout);

} // namespace rv::smoke
