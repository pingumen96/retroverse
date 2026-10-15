#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

#include <glm/gtc/matrix_transform.hpp>

#include "support/temp_dir.hpp"
#include <rv/assets/glb_writer.hpp>
#include <rv/core/text.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/preview/crt_fixture.hpp>
#include <rv/preview/preview_controls.hpp>
#include <rv/preview/preview_overlay.hpp>

using Catch::Approx;
using rv::preview::OverlaySelection;
using rv::preview::PreviewOverlay;
using rv::preview::PreviewSubject;

namespace {

const std::filesystem::path kAssetRoot = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

PreviewSubject load(std::string_view text) {
    auto target = rv::preview::parsePreviewTarget(text);
    REQUIRE(target.has_value());
    return rv::preview::loadPreviewSubject(*target, kAssetRoot);
}

bool hasLabel(const PreviewOverlay& overlay, std::string_view text) {
    return std::any_of(overlay.labels.begin(), overlay.labels.end(),
                       [text](const rv::preview::SceneLabel& label) { return label.text == text; });
}

/// Every overlay off.
OverlaySelection none() {
    return OverlaySelection{false, false, false, false, false, false, false};
}

} // namespace

TEST_CASE("preview/overlay: the fixture shows its dimensions in centimeters and its named sockets",
          "[preview][overlay]") {
    rv::test::TempDir dir;
    const auto file = dir.file("crt_fixture.glb");
    REQUIRE(rv::assets::writeGlb(file, rv::preview::makeCrtFixture()).has_value());
    const PreviewSubject subject = load(rv::core::pathToUtf8(file));

    const PreviewOverlay overlay = rv::preview::buildPreviewOverlay(subject, OverlaySelection{});
    for (const char* label : {"W 71.0 cm", "H 61.0 cm", "D 56.0 cm", "ScreenSurface", "Anchor_TV_Stand",
                              "Anchor_ScreenLook", "ColliderProxy", "pivot", "1 m cube", "1.70 m"}) {
        INFO(label);
        CHECK(hasLabel(overlay, label));
    }
    // Socket axes are markers; the collision proxy's box shows its 12 sides (not the diagonals of its faces).
    CHECK(overlay.lines.markerVertices().size() == 2 * 3 * (4 + 1)); // the four sockets and the pivot
    OverlaySelection socketsOnly = none();
    socketsOnly.sockets = true;
    const PreviewOverlay sockets = rv::preview::buildPreviewOverlay(subject, socketsOnly);
    CHECK(sockets.lines.vertices().size() == 2 * (12 + 12)); // the screen's outline and the proxy's

    // Normals: one line per drawn vertex (the body's 24, the screen's 4), 3% of the largest extent long, pointing out
    // of the box.
    OverlaySelection normalsOnly = none();
    normalsOnly.normals = true;
    const PreviewOverlay normals = rv::preview::buildPreviewOverlay(subject, normalsOnly);
    REQUIRE(normals.lines.vertices().size() == 2 * 28);
    const glm::vec3 center = subject.bounds.center();
    for (std::size_t i = 0; i < normals.lines.vertices().size(); i += 2) {
        const glm::vec3 from = normals.lines.vertices()[i].position;
        const glm::vec3 along = normals.lines.vertices()[i + 1].position - from;
        CHECK(glm::length(along) == Approx(0.03f * 0.71f));
        CHECK(glm::dot(along, from - center) > 0.0f);
    }
    // Node bounds: one box per drawn node.
    OverlaySelection nodesOnly = none();
    nodesOnly.nodeBounds = true;
    CHECK(rv::preview::buildPreviewOverlay(subject, nodesOnly).lines.lineCount() == 2 * 12);
    CHECK(rv::preview::buildPreviewOverlay(subject, none()).lines.empty());
}

TEST_CASE("preview/overlay: the placeholder shows its box without dimensions", "[preview][overlay]") {
    rv::test::TempDir dir;
    const PreviewSubject missing = load(rv::core::pathToUtf8(dir.file("missing.glb")));
    REQUIRE(missing.placeholder);
    OverlaySelection boundsOnly = none();
    boundsOnly.bounds = true;
    const PreviewOverlay overlay = rv::preview::buildPreviewOverlay(missing, boundsOnly);
    CHECK(overlay.lines.lineCount() == 12);
    CHECK(overlay.labels.empty());
    CHECK(hasLabel(rv::preview::buildPreviewOverlay(missing, OverlaySelection{}), "pivot"));
}

TEST_CASE("preview/overlay: shelf slots show their axes without names", "[preview][overlay]") {
    OverlaySelection socketsOnly = none();
    socketsOnly.sockets = true;
    const PreviewSubject shelf = load("prefab:shelf");
    REQUIRE(shelf.prefab.has_value());
    const PreviewOverlay overlay = rv::preview::buildPreviewOverlay(shelf, socketsOnly);
    CHECK(overlay.labels.empty());
    CHECK(overlay.lines.markerVertices().size() == 2 * 3 * shelf.sockets.size());
    // The prefab's collider box is drawn as its proxy.
    CHECK(overlay.lines.vertices().size() == 2 * 12);
}

TEST_CASE("preview/overlay: the floor grid has 10 cm and 1 m lines around the subject and its references",
          "[preview][overlay]") {
    const rv::core::Aabb crt =
        rv::core::Aabb::fromMinMax(glm::vec3(-0.355f, 0.0f, -0.28f), glm::vec3(0.355f, 0.61f, 0.28f));
    const rv::core::Aabb cube = rv::preview::referenceCubeBounds(crt);
    CHECK(cube.size() == glm::vec3(1.0f));
    CHECK(cube.max.x == Approx(-0.355f - 0.25f));
    CHECK(cube.min.y == 0.0f);
    const float half = rv::preview::gridHalfExtent(crt);
    CHECK(half == 3.0f); // reaches the cube and the height marker beyond it, in whole meters
    CHECK(rv::preview::gridHalfExtent(rv::core::Aabb::fromMinMax(glm::vec3(-500.0f), glm::vec3(500.0f))) == 50.0f);

    PreviewSubject subject;
    subject.target = rv::preview::parsePreviewTarget("prefab:crt").value();
    subject.bounds = crt;
    OverlaySelection gridOnly = none();
    gridOnly.grid = true;
    const PreviewOverlay grid = rv::preview::buildPreviewOverlay(subject, gridOnly);
    // 61 lines each way every 10 cm out to 3 m, the 7 on whole meters drawn by the major grid with its +Y marker.
    CHECK(grid.lines.lineCount() == 2 * (61 - 7) + 1 + 2 * 7);
    CHECK(grid.labels.empty());
}

TEST_CASE("preview/overlay: centimeters and projections for the labels", "[preview][overlay]") {
    CHECK(rv::preview::formatCentimeters(0.71f) == "71.0 cm");
    CHECK(rv::preview::formatCentimeters(0.0124f) == "1.2 cm");
    CHECK(rv::preview::formatDimensions(glm::vec3(0.71f, 0.61f, 0.56f)) == "71.0 x 61.0 x 56.0 cm");

    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 2.0f, 0.1f, 100.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec2 size(1280.0f, 640.0f);
    const auto center = rv::preview::projectToViewport(projection * view, glm::vec3(0.0f), size);
    REQUIRE(center.has_value());
    CHECK(center->x == Approx(640.0f));
    CHECK(center->y == Approx(320.0f));
    // Up in the world is up on screen: a smaller y.
    const auto above = rv::preview::projectToViewport(projection * view, glm::vec3(0.0f, 1.0f, 0.0f), size);
    REQUIRE(above.has_value());
    CHECK(above->y < center->y);
    CHECK_FALSE(rv::preview::projectToViewport(projection * view, glm::vec3(0.0f, 0.0f, 10.0f), size).has_value());
    CHECK_FALSE(rv::preview::projectToViewport(projection * view, glm::vec3(100.0f, 0.0f, 0.0f), size).has_value());
}

TEST_CASE("preview/controls: drags, the wheel and the gamepad drive the orbit", "[preview][controls]") {
    using rv::input::GamepadAxis;
    using rv::input::MouseButton;
    rv::input::RawInputFrame frame;
    frame.mouseDelta = glm::vec2(12.0f, -4.0f);
    frame.mouseWheel = 2.0f;
    // Motion alone (no drag) moves nothing; the wheel zooms.
    rv::scene::OrbitInput orbit = rv::preview::orbitInputFrom(frame);
    CHECK(orbit.orbitPixels == glm::vec2(0.0f));
    CHECK(orbit.panPixels == glm::vec2(0.0f));
    CHECK(orbit.zoomNotches == 2.0f);

    frame.mouseButtonsDown.set(rv::input::toIndex(MouseButton::Left));
    CHECK(rv::preview::orbitInputFrom(frame).orbitPixels == glm::vec2(12.0f, -4.0f));
    frame.mouseButtonsDown.reset();
    for (const MouseButton pan : {MouseButton::Middle, MouseButton::Right}) {
        frame.mouseButtonsDown.reset();
        frame.mouseButtonsDown.set(rv::input::toIndex(pan));
        orbit = rv::preview::orbitInputFrom(frame);
        CHECK(orbit.panPixels == glm::vec2(12.0f, -4.0f));
        CHECK(orbit.orbitPixels == glm::vec2(0.0f));
    }

    // Gamepad: ignored until connected; sticks with a deadzone; triggers zoom.
    rv::input::RawInputFrame pad;
    pad.gamepadAxes[rv::input::toIndex(GamepadAxis::RightX)] = 1.0f;
    pad.gamepadAxes[rv::input::toIndex(GamepadAxis::LeftY)] = 0.1f; // inside the deadzone
    pad.gamepadAxes[rv::input::toIndex(GamepadAxis::RightTrigger)] = 0.75f;
    pad.gamepadAxes[rv::input::toIndex(GamepadAxis::LeftTrigger)] = 0.25f;
    CHECK(rv::preview::orbitInputFrom(pad).orbitRate == glm::vec2(0.0f));
    pad.gamepadConnected = true;
    orbit = rv::preview::orbitInputFrom(pad);
    CHECK(orbit.orbitRate.x == Approx(1.0f));
    CHECK(orbit.panRate == glm::vec2(0.0f));
    CHECK(orbit.zoomRate == Approx(0.5f));
    pad.gamepadAxes[rv::input::toIndex(GamepadAxis::RightX)] = 0.575f; // halfway past the deadzone
    CHECK(rv::preview::orbitInputFrom(pad).orbitRate.x == Approx(0.5f));

    // Whatever moves the camera takes it over from the automatic framing.
    CHECK(rv::preview::movesCamera(orbit));
    CHECK_FALSE(rv::preview::movesCamera(rv::scene::OrbitInput{}));
    rv::scene::OrbitInput wheel;
    wheel.zoomNotches = -1.0f;
    CHECK(rv::preview::movesCamera(wheel));
}

TEST_CASE("preview/controls: the shortcuts frame, rotate and pick a view", "[preview][controls]") {
    using rv::input::Key;
    rv::input::RawInputFrame frame;
    CHECK_FALSE(rv::preview::previewShortcutsFrom(frame).frameSubject);
    frame.keysPressed.set(rv::input::toIndex(Key::F));
    frame.keysPressed.set(rv::input::toIndex(Key::R));
    frame.keysPressed.set(rv::input::toIndex(Key::Digit3));
    const rv::preview::PreviewShortcuts shortcuts = rv::preview::previewShortcutsFrom(frame);
    CHECK(shortcuts.frameSubject);
    CHECK(shortcuts.toggleAutoRotate);
    CHECK(shortcuts.viewMode == std::optional<rv::preview::ViewMode>(rv::preview::ViewMode::Normals));
}
