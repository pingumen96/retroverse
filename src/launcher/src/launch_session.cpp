#include <rv/launcher/launch_session.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>

namespace rv::launcher {
namespace {

[[nodiscard]] double secondsBetween(LaunchClock::time_point from, LaunchClock::time_point to) noexcept {
    return std::chrono::duration<double>(to - from).count();
}

} // namespace

std::string_view toString(SessionState state) noexcept {
    switch (state) {
    case SessionState::Idle:
        return "Idle";
    case SessionState::Validating:
        return "Validating";
    case SessionState::Starting:
        return "Starting";
    case SessionState::Running:
        return "Running";
    case SessionState::StillRunning:
        return "StillRunning";
    case SessionState::AwaitingConfirmation:
        return "AwaitingConfirmation";
    case SessionState::Returned:
        return "Returned";
    case SessionState::FailedToStart:
        return "FailedToStart";
    case SessionState::Crashed:
        return "Crashed";
    }
    return "unknown";
}

bool isFinal(SessionState state) noexcept {
    return state == SessionState::Returned || state == SessionState::FailedToStart || state == SessionState::Crashed;
}

std::string_view toString(FailureReason reason) noexcept {
    switch (reason) {
    case FailureReason::None:
        return "none";
    case FailureReason::InvalidRequest:
        return "invalid request";
    case FailureReason::StartFailed:
        return "start failed";
    case FailureReason::LoaderError:
        return "loader error";
    case FailureReason::EarlyExit:
        return "early exit";
    }
    return "unknown";
}

LaunchSession::LaunchSession(LaunchRequest request, IProcessRunner& runner)
    : m_request(std::move(request)), m_runner(&runner), m_strategy(m_request.waitStrategy) {}

LaunchSession::LaunchSession(LaunchSession&&) noexcept = default;
LaunchSession& LaunchSession::operator=(LaunchSession&&) noexcept = default;
LaunchSession::~LaunchSession() = default;

std::uint32_t LaunchSession::processId() const noexcept {
    return m_process ? m_process->processId() : 0u;
}

std::size_t LaunchSession::liveProcessCount() const noexcept {
    if (!m_process || m_treeEmpty) {
        return 0;
    }
    return (m_mainExit ? 0u : 1u) + m_liveChildren.size();
}

std::string LaunchSession::describeLaunch() const {
    if (!m_request.label.empty()) {
        return m_request.label;
    }
    try {
        return core::pathToUtf8(m_request.executable.filename());
    } catch (...) {
        return "the program";
    }
}

void LaunchSession::setState(SessionState state, std::string_view why) {
    if (m_state == state) {
        return;
    }
    if (why.empty()) {
        spdlog::info("Launch session: {} -> {}", toString(m_state), toString(state));
    } else {
        spdlog::info("Launch session: {} -> {}: {}", toString(m_state), toString(state), why);
    }
    m_state = state;
    if (isFinal(state)) {
        m_outcome.state = state;
    }
}

void LaunchSession::fail(FailureReason reason, std::string message, LaunchClock::time_point now) {
    m_outcome.failure = reason;
    m_outcome.message = std::move(message);
    m_times.ended = now;
    setState(SessionState::FailedToStart, m_outcome.message);
}

void LaunchSession::start(LaunchClock::time_point now, StartCallbacks callbacks) {
    if (m_state != SessionState::Idle) {
        spdlog::warn("Launch session: start() ignored in state {}", toString(m_state));
        return;
    }
    m_times.begun = now;
    setState(SessionState::Validating, {});
    if (auto valid = validateLaunchRequest(m_request); !valid) {
        fail(FailureReason::InvalidRequest,
             fmt::format("{} was not started: {}", describeLaunch(), valid.error().toString()), now);
        return;
    }
    m_commandLine = commandLineOf(m_request).value_or(std::string());

    setState(SessionState::Starting, {});
    std::string workingDirectory;
    try {
        workingDirectory = core::pathToUtf8(workingDirectoryOf(m_request));
    } catch (...) {
        workingDirectory = "?";
    }
    spdlog::info("Launching {} ({}, working directory {}): {}", describeLaunch(), toString(m_request.waitStrategy),
                 workingDirectory, m_commandLine);
    auto process = m_runner->start(m_request, std::move(callbacks));
    if (!process) {
        fail(FailureReason::StartFailed,
             fmt::format("{} could not be started: {}", describeLaunch(), process.error().toString()), now);
        return;
    }
    m_process = std::move(*process);
    m_times.started = now;
    m_outcome.processesSeen = 1;
    m_windowClockStart = now;
    m_strategy = effectiveStrategy(m_request.waitStrategy, m_process->followsTree());
    if (m_strategy != m_request.waitStrategy) {
        spdlog::warn("Launch session: the process tree of {} cannot be followed; waiting for the started process, "
                     "then for the user's confirmation",
                     describeLaunch());
    }
    setState(SessionState::Running, fmt::format("process {}", m_process->processId()));
}

void LaunchSession::poll(LaunchClock::time_point now) {
    if (finished() || !m_process) {
        return;
    }
    m_pending.clear();
    m_process->takeEvents(m_pending);
    for (const ProcessEvent& event : m_pending) {
        apply(event);
        if (finished()) {
            break;
        }
    }
    checkTimeouts(now);
}

void LaunchSession::apply(const ProcessEvent& event) {
    if (m_state != SessionState::Running && m_state != SessionState::StillRunning &&
        m_state != SessionState::AwaitingConfirmation) {
        return;
    }
    if (m_history.size() < kMaxHistory) {
        m_history.push_back(event);
    }
    switch (event.kind) {
    case ProcessEventKind::MainExited:
        onMainExited(event);
        break;
    case ProcessEventKind::ChildStarted:
        if (event.processId != processId() && !m_treeEmpty &&
            std::find(m_liveChildren.begin(), m_liveChildren.end(), event.processId) == m_liveChildren.end()) {
            m_liveChildren.push_back(event.processId);
            ++m_outcome.processesSeen;
            spdlog::info("Launch session: process {} ({}) joined the tree", event.processId,
                         event.image.empty() ? std::string_view("?") : std::string_view(event.image));
        }
        break;
    case ProcessEventKind::ChildExited:
        onChildExited(event);
        break;
    case ProcessEventKind::TreeEmpty:
        onTreeEmpty(event);
        break;
    case ProcessEventKind::WindowShown:
        if (!m_times.firstWindow) {
            m_times.firstWindow = event.observedAt;
            if (m_times.started) {
                spdlog::info("Launch session: {} shows a window {:.2f} s after the start", describeLaunch(),
                             secondsBetween(*m_times.started, event.observedAt));
            }
        }
        m_windowVisible = true;
        if (m_state == SessionState::StillRunning) {
            setState(SessionState::Running, "a window of the emulator is visible again");
        }
        break;
    case ProcessEventKind::WindowsGone:
        m_windowVisible = false;
        m_windowClockStart = event.observedAt;
        break;
    case ProcessEventKind::WatchFailed:
        spdlog::error("Launch session: the processes of {} can no longer be followed ({}); the return needs the "
                      "user's confirmation",
                      describeLaunch(), event.message);
        m_strategy = WaitStrategy::UserConfirmReturn;
        break;
    }
}

void LaunchSession::onMainExited(const ProcessEvent& event) {
    if (m_mainExit) {
        return;
    }
    const std::uint32_t id = event.processId != 0 ? event.processId : processId();
    m_mainExit = Exit{id, event.exitCode.value_or(0u), event.occurredAt, event.observedAt};
    m_times.mainExited = event.occurredAt;
    const ExitStatus status = classifyExitCode(m_mainExit->code);

    switch (m_strategy) {
    case WaitStrategy::DirectProcess:
        conclude(*m_mainExit);
        break;
    case WaitStrategy::ProcessTree:
        if (!m_lastExit || m_mainExit->occurredAt >= m_lastExit->occurredAt) {
            m_lastExit = m_mainExit;
        }
        if (status.kind == ExitKind::Crash && !m_treeCrash) {
            m_treeCrash = m_mainExit;
        }
        spdlog::info("Launch session: the started process exited ({}); waiting for {} more process(es) of the tree",
                     describeExitCode(m_mainExit->code), m_liveChildren.size());
        if (m_treeEmpty) {
            conclude(m_treeCrash ? *m_treeCrash : *m_lastExit);
        }
        break;
    case WaitStrategy::UserConfirmReturn:
        if (status.kind == ExitKind::Crash || status.kind == ExitKind::LoaderError ||
            (status.kind != ExitKind::Success && withinStartupGrace(m_mainExit->occurredAt))) {
            conclude(*m_mainExit);
            break;
        }
        m_outcome.exitCode = m_mainExit->code;
        m_outcome.exitProcessId = m_mainExit->processId;
        m_outcome.exitKind = status.kind;
        m_outcome.quickExit = status.kind == ExitKind::Success && withinStartupGrace(m_mainExit->occurredAt);
        m_times.exited = m_mainExit->occurredAt;
        m_times.exitObserved = m_mainExit->observedAt;
        setState(SessionState::AwaitingConfirmation,
                 fmt::format("the started process exited ({}); the user confirms the return",
                             describeExitCode(m_mainExit->code)));
        break;
    }
}

void LaunchSession::onChildExited(const ProcessEvent& event) {
    std::erase(m_liveChildren, event.processId);
    if (!event.exitCode) {
        spdlog::info("Launch session: process {} of the tree exited (exit code unknown)", event.processId);
        return;
    }
    const Exit exit{event.processId, *event.exitCode, event.occurredAt, event.observedAt};
    spdlog::info("Launch session: process {} ({}) of the tree exited: {}", event.processId,
                 event.image.empty() ? std::string_view("?") : std::string_view(event.image),
                 describeExitCode(exit.code));
    if (!m_lastExit || exit.occurredAt >= m_lastExit->occurredAt) {
        m_lastExit = exit;
    }
    if (classifyExitCode(exit.code).kind == ExitKind::Crash && !m_treeCrash) {
        m_treeCrash = exit;
    }
}

void LaunchSession::onTreeEmpty(const ProcessEvent& event) {
    m_treeEmpty = true;
    m_liveChildren.clear();
    if (m_strategy != WaitStrategy::ProcessTree || !m_mainExit) {
        return; // informational, or the started process's exit is still to be reported
    }
    Exit exit = m_treeCrash ? *m_treeCrash : (m_lastExit ? *m_lastExit : *m_mainExit);
    // The session ends with the tree: its end is the last exit, observed now.
    exit.occurredAt = std::max(exit.occurredAt, event.occurredAt);
    exit.observedAt = event.observedAt;
    conclude(exit);
}

bool LaunchSession::withinStartupGrace(LaunchClock::time_point at) const noexcept {
    const auto grace = m_request.timeouts.startupGrace;
    return grace.count() > 0 && m_times.started && at - *m_times.started < grace;
}

void LaunchSession::conclude(const Exit& exit) {
    const ExitStatus status = classifyExitCode(exit.code);
    m_outcome.exitCode = exit.code;
    m_outcome.exitProcessId = exit.processId;
    m_outcome.exitKind = status.kind;
    m_outcome.processesRemained = liveProcessCount() > 0;
    m_times.exited = exit.occurredAt;
    m_times.exitObserved = exit.observedAt;
    if (!m_times.ended) {
        m_times.ended = exit.observedAt;
    }
    const double ranFor = m_times.started ? secondsBetween(*m_times.started, exit.occurredAt) : 0.0;
    const std::string code = describeExitCode(exit.code);
    const bool early = withinStartupGrace(exit.occurredAt);

    if (status.kind == ExitKind::Crash) {
        m_outcome.message = fmt::format("{} crashed after {:.1f} s: {}", describeLaunch(), ranFor, code);
        setState(SessionState::Crashed, m_outcome.message);
        return;
    }
    if (status.kind == ExitKind::LoaderError) {
        m_outcome.failure = FailureReason::LoaderError;
        m_outcome.message = fmt::format("{} could not be loaded: {}", describeLaunch(), code);
        setState(SessionState::FailedToStart, m_outcome.message);
        return;
    }
    if (early && status.kind != ExitKind::Success) {
        m_outcome.failure = FailureReason::EarlyExit;
        m_outcome.message =
            fmt::format("{} ended {:.0f} ms after it started, with {}: it did not start", describeLaunch(),
                        ranFor * 1000.0, code);
        setState(SessionState::FailedToStart, m_outcome.message);
        return;
    }
    m_outcome.quickExit = early;
    if (m_outcome.confirmedByUser) {
        m_outcome.message =
            fmt::format("{} exited after {:.1f} s ({}); the user confirmed the return", describeLaunch(), ranFor, code);
    } else if (early) {
        m_outcome.message = fmt::format("{} exited {:.0f} ms after it started ({}): if the emulator is still running, "
                                        "it was started by a launcher; wait for its process tree or for a confirmation",
                                        describeLaunch(), ranFor * 1000.0, code);
    } else {
        m_outcome.message = fmt::format("{} exited after {:.1f} s ({})", describeLaunch(), ranFor, code);
    }
    if (m_outcome.processesRemained) {
        m_outcome.message += fmt::format("; {} process(es) of its tree still run", liveProcessCount());
    }
    setState(SessionState::Returned, m_outcome.message);
}

void LaunchSession::checkTimeouts(LaunchClock::time_point now) {
    const auto timeout = m_request.timeouts.windowTimeout;
    if (m_state != SessionState::Running || timeout.count() <= 0 || m_windowVisible) {
        return;
    }
    if (now - m_windowClockStart < timeout) {
        return;
    }
    m_times.stillRunningSince = now;
    m_outcome.wasStillRunning = true;
    setState(SessionState::StillRunning,
             fmt::format("{} has shown no window for {:.1f} s while {} process(es) run; it is left running, the user "
                         "can return",
                         describeLaunch(), secondsBetween(m_windowClockStart, now), liveProcessCount()));
}

void LaunchSession::confirmReturn(LaunchClock::time_point now) {
    switch (m_state) {
    case SessionState::AwaitingConfirmation:
        m_outcome.confirmedByUser = true;
        m_times.ended = now;
        conclude(*m_mainExit);
        break;
    case SessionState::Running:
    case SessionState::StillRunning:
        m_outcome.confirmedByUser = true;
        m_outcome.processesRemained = liveProcessCount() > 0;
        if (m_lastExit) {
            m_outcome.exitCode = m_lastExit->code;
            m_outcome.exitProcessId = m_lastExit->processId;
            m_outcome.exitKind = classifyExitCode(m_lastExit->code).kind;
        }
        m_times.ended = now;
        m_outcome.message = fmt::format("The user returned while {} still runs ({} process(es)); it is left running",
                                        describeLaunch(), liveProcessCount());
        setState(SessionState::Returned, m_outcome.message);
        break;
    default:
        break;
    }
}

} // namespace rv::launcher
