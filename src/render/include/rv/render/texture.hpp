#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <glad/gl.h>
#include <glm/vec2.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>
#include <rv/render/texture_memory.hpp>

namespace rv::gl {

/// How the texels of a texture are interpreted (ADR-0002). Color data (base color, emissive, pictures) is sRGB
/// encoded and stored as GL_SRGB8_ALPHA8, so sampling returns linear values; data textures (metallic-roughness,
/// normal, occlusion) are linear and stored as GL_RGBA8.
enum class ColorSpace : std::uint8_t { Srgb, Linear };

/// "srgb" or "linear".
[[nodiscard]] std::string_view toString(ColorSpace colorSpace) noexcept;

/// Texture coordinate wrapping, for both axes.
enum class TextureWrap : std::uint8_t {
    Repeat,         ///< GL_REPEAT: tiling surfaces (wood, walls)
    ClampToEdge,    ///< GL_CLAMP_TO_EDGE: single pictures (labels, posters, screens)
    MirroredRepeat, ///< GL_MIRRORED_REPEAT
};
inline constexpr std::size_t kTextureWrapCount = 3;

/// "repeat", "clamp_to_edge" or "mirrored_repeat".
[[nodiscard]] std::string_view toString(TextureWrap wrap) noexcept;

enum class TextureFilter : std::uint8_t {
    Nearest,   ///< GL_NEAREST for minification and magnification, base level only
    Bilinear,  ///< GL_LINEAR, base level only
    Trilinear, ///< GL_LINEAR_MIPMAP_LINEAR / GL_LINEAR (bilinear on a texture without mipmaps)
};

/// Sampling state: the parameters of a texture (used while no sampler object is bound to its unit) or of a Sampler.
struct SamplingDesc {
    TextureWrap wrap = TextureWrap::Repeat;
    TextureFilter filter = TextureFilter::Trilinear;
    /// GL_TEXTURE_MAX_ANISOTROPY, applied when > 1 (with Trilinear and Bilinear filtering). Must not exceed
    /// GLCapabilities::maxAnisotropy, which is 1 when the driver has no anisotropic filtering: see clampAnisotropy().
    float anisotropy = 1.0f;
};

/// `requested` clamped to [1, maxSupported] (GLCapabilities::maxAnisotropy); 1 for non-finite input.
[[nodiscard]] float clampAnisotropy(float requested, float maxSupported) noexcept;

/// A GL_RGBA8 view (glTextureView) of the first `levelCount` levels of `texture`, an immutable GL_RGBA8 or
/// GL_SRGB8_ALPHA8 texture: sampling it returns the stored bytes as they are, without sRGB decoding, so an sRGB image
/// drawn without sRGB encoding (the developer overlay draws so) shows the colors it holds, and reading it back returns
/// them unconverted on every driver (Framebuffer::readPixels). Trilinear filtering. Empty when the driver creates
/// none.
[[nodiscard]] TextureObject createRgba8View(GLuint texture, int levelCount);

struct TextureDesc {
    ColorSpace colorSpace = ColorSpace::Srgb;
    /// Allocate and generate the full mip chain (box-filtered by the driver; sRGB textures are filtered in linear
    /// space); false keeps the base level only.
    bool mipmaps = true;
    SamplingDesc sampling;
};

/// Move-only RAII 2D texture with immutable storage (glTextureStorage2D, DSA), uploaded once from an 8-bit image:
/// RGBA8 texels in the TextureDesc color space, rows uploaded top row first and never flipped (ADR-0002: texture
/// coordinate t = 0 samples the image's top row). Its sampling parameters come from TextureDesc::sampling; a Sampler
/// bound to the same unit overrides them. A default-constructed or moved-from texture is empty. Main thread only.
class Texture2D {
public:
    Texture2D() noexcept = default;

    /// Uploads `image` (1 to 4 channels, expanded to RGBA with assets::convertChannels: grey replicated, missing alpha
    /// opaque) and generates its mipmaps. `debugName` labels the GL object. Errors: InvalidArgument for an invalid
    /// image, OutOfRange for a side above GL_MAX_TEXTURE_SIZE, GraphicsError when the storage cannot be allocated.
    [[nodiscard]] static Expected<Texture2D> create(const assets::ImageData& image, const TextureDesc& desc,
                                                    std::string_view debugName);

    [[nodiscard]] GLuint id() const noexcept { return m_texture.id(); }
    [[nodiscard]] bool valid() const noexcept { return m_texture.valid(); }
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_size; }
    [[nodiscard]] int mipLevels() const noexcept { return m_levels; }
    [[nodiscard]] const TextureDesc& desc() const noexcept { return m_desc; }
    /// Bytes of all allocated levels (4 per texel).
    [[nodiscard]] std::size_t estimatedBytes() const noexcept { return m_bytes; }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    TextureObject m_texture;
    glm::ivec2 m_size{0};
    int m_levels = 0;
    std::size_t m_bytes = 0;
    TextureDesc m_desc;
    std::string m_debugName;
};

} // namespace rv::gl
