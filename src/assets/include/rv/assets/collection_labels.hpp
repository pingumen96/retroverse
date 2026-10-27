#pragma once

// Generated front labels and spines (I3 step 5, ADR-0011 §5, style §12.2 and §21): the spine is always generated from
// the title, the front label when the game has no usable cover. Text is set in the bundled Atkinson Hyperlegible
// (text_raster.hpp), on the cover's dominant colour or a colour of the theme's label palette chosen from the
// placement's seed, darkened or lightened until the text's contrast is at least kLabelContrastTarget.
//
// Spine (40 x 256 in BC1, 20 x 128 in RGBA8): the title reads top to bottom (the convention of the US and UK spines):
// the line runs down the layer from its top row, the glyphs' tops towards the layer's right edge (+x), i.e. the text
// is set horizontally and turned 90 degrees clockwise. One line, or two when it is long (split at a separator first,
// " : ", ": " or " - ", else between words, the split that lets the larger size fit), centred both ways; an ellipsis
// when it still does not fit at the minimum size. All sizes are shares of the spine's thickness T (the layer's width):
// one line from 0.62 T down to 0.45 T, two lines from 0.45 T down to 0.34 T (the minimum), stb_truetype's pixel height
// (ascent minus descent). At 1 m a 2.0 cm spine covers about 25 px at 1920 x 1080, so the minimum size is about 8.5 px.
// A printed spine (I6 step 4's review: SpineRequest::spine, the full-height spine of a case or box standing on its
// side) is laid out the same way on a canvas of its quad's shape (labelCanvas(): a jewel case's 7.1 x 123.4 mm spine
// on 40 x 696 at BC1), T the canvas's width and the line's room its length less a darker band of the spine's colour
// across its top 12%, then resampled once into the layer, which the quad stretches back to the spine's shape.
//
// Generated front label (256 x 188 in BC1, 128 x 94 in RGBA8): a frame inset from the edge and two stripes across the
// top in tints of the text colour, the title centred in the space between them, the system's name at the bottom left
// and the year at the bottom right (for a label quad of another shape, laid out at that shape and resampled into the
// layer: FrontLabelRequest::label). Readable, never an imitation of a real layout. The title (sizes in pixels at
// 256 px wide, scaled with the layer): one line from 34 down to 20 px; else two or three lines from 26 down to 14 px,
// a separator forcing the first break and each part broken between words where its widest line is narrowest, the
// largest size winning unless fewer lines reach 85% of it; else three lines at 14 px, the last one cut with an
// ellipsis.
//
// Every function is deterministic (the same pixels on every build) and safe on several threads with shared fonts.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_data.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/rvtex.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// WCAG 2's minimum contrast for text (ADR-0011 §5), checked after encoding by the tests.
inline constexpr double kMinTextContrast = 4.5;
/// The contrast the colours are adjusted to, with a margin for BC1's 5:6:5 endpoints.
inline constexpr double kLabelContrastTarget = 4.8;

/// The two text colours (style §3: "UI testo" ivory and "UI neutra" charcoal).
inline constexpr Rgb8 kLabelTextLight{0xF4, 0xEF, 0xE6};
inline constexpr Rgb8 kLabelTextDark{0x17, 0x1B, 0x22};

/// The default label palette: style §3's roles (bordeaux, electric blue, sign green, arcade magenta, oak, graphite,
/// tungsten amber, ivory). Since I6 step 3 a room's theme pack gives its own (rv/world/room_look.hpp's labelPalette;
/// classic_16bit_warm's is this one), carried by the requests below; this one stands for a request without any.
inline constexpr std::array<Rgb8, 8> kDefaultLabelPalette = {{
    {0x68, 0x3D, 0x42},
    {0x4C, 0x8B, 0xC9},
    {0x6E, 0xAF, 0x93},
    {0xB3, 0x4D, 0xCE},
    {0x76, 0x51, 0x3A},
    {0x25, 0x2A, 0x2E},
    {0xFF, 0xC8, 0x8A},
    {0xF4, 0xEF, 0xE6},
}};

/// WCAG 2 relative luminance of an sRGB colour, and the contrast ratio (L1 + 0.05) / (L2 + 0.05) of two (>= 1).
[[nodiscard]] double relativeLuminance(Rgb8 colour) noexcept;
[[nodiscard]] double contrastRatio(Rgb8 a, Rgb8 b) noexcept;

struct LabelColours {
    Rgb8 background;
    Rgb8 text;
    double contrast = 0.0;
};

/// The text colour that contrasts more with `base` (light or dark), and `base` mixed in linear light towards black
/// (light text) or white (dark text) in steps of 2% until the contrast reaches kLabelContrastTarget.
[[nodiscard]] LabelColours labelColours(Rgb8 base) noexcept;

/// The palette's colour for a placement's seed (core::Random(seed).below(size)); the first for an empty palette.
[[nodiscard]] Rgb8 paletteColour(std::uint64_t seed, std::span<const Rgb8> palette = kDefaultLabelPalette) noexcept;

/// `palette`, or kDefaultLabelPalette when it is empty (a request without a pack's palette).
[[nodiscard]] std::span<const Rgb8> labelPaletteOr(std::span<const Rgb8> palette) noexcept;

/// The fonts of the generated layers: the title's and the details' (both may be the same face).
struct LabelFonts {
    const FontFace* title = nullptr;
    const FontFace* detail = nullptr;
};

/// How a title was split into lines.
enum class LineSplit {
    OneLine,   ///< the whole title on one line
    Separator, ///< at " : ", ": " or " - " (the colon kept on the first line, the dash dropped)
    Words,     ///< between words
};

[[nodiscard]] std::string_view toString(LineSplit split) noexcept; ///< "one-line", "separator", "words"

/// The lines a title was set in.
struct TitleLayout {
    std::vector<std::string> lines; ///< UTF-8, each as drawn (an ellipsis included)
    float pixelHeight = 0.0F;       ///< the size every line is drawn at, in the layer's pixels
    LineSplit split = LineSplit::OneLine;
    bool ellipsized = false;
    /// The catalog's cleaned file-name title was used: the title holds no character the font has (a Japanese title in
    /// the Latin font).
    bool usedFallbackTitle = false;
    /// Characters drawn as the font's replacement glyph.
    std::size_t replacementGlyphs = 0;
};

/// The text a layer shows: `title` unless it has no character the font has (white space aside), then `fallbackTitle`
/// when it has one, else `title` anyway (drawn with replacement glyphs). White space collapsed and trimmed.
[[nodiscard]] std::u32string chooseTitleText(const FontFace& font, std::string_view title,
                                             std::string_view fallbackTitle, bool* usedFallback = nullptr);

/// The spine's title layout in `format` (the rules in the file comment).
[[nodiscard]] TitleLayout layoutSpineTitle(const FontFace& font, std::string_view title, std::string_view fallbackTitle,
                                           TextureFormat format);
/// The same on the canvas of `spine`'s shape (labelCanvas() of the spine layer: the sizes shares of the canvas's
/// thickness, the line as long as its length allows less the printed spine's band); layoutSpineTitle() above for the
/// layer's shape.
[[nodiscard]] TitleLayout layoutSpineTitle(const FontFace& font, std::string_view title, std::string_view fallbackTitle,
                                           TextureFormat format, const LabelAspect& spine);

/// The generated front label's title layout in `format`.
[[nodiscard]] TitleLayout layoutFrontTitle(const FontFace& font, std::string_view title,
                                           std::string_view fallbackTitle, TextureFormat format);
/// The same on the canvas of `label`'s shape (labelCanvas(): the line as wide as the canvas allows, the sizes those of
/// the layer's scale); layoutFrontTitle() above for the layer's shape.
[[nodiscard]] TitleLayout layoutFrontTitle(const FontFace& font, std::string_view title,
                                           std::string_view fallbackTitle, TextureFormat format,
                                           const LabelAspect& label);

struct SpineRequest {
    std::string title;
    std::string fallbackTitle; ///< the catalog's cleaned file-name title
    /// The cover's dominant colour (its layer's RvTexture::dominant); none without a cover.
    std::optional<Rgb8> coverColour;
    std::uint64_t seed = 0; ///< the placement's seed: the palette colour without a cover
    /// The spine label quad's shape (I6 step 4's review, MediaFamily::spineAspect(): across by along): a printed spine,
    /// laid out on a canvas of that shape (labelCanvas() of the 40 x 256 layer), a darker band of its colour across its
    /// top 12%, then resampled once into the layer, which the quad stretches back; the layer's own shape by default
    /// (I3's spine, the cartridges').
    LabelAspect spine;
    /// The room's theme pack's label palette (I6 step 3), not owned (the caller's, alive while the request is served);
    /// empty: kDefaultLabelPalette.
    std::span<const Rgb8> palette;
};

struct FrontLabelRequest {
    std::string title;
    std::string fallbackTitle;
    std::string systemName; ///< the system's full name (catalog data; an invented one in committed fixtures)
    std::string year;       ///< "1994", or empty
    std::uint64_t seed = 0;
    /// The room's theme pack's label palette (I6 step 3), not owned; empty: kDefaultLabelPalette.
    std::span<const Rgb8> palette;
    /// The label quad's shape (ADR-0013 §4): the label is laid out on a canvas of that shape (labelCanvas(): the
    /// frame, the stripes and the title at the layer's scale, the title's band taller or the lines wider), then
    /// resampled once into the layer; the layer's own shape by default (I3's label).
    LabelAspect label;
};

/// The colour a spine starts from: the cover's dominant colour, else the request's palette's colour for the seed.
[[nodiscard]] Rgb8 spineBaseColour(const SpineRequest& request) noexcept;

/// A generated layer's picture (level 0, before encoding) and what was decided.
struct GeneratedPicture {
    ImageData picture; ///< RGBA8 sRGB of layerSize(class, format)
    TitleLayout layout;
    LabelColours colours;
};

/// A generated layer, encoded.
struct GeneratedLayer {
    RvTexture texture; ///< RvTexture::dominant is the background
    TitleLayout layout;
    LabelColours colours;
};

/// The spine's picture and layer. Errors: InvalidArgument (no title font).
[[nodiscard]] Expected<GeneratedPicture> drawSpine(const LabelFonts& fonts, const SpineRequest& request,
                                                   TextureFormat format);
[[nodiscard]] Expected<GeneratedLayer> buildSpineLayer(const LabelFonts& fonts, const SpineRequest& request,
                                                       TextureFormat format);

/// The generated front label's picture and layer. Errors: InvalidArgument (no title font; the details use the title
/// font when `fonts.detail` is null).
[[nodiscard]] Expected<GeneratedPicture> drawFrontLabel(const LabelFonts& fonts, const FrontLabelRequest& request,
                                                        TextureFormat format);
[[nodiscard]] Expected<GeneratedLayer> buildFrontLabelLayer(const LabelFonts& fonts, const FrontLabelRequest& request,
                                                            TextureFormat format);

/// The smallest side of a title card.
inline constexpr int kMinTitleCardSide = 64;

/// A screen's generated title card (I5 step 1, ADR-0016 §8: the end of a game's fallback chain on the hero CRT, 640 x
/// 480), a period title screen of the item's label: the label's layout (a frame, the title centred, the system's name
/// at the bottom left and the year at the bottom right; one thin rule across the top for the label's two stripes) drawn
/// at `width` x `height` directly, never resampled, its sizes those of the 256 px wide label times width / 256 (the
/// title's 1.3 times, the details' 1.5 times), the rows a taller canvas adds given to the title's band. Its colours come
/// from the request's palette colour for its seed (the label's on the shelf): a dark field of that colour, muted 30%
/// towards grey and scaled to one luma for every colour (about the clips' level on the tube, never a bright panel), the
/// title, details, frame and rule in that colour raised and mixed towards ivory; a soft glow at the middle falling off
/// to the edges is baked in (as makeStandby's). GeneratedPicture::colours holds the field (before the glow) and the
/// ink. `request.label` is not used. Errors: InvalidArgument (no title font; a side below kMinTitleCardSide or above
/// kMaxImageDimension).
[[nodiscard]] Expected<GeneratedPicture> drawTitleCard(const LabelFonts& fonts, const FrontLabelRequest& request,
                                                       int width, int height);

/// Where a front layer came from.
enum class FrontOrigin {
    Cover,          ///< the cover, fitted
    NoCover,        ///< no cover given: the generated label
    CoverFailed,    ///< the cover could not be used (not an image, truncated, too large): the generated label
};

[[nodiscard]] std::string_view toString(FrontOrigin origin) noexcept; ///< "cover", "no-cover", "cover-failed"

struct FrontLayer {
    RvTexture texture;
    FrontOrigin origin = FrontOrigin::Cover;
    std::optional<Error> coverError; ///< why the cover was not used (CoverFailed)
};

/// A front layer whatever happens (ADR-0011 §5, style §21: a fallback for every failure): the cover's layer when
/// `coverBytes` is given and decodes, else the generated label. Errors only when the generated label itself cannot be
/// drawn (no title font).
[[nodiscard]] Expected<FrontLayer> buildFrontLayer(std::optional<std::span<const std::byte>> coverBytes,
                                                   const FrontLabelRequest& label, const LabelFonts& fonts,
                                                   TextureFormat format, std::string_view context,
                                                   const FitOptions& options = {});

} // namespace rv::assets
