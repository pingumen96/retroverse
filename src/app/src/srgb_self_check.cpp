#include "srgb_self_check.hpp"

#include <span>
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
#include <rv/render/tonemap.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::app {
namespace {

constexpr int kTargetSize = 8;

/// The uniform blocks the unlit program reads: the view (identity matrices) and the lights (none: a CRT screen's glass
/// would reflect them; the check's grey material is not one, but every block the program declares is backed).
struct CheckBlocks {
    gl::UniformBuffer<gl::FrameUniforms> frame;
    gl::UniformBuffer<gl::LightsUniforms> lights;
};

/// The quad of the check drawn into the HDR target of `targets`, post-processed with `settings` and read back.
Expected<SrgbCheckResult> runPass(gl::GLDevice& device, const gl::ShaderProgram& unlit, const gl::GpuMesh& quad,
                                  const gl::MaterialRegistry& materials, scene::MaterialHandle material,
                                  const CheckBlocks& blocks, gl::PostProcessor& postProcessor,
                                  const gl::PostShaders& post, const gl::PostSettings& settings,
                                  const gl::ViewTargets& targets) {
    // The scene pass: linear radiance (all of it emitted, as from every unlit surface) into the HDR target.
    device.beginPass(targets.hdr(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    gl::RenderStateCache& state = device.state();
    state.setDepthTest(true);
    state.setDepthFunc(GL_LESS);
    state.setCullFace(true);
    state.setCullMode(GL_BACK);
    state.setBlend(false);
    state.setPolygonMode(GL_FILL);
    blocks.frame.bind(gl::UniformBinding::Frame);
    blocks.lights.bind(gl::UniformBinding::Lights);
    materials.bind(material);
    unlit.use();
    quad.drawInstanced(1);

    postProcessor.apply(device, post, settings, targets);
    auto pixels = targets.output().readPixels();
    gl::Framebuffer::unbind();
    if (!pixels) {
        return propagate(pixels.error());
    }
    return evaluateSrgbCheck(pixels->pixels);
}

} // namespace

Expected<SrgbSelfCheckResults> runSrgbSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                                const gl::ShaderProgram& unlit, const gl::PostShaders& post) {
    const auto usable = [](const gl::ShaderProgram* program) { return program != nullptr && program->valid(); };
    if (!unlit.valid() || !usable(post.post) || !usable(post.antialiasing)) {
        return makeError(ErrorCode::GraphicsError, "the unlit, post or anti-aliasing program did not build",
                         "--self-check=srgb");
    }
    const gl::ScopedDebugGroup group(device, "SrgbSelfCheck");

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

    // The grey radiance that exposure 1 and the ACES curve show as linear kSrgbCheckLinearValue (CPU mirror).
    assets::MaterialDesc grey;
    grey.name = "SrgbSelfCheck.grey";
    grey.shading = assets::ShadingModel::Unlit;
    grey.baseColorFactor = glm::vec4(glm::vec3(gl::greyRadianceForDisplay(kSrgbCheckLinearValue, 1.0f)), 1.0f);
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
    auto lightsUniforms = gl::UniformBuffer<gl::LightsUniforms>::create("SrgbSelfCheck.lights");
    if (!lightsUniforms) {
        return propagate(lightsUniforms.error());
    }
    if (auto updated = lightsUniforms->update(gl::LightsUniforms{}); !updated) { // no light, no ambient
        return propagate(updated.error());
    }
    const CheckBlocks blocks{std::move(*frameUniforms), std::move(*lightsUniforms)};
    auto targets = gl::ViewTargets::create(glm::ivec2(kTargetSize), "SrgbSelfCheck.targets");
    if (!targets) {
        return propagate(targets.error());
    }
    auto postProcessor = gl::PostProcessor::create();
    if (!postProcessor) {
        return propagate(postProcessor.error());
    }

    SrgbSelfCheckResults results;
    // Exposure 1, no bloom, vignette, dithering or anti-aliasing (gl::PostSettings defaults): the tonemap and the
    // encoding alone.
    auto plain =
        runPass(device, unlit, *mesh, *materials, material, blocks, *postProcessor, post, gl::PostSettings{}, *targets);
    if (!plain) {
        return propagate(plain.error());
    }
    results.plain = *plain;
    // The whole light of the uniform quad blooms (it is emitted), and the bloom of a uniform field is that field: the
    // output must not change, and the dither stays within one 8-bit step. A uniform field has no edge: the
    // anti-aliasing pass copies it exactly.
    gl::PostSettings postProcessed;
    postProcessed.bloomStrength = 1.0f;
    postProcessed.bloomRadius = 0.5f;
    postProcessed.ditherAmplitude = gl::kDitherAmplitude;
    postProcessed.antialiasing = true;
    auto bloomed =
        runPass(device, unlit, *mesh, *materials, material, blocks, *postProcessor, post, postProcessed, *targets);
    if (!bloomed) {
        return propagate(bloomed.error());
    }
    results.postProcessed = *bloomed;
    return results;
}

} // namespace rv::app
