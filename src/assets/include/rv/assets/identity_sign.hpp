#pragma once

// A room's sign drawn from its system's identity (I11 step 4, ADR-0019 §3): the system's logo, or the logos of a shared
// room's sections, and its name in the bundled TrueType face, in place of the generic bitmap lettering of
// makeSignLettering() and makeNeonLettering(). GPU-free; the app draws these on the worker pool from the logos it has
// rasterized and puts them on the signs' materials at runtime, so that the scene files and the plans never hold a
// trade name.
//
// Two styles, as the generic signs have:
//   - Lit: the backlit plates of the halls (the portals, their blades, the hub's fascia): the logo in its own colours
//     (a dark logo as a white silhouette, else it would vanish on the plate) and the name in white, on black. Multiplied
//     by the plate's unlit material, as the generic lettering is.
//   - Neon: the room's neon (and a fascia that takes it): the logo's outline and the name's letters as the neon's tubes
//     (makeNeonTubes()), grey levels the neon's material colours.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/assets/text_raster.hpp>

namespace rv::assets {

enum class IdentitySignStyle : std::uint8_t { Lit, Neon };

/// The most logos a sign shows side by side (a shared room's sections; the others are left out).
inline constexpr std::size_t kMaxSignLogos = 4;

/// A box in a sign's pixels, from its top-left corner.
struct SignBox {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;

    bool operator==(const SignBox&) const = default;
};

/// Where a sign's parts go (layoutIdentitySign()).
struct IdentitySignLayout {
    /// Each logo's box, at its own aspect, in the order given (at most kMaxSignLogos).
    std::vector<SignBox> logos;
    /// The name's lines (one or two, none without a name), each laid out at `namePixelHeight`, and each line's box:
    /// its pen's start, its top (the baseline is that top plus the font's ascent), its advance and the pixel height.
    std::vector<std::u32string> nameLines;
    std::vector<SignBox> nameBoxes;
    float namePixelHeight = 0.0F;
    /// The name beside the logos rather than under them (a sign much wider than tall, such as the hub's fascia).
    bool sideBySide = false;
};

/// A sign this wide against its inner height (its margin taken off) puts the name beside the logos.
inline constexpr float kSideBySideAspect = 4.5F;

/// The layout of a sign of `width` x `height` pixels with logos of `logoSizes` (each a width and a height, the logos as
/// rasterized: only their aspect counts) and `name` (UTF-8, possibly empty), in `font`:
///   - a margin of a twelfth of the shorter side all round, as the generic signs have;
///   - the logos in one row, each at its aspect and the same height, a gap of a tenth of that height between them;
///   - the name on one line, or on two when breaking it at the space nearest its middle lets it be larger; never larger
///     than the space it has, never split inside a word;
///   - side by side (kSideBySideAspect): the logos' row at the inner height on the left (at most half the width), the
///     name in the rest, the whole group centred; else stacked: the logos in the upper 62%, the name in the lower 30%,
///     each centred; only logos or only a name take the whole inner box.
/// Logos with a size that is not positive are left out. Deterministic: the same arguments, the same layout.
[[nodiscard]] IdentitySignLayout layoutIdentitySign(int width, int height,
                                                    std::span<const std::pair<int, int>> logoSizes,
                                                    std::string_view name, const FontFace& font);

/// The sign of `width` x `height` (RGBA8 sRGB, opaque) in `style`, laid out by layoutIdentitySign(): `logos` (RGBA8 sRGB,
/// straight alpha, assets::loadLogo()'s) resampled into their boxes in linear light, `name` in `font`. An empty image
/// for a size that is not positive. The same arguments always produce the same pixels.
[[nodiscard]] ImageData makeIdentitySign(IdentitySignStyle style, int width, int height,
                                         std::span<const ImageData> logos, std::string_view name,
                                         const FontFace& font);

/// A logo too dark to read on the black plate (its mean luminance, weighted by its alpha, in linear light, below
/// kDarkLogoLuminance): drawn as a white silhouette instead.
inline constexpr float kDarkLogoLuminance = 0.06F;
[[nodiscard]] bool isDarkLogo(const ImageData& logo);

} // namespace rv::assets
