#pragma once

// Text rasterized with stb_truetype from the bundled Atkinson Hyperlegible (I3 step 5, ADR-0011 §5): the spines and
// generated labels of the collection. stb_truetype is not hardened against malformed fonts, so a FontFace only ever
// loads a file whose SHA-256 matches its entry in <asset root>/registry/assets.json (never a system or user font).
// Text is drawn at twice the layer's resolution and reduced (drawText()), so that small glyphs keep their shapes; a
// character the font lacks is drawn as the font's replacement glyph (U+FFFD when the font has it, else its .notdef).
//
// A FontFace is immutable once loaded: its const functions may run on several worker threads at once.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// The bundled faces (asset-relative paths, as assets/registry/assets.json names them): the titles are set in bold, the
/// generated label's system name and year in the regular weight.
inline constexpr std::string_view kLabelTitleFontPath = "ui/fonts/AtkinsonHyperlegible-Bold.ttf";
inline constexpr std::string_view kLabelDetailFontPath = "ui/fonts/AtkinsonHyperlegible-Regular.ttf";
/// Font files larger than this are refused before they are hashed.
inline constexpr std::uintmax_t kMaxFontFileBytes = 16ull * 1024ull * 1024ull;

/// UTF-8 to code points; each malformed sequence becomes U+FFFD.
[[nodiscard]] std::u32string decodeUtf8(std::string_view text);
/// Code points to UTF-8 (surrogates and values above U+10FFFF become U+FFFD).
[[nodiscard]] std::string encodeUtf8(std::u32string_view text);

/// One run of glyphs laid out on a line: the glyph indices and their pen positions, in pixels from the run's start.
struct GlyphRun {
    std::vector<int> glyphs;
    std::vector<float> positions;
    float width = 0.0F;             ///< the advance of the whole run (kerning included)
    std::size_t replacementGlyphs = 0; ///< characters the font lacks, drawn as the replacement glyph
};

class FontFace {
public:
    /// Loads `file` when its SHA-256 (lowercase hex) is `expectedSha256`, else refuses it unread by stb_truetype.
    /// Errors: NotFound, IoError (the file), OutOfRange (larger than kMaxFontFileBytes), ValidationFailed (another
    /// SHA-256), ParseError (stb_truetype cannot read the font).
    [[nodiscard]] static Expected<FontFace> loadVerified(const std::filesystem::path& file,
                                                         std::string_view expectedSha256);

    /// Loads a bundled font: its entry (path `relativePath`, kind "font") read from
    /// <assetRoot>/registry/assets.json, then loadVerified(). Errors: those of loadVerified(), NotFound (no entry),
    /// ParseError (the registry).
    [[nodiscard]] static Expected<FontFace> loadBundled(const std::filesystem::path& assetRoot,
                                                        std::string_view relativePath);

    /// The file's SHA-256, lowercase hex (part of the generated layers' cache keys).
    [[nodiscard]] const std::string& sha256() const noexcept;

    /// The glyph of `codePoint`, 0 when the font lacks it.
    [[nodiscard]] int glyphIndex(char32_t codePoint) const noexcept;
    [[nodiscard]] bool hasGlyph(char32_t codePoint) const noexcept { return glyphIndex(codePoint) != 0; }
    /// The glyph drawn for a character the font lacks: U+FFFD's when the font has one, else 0 (.notdef).
    [[nodiscard]] int replacementGlyph() const noexcept;

    /// The font's ascent and descent (descent negative) in pixels at `pixelHeight` (stb_truetype's size: ascent minus
    /// descent equals pixelHeight).
    [[nodiscard]] float ascent(float pixelHeight) const noexcept;
    [[nodiscard]] float descent(float pixelHeight) const noexcept;
    /// The height of a capital H in pixels at `pixelHeight`.
    [[nodiscard]] float capHeight(float pixelHeight) const noexcept;

    /// Lays out `text` on one line at `pixelHeight`: advances and kerning, missing characters as the replacement glyph.
    [[nodiscard]] GlyphRun layout(std::u32string_view text, float pixelHeight) const;

    /// Adds the coverage of `run` (laid out at `pixelHeight`) to `coverage` (width x height, one byte per pixel; where
    /// glyphs overlap the larger coverage counts), the pen starting at (x, baseline).
    void draw(const GlyphRun& run, float pixelHeight, float x, float baseline, std::span<std::uint8_t> coverage,
              int width, int height) const;

private:
    struct Impl;
    explicit FontFace(std::shared_ptr<const Impl> impl) noexcept : m_impl(std::move(impl)) {}
    std::shared_ptr<const Impl> m_impl;
};

/// A run placed on a layer, in the layer's pixels.
struct PlacedRun {
    const FontFace* font = nullptr;
    std::u32string text;
    float pixelHeight = 0.0F;
    float x = 0.0F;
    float baseline = 0.0F;
};

/// Draws `runs` in `foreground` onto an RGBA8 sRGB image (4 channels; anything else is left as it is): the coverage
/// drawn at twice the size (every position and size doubled, each run laid out again at its doubled size), reduced by a
/// 2 x 2 box, and each pixel mixed with the foreground by it in linear light.
void drawText(ImageData& image, Rgb8 foreground, std::span<const PlacedRun> runs);

} // namespace rv::assets
