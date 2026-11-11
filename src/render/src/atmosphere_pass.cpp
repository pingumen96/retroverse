#include <rv/render/atmosphere_pass.hpp>

#include <utility>

#include <glad/gl.h>
#include <spdlog/spdlog.h>

#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::gl {
namespace {

bool usable(const ShaderProgram* program) {
    return program != nullptr && program->valid();
}

} // namespace

Expected<AtmospherePass> AtmospherePass::create() {
    AtmospherePass pass;
    auto empty = VertexArray::create("AtmospherePass.Empty");
    if (!empty) {
        return propagate(empty.error());
    }
    auto uniforms = UniformBuffer<AtmosphereUniforms>::create("AtmospherePass.Uniforms");
    if (!uniforms) {
        return propagate(uniforms.error());
    }
    auto timer = GpuTimer::create("AtmospherePass.GpuTime");
    if (!timer) {
        return propagate(timer.error());
    }
    pass.m_empty = std::move(*empty);
    pass.m_uniforms = std::move(*uniforms);
    pass.m_timer = std::move(*timer);
    return pass;
}

bool AtmospherePass::draw(GLDevice& device, const AtmosphereShaders& shaders, const AtmosphereFrame& frame,
                          const ViewTargets& targets) {
    const bool dust = frame.motes > 0 && usable(shaders.dust);
    const bool cones = frame.cones > 0 && usable(shaders.cones);
    if (!valid() || !targets.valid() || (!dust && !cones)) {
        return false;
    }
    if (auto updated = m_uniforms.update(frame.uniforms); !updated) {
        spdlog::warn("Atmosphere: the view's block cannot be written: {}", updated.error().toString());
        return false;
    }
    const ScopedDebugGroup group(device, "Atmosphere");
    m_timer.begin();
    RenderStateCache& state = device.state();
    const Framebuffer& hdr = targets.hdr();
    hdr.bind();
    const glm::ivec2 size = hdr.size();
    state.setViewport(glm::ivec4(0, 0, size.x, size.y));
    state.setDepthTest(true);
    state.setDepthFunc(GL_LEQUAL);
    state.setDepthWrite(false);
    state.setPolygonMode(GL_FILL);
    state.setCullFace(false);
    state.setBlend(true);
    state.setBlendFunc(GL_ONE, GL_ONE);
    m_uniforms.bind(UniformBinding::Atmosphere);
    m_empty.bind();
    if (cones) {
        shaders.cones->use();
        glDrawArraysInstanced(GL_TRIANGLES, 0, kAtmosphereConeVertices,
                              static_cast<GLsizei>(frame.uniforms.emitterCount));
    }
    if (dust) {
        shaders.dust->use();
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(frame.motes));
    }
    state.setBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    state.setBlend(false);
    state.setDepthWrite(true);
    state.setDepthFunc(GL_LESS);
    state.setCullFace(true);
    m_timer.end();
    return true;
}

} // namespace rv::gl
