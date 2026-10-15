#include <rv/render/scene_renderer.hpp>

#include <cstring>
#include <optional>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/render/debug_draw.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/shader_program.hpp>

namespace rv::gl {

Expected<SceneRenderer> SceneRenderer::create(MeshRegistry& meshes, const MaterialRegistry& materials) {
    SceneRenderer renderer(meshes, materials);
    auto frame = UniformBuffer<FrameUniforms>::create("SceneRenderer.FrameUniforms");
    if (!frame) {
        return propagate(frame.error());
    }
    auto lights = UniformBuffer<LightsUniforms>::create("SceneRenderer.LightsUniforms");
    if (!lights) {
        return propagate(lights.error());
    }
    auto instances = InstanceBuffer::create(kInitialInstanceCapacity, "SceneRenderer.Instances");
    if (!instances) {
        return propagate(instances.error());
    }
    auto timer = GpuTimer::create("SceneRenderer.GpuTime");
    if (!timer) {
        return propagate(timer.error());
    }
    renderer.m_frameUniforms = std::move(*frame);
    renderer.m_lightsUniforms = std::move(*lights);
    renderer.m_instances = std::move(*instances);
    renderer.m_timer = std::move(*timer);
    return renderer;
}

bool SceneRenderer::updateUniforms(const RenderView& view, const scene::Camera& camera) {
    FrameUniforms frame;
    frame.view = camera.view();
    frame.projection = camera.projection();
    frame.viewProjection = frame.projection * frame.view;
    frame.cameraPosition = camera.position();
    frame.time = view.timeSeconds;
    if (auto updated = m_frameUniforms.update(frame); !updated) {
        spdlog::error("FrameUniforms update failed: {}", updated.error().toString());
        return false;
    }
    // LightsUniforms is a std140 mirror without implicit padding: equal bytes mean equal lights.
    if (!m_uploadedLights || std::memcmp(&*m_uploadedLights, &view.lights, sizeof(LightsUniforms)) != 0) {
        if (auto updated = m_lightsUniforms.update(view.lights); !updated) {
            spdlog::error("LightsUniforms update failed: {}", updated.error().toString());
            return false;
        }
        m_uploadedLights = view.lights;
    }
    m_frameUniforms.bind(UniformBinding::Frame);
    m_lightsUniforms.bind(UniformBinding::Lights);
    return true;
}

void SceneRenderer::drawBatches(GLDevice& device, const SceneShaders& shaders, const scene::BatchPlan& plan,
                                std::uint32_t baseInstance, FrameStats& stats) const {
    RenderStateCache& state = device.state();
    const ShaderProgram* currentProgram = nullptr;
    std::optional<scene::MaterialHandle> currentMaterial;
    for (const scene::Batch& batch : plan.batches) {
        if (!m_meshes->contains(batch.mesh) || batch.material.value >= m_materials->uploadedCount()) {
            continue;
        }
        const ShaderProgram* program = batch.shader == scene::ShaderKind::Unlit ? shaders.unlit : shaders.pbr;
        if (program == nullptr || !program->valid()) {
            continue;
        }
        if (program != currentProgram) {
            program->use();
            currentProgram = program;
            ++stats.shaderSwitches;
        }
        if (currentMaterial != batch.material) {
            m_materials->bind(batch.material);
            const bool blend = batch.pass == scene::DrawPass::Blend;
            state.setBlend(blend);
            state.setDepthWrite(!blend);
            state.setCullFace(!m_materials->material(batch.material).doubleSided);
            currentMaterial = batch.material;
            ++stats.materialSwitches;
        }
        const GpuMesh& mesh = m_meshes->mesh(batch.mesh);
        mesh.drawInstanced(batch.instanceCount, baseInstance + batch.firstInstance);
        ++stats.drawCalls;
        stats.instances += batch.instanceCount;
        stats.triangles += batch.instanceCount * mesh.triangleCount();
    }
}

void SceneRenderer::render(GLDevice& device, const SceneShaders& shaders, const RenderView& view,
                           const Framebuffer& target) {
    if (!target.valid()) {
        return;
    }
    const ScopedDebugGroup group(device, "SceneRenderer");
    m_timer.begin();
    device.beginPass(target, view.clearColor);

    FrameStats stats;
    stats.items = view.items.size();
    scene::Camera camera = view.camera;
    const glm::ivec2 size = target.size();
    camera.setAspect(static_cast<float>(size.x) / static_cast<float>(size.y));
    if (updateUniforms(view, camera)) {
        scene::PlanOptions options;
        options.frustum = camera.frustum();
        options.eye = camera.position();
        options.localBounds = [this](scene::MeshHandle mesh) {
            return m_meshes->bounds(mesh);
        };
        options.passOf = [this](scene::MaterialHandle material) {
            return m_materials->pass(material);
        };
        const scene::BatchPlan plan = m_planner.plan(view.items, options);
        stats.culledItems = plan.culledItems;
        stats.batches = plan.batches.size();

        RenderStateCache& state = device.state();
        state.setDepthTest(true);
        state.setDepthFunc(GL_LESS);
        state.setPolygonMode(GL_FILL);
        state.setCullMode(GL_BACK);
        state.setBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (auto baseInstance = m_instances.write(plan.instanceTransforms); !baseInstance) {
            spdlog::error("Scene instances cannot be written, the view is not drawn: {}",
                          baseInstance.error().toString());
        } else if (auto attached = m_meshes->attachInstanceBuffer(m_instances); !attached) {
            spdlog::error("Meshes cannot read the instance buffer, the view is not drawn: {}",
                          attached.error().toString());
        } else {
            drawBatches(device, shaders, plan, *baseInstance, stats);
        }
        m_instances.release(); // the draws that read this view's instances are issued

        if (view.debugDraw != nullptr && shaders.debugLines != nullptr && shaders.debugLines->valid()) {
            view.debugDraw->draw(*shaders.debugLines, state);
            stats.debugLines = view.debugDraw->lineCount();
        }
        // Leave the default fixed-function state for whatever draws next (overlays, the next pass).
        state.setBlend(false);
        state.setDepthWrite(true);
        state.setCullFace(true);
    }
    m_timer.end();
    stats.gpuMilliseconds = m_timer.latestMilliseconds();
    m_stats = stats;
}

} // namespace rv::gl
