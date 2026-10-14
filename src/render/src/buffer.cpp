#include <rv/render/buffer.hpp>

#include <limits>
#include <utility>

#include <fmt/format.h>

#include "gl_object_label.hpp"

namespace rv::gl {

Buffer::Buffer(GLuint id, std::size_t size, GLbitfield storageFlags, std::string debugName) noexcept
    : m_id(id), m_size(size), m_storageFlags(storageFlags), m_debugName(std::move(debugName)) {}

Buffer::Buffer(Buffer&& other) noexcept
    : m_id(std::exchange(other.m_id, 0u)), m_size(std::exchange(other.m_size, std::size_t{0})),
      m_storageFlags(std::exchange(other.m_storageFlags, 0u)), m_debugName(std::move(other.m_debugName)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        release();
        m_id = std::exchange(other.m_id, 0u);
        m_size = std::exchange(other.m_size, std::size_t{0});
        m_storageFlags = std::exchange(other.m_storageFlags, 0u);
        m_debugName = std::move(other.m_debugName);
    }
    return *this;
}

Buffer::~Buffer() {
    release();
}

void Buffer::release() noexcept {
    if (m_id != 0) {
        glDeleteBuffers(1, &m_id);
        m_id = 0;
    }
    m_size = 0;
    m_storageFlags = 0;
}

Expected<Buffer> Buffer::create(std::size_t sizeBytes, GLbitfield storageFlags, std::string_view debugName) {
    return allocate(sizeBytes, nullptr, storageFlags, debugName);
}

Expected<Buffer> Buffer::create(std::span<const std::byte> initialData, GLbitfield storageFlags,
                                std::string_view debugName) {
    return allocate(initialData.size(), initialData.data(), storageFlags, debugName);
}

Expected<Buffer> Buffer::allocate(std::size_t sizeBytes, const void* data, GLbitfield storageFlags,
                                  std::string_view debugName) {
    if (sizeBytes == 0) {
        return makeError(ErrorCode::InvalidArgument, "buffer size must be greater than zero", std::string(debugName));
    }
    if (sizeBytes > static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max())) {
        return makeError(ErrorCode::OutOfRange, fmt::format("buffer size {} exceeds GLsizeiptr", sizeBytes),
                         std::string(debugName));
    }
    GLuint id = 0;
    glCreateBuffers(1, &id);
    if (id == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateBuffers returned no buffer", std::string(debugName));
    }
    Buffer buffer(id, sizeBytes, storageFlags, std::string(debugName));
    glNamedBufferStorage(id, static_cast<GLsizeiptr>(sizeBytes), data, storageFlags);
    // Storage only becomes immutable when the allocation succeeded (it fails with GL_OUT_OF_MEMORY otherwise).
    GLint immutable = GL_FALSE;
    glGetNamedBufferParameteriv(id, GL_BUFFER_IMMUTABLE_STORAGE, &immutable);
    if (immutable == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not allocate {} bytes of buffer storage", sizeBytes),
                         std::string(debugName));
    }
    detail::setObjectLabel(GL_BUFFER, id, debugName);
    return buffer;
}

Status Buffer::update(std::size_t offsetBytes, std::span<const std::byte> bytes) {
    if (m_id == 0) {
        return makeError(ErrorCode::InvalidArgument, "cannot update an empty buffer", m_debugName);
    }
    if ((m_storageFlags & GL_DYNAMIC_STORAGE_BIT) == 0) {
        return makeError(ErrorCode::InvalidArgument, "buffer was created without GL_DYNAMIC_STORAGE_BIT", m_debugName);
    }
    if (offsetBytes > m_size || bytes.size() > m_size - offsetBytes) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("update of {} bytes at offset {} exceeds the buffer size of {} bytes",
                                     bytes.size(), offsetBytes, m_size),
                         m_debugName);
    }
    if (bytes.empty()) {
        return ok();
    }
    glNamedBufferSubData(m_id, static_cast<GLintptr>(offsetBytes), static_cast<GLsizeiptr>(bytes.size()), bytes.data());
    return ok();
}

} // namespace rv::gl
