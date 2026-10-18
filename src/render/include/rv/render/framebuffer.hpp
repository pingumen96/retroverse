#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::gl {

class RenderStateCache;

/// Internal format of a framebuffer's color attachment.
enum class ColorFormat : std::uint8_t {
    /// GL_SRGB8_ALPHA8: with GL_FRAMEBUFFER_SRGB enabled, linear shader output is encoded to sRGB on write, so the
    /// stored bytes are display-ready (window presentation, screenshots).
    Srgb8Alpha8,
    /// GL_RGBA16F: linear values beyond [0, 1] (up to 65504), the HDR scene and its bloom before tonemapping.
    Rgba16F,
    /// GL_R8: one channel in [0, 1], 8 bits (the ambient occlusion the lit shader reads).
    R8,
    /// GL_RG16F: two half floats (the half-resolution ambient occlusion and the distance it was estimated at).
    Rg16F,
};

/// "srgb8_alpha8", "rgba16f", "r8" or "rg16f".
[[nodiscard]] std::string_view toString(ColorFormat format) noexcept;
/// The storage format of `format`, for the memory accounting.
[[nodiscard]] GpuFormat gpuFormat(ColorFormat format) noexcept;

/// Most color attachments a Framebuffer has (FramebufferDesc::colorAttachments).
inline constexpr std::size_t kMaxColorAttachments = 2;

struct FramebufferDesc {
    /// Pixels; each side in [1, min(GL_MAX_FRAMEBUFFER_WIDTH/HEIGHT, GL_MAX_TEXTURE_SIZE, GL_MAX_RENDERBUFFER_SIZE)]
    /// (at least 16384 on any OpenGL 4.5 implementation).
    glm::ivec2 size{0};
    /// Format of every color attachment.
    ColorFormat colorFormat = ColorFormat::Srgb8Alpha8;
    /// Adds a GL_DEPTH24_STENCIL8 buffer at GL_DEPTH_STENCIL_ATTACHMENT.
    bool depthStencil = true;
    /// Color attachments, 1 to kMaxColorAttachments: GL_COLOR_ATTACHMENT0 + i is the draw buffer of fragment output i.
    std::size_t colorAttachments = 1;
    /// With depthStencil: the depth/stencil buffer is a texture that later passes can sample (bindDepthTexture():
    /// window depth in [0, 1], nearest filtering unless a sampler says otherwise) instead of a renderbuffer.
    bool depthTexture = false;
};

/// Bytes of the attachments of `desc` (every color attachment and the depth/stencil buffer, one level each): what a
/// Framebuffer records as GpuMemoryKind::Targets. 0 for an empty size.
[[nodiscard]] std::size_t framebufferBytes(const FramebufferDesc& desc) noexcept;

/// Move-only offscreen render target (DSA): one or more color textures with immutable storage (one mip level, linear
/// filtering, clamped to edge) at GL_COLOR_ATTACHMENT0, 1, ..., all drawn into at once, and an optional
/// depth24/stencil8 buffer (a renderbuffer, or a texture with FramebufferDesc::depthTexture). The first color
/// attachment is the read buffer: the copies, blitToDefault() and readPixels() take it. Rendering into it starts with
/// GLDevice::beginPass(); later passes sample its color textures (bindColorTexture()); blitToDefault() presents it in
/// the window and readPixels() returns its pixels for screenshots. Its pixel rows are bottom-first like every OpenGL
/// framebuffer. Its attachments are recorded in gpuMemoryLedger() as GpuMemoryKind::Targets (framebufferBytes()). A
/// default-constructed or moved-from Framebuffer is empty. Main thread only.
class Framebuffer {
public:
    Framebuffer() noexcept = default;

    /// Allocates the attachments and checks completeness. Errors: OutOfRange for a size or a color attachment count
    /// outside the limits above, GraphicsError when an allocation fails (out of memory) or the driver reports the
    /// framebuffer incomplete.
    [[nodiscard]] static Expected<Framebuffer> create(const FramebufferDesc& desc, std::string_view debugName);

    /// Reallocates the attachments at `size`; their contents are undefined afterwards. No-op for the current size. On
    /// error the framebuffer keeps its previous attachments.
    [[nodiscard]] Status resize(glm::ivec2 size);

    /// Binds this framebuffer for drawing and reading (glBindFramebuffer(GL_FRAMEBUFFER)). The viewport is the
    /// caller's: GLDevice::beginPass() binds, sets the viewport and clears.
    void bind() const;
    /// Binds the default framebuffer (the window's back buffer).
    static void unbind();

    /// Clears the first color attachment to `colorLinear` (linear RGBA, encoded to sRGB by an Srgb8Alpha8 attachment
    /// while GL_FRAMEBUFFER_SRGB is enabled), the others to 0, depth to 1 and stencil to 0. Like glClear it honors the
    /// color, depth and stencil write masks, which GLDevice::beginPass() enables, and the scissor test.
    void clear(const glm::vec4& colorLinear) const;

    /// Binds color attachment `attachment` to texture unit `unit` (glBindTextureUnit) for a later pass to sample; the
    /// sampler bound to that unit, if any, decides how. An attachment this framebuffer does not have unbinds the unit.
    /// Do not sample it while this framebuffer is bound for drawing.
    void bindColorTexture(GLuint unit, std::size_t attachment = 0) const;
    /// Binds the depth/stencil texture (FramebufferDesc::depthTexture) to texture unit `unit`: sampled as window depth
    /// in [0, 1] (GL_DEPTH_STENCIL_TEXTURE_MODE GL_DEPTH_COMPONENT). A framebuffer without one unbinds the unit. Do not
    /// sample it while this framebuffer is bound for drawing.
    void bindDepthTexture(GLuint unit) const;

    /// Copies the depth and stencil values into `destination`'s (glBlitNamedFramebuffer, exact), so that what is drawn
    /// there afterwards is depth-tested against this framebuffer's scene. Does nothing unless both have the same size
    /// and a depth/stencil buffer.
    void copyDepthStencilTo(const Framebuffer& destination) const;
    /// Copies the color attachment into `destination`'s, stretched to its size: the values are converted to its
    /// format (clamped to [0, 1] for Srgb8Alpha8, and encoded to sRGB while GL_FRAMEBUFFER_SRGB is enabled). Does
    /// nothing for an empty framebuffer.
    void copyColorTo(const Framebuffer& destination) const;

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
    /// them; the other formats return their values clamped to [0, 1] (channels they lack read 0, alpha 1). Waits for
    /// pending rendering (a pipeline stall: meant for screenshots and tests, not for every frame).
    [[nodiscard]] Expected<assets::ImageData> readPixels() const;
    /// Reads the first color attachment of an Rgba16F framebuffer back as linear RGBA, bottom row first (texel order),
    /// the values as stored. A pipeline stall like readPixels(): meant for checks and tests. Errors: InvalidArgument
    /// for an empty or Srgb8Alpha8 framebuffer, OutOfRange when the pixels do not fit one readback.
    [[nodiscard]] Expected<std::vector<glm::vec4>> readLinearPixels() const;

    [[nodiscard]] bool valid() const noexcept { return m_framebuffer.valid(); }
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_desc.size; }
    [[nodiscard]] ColorFormat colorFormat() const noexcept { return m_desc.colorFormat; }
    [[nodiscard]] std::size_t colorAttachments() const noexcept { return m_desc.colorAttachments; }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    /// True when the framebuffer has a depth/stencil buffer (renderbuffer or texture).
    [[nodiscard]] bool hasDepthStencil() const noexcept {
        return m_depthStencil.valid() || m_depthStencilTexture.valid();
    }

    FramebufferDesc m_desc;
    std::string m_debugName;
    std::array<TextureObject, kMaxColorAttachments> m_colors;
    /// The depth/stencil buffer: a renderbuffer, or a texture with FramebufferDesc::depthTexture.
    RenderbufferObject m_depthStencil;
    TextureObject m_depthStencilTexture;
    FramebufferObject m_framebuffer;
    GpuMemoryAllocation m_memory;
};

} // namespace rv::gl
