#include "srgb_self_check.hpp"

#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/assets/material_desc.hpp>
#include <rv/assets/shader_source.hpp>
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
#include <rv/render/uniform_buffer.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::app {
namespace {

constexpr int kTargetSize = 8;

Expected<gl::ShaderProgram> buildProgram(const assets::ShaderSourceLoader& loader, std::string_view vertexPath,
                                         std::string_view fragmentPath, std::span<const std::string> defines,
                                         std::string_view debugName) {
    auto vertex = loader.load(vertexPath, defines);
    if (!vertex) {
        return propagate(vertex.error());
    }
    auto fragment = loader.load(fragmentPath, defines);
    if (!fragment) {
        return propagate(fragment.error());
    }
    return gl::ShaderProgram::build(*vertex, *fragment, debugName);
}

} // namespace

Expected<SrgbCheckResult> runSrgbSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                           const std::filesystem::path& shaderRoot) {
    const gl::ScopedDebugGroup group(device, "SrgbSelfCheck");
    const assets::ShaderSourceLoader loader(shaderRoot);
    auto unlit = buildProgram(loader, "pbr/pbr.vert", "unlit/unlit_emissive.frag", {}, "SrgbSelfCheck.unlit");
    if (!unlit) {
        return propagate(unlit.error());
    }
    const std::string linearOutput[] = {"RV_POST_LINEAR_OUTPUT"};
    auto post = buildProgram(loader, "post/fullscreen.vert", "post/post.frag", linearOutput, "SrgbSelfCheck.post");
    if (!post) {
        return propagate(post.error());
    }

    // A 2 x 2 quad facing +Z through identity matrices covers the whole target: one instance with the identity model
    // matrix, read like every mesh instance (pbr.vert).
    auto quad = geometry::buildPrimitive("quad", nlohmann::json{{"size", {2.0, 2.0}}});
    if (!quad) {
        return propagate(quad.error());
    }
    auto mesh = gl::GpuMesh::create(*quad, "SrgbSelfCheck.quad");
    if (!mesh) {
        return propagate(mesh.error());
    }
    const glm::mat4 identity(1.0f);
    auto instance =
        gl::Buffer::create(std::as_bytes(std::span<const glm::mat4, 1>(&identity, 1)), 0, "SrgbSelfCheck.instance");
    if (!instance) {
        return propagate(instance.error());
    }
    if (auto attached = mesh->attachInstanceBuffer(*instance); !attached) {
        return propagate(attached.error());
    }

    assets::MaterialDesc grey;
    grey.name = "SrgbSelfCheck.grey";
    grey.shading = assets::ShadingModel::Unlit;
    grey.baseColorFactor = glm::vec4(glm::vec3(kSrgbCheckLinearValue), 1.0f);
    grey.emissiveStrength = 1.0f;
    auto materials = gl::MaterialRegistry::create(defaults, 1.0f);
    if (!materials) {
        return propagate(materials.error());
    }
    const scene::MaterialHandle material = materials->add(grey.name, grey.name, grey, gl::MaterialTextures{});
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
    auto targets = gl::ViewTargets::create(glm::ivec2(kTargetSize), "SrgbSelfCheck.targets");
    if (!targets) {
        return propagate(targets.error());
    }
    auto postProcessor = gl::PostProcessor::create();
    if (!postProcessor) {
        return propagate(postProcessor.error());
    }

    // The scene pass: linear radiance into the HDR target.
    device.beginPass(targets->hdr(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    gl::RenderStateCache& state = device.state();
    state.setDepthTest(true);
    state.setDepthFunc(GL_LESS);
    state.setCullFace(true);
    state.setCullMode(GL_BACK);
    state.setBlend(false);
    state.setPolygonMode(GL_FILL);
    frameUniforms->bind(gl::UniformBinding::Frame);
    materials->bind(material);
    unlit->use();
    mesh->drawInstanced(1);

    // The post pass to the sRGB output: exposure 1, no bloom, vignette or dithering (gl::PostSettings defaults).
    gl::PostShaders postShaders;
    postShaders.post = &*post;
    postProcessor->apply(device, postShaders, gl::PostSettings{}, *targets);

    auto pixels = targets->output().readPixels();
    gl::Framebuffer::unbind();
    if (!pixels) {
        return propagate(pixels.error());
    }
    return evaluateSrgbCheck(pixels->pixels);
}

} // namespace rv::app
