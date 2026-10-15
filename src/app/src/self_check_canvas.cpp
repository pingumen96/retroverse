#include "self_check_canvas.hpp"

#include <span>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_mesh.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/post_processor.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::app {

struct SelfCheckCanvas::State {
    gl::GpuMesh quad;
    gl::Buffer instance;
    gl::MaterialRegistry materials;
    scene::MaterialHandle material;
    /// The blocks the unlit program reads: the view (identity matrices) and the lights (none: a CRT screen's glass
    /// would reflect them; the canvas material is not one, but every block the program declares is backed).
    gl::UniformBuffer<gl::FrameUniforms> frame;
    gl::UniformBuffer<gl::LightsUniforms> lights;
    gl::ViewTargets targets;
    gl::PostProcessor postProcessor;
};

SelfCheckCanvas::SelfCheckCanvas(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
SelfCheckCanvas::SelfCheckCanvas(SelfCheckCanvas&&) noexcept = default;
SelfCheckCanvas& SelfCheckCanvas::operator=(SelfCheckCanvas&&) noexcept = default;
SelfCheckCanvas::~SelfCheckCanvas() = default;

Expected<SelfCheckCanvas> SelfCheckCanvas::create(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                                  glm::ivec2 size, const glm::mat4& quadTransform, glm::vec3 radiance,
                                                  std::string_view name) {
    const std::string label(name);
    auto quad = geometry::buildPrimitive("quad", nlohmann::json{{"size", {2.0, 2.0}}});
    if (!quad) {
        return propagate(quad.error());
    }
    auto mesh = gl::GpuMesh::create(*quad, label + ".quad");
    if (!mesh) {
        return propagate(mesh.error());
    }
    // One instance, read like every mesh instance (pbr.vert: the model matrix at attribute locations 4..7).
    auto instance = gl::Buffer::create(std::as_bytes(std::span<const glm::mat4, 1>(&quadTransform, 1)), 0,
                                       label + ".instance");
    if (!instance) {
        return propagate(instance.error());
    }
    if (auto attached = mesh->attachInstanceBuffer(*instance); !attached) {
        return propagate(attached.error());
    }

    assets::MaterialDesc unlit;
    unlit.name = label + ".material";
    unlit.shading = assets::ShadingModel::Unlit;
    unlit.baseColorFactor = glm::vec4(radiance, 1.0f);
    unlit.emissiveStrength = 1.0f;
    auto materials = gl::MaterialRegistry::create(defaults, 1.0f);
    if (!materials) {
        return propagate(materials.error());
    }
    const scene::MaterialHandle material = materials->add(unlit.name, unlit.name, unlit, gl::MaterialTextures{});
    if (auto uploaded = materials->upload(device.caps().uniformBufferOffsetAlignment, label + ".materials");
        !uploaded) {
        return propagate(uploaded.error());
    }

    auto frame = gl::UniformBuffer<gl::FrameUniforms>::create(label + ".frame");
    if (!frame) {
        return propagate(frame.error());
    }
    if (auto updated = frame->update(gl::FrameUniforms{}); !updated) { // identity view and projection
        return propagate(updated.error());
    }
    auto lights = gl::UniformBuffer<gl::LightsUniforms>::create(label + ".lights");
    if (!lights) {
        return propagate(lights.error());
    }
    if (auto updated = lights->update(gl::LightsUniforms{}); !updated) { // no light, no ambient
        return propagate(updated.error());
    }
    auto targets = gl::ViewTargets::create(size, label + ".targets");
    if (!targets) {
        return propagate(targets.error());
    }
    auto postProcessor = gl::PostProcessor::create();
    if (!postProcessor) {
        return propagate(postProcessor.error());
    }
    return SelfCheckCanvas(std::make_unique<State>(State{std::move(*mesh), std::move(*instance), std::move(*materials),
                                                         material, std::move(*frame), std::move(*lights),
                                                         std::move(*targets), std::move(*postProcessor)}));
}

Expected<assets::ImageData> SelfCheckCanvas::render(gl::GLDevice& device, const gl::ShaderProgram& unlit,
                                                    const gl::PostShaders& post, const gl::PostSettings& settings) {
    State& state = *m_state;
    // The scene pass: linear radiance (all of it emitted, as from every unlit surface) into the HDR target.
    device.beginPass(state.targets.hdr(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    gl::RenderStateCache& cache = device.state();
    cache.setDepthTest(true);
    cache.setDepthFunc(GL_LESS);
    cache.setCullFace(true);
    cache.setCullMode(GL_BACK);
    cache.setBlend(false);
    cache.setPolygonMode(GL_FILL);
    state.frame.bind(gl::UniformBinding::Frame);
    state.lights.bind(gl::UniformBinding::Lights);
    state.materials.bind(state.material);
    unlit.use();
    state.quad.drawInstanced(1);

    state.postProcessor.apply(device, post, settings, state.targets);
    auto pixels = state.targets.output().readPixels();
    gl::Framebuffer::unbind();
    return pixels;
}

} // namespace rv::app
