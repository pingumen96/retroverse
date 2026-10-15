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

/// Draws a batch of debug lines (scene::DebugLines: object bounds, socket axes, a world grid) as GL_LINES in one draw
/// call with the debug_lines program (debug/debug_lines.{vert,frag}: positions through the view-projection of the frame
/// block, linear colors without lighting or tonemapping). setLines() uploads the vertices once for static content into
/// a buffer that grows as needed. Move-only; a default-constructed DebugDraw is empty. Main thread only.
class DebugDraw {
public:
    /// Attribute locations of the line vertices (scene::DebugVertex).
    static constexpr GLuint kPositionLocation = 0;
    static constexpr GLuint kColorLocation = 1;

    DebugDraw() noexcept = default;

    /// Creates the vertex array, labeled `debugName`; no lines yet.
    [[nodiscard]] static Expected<DebugDraw> create(std::string_view debugName);

    /// Replaces the lines with `lines`, reallocating the vertex buffer when they do not fit. Errors: those of
    /// Buffer::create and VertexArray (the previous lines are then dropped).
    [[nodiscard]] Status setLines(const scene::DebugLines& lines);

    /// Draws the lines with `program` into the bound target: depth-tested against the scene already there (GL_LEQUAL;
    /// the vertex stage pulls lines slightly towards the eye, so edges lying on surfaces stay visible) without writing
    /// depth, blending off. The frame uniform block must be bound. Depth writes and GL_LESS are restored afterwards.
    void draw(const ShaderProgram& program, RenderStateCache& state) const;

    [[nodiscard]] std::size_t lineCount() const noexcept { return m_vertexCount / 2; }
    [[nodiscard]] bool valid() const noexcept { return m_vertexArray.valid(); }

private:
    VertexArray m_vertexArray;
    Buffer m_vertices;
    std::size_t m_vertexCount = 0;
    std::string m_debugName;
};

} // namespace rv::gl
