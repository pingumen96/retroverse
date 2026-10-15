#pragma once

#include <utility>

#include <glad/gl.h>

namespace rv::gl {

/// Move-only owner of one OpenGL object name, deleted with `Deleter` (a stateless functor taking the name) when the
/// owner is destroyed, reset or assigned. Zero means "no object". Building block of the RAII resource classes
/// (Framebuffer, Texture2D, Sampler). Main thread only, like every GL call.
template <class Deleter>
class GlObject {
public:
    GlObject() noexcept = default;
    /// Takes ownership of `id` (0 for none).
    explicit GlObject(GLuint id) noexcept : m_id(id) {}
    GlObject(GlObject&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}
    GlObject& operator=(GlObject&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
        }
        return *this;
    }
    GlObject(const GlObject&) = delete;
    GlObject& operator=(const GlObject&) = delete;
    ~GlObject() { reset(); }

    [[nodiscard]] GLuint id() const noexcept { return m_id; }
    [[nodiscard]] bool valid() const noexcept { return m_id != 0; }

    /// Deletes the object, if any.
    void reset() noexcept {
        if (m_id != 0) {
            Deleter{}(m_id);
            m_id = 0;
        }
    }

private:
    GLuint m_id = 0;
};

struct TextureDeleter {
    void operator()(GLuint id) const noexcept { glDeleteTextures(1, &id); }
};
struct RenderbufferDeleter {
    void operator()(GLuint id) const noexcept { glDeleteRenderbuffers(1, &id); }
};
struct FramebufferDeleter {
    void operator()(GLuint id) const noexcept { glDeleteFramebuffers(1, &id); }
};
struct SamplerDeleter {
    void operator()(GLuint id) const noexcept { glDeleteSamplers(1, &id); }
};

using TextureObject = GlObject<TextureDeleter>;
using RenderbufferObject = GlObject<RenderbufferDeleter>;
using FramebufferObject = GlObject<FramebufferDeleter>;
using SamplerObject = GlObject<SamplerDeleter>;

} // namespace rv::gl
