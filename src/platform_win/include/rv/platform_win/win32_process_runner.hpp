#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/launcher/environment.hpp>
#include <rv/launcher/process_runner.hpp>

namespace rv::platform_win {

/// Options of the Windows process runner beyond the launch request.
struct Win32ProcessRunnerOptions {
    /// How often the watcher thread checks the tree when no notification arrives: the job's process count (the
    /// system does not guarantee the delivery of job notifications) and the tree's visible windows (WindowShown,
    /// WindowsGone). It never polls the started process's exit, which wakes it at once.
    std::chrono::milliseconds pollInterval{100};
    /// Calls AllowSetForegroundWindow for the started process before it runs, so that its window may take the
    /// foreground even when it appears late. Effective only while Retroverse is the foreground process.
    bool allowSetForeground = true;
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
///    Retroverse's), CREATE_NEW_PROCESS_GROUP, and a console of its own for a console program: CREATE_NEW_CONSOLE, or
///    CREATE_NO_WINDOW for LaunchRequest::hideConsoleWindow (never Retroverse's console, whose closing or Ctrl+Break
///    would end it; ignored for GUI programs);
///  - standard output and error appended to LaunchRequest::outputLog when set, standard input from NUL; only those two
///    handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), nothing else of Retroverse's;
///  - the suspended process is assigned to a job object WITHOUT JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (closing Retroverse,
///    or destroying the running process object, never ends the emulator) and with JOB_OBJECT_LIMIT_BREAKAWAY_OK (its
///    children may leave the job when they ask to), whose I/O completion port reports the processes of the tree
///    (JOB_OBJECT_MSG_NEW_PROCESS, _EXIT_PROCESS, _ABNORMAL_EXIT_PROCESS, _ACTIVE_PROCESS_ZERO). When the job cannot
///    be set up (nested jobs refused, ...) the process runs without one: followsTree() is false and the session falls
///    back to the user's confirmation for ProcessTree (launcher::effectiveStrategy);
///  - AllowSetForegroundWindow for the process (Win32ProcessRunnerOptions::allowSetForeground), the caller's
///    StartCallbacks::beforeResume, then ResumeThread (IRunningProcess::startedAt). Until then any failure, an
///    exception from beforeResume included, ends the suspended process, which never ran; start() never throws.
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

    Win32ProcessRunnerOptions m_options;
    Win32StartInfo m_lastStart;
};

/// The CreateProcessW creation flags of a request (see Win32ProcessRunner).
[[nodiscard]] std::uint32_t processCreationFlags(const launcher::LaunchRequest& request) noexcept;

/// The environment of the current process as UTF-8 variables (GetEnvironmentStringsW; per-drive entries such as "=C:"
/// included). Unpaired UTF-16 surrogates become U+FFFD.
[[nodiscard]] std::vector<launcher::EnvironmentVariable> currentEnvironment();

} // namespace rv::platform_win
