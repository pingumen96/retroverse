#pragma once

#include <cstdint>
#include <span>
#include <string_view>

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
/// Text is uppercased (Latin-1 accented letters fold to their base letter, other characters become '?'),
/// word-wrapped onto at most 3 lines, scaled by the largest integer factor that fits and centered; text that does
/// not fit even at scale 1 is truncated with "...". The same arguments always produce the same pixels.
[[nodiscard]] ImageData makeCoverPlaceholder(int width, int height, std::string_view title, std::uint32_t seed);

/// Uniform color.
[[nodiscard]] ImageData makeSolid(int width, int height, glm::u8vec4 color);

/// Pattern names shared with the scene description files (textures.<role>.pattern).
inline constexpr std::string_view kPatternColorBars = "color_bars";
inline constexpr std::string_view kPatternChecker = "checker";
inline constexpr std::string_view kPatternCover = "cover";

/// The pattern names accepted by makeNamedPattern(), in a fixed order.
[[nodiscard]] std::span<const std::string_view> patternNames() noexcept;

/// Builds a pattern by name: "color_bars"; "checker" (8 x 8 cells, ivory #F4EFE6 and wall grey #383735);
/// "cover" (uses `title` and `seed`). Errors: NotFound for an unknown name, InvalidArgument for a side outside
/// [1, kMaxImageDimension].
[[nodiscard]] Expected<ImageData> makeNamedPattern(std::string_view name, int width, int height,
                                                   std::string_view title = {}, std::uint32_t seed = 0);

} // namespace rv::assets
