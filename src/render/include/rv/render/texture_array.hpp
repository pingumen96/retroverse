#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glad/gl.h>
#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>
#include <rv/render/gpu_memory.hpp>
#include <rv/render/texture_array_layout.hpp>

namespace rv::gl {

/// Move-only RAII GL_TEXTURE_2D_ARRAY with immutable storage (glTextureStorage3D, DSA) for the collection's layers
/// (ADR-0011 §4): every layer of one size with a full mip chain, in BC1 sRGB (GL_COMPRESSED_SRGB_S3TC_DXT1_EXT) or
/// RGBA8 sRGB (GL_SRGB8_ALPHA8). Layers are uploaded level by level from their prepared mip chains
/// (glCompressedTextureSubImage3D or glTextureSubImage3D, whole levels, as S3TC requires of levels whose sides are not
/// multiples of 4); the driver never generates mips for it. An array that must hold more layers is replaced by a
/// larger one into which the GPU copies the old one's layers (grow(): glCopyImageSubData, every level). Its bytes are
/// recorded in gpuMemoryLedger() under GpuMemoryKind::CollectionArrays (textureBytes of its format, size, levels and
/// layers) while it lives; a growth records the new array before it releases the old. Trilinear filtering, clamped to
/// the edges, as texture parameters (a sampler bound to the unit overrides them). Main thread only.
class Texture2DArray {
public:
    Texture2DArray() noexcept = default;

    /// An array of `layers` layers of `size` texels with `levels` levels (1 to the full chain) in `format`, its
    /// contents undefined until uploaded. Errors: InvalidArgument for an empty size, a level count outside [1, full
    /// chain] or a layer count outside [1, GL_MAX_ARRAY_TEXTURE_LAYERS]; OutOfRange for a side above
    /// GL_MAX_TEXTURE_SIZE; GraphicsError when the storage cannot be allocated.
    [[nodiscard]] static Expected<Texture2DArray> create(CollectionArrayFormat format, glm::ivec2 size, int levels,
                                                         int layers, std::string_view debugName,
                                                         GpuMemoryKind memoryKind = GpuMemoryKind::CollectionArrays);

    /// Uploads every level of one layer: `levels[i]` holds level i whole, textureLevelBytes(format, its size) bytes
    /// (BC1 blocks, or RGBA8 texels rows top first). Errors: InvalidArgument for a layer out of range, a level count
    /// other than levelCount() or a level of another size; nothing is uploaded then.
    [[nodiscard]] Status uploadLayer(int layer, std::span<const std::vector<std::byte>> levels);

    /// Replaces the storage with an array of `layers` layers (more than now, at most GL_MAX_ARRAY_TEXTURE_LAYERS) of
    /// the same format, size and levels, the GPU copying every current layer (all levels) into it before the old
    /// storage is deleted; the layers beyond are undefined until uploaded. The object keeps its address, so whatever
    /// refers to it (materials) samples the new storage; id() changes. Errors: InvalidArgument for an empty array or a
    /// count not above layerCount(), and those of create(); the array is unchanged then.
    [[nodiscard]] Status grow(int layers);

    /// Copies layer `source` into layers [first, first + count) on the GPU (every level, by doubling runs of
    /// glCopyImageSubData), so that they show its picture and the driver commits their storage now rather than at a
    /// later upload. Errors: InvalidArgument for an empty array, or a source or range out of range or overlapping the
    /// source.
    [[nodiscard]] Status fillLayers(int source, int first, int count);

    [[nodiscard]] GLuint id() const noexcept { return m_texture.id(); }
    [[nodiscard]] bool valid() const noexcept { return m_texture.valid(); }
    [[nodiscard]] CollectionArrayFormat format() const noexcept { return m_format; }
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_size; }
    [[nodiscard]] int levelCount() const noexcept { return m_levels; }
    [[nodiscard]] int layerCount() const noexcept { return m_layers; }
    /// textureBytes(gpuFormatOf(format()), size(), levelCount(), layerCount()): what the ledger records.
    [[nodiscard]] std::size_t estimatedBytes() const noexcept { return m_bytes; }
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    TextureObject m_texture;
    CollectionArrayFormat m_format = CollectionArrayFormat::Bc1Srgb;
    glm::ivec2 m_size{0};
    int m_levels = 0;
    int m_layers = 0;
    std::size_t m_bytes = 0;
    GpuMemoryKind m_memoryKind = GpuMemoryKind::CollectionArrays;
    std::string m_debugName;
    GpuMemoryAllocation m_memory;
};

/// The OpenGL internal format of `format`: GL_COMPRESSED_SRGB_S3TC_DXT1_EXT or GL_SRGB8_ALPHA8.
[[nodiscard]] GLenum internalFormatOf(CollectionArrayFormat format) noexcept;

} // namespace rv::gl
