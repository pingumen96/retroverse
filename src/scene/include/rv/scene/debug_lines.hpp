#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::scene {

/// One end of a debug line: a world position and a linear RGBA color, drawn without lighting or tonemapping.
struct DebugVertex {
    glm::vec3 position{0.0f};
    glm::vec4 color{1.0f};
};
static_assert(sizeof(DebugVertex) == 28, "DebugVertex is uploaded as is: 3 + 4 floats");

/// Colors of the debug overlay (linear RGBA).
namespace debug_colors {
inline constexpr glm::vec4 kObjectBounds{1.0f, 0.78f, 0.1f, 1.0f}; ///< amber
inline constexpr glm::vec4 kAxisX{0.95f, 0.1f, 0.1f, 1.0f};        ///< red
inline constexpr glm::vec4 kAxisY{0.1f, 0.85f, 0.1f, 1.0f};        ///< green
inline constexpr glm::vec4 kAxisZ{0.15f, 0.35f, 1.0f, 1.0f};       ///< blue
inline constexpr glm::vec4 kGrid{0.32f, 0.32f, 0.32f, 1.0f};       ///< grey
} // namespace debug_colors

/// A list of debug lines in world space (developer overlay: bounds, sockets, a reference grid), built on the CPU and
/// drawn as GL_LINES (gl::DebugDraw). GPU-free. Lines with a non-finite end are skipped. Two kinds of lines:
///  - plain lines (addLine, addBox, addGrid): depth-tested against the scene, so surfaces in front hide them;
///  - markers (addAxes: socket axes): their parts hidden by surfaces are drawn too, dimmed, so that a socket inside or
///    just behind a surface (a screen's mapping plane under its bulged glass, an anchor inside a body) still shows its
///    origin and axes.
/// Plain lines are drawn first, then markers, each in the order they were added: where two lines coincide on screen,
/// the later one shows.
class DebugLines {
public:
    void addLine(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color);
    /// The 12 edges of the local `box` placed by `transform` (an oriented box). An empty box adds nothing.
    void addBox(const core::Aabb& box, const glm::mat4& transform, const glm::vec4& color);
    /// Marker lines: the axes of `transform` from its origin, `length` meters long along each normalized basis vector:
    /// X red, Y green, Z blue (debug_colors). A degenerate basis vector adds no line.
    void addAxes(const glm::mat4& transform, float length);
    /// Reference grid on the y = 0 plane: lines every `spacing` meters out to `halfExtent` along x and z. The lines
    /// through the origin are the X axis (red) and the Z axis (blue), and a green line a tenth of `halfExtent` long
    /// marks +Y at the origin. The +Y line comes first, so that the floor axes show where it covers them on screen
    /// (from any eye on the x = 0 plane it overlaps the Z axis). A non-positive spacing or extent adds nothing; the
    /// spacing is raised so that at most 1000 lines run each way.
    void addGrid(float halfExtent, float spacing);
    /// Appends every line of `other`, each to the lines of its kind.
    void append(const DebugLines& other);
    void clear() noexcept;

    /// Plain lines, two vertices each.
    [[nodiscard]] std::span<const DebugVertex> vertices() const noexcept { return m_vertices; }
    /// Marker lines, two vertices each.
    [[nodiscard]] std::span<const DebugVertex> markerVertices() const noexcept { return m_markers; }
    /// Lines of both kinds.
    [[nodiscard]] std::size_t lineCount() const noexcept { return (m_vertices.size() + m_markers.size()) / 2; }
    [[nodiscard]] bool empty() const noexcept { return m_vertices.empty() && m_markers.empty(); }

private:
    /// Appends a line to `vertices` unless an end is not finite.
    static void push(std::vector<DebugVertex>& vertices, const glm::vec3& from, const glm::vec3& to,
                     const glm::vec4& color);

    std::vector<DebugVertex> m_vertices;
    std::vector<DebugVertex> m_markers;
};

/// Axis length used for a socket of an object with local bounds `objectBounds`, so that axes stay in proportion from a
/// 1 cm model to a room: half the largest extent of the socket's bounds (a shelf slot, a screen), or for a socket
/// without bounds (an anchor) a quarter of the object's largest extent, clamped to [1 mm, 0.5 m]; 0.1 m when neither
/// has bounds.
[[nodiscard]] float socketAxisLength(const geometry::Socket& socket, const core::Aabb& objectBounds) noexcept;

/// Adds the bounds of every instance of every object of `geometry` as plain lines: SceneObjectGeometry::bounds under
/// the instance transform, an oriented box in debug_colors::kObjectBounds.
void addSceneBoundsLines(const SceneGeometry& geometry, DebugLines& lines);

/// Adds, for every instance of every object of `geometry`, the axes of each of its sockets as markers
/// (socketAxisLength): shelf slots, CRT screen surfaces, model anchors.
void addSceneSocketLines(const SceneGeometry& geometry, DebugLines& lines);

} // namespace rv::scene
