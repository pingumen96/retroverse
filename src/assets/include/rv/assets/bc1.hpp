#pragma once

// BC1 (S3TC DXT1) encoding with stb_dxt (I3 step 5, ADR-0011 §4): the collection's layers are opaque, so the arrays
// hold BC1 sRGB (GL_COMPRESSED_SRGB_S3TC_DXT1_EXT), 8 bytes per 4 x 4 block. Levels whose sides are not multiples of 4
// (94, 47, 5, ...) are encoded as whole blocks with their edge pixels repeated, and uploaded whole, as S3TC requires.
// The encoder takes the sRGB codes as they are (BC1 sRGB interpolates the decoded codes); decodeBc1() is the reference
// decoder the tests and the measurements compare against (the S3TC rules, stb_dxt's interpolation).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rv/assets/image_data.hpp>

namespace rv::assets {

inline constexpr std::size_t kBc1BlockBytes = 8;

/// Blocks along a side: ceil(side / 4), 0 for a side below 1.
[[nodiscard]] int bc1BlocksAlong(int side) noexcept;

/// Bytes of a width x height level in BC1: whole blocks, 8 bytes each (0 for a side below 1).
[[nodiscard]] std::size_t bc1LevelBytes(int width, int height) noexcept;

/// How hard stb_dxt works on each block.
enum class Bc1Quality {
    Normal, ///< one refinement step
    High,   ///< STB_DXT_HIGHQUAL: two refinement steps (the default: about 30-40% slower, better endpoints)
};

/// Encodes an RGBA8 image (4 channels; alpha ignored, the layers are opaque) as BC1 blocks in row-major block order,
/// each block's missing pixels (past the right or bottom edge) repeating the last column or row. Deterministic. Empty
/// for an invalid image or one without 4 channels.
[[nodiscard]] std::vector<std::byte> encodeBc1(const ImageData& image, Bc1Quality quality = Bc1Quality::High);

/// I11 step 7: BC3 (S3TC DXT5), 16 bytes per 4 x 4 block: BC1's colour block after an alpha block (two endpoints and
/// 3-bit indices), for the user's console models' base colours with transparency.
inline constexpr std::size_t kBc3BlockBytes = 16;
/// Bytes of a width x height level in BC3: whole blocks, 16 bytes each (0 for a side below 1).
[[nodiscard]] std::size_t bc3LevelBytes(int width, int height) noexcept;
/// Encodes an RGBA8 image (4 channels) as BC3 blocks with its alpha, as encodeBc1() lays them out. Deterministic.
/// Empty for an invalid image or one without 4 channels.
[[nodiscard]] std::vector<std::byte> encodeBc3(const ImageData& image, Bc1Quality quality = Bc1Quality::High);
/// The alpha of every pixel of an RGBA8 image is 255 (BC1 keeps it), false for any other image.
[[nodiscard]] bool isOpaque(const ImageData& image) noexcept;

/// Decodes BC1 blocks into a width x height RGBA8 image (alpha 255, or 0 for the transparent index of a three-colour
/// block): the endpoints expanded from 5:6:5 by bit replication, the four-colour palette (c0 > c1) interpolated at a
/// third and two thirds, the three-colour one (c0 <= c1) at the middle with index 3 black. Empty when `blocks` is not
/// exactly bc1LevelBytes(width, height).
[[nodiscard]] ImageData decodeBc1(std::span<const std::byte> blocks, int width, int height);

/// Peak signal-to-noise ratio in dB of `actual` against `expected` over the RGB channels of two images of one size
/// (8-bit codes compared as they are, in sRGB). Infinity when identical; 0 when they cannot be compared.
[[nodiscard]] double psnrRgb(const ImageData& expected, const ImageData& actual);

} // namespace rv::assets
