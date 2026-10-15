#include <rv/render/scene_renderer.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/render/debug_draw.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::gl {
namespace {

bool usable(const ShaderProgram* program) noexcept {
    return program != nullptr && program->valid();
}

/// Writes `value` into `buffer` unless `uploaded` already holds the same bytes (the std140 mirrors have no implicit
/// padding: equal bytes mean equal contents); false when it cannot be written.
template <class Block>
bool updateIfChanged(UniformBuffer<Block>& buffer, std::optional<Block>& uploaded, const Block& value,
                     const char* name) {
    if (uploaded && std::memcmp(&*uploaded, &value, sizeof(Block)) == 0) {
        return true;
    }
    if (auto updated = buffer.update(value); !updated) {
        uploaded.reset();
        spdlog::error("{} update failed: {}", name, updated.error().toString());
        return false;
    }
    uploaded = value;
    return true;
}

/// The frame block of a camera or light view.
FrameUniforms frameUniforms(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& eye, float time) {
    FrameUniforms frame;
    frame.view = view;
    frame.projection = projection;
    frame.viewProjection = projection * view;
    frame.cameraPosition = eye;
    frame.time = time;
    return frame;
}

} // namespace

Expected<SceneRenderer> SceneRenderer::create(MeshRegistry& meshes, const MaterialRegistry& materials) {
    SceneRenderer renderer(meshes, materials);
    auto frame = UniformBuffer<FrameUniforms>::create("SceneRenderer.FrameUniforms");
    if (!frame) {
        return propagate(frame.error());
    }
    auto shadowFrame = UniformBuffer<FrameUniforms>::create("SceneRenderer.ShadowFrameUniforms");
    if (!shadowFrame) {
        return propagate(shadowFrame.error());
    }
    auto lights = UniformBuffer<LightsUniforms>::create("SceneRenderer.LightsUniforms");
    if (!lights) {
        return propagate(lights.error());
    }
    auto shadow = UniformBuffer<ShadowUniforms>::create("SceneRenderer.ShadowUniforms");
    if (!shadow) {
        return propagate(shadow.error());
    }
    auto instances = InstanceBuffer::create(kInitialInstanceCapacity, "SceneRenderer.Instances");
    if (!instances) {
        return propagate(instances.error());
    }
    auto shadowMap = ShadowMap::create(kShadowMapResolution, "SceneRenderer.ShadowMap");
    if (!shadowMap) {
        return propagate(shadowMap.error());
    }
    auto shadowTimer = GpuTimer::create("SceneRenderer.ShadowGpuTime");
    if (!shadowTimer) {
        return propagate(shadowTimer.error());
    }
    auto depthTimer = GpuTimer::create("SceneRenderer.DepthGpuTime");
    if (!depthTimer) {
        return propagate(depthTimer.error());
    }
    auto timer = GpuTimer::create("SceneRenderer.GpuTime");
    if (!timer) {
        return propagate(timer.error());
    }
    auto ssao = SsaoPass::create();
    if (!ssao) {
        return propagate(ssao.error());
    }
    auto post = PostProcessor::create();
    if (!post) {
        return propagate(post.error());
    }
    renderer.m_frameUniforms = std::move(*frame);
    renderer.m_shadowFrameUniforms = std::move(*shadowFrame);
    renderer.m_lightsUniforms = std::move(*lights);
    renderer.m_shadowUniforms = std::move(*shadow);
    renderer.m_instances = std::move(*instances);
    renderer.m_shadowMap = std::move(*shadowMap);
    renderer.m_shadowTimer = std::move(*shadowTimer);
    renderer.m_depthTimer = std::move(*depthTimer);
    renderer.m_timer = std::move(*timer);
    renderer.m_ssao = std::move(*ssao);
    renderer.m_post = std::move(*post);
    return renderer;
}

bool SceneRenderer::updateUniforms(const RenderView& view, const scene::Camera& camera,
                                   const std::optional<ShadowProjection>& shadow) {
    if (auto updated = m_frameUniforms.update(
            frameUniforms(camera.view(), camera.projection(), camera.position(), view.timeSeconds));
        !updated) {
        spdlog::error("FrameUniforms update failed: {}", updated.error().toString());
        return false;
    }
    if (!updateIfChanged(m_lightsUniforms, m_uploadedLights, view.lights, "LightsUniforms")) {
        return false;
    }
    // Without a shadow map this view, no light is shadowed (lightIndex kNoShadowLight).
    const ShadowUniforms shadowBlock =
        shadow ? makeShadowUniforms(*shadow, view.shadowLight, view.shadowSettings, m_shadowMap.resolution())
               : ShadowUniforms{};
    if (!updateIfChanged(m_shadowUniforms, m_uploadedShadow, shadowBlock, "ShadowUniforms")) {
        return false;
    }
    // The depth programs read the light's transforms only: a fixed time keeps the block of a static light unchanged, so
    // it is written once.
    if (shadow && !updateIfChanged(m_shadowFrameUniforms, m_uploadedShadowFrame,
                                   frameUniforms(shadow->view, shadow->projection, shadow->eye, 0.0f),
                                   "ShadowFrameUniforms")) {
        return false;
    }
    m_frameUniforms.bind(UniformBinding::Frame);
    m_lightsUniforms.bind(UniformBinding::Lights);
    m_shadowUniforms.bind(UniformBinding::Shadow);
    return true;
}

Expected<std::uint32_t> SceneRenderer::uploadInstances(bool withShadow) {
    const std::array<std::span<const glm::mat4>, 2> parts{
        std::span<const glm::mat4>(m_plan.instanceTransforms),
        withShadow ? std::span<const glm::mat4>(m_shadowPlan.instanceTransforms) : std::span<const glm::mat4>()};
    auto baseInstance = m_instances.write(std::span<const std::span<const glm::mat4>>(parts));
    if (!baseInstance) {
        Error error = baseInstance.error();
        error.message = "the instances cannot be written: " + error.message;
        return propagate(std::move(error));
    }
    if (auto attached = m_meshes->attachInstanceBuffer(m_instances); !attached) {
        Error error = attached.error();
        error.message = "the meshes cannot read the instance buffer: " + error.message;
        return propagate(std::move(error));
    }
    return *baseInstance;
}

void SceneRenderer::drawDepthBatches(GLDevice& device, const SceneShaders& shaders, const scene::BatchPlan& plan,
                                     std::uint32_t baseInstance, DrawCounts& counts) const {
    RenderStateCache& state = device.state();
    const ShaderProgram* currentProgram = nullptr;
    std::optional<scene::MaterialHandle> currentMaterial;
    for (const scene::Batch& batch : plan.batches) {
        if (batch.pass == scene::DrawPass::Blend || !m_meshes->contains(batch.mesh) ||
            batch.material.value >= m_materials->uploadedCount()) {
            continue;
        }
        // Opaque batches read no material: only alpha-masked ones bind theirs, for the alpha test.
        const bool masked = batch.pass == scene::DrawPass::Mask;
        const ShaderProgram* program = masked ? shaders.depthMasked : shaders.depth;
        if (program != currentProgram) {
            program->use();
            currentProgram = program;
            ++counts.shaderSwitches;
        }
        if (masked && currentMaterial != batch.material) {
            m_materials->bind(batch.material);
            currentMaterial = batch.material;
            ++counts.materialSwitches;
        }
        state.setCullFace(!m_materials->material(batch.material).doubleSided);
        const GpuMesh& mesh = m_meshes->mesh(batch.mesh);
        mesh.drawInstanced(batch.instanceCount, baseInstance + batch.firstInstance);
        ++counts.drawCalls;
        counts.instances += batch.instanceCount;
        counts.triangles += batch.instanceCount * mesh.triangleCount();
    }
}

void SceneRenderer::drawShadedBatches(GLDevice& device, const SceneShaders& shaders, std::uint32_t baseInstance,
                                      bool afterPrePass, DrawCounts& counts) const {
    RenderStateCache& state = device.state();
    const ShaderProgram* currentProgram = nullptr;
    std::optional<scene::MaterialHandle> currentMaterial;
    for (const scene::Batch& batch : m_plan.batches) {
        if (!m_meshes->contains(batch.mesh) || batch.material.value >= m_materials->uploadedCount()) {
            continue;
        }
        const ShaderProgram* program = shaders.inspection != nullptr              ? shaders.inspection
                                       : batch.shader == scene::ShaderKind::Unlit ? shaders.unlit
                                                                                  : shaders.pbr;
        if (program == nullptr || !program->valid()) {
            continue;
        }
        if (program != currentProgram) {
            program->use();
            currentProgram = program;
            ++counts.shaderSwitches;
        }
        if (currentMaterial != batch.material) {
            m_materials->bind(batch.material);
            const bool blend = batch.pass == scene::DrawPass::Blend;
            state.setBlend(blend);
            state.setDepthWrite(!blend && !afterPrePass);
            state.setCullFace(!m_materials->material(batch.material).doubleSided);
            currentMaterial = batch.material;
            ++counts.materialSwitches;
        }
        const GpuMesh& mesh = m_meshes->mesh(batch.mesh);
        mesh.drawInstanced(batch.instanceCount, baseInstance + batch.firstInstance);
        ++counts.drawCalls;
        counts.instances += batch.instanceCount;
        counts.triangles += batch.instanceCount * mesh.triangleCount();
    }
}

void SceneRenderer::drawShadowMap(GLDevice& device, const SceneShaders& shaders, std::uint32_t baseInstance,
                                  const ShadowSettings& settings, FrameStats& stats) {
    const ScopedDebugGroup group(device, "ShadowMap");
    m_shadowTimer.begin();
    RenderStateCache& state = device.state();
    m_shadowMap.beginPass(state);
    state.setDepthTest(true);
    state.setDepthFunc(GL_LESS);
    state.setDepthWrite(true);
    state.setBlend(false);
    state.setPolygonMode(GL_FILL);
    state.setCullMode(GL_BACK);
    const glm::vec2 bias = shadowDepthBias(settings);
    state.setPolygonOffset(true, bias.x, bias.y);
    // The depth program places the casters with the light's view.
    m_shadowFrameUniforms.bind(UniformBinding::Frame);
    DrawCounts counts;
    drawDepthBatches(device, shaders, m_shadowPlan, baseInstance, counts);
    m_frameUniforms.bind(UniformBinding::Frame);
    state.setPolygonOffset(false);
    stats.shadowDrawCalls = counts.drawCalls;
    stats.shadowInstances = counts.instances;
    m_shadowTimer.end();
}

void SceneRenderer::render(GLDevice& device, const SceneShaders& shaders, const RenderView& view,
                           const ViewTargets& targets) {
    if (!targets.valid()) {
        return;
    }
    const ScopedDebugGroup group(device, "SceneRenderer");
    RenderStateCache& state = device.state();
    FrameStats stats;
    stats.items = view.items.size();
    scene::Camera camera = view.camera;
    const glm::ivec2 size = targets.size();
    camera.setAspect(static_cast<float>(size.x) / static_cast<float>(size.y));

    // The depth passes (shadow map, pre-pass, hence the ambient occlusion) need both depth programs.
    const bool depthPrograms = usable(shaders.depth) && usable(shaders.depthMasked);
    std::optional<ShadowProjection> shadow;
    if (depthPrograms && view.shadowSettings.enabled && m_shadowMap.valid()) {
        shadow = shadowProjectionFor(view.shadowLight, view.shadowCasterBounds, m_shadowMap.resolution());
    }
    const bool uniformsReady = updateUniforms(view, camera, shadow);
    std::optional<std::uint32_t> baseInstance;
    if (uniformsReady) {
        scene::PlanOptions options;
        options.frustum = camera.frustum();
        options.eye = camera.position();
        options.localBounds = [this](scene::MeshHandle mesh) {
            return m_meshes->bounds(mesh);
        };
        options.passOf = [this](scene::MaterialHandle material) {
            return m_materials->pass(material);
        };
        m_planner.plan(view.items, options, m_plan);
        stats.culledItems = m_plan.culledItems;
        stats.batches = m_plan.batches.size();
        if (shadow) {
            options.frustum = scene::Frustum::fromViewProjection(shadow->viewProjection);
            options.eye = shadow->eye;
            options.shadowCastersOnly = true;
            m_planner.plan(view.items, options, m_shadowPlan);
        }
        if (auto uploaded = uploadInstances(shadow.has_value()); uploaded) {
            if (const std::optional<std::size_t> skipped = m_uploadFailures.succeed()) {
                spdlog::info("Scene views are drawn again after {} view(s) were skipped", *skipped);
            }
            baseInstance = *uploaded;
        } else if (m_uploadFailures.fail(uploaded.error().code)) {
            // Logged once per cause: a persistent failure would otherwise flood the log at the frame rate.
            spdlog::error("Scene views are not drawn until this failure stops (repeats are not logged): {}",
                          uploaded.error().toString());
        }
    }

    // 1. The key light's shadow map; the shadow plan's instances follow the view's.
    const bool shadowDrawn = baseInstance && shadow;
    if (shadowDrawn) {
        drawShadowMap(device, shaders, *baseInstance + static_cast<std::uint32_t>(m_plan.instanceTransforms.size()),
                      view.shadowSettings, stats);
    }

    // 2. The depth pre-pass into the cleared HDR target.
    m_depthTimer.begin();
    device.beginPass(targets.hdr(), view.clearColor);
    const bool prePass = baseInstance && depthPrograms && !view.wireframe;
    if (prePass) {
        const ScopedDebugGroup depthGroup(device, "DepthPrePass");
        state.setColorWrite(false);
        state.setDepthTest(true);
        state.setDepthFunc(GL_LESS);
        state.setBlend(false);
        state.setPolygonMode(GL_FILL);
        state.setCullMode(GL_BACK);
        DrawCounts counts;
        drawDepthBatches(device, shaders, m_plan, *baseInstance, counts);
        state.setColorWrite(true);
        stats.depthDrawCalls = counts.drawCalls;
    }
    m_depthTimer.end();

    // 3. The ambient occlusion of that depth (cleared to "nothing occluded" without a pre-pass).
    scene::SsaoSetup ssao = view.ssao;
    ssao.enabled = ssao.enabled && prePass;
    m_ssao.apply(device, shaders.ssao, ssao, targets);

    // 4. The lit pass.
    m_timer.begin();
    if (baseInstance) {
        const ScopedDebugGroup litGroup(device, "LitPass");
        targets.hdr().bind();
        state.setViewport(glm::ivec4(0, 0, size.x, size.y));
        m_shadowMap.bind(toIndex(SceneTextureUnit::ShadowMap));
        targets.ambientOcclusion().bindColorTexture(toIndex(SceneTextureUnit::AmbientOcclusion));
        state.setDepthTest(true);
        state.setDepthFunc(prePass ? GL_LEQUAL : GL_LESS);
        state.setPolygonMode(view.wireframe ? GL_LINE : GL_FILL);
        state.setCullMode(GL_BACK);
        state.setBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        DrawCounts counts;
        drawShadedBatches(device, shaders, *baseInstance, prePass, counts);
        stats.drawCalls = counts.drawCalls;
        stats.instances = counts.instances;
        stats.triangles = counts.triangles;
        stats.shaderSwitches = counts.shaderSwitches;
        stats.materialSwitches = counts.materialSwitches;
    }
    m_instances.release(); // the draws that read this view's instances are issued
    // Leave the default fixed-function state for the passes that follow.
    state.setPolygonMode(GL_FILL);
    state.setDepthFunc(GL_LESS);
    state.setBlend(false);
    state.setDepthWrite(true);
    state.setCullFace(true);
    m_timer.end();

    m_post.apply(device, shaders.post, view.post, targets);

    if (uniformsReady && view.debugDraw != nullptr && shaders.debugLines != nullptr && shaders.debugLines->valid()) {
        // Over the tonemapped image, in their own colors (encoded by the sRGB output), hidden where the scene is in
        // front of them.
        const ScopedDebugGroup lines(device, "DebugLines");
        const Framebuffer& output = targets.output();
        targets.hdr().copyDepthStencilTo(output);
        output.bind();
        state.setViewport(glm::ivec4(0, 0, size.x, size.y));
        state.setFramebufferSrgb(true);
        view.debugDraw->draw(*shaders.debugLines, state);
        stats.debugLines = view.debugDraw->lineCount();
    }
    // A view without a shadow map reports no shadow time (the timer still holds an earlier view's).
    stats.shadowGpuMilliseconds = shadowDrawn ? m_shadowTimer.latestMilliseconds() : std::nullopt;
    stats.depthGpuMilliseconds = m_depthTimer.latestMilliseconds();
    stats.ssaoGpuMilliseconds = m_ssao.latestGpuMilliseconds();
    stats.sceneGpuMilliseconds = m_timer.latestMilliseconds();
    stats.postGpuMilliseconds = m_post.latestGpuMilliseconds();
    m_stats = stats;
}

} // namespace rv::gl
