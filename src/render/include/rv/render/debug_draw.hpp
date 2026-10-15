#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <glad/gl.h>

#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/vertex_array.hpp>
#include <rv/scene/debug_lines.hpp>

namespace rv::gl {

class RenderStateCache;
class ShaderProgram;

/// Draws debug lines (scene::DebugLines: object bounds, socket axes, a world grid) as GL_LINES with the debug_lines
/// program (debug/debug_lines.{vert,frag}: positions through the view-projection of the frame block, linear colors
/// without lighting or tonemapping), in up to two draw calls: the plain lines and then the markers (socket axes),
/// depth-tested in their colors; then the parts of the markers that surfaces hide, dimmed to kHiddenMarkerScale.
/// setLines() uploads the vertices once for static content into a buffer that grows as needed. Move-only; a
/// default-constructed DebugDraw is empty. Main thread only.
class DebugDraw {
public:
    /// Attribute locations of the line vertices (scene::DebugVertex).
    static constexpr GLuint kPositionLocation = 0;
    static constexpr GLuint kColorLocation = 1;
    /// Color scale of the marker parts that surfaces hide.
    static constexpr float kHiddenMarkerScale = 0.4f;

    DebugDraw() noexcept = default;

    /// Creates the vertex array, labeled `debugName`; no lines yet.
    [[nodiscard]] static Expected<DebugDraw> create(std::string_view debugName);

    /// Replaces the lines with `lines` (markers are uploaded twice: as given and dimmed), reallocating the vertex
    /// buffer when they do not fit. Errors: those of Buffer::create and VertexArray (the previous lines are then
    /// dropped).
    [[nodiscard]] Status setLines(const scene::DebugLines& lines);

    /// Draws the lines with `program` into the bound target, depth-tested against the scene already there without
    /// writing depth, blending off: plain lines and markers where they are in front (GL_LEQUAL; the vertex stage pulls
    /// lines slightly towards the eye, so edges lying on surfaces stay visible), then the dimmed markers where they are
    /// behind (GL_GREATER). The frame uniform block must be bound. Depth writes and GL_LESS are restored afterwards.
    void draw(const ShaderProgram& program, RenderStateCache& state) const;

    /// Lines of both kinds.
    [[nodiscard]] std::size_t lineCount() const noexcept { return (m_lineVertexCount + m_markerVertexCount) / 2; }
    [[nodiscard]] bool valid() const noexcept { return m_vertexArray.valid(); }

private:
    VertexArray m_vertexArray;
    /// Plain lines, then markers, then the dimmed copy of the markers.
    Buffer m_vertices;
    std::size_t m_lineVertexCount = 0;
    std::size_t m_markerVertexCount = 0;
    std::string m_debugName;
};

} // namespace rv::gl
