#pragma once

#include <string>
#include <string_view>

#include <glad/gl.h>

#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::gl {

class RenderStateCache;

/// Move-only depth target of the key light's shadow (DSA): a square GL_DEPTH_COMPONENT32F texture with immutable
/// storage in a framebuffer without color buffers, and the comparison sampler the lit shader reads it with
/// (sampler2DShadow in common/shadow.glsl): GL_COMPARE_REF_TO_TEXTURE with GL_LEQUAL (1 where the reference depth is
/// not behind the stored one) and bilinear filtering, so every fetch is a 2 x 2 percentage-closer filter; clamped to a
/// border of depth 1, so nothing is in shadow outside the map. Its texture is recorded in gpuMemoryLedger() as
/// GpuMemoryKind::Targets. A default-constructed or moved-from ShadowMap is empty. Main thread only.
class ShadowMap {
public:
    ShadowMap() noexcept = default;

    /// Allocates a `resolution` x `resolution` map labeled `debugName` (".depth", ".comparison" for its texture and
    /// sampler). Errors: OutOfRange for a resolution outside [1, GL_MAX_TEXTURE_SIZE], GraphicsError when the storage
    /// cannot be allocated or the framebuffer is incomplete.
    [[nodiscard]] static Expected<ShadowMap> create(int resolution, std::string_view debugName);

    /// Starts drawing casters into the map: binds its framebuffer with a viewport of its size and clears the depth to 1
    /// (depth writes enabled, as clears honor the depth mask). Does nothing for an empty map.
    void beginPass(RenderStateCache& state) const;

    /// Binds the depth texture and the comparison sampler to texture unit `unit` (an empty map unbinds both). Do not
    /// sample it while the map is bound for drawing.
    void bind(GLuint unit) const;

    [[nodiscard]] bool valid() const noexcept { return m_framebuffer.valid(); }
    /// Texels per side.
    [[nodiscard]] int resolution() const noexcept { return m_resolution; }

private:
    TextureObject m_depth;
    SamplerObject m_comparison;
    FramebufferObject m_framebuffer;
    int m_resolution = 0;
    std::string m_debugName;
    GpuMemoryAllocation m_memory;
};

} // namespace rv::gl
