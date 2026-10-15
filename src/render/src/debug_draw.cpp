#include <rv/render/debug_draw.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>

#include <fmt/format.h>

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
    const std::span<const scene::DebugVertex> vertices = lines.vertices();
    if (vertices.size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
        return makeError(ErrorCode::OutOfRange, fmt::format("{} debug line vertices exceed GLsizei", vertices.size()),
                         m_debugName);
    }
    const std::span<const std::byte> bytes = std::as_bytes(vertices);
    m_vertexCount = 0;
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
    m_vertexCount = vertices.size();
    return ok();
}

void DebugDraw::draw(const ShaderProgram& program, RenderStateCache& state) const {
    if (m_vertexCount == 0 || !program.valid()) {
        return;
    }
    state.setDepthTest(true);
    state.setDepthFunc(GL_LEQUAL);
    state.setDepthWrite(false);
    state.setBlend(false);
    program.use();
    m_vertexArray.bind();
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_vertexCount));
    state.setDepthWrite(true);
    state.setDepthFunc(GL_LESS);
}

} // namespace rv::gl
