#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <tuple>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/debug_lines.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

using Catch::Approx;
using rv::core::Aabb;
using rv::scene::DebugLines;
using rv::scene::DebugVertex;
namespace colors = rv::scene::debug_colors;

namespace {

/// Unordered line, rounded to millimeters, for set comparisons.
using LineKey = std::tuple<int, int, int, int, int, int>;

int millimeters(float meters) {
    return static_cast<int>(std::lround(meters * 1000.0f));
}

LineKey keyOf(const glm::vec3& a, const glm::vec3& b) {
    const auto point = [](const glm::vec3& p) {
        return std::tuple(millimeters(p.x), millimeters(p.y), millimeters(p.z));
    };
    auto first = point(a);
    auto second = point(b);
    if (second < first) {
        std::swap(first, second);
    }
    return std::tuple_cat(first, second);
}

std::set<LineKey> linesOf(const DebugLines& lines) {
    std::set<LineKey> keys;
    const auto vertices = lines.vertices();
    for (std::size_t i = 0; i + 1 < vertices.size(); i += 2) {
        keys.insert(keyOf(vertices[i].position, vertices[i + 1].position));
    }
    return keys;
}

} // namespace

TEST_CASE("scene/debug_lines: a box is its 12 edges under the transform", "[scene][debug_lines]") {
    DebugLines lines;
    const Aabb box = Aabb::fromMinMax(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    lines.addBox(box, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)), colors::kObjectBounds);
    REQUIRE(lines.lineCount() == 12);
    CHECK(lines.vertices().size() == 24);
    const std::set<LineKey> keys = linesOf(lines);
    CHECK(keys.size() == 12); // all distinct
    CHECK(keys.contains(keyOf({10.0f, 0.0f, 0.0f}, {11.0f, 0.0f, 0.0f})));
    CHECK(keys.contains(keyOf({10.0f, 0.0f, 0.0f}, {10.0f, 2.0f, 0.0f})));
    CHECK(keys.contains(keyOf({11.0f, 2.0f, 0.0f}, {11.0f, 2.0f, 3.0f})));
    CHECK_FALSE(keys.contains(keyOf({10.0f, 0.0f, 0.0f}, {11.0f, 2.0f, 3.0f}))); // no diagonal
    for (const DebugVertex& vertex : lines.vertices()) {
        CHECK(vertex.color == colors::kObjectBounds);
    }

    // Turned by 90 degrees about +Y the box stays oriented: its 3 m edge now runs along +X.
    DebugLines turned;
    turned.addBox(box, glm::rotate(glm::mat4(1.0f), rv::core::kHalfPi, glm::vec3(0.0f, 1.0f, 0.0f)), colors::kGrid);
    CHECK(linesOf(turned).contains(keyOf({0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f})));

    // Empty boxes and non-finite ends add nothing.
    DebugLines nothing;
    nothing.addBox(Aabb{}, glm::mat4(1.0f), colors::kGrid);
    nothing.addLine(glm::vec3(std::numeric_limits<float>::infinity()), glm::vec3(0.0f), colors::kGrid);
    CHECK(nothing.empty());
}

TEST_CASE("scene/debug_lines: axes are colored X red, Y green, Z blue and normalized", "[scene][debug_lines]") {
    DebugLines lines;
    // Scaled by 4 and turned: the axes keep the requested length along the turned directions.
    const glm::mat4 transform = glm::scale(glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)),
                                                       rv::core::kHalfPi, glm::vec3(0.0f, 1.0f, 0.0f)),
                                           glm::vec3(4.0f));
    lines.addAxes(transform, 0.5f);
    REQUIRE(lines.lineCount() == 3);
    // Axes are markers (their hidden parts are drawn dimmed), not plain depth-tested lines.
    CHECK(lines.vertices().empty());
    CHECK_FALSE(lines.empty());
    const auto vertices = lines.markerVertices();
    REQUIRE(vertices.size() == 6);
    CHECK(vertices[0].color == colors::kAxisX);
    CHECK(vertices[2].color == colors::kAxisY);
    CHECK(vertices[4].color == colors::kAxisZ);
    for (std::size_t i = 0; i < 6; i += 2) {
        CHECK(rv::core::nearlyEqual(vertices[i].position, glm::vec3(1.0f, 2.0f, 3.0f)));
        CHECK(glm::length(vertices[i + 1].position - vertices[i].position) == Approx(0.5f));
    }
    // Local +X turned by +90 degrees about +Y points to -Z.
    CHECK(rv::core::nearlyEqual(vertices[1].position, glm::vec3(1.0f, 2.0f, 2.5f), 1e-5f));

    // A degenerate basis vector draws no axis.
    DebugLines flat;
    flat.addAxes(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f)), 1.0f);
    CHECK(flat.lineCount() == 2);
}

TEST_CASE("scene/debug_lines: the grid covers the extent with axis-colored center lines", "[scene][debug_lines]") {
    DebugLines grid;
    grid.addGrid(2.0f, 1.0f);
    // 5 lines along X, 5 along Z, and the +Y marker.
    REQUIRE(grid.lineCount() == 11);
    std::size_t red = 0;
    std::size_t blue = 0;
    std::size_t green = 0;
    const auto vertices = grid.vertices();
    for (std::size_t i = 0; i < vertices.size(); i += 2) {
        red += vertices[i].color == colors::kAxisX ? 1u : 0u;
        blue += vertices[i].color == colors::kAxisZ ? 1u : 0u;
        green += vertices[i].color == colors::kAxisY ? 1u : 0u;
        if (vertices[i].color != colors::kAxisY) {
            CHECK(vertices[i].position.y == 0.0f);
            CHECK(vertices[i + 1].position.y == 0.0f);
        }
    }
    CHECK(red == 1);
    CHECK(blue == 1);
    CHECK(green == 1);
    // The +Y marker comes first, so the floor axes drawn after it show where they overlap it on screen.
    CHECK(vertices[0].color == colors::kAxisY);
    CHECK(grid.markerVertices().empty());
    const std::set<LineKey> keys = linesOf(grid);
    CHECK(keys.contains(keyOf({-2.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f})));   // the X axis
    CHECK(keys.contains(keyOf({0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 2.0f})));   // the Z axis
    CHECK(keys.contains(keyOf({-2.0f, 0.0f, -2.0f}, {2.0f, 0.0f, -2.0f}))); // the farthest line
    CHECK(keys.contains(keyOf({0.0f, 0.0f, 0.0f}, {0.0f, 0.2f, 0.0f})));    // +Y

    // A dense grid is thinned to at most 1000 lines each way; invalid sizes add nothing.
    DebugLines dense;
    dense.addGrid(100.0f, 0.001f);
    CHECK(dense.lineCount() <= 2 * 1001 + 1);
    DebugLines invalid;
    invalid.addGrid(-1.0f, 1.0f);
    invalid.addGrid(1.0f, 0.0f);
    invalid.addGrid(std::numeric_limits<float>::quiet_NaN(), 1.0f);
    CHECK(invalid.empty());

    DebugLines both;
    both.append(grid);
    both.append(grid);
    CHECK(both.lineCount() == 22);
    // Appending keeps each kind of line apart.
    DebugLines axes;
    axes.addAxes(glm::mat4(1.0f), 1.0f);
    both.append(axes);
    CHECK(both.lineCount() == 25);
    CHECK(both.vertices().size() == 44);
    CHECK(both.markerVertices().size() == 6);
    both.clear();
    CHECK(both.empty());
    CHECK(both.lineCount() == 0);
}

TEST_CASE("scene/debug_lines: socket axes follow the size of the socket, else of its object", "[scene][debug_lines]") {
    using rv::scene::socketAxisLength;
    rv::geometry::Socket socket;
    const Aabb crt = Aabb::fromMinMax(glm::vec3(-0.355f, 0.0f, -0.28f), glm::vec3(0.355f, 0.61f, 0.28f));
    const Aabb tiny = Aabb::fromMinMax(glm::vec3(-0.005f, 0.0f, -0.005f), glm::vec3(0.005f, 0.01f, 0.005f));
    // An anchor (no bounds): a quarter of its object's largest extent, so a 1 cm model gets 2.5 mm axes.
    CHECK(socketAxisLength(socket, crt) == Approx(0.1775f));
    CHECK(socketAxisLength(socket, tiny) == Approx(0.0025f));
    CHECK(socketAxisLength(socket, Aabb{}) == Approx(0.1f)); // nothing to measure
    // Sockets with bounds: half their largest extent (a shelf slot: half its width), within [1 mm, 50 cm].
    socket.bounds = Aabb::fromMinMax(glm::vec3(-0.0625f, 0.0f, -0.01f), glm::vec3(0.0625f, 0.085f, 0.01f));
    CHECK(socketAxisLength(socket, crt) == Approx(0.0625f));
    socket.bounds = Aabb::fromMinMax(glm::vec3(0.0f), glm::vec3(0.0001f));
    CHECK(socketAxisLength(socket, crt) == Approx(0.001f));
    socket.bounds = Aabb::fromMinMax(glm::vec3(0.0f), glm::vec3(4.0f));
    CHECK(socketAxisLength(socket, crt) == Approx(0.5f));
}

TEST_CASE("scene/debug_lines: scene lines are object bounds and socket axes at every instance",
          "[scene][debug_lines]") {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto library = rv::assets::MaterialLibrary::loadFromFile(assets / "materials/base/materials.json");
    REQUIRE(library.has_value());
    auto scene = rv::scene::loadSceneDescription(assets / "style_tests/reference_scenes/m0_testroom.json");
    REQUIRE(scene.has_value());
    const rv::scene::SceneGeometry geometry = rv::scene::buildSceneGeometry(*scene, *library);

    DebugLines bounds;
    rv::scene::addSceneBoundsLines(geometry, bounds);
    DebugLines sockets;
    rv::scene::addSceneSocketLines(geometry, sockets);
    std::size_t boxes = 0;
    std::size_t axes = 0;
    for (const rv::scene::SceneObjectGeometry& object : geometry.objects) {
        boxes += object.instances.size() * 12;
        axes += object.instances.size() * 3 * object.sockets.size();
    }
    // Bounds are plain lines, socket axes markers (the CRT screen surface lies under its bulged glass).
    CHECK(bounds.lineCount() == boxes);
    CHECK(bounds.vertices().size() == 2 * boxes);
    CHECK(bounds.markerVertices().empty());
    CHECK(sockets.lineCount() == axes);
    CHECK(sockets.vertices().empty());
    CHECK(sockets.markerVertices().size() == 2 * axes);

    // The shelf's bounds sit at its position: 1.2 x 2.0 x 0.35 m standing on the floor at (-1.5, 0, -4.8).
    DebugLines shelfOnly;
    rv::scene::SceneGeometry shelf;
    for (const rv::scene::SceneObjectGeometry& object : geometry.objects) {
        if (object.id == "shelf_main") {
            shelf.objects.push_back(object);
        }
    }
    REQUIRE(shelf.objects.size() == 1);
    rv::scene::addSceneBoundsLines(shelf, shelfOnly);
    REQUIRE(shelfOnly.lineCount() == 12);
    CHECK(linesOf(shelfOnly).contains(keyOf({-2.1f, 0.0f, -4.975f}, {-0.9f, 0.0f, -4.975f})));
}
