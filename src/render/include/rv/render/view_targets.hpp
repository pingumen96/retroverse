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
///  - hdr(): GL_RGBA16F color + depth24/stencil8. The scene pass writes linear radiance into it (SceneRenderer).
///  - bloomLevels(): kBloomLevelCount GL_RGBA16F targets of bloomLevelSizes(size()), the bloom chain (PostProcessor).
///  - output(): GL_SRGB8_ALPHA8 color + depth24/stencil8. The post pass writes the tonemapped image into it, debug
///    lines and overlays are drawn over it; it is what the window shows (Framebuffer::blitToDefault) and what
///    screenshots read (Framebuffer::readPixels).
/// Allocated once per size: resize() reallocates them only when the size changes, so drawing allocates nothing.
/// Move-only; a default-constructed ViewTargets is empty. Main thread only.
class ViewTargets {
public:
    ViewTargets() noexcept = default;

    /// Allocates every target for an output of `size` pixels, labeled "<debugName>.hdr", "<debugName>.bloom<i>" and
    /// "<debugName>.output". Errors: those of Framebuffer::create (a size outside the limits, an allocation failure).
    [[nodiscard]] static Expected<ViewTargets> create(glm::ivec2 size, std::string_view debugName);

    /// Reallocates every target at `size`; their contents are undefined afterwards. No-op for the current size. On
    /// error the previous targets are kept, all of the previous size.
    [[nodiscard]] Status resize(glm::ivec2 size);

    [[nodiscard]] bool valid() const noexcept { return m_output.valid(); }
    /// Size of the output, and of the HDR target.
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_output.size(); }
    [[nodiscard]] const Framebuffer& hdr() const noexcept { return m_hdr; }
    /// The bloom chain, largest level first.
    [[nodiscard]] std::span<const Framebuffer, kBloomLevelCount> bloomLevels() const noexcept { return m_bloom; }
    [[nodiscard]] const Framebuffer& output() const noexcept { return m_output; }

private:
    Framebuffer m_hdr;
    std::array<Framebuffer, kBloomLevelCount> m_bloom;
    Framebuffer m_output;
    std::string m_debugName;
};

} // namespace rv::gl
