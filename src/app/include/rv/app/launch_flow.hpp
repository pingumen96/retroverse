#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/launch_outcome.hpp>
#include <rv/app/run_report.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/launcher/launch_session.hpp>
#include <rv/launcher/process_runner.hpp>

namespace rv::app {

/// What the game-session panel shows over the quick catalog (assets/ui/game_session.rml).
enum class SessionPanel : std::uint8_t {
    None,
    /// "Gioco in corso", without choices: the game has the screen. The frame that asked for the launch shows it, the last
    /// one presented before the emulator starts, and so does the frame after a choice to go back to the game: the
    /// window kept behind the emulator shows that frame while it runs (ADR-0006: the keep policy renders nothing).
    Launching,
    /// The user came back to Retroverse while the game runs (the window kept the focus for kComeBackDelay): return,
    /// leaving the game running, or go back to it.
    Playing,
    /// No window of the game for its window timeout (launcher::SessionState::StillRunning): return, leaving it
    /// running, or keep waiting.
    StillRunning,
    /// UserConfirmReturn: the started program ended (launcher::SessionState::AwaitingConfirmation); confirm the return,
    /// or go back to the game.
    Confirm,
    /// After the session, or a launch that could not start: the outcome's message (showsMessage()).
    Outcome,
};

/// "none", "launching", "playing", "still-running", "confirm", "outcome": the document's phases.
[[nodiscard]] std::string_view toString(SessionPanel panel) noexcept;

/// The game a launch is for, for the logs, the panel and the report.
struct LaunchInfo {
    std::string gameId;
    std::string title;
    /// The emulator profile's id.
    std::string profile;
};

/// A launch from the quick catalog, from Avvia to the return (specification §11.4, §15.5 ExternalEmulatorRunning;
/// ADR-0006): the launcher::LaunchSession and what the application does around it. GPU-free and window-free: the
/// application turns hostIdle() into the host's idle mode (AppContext::setIdle), shows panel(), hands its window over
/// through the StartCallbacks it gives start() and takes it back when takeReturned() says so.
///
///  1. request(): Avvia with a ready request. The panel shows Launching on the frame that asked; hostIdle() is true, so
///     the host presents that frame and then waits instead of rendering the next one.
///  2. start(), on the host's first wake: the session starts (the window handed over before the process runs). From
///     here no frame is rendered while the emulator runs: the host waits for events (the runner's wake callback among
///     them) and the application calls poll() on every wake.
///  3. poll(): the session's end, or a reason to ask the user, which stops the wait so that the panel can render:
///     StillRunning (no window for the window timeout), AwaitingConfirmation (UserConfirmReturn), the user back in
///     Retroverse (focusChanged(): the window kept the focus for kComeBackDelay). A question renders while the window
///     has the focus; without it, its frame is presented once (the window keeps it behind the game) and the host waits
///     again until the focus comes back (ADR-0006: no GPU work while the emulator runs). The panel's choices: return
///     (returnToRetroverse(): LaunchSession::confirmReturn, the emulator left running when it runs) or back to the game
///     (backToGame(): a frame with the notice again, then the host waits; with the focus, the application hands the
///     foreground to the game, and the question comes back when the user presses something or the game never takes the
///     focus). A window the game shows again ends StillRunning's question the same way.
///  4. The end: the outcome (messageForOutcome()), the Outcome panel when it has something to say (showsMessage()),
///     takeReturned() once, and whether the window may take the foreground back (returnTakesForeground()).
/// refuse() records a launch that cannot start (a plan's blocker, an unknown profile, no launch target) and shows its
/// message at once; quit() ends a running session as left running when Retroverse quits, and so does poll() when the
/// session outlives the wait limit (setWaitLimit(): frame-limited runs): nothing is ever terminated. Every launch is
/// recorded (records(): the run report's launches). One launch at a time. Main thread only.
///
/// The focus (focusChanged()) is Retroverse's window's input focus; a hidden window counts as focused (the application
/// says so: automation, the input script, stands for the user in front of it).
class LaunchFlow {
public:
    /// How long Retroverse's window must keep the focus while a game runs before the panel asks the user: the
    /// emulator's window closing hands the focus back 0.2 to 0.35 s before its process ends (ADR-0006), which must not
    /// flash the panel at every return.
    static constexpr std::chrono::milliseconds kComeBackDelay{1500};

    explicit LaunchFlow(launcher::IProcessRunner& runner) noexcept : m_runner(runner) {}
    LaunchFlow(const LaunchFlow&) = delete;
    LaunchFlow& operator=(const LaunchFlow&) = delete;

    /// Avvia with a ready request (launcher::planLaunch accepted it) on frame `frame`: it starts at the host's next wake
    /// (start()). False, and nothing changes, while another launch is pending or running.
    bool request(launcher::LaunchRequest request, LaunchInfo info, std::uint64_t frame);
    /// Avvia for a game that cannot launch: the Outcome panel shows `message` at once, the launch is recorded as
    /// "Blocked". False, and nothing changes, while another launch is pending or running.
    bool refuse(LaunchMessage message, LaunchInfo info, std::uint64_t frame);

    /// Starts the pending request (after the frame that asked for it was presented: `frame` is the next frame's index)
    /// with `callbacks` (the host's wake, the window's hand-over before the process runs). A request that cannot start
    /// ends at once with its outcome. Nothing without a pending request.
    void start(launcher::LaunchClock::time_point now, launcher::StartCallbacks callbacks, std::uint64_t frame);
    /// Follows the session (LaunchSession::poll) and decides the panel (see the class comment); ends a session that
    /// outlived the wait limit as left running. Call on every wake of the host's wait and once per rendered frame while
    /// sessionActive(); harmless otherwise.
    void poll(launcher::LaunchClock::time_point now, std::uint64_t frame);
    /// Retroverse's window gained or lost the input focus (kept whatever the session's state; false until the first
    /// call). A gain while the session runs starts the come-back delay; a loss cancels it, and takes back the Playing
    /// question (the user went back to the game without answering). The questions of the session itself
    /// (StillRunning, Confirm) stay until answered: they render again once the focus is back.
    void focusChanged(bool focused, launcher::LaunchClock::time_point now) noexcept;
    /// A press (a key, a button, a click) reached the focused window while the game had the screen, after the user
    /// chose to go back to the game or keep waiting without leaving Retroverse (backToGame()): the user is still in
    /// front of it, so the question comes back at once. The host drops the press itself. True when a question shows.
    bool userPressed() noexcept;

    /// The panel's "return to Retroverse" (Playing, StillRunning) and "I am back" (Confirm): the session ends
    /// (LaunchSession::confirmReturn), the emulator left running when it still runs.
    void returnToRetroverse(launcher::LaunchClock::time_point now, std::uint64_t frame);
    /// The panel's "back to the game" (Playing, Confirm) and "keep waiting" (StillRunning): one frame with the "Gioco in
    /// corso" notice, then the host waits again. The panel asks again when the session's state changes, when the user
    /// comes back once more (a new focus gain, then kComeBackDelay) or, while the window keeps the focus, at the next
    /// press (userPressed()), and for Playing and Confirm after kComeBackDelay too (the game never took the focus).
    /// True when the application must hand the foreground to the game: the window has the focus and the question was
    /// Playing or Confirm (StillRunning's game shows no window to go back to).
    bool backToGame(launcher::LaunchClock::time_point now) noexcept;
    /// The Outcome panel dismissed.
    void dismissOutcome() noexcept;
    /// The panel's Back: backToGame() while a session runs (its result), the outcome dismissed after it (false);
    /// nothing on Launching.
    bool panelBack(launcher::LaunchClock::time_point now) noexcept;

    /// Frame-limited runs: a session still running `limit` after its start ends as left running (LaunchSession::
    /// confirmReturn, never a kill; launch_outcome::kReasonWaitLimit when processes remained, LaunchRecord::
    /// waitLimitReached), so that the run reaches its frame limit and writes its report. None by default.
    void setWaitLimit(std::optional<std::chrono::milliseconds> limit) noexcept { m_waitLimit = limit; }

    /// Retroverse quits (its window closed, a quit request) during a launch: a running session ends as left running
    /// (LaunchSession::confirmReturn, never a kill), recorded with quitDuringGame and launch_outcome::kReasonQuit; a
    /// pending launch never starts (recorded as "Pending").
    void quit(launcher::LaunchClock::time_point now, std::uint64_t frame);

    /// A frame was rendered: counted for the running session, as a frame of a question (LaunchRecord::framesWithPanel;
    /// the question was presented) or of a game that has the screen (framesWhileRunning: only the one after a choice to
    /// go back to the game).
    void frameRendered() noexcept;
    /// The host woke from its wait while the session ran (LaunchRecord::idleWakes).
    void idleWake() noexcept;
    /// Retroverse's window was back in the foreground at `at` after the last session (LaunchRecord::foregroundMs).
    void noteForeground(launcher::LaunchClock::time_point at);

    /// A request waits for start().
    [[nodiscard]] bool pending() const noexcept { return m_pending.has_value(); }
    /// A session exists and has not ended.
    [[nodiscard]] bool sessionActive() const noexcept;
    /// The host waits instead of rendering: a launch is pending (start() at its next wake), or a session runs with
    /// nothing to ask (the panel shows the "Gioco in corso" notice, or nothing), or its question was presented while the
    /// window does not have the focus (it renders again once the focus is back).
    [[nodiscard]] bool hostIdle() const noexcept;
    [[nodiscard]] SessionPanel panel() const noexcept { return m_panel; }
    /// The game of the current or last launch.
    [[nodiscard]] const LaunchInfo& info() const noexcept { return m_info; }
    /// The last launch's message, once it ended (or was refused).
    [[nodiscard]] const LaunchMessage& message() const noexcept { return m_message; }
    /// True once after a session ended (the application takes its window back: WindowHandover::restore(), when
    /// returnTakesForeground()).
    [[nodiscard]] bool takeReturned() noexcept;
    /// After the last session: Retroverse may take the foreground back. False when processes of the game's tree still
    /// ran and the user did not say they were back (kProcessesRemained, a wait limit, quitting): restoring and raising
    /// the window would cover a game that may still run.
    [[nodiscard]] bool returnTakesForeground() const noexcept { return m_takeForeground; }
    /// The current or last session; null before the first start().
    [[nodiscard]] const launcher::LaunchSession* session() const noexcept { return m_session ? &*m_session : nullptr; }
    /// Every launch of the run, the current one last.
    [[nodiscard]] const std::vector<LaunchRecord>& records() const noexcept { return m_records; }

private:
    /// Shows `panel`, recording it when it differs from the one shown.
    void show(SessionPanel panel);
    /// The session reached its final state: the outcome, the panel, the record.
    void finish(launcher::LaunchClock::time_point now, std::uint64_t frame);
    [[nodiscard]] LaunchRecord& current() { return m_records.back(); }

    launcher::IProcessRunner& m_runner;
    std::optional<launcher::LaunchRequest> m_pending;
    std::optional<launcher::LaunchSession> m_session;
    SessionPanel m_panel = SessionPanel::None;
    /// The session's state at the last poll(): its transitions decide the panel.
    launcher::SessionState m_seenState = launcher::SessionState::Idle;
    /// Retroverse's window has the input focus (focusChanged()).
    bool m_focused = false;
    /// Since when the come-back delay runs: the window gained the focus while the session runs, or kept it through a
    /// choice to go back to the game (backToGame()). Cleared when the focus leaves.
    std::optional<launcher::LaunchClock::time_point> m_focusSince;
    /// The user went back to the game (or kept waiting) without leaving Retroverse: a press asks again (userPressed()).
    bool m_askOnPress = false;
    /// The question shown was rendered at least once (the frame the window keeps when it does not have the focus).
    bool m_questionPresented = false;
    std::optional<std::chrono::milliseconds> m_waitLimit;
    /// The current session ended because it outlived m_waitLimit.
    bool m_waitLimitReached = false;
    bool m_quitting = false;
    bool m_returned = false;
    bool m_takeForeground = true;
    LaunchInfo m_info;
    LaunchMessage m_message;
    std::vector<LaunchRecord> m_records;
};

} // namespace rv::app
