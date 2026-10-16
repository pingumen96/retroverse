#pragma once

#include <utility>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/vertex_array.hpp>

namespace rv::gl {
class GLDevice;
} // namespace rv::gl

namespace rv::spikes::launch {

/// The spike window's content: an animated full-window picture (a warm gradient, drifting shelf stripes, a bar that
/// sweeps across in two seconds, so that dropped frames show) with a status band at the top, drawn with rv_render as
/// one fullscreen triangle into the default framebuffer. Cheap on purpose: the frame rate measures the hand-over, not
/// the renderer. Main thread only, while the GL context is current.
class SpikeScene {
public:
    [[nodiscard]] static Expected<SpikeScene> create();

    /// Draws a frame of `size` pixels at `timeSeconds`; `status` colors the band at the top (alpha 0 hides it).
    void render(gl::GLDevice& device, glm::ivec2 size, double timeSeconds, const glm::vec4& status) const;

private:
    SpikeScene(gl::ShaderProgram program, gl::VertexArray vertexArray) noexcept
        : m_program(std::move(program)), m_vertexArray(std::move(vertexArray)) {}

    gl::ShaderProgram m_program;
    gl::VertexArray m_vertexArray;
};

} // namespace rv::spikes::launch
