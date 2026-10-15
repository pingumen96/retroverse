// The rendering self-checks of retroverse.exe (--self-check): the sRGB output path, the bloom chain and the FXAA pass,
// each run with the programs the views use.

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
