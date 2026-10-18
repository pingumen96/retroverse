#include <rv/render/texture_array.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include <rv/render/texture_memory.hpp>

#include "gl_object_label.hpp"

namespace rv::gl {

GLenum internalFormatOf(CollectionArrayFormat format) noexcept {
    return format == CollectionArrayFormat::Bc1Srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_SRGB8_ALPHA8;
}

Expected<Texture2DArray> Texture2DArray::create(CollectionArrayFormat format, glm::ivec2 size, int levels, int layers,
                                                std::string_view debugName, GpuMemoryKind memoryKind) {
    std::string name(debugName);
    if (size.x <= 0 || size.y <= 0) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("empty size {}x{}", size.x, size.y), name);
    }
    if (levels < 1 || levels > fullMipLevelCount(size)) {
        return makeError(
            ErrorCode::InvalidArgument,
            fmt::format("{} level(s) for {}x{} (1 to {})", levels, size.x, size.y, fullMipLevelCount(size)), name);
    }
    GLint maxLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
    if (layers < 1 || layers > maxLayers) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("{} layer(s) (1 to {})", layers, maxLayers), name);
    }
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (size.x > maxSize || size.y > maxSize) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("{}x{} exceeds the largest texture side, {}", size.x, size.y, maxSize), name);
    }
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &id);
    TextureObject texture(id);
    if (id == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateTextures returned no texture", name);
    }
    glTextureStorage3D(id, levels, internalFormatOf(format), size.x, size.y, layers);
    // Storage only becomes immutable when the allocation succeeded (GL_OUT_OF_MEMORY otherwise).
    GLint immutable = GL_FALSE;
    glGetTextureParameteriv(id, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    if (immutable == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not allocate a {} array of {} layer(s) of {}x{} with {} level(s)",
                                     toString(format), layers, size.x, size.y, levels),
                         name);
    }
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    detail::setObjectLabel(GL_TEXTURE, id, name);

    Texture2DArray result;
    result.m_texture = std::move(texture);
    result.m_format = format;
    result.m_size = size;
    result.m_levels = levels;
    result.m_layers = layers;
    result.m_bytes = textureBytes(gpuFormatOf(format), size, levels, layers);
    result.m_memoryKind = memoryKind;
    result.m_memory = GpuMemoryAllocation(memoryKind, result.m_bytes);
    result.m_debugName = std::move(name);
    return result;
}

Status Texture2DArray::uploadLayer(int layer, std::span<const std::vector<std::byte>> levels) {
    if (!valid()) {
        return makeError(ErrorCode::InvalidArgument, "the array is empty", m_debugName);
    }
    if (layer < 0 || layer >= m_layers) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("layer {} of {}", layer, m_layers), m_debugName);
    }
    if (static_cast<int>(levels.size()) != m_levels) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("{} level(s) instead of {}", levels.size(), m_levels),
                         m_debugName);
    }
    const GpuFormat gpuFormat = gpuFormatOf(m_format);
    for (int level = 0; level < m_levels; ++level) {
        const glm::ivec2 size = mipLevelSize(m_size, level);
        const std::size_t expected = textureLevelBytes(gpuFormat, size);
        if (levels[static_cast<std::size_t>(level)].size() != expected) {
            return makeError(ErrorCode::InvalidArgument,
                             fmt::format("level {} ({}x{}) holds {} bytes instead of {}", level, size.x, size.y,
                                         levels[static_cast<std::size_t>(level)].size(), expected),
                             m_debugName);
        }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (m_format == CollectionArrayFormat::Rgba8Srgb) {
        // Tightly packed RGBA8 rows are 4-byte aligned.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    for (int level = 0; level < m_levels; ++level) {
        const glm::ivec2 size = mipLevelSize(m_size, level);
        const std::vector<std::byte>& bytes = levels[static_cast<std::size_t>(level)];
        if (m_format == CollectionArrayFormat::Bc1Srgb) {
            // Whole levels: a level whose sides are not multiples of 4 is only accepted as a whole (S3TC).
            glCompressedTextureSubImage3D(m_texture.id(), level, 0, 0, layer, size.x, size.y, 1,
                                          internalFormatOf(m_format), static_cast<GLsizei>(bytes.size()), bytes.data());
        } else {
            glTextureSubImage3D(m_texture.id(), level, 0, 0, layer, size.x, size.y, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                bytes.data());
        }
    }
    return ok();
}

Status Texture2DArray::grow(int layers) {
    if (!valid()) {
        return makeError(ErrorCode::InvalidArgument, "the array is empty", m_debugName);
    }
    if (layers <= m_layers) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("{} layer(s) is not more than the {} it has", layers, m_layers), m_debugName);
    }
    auto grown = create(m_format, m_size, m_levels, layers, m_debugName, m_memoryKind);
    if (!grown) {
        return propagate(grown.error());
    }
    for (int level = 0; level < m_levels; ++level) {
        const glm::ivec2 size = mipLevelSize(m_size, level);
        // Whole levels of every current layer (compressed levels need whole blocks or the level's edge: both hold).
        glCopyImageSubData(m_texture.id(), GL_TEXTURE_2D_ARRAY, level, 0, 0, 0, grown->id(), GL_TEXTURE_2D_ARRAY, level,
                           0, 0, 0, size.x, size.y, m_layers);
    }
    // The new storage was recorded before the old one is released here: the ledger's peak holds both.
    *this = std::move(*grown);
    return ok();
}

Status Texture2DArray::fillLayers(int source, int first, int count) {
    if (!valid()) {
        return makeError(ErrorCode::InvalidArgument, "the array is empty", m_debugName);
    }
    if (source < 0 || source >= m_layers || first < 0 || count < 0 || first + count > m_layers ||
        (source >= first && source < first + count)) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("layer {} into layers [{}, {}) of {}", source, first, first + count, m_layers),
                         m_debugName);
    }
    const auto copy = [this](int from, int to, int layers) {
        for (int level = 0; level < m_levels; ++level) {
            const glm::ivec2 size = mipLevelSize(m_size, level);
            glCopyImageSubData(m_texture.id(), GL_TEXTURE_2D_ARRAY, level, 0, 0, from, m_texture.id(),
                               GL_TEXTURE_2D_ARRAY, level, 0, 0, to, size.x, size.y, layers);
        }
    };
    if (count == 0) {
        return ok();
    }
    // One copy of the source, then runs that double: [first, first + done) into the layers after it, never
    // overlapping.
    copy(source, first, 1);
    int done = 1;
    while (done < count) {
        const int run = std::min(done, count - done);
        copy(first, first + done, run);
        done += run;
    }
    return ok();
}

} // namespace rv::gl
