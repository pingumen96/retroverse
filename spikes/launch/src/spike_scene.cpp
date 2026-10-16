#include "spike_scene.hpp"

#include <cmath>
#include <string_view>
#include <utility>

#include <glad/gl.h>

#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>

namespace rv::spikes::launch {
namespace {

constexpr std::string_view kVertexShader = R"(#version 450 core
// One triangle covering the window, without vertex buffers.
void main() {
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);
}
)";

constexpr std::string_view kFragmentShader = R"(#version 450 core
layout(location = 0) out vec4 outColor;
uniform vec2 uResolution;
uniform float uTime;
uniform vec4 uStatus;

void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;
    // A dim, warm room: brighter towards the floor.
    vec3 color = mix(vec3(0.18, 0.09, 0.04), vec3(0.03, 0.02, 0.02), uv.y);
    // Shelf boards drifting slowly to the left.
    float board = smoothstep(0.0, 0.004, fract(uv.y * 6.0) - 0.92) * 0.25;
    float slots = step(0.5, fract(uv.x * 24.0 + uTime * 0.2)) * 0.05;
    color += vec3(0.55, 0.35, 0.18) * (board + slots * step(0.2, uv.y));
    // A bar sweeping across in two seconds: a dropped frame makes it jump.
    float sweep = fract(uTime * 0.5);
    color = mix(color, vec3(0.95, 0.85, 0.55), step(abs(uv.x - sweep), 0.006));
    // The status band.
    color = mix(color, uStatus.rgb, step(0.97, uv.y) * uStatus.a);
    outColor = vec4(color, 1.0);
}
)";

} // namespace

Expected<SpikeScene> SpikeScene::create() {
    auto program = gl::ShaderProgram::build(kVertexShader, kFragmentShader, "spike_launch.scene");
    if (!program) {
        return propagate(program.error());
    }
    auto vertexArray = gl::VertexArray::create("spike_launch.fullscreen");
    if (!vertexArray) {
        return propagate(vertexArray.error());
    }
    return SpikeScene(std::move(*program), std::move(*vertexArray));
}

void SpikeScene::render(gl::GLDevice& device, glm::ivec2 size, double timeSeconds, const glm::vec4& status) const {
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    gl::RenderStateCache& state = device.state();
    gl::Framebuffer::clearDefault(state, size);
    state.setDepthTest(false);
    state.setDepthWrite(false);
    state.setCullFace(false);
    state.setBlend(false);
    m_program.setVec2("uResolution", glm::vec2(size));
    // Wrapped, so that a long run keeps the float precise.
    m_program.setFloat("uTime", static_cast<float>(std::fmod(timeSeconds, 3600.0)));
    m_program.setVec4("uStatus", status);
    m_program.use();
    m_vertexArray.bind();
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace rv::spikes::launch
