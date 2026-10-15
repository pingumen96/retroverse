#include <rv/render/post_processor.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>

#include <glad/gl.h>
#include <glm/vec4.hpp>
#include <spdlog/spdlog.h>

#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::gl {
namespace {

constexpr GLuint kSourceUnit = toIndex(PostTextureUnit::Source);
constexpr GLuint kBloomUnit = toIndex(PostTextureUnit::Bloom);
constexpr GLuint kEmissionUnit = toIndex(PostTextureUnit::Emission);
constexpr std::size_t kEmissionAttachment = toIndex(SceneOutput::Emission);

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

Expected<PostProcessor> PostProcessor::create() {
    PostProcessor processor;
    auto uniforms = UniformBuffer<PostUniforms>::create("PostProcessor.PostUniforms");
    if (!uniforms) {
        return propagate(uniforms.error());
    }
    auto triangle = VertexArray::create("PostProcessor.FullscreenTriangle");
    if (!triangle) {
        return propagate(triangle.error());
    }
    SamplingDesc sampling;
    sampling.wrap = TextureWrap::ClampToEdge;
    sampling.filter = TextureFilter::Bilinear;
    auto sampler = Sampler::create(sampling, "PostProcessor.BilinearClamp");
    if (!sampler) {
        return propagate(sampler.error());
    }
    auto timer = GpuTimer::create("PostProcessor.GpuTime");
    if (!timer) {
        return propagate(timer.error());
    }
    processor.m_uniforms = std::move(*uniforms);
    processor.m_triangle = std::move(*triangle);
    processor.m_sampler = std::move(*sampler);
    processor.m_timer = std::move(*timer);
    return processor;
}

void PostProcessor::drawPass(GLDevice& device, const ShaderProgram& program, const Framebuffer& source,
                             const Framebuffer& target) const {
    bindTarget(device.state(), target);
    source.bindColorTexture(kSourceUnit);
    program.use();
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void PostProcessor::bloom(GLDevice& device, const PostShaders& shaders, float scatter,
                          const ViewTargets& targets) const {
    const ScopedDebugGroup group(device, "Bloom");
    const std::span<const Framebuffer, kBloomLevelCount> levels = targets.bloomLevels();
    // Downsampling: the light of the scene that blooms (its radiance and emission) into the first level (Karis
    // average), then each level into the next smaller one.
    drawPass(device, *shaders.bloomDownsampleFirst, targets.hdr(), levels[0]);
    for (std::size_t level = 1; level < kBloomLevelCount; ++level) {
        drawPass(device, *shaders.bloomDownsample, levels[level - 1], levels[level]);
    }
    // Upsampling from the smallest level up: the blend mixes the tent upsampling of the next smaller level into each
    // level, level = upsampled * scatter + level * (1 - scatter).
    RenderStateCache& state = device.state();
    state.setBlend(true);
    state.setBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    state.setBlendColor(glm::vec4(0.0f, 0.0f, 0.0f, scatter));
    for (std::size_t level = kBloomLevelCount - 1; level > 0; --level) {
        drawPass(device, *shaders.bloomUpsample, levels[level], levels[level - 1]);
    }
    state.setBlend(false);
    state.setBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void PostProcessor::apply(GLDevice& device, const PostShaders& shaders, const PostSettings& settings,
                          const ViewTargets& targets) {
    if (!valid() || !targets.valid()) {
        return;
    }
    const ScopedDebugGroup group(device, "PostProcess");
    m_timer.begin();
    RenderStateCache& state = device.state();
    // Fullscreen triangles cover their targets: no depth test (the scene's depth is not theirs), no culling.
    state.setDepthTest(false);
    state.setCullFace(false);
    state.setBlend(false);
    state.setPolygonMode(GL_FILL);

    PostUniforms uniforms = makePostUniforms(settings);
    const bool bloomAvailable =
        usable(shaders.bloomDownsampleFirst) && usable(shaders.bloomDownsample) && usable(shaders.bloomUpsample);
    if (!bloomAvailable) {
        uniforms.bloomMix = 0.0f; // the post pass then leaves the bloom levels unread
    }
    // Inspection views (no tonemap) are shown as their shaders wrote them, like views without the post program.
    bool uniformsWritten = false;
    if (settings.tonemap) {
        if (auto updated = m_uniforms.update(uniforms); updated) {
            uniformsWritten = true;
            if (const std::optional<std::size_t> failed = m_uniformFailures.succeed()) {
                spdlog::info("Views are post-processed again after {} view(s) without post-processing", *failed);
            }
        } else if (m_uniformFailures.fail(updated.error().code)) {
            // Logged once per cause: a persistent failure would otherwise flood the log at the frame rate.
            spdlog::error("Views are shown without post-processing until this failure stops (repeats are not "
                          "logged): {}",
                          updated.error().toString());
        }
    }

    const Framebuffer& output = targets.output();
    if (uniformsWritten && usable(shaders.post)) {
        m_uniforms.bind(UniformBinding::Post);
        m_triangle.bind();
        glBindSampler(kSourceUnit, m_sampler.id());
        glBindSampler(kBloomUnit, m_sampler.id());
        glBindSampler(kEmissionUnit, m_sampler.id());
        // The emitted light, read by the first downsample and the post pass.
        targets.hdr().bindColorTexture(kEmissionUnit, kEmissionAttachment);
        if (uniforms.bloomMix > 0.0f) {
            bloom(device, shaders, bloomScatter(settings.bloomRadius), targets);
        }
        const bool antialiasing = settings.antialiasing && usable(shaders.antialiasing);
        {
            const ScopedDebugGroup postGroup(device, "PostPass");
            bindTarget(state, antialiasing ? targets.tonemapped() : output);
            state.setFramebufferSrgb(true);
            targets.hdr().bindColorTexture(kSourceUnit);
            targets.bloomLevels()[0].bindColorTexture(kBloomUnit);
            shaders.post->use();
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (antialiasing) {
            const ScopedDebugGroup antialiasingGroup(device, "AntiAliasing");
            drawPass(device, *shaders.antialiasing, targets.tonemapped(), output);
        }
    } else {
        // The scene as it is, clamped and encoded by the output, so that it stays visible.
        state.setFramebufferSrgb(true);
        targets.hdr().copyColorTo(output);
        bindTarget(state, output);
    }
    state.setDepthTest(true);
    state.setCullFace(true);
    m_timer.end();
}

} // namespace rv::gl
