#include <rv/render/vertex_array.hpp>

#include <limits>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <rv/render/buffer.hpp>

#include "gl_object_label.hpp"

namespace rv::gl {

VertexArray::VertexArray(VertexArray&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}

VertexArray& VertexArray::operator=(VertexArray&& other) noexcept {
    if (this != &other) {
        release();
        m_id = std::exchange(other.m_id, 0u);
    }
    return *this;
}

VertexArray::~VertexArray() {
    release();
}

void VertexArray::release() noexcept {
    if (m_id != 0) {
        glDeleteVertexArrays(1, &m_id);
        m_id = 0;
    }
}

Expected<VertexArray> VertexArray::create(std::string_view debugName) {
    GLuint id = 0;
    glCreateVertexArrays(1, &id);
    if (id == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateVertexArrays returned no vertex array",
                         std::string(debugName));
    }
    detail::setObjectLabel(GL_VERTEX_ARRAY, id, debugName);
    return VertexArray(id);
}

Status VertexArray::setFloatAttribute(GLuint location, GLuint bindingIndex, int componentCount,
                                      std::size_t relativeOffsetBytes) {
    if (m_id == 0) {
        return makeError(ErrorCode::InvalidArgument, "vertex array is empty");
    }
    if (location >= kMaxAttributes || bindingIndex >= kMaxBindings) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("attribute location {} / binding {} exceeds the OpenGL 4.5 minimum of {}",
                                     location, bindingIndex, kMaxAttributes));
    }
    if (componentCount < 1 || componentCount > 4) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("attribute {} has {} components (expected 1 to 4)", location, componentCount));
    }
    if (relativeOffsetBytes > kMaxRelativeOffset) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("attribute {} relative offset {} exceeds {}", location, relativeOffsetBytes,
                                     kMaxRelativeOffset));
    }
    glVertexArrayAttribFormat(m_id, location, componentCount, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(relativeOffsetBytes));
    glVertexArrayAttribBinding(m_id, location, bindingIndex);
    glEnableVertexArrayAttrib(m_id, location);
    return ok();
}

Status VertexArray::setVertexBuffer(GLuint bindingIndex, const Buffer& buffer, std::size_t offsetBytes,
                                    std::size_t strideBytes) {
    if (m_id == 0 || !buffer.valid()) {
        return makeError(ErrorCode::InvalidArgument, "vertex array or vertex buffer is empty", buffer.debugName());
    }
    if (bindingIndex >= kMaxBindings) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("binding {} exceeds the OpenGL 4.5 minimum of {}", bindingIndex, kMaxBindings),
                         buffer.debugName());
    }
    if (strideBytes > kMaxStride || offsetBytes > static_cast<std::size_t>(std::numeric_limits<GLintptr>::max())) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("stride {} / offset {} out of range", strideBytes, offsetBytes),
                         buffer.debugName());
    }
    glVertexArrayVertexBuffer(m_id, bindingIndex, buffer.id(), static_cast<GLintptr>(offsetBytes),
                              static_cast<GLsizei>(strideBytes));
    return ok();
}

Status VertexArray::setBindingDivisor(GLuint bindingIndex, GLuint divisor) {
    if (m_id == 0) {
        return makeError(ErrorCode::InvalidArgument, "vertex array is empty");
    }
    if (bindingIndex >= kMaxBindings) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("binding {} exceeds the OpenGL 4.5 minimum of {}", bindingIndex, kMaxBindings));
    }
    glVertexArrayBindingDivisor(m_id, bindingIndex, divisor);
    return ok();
}

Status VertexArray::setElementBuffer(const Buffer& buffer) {
    if (m_id == 0 || !buffer.valid()) {
        return makeError(ErrorCode::InvalidArgument, "vertex array or index buffer is empty", buffer.debugName());
    }
    glVertexArrayElementBuffer(m_id, buffer.id());
    return ok();
}

void VertexArray::bind() const {
    glBindVertexArray(m_id);
}

} // namespace rv::gl
