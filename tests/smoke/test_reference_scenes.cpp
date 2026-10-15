// The reference scenes of assets/style_tests/reference_scenes rendered by retroverse.exe: every run clean (exit code 0,
// no OpenGL error, the assets of its configuration), frame statistics within bounds, a screenshot of the requested size
// that shows something.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include <rv/screenshot_diff/screenshot_diff.hpp>

#include "smoke_support.hpp"

using namespace rv::smoke;

namespace {

/// Screenshots of the scene runs: smaller than the default, which the golden comparison covers.
constexpr glm::ivec2 kShotSize{960, 540};
/// The renderer's instance buffer starts with this many matrices per region (gl::SceneRenderer).
constexpr std::size_t kInitialInstanceCapacity = 1024;

/// Renders `scene` for 5 frames with a screenshot of the last one; checks the run and the screenshot.
AppRun renderScene(const std::filesystem::path& caseDir, const std::string& scene) {
    const std::filesystem::path shot = caseDir / (scene + ".png");
    AppRun run = runApp(retroverseExe(), caseDir, scene,
                        {"--scene", scene, "--smoke", "--frames", "5", "--screenshot", arg(shot), "--screenshot-size",
                         "960x540"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    requireScreenshot(run, shot, kShotSize);
    INFO(run.describe());
    REQUIRE(run.report->lastView.has_value());
    return run;
}

} // namespace

TEST_CASE("scenes/m0_testroom renders cleanly within its draw call bound", "[gpu][scene]") {
    const AppRun run = renderScene(freshCaseDir("scene_m0_testroom"), "m0_testroom");
    const rv::gl::FrameStats& view = *run.report->lastView;
    INFO(run.describe());
    // 231 items in 129 draw calls at M0: the 96 cartridge labels are a batch each (one cover each, BACKLOG "Texture
    // array (or atlas) for cartridge labels"), the rest instanced. Without instancing every visible item would be a
    // draw call.
    CHECK(view.items >= 200);
    CHECK(view.drawCalls >= 1);
    CHECK(view.drawCalls <= 160);
    CHECK(view.drawCalls < view.items - view.culledItems);
    CHECK(view.triangles > 10000);
    // The ceiling key light casts the room's shadows; the depth pre-pass draws the opaque batches.
    CHECK(view.shadowDrawCalls > 0);
    CHECK(view.depthDrawCalls > 0);
    CHECK(view.depthDrawCalls <= view.drawCalls);
}

TEST_CASE("scenes/material_test renders its sphere grid cleanly", "[gpu][scene]") {
    const AppRun run = renderScene(freshCaseDir("scene_material_test"), "material_test");
    const rv::gl::FrameStats& view = *run.report->lastView;
    INFO(run.describe());
    // 25 spheres of 25 inline materials and the floor: a batch per material at most.
    CHECK(view.items == 26);
    CHECK(view.drawCalls >= 1);
    CHECK(view.drawCalls <= 26);
}

TEST_CASE("scenes/instancing_stress draws 1000 boxes in at most 4 batches", "[gpu][scene]") {
    const AppRun run = renderScene(freshCaseDir("scene_instancing_stress"), "instancing_stress");
    const rv::gl::FrameStats& view = *run.report->lastView;
    INFO(run.describe());
    CHECK(view.batches <= 4);
    CHECK(view.drawCalls <= 4);
    CHECK(view.instances >= 1000);
    // The sun casts: its shadow map draws the 1000 boxes too, about 2000 instances per view in all, so the instance
    // buffer has grown on the first frame.
    CHECK(view.shadowInstances >= 1000);
    CHECK(run.report->instanceCapacity >= view.instances + view.shadowInstances);
}

TEST_CASE("scenes/instancing_growth grows the instance buffer mid-run and draws what a grown start draws",
          "[gpu][scene]") {
    const std::filesystem::path dir = freshCaseDir("scene_instancing_growth");
    const std::string script = arg(sourceDir() / "tests/input_scripts/turn_around.json");

    // The start view needs fewer instances than the buffer starts with: one frame does not grow it.
    const AppRun first =
        runApp(retroverseExe(), dir, "first_frame", {"--scene", "instancing_growth", "--smoke", "--frames", "1"});
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    {
        INFO(first.describe());
        REQUIRE(first.report->lastView.has_value());
        CHECK(first.report->lastView->instances < kInitialInstanceCapacity);
        CHECK(first.report->instanceCapacity == kInitialInstanceCapacity);
    }

    // Turning around brings the field of 2304 boxes into view: the buffer grows mid-run and the meshes, attached to the
    // first buffer since frame 0, are attached to the new one.
    const std::filesystem::path turnedShot = dir / "turned.png";
    const AppRun turned = runApp(retroverseExe(), dir, "turned",
                                 {"--scene", "instancing_growth", "--smoke", "--frames", "10", "--fixed-dt", "0.016667",
                                  "--input-script", script, "--screenshot", arg(turnedShot), "--screenshot-size",
                                  "960x540"});
    requireCleanRun(turned);
    requireScreenshot(turned, turnedShot, kShotSize);
    INFO(turned.describe());
    REQUIRE(turned.report->lastView.has_value());
    REQUIRE(turned.report->finalCamera.has_value());
    CHECK(turned.report->lastView->instances > kInitialInstanceCapacity);
    CHECK(turned.report->instanceCapacity >= turned.report->lastView->instances);

    // A run that starts where the turn ends grows the buffer before its first draw: the frame after the growth mid-run
    // must look the same, every box drawn from the re-attached buffer.
    const std::filesystem::path spawnedShot = dir / "spawned.png";
    const AppRun spawned =
        runApp(retroverseExe(), dir, "spawned",
               {"--scene", "instancing_growth", "--smoke", "--frames", "1", "--spawn",
                spawnArgument(*turned.report->finalCamera), "--screenshot", arg(spawnedShot), "--screenshot-size",
                "960x540"});
    requireCleanRun(spawned);
    requireScreenshot(spawned, spawnedShot, kShotSize);
    const auto comparison =
        rv::screenshot_diff::compareFiles(turnedShot, spawnedShot, rv::screenshot_diff::Tolerance{},
                                          dir / "turned_vs_spawned_heatmap.png");
    REQUIRE(comparison.has_value());
    INFO(comparison->describe(rv::screenshot_diff::Tolerance{}));
    CHECK(comparison->matches);
}
