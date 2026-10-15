#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/post_settings.hpp>

namespace rv::gl {

/// The render targets one output of the renderer (the window, a screenshot) is drawn through, all for the size of that
/// output:
///  - hdr(): two GL_RGBA16F color attachments + a depth24/stencil8 texture. The scene pass writes the fragment outputs
///    of the scene shaders into them (SceneOutput, SceneRenderer): the linear radiance (the first attachment, the one
///    copies and readbacks take) and its emitted part, the light the bloom always spreads (PostProcessor). Its depth,
///    written by the depth pre-pass, is what the ambient occlusion is estimated from (Framebuffer::bindDepthTexture).
///  - ambientOcclusionRaw(): GL_RG16F at ssaoTargetSize(size()), half the resolution: the ambient occlusion estimate
///    and the distance it was taken at (SsaoPass).
///  - ambientOcclusion(): GL_R8 at size(): the blurred ambient occlusion the lit shader reads, 1 where nothing occludes
///    (SsaoPass).
///  - bloomLevels(): kBloomLevelCount GL_RGBA16F targets of bloomLevelSizes(size()), the bloom chain (PostProcessor).
///  - tonemapped(): GL_SRGB8_ALPHA8 at size(), no depth: the tonemapped image with the perceptual luma of each pixel in
///    its alpha, which the post pass writes into it when the anti-aliasing pass follows (PostProcessor).
///  - output(): GL_SRGB8_ALPHA8 color + depth24/stencil8. The post pass (or the anti-aliasing pass after it) writes the
///    tonemapped image into it, debug lines and overlays are drawn over it; it is what the window shows
///    (Framebuffer::blitToDefault) and what screenshots read (Framebuffer::readPixels). Its alpha is not meant to be
///    shown: 1, or the luma the post pass writes when no anti-aliasing follows.
/// Allocated once per size: resize() reallocates them only when the size changes, so drawing allocates nothing.
/// Move-only; a default-constructed ViewTargets is empty. Main thread only.
class ViewTargets {
public:
    ViewTargets() noexcept = default;

    /// Allocates every target for an output of `size` pixels, labeled "<debugName>.hdr", "<debugName>.ssaoRaw",
    /// "<debugName>.ssao", "<debugName>.bloom<i>", "<debugName>.tonemapped" and "<debugName>.output". Errors: those of
    /// Framebuffer::create (a size outside the limits, an allocation failure).
    [[nodiscard]] static Expected<ViewTargets> create(glm::ivec2 size, std::string_view debugName);

    /// Reallocates every target at `size`; their contents are undefined afterwards. No-op for the current size. On
    /// error the previous targets are kept, all of the previous size.
    [[nodiscard]] Status resize(glm::ivec2 size);

    [[nodiscard]] bool valid() const noexcept { return m_output.valid(); }
    /// Size of the output, and of the HDR target.
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_output.size(); }
    [[nodiscard]] const Framebuffer& hdr() const noexcept { return m_hdr; }
    /// The half-resolution ambient occlusion estimate.
    [[nodiscard]] const Framebuffer& ambientOcclusionRaw() const noexcept { return m_ambientOcclusionRaw; }
    /// The ambient occlusion of the view at its resolution.
    [[nodiscard]] const Framebuffer& ambientOcclusion() const noexcept { return m_ambientOcclusion; }
    /// The bloom chain, largest level first.
    [[nodiscard]] std::span<const Framebuffer, kBloomLevelCount> bloomLevels() const noexcept { return m_bloom; }
    /// The tonemapped image before anti-aliasing.
    [[nodiscard]] const Framebuffer& tonemapped() const noexcept { return m_tonemapped; }
    [[nodiscard]] const Framebuffer& output() const noexcept { return m_output; }

private:
    Framebuffer m_hdr;
    Framebuffer m_ambientOcclusionRaw;
    Framebuffer m_ambientOcclusion;
    std::array<Framebuffer, kBloomLevelCount> m_bloom;
    Framebuffer m_tonemapped;
    Framebuffer m_output;
    std::string m_debugName;
};

} // namespace rv::gl
