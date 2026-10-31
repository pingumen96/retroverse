#include <rv/render/panel_pass.hpp>

#include <utility>

#include <rv/render/framebuffer.hpp>
#include <rv/render/render_state_cache.hpp>
#include <rv/render/shader_program.hpp>

namespace rv::gl {
namespace {

/// The picture's texture unit: one no other pass binds (materials 0-4, post 5-7, the scene's 8-9, the outline's 13).
constexpr GLuint kPanelUnit = 14;

} // namespace

Expected<PanelPass> PanelPass::create() {
    auto quad = VertexArray::create("PanelPass.Quad");
    if (!quad) {
        return propagate(quad.error());
    }
    return PanelPass(std::move(*quad));
}

void PanelPass::draw(const ShaderProgram& program, RenderStateCache& state, const Framebuffer& target,
                     const glm::mat4& viewProjection, std::span<const Panel> panels) const {
    if (!target.valid() || panels.empty()) {
        return;
    }
    target.bind();
    const glm::ivec2 size = target.size();
    state.setViewport(glm::ivec4(0, 0, size.x, size.y));
    // As the UI over the window: in the display's encoding, premultiplied.
    state.setFramebufferSrgb(false);
    state.setDepthTest(false);
    state.setDepthWrite(false);
    state.setCullFace(false);
    state.setBlend(true);
    state.setBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    program.use();
    program.setMat4("uPanelViewProjection", viewProjection);
    m_quad.bind();
    glBindSampler(kPanelUnit, 0);
    for (const Panel& panel : panels) {
        if (panel.texture == 0 || !(panel.opacity > 0.0f)) {
            continue;
        }
        glBindTextureUnit(kPanelUnit, panel.texture);
        program.setMat4("uPanelModel", panel.model);
        program.setFloat("uPanelOpacity", panel.opacity < 1.0f ? panel.opacity : 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindTextureUnit(kPanelUnit, 0);
    state.setBlend(false);
}

} // namespace rv::gl
