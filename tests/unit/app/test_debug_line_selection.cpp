#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <rv/app/debug_line_selection.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/debug_lines.hpp>

using rv::app::composeDebugLines;
using rv::app::DebugGrid;
using rv::app::DebugLineSelection;
using rv::scene::DebugLines;

namespace {

DebugLines boundsLines() {
    DebugLines lines;
    lines.addBox(rv::core::Aabb::fromMinMax(glm::vec3(0.0f), glm::vec3(1.0f)), glm::mat4(1.0f),
                 rv::scene::debug_colors::kObjectBounds);
    return lines;
}

DebugLines socketLines() {
    DebugLines lines;
    lines.addAxes(glm::mat4(1.0f), 0.1f);
    return lines;
}

} // namespace

TEST_CASE("app/debug_line_selection: each kind of debug line is drawn only when selected",
          "[app][debug_line_selection]") {
    const DebugGrid grid{2.0f, 1.0f}; // 5 lines each way and the +Y marker
    DebugLines gridOnly;
    gridOnly.addGrid(grid.halfExtent, grid.spacing);
    const DebugLines bounds = boundsLines();
    const DebugLines sockets = socketLines();

    CHECK_FALSE(DebugLineSelection{}.any());
    CHECK(composeDebugLines(DebugLineSelection{}, grid, bounds, sockets).empty());

    const DebugLines all = composeDebugLines(rv::app::kAllDebugLines, grid, bounds, sockets);
    CHECK(rv::app::kAllDebugLines.any());
    CHECK(all.lineCount() == gridOnly.lineCount() + 12 + 3);
    // The grid comes first, then the bounds; socket axes are markers.
    REQUIRE(all.vertices().size() == gridOnly.vertices().size() + bounds.vertices().size());
    CHECK(all.vertices().front().position == gridOnly.vertices().front().position);
    CHECK(all.vertices().back().position == bounds.vertices().back().position);
    CHECK(all.markerVertices().size() == sockets.markerVertices().size());

    const DebugLines noGrid = composeDebugLines(DebugLineSelection{false, true, true}, grid, bounds, sockets);
    CHECK(noGrid.vertices().size() == bounds.vertices().size());
    CHECK(noGrid.markerVertices().size() == sockets.markerVertices().size());

    const DebugLines socketsOnly = composeDebugLines(DebugLineSelection{false, false, true}, grid, bounds, sockets);
    CHECK(socketsOnly.vertices().empty());
    CHECK(socketsOnly.lineCount() == 3);

    const DebugLines gridAndBounds = composeDebugLines(DebugLineSelection{true, true, false}, grid, bounds, sockets);
    CHECK(gridAndBounds.markerVertices().empty());
    CHECK(gridAndBounds.lineCount() == gridOnly.lineCount() + 12);

    CHECK(DebugLineSelection{true, false, true} == DebugLineSelection{true, false, true});
    CHECK_FALSE(DebugLineSelection{true, false, true} == DebugLineSelection{true, true, true});
}
