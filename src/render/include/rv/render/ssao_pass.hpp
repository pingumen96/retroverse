#pragma once

#include <optional>

#include <rv/core/expected.hpp>
#include <rv/core/failure_streak.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/render/sampler.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/render/uniform_buffer.hpp>
#include <rv/render/vertex_array.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::gl {

class GLDevice;
class ShaderProgram;
class ViewTargets;

/// Programs of the ambient occlusion, each post/fullscreen.vert with a fragment stage. A null or empty program (a
/// shader that never built) leaves the view without ambient occlusion.
struct SsaoShaders {
    const ShaderProgram* occlusion = nullptr; ///< ssao/ssao.frag: the half-resolution estimate
    const ShaderProgram* blur = nullptr;      ///< ssao/ssao_blur.frag: the depth-aware blur to full resolution
};

/// Screen-space ambient occlusion of a view (style §4.1 contact cues, common/ssao.glsl): from the depth the depth
/// pre-pass left in the HDR target of a ViewTargets, into its ambientOcclusion() target, which the lit shader reads to
/// darken the ambient light only (common/occlusion.glsl), in two passes that each draw one fullscreen triangle without
/// a vertex buffer:
///  1. the estimate at half the resolution of the view (ViewTargets::ambientOcclusionRaw()): kSsaoSampleCount samples
///     of the hemisphere above each surface point, with normals reconstructed from the depth;
///  2. the depth-aware blur to the full resolution (ViewTargets::ambientOcclusion()), which also removes the 4 x 4
///     rotation pattern of the estimate.
/// The camera comes from the FrameUniforms block bound at UniformBinding::Frame; the parameters (scene::SsaoSetup:
/// radius, intensity) reach the shaders through the SsaoUniforms block (UniformBinding::Ssao, rewritten only when they
/// change). The scene depth is read on SsaoTextureUnit::SceneDepth with a bilinear sampler clamped to the edges. When
/// the occlusion is off, a program is missing or the block cannot be written (logged once, and again only when the
/// cause changes), the target is cleared to 1: nothing is occluded. A GpuTimer measures both passes. Nothing is
/// allocated per call. Main thread only; destroy it while the GL context is current.
class SsaoPass {
public:
    /// An empty pass, whose apply() does nothing.
    SsaoPass() noexcept = default;

    /// Creates the uniform buffer, the empty vertex array, the depth sampler and the GPU timer. Errors: GPU objects
    /// that cannot be created.
    [[nodiscard]] static Expected<SsaoPass> create();

    /// Computes the ambient occlusion of the scene depth of `targets` with `setup`, or clears it to 1 (see above).
    /// Leaves the ambient occlusion target bound for drawing with a viewport of its size, and the state defaults
    /// documented on GLDevice::state() (depth test on, back faces culled, blending off). Does nothing for empty
    /// targets or an empty pass.
    void apply(GLDevice& device, const SsaoShaders& shaders, const scene::SsaoSetup& setup, const ViewTargets& targets);

    /// GPU time of the most recent apply() whose timer query has completed, in milliseconds (GpuTimer: results arrive a
    /// frame or two late); nothing before the first result.
    [[nodiscard]] std::optional<double> latestGpuMilliseconds() { return m_timer.latestMilliseconds(); }

    [[nodiscard]] bool valid() const noexcept { return m_uniforms.valid(); }

private:
    /// Writes the block for `setup` when it differs from the one written last; false when it cannot be written.
    bool updateUniforms(const scene::SsaoSetup& setup);

    UniformBuffer<SsaoUniforms> m_uniforms;
    /// The settings of the block's contents, once written.
    std::optional<scene::SsaoSetup> m_uploaded;
    /// Bound for the fullscreen triangles, which read no vertex attribute.
    VertexArray m_triangle;
    /// Bilinear and clamped: the estimate reads the scene depth between pixels.
    Sampler m_depthSampler;
    GpuTimer m_timer;
    /// Calls whose SsaoUniforms could not be written, for the log.
    core::FailureStreak m_uniformFailures;
};

} // namespace rv::gl
