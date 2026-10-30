#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/launcher/exit_status.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/launcher/process_runner.hpp>

namespace rv::launcher {

/// The life of one launch (specification §11.4; the application is in ExternalEmulatorRunning, §15.5, from Starting
/// until a final state):
///
///   Idle -> Validating -> Starting -> Running <-> StillRunning
///              |             |          |  \-> AwaitingConfirmation (UserConfirmReturn: the process exited)
///              v             v          v                  |
///        FailedToStart  FailedToStart  Returned | Crashed | FailedToStart   <-  confirmReturn()
enum class SessionState : std::uint8_t {
    Idle,                 ///< created, nothing done yet
    Validating,           ///< the request is being checked (passed through within start())
    Starting,             ///< the process is being created (passed through within start())
    Running,              ///< the emulator runs and Retroverse waits for it
    StillRunning,         ///< a timeout passed while it runs (no window for LaunchTimeouts::windowTimeout): the user
                          ///< can be offered the return; nothing is terminated, and a window shown again resumes
                          ///< Running
    AwaitingConfirmation, ///< UserConfirmReturn: the started process exited cleanly; the user's confirmation ends it
    Returned,             ///< final: the emulator is gone (or the user confirmed the return)
    FailedToStart,        ///< final: see FailureReason
    Crashed,              ///< final: the authoritative process ended with an unhandled exception
};

[[nodiscard]] std::string_view toString(SessionState state) noexcept;
/// Returned, FailedToStart and Crashed.
[[nodiscard]] bool isFinal(SessionState state) noexcept;

/// Why a session ended in FailedToStart.
enum class FailureReason : std::uint8_t {
    None,
    InvalidRequest, ///< validateLaunchRequest() refused the request: nothing was started
    StartFailed,    ///< the process could not be created (the runner's error says why; the automation guard's refusal,
                    ///< launch_guard.hpp, is one)
    LoaderError,    ///< it could not be loaded (a missing DLL, a bad image): ExitKind::LoaderError
    EarlyExit,      ///< it ended unclean within LaunchTimeouts::startupGrace
};

[[nodiscard]] std::string_view toString(FailureReason reason) noexcept;

/// How a session ended. Valid once the session is in a final state.
struct LaunchOutcome {
    SessionState state = SessionState::Idle;
    FailureReason failure = FailureReason::None;
    /// Exit code of the authoritative process: the started one (DirectProcess, UserConfirmReturn), the tree's process
    /// that crashed, or else the last one to exit (ProcessTree). Absent when nothing exited (the user confirmed the
    /// return while the emulator ran, or nothing started).
    std::optional<std::uint32_t> exitCode;
    std::uint32_t exitProcessId = 0;
    ExitKind exitKind = ExitKind::Success;
    /// One sentence for the user and the log.
    std::string message;
    /// A clean exit of the started process within the startup grace while its tree was not waited for (DirectProcess,
    /// UserConfirmReturn): a launcher stub, a wrapper waited for as a direct process, or an emulator that gave up at
    /// once without an error code (a missing BIOS shown in its own dialog). Not a failure to start, since the emulator
    /// may still run: the product tells the user that the game ended right away (ADR-0006). Never set for ProcessTree,
    /// whose end is the whole tree's.
    bool quickExit = false;
    /// Ended by confirmReturn().
    bool confirmedByUser = false;
    /// Processes of the tree were still running when the session ended (left running: never terminated).
    bool processesRemained = false;
    /// The session was StillRunning at some point.
    bool wasStillRunning = false;
    /// Processes seen in the tree, the started one included (1 when the tree is not followed).
    std::size_t processesSeen = 0;
    /// StartFailed because the runner reported the automation guard's refusal (isGuardRefusal(): RV_LAUNCH_ALLOWLIST
    /// is set and does not allow the request); nothing was created.
    bool refusedByGuard = false;
};

/// Time points of a session (LaunchClock), present once reached.
struct SessionTimes {
    std::optional<LaunchClock::time_point> begun;            ///< start() was called
    std::optional<LaunchClock::time_point> started;          ///< the process started running (IRunningProcess::
                                                             ///< startedAt: after the window's hand-over); the
                                                             ///< startup grace and the durations count from it
    std::optional<LaunchClock::time_point> firstWindow;      ///< the tree first showed a window
    std::optional<LaunchClock::time_point> mainExited;       ///< the started process exited (its exit time)
    std::optional<LaunchClock::time_point> exited;           ///< the authoritative exit (its exit time)
    std::optional<LaunchClock::time_point> exitObserved;     ///< the runner noticed that exit
    std::optional<LaunchClock::time_point> stillRunningSince; ///< the last switch to StillRunning
    std::optional<LaunchClock::time_point> ended;            ///< the final state was reached
};

/// A process of the tree other than the started one, known to run (its exit has not been observed).
struct TreeProcess {
    std::uint32_t processId = 0;
    /// The executable's file name; empty when the runner could not verify that the process is in the tree.
    std::string image;
    /// When the runner noticed it (the ChildStarted event's observedAt).
    LaunchClock::time_point seenAt{};
};

/// One launch, from validation to the emulator's end, driven by the observations of an IProcessRunner. Main thread
/// only; never terminates a process. Typical use: start(), then poll() every frame (or whenever the runner's wake
/// callback fires) until finished(); confirmReturn() when the user says they are back. Once the session is final it
/// stops following the processes (IRunningProcess::stopWatching): whatever still runs is left alone.
///
/// The authoritative end depends on the effective strategy (effectiveStrategy(): ProcessTree without a followed tree
/// becomes UserConfirmReturn):
///  - DirectProcess: the started process's exit;
///  - ProcessTree: the tree's end (TreeEmpty); a crash of any of its processes makes the session Crashed;
///  - UserConfirmReturn: confirmReturn(); a crash, a loader error or an unclean exit within the startup grace of the
///    started process still end the session at once.
/// Classification of the authoritative exit (classifyExitCode), times counted from the moment the process started
/// running (SessionTimes::started): a crash -> Crashed; a loader error -> FailedToStart; an unclean exit within
/// LaunchTimeouts::startupGrace -> FailedToStart (EarlyExit); anything else -> Returned. A clean exit within the grace
/// is Returned too, not a failure to start (deliberately, see LaunchOutcome::quickExit): with quickExit set unless the
/// whole tree was waited for (ProcessTree).
class LaunchSession {
public:
    LaunchSession(LaunchRequest request, IProcessRunner& runner);
    LaunchSession(const LaunchSession&) = delete;
    LaunchSession& operator=(const LaunchSession&) = delete;
    LaunchSession(LaunchSession&&) noexcept;
    LaunchSession& operator=(LaunchSession&&) noexcept;
    /// Stops following the process (it keeps running).
    ~LaunchSession();

    /// Idle -> Validating -> Starting -> Running, or FailedToStart (InvalidRequest, StartFailed; also when the runner
    /// throws). `callbacks` go to the runner (IProcessRunner::start); beforeResume is not called when nothing is
    /// started. `now` is SessionTimes::begun; SessionTimes::started is the runner's IRunningProcess::startedAt().
    /// Ignored unless Idle.
    void start(LaunchClock::time_point now, StartCallbacks callbacks = {});
    /// Applies the events the runner observed since the last call, then the timeouts at `now`.
    void poll(LaunchClock::time_point now);
    /// Applies one observation (poll() applies the runner's; tests feed their own). Ignored once finished.
    void apply(const ProcessEvent& event);
    /// Running -> StillRunning when no window of the tree has been visible for LaunchTimeouts::windowTimeout.
    void checkTimeouts(LaunchClock::time_point now);
    /// The user is back: ends a session that is Running, StillRunning or AwaitingConfirmation (Returned, with
    /// processesRemained when the emulator still runs; the exit recorded earlier classifies it otherwise). Nothing is
    /// terminated. Ignored in other states.
    void confirmReturn(LaunchClock::time_point now);

    [[nodiscard]] SessionState state() const noexcept { return m_state; }
    [[nodiscard]] bool finished() const noexcept { return isFinal(m_state); }
    /// Meaningful once finished().
    [[nodiscard]] const LaunchOutcome& outcome() const noexcept { return m_outcome; }
    [[nodiscard]] const SessionTimes& times() const noexcept { return m_times; }
    [[nodiscard]] const LaunchRequest& request() const noexcept { return m_request; }
    /// The strategy in effect: the request's until the process runs, then effectiveStrategy() of it.
    [[nodiscard]] WaitStrategy strategy() const noexcept { return m_strategy; }
    /// The command line passed to the process (UTF-8); empty before Starting.
    [[nodiscard]] const std::string& commandLine() const noexcept { return m_commandLine; }
    /// The running process, or nullptr before it started or when it could not.
    [[nodiscard]] const IRunningProcess* process() const noexcept { return m_process.get(); }
    [[nodiscard]] std::uint32_t processId() const noexcept;
    /// The emulator's window while one shows (ProcessEvent::window, I10 step 6), else 0.
    [[nodiscard]] std::uintptr_t window() const noexcept { return m_window; }
    /// Processes of the tree known to run (the started one included until it exits).
    [[nodiscard]] std::size_t liveProcessCount() const noexcept;
    /// The processes of the tree other than the started one known to run, in the order they were noticed (cleared
    /// when the tree is empty). Not updated once the session is final.
    [[nodiscard]] const std::vector<TreeProcess>& liveChildren() const noexcept { return m_liveChildren; }
    /// Every event applied, oldest first (the first kMaxHistory).
    [[nodiscard]] const std::vector<ProcessEvent>& history() const noexcept { return m_history; }

    static constexpr std::size_t kMaxHistory = 256;

private:
    struct Exit {
        std::uint32_t processId = 0;
        std::uint32_t code = 0;
        LaunchClock::time_point occurredAt{};
        LaunchClock::time_point observedAt{};
    };

    void setState(SessionState state, std::string_view why);
    void fail(FailureReason reason, std::string message, LaunchClock::time_point now);
    /// Ends the session with the authoritative exit `exit`.
    void conclude(const Exit& exit);
    void onMainExited(const ProcessEvent& event);
    void onChildExited(const ProcessEvent& event);
    void onTreeEmpty(const ProcessEvent& event);
    [[nodiscard]] bool withinStartupGrace(LaunchClock::time_point at) const noexcept;
    [[nodiscard]] std::string describeLaunch() const;

    LaunchRequest m_request;
    IProcessRunner* m_runner = nullptr;
    std::unique_ptr<IRunningProcess> m_process;
    SessionState m_state = SessionState::Idle;
    WaitStrategy m_strategy = WaitStrategy::DirectProcess;
    LaunchOutcome m_outcome;
    SessionTimes m_times;
    std::string m_commandLine;
    std::vector<ProcessEvent> m_pending; ///< scratch for poll(), reused
    std::vector<ProcessEvent> m_history;

    std::optional<Exit> m_mainExit;
    std::optional<Exit> m_lastExit;  ///< the latest exit in the tree (ProcessTree)
    std::optional<Exit> m_treeCrash; ///< the first crash in the tree (ProcessTree)
    std::vector<TreeProcess> m_liveChildren;
    bool m_treeEmpty = false;
    bool m_windowVisible = false;
    std::uintptr_t m_window = 0;
    LaunchClock::time_point m_windowClockStart{};
};

} // namespace rv::launcher
