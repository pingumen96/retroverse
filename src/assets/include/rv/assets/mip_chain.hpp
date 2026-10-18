#pragma once

// sRGB colours and mip chains computed in linear light (I3 step 5, ADR-0011 §6): the collection's layers (covers,
// spines, generated labels) are sRGB images whose mip levels are box-filtered on linear values and encoded back to
// sRGB, so that a black and white checker reduces to the grey that reads as half the light (sRGB 188), not to 128.

#include <cstdint>
#include <string>
#include <vector>

#include <rv/assets/image_data.hpp>

namespace rv::assets {

/// An sRGB-encoded colour, 8 bits per channel (style §3's hex codes are these).
struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const Rgb8&) const = default;
};

/// A colour in linear light, each channel in [0, 1].
struct LinearRgb {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
};

/// The linear value of an 8-bit sRGB code (IEC 61966-2-1), from a table: exact and identical on every call.
[[nodiscard]] float srgbByteToLinear(std::uint8_t code) noexcept;

/// The 8-bit sRGB code nearest to `linear` in the encoded domain: round(encode(linear) * 255), found against the 255
/// thresholds between neighbouring codes, so that srgbByteToLinear() and this round-trip every code exactly. Values
/// below 0 give 0, above 1 give 255, NaN gives 0.
[[nodiscard]] std::uint8_t linearToSrgbByte(float linear) noexcept;

[[nodiscard]] LinearRgb toLinear(Rgb8 colour) noexcept;
[[nodiscard]] Rgb8 toSrgb8(LinearRgb colour) noexcept;

/// "#RRGGBB" in uppercase hexadecimal.
[[nodiscard]] std::string toHexCode(Rgb8 colour);

/// The number of levels of a full chain from width x height down to 1 x 1, each side halved and rounded down (as
/// OpenGL sizes the levels of a texture, at least 1): 9 for 256 x 188 and for 40 x 256. 0 for a side below 1.
[[nodiscard]] int mipLevelCount(int width, int height) noexcept;

/// The size of level `level` of a width x height image: max(1, side >> level).
[[nodiscard]] int mipLevelSide(int side, int level) noexcept;

/// The full mip chain of an sRGB RGBA8 image (4 channels; level 0 is a copy of `image`): each level is the previous
/// one reduced by an exact box filter on linear values (every source pixel weighted by the share of it a destination
/// pixel covers, so odd sides such as 47 -> 23 lose nothing), RGB decoded from sRGB before filtering and encoded after,
/// alpha filtered as it is (covers and labels are opaque). Deterministic: the same bytes on every build. Empty for an
/// invalid image or one without 4 channels.
[[nodiscard]] std::vector<ImageData> buildMipChain(const ImageData& image);

/// One level reduced to width x height (each at least 1, at most the source's) by the same box filter in linear light;
/// also the reduction of text drawn at twice the size (text_raster). Empty when the arguments do not fit.
[[nodiscard]] ImageData reduceLinear(const ImageData& image, int width, int height);

} // namespace rv::assets
