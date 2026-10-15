#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

#include <glm/vec2.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/overlay_policy.hpp>

using rv::app::CommandLineOptions;
using rv::app::overlayCaptureSize;
using rv::app::overlayRequired;

namespace {

/// --exit-after-frames --frames `frames`.
CommandLineOptions frameLimited(std::uint32_t frames) {
    CommandLineOptions options;
    options.exitAfterFrames = true;
    options.frameLimit = frames;
    return options;
}

} // namespace

TEST_CASE("app/overlay_policy: frame-limited runs that ask for the overlay need it", "[app][overlay_policy]") {
    CommandLineOptions open = frameLimited(30);
    open.devUiOpen = true;
    CHECK(overlayRequired(open));

    CommandLineOptions inScreenshots = frameLimited(30);
    inScreenshots.screenshotIncludeUi = true;
    CHECK(overlayRequired(inScreenshots));

    // A frame-limited run that never mentions the overlay does not depend on it.
    CHECK_FALSE(overlayRequired(frameLimited(30)));

    // Interactive runs go on without it.
    CommandLineOptions interactive;
    interactive.devUiOpen = true;
    interactive.screenshotIncludeUi = true;
    CHECK_FALSE(overlayRequired(interactive));
}

TEST_CASE("app/overlay_policy: the overlay is drawn into the screenshots of --screenshot-include-ui only",
          "[app][overlay_policy]") {
    CommandLineOptions options = frameLimited(30);
    options.screenshot = "ui.png";
    options.screenshotWidth = 1080;
    options.screenshotHeight = 1920;

    // Without --screenshot-include-ui no frame draws it into a screenshot.
    CHECK_FALSE(overlayCaptureSize(options, 29, false));
    CHECK_FALSE(overlayCaptureSize(options, 5, true));

    options.screenshotIncludeUi = true;
    // The last frame (29, counted from 0) writes --screenshot; earlier frames take none unless F12 asks.
    CHECK(overlayCaptureSize(options, 29, false) == std::optional<glm::ivec2>(glm::ivec2(1080, 1920)));
    CHECK_FALSE(overlayCaptureSize(options, 28, false));
    CHECK(overlayCaptureSize(options, 5, true) == std::optional<glm::ivec2>(glm::ivec2(1080, 1920)));

    // Without --screenshot, the last frame is an ordinary frame.
    options.screenshot.reset();
    CHECK_FALSE(overlayCaptureSize(options, 29, false));

    // Interactive runs have no last frame: only F12 screenshots.
    CommandLineOptions interactive;
    interactive.screenshotIncludeUi = true;
    CHECK_FALSE(overlayCaptureSize(interactive, 0, false));
    CHECK(overlayCaptureSize(interactive, 1234, true) ==
          std::optional<glm::ivec2>(glm::ivec2(CommandLineOptions::kDefaultScreenshotWidth,
                                               CommandLineOptions::kDefaultScreenshotHeight)));
}
