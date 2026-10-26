#include <rv/render/texture.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rv/assets/image_io.hpp>

#include "gl_object_label.hpp"
#include "sampling_params.hpp"

namespace rv::gl {
namespace {

GLenum internalFormat(ColorSpace colorSpace) noexcept {
    return colorSpace == ColorSpace::Srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
}

} // namespace

std::string_view toString(ColorSpace colorSpace) noexcept {
    return colorSpace == ColorSpace::Srgb ? "srgb" : "linear";
}

std::string_view toString(TextureWrap wrap) noexcept {
    switch (wrap) {
    case TextureWrap::Repeat:
        return "repeat";
    case TextureWrap::ClampToEdge:
        return "clamp_to_edge";
    case TextureWrap::MirroredRepeat:
        return "mirrored_repeat";
    }
    return "repeat";
}

float clampAnisotropy(float requested, float maxSupported) noexcept {
    if (!std::isfinite(requested) || !std::isfinite(maxSupported)) {
        return 1.0f;
    }
    return std::clamp(requested, 1.0f, std::max(maxSupported, 1.0f));
}

TextureObject createRgba8View(GLuint texture, int levelCount) {
    GLuint viewId = 0;
    glGenTextures(1, &viewId); // glTextureView needs a name that has never been bound: not glCreateTextures
    TextureObject view(viewId);
    if (viewId == 0 || texture == 0 || levelCount < 1) {
        return {};
    }
    glTextureView(viewId, GL_TEXTURE_2D, texture, GL_RGBA8, 0, static_cast<GLuint>(levelCount), 0, 1);
    glTextureParameteri(viewId, GL_TEXTURE_MIN_FILTER, levelCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(viewId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return view;
}

Expected<Texture2D> Texture2D::create(const assets::ImageData& image, const TextureDesc& desc,
                                      std::string_view debugName) {
    std::string name(debugName);
    if (!image.isValid()) {
        return makeError(ErrorCode::InvalidArgument, "the image is empty or inconsistent", name);
    }
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (image.width > maxSize || image.height > maxSize) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("{}x{} exceeds the largest texture side, {}", image.width, image.height, maxSize),
                         name);
    }
    // Texel rows are uploaded as tightly packed RGBA8: 4 bytes per texel, so every row is 4-byte aligned. A
    // metallic-roughness map keeps its G and B alone (R8 or RG8, rows packed byte by byte).
    const GpuFormat format = desc.metallicRoughness && desc.colorSpace == ColorSpace::Linear
                                 ? metallicRoughnessFormat(image)
                                 : (desc.colorSpace == ColorSpace::Srgb ? GpuFormat::Srgb8Alpha8 : GpuFormat::Rgba8);
    assets::ImageData converted;
    const assets::ImageData* source = &image;
    std::vector<std::uint8_t> packed;
    if (format == GpuFormat::R8 || format == GpuFormat::Rg8) {
        const std::size_t channels = static_cast<std::size_t>(image.channels);
        const std::size_t kept = format == GpuFormat::R8 ? 1 : 2;
        const std::size_t texels = image.pixels.size() / channels;
        packed.resize(texels * kept);
        for (std::size_t texel = 0; texel < texels; ++texel) {
            packed[texel * kept] = image.pixels[texel * channels + 1]; // roughness
            if (kept == 2) {
                packed[texel * kept + 1] = image.pixels[texel * channels + 2]; // metallic
            }
        }
    } else if (image.channels != 4) {
        converted = assets::convertChannels(image, 4);
        source = &converted;
    }

    const glm::ivec2 size(image.width, image.height);
    const int levels = desc.mipmaps ? fullMipLevelCount(size) : 1;
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    TextureObject texture(id);
    if (id == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateTextures returned no texture", name);
    }
    const GLenum storage = format == GpuFormat::R8    ? GL_R8
                           : format == GpuFormat::Rg8 ? GL_RG8
                                                      : internalFormat(desc.colorSpace);
    glTextureStorage2D(id, levels, storage, size.x, size.y);
    // Storage only becomes immutable when the allocation succeeded (GL_OUT_OF_MEMORY otherwise).
    GLint immutable = GL_FALSE;
    glGetTextureParameteriv(id, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    if (immutable == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not allocate a {}x{} texture with {} level(s)", size.x, size.y, levels),
                         name);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (!packed.empty()) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTextureSubImage2D(id, 0, 0, 0, size.x, size.y, format == GpuFormat::R8 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE,
                            packed.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        // Sampled as the RGBA8 map would be: R and A 1, G the roughness, B the metallic (its one value, 0 or 1, when
        // none is stored).
        const GLint metallic = format == GpuFormat::Rg8 ? GL_GREEN : (image.pixels[2] == 0 ? GL_ZERO : GL_ONE);
        const GLint swizzle[4] = {GL_ONE, GL_RED, metallic, GL_ONE};
        glTextureParameteriv(id, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    } else {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTextureSubImage2D(id, 0, 0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, source->pixels.data());
    }
    if (levels > 1) {
        glGenerateTextureMipmap(id);
    }

    const GLint wrap = detail::wrapMode(desc.sampling.wrap);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, wrap);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, wrap);
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, detail::minFilter(desc.sampling.filter, levels > 1));
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, detail::magFilter(desc.sampling.filter));
    if (detail::usesAnisotropy(desc.sampling)) {
        glTextureParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, desc.sampling.anisotropy);
    }
    detail::setObjectLabel(GL_TEXTURE, id, name);

    Texture2D result;
    result.m_texture = std::move(texture);
    result.m_size = size;
    result.m_levels = levels;
    result.m_bytes = textureBytes(format, size, levels);
    result.m_desc = desc;
    result.m_memory = GpuMemoryAllocation(desc.memoryKind, result.m_bytes);
    result.m_debugName = std::move(name);
    return result;
}

} // namespace rv::gl
