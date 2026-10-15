// The rendering self-checks of retroverse.exe (--self-check): the sRGB output path, the bloom chain and the FXAA pass,
// each run with the programs the views use; and --gl-selftest, which proves that OpenGL errors reach the log and the
// run report.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "smoke_support.hpp"

using namespace rv::smoke;

namespace {

void runSelfCheck(const std::string& name, rv::app::SelfCheck check) {
    const std::filesystem::path dir = freshCaseDir("self_check_" + name);
    const AppRun run = runApp(retroverseExe(), dir, name, {"--self-check", name, "--smoke", "--frames", "1"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->selfCheck.has_value());
    INFO(run.report->selfCheck->summary);
    CHECK(run.report->selfCheck->check == check);
    CHECK(run.report->selfCheck->passed);
}

} // namespace

TEST_CASE("self-check/srgb: linear 0.5 reaches the sRGB output as 188", "[gpu][self_check]") {
    runSelfCheck("srgb", rv::app::SelfCheck::Srgb);
}

TEST_CASE("self-check/bloom: the bloom chain keeps lit surfaces out, energy and steady fireflies",
          "[gpu][self_check]") {
    runSelfCheck("bloom", rv::app::SelfCheck::Bloom);
}

TEST_CASE("self-check/fxaa: a slanted edge is blended and flat areas are copied exactly", "[gpu][self_check]") {
    runSelfCheck("fxaa", rv::app::SelfCheck::Fxaa);
}

TEST_CASE("gl-selftest: the provoked GL_INVALID_ENUM is reported and the run stays clean", "[gpu][gl_selftest]") {
    const std::filesystem::path dir = freshCaseDir("gl_selftest");
    const AppRun run = runApp(retroverseExe(), dir, "gl_selftest",
                              {"--scene", "material_test", "--gl-selftest", "--smoke", "--frames", "1"});
    skipWithoutOpenGl(run);
    // The --smoke run passes, and its report agrees: the provoked messages are the self-test's, none other.
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->glSelfTest.has_value());
    const rv::app::SelfTestOutcome& selfTest = *run.report->glSelfTest;
    CHECK(selfTest.errorFlagRaised);
    CHECK(selfTest.debugOutputActive == buildConfig().glDebugOutput);
    if (buildConfig().glDebugOutput) {
        // The debug callback reported the error, and the run's counts include what it reported.
        REQUIRE(run.report->debugMessages.has_value());
        const rv::app::DebugMessageCounts& counted = *run.report->debugMessages;
        CHECK(selfTest.messagesCaptured >= 1);
        CHECK(counted.high + counted.medium + counted.low >= selfTest.messagesCaptured);
    }
}
