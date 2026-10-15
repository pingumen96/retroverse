// The developer overlay drawn into screenshots and the asset preview tool: developer builds only (RV_DEV_UI), Release
// builds compile neither.

#include <catch2/catch_test_macros.hpp>

#include "image_checks.hpp"
#include "smoke_support.hpp"

#if RV_SMOKE_DEVELOPER_BUILD

using namespace rv::smoke;

namespace {

/// Dear ImGui's dark theme draws the title bar of the focused window in sRGB (41, 74, 122): the overlay's colors are
/// sRGB already and must reach the screenshot unchanged, neither washed out by a second encoding nor darkened.
constexpr glm::ivec3 kFocusedTitleBar{41, 74, 122};
/// Smallest area of that color in an overlay screenshot: a title bar a few hundred pixels long.
constexpr std::size_t kMinTitleBarPixels = 1000;

} // namespace

TEST_CASE("developer-overlay: drawn into the screenshot with its theme's exact colors, absent without it",
          "[gpu][devui]") {
    const std::filesystem::path dir = freshCaseDir("developer_overlay");
    // The panels settle over the first three frames.
    const std::filesystem::path withUi = dir / "with_overlay.png";
    const AppRun shown = runApp(retroverseExe(), dir, "with_overlay",
                                {"--devui-open", "--screenshot-include-ui", "--smoke", "--frames", "5",
                                 "--screenshot", arg(withUi)});
    skipWithoutOpenGl(shown);
    requireCleanRun(shown);
    const rv::assets::ImageData overlay = requireScreenshot(shown, withUi, glm::ivec2(1920, 1080));
    const std::size_t titleBar = countColor(overlay, kFocusedTitleBar);
    INFO("pixels of the focused title bar's color with the overlay: " << titleBar);
    CHECK(titleBar >= kMinTitleBarPixels);

    // The same run without --screenshot-include-ui: the overlay is open in the window, the screenshot shows the scene
    // alone.
    const std::filesystem::path sceneOnly = dir / "scene_only.png";
    const AppRun hidden = runApp(retroverseExe(), dir, "scene_only",
                                 {"--devui-open", "--smoke", "--frames", "5", "--screenshot", arg(sceneOnly)});
    requireCleanRun(hidden);
    const rv::assets::ImageData scene = requireScreenshot(hidden, sceneOnly, glm::ivec2(1920, 1080));
    CHECK(countColor(scene, kFocusedTitleBar) == 0);
}

TEST_CASE("asset-preview: renders its own CRT fixture cleanly", "[gpu][devui]") {
    const std::filesystem::path dir = freshCaseDir("asset_preview");
    const auto preview = assetPreviewExe();
    REQUIRE(preview.has_value());
    const std::filesystem::path fixture = dir / "crt_fixture.glb";
    // --write-fixture exits before the host starts: no window, no run report.
    const AppRun written = runCommand(*preview, dir, "write_fixture", {"--write-fixture", arg(fixture)});
    INFO(written.describe());
    REQUIRE(written.exitCode == 0);
    REQUIRE(std::filesystem::exists(fixture));

    const std::filesystem::path shot = dir / "crt_fixture.png";
    const AppRun run = runApp(*preview, dir, "preview",
                              {arg(fixture), "--smoke", "--frames", "5", "--screenshot", arg(shot),
                               "--screenshot-size", "960x540"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    requireScreenshot(run, shot, glm::ivec2(960, 540));
    INFO(run.describe());
    REQUIRE(run.report->lastView.has_value());
    CHECK(run.report->application == "Retroverse Asset Preview");
    CHECK(run.report->lastView->items > 0);
    CHECK(run.report->lastView->drawCalls > 0);
}

#endif
