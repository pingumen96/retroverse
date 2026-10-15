#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <rv/scene/debug_lines.hpp>

namespace rv::app {

/// Reference grid of the debug lines (scene::DebugLines::addGrid) on the y = 0 plane.
struct DebugGrid {
    float halfExtent = 10.0f; ///< meters from the origin along x and z
    float spacing = 1.0f;     ///< meters between lines
};

/// Kinds of debug lines drawn over the scene: --debug-draw turns every kind on, the developer overlay toggles each.
struct DebugLineSelection {
    bool grid = false;    ///< the reference grid (DebugGrid)
    bool bounds = false;  ///< the bounds of every object instance (scene::addSceneBoundsLines)
    bool sockets = false; ///< the axes of every socket (scene::addSceneSocketLines)

    [[nodiscard]] bool any() const noexcept { return grid || bounds || sockets; }
    friend bool operator==(const DebugLineSelection&, const DebugLineSelection&) = default;
};

/// Every kind of debug line (--debug-draw).
inline constexpr DebugLineSelection kAllDebugLines{true, true, true};

/// The debug lines of `selection`, in drawing order: the grid of `grid` first (its +Y marker before the floor axes),
/// then the object bounds `bounds`, then the socket axes `sockets` (markers). Kinds left out add nothing.
[[nodiscard]] scene::DebugLines composeDebugLines(const DebugLineSelection& selection, const DebugGrid& grid,
                                                  const scene::DebugLines& bounds, const scene::DebugLines& sockets);

} // namespace rv::app
