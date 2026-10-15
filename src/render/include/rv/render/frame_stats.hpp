#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstddef>
#include <optional>

namespace rv::gl {

/// What one SceneRenderer::render() call drew (one view of the scene).
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
    /// GPU time of the most recent view whose timer query has completed, in milliseconds: results arrive a frame or two
    /// late, so this usually measures an earlier view. Empty until the first result.
    std::optional<double> gpuMilliseconds;
};

} // namespace rv::gl
