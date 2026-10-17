#include <rv/app/launch_flow.hpp>

#include <utility>

#include <spdlog/spdlog.h>

namespace rv::app {
namespace {

namespace lo = launch_outcome;
using launcher::LaunchClock;
using launcher::SessionState;

[[nodiscard]] std::optional<double> millisecondsBetween(const std::optional<LaunchClock::time_point>& from,
                                                        const std::optional<LaunchClock::time_point>& to) {
    if (!from || !to) {
        return std::nullopt;
    }
    return std::chrono::duration<double, std::milli>(*to - *from).count();
}

/// The panel that asks the user while the session is in `state`.
[[nodiscard]] SessionPanel askingPanel(SessionState state) noexcept {
    switch (state) {
    case SessionState::StillRunning:
        return SessionPanel::StillRunning;
    case SessionState::AwaitingConfirmation:
        return SessionPanel::Confirm;
    default:
        return SessionPanel::Playing;
    }
}

[[nodiscard]] bool asksUser(SessionPanel panel) noexcept {
    return panel == SessionPanel::Playing || panel == SessionPanel::StillRunning || panel == SessionPanel::Confirm;
}

/// Nothing asks the user: the game has the screen (the window keeps the "Gioco in corso" notice behind it).
[[nodiscard]] bool gameHasScreen(SessionPanel panel) noexcept {
    return panel == SessionPanel::None || panel == SessionPanel::Launching;
}

} // namespace

std::string_view toString(SessionPanel panel) noexcept {
    switch (panel) {
    case SessionPanel::None:
        return "none";
    case SessionPanel::Launching:
        return "launching";
    case SessionPanel::Playing:
        return "playing";
    case SessionPanel::StillRunning:
        return "still-running";
    case SessionPanel::Confirm:
        return "confirm";
    case SessionPanel::Outcome:
        return "outcome";
    }
    return "unknown";
}

bool LaunchFlow::request(launcher::LaunchRequest request, LaunchInfo info, std::uint64_t frame) {
    if (m_pending || sessionActive()) {
        return false;
    }
    spdlog::info("Launch of '{}' ({}, profile '{}') asked on frame {}: it starts once the frame is presented",
                 info.title, info.gameId, info.profile, frame);
    LaunchRecord record;
    record.gameId = info.gameId;
    record.profile = info.profile;
    record.state = "Pending";
    record.requestFrame = frame;
    m_records.push_back(std::move(record));
    m_info = std::move(info);
    m_message = {};
    m_pending = std::move(request);
    m_returned = false;
    m_takeForeground = true;
    m_quitting = false;
    m_waitLimitReached = false;
    m_focusSince.reset();
    m_askOnPress = false;
    show(SessionPanel::Launching);
    return true;
}

bool LaunchFlow::refuse(LaunchMessage message, LaunchInfo info, std::uint64_t frame) {
    if (m_pending || sessionActive()) {
        return false;
    }
    spdlog::warn("Launch of '{}' ({}, profile '{}') cannot start: {}{}{}", info.title, info.gameId,
                 info.profile.empty() ? std::string("none") : info.profile, message.code,
                 message.reason.empty() ? std::string() : " (" + message.reason + ")",
                 message.detail.empty() ? std::string() : ": " + message.detail);
    LaunchRecord record;
    record.gameId = info.gameId;
    record.profile = info.profile;
    record.state = "Blocked";
    record.outcome = message.code;
    record.reason = message.reason;
    record.detail = message.detail;
    record.requestFrame = frame;
    m_records.push_back(std::move(record));
    m_info = std::move(info);
    m_message = std::move(message);
    show(SessionPanel::Outcome);
    return true;
}

void LaunchFlow::start(LaunchClock::time_point now, launcher::StartCallbacks callbacks, std::uint64_t frame) {
    if (!m_pending) {
        return;
    }
    m_session.emplace(std::move(*m_pending), m_runner);
    m_pending.reset();
    // The Launching frame was presented: the window keeps it behind the emulator while it runs.
    LaunchRecord& record = current();
    record.startFrame = frame;
    m_session->start(now, std::move(callbacks));
    m_seenState = m_session->state();
    record.state = std::string(launcher::toString(m_seenState));
    record.strategy = std::string(launcher::toString(m_session->strategy()));
    record.processId = m_session->processId();
    if (m_session->finished()) {
        finish(now, frame);
        return;
    }
    spdlog::info("Launch of '{}' running (process {}, {}): Retroverse renders nothing until it returns", m_info.title,
                 record.processId, record.strategy);
}

void LaunchFlow::poll(LaunchClock::time_point now, std::uint64_t frame) {
    if (!sessionActive()) {
        return;
    }
    m_session->poll(now);
    if (m_session->finished()) {
        finish(now, frame);
        return;
    }
    const std::optional<LaunchClock::time_point>& begun = m_session->times().begun;
    if (m_waitLimit && begun && now - *begun >= *m_waitLimit) {
        // A frame-limited run must reach its frame limit and write its report: it stops waiting, never ends the game.
        spdlog::warn("Launch of '{}' still {} after the wait limit of this frame-limited run ({} s): left running",
                     m_info.title, launcher::toString(m_session->state()),
                     std::chrono::duration<double>(*m_waitLimit).count());
        m_waitLimitReached = true;
        m_session->confirmReturn(now);
        if (m_session->finished()) {
            finish(now, frame);
        }
        return;
    }
    const SessionState state = m_session->state();
    if (state != m_seenState) {
        if (state == SessionState::StillRunning || state == SessionState::AwaitingConfirmation) {
            show(askingPanel(state));
        } else if (state == SessionState::Running && m_panel == SessionPanel::StillRunning) {
            // The game shows a window again: it has the screen, the host waits again after a frame with the notice.
            m_panel = SessionPanel::Launching;
        }
        m_seenState = state;
        current().state = std::string(launcher::toString(state));
    }
    if (gameHasScreen(m_panel) && m_focusSince && now - *m_focusSince >= kComeBackDelay) {
        spdlog::info("Back in Retroverse while '{}' runs: asking whether to return", m_info.title);
        show(askingPanel(state));
    }
}

void LaunchFlow::focusChanged(bool focused, LaunchClock::time_point now) noexcept {
    m_focused = focused;
    if (!focused) {
        m_focusSince.reset();
        m_askOnPress = false;
        // The user went back to the game without answering "Gioco in corso": the game has the screen again.
        if (sessionActive() && m_panel == SessionPanel::Playing) {
            m_panel = SessionPanel::Launching;
        }
    } else if (sessionActive() && !m_focusSince) {
        m_focusSince = now;
    }
}

bool LaunchFlow::userPressed() noexcept {
    if (!sessionActive() || !gameHasScreen(m_panel) || !m_focused || !m_askOnPress) {
        return false;
    }
    spdlog::info("A press in Retroverse while '{}' runs: asking again", m_info.title);
    show(askingPanel(m_session->state()));
    return true;
}

void LaunchFlow::returnToRetroverse(LaunchClock::time_point now, std::uint64_t frame) {
    if (!sessionActive()) {
        return;
    }
    spdlog::info("Return to Retroverse confirmed while '{}' is {}", m_info.title,
                 launcher::toString(m_session->state()));
    m_session->confirmReturn(now);
    if (m_session->finished()) {
        finish(now, frame);
    }
}

bool LaunchFlow::backToGame(LaunchClock::time_point now) noexcept {
    if (!sessionActive() || !asksUser(m_panel)) {
        return false;
    }
    const SessionPanel asked = m_panel;
    // One frame with the "Gioco in corso" notice, which the window then keeps behind the game.
    m_panel = SessionPanel::Launching;
    m_questionPresented = false;
    if (!m_focused) {
        // Asked again once the user comes back (a new focus gain), or when the session's state changes.
        m_focusSince.reset();
        m_askOnPress = false;
        return false;
    }
    // The user is still in front of Retroverse: a press asks again at once. Nothing must ever leave a focused window
    // waiting without a way back (the host drops the presses while it waits).
    m_askOnPress = true;
    if (asked == SessionPanel::StillRunning) {
        // "Continua ad aspettare": the game shows no window to go back to; the notice stays until a press or a change.
        m_focusSince.reset();
        return false;
    }
    // The application hands the foreground to the game; should the game never take it, the question comes back after
    // the come-back delay.
    m_focusSince = now;
    return true;
}

void LaunchFlow::dismissOutcome() noexcept {
    if (m_panel == SessionPanel::Outcome) {
        m_panel = SessionPanel::None;
    }
}

bool LaunchFlow::panelBack(LaunchClock::time_point now) noexcept {
    if (m_panel == SessionPanel::Outcome) {
        dismissOutcome();
        return false;
    }
    return backToGame(now);
}

void LaunchFlow::quit(LaunchClock::time_point now, std::uint64_t frame) {
    if (m_pending) {
        spdlog::info("Quitting before the launch of '{}' started: it never starts", m_info.title);
        m_pending.reset();
        m_panel = SessionPanel::None;
        return;
    }
    if (!sessionActive()) {
        return;
    }
    spdlog::info("Quitting while '{}' is {}: the emulator is left running (Retroverse never ends it)", m_info.title,
                 launcher::toString(m_session->state()));
    m_quitting = true;
    current().quitDuringGame = true;
    m_session->confirmReturn(now);
    if (m_session->finished()) {
        finish(now, frame);
    }
}

void LaunchFlow::frameRendered() noexcept {
    if (!sessionActive()) {
        return;
    }
    LaunchRecord& record = m_records.back();
    if (asksUser(m_panel)) {
        ++record.framesWithPanel;
        m_questionPresented = true;
    } else {
        ++record.framesWhileRunning;
    }
}

void LaunchFlow::idleWake() noexcept {
    if (sessionActive()) {
        ++m_records.back().idleWakes;
    }
}

void LaunchFlow::noteForeground(LaunchClock::time_point at) {
    if (!m_session || !m_session->finished() || m_records.empty()) {
        return;
    }
    const launcher::SessionTimes& times = m_session->times();
    m_records.back().foregroundMs = millisecondsBetween(times.exited ? times.exited : times.ended, at);
}

bool LaunchFlow::sessionActive() const noexcept {
    return m_session && !m_session->finished() && m_session->state() != SessionState::Idle;
}

bool LaunchFlow::hostIdle() const noexcept {
    if (m_pending) {
        return true;
    }
    if (!sessionActive()) {
        return false;
    }
    // A question nobody can see is presented once, then waits for the focus (no GPU work while the game runs).
    return gameHasScreen(m_panel) || (asksUser(m_panel) && !m_focused && m_questionPresented);
}

bool LaunchFlow::takeReturned() noexcept {
    return std::exchange(m_returned, false);
}

void LaunchFlow::show(SessionPanel panel) {
    if (panel == m_panel) {
        return;
    }
    m_panel = panel;
    m_questionPresented = false;
    if (asksUser(panel)) {
        m_askOnPress = false;
    }
    if (panel != SessionPanel::None && panel != SessionPanel::Launching && !m_records.empty()) {
        m_records.back().panels.emplace_back(toString(panel));
    }
}

void LaunchFlow::finish(LaunchClock::time_point now, std::uint64_t frame) {
    const launcher::LaunchOutcome& outcome = m_session->outcome();
    const launcher::SessionTimes& times = m_session->times();
    m_message = messageForOutcome(outcome);
    // Quitting and the wait limit end the session as LaunchSession::confirmReturn does, but nobody confirmed anything.
    const bool endedByUser = outcome.confirmedByUser && !m_quitting && !m_waitLimitReached;
    if ((m_quitting || m_waitLimitReached) && outcome.processesRemained) {
        m_message.code = std::string(lo::kLeftRunning);
        m_message.reason = std::string(m_quitting ? lo::kReasonQuit : lo::kReasonWaitLimit);
        m_message.detail.clear();
    }
    LaunchRecord& record = current();
    record.state = std::string(launcher::toString(outcome.state));
    record.outcome = m_message.code;
    record.reason = m_message.reason;
    record.detail = m_message.detail;
    record.exitCode = outcome.exitCode;
    record.strategy = std::string(launcher::toString(m_session->strategy()));
    record.processId = m_session->processId();
    record.quickExit = outcome.quickExit;
    record.processesRemained = outcome.processesRemained;
    record.confirmedByUser = endedByUser;
    record.wasStillRunning = outcome.wasStillRunning;
    record.waitLimitReached = m_waitLimitReached;
    record.endFrame = frame;
    record.startMs = millisecondsBetween(times.begun, times.started);
    record.runMs = millisecondsBetween(times.started, times.exited ? times.exited : times.ended);
    record.returnMs = millisecondsBetween(times.exited, times.ended ? times.ended : std::optional(now));
    m_focusSince.reset();
    m_askOnPress = false;
    m_returned = true;
    // Processes of the game's tree still run and nobody said the game was over: the window stays where it is.
    m_takeForeground = !outcome.processesRemained || endedByUser;
    spdlog::info("Launch of '{}' ended: {}, outcome {}{}{} (frames {} to {}: {} rendered while it ran, {} for the panel)",
                 m_info.title, record.state, record.outcome, record.reason.empty() ? "" : " (" + record.reason + ")",
                 record.detail.empty() ? std::string() : ": " + record.detail, record.startFrame.value_or(frame),
                 frame, record.framesWhileRunning, record.framesWithPanel);
    // Quitting shows nothing: the run is ending.
    if (!m_quitting && showsMessage(m_message)) {
        show(SessionPanel::Outcome);
    } else {
        m_panel = SessionPanel::None;
    }
}

} // namespace rv::app
