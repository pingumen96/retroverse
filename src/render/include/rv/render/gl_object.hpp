#pragma once

#include <cstdint>
#include <utility>

#include <glad/gl.h>

namespace rv::gl {

/// Move-only owner of one OpenGL object name, deleted with `Deleter` (a stateless functor taking the name) when the
/// owner is destroyed, reset or assigned. Zero means "no object". Building block of the RAII resource classes
/// (Framebuffer, Texture2D, Sampler, GpuTimer). Main thread only, like every GL call.
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
struct QueryDeleter {
    void operator()(GLuint id) const noexcept { glDeleteQueries(1, &id); }
};

using TextureObject = GlObject<TextureDeleter>;
using RenderbufferObject = GlObject<RenderbufferDeleter>;
using FramebufferObject = GlObject<FramebufferDeleter>;
using SamplerObject = GlObject<SamplerDeleter>;
using QueryObject = GlObject<QueryDeleter>;

/// Move-only owner of a fence sync object (glFenceSync), deleted with glDeleteSync. It marks a point in the command
/// stream: once the GPU has passed it, every command issued before it has completed (buffers it read can be
/// rewritten). Empty by default. Main thread only.
class FenceSync {
public:
    FenceSync() noexcept = default;
    FenceSync(FenceSync&& other) noexcept : m_sync(std::exchange(other.m_sync, nullptr)) {}
    FenceSync& operator=(FenceSync&& other) noexcept {
        if (this != &other) {
            reset();
            m_sync = std::exchange(other.m_sync, nullptr);
        }
        return *this;
    }
    FenceSync(const FenceSync&) = delete;
    FenceSync& operator=(const FenceSync&) = delete;
    ~FenceSync() { reset(); }

    /// Inserts a fence after the commands issued so far; empty when the driver creates none.
    [[nodiscard]] static FenceSync insert() noexcept {
        FenceSync fence;
        fence.m_sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        return fence;
    }

    /// Waits until the GPU has passed the fence for at most `timeoutNanoseconds`, flushing the commands before it so
    /// that it can be reached. True when it was passed, also for an empty fence; false on timeout or failure.
    [[nodiscard]] bool clientWait(std::uint64_t timeoutNanoseconds) const noexcept {
        if (m_sync == nullptr) {
            return true;
        }
        const GLenum result = glClientWaitSync(m_sync, GL_SYNC_FLUSH_COMMANDS_BIT, timeoutNanoseconds);
        return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
    }

    [[nodiscard]] bool valid() const noexcept { return m_sync != nullptr; }

    /// Deletes the fence, if any.
    void reset() noexcept {
        if (m_sync != nullptr) {
            glDeleteSync(m_sync);
            m_sync = nullptr;
        }
    }

private:
    GLsync m_sync = nullptr;
};

} // namespace rv::gl
