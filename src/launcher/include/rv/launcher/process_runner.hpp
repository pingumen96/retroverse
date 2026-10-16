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
#include <rv/launcher/launch_request.hpp>

namespace rv::launcher {

/// Clock of every launch time point: monotonic, so that durations survive changes of the wall clock.
using LaunchClock = std::chrono::steady_clock;

/// What a process runner observed about a started process and its tree.
enum class ProcessEventKind : std::uint8_t {
    MainExited,   ///< the started process exited: exitCode, occurredAt = its exit time
    ChildStarted, ///< another process joined the tree: processId, image
    ChildExited,  ///< a process of the tree other than the started one exited: processId, image, exitCode when known
    TreeEmpty,    ///< no process of the tree is left: occurredAt = the exit time of the last one (when known)
    WindowShown,  ///< a process of the tree shows a top-level window: the first one, or one again after none was shown
    WindowsGone,  ///< no process of the tree shows a window any more
    WatchFailed,  ///< the runner can no longer follow the processes: message; only the user can end the session now
};

[[nodiscard]] std::string_view toString(ProcessEventKind kind) noexcept;

/// One observation, in the order the runner made them.
struct ProcessEvent {
    ProcessEventKind kind = ProcessEventKind::MainExited;
    /// When the runner noticed it.
    LaunchClock::time_point observedAt{};
    /// When it happened: the system's exit time of the process for exits (converted to LaunchClock), observedAt for
    /// the other kinds and when the exit time is unknown.
    LaunchClock::time_point occurredAt{};
    std::uint32_t processId = 0;
    std::optional<std::uint32_t> exitCode;
    /// The executable's file name (ChildStarted, ChildExited), when it could be read.
    std::string image;
    /// WatchFailed: why.
    std::string message;
};

/// A started process and its tree, followed by a runner until stopWatching() or destruction. Implementations never
/// terminate the process: destroying this object stops following it, the program keeps running (specification §11.5).
class IRunningProcess {
public:
    virtual ~IRunningProcess();

    [[nodiscard]] virtual std::uint32_t processId() const noexcept = 0;
    /// When the process started running: its main thread was resumed, after StartCallbacks::beforeResume (the
    /// hand-over of Retroverse's window, which can take a few hundred milliseconds, comes before it).
    [[nodiscard]] virtual LaunchClock::time_point startedAt() const noexcept = 0;
    /// True when the whole tree is followed (ProcessTree waiting, ChildStarted/ChildExited/TreeEmpty events), false
    /// when only the started process is (the job object could not be set up).
    [[nodiscard]] virtual bool followsTree() const noexcept = 0;
    /// Appends the events observed since the last call to `out`, oldest first. Main thread; does not allocate when
    /// nothing happened and `out` has room.
    virtual void takeEvents(std::vector<ProcessEvent>& out) = 0;
    /// Stops following the processes, which keep running: nothing more is observed or queued, and the wake callback is
    /// no longer called. Returns at once (the runner's thread ends on its own); idempotent. LaunchSession calls it when
    /// the session reaches a final state.
    virtual void stopWatching() noexcept = 0;
};

/// What a caller hooks into IProcessRunner::start.
struct StartCallbacks {
    /// Called from the runner's own thread once per event queued (a main loop blocked waiting for input can be woken by
    /// it), until IRunningProcess::stopWatching(): thread-safe and quick. Optional.
    std::function<void()> wake;
    /// Called on the calling thread once the process exists, suspended (in its job, allowed to take the foreground),
    /// before its first instruction runs: the moment to hand the window over (a window minimized earlier would have
    /// cost Retroverse the foreground it passes on). An exception it throws makes start() fail and end the process,
    /// which never ran. Optional.
    std::function<void(std::uint32_t processId)> beforeResume;
};

/// Starts processes for launch sessions (the port of specification §15's IProcessRunner; the Windows implementation is
/// rv::platform::Win32ProcessRunner).
class IProcessRunner {
public:
    virtual ~IProcessRunner();

    /// Starts `request`, which validateLaunchRequest() accepted, calling `callbacks` as they document. Errors: the
    /// process could not be created or could not be followed (the message says why); a process that never ran is not
    /// left behind. Implementations report failures, an exception from a callback included, as errors and never throw.
    [[nodiscard]] virtual Expected<std::unique_ptr<IRunningProcess>> start(const LaunchRequest& request,
                                                                           StartCallbacks callbacks) = 0;
};

} // namespace rv::launcher
