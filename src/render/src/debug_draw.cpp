#include <rv/render/debug_draw.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/render/render_state_cache.hpp>
#include <rv/render/shader_program.hpp>

namespace rv::gl {
namespace {

constexpr GLuint kVertexBinding = 0;
constexpr std::size_t kStride = sizeof(scene::DebugVertex);

} // namespace

Expected<DebugDraw> DebugDraw::create(std::string_view debugName) {
    auto vertexArray = VertexArray::create(debugName);
    if (!vertexArray) {
        return propagate(vertexArray.error());
    }
    Status status =
        vertexArray->setFloatAttribute(kPositionLocation, kVertexBinding, 3, offsetof(scene::DebugVertex, position));
    if (status) {
        status = vertexArray->setFloatAttribute(kColorLocation, kVertexBinding, 4, offsetof(scene::DebugVertex, color));
    }
    if (!status) {
        return propagate(status.error());
    }
    DebugDraw draw;
    draw.m_vertexArray = std::move(*vertexArray);
    draw.m_debugName = std::string(debugName);
    return draw;
}

Status DebugDraw::setLines(const scene::DebugLines& lines) {
    const std::span<const scene::DebugVertex> plain = lines.vertices();
    const std::span<const scene::DebugVertex> markers = lines.markerVertices();
    // Markers twice: as given, for their visible parts, then dimmed, for the parts surfaces hide.
    std::vector<scene::DebugVertex> vertices;
    vertices.reserve(plain.size() + 2 * markers.size());
    vertices.insert(vertices.end(), plain.begin(), plain.end());
    vertices.insert(vertices.end(), markers.begin(), markers.end());
    for (scene::DebugVertex vertex : markers) {
        vertex.color = glm::vec4(glm::vec3(vertex.color) * kHiddenMarkerScale, vertex.color.a);
        vertices.push_back(vertex);
    }
    if (vertices.size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
        return makeError(ErrorCode::OutOfRange, fmt::format("{} debug line vertices exceed GLsizei", vertices.size()),
                         m_debugName);
    }
    const std::span<const std::byte> bytes = std::as_bytes(std::span<const scene::DebugVertex>(vertices));
    m_lineVertexCount = 0;
    m_markerVertexCount = 0;
    if (bytes.empty()) {
        return ok();
    }
    if (!m_vertices.valid() || m_vertices.size() < bytes.size()) {
        auto buffer = Buffer::create(std::max(bytes.size(), 2 * m_vertices.size()), GL_DYNAMIC_STORAGE_BIT,
                                     m_debugName + ".vertices");
        if (!buffer) {
            return propagate(buffer.error());
        }
        if (auto attached = m_vertexArray.setVertexBuffer(kVertexBinding, *buffer, 0, kStride); !attached) {
            return attached;
        }
        m_vertices = std::move(*buffer);
    }
    if (auto updated = m_vertices.update(0, bytes); !updated) {
        return updated;
    }
    m_lineVertexCount = plain.size();
    m_markerVertexCount = markers.size();
    return ok();
}

void DebugDraw::draw(const ShaderProgram& program, RenderStateCache& state) const {
    if ((m_lineVertexCount == 0 && m_markerVertexCount == 0) || !program.valid()) {
        return;
    }
    state.setDepthTest(true);
    state.setDepthFunc(GL_LEQUAL);
    state.setDepthWrite(false);
    state.setBlend(false);
    program.use();
    m_vertexArray.bind();
    // Plain lines and markers in front of the scene, in one range (the markers follow the plain lines).
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_lineVertexCount + m_markerVertexCount));
    if (m_markerVertexCount > 0) {
        // The dimmed copy of the markers where a surface is in front: the complementary depth test, with the same
        // depths, so every marker fragment is drawn exactly once, either in its color or dimmed.
        state.setDepthFunc(GL_GREATER);
        glDrawArrays(GL_LINES, static_cast<GLint>(m_lineVertexCount + m_markerVertexCount),
                     static_cast<GLsizei>(m_markerVertexCount));
    }
    state.setDepthWrite(true);
    state.setDepthFunc(GL_LESS);
}

} // namespace rv::gl
