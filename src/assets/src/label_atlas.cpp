#include <rv/assets/label_atlas.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rv/assets/test_patterns.hpp>

namespace rv::assets {

namespace {

constexpr char32_t kEllipsis = U'…';

/// Tabs: the text's largest and smallest sizes, its margin across.
constexpr float kTabLargest = 46.0F;
constexpr float kTabSmallest = 20.0F;
constexpr float kTabMargin = 10.0F;
/// Captions: the title's sizes, the detail's, the margins and the hairline.
constexpr float kCaptionTitleLargest = 32.0F;
constexpr float kCaptionTitleSmallest = 22.0F;
constexpr float kCaptionDetail = 24.0F;
constexpr float kCaptionMargin = 12.0F;
constexpr int kCaptionRule = 2;

constexpr int roundUp4(int value) noexcept {
    return (value + 3) / 4 * 4;
}

/// `text` as code points, its runs of white space one space, trimmed.
std::u32string collapsed(std::string_view text) {
    std::u32string out;
    bool space = false;
    for (const char32_t c : decodeUtf8(text)) {
        if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r') {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(U' ');
            space = false;
        }
        out.push_back(c);
    }
    return out;
}

/// `line` cut with an ellipsis until it fits `width` at `size`.
std::u32string fitted(const FontFace& font, std::u32string line, float size, float width) {
    if (font.layout(line, size).width <= width) {
        return line;
    }
    while (!line.empty()) {
        line.pop_back();
        while (!line.empty() && line.back() == U' ') {
            line.pop_back();
        }
        std::u32string candidate = line;
        candidate.push_back(kEllipsis);
        if (font.layout(candidate, size).width <= width) {
            return candidate;
        }
    }
    return std::u32string(1, kEllipsis);
}

/// The largest size from `largest` down to `smallest` (whole pixels) at which `line` fits `width`; `smallest` when
/// none.
float largestFit(const FontFace& font, std::u32string_view line, float largest, float smallest, float width) {
    for (float size = largest; size > smallest; size -= 1.0F) {
        if (font.layout(line, size).width <= width) {
            return size;
        }
    }
    return smallest;
}

void fill(ImageData& image, int x0, int y0, int x1, int y1, Rgb8 colour) {
    x0 = std::clamp(x0, 0, image.width);
    x1 = std::clamp(x1, 0, image.width);
    y0 = std::clamp(y0, 0, image.height);
    y1 = std::clamp(y1, 0, image.height);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
            image.pixels[at] = colour.r;
            image.pixels[at + 1] = colour.g;
            image.pixels[at + 2] = colour.b;
            image.pixels[at + 3] = 255;
        }
    }
}

/// A run of `text` at `size` centred across the cell, its baseline at `baseline`.
PlacedRun centred(const FontFace& font, std::u32string text, float size, const AtlasCell& cell, float baseline) {
    const float width = font.layout(text, size).width;
    return PlacedRun{&font, std::move(text), size,
                     static_cast<float>(cell.x) + 0.5F * (static_cast<float>(cell.width) - width), baseline};
}

} // namespace

glm::vec4 LabelAtlasLayout::uv(std::size_t index) const noexcept {
    if (index >= cells.size() || width <= 0 || height <= 0) {
        return glm::vec4(0.0F, 0.0F, 1.0F, 1.0F);
    }
    const AtlasCell& cell = cells[index];
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    return glm::vec4(static_cast<float>(cell.x) / w, static_cast<float>(cell.y) / h,
                     static_cast<float>(cell.x + cell.width) / w, static_cast<float>(cell.y + cell.height) / h);
}

LabelAtlasLayout labelAtlasLayout(std::size_t count, glm::ivec2 cell, int columns) {
    LabelAtlasLayout layout;
    if (count == 0 || count > kMaxAtlasCells || columns < 1 || cell.x < 8 || cell.y < 8) {
        return layout;
    }
    const int across = static_cast<int>(std::min<std::size_t>(count, static_cast<std::size_t>(columns)));
    const int rows =
        static_cast<int>((count + static_cast<std::size_t>(across) - 1) / static_cast<std::size_t>(across));
    const int pitchX = cell.x + 2 * kAtlasGutter;
    const int pitchY = cell.y + 2 * kAtlasGutter;
    layout.width = roundUp4(across * pitchX);
    layout.height = roundUp4(rows * pitchY);
    layout.cells.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int column = static_cast<int>(i % static_cast<std::size_t>(across));
        const int row = static_cast<int>(i / static_cast<std::size_t>(across));
        layout.cells.push_back(AtlasCell{column * pitchX + kAtlasGutter, row * pitchY + kAtlasGutter, cell.x, cell.y});
    }
    return layout;
}

ImageData makeTabAtlas(std::span<const std::string> texts, const FontFace& bold) {
    const LabelAtlasLayout layout = labelAtlasLayout(texts.size(), kTabCell, kTabColumns);
    if (layout.cells.empty()) {
        return {};
    }
    ImageData image = makeSolid(layout.width, layout.height, glm::u8vec4(kTabPaper.r, kTabPaper.g, kTabPaper.b, 255));
    std::vector<PlacedRun> runs;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        const AtlasCell& cell = layout.cells[i];
        const float room = static_cast<float>(cell.width) - 2.0F * kTabMargin;
        std::u32string text = collapsed(texts[i]);
        if (text.empty()) {
            continue;
        }
        const float size = largestFit(bold, text, kTabLargest, kTabSmallest, room);
        text = fitted(bold, std::move(text), size, room);
        // The capitals centred up and down.
        const float baseline =
            static_cast<float>(cell.y) + 0.5F * (static_cast<float>(cell.height) + bold.capHeight(size));
        runs.push_back(centred(bold, std::move(text), size, cell, baseline));
    }
    drawText(image, kTabInk, runs);
    return image;
}

ImageData makeCaptionAtlas(std::span<const CaptionText> cards, const FontFace& bold, const FontFace& regular) {
    const LabelAtlasLayout layout = labelAtlasLayout(cards.size(), kCaptionCell, kCaptionColumns);
    if (layout.cells.empty()) {
        return {};
    }
    ImageData image =
        makeSolid(layout.width, layout.height, glm::u8vec4(kCardPaper.r, kCardPaper.g, kCardPaper.b, 255));
    std::vector<PlacedRun> runs;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        const AtlasCell& cell = layout.cells[i];
        const float room = static_cast<float>(cell.width) - 2.0F * kCaptionMargin;
        std::u32string title = collapsed(cards[i].title);
        std::u32string detail = collapsed(cards[i].detail);
        const float titleSize = title.empty()
                                    ? kCaptionTitleSmallest
                                    : largestFit(bold, title, kCaptionTitleLargest, kCaptionTitleSmallest, room);
        if (!title.empty()) {
            title = fitted(bold, std::move(title), titleSize, room);
        }
        const auto top = static_cast<float>(cell.y);
        const auto height = static_cast<float>(cell.height);
        if (detail.empty()) {
            if (!title.empty()) {
                runs.push_back(centred(bold, std::move(title), titleSize, cell,
                                       top + 0.5F * (height + bold.capHeight(titleSize))));
            }
            continue;
        }
        detail = fitted(regular, std::move(detail), kCaptionDetail, room);
        // The title's capitals in the upper half, the detail's in the lower one, the hairline between them.
        const float titleBaseline = top + 0.25F * height + 0.5F * bold.capHeight(titleSize) + 2.0F;
        const float detailBaseline = top + 0.75F * height + 0.5F * regular.capHeight(kCaptionDetail) - 2.0F;
        if (!title.empty()) {
            runs.push_back(centred(bold, std::move(title), titleSize, cell, titleBaseline));
        }
        runs.push_back(centred(regular, std::move(detail), kCaptionDetail, cell, detailBaseline));
        const int ruleY = cell.y + cell.height / 2;
        fill(image, cell.x + static_cast<int>(kCaptionMargin), ruleY - kCaptionRule / 2,
             cell.x + cell.width - static_cast<int>(kCaptionMargin), ruleY + kCaptionRule / 2, kCardRule);
    }
    drawText(image, kCardInk, runs);
    return image;
}

} // namespace rv::assets
