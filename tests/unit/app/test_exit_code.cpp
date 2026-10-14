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

RunSummary smokeRun(std::uint64_t highSeverityMessages) {
    RunSummary summary;
    summary.smoke = true;
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
    CHECK(rv::app::toString(ExitCode::RuntimeFailure) == "unexpected runtime failure");
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
