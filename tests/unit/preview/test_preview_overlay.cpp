#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// Where the label `text` is placed in the scene.
glm::vec3 labelPosition(const PreviewOverlay& overlay, std::string_view text) {
    const auto found = std::find_if(overlay.labels.begin(), overlay.labels.end(),
                                    [text](const rv::preview::SceneLabel& label) { return label.text == text; });
    REQUIRE(found != overlay.labels.end());
    return found->position;
}

/// `value` is one of `candidates` (within 0.01 mm).
bool oneOf(float value, std::initializer_list<float> candidates) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [value](float candidate) { return std::abs(value - candidate) < 1e-5f; });
}

/// Rectangles (x0, y0, x1, y1) that share some area.
bool overlap(const glm::vec4& a, const glm::vec4& b) {
    return a.x < b.z && b.x < a.z && a.y < b.w && b.y < a.w;
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
    // The dimensions sit at the middle of three edges of the bounds, the names at their sockets' origins.
    const rv::core::Aabb& bounds = subject.bounds;
    const glm::vec3 center = bounds.center();
    CHECK(rv::core::nearlyEqual(labelPosition(overlay, "W 71.0 cm"), glm::vec3(center.x, bounds.min.y, bounds.max.z)));
    CHECK(rv::core::nearlyEqual(labelPosition(overlay, "H 61.0 cm"), glm::vec3(bounds.max.x, center.y, bounds.max.z)));
    CHECK(rv::core::nearlyEqual(labelPosition(overlay, "D 56.0 cm"), glm::vec3(bounds.max.x, bounds.min.y, center.z)));
    CHECK(rv::core::nearlyEqual(labelPosition(overlay, "ScreenSurface"), glm::vec3(0.0f, 0.33f, 0.28f)));
    CHECK(rv::core::nearlyEqual(labelPosition(overlay, "Anchor_ScreenLook"), glm::vec3(0.0f, 0.33f, 0.28f + 0.9f)));
    CHECK(rv::core::nearlyEqual(labelPosition(overlay, "Anchor_TV_Stand"), glm::vec3(0.0f)));
    CHECK(labelPosition(overlay, "pivot") == glm::vec3(0.0f));
    const rv::core::Aabb cube = rv::preview::referenceCubeBounds(bounds);
    CHECK(
        rv::core::nearlyEqual(labelPosition(overlay, "1 m cube"), glm::vec3(cube.center().x, cube.max.y, cube.max.z)));
    CHECK(labelPosition(overlay, "1.70 m").y == Approx(1.7f));
    CHECK(labelPosition(overlay, "1.70 m").x < cube.min.x); // the marker stands left of the cube

    // Socket axes are markers; the collision proxy's box shows its 12 sides (not the diagonals of its faces).
    CHECK(overlay.lines.markerVertices().size() == 2 * 3 * (4 + 1)); // the four sockets and the pivot
    OverlaySelection socketsOnly = none();
    socketsOnly.sockets = true;
    const PreviewOverlay sockets = rv::preview::buildPreviewOverlay(subject, socketsOnly);
    REQUIRE(sockets.lines.vertices().size() == 2 * (12 + 12)); // the screen's outline and the proxy's
    // The screen's outline is the 48 x 36 cm rectangle on the front of the bounds, centered 33 cm up.
    for (std::size_t i = 0; i < 2 * 12; ++i) {
        const glm::vec3 corner = sockets.lines.vertices()[i].position;
        INFO("screen outline vertex " << i);
        CHECK(oneOf(corner.x, {-0.24f, 0.24f}));
        CHECK(oneOf(corner.y, {0.33f - 0.18f, 0.33f + 0.18f}));
        CHECK(corner.z == Approx(0.28f));
    }
    // The proxy's outline runs along the edges of the body: 71 x 61 cm, 55 cm deep from the back of the bounds.
    for (std::size_t i = 2 * 12; i < 2 * 24; ++i) {
        const glm::vec3 corner = sockets.lines.vertices()[i].position;
        INFO("proxy outline vertex " << i);
        CHECK(oneOf(corner.x, {-0.355f, 0.355f}));
        CHECK(oneOf(corner.y, {0.0f, 0.61f}));
        CHECK(oneOf(corner.z, {-0.28f, 0.27f}));
    }

    // Normals: one line per drawn vertex (the body's 24, the screen's 4), 3% of the largest extent long, pointing out
    // of the box.
    OverlaySelection normalsOnly = none();
    normalsOnly.normals = true;
    const PreviewOverlay normals = rv::preview::buildPreviewOverlay(subject, normalsOnly);
    REQUIRE(normals.lines.vertices().size() == 2 * 28);
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

TEST_CASE("preview/overlay: slots and levels show their axes and volumes without names", "[preview][overlay]") {
    OverlaySelection socketsOnly = none();
    socketsOnly.sockets = true;
    for (const std::string_view target : {"prefab:shelf", "prefab:display_case", "prefab:wall_shelf"}) {
        INFO(target);
        const PreviewSubject subject = load(target);
        REQUIRE(subject.prefab.has_value());
        REQUIRE_FALSE(subject.sockets.empty());
        const PreviewOverlay overlay = rv::preview::buildPreviewOverlay(subject, socketsOnly);
        CHECK(overlay.labels.empty());
        CHECK(overlay.lines.markerVertices().size() == 2 * 3 * subject.sockets.size());
        // Each slot's and each level's volume as a box in its color, then the prefab's collider box as its proxy.
        REQUIRE(overlay.lines.vertices().size() == 2 * 12 * (subject.sockets.size() + 1));
        std::size_t slots = 0;
        std::size_t levels = 0;
        for (const rv::geometry::Socket& socket : subject.sockets) {
            slots += socket.kind == rv::geometry::SocketKind::ShelfSlot ? 1 : 0;
            levels += rv::geometry::parseLevelId(socket.id).has_value() ? 1 : 0;
        }
        CHECK(slots + levels == subject.sockets.size());
        const auto colored = [&](const glm::vec4& color) {
            return static_cast<std::size_t>(std::count_if(overlay.lines.vertices().begin(),
                                                          overlay.lines.vertices().end(),
                                                          [&](const auto& vertex) { return vertex.color == color; }));
        };
        CHECK(colored(rv::preview::overlay_colors::kSlot) == 2 * 12 * slots);
        CHECK(colored(rv::preview::overlay_colors::kLevel) == 2 * 12 * levels);
        CHECK(overlay.lines.vertices().back().color == rv::preview::overlay_colors::kCollider);
        // A socket's box is its volume placed by the socket: its corners lie at the socket's bounds.
        for (std::size_t s = 0; s < subject.sockets.size(); ++s) {
            const rv::geometry::Socket& socket = subject.sockets[s];
            const rv::core::Aabb placed = socket.bounds.transformed(socket.transform).inflated(1e-5f);
            for (std::size_t i = 2 * 12 * s; i < 2 * 12 * (s + 1); ++i) {
                CHECK(placed.contains(overlay.lines.vertices()[i].position));
            }
        }
    }
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
    // Just below the floor, by 0.4% of the subject's diagonal (4.4 mm under the CRT): a surface at y = 0 hides it.
    const float height = rv::preview::gridHeight(crt);
    CHECK(height == Approx(-0.004f * glm::length(crt.size())));
    std::size_t level = 0;
    for (const rv::scene::DebugVertex& vertex : grid.lines.vertices()) {
        level += vertex.position.y == height ? 1u : 0u;
    }
    CHECK(level == grid.lines.vertices().size() - 1); // all but the top of the +Y marker

    // A room (9.3 x 3 x 10.3 m) gets the 1 m lines only: its 10 cm lines would be a few pixels apart. The grid lies
    // 5.7 cm below its floor; the drop stops at 10 cm.
    subject.bounds = rv::core::Aabb::fromMinMax(glm::vec3(-4.65f, 0.0f, -5.15f), glm::vec3(4.65f, 3.0f, 5.15f));
    const float roomHalf = rv::preview::gridHalfExtent(subject.bounds);
    const PreviewOverlay room = rv::preview::buildPreviewOverlay(subject, gridOnly);
    CHECK(room.lines.lineCount() == 2 * (2 * static_cast<std::size_t>(roomHalf) + 1) + 1);
    CHECK(rv::preview::gridHeight(subject.bounds) == Approx(-0.004f * glm::length(subject.bounds.size())));
    CHECK(rv::preview::gridHeight(rv::core::Aabb::fromMinMax(glm::vec3(-50.0f), glm::vec3(50.0f))) == Approx(-0.1f));
    CHECK(rv::preview::gridHeight(rv::core::Aabb::fromMinMax(glm::vec3(0.0f), glm::vec3(0.001f))) ==
          Approx(-0.00001f)); // a millimeter part: a hundredth of a millimeter
}

TEST_CASE("preview/overlay: framing again takes in the sockets and the scale references", "[preview][overlay]") {
    rv::test::TempDir dir;
    const auto file = dir.file("crt_fixture.glb");
    REQUIRE(rv::assets::writeGlb(file, rv::preview::makeCrtFixture()).has_value());
    const PreviewSubject subject = load(rv::core::pathToUtf8(file));
    const OverlaySelection all;

    CHECK(rv::preview::framingBounds(subject, all, false) == subject.bounds);
    const rv::core::Aabb around = rv::preview::framingBounds(subject, all, true);
    CHECK(around.contains(subject.bounds));
    CHECK(around.contains(glm::vec3(0.0f, 0.33f, 0.28f + 0.9f))); // Anchor_ScreenLook, in front of the screen
    const rv::core::Aabb cube = rv::preview::referenceCubeBounds(subject.bounds);
    CHECK(around.contains(cube));
    CHECK(around.max.y == Approx(rv::preview::kHumanHeight)); // the marker's head
    CHECK(around.min.x < cube.min.x - 0.5f);                  // the marker beside the cube
    // Only what the overlays show counts.
    CHECK(rv::preview::framingBounds(subject, none(), true) == subject.bounds);
    OverlaySelection socketsOnly = none();
    socketsOnly.sockets = true;
    CHECK(rv::preview::framingBounds(subject, socketsOnly, true).max.z == Approx(0.28f + 0.9f));

    // The empty preview: the references beside a 1 m cube standing on the origin.
    const rv::core::Aabb empty = rv::preview::framingBounds(PreviewSubject{}, all, true);
    CHECK(empty.contains(rv::core::Aabb::fromMinMax(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f))));
    CHECK(empty.contains(rv::preview::referenceCubeBounds(rv::core::Aabb{})));
    CHECK(empty.max.y == Approx(rv::preview::kHumanHeight));
}

TEST_CASE("preview/overlay: label plates keep apart and clear of every labelled point", "[preview][overlay]") {
    const rv::preview::LabelLayout layout;
    std::vector<glm::vec4> plates;
    // Alone, a label sits above and right of its point.
    const std::vector<glm::vec2> one{{100.0f, 100.0f}};
    const std::vector<glm::vec2> text{{40.0f, 10.0f}};
    rv::preview::placeLabelPlates(one, text, layout, plates);
    REQUIRE(plates.size() == 1);
    CHECK(plates[0] == glm::vec4(103.0f, 81.0f, 149.0f, 97.0f));

    CHECK_FALSE(rv::preview::plateMoved(plates[0], one[0], layout));

    // Three labels on one point: the second goes below the first, the third above it, a pixel apart.
    const std::vector<glm::vec2> same{{100.0f, 100.0f}, {100.0f, 100.0f}, {100.0f, 100.0f}};
    const std::vector<glm::vec2> texts{{40.0f, 10.0f}, {30.0f, 10.0f}, {35.0f, 10.0f}};
    rv::preview::placeLabelPlates(same, texts, layout, plates);
    REQUIRE(plates.size() == 3);
    CHECK(plates[1].y == plates[0].w + 1.0f);
    CHECK(plates[2].w + 1.0f == plates[0].y);
    CHECK(rv::preview::plateMoved(plates[1], same[1], layout));
    CHECK(rv::preview::plateMoved(plates[2], same[2], layout));

    // A point under the first plate (the pivot's dot inside "W 71.0 cm") moves that plate off the dot; no plate covers
    // a dot or another plate.
    const std::vector<glm::vec2> crowded{{100.0f, 100.0f}, {110.0f, 90.0f}, {104.0f, 104.0f}};
    const std::vector<glm::vec2> sizes{{40.0f, 10.0f}, {30.0f, 10.0f}, {20.0f, 10.0f}};
    rv::preview::placeLabelPlates(crowded, sizes, layout, plates);
    REQUIRE(plates.size() == 3);
    for (std::size_t i = 0; i < plates.size(); ++i) {
        for (const glm::vec2& dot : crowded) {
            INFO("plate " << i << ", dot " << dot.x << ", " << dot.y);
            CHECK_FALSE(overlap(plates[i], glm::vec4(dot - layout.dotRadius, dot + layout.dotRadius)));
        }
        for (std::size_t j = i + 1; j < plates.size(); ++j) {
            INFO("plates " << i << " and " << j);
            CHECK_FALSE(overlap(plates[i], plates[j]));
        }
    }
    CHECK(rv::preview::plateMoved(plates[0], crowded[0], layout));
    // Fewer sizes than points: one plate per pair.
    rv::preview::placeLabelPlates(crowded, std::span<const glm::vec2>(sizes).first(2), layout, plates);
    CHECK(plates.size() == 2);
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
