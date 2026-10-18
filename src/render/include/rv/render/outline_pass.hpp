#pragma once

// The hover outline (ADR-0012 §3, I3 step 8): the highlighted instances drawn once more into the stencil of the HDR
// target after the lit pass, the stencil read through a stencil-index view of its depth/stencil texture, the 2 px edge
// marked into the output after the anti-aliasing, timed on the GPU on its own.

#include <array>
#include <cstdint>
#include <optional>

#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/gpu_timer.hpp>
#include <rv/render/vertex_array.hpp>
#include <rv/scene/instance_clusters.hpp>

namespace rv::gl {

class GLDevice;
class ShaderProgram;
class ViewTargets;

/// Programs of the outline. Without both mark programs and the edge program no outline is drawn.
struct OutlineShaders {
    /// pbr/pbr.vert + outline/outline_mark.frag: opaque batches.
    const ShaderProgram* mark = nullptr;
    /// The same with RV_ALPHA_TEST: alpha-masked batches, whose cut-outs stay out of the mark.
    const ShaderProgram* markMasked = nullptr;
    /// post/fullscreen.vert + outline/outline_edge.frag.
    const ShaderProgram* edge = nullptr;
};

/// How a view draws the outline of its highlighted instances (scene::instance_flags::kHighlighted).
struct OutlineSettings {
    bool enabled = true;
    /// sRGB-encoded bytes the outline's pixels hold in the output.
    std::array<std::uint8_t, 3> colorSrgb = scene::kOutlineColorSrgb;
    /// Pixels at 1080 output rows; scaled with the output's height (scene::outlineWidthPixels).
    float widthAt1080 = scene::kOutlineWidthAt1080;
};

/// The outline of a view, in two steps that gl::SceneRenderer runs after the post-processing (so that the anti-aliasing
/// neither softens the outline nor makes it flicker), with the instance draws of the mark between them:
///  1. beginMark(): the HDR target bound with its stencil cleared to 0, colour writes off, the depth test GL_LEQUAL
///     against the scene's depth without writes, every fragment that passes setting the stencil to 1. The caller then
///     draws the highlighted instances with OutlineShaders::mark (or markMasked), whose fragment stage keeps only the
///     fragments of instances flagged kHighlighted: the stencil holds the visible part of their silhouettes.
///  2. drawEdge(): the output bound, GL_FRAMEBUFFER_SRGB off, a fullscreen triangle scissored to `rect` whose fragment
///     stage reads the stencil through ViewTargets::hdr()'s stencil view (Framebuffer::bindStencilTexture,
///     OutlineTextureUnit::Stencil) and writes OutlineSettings::colorSrgb as it is into every pixel outside the
///     silhouettes within scene::outlineWidthPixels(height) of one inside (scene::outlineCovers), and nothing elsewhere.
/// A GpuTimer measures both steps. Leaves the state defaults documented on GLDevice::state() (depth test on with writes,
/// back faces culled, blending off, the stencil and scissor tests off) and the output bound for drawing with
/// GL_FRAMEBUFFER_SRGB enabled, as the post-processing leaves them. Main thread only; destroy it while the GL context is
/// current.
class OutlinePass {
public:
    /// An empty pass: draws nothing.
    OutlinePass() noexcept = default;

    /// Creates the empty vertex array of the fullscreen triangle and the GPU timer. Errors: GPU objects that cannot be
    /// created.
    [[nodiscard]] static Expected<OutlinePass> create();

    /// Step 1 for `targets` (see above).
    void beginMark(GLDevice& device, const ViewTargets& targets);
    /// Step 2: the edge around what was marked, within the pixel rectangle `rect` ({x, y, width, height}, bottom-left
    /// origin: scene::screenRect of the highlighted bounds grown by the outline's reach).
    void drawEdge(GLDevice& device, const ShaderProgram& edge, const OutlineSettings& settings,
                  const ViewTargets& targets, const glm::ivec4& rect);

    /// GPU time of both steps of the most recent outline whose timer query has completed, in milliseconds (GpuTimer:
    /// results arrive a frame or two late); nothing before the first result.
    [[nodiscard]] std::optional<double> latestGpuMilliseconds() { return m_timer.latestMilliseconds(); }

    [[nodiscard]] bool valid() const noexcept { return m_triangle.valid(); }

private:
    VertexArray m_triangle;
    GpuTimer m_timer;
    bool m_marking = false;
};

} // namespace rv::gl
