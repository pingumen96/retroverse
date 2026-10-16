#pragma once

// Play sessions (ADR-0008 §9, specification §13.4): every launch attempt, recorded when it starts and completed when it
// ends, with UTC wall-clock times, its outcome (failures to start and quick exits included) and its owner, the process
// that launched it. A session stays open while its owner runs; at start, only sessions whose owner is gone are closed,
// as interrupted, so the app and retroverse-cli never close each other's live sessions.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/catalog/ids.hpp>
#include <rv/launcher/exit_status.hpp>
#include <rv/launcher/launch_session.hpp>

namespace rv::catalog {

/// How a session ended: launcher::SessionState's final states, plus the two a record has besides.
enum class PlaySessionOutcome : std::uint8_t {
    Running,       ///< open: the launch started and its end is not recorded yet
    Returned,      ///< launcher::SessionState::Returned (the emulator ended, or the user confirmed the return)
    FailedToStart, ///< launcher::SessionState::FailedToStart (PlaySession::failure says why)
    Crashed,       ///< launcher::SessionState::Crashed
    Interrupted,   ///< its owner ended before the session did; closed by a later start (no end time is known)
};

/// "running", "returned", "failed-to-start", "crashed", "interrupted".
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
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<PlaySessionOutcome> parsePlaySessionOutcome(std::string_view text) noexcept {
    return detail::enumFromString(text, PlaySessionOutcome::Interrupted);
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
    /// What launched it: a Retroverse emulator profile's id, or (empty) the ES-DE command commandLabel names.
    std::string profile;
    /// The label of the system's ES-DE command that launched it ("Fake Emulator"), when no profile did.
    std::string commandLabel;
    std::chrono::system_clock::time_point started;
    /// When it ended; nothing while Running, and for Interrupted (the owner's end is not known).
    std::optional<std::chrono::system_clock::time_point> ended;
    PlaySessionOutcome outcome = PlaySessionOutcome::Running;
    /// The details of launcher::LaunchOutcome, once the session is no longer Running.
    launcher::FailureReason failure = launcher::FailureReason::None;
    std::optional<std::uint32_t> exitCode;
    launcher::ExitKind exitKind = launcher::ExitKind::Success;
    bool quickExit = false;
    bool confirmedByUser = false;
    bool processesRemained = false;
    std::string message;
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

/// Completes `session` with a finished launch: its outcome and details, ended at `ended`.
inline void recordOutcome(PlaySession& session, const launcher::LaunchOutcome& outcome,
                          std::chrono::system_clock::time_point ended) {
    session.outcome = sessionOutcomeOf(outcome);
    session.ended = ended;
    session.failure = outcome.failure;
    session.exitCode = outcome.exitCode;
    session.exitKind = outcome.exitKind;
    session.quickExit = outcome.quickExit;
    session.confirmedByUser = outcome.confirmedByUser;
    session.processesRemained = outcome.processesRemained;
    session.message = outcome.message;
}

} // namespace rv::catalog
