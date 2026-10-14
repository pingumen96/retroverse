#pragma once

#include <cstddef>
#include <string_view>

#include <glad/gl.h>

#include <rv/core/expected.hpp>

namespace rv::gl {

class Buffer;

/// Move-only RAII vertex array object configured with the GL 4.5 separate attribute format API: attributes (shader
/// input locations) read from binding points, and vertex buffers are attached to binding points. Limits are checked
/// against the minimums every OpenGL 4.5 implementation provides (16 attributes and bindings, 2047-byte relative
/// offsets, 2048-byte strides). The VAO does not own the buffers it references: keep them alive while drawing.
/// A default-constructed or moved-from VertexArray is empty (id 0). Main thread only.
class VertexArray {
public:
    static constexpr GLuint kMaxAttributes = 16;
    static constexpr GLuint kMaxBindings = 16;
    static constexpr std::size_t kMaxRelativeOffset = 2047;
    static constexpr std::size_t kMaxStride = 2048;

    VertexArray() noexcept = default;
    VertexArray(VertexArray&& other) noexcept;
    VertexArray& operator=(VertexArray&& other) noexcept;
    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;
    ~VertexArray();

    /// Creates an empty vertex array labeled `debugName`.
    [[nodiscard]] static Expected<VertexArray> create(std::string_view debugName);

    /// Declares attribute `location` as `componentCount` (1-4) floats read `relativeOffsetBytes` into each vertex of
    /// binding point `bindingIndex`, and enables it.
    [[nodiscard]] Status setFloatAttribute(GLuint location, GLuint bindingIndex, int componentCount,
                                           std::size_t relativeOffsetBytes);
    /// Attaches `buffer` to binding point `bindingIndex`: vertices start at `offsetBytes` and are `strideBytes` apart.
    [[nodiscard]] Status setVertexBuffer(GLuint bindingIndex, const Buffer& buffer, std::size_t offsetBytes,
                                         std::size_t strideBytes);
    /// Makes binding point `bindingIndex` advance once every `divisor` instances instead of once per vertex
    /// (glVertexArrayBindingDivisor; 0 restores per-vertex data). Per-instance data uses divisor 1.
    [[nodiscard]] Status setBindingDivisor(GLuint bindingIndex, GLuint divisor);
    /// Uses `buffer` as the index buffer of indexed draws.
    [[nodiscard]] Status setElementBuffer(const Buffer& buffer);

    /// glBindVertexArray.
    void bind() const;

    [[nodiscard]] GLuint id() const noexcept { return m_id; }
    [[nodiscard]] bool valid() const noexcept { return m_id != 0; }

private:
    explicit VertexArray(GLuint id) noexcept : m_id(id) {}
    void release() noexcept;

    GLuint m_id = 0;
};

} // namespace rv::gl
