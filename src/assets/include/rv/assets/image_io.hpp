#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// Largest accepted width or height in pixels. Larger images are rejected before any pixel is decoded and are
/// never produced by the encoders, so every image Retroverse writes can be read back.
inline constexpr int kMaxImageDimension = 16384;

/// Largest image file read by loadImage() (untrusted inputs, specification §19.1).
inline constexpr std::uintmax_t kMaxImageFileBytes = 256ull * 1024ull * 1024ull;

/// Decodes a PNG or JPEG image held in memory (the only formats accepted, to keep the attack surface small).
///
/// Rows are stored top to bottom exactly as encoded: images are never flipped on load (ADR-0002), independently
/// of any stb_image global state. `desiredChannels` = 0 keeps the channel count of the file (1 grey,
/// 2 grey + alpha, 3 RGB, 4 RGBA; palettes are expanded, 16-bit samples reduced to 8 bits); 1..4 converts.
/// `context` names the source in error messages (file path, "image 2 of crt.glb", ...).
///
/// Errors: InvalidArgument (empty input, desiredChannels outside 0..4), Unsupported (neither PNG nor JPEG),
/// OutOfRange (a side larger than kMaxImageDimension, input larger than 2 GiB), ParseError (corrupt or
/// truncated data).
[[nodiscard]] Expected<ImageData> decodeImage(std::span<const std::byte> bytes, int desiredChannels,
                                              std::string_view context);

/// Reads a PNG or JPEG file (Unicode paths supported, size limited to kMaxImageFileBytes) and decodes it
/// like decodeImage(). Errors carry the UTF-8 path as context.
[[nodiscard]] Expected<ImageData> loadImage(const std::filesystem::path& path, int desiredChannels = 0);

/// Encodes a valid image (ImageData::isValid(), sides up to kMaxImageDimension) as PNG. The first row of
/// `image` becomes the top row of the file. Output is deterministic for identical input.
[[nodiscard]] Expected<std::vector<std::byte>> encodePng(const ImageData& image);

/// Encodes a valid image as baseline JPEG with `quality` in [1, 100]. Alpha channels are discarded
/// (2 channels encode as grey, 4 as RGB).
[[nodiscard]] Expected<std::vector<std::byte>> encodeJpeg(const ImageData& image, int quality = 90);

/// Encodes `image` as PNG and writes it atomically (temporary file + rename, parent directories created).
[[nodiscard]] Status writePng(const std::filesystem::path& path, const ImageData& image);

/// Reverses the row order in place (top row becomes bottom row). Used once for glReadPixels screenshots,
/// whose first row is the bottom of the framebuffer. Invalid images are left untouched.
void flipRowsInPlace(ImageData& image);

/// Converts to `channels` (1..4) components per pixel. Grey expands to RGB by replication; colour reduces to
/// grey with integer Rec. 601 luma weights (77, 150, 29) / 256; missing alpha becomes 255; dropped alpha is
/// discarded. Returns a copy when the channel count already matches, and an empty (invalid) image when
/// `image` is invalid or `channels` is outside 1..4.
[[nodiscard]] ImageData convertChannels(const ImageData& image, int channels);

} // namespace rv::assets
