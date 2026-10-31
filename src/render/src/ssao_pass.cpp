#include <rv/render/ssao_pass.hpp>

#include <cstddef>
#include <optional>
#include <utility>

#include <glad/gl.h>
#include <glm/vec4.hpp>
#include <spdlog/spdlog.h>

#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/ssao_settings.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::gl {
namespace {

constexpr GLuint kDepthUnit = toIndex(SsaoTextureUnit::SceneDepth);
constexpr GLuint kRawUnit = toIndex(SsaoTextureUnit::RawOcclusion);

bool usable(const ShaderProgram* program) noexcept {
    return program != nullptr && program->valid();
}

/// Binds `target` for drawing with a viewport of its size.
void bindTarget(RenderStateCache& state, const Framebuffer& target) {
    target.bind();
    const glm::ivec2 size = target.size();
    state.setViewport(glm::ivec4(0, 0, size.x, size.y));
}

} // namespace

Expected<SsaoPass> SsaoPass::create() {
    SsaoPass pass;
    auto uniforms = UniformBuffer<SsaoUniforms>::create("SsaoPass.SsaoUniforms");
    if (!uniforms) {
        return propagate(uniforms.error());
    }
    auto triangle = VertexArray::create("SsaoPass.FullscreenTriangle");
    if (!triangle) {
        return propagate(triangle.error());
    }
    SamplingDesc sampling;
    sampling.wrap = TextureWrap::ClampToEdge;
    sampling.filter = TextureFilter::Bilinear;
    auto sampler = Sampler::create(sampling, "SsaoPass.DepthBilinearClamp");
    if (!sampler) {
        return propagate(sampler.error());
    }
    auto timer = GpuTimer::create("SsaoPass.GpuTime");
    if (!timer) {
        return propagate(timer.error());
    }
    pass.m_uniforms = std::move(*uniforms);
    pass.m_triangle = std::move(*triangle);
    pass.m_depthSampler = std::move(*sampler);
    pass.m_timer = std::move(*timer);
    return pass;
}

bool SsaoPass::updateUniforms(const scene::SsaoSetup& setup) {
    if (m_uploaded == setup) {
        return true;
    }
    if (auto updated = m_uniforms.update(makeSsaoUniforms(setup)); !updated) {
        m_uploaded.reset();
        if (m_uniformFailures.fail(updated.error().code)) {
            // Logged once per cause: a persistent failure would otherwise flood the log at the frame rate.
            spdlog::error("Views are drawn without ambient occlusion until this failure stops (repeats are not "
                          "logged): {}",
                          updated.error().toString());
        }
        return false;
    }
    if (const std::optional<std::size_t> failed = m_uniformFailures.succeed()) {
        spdlog::info("Views have ambient occlusion again after {} view(s) without it", *failed);
    }
    m_uploaded = setup;
    return true;
}

void SsaoPass::apply(GLDevice& device, const SsaoShaders& shaders, const scene::SsaoSetup& setup,
                     const ViewTargets& targets) {
    if (!valid() || !targets.valid()) {
        return;
    }
    const ScopedDebugGroup group(device, "AmbientOcclusion");
    m_timer.begin();
    RenderStateCache& state = device.state();
    const Framebuffer& occlusion = targets.ambientOcclusion();
    const bool enabled = setup.enabled && usable(shaders.occlusion) && usable(shaders.blur);
    if (!enabled || !updateUniforms(setup)) {
        // Nothing occluded: the lit shader reads 1 everywhere.
        occlusion.clear(glm::vec4(1.0f));
        bindTarget(state, occlusion);
        m_timer.end();
        return;
    }
    // Fullscreen triangles cover their targets: no depth test (the scene's depth is their input), no culling.
    state.setDepthTest(false);
    state.setCullFace(false);
    state.setBlend(false);
    state.setPolygonMode(GL_FILL);
    m_uniforms.bind(UniformBinding::Ssao);
    m_triangle.bind();
    targets.hdr().bindDepthTexture(kDepthUnit);
    glBindSampler(kDepthUnit, m_depthSampler.id());

    // 1. The half-resolution estimate; its target is not sampled while it is drawn.
    glBindTextureUnit(kRawUnit, 0);
    const Framebuffer& raw = targets.ambientOcclusionRaw();
    bindTarget(state, raw);
    shaders.occlusion->use();
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 2. The depth-aware blur to the resolution of the view. The raw occlusion's storage is the first bloom level's
    // once the blur is over (viewTargetPlan() gives the lifetimes): reading the raw occlusion after this pass is
    // forbidden, and its unit is unbound right after it.
    raw.bindColorTexture(kRawUnit);
    bindTarget(state, occlusion);
    shaders.blur->use();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTextureUnit(kRawUnit, 0);

    state.setDepthTest(true);
    state.setCullFace(true);
    m_timer.end();
}

} // namespace rv::gl
