#pragma once

// A console room's history plaque (I11 step 5, ADR-0019 §6): a small brass plate beside the hero corner with what the
// system's identity says of it, engraved: its logo (or its name without one), its maker, year and type on one line, and
// its description, wrapped and cut to the plate. GPU-free and deterministic; the app draws it on the worker pool and
// puts it on the plaque's picture at runtime, as the identity's signs are put (I11 step 4). The full description is
// what the plaque's card shows (room_plaque.rml).

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/assets/text_raster.hpp>

namespace rv::assets {

/// What a plaque says: the theme's text as it is (UTF-8), any of it possibly empty.
struct PlaqueText {
    std::string name;
    std::string maker;
    std::string year;
    std::string type;
    std::string description;

    bool operator==(const PlaqueText&) const = default;
};

/// The plate's brass and the engraving's colour (sRGB).
inline constexpr Rgb8 kPlaqueBrass{176, 140, 82};
inline constexpr Rgb8 kPlaqueEngraving{46, 32, 18};

/// Shares of the plate's height: its margin, the logo's (or the name's) line, the details' line, the description's
/// lines.
inline constexpr float kPlaqueMargin = 0.07F;
inline constexpr float kPlaqueTitle = 0.16F;
inline constexpr float kPlaqueDetails = 0.065F;
inline constexpr float kPlaqueLine = 0.058F;

/// The details' line: the maker, the year and the type that are not empty, joined by " · ".
[[nodiscard]] std::string plaqueDetails(const PlaqueText& text);

/// `text` broken into lines no wider than `width` pixels at `pixelHeight` in `font`, at the spaces (a word wider than a
/// line stands alone and is cut), at most `maxLines`: the last line that cannot hold the rest ends with an ellipsis.
/// Runs of spaces and line breaks count as one space.
[[nodiscard]] std::vector<std::u32string> wrapText(std::string_view text, float width, float pixelHeight,
                                                   const FontFace& font, std::size_t maxLines);

/// The plaque of `width` x `height` pixels (RGBA8 sRGB, opaque): brass with a fine engraved border; `logo` (RGBA8, straight
/// alpha; none or invalid: the name in `bold` instead) engraved as a silhouette, centred at the top; the details in
/// `regular` under it; a rule; the description in `regular`, wrapped to the plate's inner width and cut to its height.
/// An empty image for a size that is not positive.
[[nodiscard]] ImageData makeHistoryPlaque(int width, int height, const PlaqueText& text, const ImageData* logo,
                                          const FontFace& bold, const FontFace& regular);

} // namespace rv::assets
