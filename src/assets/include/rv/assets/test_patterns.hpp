#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <glm/ext/vector_int2.hpp>
#include <glm/ext/vector_uint4_sized.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

// Deterministic procedural RGBA8 images (4 channels, rows top to bottom). Every design is original: abstract
// geometry and a built-in bitmap font, no logos or commercial artwork (style §17.2). Functions return an empty
// (invalid) ImageData when a side is not in [1, kMaxImageDimension].

/// 75% SMPTE-like color bars: seven bars (grey, yellow, cyan, green, magenta, red, blue at 75% intensity) on the
/// top two thirds, a reverse strip (blue, black, magenta, black, cyan, black, grey) down to 3/4 of the height,
/// and a bottom strip with a horizontal black-to-white grey ramp.
[[nodiscard]] ImageData makeColorBars(int width, int height);

/// `cells` x `cells` checkerboard (cells clamped to [1, min(width, height)]); the top-left cell uses `a`.
[[nodiscard]] ImageData makeCheckerboard(int width, int height, int cells, glm::u8vec4 a, glm::u8vec4 b);

/// Generic box-art placeholder for games without cover art (style §12.2 points 4-5, §21): dark border, an
/// abstract geometric pattern whose colors and layout are picked by `seed` from the style §3 palette, and a light
/// title band with `title` in a built-in 5x7 bitmap font (A-Z, 0-9, space and . , : ; ! ? - ' " & ( ) [ ] / + #).
/// The band spans 58% to 88% of the height inside the border; on wide covers (cartridge labels) it grows upwards, up
/// to 45% of that height, so that a two-line title keeps legible glyphs. Text is uppercased (Latin-1 accented letters
/// fold to their base letter, other characters become '?'), word-wrapped onto at most 3 lines, scaled by the largest
/// integer factor at which no word is split (a word is split only when no scale keeps it whole) and centered; text
/// that does not fit even at scale 1 is truncated with "...". The same arguments always produce the same pixels.
[[nodiscard]] ImageData makeCoverPlaceholder(int width, int height, std::string_view title, std::uint32_t seed);

/// A framed print of the halls (I6 step 2): makeCoverPlaceholder()'s design (the same layouts by `seed`, the title
/// band) in a muted palette: the walnut, oak, wall and graphite grounds, bordeaux, the signage green and the cold blue
/// at about 60% of their saturation and a softer tungsten, a dark border and lettering on an aged paper band.
[[nodiscard]] ImageData makePrintPlaceholder(int width, int height, std::string_view title, std::uint32_t seed);

/// Uniform color.
[[nodiscard]] ImageData makeSolid(int width, int height, glm::u8vec4 color);

/// Lettering of an illuminated room sign (style §17.1 room signage, with original themes such as "16-BIT CORNER" of
/// §17.2): `text` in the built-in bitmap font, white on black, on at most 2 lines, scaled by the largest integer factor
/// that leaves a margin of a twelfth of the shorter side and centered (folding, whole words and truncation as for
/// makeCoverPlaceholder). Multiplied by an unlit material's color, the letters glow in that color on a dark panel.
[[nodiscard]] ImageData makeSignLettering(int width, int height, std::string_view text);

/// Pattern names shared with the scene description files (textures.<role>.pattern).
inline constexpr std::string_view kPatternColorBars = "color_bars";
inline constexpr std::string_view kPatternChecker = "checker";
inline constexpr std::string_view kPatternCover = "cover";
inline constexpr std::string_view kPatternSign = "sign";
inline constexpr std::string_view kPatternPrint = "print";

/// The pattern names accepted by makeNamedPattern(), in a fixed order.
[[nodiscard]] std::span<const std::string_view> patternNames() noexcept;

/// Builds a pattern by name: "color_bars"; "checker" (8 x 8 cells, ivory #F4EFE6 and wall grey #383735);
/// "cover" (uses `title` and `seed`); "sign" (uses `title` as the lettering); "print" (makePrintPlaceholder(), uses
/// `title` and `seed`). Errors: NotFound for an unknown name,
/// InvalidArgument for a side outside [1, kMaxImageDimension].
[[nodiscard]] Expected<ImageData> makeNamedPattern(std::string_view name, int width, int height,
                                                   std::string_view title = {}, std::uint32_t seed = 0);

/// Largest longer side, in pixels, of the pattern images generated for scene surfaces (cartridge labels, posters,
/// screens).
inline constexpr int kPatternLongSide = 512;
/// Pixels per meter of surface pattern images are generated at, up to kPatternLongSide on their longer side (about
/// 0.4 mm per pixel): a 10 cm cartridge label gets about 260 pixels, a poster or a screen the full kPatternLongSide.
inline constexpr float kPatternPixelsPerMeter = 2560.0f;
/// Smallest longer side of a pattern image, so that tiny surfaces still get a readable picture.
inline constexpr int kMinPatternLongSide = 64;

/// Longer side, in pixels, of the pattern image of a surface whose longer side is `meters` long: kPatternPixelsPerMeter
/// per meter, rounded to the nearest pixel, clamped to [kMinPatternLongSide, kPatternLongSide] (kPatternLongSide for a
/// non-finite or non-positive length).
[[nodiscard]] int patternLongSideFor(float meters) noexcept;

/// Pixel size of a pattern image shown on a surface whose width / height is `aspect` (for instance the ratio of
/// rv::geometry::uvMetersPerUnit), so that the pattern is not stretched: the longer side gets `longSide` pixels
/// (clamped to [1, kMaxImageDimension]) and the other one the rounded proportional count, at least 1. A non-finite
/// or non-positive aspect gives a square.
[[nodiscard]] glm::ivec2 patternSizeForAspect(float aspect, int longSide = kPatternLongSide) noexcept;

/// Identity of a generated pattern, "pattern:<name>:<title>:<seed>" (the title verbatim, may be empty), used to key
/// pattern textures in caches and logs.
[[nodiscard]] std::string patternKey(std::string_view name, std::string_view title, std::uint32_t seed);

} // namespace rv::assets
