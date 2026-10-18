#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <string>
#include <string_view>

#include <rv/launcher/launch_plan.hpp>
#include <rv/launcher/launch_session.hpp>

namespace rv::app {

/// How a launch from the quick catalog ended, as the product UI tells the user (specification §11.5: a readable error
/// and a safe return; ADR-0006): a code and a reason within it that the game-session panel's document turns into its
/// Italian text (assets/ui/game_session.rml: no user-facing string in C++), and a detail shown as it comes (an NTSTATUS
/// name, a token, an exit code, a file name, the runner's error). Recorded in the run report too.
struct LaunchMessage {
    /// launch_outcome::k*: what happened.
    std::string code;
    /// launch_outcome::kReason*: why, within the code; empty when the code says it all.
    std::string reason;
    /// Data shown with the message (never a sentence the documents translate).
    std::string detail;

    bool operator==(const LaunchMessage&) const = default;
};

/// The codes and reasons of LaunchMessage, as the documents name them.
namespace launch_outcome {
/// The emulator ended and the session with it: nothing to tell.
inline constexpr std::string_view kReturned = "returned";
/// The started program exited cleanly within the startup grace (ADR-0006: a launcher stub, or an emulator that gave up
/// at once): "if it is still running, its profile needs to wait for the process tree".
inline constexpr std::string_view kQuickExit = "quick-exit";
/// The started program ended on its own while processes of its tree still ran (DirectProcess: a wrapper or launcher
/// whose emulator is its child): the game may still run, left alone, and its profile should wait for the process tree
/// (ADR-0006: the product shows processesRemained).
inline constexpr std::string_view kProcessesRemained = "processes-remained";
/// The session ended while the emulator still ran: the user returned (kReasonConfirmed), Retroverse quit (kReasonQuit)
/// or a frame-limited run stopped waiting (kReasonWaitLimit). Nothing was terminated.
inline constexpr std::string_view kLeftRunning = "left-running";
/// The emulator ended with an unhandled exception; the detail is its exit code and NTSTATUS name.
inline constexpr std::string_view kCrashed = "crashed";
/// The emulator did not start; the reason says how (launcher::FailureReason).
inline constexpr std::string_view kFailedToStart = "failed-to-start";
/// The game cannot be launched as configured (the launch plan's configuration blockers, a game without a launch
/// target or with an unknown profile); the detail names the token, the variable or the profile.
inline constexpr std::string_view kNotConfigured = "not-configured";
/// The profile's emulator is nowhere on the machine; the detail is the profile's name.
inline constexpr std::string_view kEmulatorMissing = "emulator-missing";
/// A file the profile requires is missing (a RetroArch core); the detail names it.
inline constexpr std::string_view kCoreMissing = "core-missing";
/// The game's file does not exist; the detail is its file name.
inline constexpr std::string_view kRomMissing = "rom-missing";
/// A game of the ES-DE catalog that is not ready, resolved again right before the launch (I2 step 13, ADR-0008 §8):
/// the reason is its readiness (catalog::toString(LaunchReadiness): "source-unavailable", "rom-missing",
/// "requires-configuration", "emulator-not-found", "core-not-found"), the detail its first blocker's own words; nothing
/// was started.
inline constexpr std::string_view kNotReady = "not-ready";

/// kFailedToStart: validateLaunchRequest() refused the request at the start.
inline constexpr std::string_view kReasonInvalidRequest = "invalid-request";
/// kFailedToStart: the process could not be created (the runner's error).
inline constexpr std::string_view kReasonStartFailed = "start-failed";
/// kFailedToStart: the automation guard refused the request (launcher::LaunchOutcome::refusedByGuard: RV_LAUNCH_ALLOWLIST
/// is set in this environment and does not allow it); nothing was created. The detail is the guard's own words.
inline constexpr std::string_view kReasonGuardRefused = "guard-refused";
/// kFailedToStart: Windows could not load the program (a missing DLL, a bad image); the detail is the NTSTATUS name.
inline constexpr std::string_view kReasonLoaderError = "loader-error";
/// kFailedToStart: it ended with an error code within the startup grace; the detail is the exit code.
inline constexpr std::string_view kReasonEarlyExit = "early-exit";
/// kNotConfigured: a template of the profile cannot be expanded (an unknown variable, an unset environment variable,
/// a working directory that is not absolute); the detail is the token.
inline constexpr std::string_view kReasonTemplate = "template";
/// kNotConfigured: the executable is a command interpreter or a launcher (launcher::isRefusedProgram).
inline constexpr std::string_view kReasonProgramRefused = "program-refused";
/// kNotConfigured: the game names a profile that does not exist; the detail is its id.
inline constexpr std::string_view kReasonUnknownProfile = "unknown-profile";
/// kNotConfigured: the game has no launch target at all.
inline constexpr std::string_view kReasonNoLaunch = "no-launch";
/// kLeftRunning: the user confirmed the return while the emulator ran.
inline constexpr std::string_view kReasonConfirmed = "confirmed";
/// kLeftRunning: Retroverse quit (its window closed) while the emulator ran.
inline constexpr std::string_view kReasonQuit = "quit";
/// kLeftRunning: a frame-limited run waited for the emulator longer than its --launch-wait-limit.
inline constexpr std::string_view kReasonWaitLimit = "wait-limit";
} // namespace launch_outcome

/// The message of a session that ended (launcher::LaunchOutcome of a final session):
///  - Crashed: kCrashed, the exit code and its NTSTATUS name ("0xC0000005 STATUS_ACCESS_VIOLATION");
///  - FailedToStart: kFailedToStart with the reason of its FailureReason (kReasonGuardRefused for a start the
///    automation guard refused), the detail the NTSTATUS name of a loader error, the exit code of an early exit
///    (exitCodeDetail), otherwise the error of the session's message without the launch it names (the request's
///    validation, the runner's error, the guard's refusal);
///  - Returned, ended by the user's confirmation: kLeftRunning (kReasonConfirmed) when processes remained, kReturned
///    otherwise (the user said the game was over: a quick exit of the started program, which UserConfirmReturn
///    expects, is no hint then);
///  - Returned on its own: kProcessesRemained when processes of the tree still ran, kQuickExit for a quick exit,
///    kReturned otherwise.
/// The detail of a return is the exit code (exitCodeDetail).
[[nodiscard]] LaunchMessage messageForOutcome(const launcher::LaunchOutcome& outcome);

/// The message of a launch the plan refused (nothing started): kRomMissing (the ROM's file name), kNotConfigured
/// (kReasonTemplate: the first {token} of the blocker's message, kReasonProgramRefused, kReasonInvalidRequest: the
/// message), kEmulatorMissing (`profileName`), kCoreMissing (the message).
[[nodiscard]] LaunchMessage messageForBlocker(const launcher::LaunchBlocker& blocker,
                                              const launcher::LaunchTarget& target, std::string_view profileName);

/// The first "{...}" of `text` (a template token named in an error: "{var:core}"), or nothing.
[[nodiscard]] std::string_view firstToken(std::string_view text) noexcept;

/// "0xC0000005 STATUS_ACCESS_VIOLATION": an exit code in hexadecimal with its NTSTATUS name when it has a known one.
[[nodiscard]] std::string exitCodeName(std::uint32_t code);
/// An exit code as the panel shows it after "codice": the number alone in decimal ("3") below 0x10000, which programs
/// return themselves, exitCodeName() above (the NTSTATUS range).
[[nodiscard]] std::string exitCodeDetail(std::uint32_t code);

/// True when the game-session panel shows `message` to the user: every code but kReturned and a kLeftRunning the user
/// chose (kReasonConfirmed): those ended as the user expects.
[[nodiscard]] bool showsMessage(const LaunchMessage& message) noexcept;

} // namespace rv::app
