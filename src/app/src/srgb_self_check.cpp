#include "srgb_self_check.hpp"

#include <rv/render/gl_device.hpp>
#include <rv/render/post_processor.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/tonemap.hpp>

#include "self_check_canvas.hpp"

namespace rv::app {
namespace {

constexpr int kTargetSize = 8;

/// The canvas post-processed with `settings`, read back and evaluated.
Expected<SrgbCheckResult> runPass(gl::GLDevice& device, SelfCheckCanvas& canvas, const gl::ShaderProgram& unlit,
                                  const gl::PostShaders& post, const gl::PostSettings& settings) {
    auto pixels = canvas.render(device, unlit, post, settings);
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

    // A quad over the whole target of the grey radiance that exposure 1 and the ACES curve show as linear
    // kSrgbCheckLinearValue (CPU mirror).
    auto canvas = SelfCheckCanvas::create(device, defaults, glm::ivec2(kTargetSize), glm::mat4(1.0f),
                                          glm::vec3(gl::greyRadianceForDisplay(kSrgbCheckLinearValue, 1.0f)),
                                          "SrgbSelfCheck");
    if (!canvas) {
        return propagate(canvas.error());
    }

    SrgbSelfCheckResults results;
    // Exposure 1, no bloom, vignette, dithering or anti-aliasing (gl::PostSettings defaults): the tonemap and the
    // encoding alone.
    auto plain = runPass(device, *canvas, unlit, post, gl::PostSettings{});
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
    auto bloomed = runPass(device, *canvas, unlit, post, postProcessed);
    if (!bloomed) {
        return propagate(bloomed.error());
    }
    results.postProcessed = *bloomed;
    return results;
}

} // namespace rv::app
