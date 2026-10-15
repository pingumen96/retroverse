#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>

namespace rv::gl {

class RenderStateCache;

/// Internal format of a framebuffer's color attachment.
enum class ColorFormat : std::uint8_t {
    /// GL_SRGB8_ALPHA8: with GL_FRAMEBUFFER_SRGB enabled, linear shader output is encoded to sRGB on write, so the
    /// stored bytes are display-ready (window presentation, screenshots).
    Srgb8Alpha8,
    /// GL_RGBA16F: linear values beyond [0, 1] for HDR rendering before tonemapping (later steps).
    Rgba16F,
};

/// "srgb8_alpha8" or "rgba16f".
[[nodiscard]] std::string_view toString(ColorFormat format) noexcept;

struct FramebufferDesc {
    /// Pixels; each side in [1, min(GL_MAX_FRAMEBUFFER_WIDTH/HEIGHT, GL_MAX_TEXTURE_SIZE, GL_MAX_RENDERBUFFER_SIZE)]
    /// (at least 16384 on any OpenGL 4.5 implementation).
    glm::ivec2 size{0};
    ColorFormat colorFormat = ColorFormat::Srgb8Alpha8;
    /// Adds a GL_DEPTH24_STENCIL8 renderbuffer at GL_DEPTH_STENCIL_ATTACHMENT.
    bool depthStencil = true;
};

/// Move-only offscreen render target (DSA): a color texture with immutable storage (one mip level, linear filtering,
/// clamped to edge) at GL_COLOR_ATTACHMENT0, which is both its draw and read buffer, and an optional depth24/stencil8
/// renderbuffer. Rendering into it starts with GLDevice::beginPass(); blitToDefault() presents it in the window and
/// readPixels() returns its pixels for screenshots. Its pixel rows are bottom-first like every OpenGL framebuffer.
/// A default-constructed or moved-from Framebuffer is empty. Main thread only.
class Framebuffer {
public:
    Framebuffer() noexcept = default;

    /// Allocates the attachments and checks completeness. Errors: OutOfRange for a size outside the limits above,
    /// GraphicsError when an allocation fails (out of memory) or the driver reports the framebuffer incomplete.
    [[nodiscard]] static Expected<Framebuffer> create(const FramebufferDesc& desc, std::string_view debugName);

    /// Reallocates the attachments at `size`; their contents are undefined afterwards. No-op for the current size. On
    /// error the framebuffer keeps its previous attachments.
    [[nodiscard]] Status resize(glm::ivec2 size);

    /// Binds this framebuffer for drawing and reading (glBindFramebuffer(GL_FRAMEBUFFER)). The viewport is the
    /// caller's: GLDevice::beginPass() binds, sets the viewport and clears.
    void bind() const;
    /// Binds the default framebuffer (the window's back buffer).
    static void unbind();

    /// Clears color to `colorLinear` (linear RGBA, encoded to sRGB by an Srgb8Alpha8 attachment while
    /// GL_FRAMEBUFFER_SRGB is enabled), depth to 1 and stencil to 0. Like glClear it honors the color, depth and
    /// stencil write masks, which GLDevice::beginPass() enables.
    void clear(const glm::vec4& colorLinear) const;

    /// Copies the color attachment to the default framebuffer, stretched over (0, 0)-(windowSize), then binds the
    /// default framebuffer with a matching viewport for overlays. GL_FRAMEBUFFER_SRGB is disabled first, so the stored
    /// bytes are copied verbatim: the window shows the same pixels as readPixels() returns, whether or not the
    /// window's framebuffer is sRGB-capable. Does nothing for an empty framebuffer or an empty window.
    void blitToDefault(RenderStateCache& state, glm::ivec2 windowSize) const;
    /// Clears the default framebuffer to opaque black and binds it with a viewport of (0, 0)-(windowSize), the state
    /// blitToDefault() leaves: the window of a frame that has no view to show, under the overlays drawn next. Does
    /// nothing for an empty window.
    static void clearDefault(RenderStateCache& state, glm::ivec2 windowSize);

    /// Reads the color attachment back as RGBA8 with the top row first (rows flipped once, ADR-0002). Srgb8Alpha8
    /// returns the stored sRGB-encoded bytes, read through a GL_RGBA8 view of the texture so that no driver converts
    /// them; Rgba16F returns linear values clamped to [0, 1]. Waits for pending rendering (a pipeline stall: meant for
    /// screenshots and tests, not for every frame).
    [[nodiscard]] Expected<assets::ImageData> readPixels() const;

    [[nodiscard]] bool valid() const noexcept { return m_framebuffer.valid(); }
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_desc.size; }
    [[nodiscard]] ColorFormat colorFormat() const noexcept { return m_desc.colorFormat; }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    FramebufferDesc m_desc;
    std::string m_debugName;
    TextureObject m_color;
    RenderbufferObject m_depthStencil;
    FramebufferObject m_framebuffer;
};

} // namespace rv::gl
