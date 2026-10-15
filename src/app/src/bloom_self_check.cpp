#include "bloom_self_check.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <vector>

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/post_processor.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/view_targets.hpp>

namespace rv::app {
namespace {

constexpr glm::ivec2 kTargetSize(512, 512);
/// Where the patterns are centered: far from the edges, so that the bloom chain keeps the energy of the image.
constexpr glm::ivec2 kCenter(256, 256);
constexpr int kSquareSide = 64;
/// Radiance of the lit background around the squares: dim, nothing emitted.
constexpr float kBackgroundRadiance = 0.05f;
constexpr float kEmitterRadiance = 0.5f;
/// Radiances of the single bright pixels: specular highlights 100 and 1000 times brighter than white.
constexpr std::array<float, 2> kFireflyRadiances{100.0f, 1000.0f};
/// Draw buffers of the HDR target's attachments.
constexpr GLint kRadianceBuffer = static_cast<GLint>(gl::toIndex(gl::SceneOutput::Radiance));
constexpr GLint kEmissionBuffer = static_cast<GLint>(gl::toIndex(gl::SceneOutput::Emission));

bool usable(const gl::ShaderProgram* program) noexcept {
    return program != nullptr && program->valid();
}

/// A rectangle of the pattern: its radiance and the emitted part of it.
struct PatternRect {
    glm::ivec2 origin{0};
    glm::ivec2 size{0};
    glm::vec3 radiance{0.0f};
    glm::vec3 emission{0.0f};
};

/// Writes a pattern straight into the HDR target, as the scene shaders would have drawn it (gl::SceneOutput):
/// `background` radiance, nothing emitted, with `rects` over it.
void writePattern(gl::GLDevice& device, const gl::Framebuffer& hdr, float background,
                  std::initializer_list<PatternRect> rects) {
    device.beginPass(hdr, glm::vec4(glm::vec3(background), 1.0f));
    glEnable(GL_SCISSOR_TEST);
    for (const PatternRect& rect : rects) {
        glScissor(rect.origin.x, rect.origin.y, rect.size.x, rect.size.y);
        const glm::vec4 radiance(rect.radiance, 1.0f);
        const glm::vec4 emission(rect.emission, 1.0f);
        glClearBufferfv(GL_COLOR, kRadianceBuffer, glm::value_ptr(radiance));
        glClearBufferfv(GL_COLOR, kEmissionBuffer, glm::value_ptr(emission));
    }
    glDisable(GL_SCISSOR_TEST); // the state every other pass expects
}

/// The bloom of the pattern in the HDR target of `targets`: the largest bloom level after the whole chain.
Expected<std::vector<glm::vec4>> bloomOf(gl::GLDevice& device, gl::PostProcessor& processor,
                                         const gl::PostShaders& post, const gl::PostSettings& settings,
                                         const gl::ViewTargets& targets) {
    processor.apply(device, post, settings, targets);
    return targets.bloomLevels()[0].readLinearPixels();
}

/// Bloom energy of a bloom level of `targets`: its luminance summed over its texels, times the view pixels per texel.
float bloomEnergy(std::span<const glm::vec4> level, const gl::ViewTargets& targets) {
    const glm::ivec2 levelSize = targets.bloomLevels()[0].size();
    const double pixelsPerTexel = static_cast<double>(targets.size().x) * static_cast<double>(targets.size().y) /
                                  (static_cast<double>(levelSize.x) * static_cast<double>(levelSize.y));
    double sum = 0.0;
    for (const glm::vec4& texel : level) {
        sum += 0.2126 * static_cast<double>(texel.r) + 0.7152 * static_cast<double>(texel.g) +
               0.0722 * static_cast<double>(texel.b);
    }
    return static_cast<float>(sum * pixelsPerTexel);
}

/// Largest color component of a bloom level.
float bloomPeak(std::span<const glm::vec4> level) {
    float peak = 0.0f;
    for (const glm::vec4& texel : level) {
        peak = std::max({peak, texel.r, texel.g, texel.b});
    }
    return peak;
}

} // namespace

Expected<BloomCheckMeasurements> runBloomSelfCheck(gl::GLDevice& device, const gl::PostShaders& post) {
    if (!usable(post.bloomDownsampleFirst) || !usable(post.bloomDownsample) || !usable(post.bloomUpsample) ||
        !usable(post.post)) {
        return makeError(ErrorCode::GraphicsError, "a bloom or post program did not build", "--self-check=bloom");
    }
    const gl::ScopedDebugGroup group(device, "BloomSelfCheck");
    auto targets = gl::ViewTargets::create(kTargetSize, "BloomSelfCheck.targets");
    if (!targets) {
        return propagate(targets.error());
    }
    auto processor = gl::PostProcessor::create();
    if (!processor) {
        return propagate(processor.error());
    }
    // The bloom chain itself does not depend on the strength (only the post pass does); the radius is the default.
    gl::PostSettings settings;
    settings.bloomStrength = 0.06f;
    settings.bloomRadius = 0.7f;

    BloomCheckMeasurements measurements;
    const glm::ivec2 squareOrigin = kCenter - glm::ivec2(kSquareSide / 2);
    const glm::ivec2 squareSize(kSquareSide);

    // Lit paper or wood at the brightest a room shows them: just below where the knee of the bloom threshold starts.
    const float belowKnee = 0.99f * (gl::kBloomThreshold - gl::kBloomKnee);
    writePattern(device, targets->hdr(), kBackgroundRadiance,
                 {PatternRect{squareOrigin, squareSize, glm::vec3(belowKnee), glm::vec3(0.0f)}});
    auto lit = bloomOf(device, *processor, post, settings, *targets);
    if (!lit) {
        return propagate(lit.error());
    }
    measurements.litBloomPeak = bloomPeak(*lit);

    // A screen: all of its light is emitted, all of it blooms.
    writePattern(device, targets->hdr(), kBackgroundRadiance,
                 {PatternRect{squareOrigin, squareSize, glm::vec3(kEmitterRadiance), glm::vec3(kEmitterRadiance)}});
    auto emitter = bloomOf(device, *processor, post, settings, *targets);
    if (!emitter) {
        return propagate(emitter.error());
    }
    const float emitted = static_cast<float>(kSquareSide * kSquareSide) * kEmitterRadiance;
    measurements.emitterEnergyRatio = bloomEnergy(*emitter, *targets) / emitted;

    // A specular firefly at each place of a 2x2 block of pixels, which one texel of the first bloom level covers.
    for (const float radiance : kFireflyRadiances) {
        for (const glm::ivec2 offset : {glm::ivec2(0, 0), glm::ivec2(1, 0), glm::ivec2(0, 1), glm::ivec2(1, 1)}) {
            writePattern(device, targets->hdr(), 0.0f,
                         {PatternRect{kCenter + offset, glm::ivec2(1), glm::vec3(radiance), glm::vec3(0.0f)}});
            auto firefly = bloomOf(device, *processor, post, settings, *targets);
            if (!firefly) {
                return propagate(firefly.error());
            }
            measurements.fireflyEnergies.push_back(bloomEnergy(*firefly, *targets));
        }
    }
    gl::Framebuffer::unbind();
    return measurements;
}

} // namespace rv::app
