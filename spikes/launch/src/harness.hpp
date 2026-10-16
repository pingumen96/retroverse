#pragma once

// What spike_launch does as a test harness around the product code (rv_launcher, rv_platform): measuring the machine's
// load, ending a target that outlived the run's limit, closing the Notepad window it opened. The product never ends
// or closes an emulator: these helpers exist only in the spike. Windows-only.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace rv::spikes::launch::harness {

/// The share of time every processor was busy between start() and load() (GetSystemTimes), for the load column of
/// the measurements: other agents may be compiling in parallel.
class CpuLoadSampler {
public:
    void start() noexcept;
    /// 0..1; 0 when nothing can be measured.
    [[nodiscard]] double load() const noexcept;

private:
    std::uint64_t m_idle = 0;
    std::uint64_t m_kernel = 0;
    std::uint64_t m_user = 0;
};

/// Ends a process the spike started (TerminateProcess with `exitCode`) and waits for it: the harness's clean-up of a
/// target past the run's time limit, never something the product does. True when it is gone.
bool terminateProcess(std::uint32_t processId, std::uint32_t exitCode);

/// True when a process whose executable file name is `imageName` (case-insensitive, "notepad.exe") runs.
[[nodiscard]] bool isProcessRunning(std::string_view imageName);

/// Visible top-level windows of processes named `imageName` whose title contains `titlePart`.
[[nodiscard]] std::vector<void*> findWindows(std::string_view imageName, std::string_view titlePart);
/// Asks a window to close (WM_CLOSE: what its close button does).
void closeWindow(void* window);
[[nodiscard]] bool windowExists(void* window);

/// The system's notepad.exe.
[[nodiscard]] std::filesystem::path systemNotepad();

/// The displays as SDL reports them: name, bounds, current and desktop modes (for the report). SDL video must be
/// initialized.
[[nodiscard]] nlohmann::json describeDisplays();

/// How long after the user's last input Windows refuses the foreground to the other processes
/// (SPI_GETFOREGROUNDLOCKTIMEOUT, 200000 by default); 0 when it cannot be read.
[[nodiscard]] std::uint32_t foregroundLockTimeoutMs();
/// Milliseconds since the session's last keyboard or mouse input (GetLastInputInfo; gamepads do not count).
[[nodiscard]] std::uint32_t millisecondsSinceLastInput();

/// The executable file name of a process, or empty.
[[nodiscard]] std::string processImageName(std::uint32_t processId);

} // namespace rv::spikes::launch::harness
