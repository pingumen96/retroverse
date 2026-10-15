#include <rv/app/debug_line_selection.hpp>

namespace rv::app {

scene::DebugLines composeDebugLines(const DebugLineSelection& selection, const DebugGrid& grid,
                                    const scene::DebugLines& bounds, const scene::DebugLines& sockets) {
    scene::DebugLines lines;
    if (selection.grid) {
        lines.addGrid(grid.halfExtent, grid.spacing);
    }
    if (selection.bounds) {
        lines.append(bounds);
    }
    if (selection.sockets) {
        lines.append(sockets);
    }
    return lines;
}

} // namespace rv::app
