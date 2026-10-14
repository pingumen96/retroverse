#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rv/app/exit_code.hpp>

using rv::app::exitCodeForCompletedRun;
using rv::app::ExitCode;
using rv::app::RunSummary;
using rv::app::SelfTestOutcome;

namespace {

SelfTestOutcome passingSelfTest(std::uint64_t highSeverity = 1) {
    SelfTestOutcome outcome;
    outcome.errorFlagRaised = true;
    outcome.debugOutputActive = true;
    outcome.messagesCaptured = highSeverity > 0 ? highSeverity : 1;
    outcome.highSeverityCaptured = highSeverity;
    return outcome;
}

/// A smoke run that rendered all its frames.
RunSummary smokeRun(std::uint64_t highSeverityMessages) {
    RunSummary summary;
    summary.smoke = true;
    summary.frameLimit = 3;
    summary.framesRendered = 3;
    summary.highSeverityGlMessages = highSeverityMessages;
    return summary;
}

} // namespace

TEST_CASE("app/exit_code: numeric values are part of the command line contract", "[app][exit_code]") {
    CHECK(rv::app::toInt(ExitCode::Ok) == 0);
    CHECK(rv::app::toInt(ExitCode::SmokeCheckFailed) == 1);
    CHECK(rv::app::toInt(ExitCode::InitializationFailed) == 2);
    CHECK(rv::app::toInt(ExitCode::RuntimeFailure) == 3);
    CHECK(rv::app::toString(ExitCode::Ok) == "ok");
    CHECK(rv::app::toString(ExitCode::SmokeCheckFailed) == "smoke check failed");
    CHECK(rv::app::toString(ExitCode::InitializationFailed) == "initialization failed");
    CHECK(rv::app::toString(ExitCode::RuntimeFailure) == "runtime failure");
}

TEST_CASE("app/exit_code: runs that are not smoke tests only log GL messages", "[app][exit_code]") {
    RunSummary summary;
    summary.smoke = false;
    summary.highSeverityGlMessages = 12;
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::Ok);

    SelfTestOutcome failed;
    failed.errorFlagRaised = false;
    summary.selfTest = failed;
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::Ok);
}

TEST_CASE("app/exit_code: a smoke run that stops before its frame limit fails", "[app][exit_code]") {
    RunSummary summary = smokeRun(0);
    summary.frameLimit = 5;
    summary.framesRendered = 5;
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::Ok);

    // Escape, a window close or Ctrl+C ended the run early: it proves nothing.
    summary.framesRendered = 4;
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::SmokeCheckFailed);
    summary.framesRendered = 0;
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::SmokeCheckFailed);

    // A passing self-test that excuses every GL message does not excuse the missing frames.
    summary.framesRendered = 1;
    summary.highSeverityGlMessages = 1;
    summary.selfTest = passingSelfTest(1);
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::SmokeCheckFailed);

    // The user may end an --exit-after-frames run at any time.
    RunSummary exitAfterFrames;
    exitAfterFrames.smoke = false;
    exitAfterFrames.frameLimit = 120;
    exitAfterFrames.framesRendered = 10;
    CHECK(exitCodeForCompletedRun(exitAfterFrames) == ExitCode::Ok);
}

TEST_CASE("app/exit_code: a smoke run fails on any High-severity GL message", "[app][exit_code]") {
    CHECK(exitCodeForCompletedRun(smokeRun(0)) == ExitCode::Ok);
    CHECK(exitCodeForCompletedRun(smokeRun(1)) == ExitCode::SmokeCheckFailed);
    CHECK(exitCodeForCompletedRun(smokeRun(40)) == ExitCode::SmokeCheckFailed);
}

TEST_CASE("app/exit_code: messages provoked by --gl-selftest do not fail a smoke run", "[app][exit_code]") {
    RunSummary summary = smokeRun(1);
    summary.selfTest = passingSelfTest(1);
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::Ok);

    summary.highSeverityGlMessages = 2; // one real error on top of the provoked one
    CHECK(exitCodeForCompletedRun(summary) == ExitCode::SmokeCheckFailed);

    // A driver may report the provoked error with a lower severity: then no High message is excused.
    RunSummary medium = smokeRun(0);
    medium.selfTest = passingSelfTest(0);
    CHECK(exitCodeForCompletedRun(medium) == ExitCode::Ok);
    medium.highSeverityGlMessages = 1;
    CHECK(exitCodeForCompletedRun(medium) == ExitCode::SmokeCheckFailed);
}

TEST_CASE("app/exit_code: a failed self-test fails a smoke run", "[app][exit_code]") {
    SECTION("the error flag was not raised") {
        RunSummary summary = smokeRun(0);
        SelfTestOutcome outcome = passingSelfTest(0);
        outcome.errorFlagRaised = false;
        outcome.messagesCaptured = 0;
        summary.selfTest = outcome;
        CHECK_FALSE(outcome.passed());
        CHECK(exitCodeForCompletedRun(summary) == ExitCode::SmokeCheckFailed);
    }
    SECTION("debug output is active but the callback stayed silent") {
        RunSummary summary = smokeRun(0);
        SelfTestOutcome outcome;
        outcome.errorFlagRaised = true;
        outcome.debugOutputActive = true;
        outcome.messagesCaptured = 0;
        summary.selfTest = outcome;
        CHECK_FALSE(outcome.passed());
        CHECK(exitCodeForCompletedRun(summary) == ExitCode::SmokeCheckFailed);
    }
    SECTION("without debug output only the error flag can be checked") {
        RunSummary summary = smokeRun(0);
        SelfTestOutcome outcome;
        outcome.errorFlagRaised = true;
        outcome.debugOutputActive = false;
        summary.selfTest = outcome;
        CHECK(outcome.passed());
        CHECK(exitCodeForCompletedRun(summary) == ExitCode::Ok);
    }
}

TEST_CASE("app/exit_code: a failure reported by the application logic is a runtime failure", "[app][exit_code]") {
    // --screenshot that was not written, scene shader that never built: whatever the kind of run.
    RunSummary smoke = smokeRun(0);
    smoke.logicFailed = true;
    CHECK(exitCodeForCompletedRun(smoke) == ExitCode::RuntimeFailure);

    RunSummary exitAfterFrames;
    exitAfterFrames.frameLimit = 5;
    exitAfterFrames.framesRendered = 5;
    exitAfterFrames.logicFailed = true;
    CHECK(exitCodeForCompletedRun(exitAfterFrames) == ExitCode::RuntimeFailure);

    // An --exit-after-frames run passes when the user ends it early, unless the logic reports the early end as a
    // failure: a --screenshot of the last frame was then never written.
    RunSummary interrupted;
    interrupted.frameLimit = 100;
    interrupted.framesRendered = 37;
    CHECK(exitCodeForCompletedRun(interrupted) == ExitCode::Ok);
    interrupted.logicFailed = true;
    CHECK(exitCodeForCompletedRun(interrupted) == ExitCode::RuntimeFailure);

    RunSummary interactive;
    interactive.logicFailed = true;
    CHECK(exitCodeForCompletedRun(interactive) == ExitCode::RuntimeFailure);

    // The logic failure wins over smoke-check failures: it is the more specific report.
    RunSummary both = smokeRun(3);
    both.framesRendered = 1;
    both.logicFailed = true;
    CHECK(exitCodeForCompletedRun(both) == ExitCode::RuntimeFailure);
}
