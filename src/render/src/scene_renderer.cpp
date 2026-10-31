#include <rv/render/scene_renderer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/cpu_timers.hpp>
#include <rv/render/debug_draw.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/static_instance_group.hpp>
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

/// True for an instance the outline marks: highlighted and drawn (a hidden one draws nothing).
bool outlined(const scene::InstanceAttributes& attributes) noexcept {
    return (attributes.flags & scene::instance_flags::kHighlighted) != 0 &&
           (attributes.flags & scene::instance_flags::kHidden) == 0;
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
    auto outline = OutlinePass::create();
    if (!outline) {
        return propagate(outline.error());
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
    renderer.m_outline = std::move(*outline);
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
    using Instances = std::span<const scene::InstanceData>;
    const std::array<Instances, 2> parts{Instances(m_plan.instances),
                                         withShadow ? Instances(m_shadowPlan.instances) : Instances()};
    auto baseInstance = m_instances.write(std::span<const Instances>(parts));
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

bool SceneRenderer::bindDepth(GLDevice& device, const SceneShaders& shaders, scene::DrawPass pass,
                              scene::MaterialHandle material, BoundState& bound, DrawCounts& counts) const {
    if (pass == scene::DrawPass::Blend || material.value >= m_materials->uploadedCount()) {
        return false;
    }
    // Opaque batches read no material: only alpha-masked ones bind theirs, for the alpha test.
    const bool masked = pass == scene::DrawPass::Mask;
    const ShaderProgram* program = masked ? shaders.depthMasked : shaders.depth;
    if (program != bound.program) {
        program->use();
        bound.program = program;
        ++counts.shaderSwitches;
    }
    if (masked && bound.material != material) {
        m_materials->bind(material);
        bound.material = material;
        ++counts.materialSwitches;
    }
    device.state().setCullFace(!m_materials->material(material).doubleSided);
    return true;
}

bool SceneRenderer::bindShaded(GLDevice& device, const SceneShaders& shaders, scene::DrawPass pass,
                               scene::ShaderKind shader, scene::MaterialHandle material, bool afterPrePass,
                               BoundState& bound, DrawCounts& counts) const {
    if (material.value >= m_materials->uploadedCount()) {
        return false;
    }
    const ShaderProgram* program = shaders.inspection != nullptr        ? shaders.inspection
                                   : shader == scene::ShaderKind::Unlit ? shaders.unlit
                                                                        : shaders.pbr;
    if (!usable(program)) {
        return false;
    }
    if (program != bound.program) {
        program->use();
        bound.program = program;
        ++counts.shaderSwitches;
    }
    if (bound.material != material) {
        m_materials->bind(material);
        RenderStateCache& state = device.state();
        const bool blend = pass == scene::DrawPass::Blend;
        state.setBlend(blend);
        state.setDepthWrite(!blend && !afterPrePass);
        state.setCullFace(!m_materials->material(material).doubleSided);
        bound.material = material;
        ++counts.materialSwitches;
    }
    return true;
}

void SceneRenderer::drawDepthBatches(GLDevice& device, const SceneShaders& shaders, const scene::BatchPlan& plan,
                                     std::uint32_t baseInstance, BoundState& bound, DrawCounts& counts) const {
    for (const scene::Batch& batch : plan.batches) {
        if (!m_meshes->contains(batch.mesh) || !bindDepth(device, shaders, batch.pass, batch.material, bound, counts)) {
            continue;
        }
        const GpuMesh& mesh = m_meshes->mesh(batch.mesh);
        mesh.drawInstanced(batch.instanceCount, baseInstance + batch.firstInstance);
        ++counts.drawCalls;
        counts.instances += batch.instanceCount;
        counts.triangles += batch.instanceCount * mesh.triangleCount();
    }
}

void SceneRenderer::drawStaticDepth(GLDevice& device, const SceneShaders& shaders, const RenderView& view,
                                    const std::vector<scene::StaticDrawList>& draws, BoundState& bound,
                                    DrawCounts& counts) const {
    for (std::size_t index = 0; index < view.staticGroups.size() && index < draws.size(); ++index) {
        const StaticInstanceGroup* group = view.staticGroups[index];
        if (group == nullptr) {
            continue;
        }
        const std::span<const scene::StaticBatch> batches = group->layout().batches();
        for (const scene::StaticDraw& draw : draws[index].draws) {
            const scene::StaticBatch& batch = batches[draw.batch];
            if (!bindDepth(device, shaders, batch.pass, batch.material, bound, counts)) {
                continue;
            }
            group->draw(draw);
            ++counts.drawCalls;
            counts.instances += draw.instanceCount;
            counts.triangles += draw.instanceCount * group->meshOf(draw.batch).triangleCount();
        }
    }
}

void SceneRenderer::drawShadedBatches(GLDevice& device, const SceneShaders& shaders, std::uint32_t baseInstance,
                                      bool afterPrePass, bool blended, BoundState& bound, DrawCounts& counts) const {
    for (const scene::Batch& batch : m_plan.batches) {
        if ((batch.pass == scene::DrawPass::Blend) != blended || !m_meshes->contains(batch.mesh) ||
            !bindShaded(device, shaders, batch.pass, batch.shader, batch.material, afterPrePass, bound, counts)) {
            continue;
        }
        const GpuMesh& mesh = m_meshes->mesh(batch.mesh);
        mesh.drawInstanced(batch.instanceCount, baseInstance + batch.firstInstance);
        ++counts.drawCalls;
        counts.instances += batch.instanceCount;
        counts.triangles += batch.instanceCount * mesh.triangleCount();
    }
}

void SceneRenderer::drawStaticShaded(GLDevice& device, const SceneShaders& shaders, const RenderView& view,
                                     bool afterPrePass, BoundState& bound, DrawCounts& counts) const {
    for (std::size_t index = 0; index < view.staticGroups.size() && index < m_staticDraws.size(); ++index) {
        const StaticInstanceGroup* group = view.staticGroups[index];
        if (group == nullptr) {
            continue;
        }
        const std::span<const scene::StaticBatch> batches = group->layout().batches();
        for (const scene::StaticDraw& draw : m_staticDraws[index].draws) {
            const scene::StaticBatch& batch = batches[draw.batch];
            if (!bindShaded(device, shaders, batch.pass, batch.shader, batch.material, afterPrePass, bound, counts)) {
                continue;
            }
            group->draw(draw);
            ++counts.drawCalls;
            counts.instances += draw.instanceCount;
            counts.triangles += draw.instanceCount * group->meshOf(draw.batch).triangleCount();
        }
    }
}

void SceneRenderer::drawShadowMap(GLDevice& device, const SceneShaders& shaders, const RenderView& view,
                                  std::uint32_t baseInstance, const ShadowSettings& settings, FrameStats& stats) {
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
    BoundState bound;
    drawDepthBatches(device, shaders, m_shadowPlan, baseInstance, bound, counts);
    drawStaticDepth(device, shaders, view, m_staticShadowDraws, bound, counts);
    m_frameUniforms.bind(UniformBinding::Frame);
    state.setPolygonOffset(false);
    stats.shadowDrawCalls = counts.drawCalls;
    stats.shadowInstances = counts.instances;
    m_shadowTimer.end();
}

void SceneRenderer::planStaticGroups(const RenderView& view, const scene::Frustum& viewFrustum,
                                     const std::optional<scene::Frustum>& shadowFrustum, FrameStats& stats) {
    const std::size_t count = view.staticGroups.size();
    if (m_staticDraws.size() < count) {
        m_staticDraws.resize(count);
        m_staticShadowDraws.resize(count);
    }
    if (count == 0) {
        return;
    }
    const core::CpuSectionScope timing(core::cpu_section::kCollectionPlanning);
    for (std::size_t index = 0; index < count; ++index) {
        scene::StaticDrawList& draws = m_staticDraws[index];
        scene::StaticDrawList& shadowDraws = m_staticShadowDraws[index];
        draws.draws.clear();
        shadowDraws.draws.clear();
        const StaticInstanceGroup* group = view.staticGroups[index];
        if (group == nullptr) {
            continue;
        }
        const scene::InstanceClusters& layout = group->layout();
        layout.plan(&viewFrustum, false, draws);
        if (shadowFrustum) {
            layout.plan(&*shadowFrustum, true, shadowDraws);
        }
        ++stats.staticGroups;
        stats.staticBatches += layout.batches().size();
        stats.clusters += draws.clusters;
        stats.culledClusters += draws.culledClusters;
    }
}

std::optional<glm::ivec4> SceneRenderer::collectOutlineTargets(const RenderView& view, const scene::Camera& camera,
                                                               glm::ivec2 size) {
    m_outlineTargets.clear();
    const glm::mat4 viewProjection = camera.viewProjection();
    const int margin = scene::outlineReach(scene::outlineWidthPixels(size.y, view.outline.widthAt1080)) + 1;
    std::optional<glm::ivec4> rect;
    for (std::uint32_t batchIndex = 0; batchIndex < m_plan.batches.size(); ++batchIndex) {
        const scene::Batch& batch = m_plan.batches[batchIndex];
        for (std::uint32_t record = batch.firstInstance; record < batch.firstInstance + batch.instanceCount;
             ++record) {
            const scene::InstanceData& instance = m_plan.instances[record];
            if (!outlined(instance.attributes)) {
                continue;
            }
            m_outlineTargets.push_back(OutlineTarget{nullptr, batchIndex, record});
            const core::Aabb local = m_meshes->bounds(batch.mesh);
            // A mesh of unknown bounds could cover any pixel.
            rect = scene::unionRect(rect, local.isEmpty()
                                              ? std::optional<glm::ivec4>(glm::ivec4(0, 0, size.x, size.y))
                                              : scene::screenRect(local.transformed(instance.model), viewProjection,
                                                                  size, margin));
        }
    }
    for (const StaticInstanceGroup* group : view.staticGroups) {
        if (group == nullptr) {
            continue;
        }
        const scene::InstanceClusters& layout = group->layout();
        for (const std::uint32_t record : layout.highlighted()) {
            if (!outlined(layout.instances()[record].attributes)) {
                continue;
            }
            m_outlineTargets.push_back(OutlineTarget{group, layout.batchOf(record), record});
            const core::Aabb bounds = layout.instanceBounds(record);
            rect = scene::unionRect(rect, bounds.isEmpty()
                                              ? std::optional<glm::ivec4>(glm::ivec4(0, 0, size.x, size.y))
                                              : scene::screenRect(bounds, viewProjection, size, margin));
        }
    }
    return m_outlineTargets.empty() ? std::nullopt : rect;
}

void SceneRenderer::drawOutline(GLDevice& device, const SceneShaders& shaders, const RenderView& view,
                                const ViewTargets& targets, std::uint32_t baseInstance, const glm::ivec4& rect,
                                FrameStats& stats) {
    const ScopedDebugGroup group(device, "Outline");
    m_outline.beginMark(device, targets);
    RenderStateCache& state = device.state();
    const ShaderProgram* bound = nullptr;
    std::optional<scene::MaterialHandle> boundMaterial;
    // The mark program of a batch's pass (and, alpha-masked, its material); false when it cannot be drawn.
    const auto bindMark = [&](scene::DrawPass pass, scene::MaterialHandle material) {
        if (pass == scene::DrawPass::Blend || material.value >= m_materials->uploadedCount()) {
            return false;
        }
        const bool masked = pass == scene::DrawPass::Mask;
        const ShaderProgram* program = masked ? shaders.outline.markMasked : shaders.outline.mark;
        if (program != bound) {
            program->use();
            bound = program;
        }
        if (masked && boundMaterial != material) {
            m_materials->bind(material);
            boundMaterial = material;
        }
        state.setCullFace(!m_materials->material(material).doubleSided);
        return true;
    };
    for (const OutlineTarget& target : m_outlineTargets) {
        if (target.group == nullptr) {
            const scene::Batch& batch = m_plan.batches[target.batch];
            if (m_meshes->contains(batch.mesh) && bindMark(batch.pass, batch.material)) {
                m_meshes->mesh(batch.mesh).drawInstanced(1, baseInstance + target.record);
                ++stats.outlinedInstances;
            }
        } else {
            const scene::StaticBatch& batch = target.group->layout().batches()[target.batch];
            if (bindMark(batch.pass, batch.material)) {
                target.group->drawRecord(target.record);
                ++stats.outlinedInstances;
            }
        }
    }
    const ShaderProgram* dim = usable(shaders.outline.dim) ? shaders.outline.dim : nullptr;
    m_outline.drawEdge(device, *shaders.outline.edge, dim, view.outline, targets, rect);
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
        // The static groups' casters cast like the items': a directional key light's map is fitted to both.
        core::Aabb casterBounds = view.shadowCasterBounds;
        for (const StaticInstanceGroup* staticGroup : view.staticGroups) {
            if (staticGroup != nullptr) {
                casterBounds.expand(staticGroup->layout().casterBounds());
            }
        }
        shadow = shadowProjectionFor(view.shadowLight, casterBounds, m_shadowMap.resolution());
    }
    // The map an earlier view of this frame drew is this view's too: the same light, bias and scene.
    const glm::vec2 depthBias = shadowDepthBias(view.shadowSettings);
    const bool shadowShared =
        shadow && m_frame > 0 && m_drawnShadow && m_drawnShadow->frame == m_frame &&
        m_drawnShadow->viewProjection == shadow->viewProjection && m_drawnShadow->depthBias == depthBias &&
        m_drawnShadow->items == view.items.data() && m_drawnShadow->itemCount == view.items.size() &&
        std::equal(m_drawnShadow->staticGroups.begin(), m_drawnShadow->staticGroups.end(), view.staticGroups.begin(),
                   view.staticGroups.end());
    const bool uniformsReady = updateUniforms(view, camera, shadow);
    std::optional<std::uint32_t> baseInstance;
    if (uniformsReady) {
        const scene::Frustum viewFrustum = view.cullFrustum.value_or(camera.frustum());
        scene::PlanOptions options;
        options.frustum = viewFrustum;
        options.eye = camera.position();
        options.localBounds = [this](scene::MeshHandle mesh) {
            return m_meshes->bounds(mesh);
        };
        options.passOf = [this](scene::MaterialHandle material) {
            return m_materials->pass(material);
        };
        m_planner.plan(view.items, options, m_plan);
        stats.culledItems = m_plan.culledItems;
        std::optional<scene::Frustum> shadowFrustum;
        if (shadow && !shadowShared) {
            shadowFrustum = scene::Frustum::fromViewProjection(shadow->viewProjection);
            options.frustum = shadowFrustum;
            options.eye = shadow->eye;
            options.shadowCastersOnly = true;
            m_planner.plan(view.items, options, m_shadowPlan);
        }
        planStaticGroups(view, viewFrustum, shadowFrustum, stats);
        stats.batches = m_plan.batches.size() + stats.staticBatches;
        if (auto uploaded = uploadInstances(shadow && !shadowShared); uploaded) {
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
    const bool shadowDrawn = baseInstance && shadow && !shadowShared;
    if (shadowDrawn) {
        drawShadowMap(device, shaders, view, *baseInstance + static_cast<std::uint32_t>(m_plan.instances.size()),
                      view.shadowSettings, stats);
        m_drawnShadow = DrawnShadow{m_frame,
                                    shadow->viewProjection,
                                    depthBias,
                                    view.items.data(),
                                    view.items.size(),
                                    {view.staticGroups.begin(), view.staticGroups.end()},
                                    stats.shadowDrawCalls,
                                    stats.shadowInstances};
    } else if (baseInstance && shadowShared) {
        stats.shadowDrawCalls = m_drawnShadow->drawCalls;
        stats.shadowInstances = m_drawnShadow->instances;
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
        BoundState bound;
        drawDepthBatches(device, shaders, m_plan, *baseInstance, bound, counts);
        drawStaticDepth(device, shaders, view, m_staticDraws, bound, counts);
        state.setColorWrite(true);
        stats.depthDrawCalls = counts.drawCalls;
    }
    m_depthTimer.end();

    // 3. The ambient occlusion of that depth (cleared to "nothing occluded" without a pre-pass).
    scene::SsaoSetup ssao = view.ssao;
    ssao.enabled = ssao.enabled && prePass;
    m_ssao.apply(device, shaders.ssao, ssao, targets);

    // 4. The lit pass: the opaque and alpha-masked batches, the static groups, then the blended batches over them.
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
        BoundState bound;
        drawShadedBatches(device, shaders, *baseInstance, prePass, false, bound, counts);
        DrawCounts staticCounts;
        drawStaticShaded(device, shaders, view, prePass, bound, staticCounts);
        drawShadedBatches(device, shaders, *baseInstance, prePass, true, bound, counts);
        stats.staticDrawCalls = staticCounts.drawCalls;
        stats.staticInstances = staticCounts.instances;
        stats.drawCalls = counts.drawCalls + staticCounts.drawCalls;
        stats.instances = counts.instances + staticCounts.instances;
        stats.triangles = counts.triangles + staticCounts.triangles;
        stats.shaderSwitches = counts.shaderSwitches + staticCounts.shaderSwitches;
        stats.materialSwitches = counts.materialSwitches + staticCounts.materialSwitches;
    }
    // Leave the default fixed-function state for the passes that follow.
    state.setPolygonMode(GL_FILL);
    state.setDepthFunc(GL_LESS);
    state.setBlend(false);
    state.setDepthWrite(true);
    state.setCullFace(true);
    m_timer.end();

    m_post.apply(device, shaders.post, view.post, targets);

    // 5. The hover outline, after the anti-aliasing: the mark reads this view's instances, released only after it.
    bool outlineDrawn = false;
    if (baseInstance && view.outline.enabled && m_outline.valid() && usable(shaders.outline.mark) &&
        usable(shaders.outline.markMasked) && usable(shaders.outline.edge)) {
        if (const std::optional<glm::ivec4> rect = collectOutlineTargets(view, camera, size)) {
            drawOutline(device, shaders, view, targets, *baseInstance, *rect, stats);
            outlineDrawn = true;
        }
    }
    m_instances.release(); // the draws that read this view's instances are issued

    if (uniformsReady && view.debugDraw != nullptr && shaders.debugLines != nullptr && shaders.debugLines->valid()) {
        // Over the tonemapped image, in their own colors (encoded by the sRGB output), hidden where the scene is in
        // front of them.
        const ScopedDebugGroup lines(device, "DebugLines");
        targets.outputWithSceneDepth().bind();
        state.setViewport(glm::ivec4(0, 0, size.x, size.y));
        state.setFramebufferSrgb(true);
        view.debugDraw->draw(*shaders.debugLines, state);
        stats.debugLines = view.debugDraw->lineCount();
    }
    // A view without a shadow map reports no shadow time (the timer still holds an earlier view's), nor one without an
    // outline an outline time; one sharing the frame's map reports the time of its drawing.
    stats.shadowGpuMilliseconds =
        shadowDrawn || (baseInstance && shadowShared) ? m_shadowTimer.latestMilliseconds() : std::nullopt;
    stats.depthGpuMilliseconds = m_depthTimer.latestMilliseconds();
    stats.ssaoGpuMilliseconds = m_ssao.latestGpuMilliseconds();
    stats.sceneGpuMilliseconds = m_timer.latestMilliseconds();
    stats.postGpuMilliseconds = m_post.latestGpuMilliseconds();
    stats.outlineGpuMilliseconds = outlineDrawn ? m_outline.latestGpuMilliseconds() : std::nullopt;
    m_stats = stats;
}

} // namespace rv::gl
