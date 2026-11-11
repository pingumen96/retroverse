#pragma once

// The atmosphere of a view (I12 step 6, ADR-0021 §4): dust motes and faint light cones added into the HDR target's
// radiance after the lit pass's blended batches, before the post-processing, timed on the GPU on its own.

#include <optional>

#include <rv/core/expected.hpp>
#include <rv/render/atmosphere_settings.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/render/vertex_array.hpp>

namespace rv::gl {

class GLDevice;
class ShaderProgram;
class ViewTargets;

/// Programs of the atmosphere. Without one, its part is not drawn.
struct AtmosphereShaders {
    const ShaderProgram* dust = nullptr;  ///< atmosphere/dust.vert + atmosphere/dust.frag
    const ShaderProgram* cones = nullptr; ///< atmosphere/cone.vert + atmosphere/cone.frag
};

/// Light cone vertices of one emitter (atmosphere/cone.vert: 24 segments, 8 rings, two triangles a quad).
inline constexpr int kAtmosphereConeVertices = 24 * 8 * 6;

/// Draws an AtmosphereFrame: its block written to UniformBinding::Atmosphere, then into ViewTargets::hdr() (bound,
/// its viewport set) with the depth test GL_LEQUAL against the scene's depth and no depth writes, blended additively
/// (GL_ONE, GL_ONE) into both outputs (the shaders write no emission and no alpha), both faces drawn: the motes as one
/// instanced triangle strip of four vertices, the cones as one instanced draw of kAtmosphereConeVertices, both from an
/// empty vertex array. Nothing for a frame without motes or cones. Leaves the state defaults documented on
/// GLDevice::state(). Main thread only; destroy it while the GL context is current.
class AtmospherePass {
public:
    AtmospherePass() noexcept = default;
    /// Creates the empty vertex array, the uniform buffer and the GPU timer. Errors: GPU objects that cannot be
    /// created.
    [[nodiscard]] static Expected<AtmospherePass> create();

    /// Draws `frame` into `targets` (above); true when it drew anything.
    bool draw(GLDevice& device, const AtmosphereShaders& shaders, const AtmosphereFrame& frame,
              const ViewTargets& targets);
    /// GPU time of the most recent atmosphere whose timer query has completed (GpuTimer: a frame or two late).
    [[nodiscard]] std::optional<double> latestGpuMilliseconds() { return m_timer.latestMilliseconds(); }
    [[nodiscard]] bool valid() const noexcept { return m_empty.valid() && m_uniforms.valid(); }

private:
    VertexArray m_empty;
    UniformBuffer<AtmosphereUniforms> m_uniforms;
    GpuTimer m_timer;
};

} // namespace rv::gl
