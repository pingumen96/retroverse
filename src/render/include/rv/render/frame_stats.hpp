#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstddef>
#include <optional>

namespace rv::gl {

/// What one SceneRenderer::render() call drew (one view of the scene) and how long the GPU took. The counts without a
/// prefix are those of the lit scene pass; the shadow map and the depth pre-pass have their own. The static instance
/// groups of the view (RenderView::staticGroups) count in the batches, draw calls, instances and triangles, and again
/// on their own (the static* fields).
struct FrameStats {
    std::size_t items = 0;            ///< render items submitted (the static groups' instances not included)
    std::size_t culledItems = 0;      ///< items outside the view frustum, not drawn
    std::size_t batches = 0;          ///< instanced batches: planned (scene::BatchPlan) and the static groups'
    std::size_t drawCalls = 0;        ///< instanced draw calls issued (batches whose program is available)
    std::size_t instances = 0;        ///< mesh instances drawn
    std::size_t triangles = 0;        ///< triangles drawn: instances times the triangles of their mesh
    std::size_t staticGroups = 0;     ///< static instance groups of the view
    std::size_t staticBatches = 0;    ///< their pre-sorted batches (part of batches)
    std::size_t clusters = 0;         ///< their clusters that hold instances
    std::size_t culledClusters = 0;   ///< those outside the view frustum, not drawn
    std::size_t staticDrawCalls = 0;  ///< draw calls of the lit pass from the static groups (part of drawCalls)
    std::size_t staticInstances = 0;  ///< instances those draws drew (part of instances)
    std::size_t outlinedInstances = 0; ///< highlighted instances drawn into the outline's mark
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
    /// GPU time of the hover outline (its mark into the stencil and its edge into the output), like
    /// shadowGpuMilliseconds; empty for a view that drew no outline.
    std::optional<double> outlineGpuMilliseconds;
};

} // namespace rv::gl
