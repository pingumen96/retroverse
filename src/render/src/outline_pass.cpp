#include <rv/render/outline_pass.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <glad/gl.h>
#include <glm/vec3.hpp>

#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::gl {
namespace {

constexpr GLuint kStencilUnit = toIndex(OutlineTextureUnit::Stencil);
/// The stencil value the mark writes.
constexpr GLint kMarked = 1;

} // namespace

Expected<OutlinePass> OutlinePass::create() {
    OutlinePass pass;
    auto triangle = VertexArray::create("OutlinePass.FullscreenTriangle");
    if (!triangle) {
        return propagate(triangle.error());
    }
    auto timer = GpuTimer::create("OutlinePass.GpuTime");
    if (!timer) {
        return propagate(timer.error());
    }
    pass.m_triangle = std::move(*triangle);
    pass.m_timer = std::move(*timer);
    return pass;
}

void OutlinePass::beginMark(GLDevice& device, const ViewTargets& targets) {
    if (!valid() || !targets.valid()) {
        return;
    }
    m_timer.begin();
    m_marking = true;
    RenderStateCache& state = device.state();
    const Framebuffer& hdr = targets.hdr();
    hdr.bind();
    const glm::ivec2 size = hdr.size();
    state.setViewport(glm::ivec4(0, 0, size.x, size.y));
    // The stencil starts empty whatever an earlier pass (or another renderer, through the shared GL state) left in it
    // or in its write mask; the depth stays: the mark is tested against the scene.
    glDisable(GL_SCISSOR_TEST);
    glStencilMask(0xFF);
    hdr.clearStencil();
    state.setColorWrite(false);
    state.setDepthTest(true);
    state.setDepthFunc(GL_LEQUAL);
    state.setDepthWrite(false);
    state.setBlend(false);
    state.setPolygonMode(GL_FILL);
    state.setCullMode(GL_BACK);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, kMarked, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

void OutlinePass::drawEdge(GLDevice& device, const ShaderProgram& edge, const ShaderProgram* dim,
                           const OutlineSettings& settings, const ViewTargets& targets, const glm::ivec4& rect) {
    if (!m_marking) {
        return;
    }
    m_marking = false;
    RenderStateCache& state = device.state();
    glDisable(GL_STENCIL_TEST);
    state.setColorWrite(true);
    state.setDepthWrite(true);
    state.setDepthFunc(GL_LESS);

    const Framebuffer& output = targets.output();
    output.bind();
    const glm::ivec2 size = output.size();
    state.setViewport(glm::ivec4(0, 0, size.x, size.y));
    state.setDepthTest(false);
    state.setCullFace(false);
    targets.hdr().bindStencilTexture(kStencilUnit);
    glBindSampler(kStencilUnit, 0); // the view's own nearest filtering: an integer texture takes no linear sampler
    m_triangle.bind();
    const float dimShare = std::isfinite(settings.dimOutside) ? std::clamp(settings.dimOutside, 0.0f, 1.0f) : 0.0f;
    if (dim != nullptr && dimShare > 0.0f) {
        // The inspection's dimming: black blended over everything outside the silhouettes at the share's opacity,
        // GL_FRAMEBUFFER_SRGB on, so that the blend scales the linear light (one stop is 0.5). The default blend
        // function (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GLDevice::state()) does it.
        state.setFramebufferSrgb(true);
        state.setBlend(true);
        dim->use();
        dim->setFloat("uDimShare", dimShare);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (settings.edge) {
        // The colour is written as sRGB bytes, exactly: the output must not encode it again.
        state.setFramebufferSrgb(false);
        state.setBlend(false);
        glEnable(GL_SCISSOR_TEST);
        glScissor(rect.x, rect.y, rect.z, rect.w);
        edge.use();
        edge.setVec3("uOutlineColor",
                     glm::vec3(settings.colorSrgb[0], settings.colorSrgb[1], settings.colorSrgb[2]) / 255.0f);
        edge.setFloat("uOutlineWidth", scene::outlineWidthPixels(size.y, settings.widthAt1080));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    state.setBlend(false);
    glBindTextureUnit(kStencilUnit, 0);
    glDisable(GL_SCISSOR_TEST);
    state.setFramebufferSrgb(true);
    state.setDepthTest(true);
    state.setCullFace(true);
    m_timer.end();
}

} // namespace rv::gl
