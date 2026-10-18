#pragma once

// The collection's layers and their .rvtex file (I3 step 5, ADR-0011 §4 and §7): a layer is one texture of a size class
// (a front label or a spine) with its full mip chain, in BC1 sRGB or, without the S3TC extensions, in RGBA8 sRGB at
// half the resolution; the .rvtex file holds one layer for the derived-texture cache: a fixed header (magic, version,
// class, format, size, mip count, dominant colour, the source's size and modification time, the cache key, checksums)
// and the levels, largest first.
//
// File layout, little-endian, 96 bytes of header then the payload:
//
//   offset  size  field
//        0     8  magic 89 52 56 54 58 0D 0A 1A ("\x89RVTX\r\n\x1A")
//        8     4  file version (kRvtexFileVersion)
//       12     4  pipeline version (kCoverPipelineVersion)
//       16     1  class (TextureClass)      17  1  format (TextureFormat)
//       18     1  mip count                 19  1  flags (bit 0: the source's stamp is set)
//       20     2  width                     22  2  height
//       24     3  dominant colour (sRGB R, G, B)   27  1  zero
//       28     4  payload bytes (the sum of the levels' sizes)
//       32     8  the source's size in bytes (0 without a source)
//       40     8  the source's last write time, FILETIME ticks (0 without a source)
//       48    32  the cache key's SHA-256 (the digest the entry is named after)
//       80     8  FNV-1a 64 of the payload
//       88     8  FNV-1a 64 of bytes 0 to 87
//       96     -  the levels, largest first, each bc1LevelBytes() (BC1) or width x height x 4 (RGBA8) bytes

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/sha256.hpp>

namespace rv::assets {

inline constexpr std::uint32_t kRvtexFileVersion = 1;
/// The version of the pipeline that makes the layers (fit rules, filters, encoder settings, text layout and drawing):
/// part of every cache key, so that a new version names new entries and never reads an old one. Raise it with any
/// change that alters a layer's pixels.
inline constexpr std::uint32_t kCoverPipelineVersion = 1;
inline constexpr std::size_t kRvtexHeaderBytes = 96;

/// The size classes of the collection's texture arrays (ADR-0011 §4).
enum class TextureClass : std::uint8_t {
    Front = 1, ///< a front label: the cover fitted, or a generated label (256 x 188, the cover's 1.368 aspect)
    Spine = 2, ///< a spine: always generated from the title (40 x 256, the 2.0 x 12.5 cm spine)
};

/// How a layer's texels are stored.
enum class TextureFormat : std::uint8_t {
    Bc1Srgb = 1,   ///< GL_COMPRESSED_SRGB_S3TC_DXT1_EXT at full resolution
    Rgba8Srgb = 2, ///< GL_SRGB8_ALPHA8 at half the resolution: the fallback without the S3TC extensions
};

[[nodiscard]] std::string_view toString(TextureClass textureClass) noexcept; ///< "front", "spine"
[[nodiscard]] std::string_view toString(TextureFormat format) noexcept;      ///< "bc1", "rgba8"

struct LayerSize {
    int width = 0;
    int height = 0;

    bool operator==(const LayerSize&) const = default;
};

/// A class's layer size in a format: front 256 x 188 and spine 40 x 256 in BC1, 128 x 94 and 20 x 128 in RGBA8.
[[nodiscard]] LayerSize layerSize(TextureClass textureClass, TextureFormat format) noexcept;

/// Bytes of one level: whole 4 x 4 blocks for BC1, 4 bytes per texel for RGBA8.
[[nodiscard]] std::size_t levelBytes(TextureFormat format, int width, int height) noexcept;

/// Bytes of a class's layer with its full mip chain in a format: 32,280 (front) and 6,984 (spine) in BC1.
[[nodiscard]] std::size_t layerBytes(TextureClass textureClass, TextureFormat format) noexcept;

/// What a cover's entry was made from: the source file's size and last write time (FILETIME ticks, 100 ns since
/// 1601-01-01 UTC) as its open handle reported them when it was read (core::SharedFileReader::facts()).
struct SourceStamp {
    std::uint64_t size = 0;
    std::int64_t lastWriteTime = 0;

    bool operator==(const SourceStamp&) const = default;
};

struct TextureLevel {
    int width = 0;
    int height = 0;
    std::vector<std::byte> data;
};

/// One layer: what the texture arrays upload (level by level) and what an .rvtex file holds.
struct RvTexture {
    TextureClass textureClass = TextureClass::Front;
    TextureFormat format = TextureFormat::Bc1Srgb;
    int width = 0;
    int height = 0;
    /// The picture's dominant colour (cover_fit.hpp: the most common colour of the fitted cover, which the spine's
    /// background starts from), or a generated layer's background.
    Rgb8 dominant;
    /// Set for a cover's layer, none for a generated one (spines and generated labels depend on their text only).
    std::optional<SourceStamp> source;
    std::vector<TextureLevel> levels; ///< the full chain, largest first

    [[nodiscard]] std::size_t payloadBytes() const noexcept;
};

/// The layer of an sRGB RGBA8 picture (4 channels) of exactly layerSize(textureClass, format): its mip chain in linear
/// light (mip_chain.hpp), each level BC1-encoded (bc1.hpp) or kept as RGBA8. Errors: InvalidArgument (not 4 channels,
/// another size).
[[nodiscard]] Expected<RvTexture> makeLayerTexture(const ImageData& picture, TextureClass textureClass,
                                                   TextureFormat format, Rgb8 dominant);

/// Level `level` decoded to RGBA8 (BC1 through decodeBc1()); empty when there is no such level.
[[nodiscard]] ImageData decodeLevel(const RvTexture& texture, std::size_t level);

/// FNV-1a 64 (the file's checksums).
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept;

/// The .rvtex bytes of `texture`, named by `key` (the cache key's digest). Errors: InvalidArgument (a texture that
/// does not match its class, format and chain).
[[nodiscard]] Expected<std::vector<std::byte>> serializeRvtex(const RvTexture& texture, const core::Sha256Digest& key);

/// Reads .rvtex bytes, checking everything: the magic, both versions (another pipeline version is refused: its entries
/// have other names), the class, format, size and mip count, the payload's length and both checksums, and `key` when
/// given. Errors: ValidationFailed, naming the check; `context` names the file.
[[nodiscard]] Expected<RvTexture> parseRvtex(std::span<const std::byte> bytes, const core::Sha256Digest* key,
                                             std::string_view context);

} // namespace rv::assets
