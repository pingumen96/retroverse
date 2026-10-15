#pragma once

#include <optional>

#include <rv/core/expected.hpp>
#include <rv/core/failure_streak.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/render/post_settings.hpp>
#include <rv/render/sampler.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/render/vertex_array.hpp>

namespace rv::gl {

class Framebuffer;
class GLDevice;
class ShaderProgram;
class ViewTargets;

/// Programs of the post-processing, each post/fullscreen.vert with a fragment stage. A null or empty program (a shader
/// that never built) skips its pass: bloom needs the three bloom programs (the view is composited without bloom
/// otherwise); without the anti-aliasing program the post pass writes straight into the output; without the post
/// program the HDR scene is copied to the output as it is (clamped to [0, 1], neither exposed nor tonemapped), so that
/// the scene stays visible.
struct PostShaders {
    const ShaderProgram* bloomDownsampleFirst = nullptr; ///< post/bloom_downsample.frag with RV_BLOOM_KARIS
    const ShaderProgram* bloomDownsample = nullptr;      ///< post/bloom_downsample.frag
    const ShaderProgram* bloomUpsample = nullptr;        ///< post/bloom_upsample.frag
    const ShaderProgram* post = nullptr;                 ///< post/post.frag
    const ShaderProgram* antialiasing = nullptr;         ///< post/fxaa.frag
};

/// The post-processing of a view (style §4.3, spec §14.2): from the HDR scene of a ViewTargets to its sRGB output, in
/// passes that each draw one fullscreen triangle without a vertex buffer (post/fullscreen.vert, an empty vertex array):
///  1. Bloom, when the settings ask for it (bloomStrength > 0), into ViewTargets::bloomLevels(): the light of the scene
///     that blooms (common/post.glsl bloomSource: all the light surfaces emit, from the HDR target's emission
///     attachment, and the reflected light above the soft threshold kBloomThreshold, the highlights) is downsampled
///     level by level with the 13-tap filter, the first level with the Karis average against fireflies; then, from the
///     smallest level up, each level is mixed with the tent upsampling of the next smaller one by the fixed-function
///     blend: level = mix(level, upsampled, bloomScatter(bloomRadius)), the blend constant.
///  2. The post pass (post/post.frag) with GL_FRAMEBUFFER_SRGB enabled: the share bloomStrength of the light that
///     blooms replaced by its bloom (energy is moved, never added), the vignette, exposure and the ACES fitted curve,
///     then dithering; the perceptual luma of each pixel goes into the alpha. It draws into ViewTargets::output(), or
///     into ViewTargets::tonemapped() when the settings ask for anti-aliasing.
///  3. Anti-aliasing, when the settings ask for it: the FXAA pass (post/fxaa.frag) from ViewTargets::tonemapped() into
///     the output, which smooths the stair-steps of edges and copies every other pixel exactly.
/// The parameters reach the shaders through the PostUniforms block (UniformBinding::Post, written per call); the
/// sources are bound to the PostTextureUnit units with a bilinear sampler clamped to the edges. A GpuTimer measures
/// all the steps. When the block cannot be written, the view is shown without post-processing like without the post
/// program (logged once, and again only when the cause changes). Nothing is allocated per call. Main thread only;
/// destroy it while the GL context is current.
class PostProcessor {
public:
    /// An empty post-processor, whose apply() does nothing.
    PostProcessor() noexcept = default;

    /// Creates the uniform buffer, the empty vertex array, the sampler and the GPU timer. Errors: GPU objects that
    /// cannot be created.
    [[nodiscard]] static Expected<PostProcessor> create();

    /// Post-processes the HDR scene of `targets` into their output with `settings`. Leaves the output bound for drawing
    /// with a viewport of its size and GL_FRAMEBUFFER_SRGB enabled, and the state defaults documented on
    /// GLDevice::state() (depth test on, back faces culled, blending off with (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
    /// filled polygons). Does nothing for empty targets or an empty post-processor.
    void apply(GLDevice& device, const PostShaders& shaders, const PostSettings& settings, const ViewTargets& targets);

    /// GPU time of the most recent apply() whose timer query has completed, in milliseconds (GpuTimer: results arrive a
    /// frame or two late); nothing before the first result.
    [[nodiscard]] std::optional<double> latestGpuMilliseconds() { return m_timer.latestMilliseconds(); }

    [[nodiscard]] bool valid() const noexcept { return m_uniforms.valid(); }

private:
    /// The bloom chain of step 1 into the bloom levels of `targets`.
    void bloom(GLDevice& device, const PostShaders& shaders, float scatter, const ViewTargets& targets) const;
    /// Draws the fullscreen triangle with `program` into `target`, sampling `source` on PostTextureUnit::Source.
    void drawPass(GLDevice& device, const ShaderProgram& program, const Framebuffer& source,
                  const Framebuffer& target) const;

    UniformBuffer<PostUniforms> m_uniforms;
    /// Bound for the fullscreen triangles, which read no vertex attribute.
    VertexArray m_triangle;
    Sampler m_sampler;
    GpuTimer m_timer;
    /// Calls whose PostUniforms could not be written, for the log.
    core::FailureStreak m_uniformFailures;
};

} // namespace rv::gl
