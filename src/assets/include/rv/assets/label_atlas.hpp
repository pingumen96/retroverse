#pragma once

// The shelves' labels (I12 step 5, ADR-0021 §1): the atlases of a room's letter tabs (the paper insert of each
// spine-out level's brass holder: "A–C", "D", "0-9", "#") and of its caption cards (under each face-out title of a
// display case: the title on the first line in the bold face, "year · developer" on the second in the regular one, a
// hairline between them). Each atlas is the cells of one room's plates, row by row, each cell inside a gutter of its
// own background so that its mip levels do not bleed into its neighbours; the plates' faces (geometry's label_plates)
// show their cells. The texts are the catalog's, set in the bundled Atkinson Hyperlegible (text_raster.hpp): readable,
// never an imitation of a real label. Every function is deterministic and safe on several threads with shared fonts.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/text_raster.hpp>

namespace rv::assets {

/// A cell's own rectangle in an atlas, in pixels (inside its gutter).
struct AtlasCell {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const AtlasCell&) const = default;
};

/// Where the cells of an atlas lie.
struct LabelAtlasLayout {
    int width = 0;
    int height = 0;
    std::vector<AtlasCell> cells;

    /// Cell `index`'s rectangle as (uLeft, vTop, uRight, vBottom), uv (0, 0) at the atlas's top-left corner.
    [[nodiscard]] glm::vec4 uv(std::size_t index) const noexcept;

    bool operator==(const LabelAtlasLayout&) const = default;
};

/// A letter tab's cell (its insert, 60 x 20 mm: 3 to 1) and a caption card's (75 x 28 mm), in pixels.
inline constexpr glm::ivec2 kTabCell{192, 64};
inline constexpr glm::ivec2 kCaptionCell{256, 96};
/// The gutter round every cell, in pixels.
inline constexpr int kAtlasGutter = 4;
/// Cells across an atlas of tabs and of captions.
inline constexpr int kTabColumns = 4;
inline constexpr int kCaptionColumns = 2;
/// The most cells an atlas holds (geometry's label_plates' most plates).
inline constexpr std::size_t kMaxAtlasCells = 1024;

/// `count` cells of `cell` pixels (each side at least 8) laid out `columns` across (fewer when there are fewer cells)
/// row by row, each with kAtlasGutter pixels of gutter on every side; the atlas's sides rounded up to multiples of 4
/// (its blocks). An empty layout for no cell, a count above kMaxAtlasCells or columns under 1.
[[nodiscard]] LabelAtlasLayout labelAtlasLayout(std::size_t count, glm::ivec2 cell, int columns);

/// The colours: the tab's insert and its ink, the card's paper, ink and hairline (style §3's ivory and charcoal).
inline constexpr Rgb8 kTabPaper{0xEE, 0xE4, 0xCC};
inline constexpr Rgb8 kTabInk{0x2B, 0x22, 0x1A};
inline constexpr Rgb8 kCardPaper{0xF4, 0xEF, 0xE6};
inline constexpr Rgb8 kCardInk{0x17, 0x1B, 0x22};
inline constexpr Rgb8 kCardRule{0xB9, 0xB1, 0xA3};

/// The tabs' atlas (labelAtlasLayout(texts, kTabCell, kTabColumns)): each text, its white space collapsed, centred in
/// its cell on kTabPaper in `bold`, as large as fits from 46 down to 20 pixels (then cut with an ellipsis). RGBA8 sRGB,
/// opaque; empty for no text or more than kMaxAtlasCells.
[[nodiscard]] ImageData makeTabAtlas(std::span<const std::string> texts, const FontFace& bold);

/// A caption card's two lines (UTF-8): the title, and what is known of its year and maker ("" none).
struct CaptionText {
    std::string title;
    std::string detail;

    bool operator==(const CaptionText&) const = default;
};

/// The captions' atlas (labelAtlasLayout(cards, kCaptionCell, kCaptionColumns)) on kCardPaper in kCardInk: the title in
/// `bold` from 32 down to 22 pixels (then cut with an ellipsis), the detail in `regular` at 24 pixels (cut alike), a
/// hairline in kCardRule between them; a card without a detail has its title alone, centred. RGBA8 sRGB, opaque; empty
/// for no card or more than kMaxAtlasCells.
[[nodiscard]] ImageData makeCaptionAtlas(std::span<const CaptionText> cards, const FontFace& bold,
                                         const FontFace& regular);

} // namespace rv::assets
