#include <rv/render/texture.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

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
    // Texel rows are uploaded as tightly packed RGBA8: 4 bytes per texel, so every row is 4-byte aligned.
    assets::ImageData converted;
    const assets::ImageData* source = &image;
    if (image.channels != 4) {
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
    glTextureStorage2D(id, levels, internalFormat(desc.colorSpace), size.x, size.y);
    // Storage only becomes immutable when the allocation succeeded (GL_OUT_OF_MEMORY otherwise).
    GLint immutable = GL_FALSE;
    glGetTextureParameteriv(id, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    if (immutable == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not allocate a {}x{} texture with {} level(s)", size.x, size.y, levels),
                         name);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTextureSubImage2D(id, 0, 0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, source->pixels.data());
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
    result.m_bytes = textureBytes(desc.colorSpace == ColorSpace::Srgb ? GpuFormat::Srgb8Alpha8 : GpuFormat::Rgba8, size,
                                  levels);
    result.m_desc = desc;
    result.m_memory = GpuMemoryAllocation(desc.memoryKind, result.m_bytes);
    result.m_debugName = std::move(name);
    return result;
}

} // namespace rv::gl
