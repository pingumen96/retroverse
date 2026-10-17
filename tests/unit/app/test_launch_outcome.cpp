// How a launch from the quick catalog ends, as the product UI tells it (I2 step A1): codes and reasons the game-session
// panel's document turns into its Italian text, details shown as they come.

#include <catch2/catch_test_macros.hpp>

#include <rv/app/launch_outcome.hpp>
#include <rv/core/text.hpp>

using rv::app::LaunchMessage;
using rv::launcher::BlockerKind;
using rv::launcher::FailureReason;
using rv::launcher::LaunchBlocker;
using rv::launcher::LaunchOutcome;
using rv::launcher::LaunchTarget;
using rv::launcher::SessionState;

namespace lo = rv::app::launch_outcome;

namespace {

LaunchOutcome outcome(SessionState state, std::optional<std::uint32_t> exitCode = std::nullopt) {
    LaunchOutcome result;
    result.state = state;
    result.exitCode = exitCode;
    return result;
}

LaunchTarget target() {
    LaunchTarget t;
    t.rom = rv::core::pathFromUtf8("C:/Giochi/gb/Lanterna di Vetro (Europa).gb");
    t.system = "gb";
    t.title = "Lanterna di Vetro";
    return t;
}

} // namespace

TEST_CASE("app/launch_outcome: a session's end becomes a code, a reason and a detail", "[app][launch_outcome]") {
    // A normal end tells nothing.
    const LaunchMessage returned = rv::app::messageForOutcome(outcome(SessionState::Returned, 0));
    CHECK(returned.code == lo::kReturned);
    CHECK(returned.reason.empty());
    CHECK(returned.detail == "0 (success)");
    CHECK_FALSE(rv::app::showsMessage(returned));

    // A quick exit carries ADR-0006's hint (in the document).
    LaunchOutcome quick = outcome(SessionState::Returned, 0);
    quick.quickExit = true;
    CHECK(rv::app::messageForOutcome(quick).code == lo::kQuickExit);
    CHECK(rv::app::showsMessage(rv::app::messageForOutcome(quick)));

    // A crash names its NTSTATUS.
    const LaunchMessage crashed = rv::app::messageForOutcome(outcome(SessionState::Crashed, 0xC0000005u));
    CHECK(crashed == LaunchMessage{std::string(lo::kCrashed), "", "0xC0000005 STATUS_ACCESS_VIOLATION"});
    CHECK(rv::app::messageForOutcome(outcome(SessionState::Crashed, 0xC0001234u)).detail == "0xC0001234");

    // Failures to start, by reason.
    LaunchOutcome loader = outcome(SessionState::FailedToStart, 0xC0000135u);
    loader.failure = FailureReason::LoaderError;
    CHECK(rv::app::messageForOutcome(loader) ==
          LaunchMessage{std::string(lo::kFailedToStart), std::string(lo::kReasonLoaderError),
                        "0xC0000135 STATUS_DLL_NOT_FOUND"});
    LaunchOutcome early = outcome(SessionState::FailedToStart, 3);
    early.failure = FailureReason::EarlyExit;
    CHECK(rv::app::messageForOutcome(early) ==
          LaunchMessage{std::string(lo::kFailedToStart), std::string(lo::kReasonEarlyExit), "3 (error code)"});
    LaunchOutcome refused = outcome(SessionState::FailedToStart);
    refused.failure = FailureReason::StartFailed;
    refused.message = "RV_LAUNCH_ALLOWLIST does not list C:\\x\\emulator.exe";
    CHECK(rv::app::messageForOutcome(refused) ==
          LaunchMessage{std::string(lo::kFailedToStart), std::string(lo::kReasonStartFailed), refused.message});
    LaunchOutcome invalid = outcome(SessionState::FailedToStart);
    invalid.failure = FailureReason::InvalidRequest;
    invalid.message = "the executable does not exist";
    CHECK(rv::app::messageForOutcome(invalid).reason == lo::kReasonInvalidRequest);
    CHECK(rv::app::showsMessage(rv::app::messageForOutcome(invalid)));
    // The session's sentence names the launch, which the panel names itself: the detail is the error alone.
    LaunchOutcome guarded = outcome(SessionState::FailedToStart);
    guarded.failure = FailureReason::StartFailed;
    guarded.message = "Lanterna di Vetro with Emulatore di prova could not be started: validation-failed: not listed "
                      "[RV_LAUNCH_ALLOWLIST]";
    CHECK(rv::app::messageForOutcome(guarded).detail == "validation-failed: not listed [RV_LAUNCH_ALLOWLIST]");
    invalid.message = "Lanterna with Emulatore was not started: not-found: no such file [executable]";
    CHECK(rv::app::messageForOutcome(invalid).detail == "not-found: no such file [executable]");

    // The user returned while the emulator ran: left running by the user's choice, nothing to tell.
    LaunchOutcome confirmed = outcome(SessionState::Returned);
    confirmed.confirmedByUser = true;
    confirmed.processesRemained = true;
    const LaunchMessage left = rv::app::messageForOutcome(confirmed);
    CHECK(left == LaunchMessage{std::string(lo::kLeftRunning), std::string(lo::kReasonConfirmed), ""});
    CHECK_FALSE(rv::app::showsMessage(left));
    // Confirmed after the emulator was gone (UserConfirmReturn): a plain return.
    LaunchOutcome confirmedGone = outcome(SessionState::Returned, 0);
    confirmedGone.confirmedByUser = true;
    CHECK(rv::app::messageForOutcome(confirmedGone).code == lo::kReturned);
    // Left running because Retroverse quit: shown (the panel never is while quitting; the report keeps it).
    CHECK(rv::app::showsMessage(LaunchMessage{std::string(lo::kLeftRunning), std::string(lo::kReasonQuit), ""}));
}

TEST_CASE("app/launch_outcome: a plan's blocker becomes a code with what is missing", "[app][launch_outcome]") {
    const LaunchTarget t = target();
    CHECK(rv::app::messageForBlocker(LaunchBlocker{BlockerKind::RomMissing, "the ROM does not exist: C:/..."}, t,
                                     "Emulatore") ==
          LaunchMessage{std::string(lo::kRomMissing), "", "Lanterna di Vetro (Europa).gb"});
    // The token the configuration lacks, out of the plan's message.
    CHECK(rv::app::messageForBlocker(
              LaunchBlocker{BlockerKind::VariableMissing,
                            "unknown variable {var:core} (profile 'fake', arguments[1] \"{var:core}\", system \"gb\")"},
              t, "Emulatore") ==
          LaunchMessage{std::string(lo::kNotConfigured), std::string(lo::kReasonTemplate), "{var:core}"});
    CHECK(rv::app::messageForBlocker(LaunchBlocker{BlockerKind::TemplateError, "a working directory that is relative"},
                                     t, "Emulatore")
              .detail == "a working directory that is relative");
    CHECK(rv::app::messageForBlocker(LaunchBlocker{BlockerKind::ProgramRefused, "cmd.exe is refused"}, t, "E")
              .reason == lo::kReasonProgramRefused);
    CHECK(rv::app::messageForBlocker(LaunchBlocker{BlockerKind::InvalidRequest, "a batch file"}, t, "E") ==
          LaunchMessage{std::string(lo::kNotConfigured), std::string(lo::kReasonInvalidRequest), "a batch file"});
    // The emulator by its profile's name; a missing core by the plan's message.
    CHECK(rv::app::messageForBlocker(LaunchBlocker{BlockerKind::ExecutableNotFound, "nothing at ..."}, t,
                                     "Emulatore di prova") ==
          LaunchMessage{std::string(lo::kEmulatorMissing), "", "Emulatore di prova"});
    CHECK(rv::app::messageForBlocker(LaunchBlocker{BlockerKind::RequiredFileMissing, "a file is missing: core.dll"}, t,
                                     "E")
              .code == lo::kCoreMissing);
}

TEST_CASE("app/launch_outcome: tokens and exit codes as the panel shows them", "[app][launch_outcome]") {
    CHECK(rv::app::firstToken("unknown variable {var:core} (in {var:x})") == "{var:core}");
    CHECK(rv::app::firstToken("unknown token {romz}") == "{romz}");
    CHECK(rv::app::firstToken("no token here").empty());
    CHECK(rv::app::firstToken("an open { brace").empty());
    CHECK(rv::app::exitCodeName(0xC0000409u) == "0xC0000409 STATUS_STACK_BUFFER_OVERRUN");
    CHECK(rv::app::exitCodeName(0xDEADBEEFu) == "0xDEADBEEF");
}
