#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <glad/gl.h>

#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/uniform_blocks.hpp>

namespace rv::gl {

/// A C++ mirror of a std140 uniform block: copied byte for byte into the buffer, so it must be trivially copyable,
/// standard-layout and padded to a multiple of 16 bytes (see FrameUniforms).
template <class T>
concept Std140Block = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && sizeof(T) % 16 == 0;

/// Move-only uniform buffer holding one std140 block `T`: sizeof(T) bytes of immutable storage created with
/// GL_DYNAMIC_STORAGE_BIT, rewritten with update() and attached to a binding point (uniform_blocks.hpp) with bind().
/// A default-constructed or moved-from UniformBuffer is empty. Main thread only.
template <Std140Block T>
class UniformBuffer {
public:
    UniformBuffer() noexcept = default;

    /// Allocates the buffer; its contents stay undefined until the first update().
    [[nodiscard]] static Expected<UniformBuffer> create(std::string_view debugName) {
        auto buffer = Buffer::create(sizeof(T), GL_DYNAMIC_STORAGE_BIT, debugName);
        if (!buffer) {
            return propagate(buffer.error());
        }
        return UniformBuffer(std::move(*buffer));
    }

    /// Replaces the contents (glNamedBufferSubData). Draws issued earlier still read the previous contents, so one
    /// buffer can serve several views in a frame.
    [[nodiscard]] Status update(const T& value) {
        return m_buffer.update(0, std::as_bytes(std::span<const T, 1>(&value, 1)));
    }

    /// Attaches the buffer to `binding` for the following draws (glBindBufferBase(GL_UNIFORM_BUFFER, ...)).
    void bind(UniformBinding binding) const { glBindBufferBase(GL_UNIFORM_BUFFER, toIndex(binding), m_buffer.id()); }

    [[nodiscard]] bool valid() const noexcept { return m_buffer.valid(); }
    [[nodiscard]] const Buffer& buffer() const noexcept { return m_buffer; }

private:
    explicit UniformBuffer(Buffer buffer) noexcept : m_buffer(std::move(buffer)) {}

    Buffer m_buffer;
};

} // namespace rv::gl
