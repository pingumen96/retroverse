#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <glm/vec2.hpp>

#include <rv/devui/overlay_capture.hpp>

using Catch::Approx;
using rv::devui::CaptureFit;
using rv::devui::fitOverlayCapture;

TEST_CASE("devui/overlay_capture: a screenshot of the window's aspect holds the whole overlay, scaled",
          "[devui][overlay_capture]") {
    const CaptureFit fit = fitOverlayCapture(glm::vec2(1280.0f, 720.0f), glm::ivec2(1920, 1080));
    CHECK(fit.scale == Approx(1.5f));
    CHECK(fit.offset == glm::vec2(0.0f));

    // A screenshot smaller than the window shrinks it.
    const CaptureFit small = fitOverlayCapture(glm::vec2(1920.0f, 1080.0f), glm::ivec2(1280, 720));
    CHECK(small.scale == Approx(2.0f / 3.0f));
    CHECK(small.offset == glm::vec2(0.0f));
}

TEST_CASE("devui/overlay_capture: another aspect keeps the overlay's proportions and centers it",
          "[devui][overlay_capture]") {
    // Portrait screenshot of a 16:9 window: the width decides, the overlay sits in the middle band.
    const CaptureFit portrait = fitOverlayCapture(glm::vec2(1280.0f, 720.0f), glm::ivec2(1080, 1920));
    CHECK(portrait.scale == Approx(1080.0f / 1280.0f));
    CHECK(portrait.offset.x == 0.0f);
    // 720 units become 607.5 pixels: (1920 - 607.5) / 2 = 656.25, rounded to whole pixels.
    CHECK(portrait.offset.y == 656.0f);

    // A 21:9 window in a 16:9 screenshot: letterboxed above and below.
    const CaptureFit wide = fitOverlayCapture(glm::vec2(2560.0f, 1080.0f), glm::ivec2(1920, 1080));
    CHECK(wide.scale == Approx(0.75f));
    CHECK(wide.offset == glm::vec2(0.0f, 135.0f));

    // A 16:9 window in a 21:9 screenshot: pillarboxed left and right.
    const CaptureFit pillar = fitOverlayCapture(glm::vec2(1280.0f, 720.0f), glm::ivec2(2560, 1080));
    CHECK(pillar.scale == Approx(1.5f));
    CHECK(pillar.offset == glm::vec2(320.0f, 0.0f));
}

TEST_CASE("devui/overlay_capture: the fitted axis never gets a negative offset", "[devui][overlay_capture]") {
    // 1366 x 1920 / 1366 may round to a hair above 1920: the overlay still starts at the left edge.
    const CaptureFit fit = fitOverlayCapture(glm::vec2(1366.0f, 768.0f), glm::ivec2(1920, 1080));
    CHECK(fit.scale == Approx(1920.0f / 1366.0f));
    CHECK(fit.offset.x == 0.0f);
    CHECK(fit.offset.y >= 0.0f);
    CHECK(fit.offset.y <= 1.0f);
}

TEST_CASE("devui/overlay_capture: sizes without area leave the overlay as it is", "[devui][overlay_capture]") {
    const CaptureFit identity;
    for (const glm::vec2 window : {glm::vec2(0.0f, 720.0f), glm::vec2(1280.0f, -1.0f),
                                   glm::vec2(std::numeric_limits<float>::quiet_NaN(), 720.0f),
                                   glm::vec2(std::numeric_limits<float>::infinity(), 720.0f)}) {
        const CaptureFit fit = fitOverlayCapture(window, glm::ivec2(1920, 1080));
        CHECK(fit.scale == identity.scale);
        CHECK(fit.offset == identity.offset);
    }
    const CaptureFit noTarget = fitOverlayCapture(glm::vec2(1280.0f, 720.0f), glm::ivec2(0, 1080));
    CHECK(noTarget.scale == 1.0f);
    CHECK(noTarget.offset == glm::vec2(0.0f));
}
