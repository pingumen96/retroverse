#include "scene_view.hpp"

#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/math.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/shader_program.hpp>

namespace rv::app {

SceneView SceneView::create(const scene::SceneDescription& scene, const scene::SceneGeometry& geometry) {
    SceneView view;
    view.m_camera.setLens(scene.camera.lens);
    view.m_camera.setPose(scene.camera.position, core::degreesToRadians(scene.camera.yawDeg),
                          core::degreesToRadians(scene.camera.pitchDeg));
    view.m_clearColor = glm::vec4(scene.environment.clearColor, 1.0f);
    view.m_exposure = scene.environment.exposure;

    for (const scene::SceneObjectGeometry& object : geometry.objects) {
        for (const scene::ScenePart& part : object.parts) {
            auto mesh = gl::GpuMesh::create(part.mesh, object.id + ":" + part.mesh.name);
            if (!mesh) {
                spdlog::error("Scene object '{}': part '{}' cannot be uploaded and is not drawn: {}", object.id,
                              part.role, mesh.error().toString());
                continue;
            }
            const std::size_t meshIndex = view.m_meshes.size();
            view.m_meshes.push_back(std::move(*mesh));
            for (const glm::mat4& instance : object.instances) {
                view.m_draws.push_back(
                    DrawItem{meshIndex, instance, part.material.baseColorFactor, part.material.doubleSided});
            }
        }
    }
    return view;
}

void SceneView::render(gl::GLDevice& device, const gl::ShaderProgram& program,
                       gl::UniformBuffer<gl::FrameUniforms>& frameUniforms, const gl::Framebuffer& target,
                       float timeSeconds) {
    if (!target.valid()) {
        return;
    }
    const gl::ScopedDebugGroup group(device, "SceneView");
    device.beginPass(target, m_clearColor);

    const glm::ivec2 size = target.size();
    m_camera.setAspect(static_cast<float>(size.x) / static_cast<float>(size.y));
    gl::FrameUniforms frame;
    frame.view = m_camera.view();
    frame.projection = m_camera.projection();
    frame.viewProjection = frame.projection * frame.view;
    frame.cameraPosition = m_camera.position();
    frame.time = timeSeconds;
    frame.exposure = m_exposure;
    if (auto updated = frameUniforms.update(frame); !updated) {
        spdlog::error("FrameUniforms update failed: {}", updated.error().toString());
        return;
    }
    frameUniforms.bind(gl::UniformBinding::Frame);
    if (!program.valid()) {
        return;
    }

    gl::RenderStateCache& state = device.state();
    state.setDepthTest(true);
    state.setDepthFunc(GL_LESS);
    state.setDepthWrite(true);
    state.setBlend(false);
    state.setCullMode(GL_BACK);
    state.setPolygonMode(GL_FILL);
    program.use();
    for (const DrawItem& draw : m_draws) {
        state.setCullFace(!draw.doubleSided);
        program.setMat4("uModel", draw.model);
        program.setVec4("uBaseColor", draw.color);
        m_meshes[draw.mesh].draw();
    }
}

} // namespace rv::app
