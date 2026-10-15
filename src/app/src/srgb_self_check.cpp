#include "srgb_self_check.hpp"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/assets/shader_source.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_mesh.hpp>
#include <rv/render/material_set.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/uniform_buffer.hpp>

namespace rv::app {
namespace {

constexpr int kTargetSize = 8;

Expected<gl::ShaderProgram> buildLinearUnlitProgram(const std::filesystem::path& shaderRoot) {
    const assets::ShaderSourceLoader loader(shaderRoot);
    const std::string defines[] = {"RV_UNLIT_LINEAR_OUTPUT"};
    auto vertex = loader.load("pbr/pbr.vert", defines);
    if (!vertex) {
        return propagate(vertex.error());
    }
    auto fragment = loader.load("unlit/unlit_emissive.frag", defines);
    if (!fragment) {
        return propagate(fragment.error());
    }
    return gl::ShaderProgram::build(*vertex, *fragment, "SrgbSelfCheck");
}

} // namespace

Expected<SrgbCheckResult> runSrgbSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                           const std::filesystem::path& shaderRoot) {
    const gl::ScopedDebugGroup group(device, "SrgbSelfCheck");
    auto program = buildLinearUnlitProgram(shaderRoot);
    if (!program) {
        return propagate(program.error());
    }

    // A 2 x 2 quad facing +Z through identity matrices covers the whole target.
    auto quad = geometry::buildPrimitive("quad", nlohmann::json{{"size", {2.0, 2.0}}});
    if (!quad) {
        return propagate(quad.error());
    }
    auto mesh = gl::GpuMesh::create(*quad, "SrgbSelfCheck.quad");
    if (!mesh) {
        return propagate(mesh.error());
    }

    assets::MaterialDesc grey;
    grey.name = "SrgbSelfCheck.grey";
    grey.shading = assets::ShadingModel::Unlit;
    grey.baseColorFactor = glm::vec4(glm::vec3(kSrgbCheckLinearValue), 1.0f);
    grey.emissiveStrength = 1.0f;
    auto materials = gl::MaterialSet::create(defaults, 1.0f);
    if (!materials) {
        return propagate(materials.error());
    }
    const scene::MaterialHandle material = materials->add(grey.name, grey, gl::MaterialTextures{});
    if (auto uploaded = materials->upload(device.caps().uniformBufferOffsetAlignment, "SrgbSelfCheck.material");
        !uploaded) {
        return propagate(uploaded.error());
    }

    auto frameUniforms = gl::UniformBuffer<gl::FrameUniforms>::create("SrgbSelfCheck.frame");
    if (!frameUniforms) {
        return propagate(frameUniforms.error());
    }
    if (auto updated = frameUniforms->update(gl::FrameUniforms{}); !updated) { // identity view and projection
        return propagate(updated.error());
    }

    auto target = gl::Framebuffer::create(
        gl::FramebufferDesc{glm::ivec2(kTargetSize), gl::ColorFormat::Srgb8Alpha8, true}, "SrgbSelfCheck.target");
    if (!target) {
        return propagate(target.error());
    }
    device.beginPass(*target, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    gl::RenderStateCache& state = device.state();
    state.setDepthTest(true);
    state.setDepthFunc(GL_LESS);
    state.setCullFace(true);
    state.setCullMode(GL_BACK);
    state.setBlend(false);
    state.setPolygonMode(GL_FILL);
    frameUniforms->bind(gl::UniformBinding::Frame);
    materials->bind(material);
    program->use();
    program->setMat4("uModel", glm::mat4(1.0f));
    mesh->draw();

    auto pixels = target->readPixels();
    gl::Framebuffer::unbind();
    if (!pixels) {
        return propagate(pixels.error());
    }
    return evaluateSrgbCheck(pixels->pixels);
}

} // namespace rv::app
