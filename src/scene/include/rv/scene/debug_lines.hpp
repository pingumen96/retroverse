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
/// drawn as GL_LINES in one draw call (gl::DebugDraw). GPU-free. Lines with a non-finite end are skipped.
class DebugLines {
public:
    void addLine(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color);
    /// The 12 edges of the local `box` placed by `transform` (an oriented box). An empty box adds nothing.
    void addBox(const core::Aabb& box, const glm::mat4& transform, const glm::vec4& color);
    /// The axes of `transform` from its origin, `length` meters long along each normalized basis vector: X red, Y
    /// green, Z blue (debug_colors). A degenerate basis vector adds no line.
    void addAxes(const glm::mat4& transform, float length);
    /// Reference grid on the y = 0 plane: lines every `spacing` meters out to `halfExtent` along x and z. The lines
    /// through the origin are the X axis (red) and the Z axis (blue), and a green line a tenth of `halfExtent` long
    /// marks +Y at the origin. A non-positive spacing or extent adds nothing; the spacing is raised so that at most
    /// 1000 lines run each way.
    void addGrid(float halfExtent, float spacing);
    /// Appends every line of `other`.
    void append(const DebugLines& other);
    void clear() noexcept { m_vertices.clear(); }

    /// Two vertices per line.
    [[nodiscard]] std::span<const DebugVertex> vertices() const noexcept { return m_vertices; }
    [[nodiscard]] std::size_t lineCount() const noexcept { return m_vertices.size() / 2; }
    [[nodiscard]] bool empty() const noexcept { return m_vertices.empty(); }

private:
    std::vector<DebugVertex> m_vertices;
};

/// Axis length used for a socket of an object with local bounds `objectBounds`, so that axes stay in proportion from a
/// 1 cm model to a room: half the largest extent of the socket's bounds (a shelf slot, a screen), or for a socket
/// without bounds (an anchor) a quarter of the object's largest extent, clamped to [1 mm, 0.5 m]; 0.1 m when neither
/// has bounds.
[[nodiscard]] float socketAxisLength(const geometry::Socket& socket, const core::Aabb& objectBounds) noexcept;

/// Adds, for every instance of every object of `geometry`, its bounds (SceneObjectGeometry::bounds under the instance
/// transform, an oriented box in debug_colors::kObjectBounds) and the axes of each of its sockets (socketAxisLength):
/// shelf slots, CRT screen surfaces, model anchors.
void addSceneDebugLines(const SceneGeometry& geometry, DebugLines& lines);

} // namespace rv::scene
