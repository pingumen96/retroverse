#include <rv/assets/history_plaque.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/test_patterns.hpp>

namespace rv::assets {

namespace {

constexpr char32_t kEllipsis = U'…';

/// The words of `text`: split at spaces, tabs and line breaks.
std::vector<std::u32string> wordsOf(std::string_view text) {
    std::vector<std::u32string> words;
    std::u32string word;
    for (const char32_t c : decodeUtf8(text)) {
        if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r') {
            if (!word.empty()) {
                words.push_back(std::move(word));
                word.clear();
            }
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) {
        words.push_back(std::move(word));
    }
    return words;
}

float widthOf(const FontFace& font, std::u32string_view text, float pixelHeight) {
    return font.layout(text, pixelHeight).width;
}

/// `line` cut so that it fits `width` with `tail` after it (characters taken off its end, then trailing spaces).
std::u32string cutToFit(std::u32string line, std::u32string_view tail, float width, float pixelHeight,
                        const FontFace& font) {
    while (!line.empty()) {
        std::u32string candidate = line;
        candidate.append(tail);
        if (widthOf(font, candidate, pixelHeight) <= width) {
            return candidate;
        }
        line.pop_back();
        while (!line.empty() && line.back() == U' ') {
            line.pop_back();
        }
    }
    return std::u32string(tail);
}

void fillRect(ImageData& image, int x0, int y0, int x1, int y1, Rgb8 colour) {
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
        }
    }
}

/// The brass: kPlaqueBrass, a little lighter at the top and darker at the bottom, with fine horizontal brushing.
ImageData brassPlate(int width, int height) {
    ImageData image = makeSolid(width, height, glm::u8vec4(0, 0, 0, 255));
    const float base[3] = {srgbByteToLinear(kPlaqueBrass.r), srgbByteToLinear(kPlaqueBrass.g),
                           srgbByteToLinear(kPlaqueBrass.b)};
    for (int y = 0; y < height; ++y) {
        const float t = height > 1 ? static_cast<float>(y) / static_cast<float>(height - 1) : 0.0F;
        // The brushing: a fixed pattern of rows, a few percent apart (deterministic, no random source).
        const float brush = 1.0F + 0.035F * std::sin(static_cast<float>(y) * 2.39996F) *
                                       std::sin(static_cast<float>(y) * 0.61803F);
        const float shade = (1.12F - 0.24F * t) * brush;
        for (int x = 0; x < width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                image.pixels[at + c] = linearToSrgbByte(std::clamp(base[c] * shade, 0.0F, 1.0F));
            }
        }
    }
    return image;
}

/// `logo`'s alpha engraved in kPlaqueEngraving, fitted into the box (x, y, w, h) at its aspect and centred.
void engraveLogo(ImageData& image, const ImageData& logo, float x, float y, float w, float h) {
    if (!logo.isValid() || logo.channels != 4 || w < 1.0F || h < 1.0F) {
        return;
    }
    const float scale = std::min(w / static_cast<float>(logo.width), h / static_cast<float>(logo.height));
    const int lw = std::max(1, static_cast<int>(std::lround(static_cast<float>(logo.width) * scale)));
    const int lh = std::max(1, static_cast<int>(std::lround(static_cast<float>(logo.height) * scale)));
    auto fitted = resampleSrgbAlpha(logo, lw, lh);
    if (!fitted) {
        return;
    }
    const int left = static_cast<int>(std::lround(x + (w - static_cast<float>(lw)) / 2.0F));
    const int top = static_cast<int>(std::lround(y + (h - static_cast<float>(lh)) / 2.0F));
    const float ink[3] = {srgbByteToLinear(kPlaqueEngraving.r), srgbByteToLinear(kPlaqueEngraving.g),
                          srgbByteToLinear(kPlaqueEngraving.b)};
    for (int row = 0; row < lh; ++row) {
        for (int col = 0; col < lw; ++col) {
            const int tx = left + col;
            const int ty = top + row;
            if (tx < 0 || ty < 0 || tx >= image.width || ty >= image.height) {
                continue;
            }
            const float alpha =
                static_cast<float>(
                    fitted->pixels[(static_cast<std::size_t>(row) * static_cast<std::size_t>(lw) +
                                    static_cast<std::size_t>(col)) * 4 + 3]) /
                255.0F;
            const std::size_t at =
                (static_cast<std::size_t>(ty) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(tx)) *
                4;
            for (std::size_t c = 0; c < 3; ++c) {
                const float below = srgbByteToLinear(image.pixels[at + c]);
                image.pixels[at + c] = linearToSrgbByte(ink[c] * alpha + below * (1.0F - alpha));
            }
        }
    }
}

/// One line centred in [left, right] at `baseline`.
PlacedRun centred(const FontFace& font, std::u32string text, float pixelHeight, float left, float right,
                  float baseline) {
    const float advance = widthOf(font, text, pixelHeight);
    return PlacedRun{&font, std::move(text), pixelHeight, left + std::max(0.0F, (right - left - advance) / 2.0F),
                     baseline};
}

} // namespace

std::string plaqueDetails(const PlaqueText& text) {
    std::string details;
    for (const std::string* part : {&text.maker, &text.year, &text.type}) {
        if (part->empty()) {
            continue;
        }
        if (!details.empty()) {
            details += " · ";
        }
        details += *part;
    }
    return details;
}

std::vector<std::u32string> wrapText(std::string_view text, float width, float pixelHeight, const FontFace& font,
                                     std::size_t maxLines) {
    std::vector<std::u32string> lines;
    if (maxLines == 0 || width <= 0.0F || pixelHeight <= 0.0F) {
        return lines;
    }
    const std::vector<std::u32string> words = wordsOf(text);
    const std::u32string ellipsis(1, font.hasGlyph(kEllipsis) ? kEllipsis : U'.');
    std::u32string line;
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::u32string candidate = line.empty() ? words[i] : line + U' ' + words[i];
        if (widthOf(font, candidate, pixelHeight) <= width) {
            line = std::move(candidate);
            continue;
        }
        if (line.empty()) {
            // A word wider than a line stands alone, cut.
            line = cutToFit(words[i], ellipsis, width, pixelHeight, font);
            lines.push_back(std::move(line));
            line.clear();
        } else {
            lines.push_back(std::move(line));
            line = widthOf(font, words[i], pixelHeight) <= width ? words[i]
                                                                  : cutToFit(words[i], ellipsis, width, pixelHeight, font);
        }
        if (lines.size() == maxLines) {
            // The last line holds no more: it ends with the ellipsis.
            lines.back() = cutToFit(lines.back(), ellipsis, width, pixelHeight, font);
            return lines;
        }
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

ImageData makeHistoryPlaque(int width, int height, const PlaqueText& text, const ImageData* logo,
                            const FontFace& bold, const FontFace& regular) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    ImageData image = brassPlate(width, height);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float margin = kPlaqueMargin * h;
    // The engraved border, half the margin in.
    const int border = std::max(1, static_cast<int>(std::lround(0.006F * h)));
    const int inset = static_cast<int>(std::lround(margin / 2.0F));
    fillRect(image, inset, inset, width - inset, inset + border, kPlaqueEngraving);
    fillRect(image, inset, height - inset - border, width - inset, height - inset, kPlaqueEngraving);
    fillRect(image, inset, inset, inset + border, height - inset, kPlaqueEngraving);
    fillRect(image, width - inset - border, inset, width - inset, height - inset, kPlaqueEngraving);

    const float left = margin;
    const float right = w - margin;
    float top = margin;
    std::vector<PlacedRun> runs;
    // The title: the logo, else the name.
    const float titleHeight = kPlaqueTitle * h;
    if (logo != nullptr && logo->isValid()) {
        engraveLogo(image, *logo, left, top, right - left, titleHeight);
    } else if (!text.name.empty()) {
        std::u32string name = decodeUtf8(text.name);
        float size = titleHeight * 0.8F;
        const float advance = widthOf(bold, name, size);
        if (advance > right - left && advance > 0.0F) {
            size *= (right - left) / advance;
        }
        runs.push_back(centred(bold, std::move(name), size, left, right, top + (titleHeight + bold.capHeight(size)) / 2.0F));
    }
    top += titleHeight + 0.03F * h;
    // The details.
    const std::string details = plaqueDetails(text);
    if (!details.empty()) {
        const float size = kPlaqueDetails * h;
        std::u32string line = decodeUtf8(details);
        if (widthOf(regular, line, size) > right - left) {
            line = cutToFit(std::move(line), std::u32string(1, kEllipsis), right - left, size, regular);
        }
        runs.push_back(centred(regular, std::move(line), size, left, right, top + regular.ascent(size)));
        top += size * 1.25F;
    }
    // The rule.
    const int ruleY = static_cast<int>(std::lround(top + 0.012F * h));
    fillRect(image, static_cast<int>(std::lround(left + 0.2F * (right - left))), ruleY,
             static_cast<int>(std::lround(right - 0.2F * (right - left))), ruleY + border, kPlaqueEngraving);
    top = static_cast<float>(ruleY) + 0.035F * h;
    // The description, as many lines as the plate holds.
    const float size = kPlaqueLine * h;
    const float lineStep = size * 1.18F;
    const float bottom = h - margin;
    if (!text.description.empty() && bottom - top >= size) {
        const auto lines = static_cast<std::size_t>(std::floor((bottom - top) / lineStep));
        float baseline = top + regular.ascent(size);
        for (std::u32string& line : wrapText(text.description, right - left, size, regular, lines)) {
            runs.push_back(PlacedRun{&regular, std::move(line), size, left, baseline});
            baseline += lineStep;
        }
    }
    if (!runs.empty()) {
        drawText(image, kPlaqueEngraving, runs);
    }
    return image;
}

} // namespace rv::assets
