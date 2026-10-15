#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstddef>
#include <optional>

namespace rv::gl {

/// What one SceneRenderer::render() call drew (one view of the scene) and how long the GPU took. The counts without a
/// prefix are those of the lit scene pass; the shadow map and the depth pre-pass have their own.
struct FrameStats {
    std::size_t items = 0;            ///< render items submitted
    std::size_t culledItems = 0;      ///< items outside the view frustum, not drawn
    std::size_t batches = 0;          ///< instanced batches planned (scene::BatchPlan)
    std::size_t drawCalls = 0;        ///< instanced draw calls issued (batches whose program is available)
    std::size_t instances = 0;        ///< mesh instances drawn
    std::size_t triangles = 0;        ///< triangles drawn: instances times the triangles of their mesh
    std::size_t shaderSwitches = 0;   ///< programs bound (the first one included)
    std::size_t materialSwitches = 0; ///< materials bound (the first one included)
    std::size_t debugLines = 0;       ///< debug lines drawn after the scene
    /// Draw calls and instances of the key light's shadow map (0 when the view has no shadow).
    std::size_t shadowDrawCalls = 0;
    std::size_t shadowInstances = 0;
    /// Draw calls of the depth pre-pass (the opaque and alpha-masked batches of the view, depth only).
    std::size_t depthDrawCalls = 0;
    /// GPU time of the shadow map pass of the most recent view whose timer query has completed, in milliseconds:
    /// results arrive a frame or two late, so this usually measures an earlier view. Empty until the first result (and
    /// in runs whose views never had a shadow).
    std::optional<double> shadowGpuMilliseconds;
    /// GPU time of the depth pre-pass (the clear of the HDR target and the depth of the opaque and masked batches),
    /// like shadowGpuMilliseconds.
    std::optional<double> depthGpuMilliseconds;
    /// GPU time of the ambient occlusion (the half-resolution estimate and the depth-aware blur), like
    /// shadowGpuMilliseconds.
    std::optional<double> ssaoGpuMilliseconds;
    /// GPU time of the lit scene pass (the batches into the HDR target), like shadowGpuMilliseconds.
    std::optional<double> sceneGpuMilliseconds;
    /// GPU time of the post-processing (bloom chain and post pass into the output target), like
    /// shadowGpuMilliseconds. Debug lines are not timed.
    std::optional<double> postGpuMilliseconds;
};

} // namespace rv::gl
