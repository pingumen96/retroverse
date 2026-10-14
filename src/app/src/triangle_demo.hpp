#pragma once

#include <memory>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/vertex_array.hpp>

namespace rv::gl {
class GLDevice;
} // namespace rv::gl

namespace rv::app {

/// M0 step 3 proof of the GL 4.5 path: one triangle colored with style §3 accents (tungsten, cold blue, arcade
/// magenta), stored in a DSA vertex buffer and drawn with an inline GLSL 450 program, slowly rotating and corrected
/// for the framebuffer aspect ratio. Isolated here because the scene renderer replaces it.
class TriangleDemo {
public:
    /// `encodeSrgbInShader`: the default framebuffer is not sRGB, so the fragment shader encodes its linear output.
    [[nodiscard]] static Expected<std::unique_ptr<TriangleDemo>> create(bool encodeSrgbInShader);

    void update(double deltaSeconds);
    void draw(gl::GLDevice& device, glm::ivec2 framebufferSize);

private:
    TriangleDemo(gl::Buffer vertices, gl::VertexArray vertexArray, gl::ShaderProgram program) noexcept;

    gl::Buffer m_vertices;
    gl::VertexArray m_vertexArray;
    gl::ShaderProgram m_program;
    float m_angleRadians = 0.0f;
};

} // namespace rv::app
