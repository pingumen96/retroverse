#pragma once

// Play sessions (ADR-0008 §9, specification §13.4): every launch attempt, recorded when it starts and completed when it
// ends, with UTC wall-clock times, the game, the choice it launched with, its outcome (a game blocked before the launch,
// failures to start, refusals by the automation guard and quick exits included) and its owner, the process that
// launched it. A session stays open while its owner runs; at start, only sessions whose owner is gone are closed, as
// interrupted, so the app and retroverse-cli never close each other's live sessions. rv_catalog_db's SessionStore
// stores them (rv/catalog_db/session_store.hpp, I2 step 12).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/launch_resolution.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/exit_status.hpp>
#include <rv/launcher/launch_session.hpp>

namespace rv::catalog {

/// How a session ended: launcher::SessionState's final states, plus those a record has besides.
enum class PlaySessionOutcome : std::uint8_t {
    Running,       ///< open: the launch started and its end is not recorded yet
    Returned,      ///< launcher::SessionState::Returned (the emulator ended, or the user confirmed the return with
                   ///< nothing of it left running)
    FailedToStart, ///< launcher::SessionState::FailedToStart (PlaySession::failure says why; refusedByGuard when the
                   ///< automation guard refused it)
    Crashed,       ///< launcher::SessionState::Crashed
    Interrupted,   ///< its owner ended before the session did; closed by a later start (no end time is known)
    Blocked,       ///< nothing was started: the game was not ready right before the launch (PlaySession::readiness,
                   ///< ::message)
    LeftRunning,   ///< its owner stopped following the game while it still ran (PlaySession::leftRunning says why);
                   ///< nothing was terminated, and the game's own end is not known
};

/// "running", "returned", "failed-to-start", "crashed", "interrupted", "blocked", "left-running".
[[nodiscard]] constexpr std::string_view toString(PlaySessionOutcome outcome) noexcept {
    switch (outcome) {
    case PlaySessionOutcome::Running:
        return "running";
    case PlaySessionOutcome::Returned:
        return "returned";
    case PlaySessionOutcome::FailedToStart:
        return "failed-to-start";
    case PlaySessionOutcome::Crashed:
        return "crashed";
    case PlaySessionOutcome::Interrupted:
        return "interrupted";
    case PlaySessionOutcome::Blocked:
        return "blocked";
    case PlaySessionOutcome::LeftRunning:
        return "left-running";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<PlaySessionOutcome> parsePlaySessionOutcome(std::string_view text) noexcept {
    return detail::enumFromString(text, PlaySessionOutcome::LeftRunning);
}

/// Why a session's owner stopped following a game that still ran (PlaySessionOutcome::LeftRunning): the owner ended the
/// session with launcher::LaunchSession::confirmReturn() while processes of the game's tree ran.
enum class LeftRunningReason : std::uint8_t {
    None,      ///< the session was not left running
    Confirmed, ///< the user said they were back while the game ran
    NotWaited, ///< the launch was not waited for (retroverse-cli launch without --wait: followed through the startup
               ///< grace only)
    WaitLimit, ///< the owner's wait limit passed (retroverse-cli launch --wait-limit, the app's --launch-wait-limit)
    Quit,      ///< the owner quit while the game ran (Ctrl+C in retroverse-cli, the app closed)
};

/// "none", "confirmed", "not-waited", "wait-limit", "quit".
[[nodiscard]] constexpr std::string_view toString(LeftRunningReason reason) noexcept {
    switch (reason) {
    case LeftRunningReason::None:
        return "none";
    case LeftRunningReason::Confirmed:
        return "confirmed";
    case LeftRunningReason::NotWaited:
        return "not-waited";
    case LeftRunningReason::WaitLimit:
        return "wait-limit";
    case LeftRunningReason::Quit:
        return "quit";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<LeftRunningReason> parseLeftRunningReason(std::string_view text) noexcept {
    return detail::enumFromString(text, LeftRunningReason::Quit);
}

/// The process that recorded a session.
struct SessionOwner {
    std::uint32_t processId = 0;
    /// Its creation time (GetProcessTimes), in FILETIME ticks: with the id, it tells the owner from a later process
    /// that reuses the id.
    std::uint64_t creationTime = 0;

    auto operator<=>(const SessionOwner&) const = default;
};

/// One launch attempt.
struct PlaySession {
    /// The store's id (AUTOINCREMENT, never reused); 0 until the session is stored.
    std::int64_t id = 0;
    GameId game;
    /// What launched it, or would have: an ES-DE command label of the game's system or a Retroverse emulator profile's
    /// id (ChosenLaunch::choice); for a blocked attempt, the choice that was blocked (an empty label when the system
    /// has no command).
    launcher::LaunchChoice choice;
    /// Where the choice came from (ChosenLaunch::source).
    ChoiceSource choiceSource = ChoiceSource::FirstCommand;
    /// A launch verification (retroverse-cli launch --test).
    bool test = false;
    std::chrono::system_clock::time_point started;
    /// When it ended (for LeftRunning, when the owner stopped following it); nothing while Running, and for Interrupted
    /// (the owner's end is not known).
    std::optional<std::chrono::system_clock::time_point> ended;
    /// Interrupted: when a later start closed it.
    std::optional<std::chrono::system_clock::time_point> closed;
    /// How long the game ran, from the moment its process ran to its authoritative exit (launcher::SessionTimes, a
    /// monotonic clock); for LeftRunning, to the moment the owner stopped following it. Nothing when nothing ran, and
    /// for Running and Interrupted.
    std::optional<std::chrono::milliseconds> runTime;
    PlaySessionOutcome outcome = PlaySessionOutcome::Running;
    /// Blocked: the readiness that kept the game from launching (never Ready).
    std::optional<LaunchReadiness> readiness;
    /// LeftRunning: why.
    LeftRunningReason leftRunning = LeftRunningReason::None;
    /// The details of launcher::LaunchOutcome, once the session is no longer Running.
    launcher::FailureReason failure = launcher::FailureReason::None;
    /// The exit code of the authoritative process (launcher::LaunchOutcome::exitCode) and its kind
    /// (launcher::classifyExitCode()), both present or both absent: absent when nothing exited, so for an attempt that
    /// never started (Blocked, a refusal by the automation guard, a process that could not be created), while Running,
    /// for Interrupted, and when the user returned before any process of the game exited.
    std::optional<std::uint32_t> exitCode;
    std::optional<launcher::ExitKind> exitKind;
    bool quickExit = false;
    /// The user confirmed the return (never set when the owner quit or a limit ended the session).
    bool confirmedByUser = false;
    bool processesRemained = false;
    /// FailedToStart: the automation guard (RV_LAUNCH_ALLOWLIST) refused the request; nothing was created.
    bool refusedByGuard = false;
    /// One sentence: the launcher's message, or for Blocked the blockers' (the first one's code and message).
    std::string message;
    /// The started process (the emulator, or the launcher the profile or command names); 0 when nothing started.
    std::uint32_t processId = 0;
    SessionOwner owner;

    bool operator==(const PlaySession&) const = default;
};

/// The record's outcome of a launcher outcome: its final state, or Running while the session is not final.
[[nodiscard]] inline PlaySessionOutcome sessionOutcomeOf(const launcher::LaunchOutcome& outcome) noexcept {
    switch (outcome.state) {
    case launcher::SessionState::Returned:
        return PlaySessionOutcome::Returned;
    case launcher::SessionState::FailedToStart:
        return PlaySessionOutcome::FailedToStart;
    case launcher::SessionState::Crashed:
        return PlaySessionOutcome::Crashed;
    default:
        return PlaySessionOutcome::Running;
    }
}

/// Completes `session` with a finished launch: its outcome and details, ended at `ended`. `ownerReason` says why the
/// owner ended the session with confirmReturn(), when it did (None: the session ended on its own, or the user
/// confirmed the return): a session the owner ended while processes of the game still ran is LeftRunning for that
/// reason (Confirmed for the user's own confirmation), and confirmedByUser holds only for the user's confirmation.
inline void recordOutcome(PlaySession& session, const launcher::LaunchOutcome& outcome,
                          std::chrono::system_clock::time_point ended,
                          LeftRunningReason ownerReason = LeftRunningReason::None) {
    session.outcome = sessionOutcomeOf(outcome);
    session.ended = ended;
    session.failure = outcome.failure;
    session.exitCode = outcome.exitCode;
    // LaunchOutcome::exitKind defaults to Success: it says something only of an exit code.
    session.exitKind = outcome.exitCode ? std::optional<launcher::ExitKind>(outcome.exitKind) : std::nullopt;
    session.quickExit = outcome.quickExit;
    session.confirmedByUser =
        outcome.confirmedByUser && (ownerReason == LeftRunningReason::None || ownerReason == LeftRunningReason::Confirmed);
    session.processesRemained = outcome.processesRemained;
    session.message = outcome.message;
    session.leftRunning = LeftRunningReason::None;
    if (outcome.state == launcher::SessionState::Returned && outcome.confirmedByUser && outcome.processesRemained) {
        session.outcome = PlaySessionOutcome::LeftRunning;
        session.leftRunning = ownerReason == LeftRunningReason::None ? LeftRunningReason::Confirmed : ownerReason;
    }
}

/// True when `session` returned normally: Returned, a clean exit (code 0, or none when the user confirmed the return
/// while nothing ran any more) that was no quick exit (a launcher stub's quick exit counts for nothing once the user
/// confirmed the return, as ADR-0006's UserConfirmReturn has it), nothing of it left running. Such a launch verifies
/// its choice for its system (ADR-0008 §8: "verified" is shown; a launch test, retroverse-cli launch --test, asks for
/// it).
[[nodiscard]] inline bool returnedNormally(const PlaySession& session) noexcept {
    return session.outcome == PlaySessionOutcome::Returned &&
           session.exitKind.value_or(launcher::ExitKind::Success) == launcher::ExitKind::Success &&
           (!session.exitCode || *session.exitCode == 0) && (!session.quickExit || session.confirmedByUser) &&
           !session.processesRemained;
}

} // namespace rv::catalog
