#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/launcher/environment.hpp>
#include <rv/launcher/process_runner.hpp>

namespace rv::platform_win {

/// The environment variable that starts every program in the background (automated runs, never a user's): "1" in the
/// test harnesses (the unit and gpu test presets, rv_tests and rv_smoke_tests themselves), so that no program a test
/// launches takes the foreground or shows a console window while someone works at the machine. Unset, empty or "0"
/// (the desktop tests set it so), programs start as the product starts them (ADR-0006).
inline constexpr std::string_view kLaunchBackgroundVariable = "RV_LAUNCH_BACKGROUND";

/// Whether RV_LAUNCH_BACKGROUND asks for background launches: set, and neither empty nor "0".
[[nodiscard]] bool backgroundLaunchesRequested() noexcept;

/// Turns off, for the rest of this process's life, the modal dialogs that a failure would show over the user's work
/// (automated runs: the test executables, the fake emulator, and the product's programs in the background or
/// frame-limited): the error mode gets SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX and SEM_NOOPENFILEERRORBOX (no
/// loader or crash dialog; programs started without CREATE_DEFAULT_ERROR_MODE inherit it); the C runtime's reports go
/// to standard error and the debugger instead of a message box (_CrtSetReportMode for _CRT_ASSERT and _CRT_ERROR in
/// Debug builds: failed _ASSERTE, debug iterators, invalid parameters; _set_error_mode(_OUT_TO_STDERR): assert() and
/// runtime error messages of GUI programs); abort() shows no message box and calls no fault report (it ends the
/// process with exit code 3). Idempotent.
void disableFailureDialogs() noexcept;

/// Options of the Windows process runner beyond the launch request.
struct Win32ProcessRunnerOptions {
    /// How often the watcher thread checks the tree when no notification arrives: the job's process count (the
    /// system does not guarantee the delivery of job notifications) and the tree's visible windows (WindowShown,
    /// WindowsGone). It never polls the started process's exit, which wakes it at once.
    std::chrono::milliseconds pollInterval{100};
    /// Calls AllowSetForegroundWindow for the started process before it runs, so that its window may take the
    /// foreground even when it appears late. Effective only while Retroverse is the foreground process.
    bool allowSetForeground = true;
    /// Starts every program in the background: a console program without a console window (CREATE_NO_WINDOW, as for
    /// LaunchRequest::hideConsoleWindow), a GUI program told to show its first window inactive and off every
    /// monitor (STARTF_USESHOWWINDOW with SW_SHOWNOACTIVATE and STARTF_USEPOSITION left of the virtual screen, which
    /// a program that lets Windows place and show its window follows: a visible window for the window detection, no
    /// minimized window in the taskbar), no AllowSetForegroundWindow, and an error mode without dialogs
    /// (disableFailureDialogs() in this process, once, whose error mode the program inherits instead of the default
    /// one: a missing DLL or a crash ends it with its status, no modal dialog appears).
    /// nullopt: as RV_LAUNCH_BACKGROUND says when start() is called.
    std::optional<bool> background;
};

/// What happened while the last process was started (logs, the launch spike's report).
struct Win32StartInfo {
    std::uint32_t processId = 0;
    /// The process was assigned to a job object: its tree is followed (ProcessTree waiting works).
    bool jobAssigned = false;
    /// Why the tree is not followed, when it is not.
    std::string jobError;
    /// AllowSetForegroundWindow succeeded (Retroverse was in the foreground when it launched).
    bool foregroundAllowed = false;
    /// Why it did not, when it did not.
    std::string foregroundError;
    /// Started in the background (Win32ProcessRunnerOptions::background, RV_LAUNCH_BACKGROUND).
    bool background = false;
    /// The command line passed to CreateProcessW (UTF-8).
    std::string commandLine;
    /// From the call to CreateProcessW to the resumed main thread.
    std::chrono::microseconds startDuration{0};
};

/// The Windows process runner (specification §11.4; no shell, no command interpreter):
///  - the automation guard first (launcher::checkLaunchGuard(): while RV_LAUNCH_ALLOWLIST is set, a request it refuses
///    is an error and nothing is created);
///  - CreateProcessW with lpApplicationName = the executable and the command line of launcher::commandLineOf()
///    (arguments quoted as the inverse of CommandLineToArgvW), the request's working directory and, when it has
///    environment overrides, a Unicode environment block (launcher::mergeEnvironment of Retroverse's own);
///  - flags (processCreationFlags()) CREATE_SUSPENDED (nothing runs before the process is in its job),
///    CREATE_UNICODE_ENVIRONMENT, CREATE_DEFAULT_ERROR_MODE (the program gets the default error mode, not
///    Retroverse's; not for background launches), CREATE_NEW_PROCESS_GROUP, and a console of its own for a console
///    program: CREATE_NEW_CONSOLE, or CREATE_NO_WINDOW for LaunchRequest::hideConsoleWindow and for background launches
///    (never Retroverse's console, whose closing or Ctrl+Break would end it; ignored for GUI programs);
///  - for background launches only (Win32ProcessRunnerOptions::background, RV_LAUNCH_BACKGROUND: automated runs),
///    STARTF_USESHOWWINDOW with SW_SHOWNOACTIVATE and STARTF_USEPOSITION off the virtual screen (a GUI program that lets
///    Windows place and show its first window shows it inactive and off every monitor) and this process's error mode
///    without dialogs (disableFailureDialogs()), inherited;
///  - standard output and error appended to LaunchRequest::outputLog when set, standard input from NUL; only those two
///    handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), nothing else of Retroverse's;
///  - the suspended process is assigned to a job object WITHOUT JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (closing Retroverse,
///    or destroying the running process object, never ends the emulator) and with JOB_OBJECT_LIMIT_BREAKAWAY_OK (its
///    children may leave the job when they ask to), whose I/O completion port reports the processes of the tree
///    (JOB_OBJECT_MSG_NEW_PROCESS, _EXIT_PROCESS, _ABNORMAL_EXIT_PROCESS, _ACTIVE_PROCESS_ZERO). When the job cannot
///    be set up (nested jobs refused, ...) the process runs without one: followsTree() is false and the session falls
///    back to the user's confirmation for ProcessTree (launcher::effectiveStrategy);
///  - AllowSetForegroundWindow for the process (Win32ProcessRunnerOptions::allowSetForeground, never in the
///    background), the caller's
///    StartCallbacks::beforeResume, then ResumeThread (IRunningProcess::startedAt). Until then any failure, an
///    exception from beforeResume included, ends the suspended process, which never ran; start() never throws.
/// A request whose wait is FollowWindow (I11 step 13, ADR-0020 §4) is also followed through the windows of processes
/// outside its tree: a visible window that could be the game's, of a process created since the launch and started
/// (directly or through processes created since) by the tree, by Retroverse or by one of the request's launcher
/// processes, and that is none of them nor Explorer or a console; such a process is reported as ChildStarted and
/// ChildExited, and TreeEmpty comes once a window has shown and every process followed has exited. A protocol
/// activation (LaunchRequest::activationUrl) runs through ShellExecuteExW ("open", no user interface, synchronous),
/// starts no process of its own (processId() 0) and is followed so. While a window followed holds the foreground and the
/// shell reports exclusive fullscreen (SHQueryUserNotificationState), ExclusiveFullscreen is reported once.
/// A watcher thread per process blocks on the process handle and the completion port (no busy loop) and queues
/// launcher::ProcessEvent values for the main thread (IRunningProcess::takeEvents), calling the wake callback once per
/// event, until IRunningProcess::stopWatching() or the destruction of the running process object. Exit times are the
/// system's (GetProcessTimes), converted to launcher::LaunchClock. A program that ran is never terminated.
class Win32ProcessRunner final : public launcher::IProcessRunner {
public:
    explicit Win32ProcessRunner(Win32ProcessRunnerOptions options = {}) noexcept : m_options(options) {}

    [[nodiscard]] Expected<std::unique_ptr<launcher::IRunningProcess>>
    start(const launcher::LaunchRequest& request, launcher::StartCallbacks callbacks) override;

    /// Details of the last successful start().
    [[nodiscard]] const Win32StartInfo& lastStart() const noexcept { return m_lastStart; }

private:
    /// start() without its exception barrier.
    [[nodiscard]] Expected<std::unique_ptr<launcher::IRunningProcess>>
    startProcess(const launcher::LaunchRequest& request, launcher::StartCallbacks callbacks);
    /// A protocol activation (I11 step 13): beforeResume(0), ShellExecuteExW, then the game's window followed.
    [[nodiscard]] Expected<std::unique_ptr<launcher::IRunningProcess>>
    startActivation(const launcher::LaunchRequest& request, launcher::StartCallbacks callbacks);

    Win32ProcessRunnerOptions m_options;
    Win32StartInfo m_lastStart;
};

/// The CreateProcessW creation flags of a request, started in the background or not (see Win32ProcessRunner).
[[nodiscard]] std::uint32_t processCreationFlags(const launcher::LaunchRequest& request,
                                                 bool background = false) noexcept;

/// A process as the owner of a play session knows it (ADR-0008 §9): its id and its creation time (GetProcessTimes, in
/// FILETIME ticks), which tells it from a later process that reuses the id.
struct ProcessIdentity {
    std::uint32_t processId = 0;
    std::uint64_t creationTime = 0;

    auto operator<=>(const ProcessIdentity&) const = default;
};

/// This process's identity.
[[nodiscard]] ProcessIdentity currentProcessIdentity() noexcept;

/// What processLiveness() found.
enum class ProcessLiveness : std::uint8_t {
    Running, ///< the process runs: its id is in use by a process created at its creation time, which has not exited
    Gone,    ///< it does not: no process has the id, the one that has it was created at another time, or it exited
    Unknown, ///< a process has the id but neither it nor the system's process list can be asked: taken as running
};

/// "running", "gone", "unknown".
[[nodiscard]] std::string_view toString(ProcessLiveness liveness) noexcept;

/// A process as the system's process list shows it.
struct ListedProcess {
    /// FILETIME ticks, as GetProcessTimes reads them.
    std::uint64_t creationTime = 0;
    /// 0 for a process that exited while handles to it are still open.
    std::uint32_t threadCount = 0;

    bool operator==(const ListedProcess&) const = default;
};

/// The process that has `processId` now, as the system's process list shows it (NtQuerySystemInformation with
/// SystemProcessInformation, which needs no access right to the process): what processLiveness() reads of a process
/// this user may not open. nullopt when no process has the id. Errors: PlatformError when the list cannot be read.
[[nodiscard]] Expected<std::optional<ListedProcess>> listedProcess(std::uint32_t processId);

/// Whether `process` still runs (OpenProcess with PROCESS_QUERY_LIMITED_INFORMATION, GetProcessTimes,
/// GetExitCodeProcess). A process with the id that this user may not open (access denied: a service, another user's
/// process) is compared through listedProcess() instead: another creation time is gone, the same one running. A
/// process whose exit code is STILL_ACTIVE (259) reads as running even after it exited with that code, which no
/// emulator does on purpose.
[[nodiscard]] ProcessLiveness processLiveness(const ProcessIdentity& process) noexcept;

/// The environment of the current process as UTF-8 variables (GetEnvironmentStringsW; per-drive entries such as "=C:"
/// included). Unpaired UTF-16 surrogates become U+FFFD.
[[nodiscard]] std::vector<launcher::EnvironmentVariable> currentEnvironment();

} // namespace rv::platform_win
