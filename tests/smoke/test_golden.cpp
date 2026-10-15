// Golden G01 (docs/testing/gate-a.md): the test room from its scene camera, compared with
// assets/style_tests/screenshots/m0_testroom_golden.png within the tolerance of retroverse-screenshot-diff (6/255 per
// channel, at most 0.5% of the pixels). Only on the reference machine: another GPU or driver moves pixels by more than
// dithering and filtering allow, so the comparison is skipped when GL_RENDERER or GL_VERSION (which names the driver)
// differ from the golden's metadata.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/screenshot_diff/screenshot_diff.hpp>

#include "smoke_support.hpp"

using namespace rv::smoke;

TEST_CASE("golden/G01: the test room matches its golden image within the tolerance", "[gpu][golden]") {
    const std::filesystem::path goldenDir = sourceDir() / "assets/style_tests/screenshots";
    const auto metadata = rv::core::readJsonFile(goldenDir / "m0_testroom_golden.json");
    REQUIRE(metadata.has_value());
    const std::string renderer = metadata->at("gpu").at("renderer").get<std::string>();
    const std::string glVersion = metadata->at("gpu").at("glVersion").get<std::string>();
    const int frames = metadata->at("run").at("frames").get<int>();
    const glm::ivec2 size(metadata->at("image").at("width").get<int>(), metadata->at("image").at("height").get<int>());
    const std::filesystem::path golden =
        goldenDir / rv::core::pathFromUtf8(metadata->at("image").at("file").get<std::string>());

    // The golden's run: the scene camera's last frame of a 30-frame run at 1920x1080.
    const std::filesystem::path dir = freshCaseDir("golden_g01");
    const std::filesystem::path shot = dir / "m0_testroom.png";
    const AppRun run = runApp(retroverseExe(), dir, "m0_testroom",
                              {"--scene", "m0_testroom", "--smoke", "--frames", std::to_string(frames), "--screenshot",
                               arg(shot), "--screenshot-size", std::to_string(size.x) + "x" + std::to_string(size.y)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    requireScreenshot(run, shot, size);
    // comparison.txt: the verdict for scripts/gate.ps1's summary.
    const std::filesystem::path verdictFile = dir / "comparison.txt";
    const rv::app::RunReportContext& context = *run.report->context;
    if (context.renderer != renderer || context.version != glVersion) {
        const std::string reason = "golden G01 was rendered on " + renderer + " (" + glVersion +
                                   "), this machine has " + context.renderer + " (" + context.version +
                                   "): the comparison runs on the reference machine only "
                                   "(docs/testing/reference-hardware.md)";
        REQUIRE(rv::core::writeTextFileAtomic(verdictFile, "skipped: " + reason + "\n").has_value());
        SKIP(reason);
    }

    const rv::screenshot_diff::Tolerance tolerance;
    const std::filesystem::path heatmap = dir / "m0_testroom_heatmap.png";
    const auto comparison = rv::screenshot_diff::compareFiles(golden, shot, tolerance, heatmap);
    REQUIRE(comparison.has_value());
    const std::string verdict = comparison->describe(tolerance);
    REQUIRE(rv::core::writeTextFileAtomic(verdictFile, verdict + "\n").has_value());
    INFO(verdict);
    INFO("heatmap of the differences: " << rv::core::pathToUtf8(heatmap));
    CHECK(comparison->matches);
}
