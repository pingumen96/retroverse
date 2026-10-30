#include "fxaa_self_check.hpp"

#include <glm/ext/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/post_processor.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/shader_program.hpp>

#include "self_check_canvas.hpp"

namespace rv::app {
namespace {

/// Radiance of the emitter: far beyond what the ACES curve shows as white, so the tonemap clamps it to exactly 1.
constexpr float kEmitterRadiance = 100.0f;

} // namespace

Expected<FxaaCheckResult> runFxaaSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                           const gl::ShaderProgram& unlit, const gl::PostShaders& post) {
    const auto usable = [](const gl::ShaderProgram* program) { return program != nullptr && program->valid(); };
    if (!unlit.valid() || !usable(post.post) || !usable(post.antialiasing)) {
        return makeError(ErrorCode::GraphicsError, "the unlit, post or anti-aliasing program did not build",
                         "--self-check=fxaa");
    }
    const gl::ScopedDebugGroup group(device, "FxaaSelfCheck");

    // The 2 x 2 quad scaled to 4 x 6 and moved right by 2 has its left edge on x = 0 (the target spans -1..1); turned
    // about the target's center, it covers the right half of the target up to a slanted edge through the center.
    const glm::mat4 slanted =
        glm::rotate(glm::mat4(1.0f), core::degreesToRadians(kFxaaCheckEdgeDegrees), glm::vec3(0.0f, 0.0f, 1.0f)) *
        glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 1.0f));
    auto canvas = SelfCheckCanvas::create(device, defaults, glm::ivec2(kFxaaCheckSize), slanted,
                                          glm::vec3(kEmitterRadiance), "FxaaSelfCheck");
    if (!canvas) {
        return propagate(canvas.error());
    }

    // Exposure 1 and no bloom, vignette or dithering (gl::PostSettings defaults): two exact colors, black and white.
    gl::PostSettings settings;
    auto aliased = canvas->render(device, unlit, post, settings);
    if (!aliased) {
        return propagate(aliased.error());
    }
    settings.antialiasing = true;
    auto antialiased = canvas->render(device, unlit, post, settings);
    if (!antialiased) {
        return propagate(antialiased.error());
    }
    FxaaCheckResult result = evaluateFxaaCheck(aliased->pixels, antialiased->pixels, kFxaaCheckSize, kFxaaCheckSize);

    // The in-place write (docs/i5/memory-margin.md): with the bloom on, the post pass reads the emitted light texel by
    // texel; with the anti-aliasing after it, it writes over that very storage. Its image must equal, byte for byte,
    // the one it writes into the output when no anti-aliasing follows.
    settings.bloomStrength = 0.06f;
    settings.bloomRadius = 0.7f;
    settings.antialiasing = false;
    auto direct = canvas->render(device, unlit, post, settings);
    if (!direct) {
        return propagate(direct.error());
    }
    settings.antialiasing = true;
    if (auto bloomed = canvas->render(device, unlit, post, settings); !bloomed) {
        return propagate(bloomed.error());
    }
    auto inPlace = canvas->readTonemapped();
    if (!inPlace) {
        return propagate(inPlace.error());
    }
    evaluateFxaaInPlaceWrite(result, direct->pixels, inPlace->pixels, kFxaaCheckSize, kFxaaCheckSize);
    return result;
}

} // namespace rv::app
