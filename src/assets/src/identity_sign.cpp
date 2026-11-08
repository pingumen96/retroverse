#include <rv/assets/identity_sign.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/test_patterns.hpp>

namespace rv::assets {

namespace {

/// The share of the inner height the logos take in a stacked sign, where the name begins, and the name's share.
constexpr float kStackedLogoShare = 0.62F;
constexpr float kStackedNameTop = 0.70F;
constexpr float kStackedNameShare = 0.30F;
/// The share of the inner width the logos' row may take beside the name.
constexpr float kSideBySideLogoShare = 0.5F;
/// The gap between the logos' row and the name beside it, against the inner height.
constexpr float kSideBySideGap = 0.25F;
/// The gap between two logos, against their height.
constexpr float kLogoGap = 0.1F;
/// Two lines when they let the name be this much larger than one line does.
constexpr float kTwoLinesGain = 1.15F;
/// The size names are measured at (their width scales with it).
constexpr float kMeasureHeight = 100.0F;

struct FittedName {
    std::vector<std::u32string> lines;
    float pixelHeight = 0.0F;
};

float measure(const FontFace& font, std::u32string_view text) {
    return font.layout(text, kMeasureHeight).width;
}

/// The largest size `text` takes in `boxWidth` x `boxHeight`, on one line or on two broken at the space nearest its
/// middle.
FittedName fitName(const std::u32string& text, float boxWidth, float boxHeight, const FontFace& font) {
    FittedName fitted;
    if (text.empty() || boxWidth <= 0.0F || boxHeight <= 0.0F) {
        return fitted;
    }
    const float oneWidth = measure(font, text);
    const float one = oneWidth > 0.0F ? std::min(boxHeight, boxWidth * kMeasureHeight / oneWidth) : boxHeight;
    fitted.lines = {text};
    fitted.pixelHeight = one;
    // The space nearest the middle (the first of two as near).
    std::size_t split = std::u32string::npos;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] != U' ') {
            continue;
        }
        const auto distance = [&](std::size_t i) {
            const auto twice = static_cast<std::ptrdiff_t>(2 * i) - static_cast<std::ptrdiff_t>(text.size());
            return twice < 0 ? -twice : twice;
        };
        if (split == std::u32string::npos || distance(at) < distance(split)) {
            split = at;
        }
    }
    if (split == std::u32string::npos) {
        return fitted;
    }
    std::u32string first = text.substr(0, split);
    std::u32string second = text.substr(split + 1);
    while (!first.empty() && first.back() == U' ') {
        first.pop_back();
    }
    while (!second.empty() && second.front() == U' ') {
        second.erase(second.begin());
    }
    if (first.empty() || second.empty()) {
        return fitted;
    }
    const float wider = std::max(measure(font, first), measure(font, second));
    const float two = wider > 0.0F ? std::min(boxHeight / 2.0F, boxWidth * kMeasureHeight / wider) : boxHeight / 2.0F;
    if (two > one * kTwoLinesGain) {
        fitted.lines = {std::move(first), std::move(second)};
        fitted.pixelHeight = two;
    }
    return fitted;
}

/// The logos' row in a box of `boxHeight` at most and `boxWidth` at most: their common height.
float rowHeight(std::span<const float> aspects, float boxWidth, float boxHeight) {
    float sum = 0.0F;
    for (const float aspect : aspects) {
        sum += aspect;
    }
    if (sum <= 0.0F) {
        return 0.0F;
    }
    const auto gaps = static_cast<float>(aspects.size() - 1);
    // width = height * (sum + gaps * kLogoGap)
    return std::max(0.0F, std::min(boxHeight, boxWidth / (sum + gaps * kLogoGap)));
}

float rowWidth(std::span<const float> aspects, float height) {
    float width = 0.0F;
    for (const float aspect : aspects) {
        width += aspect * height;
    }
    return width + static_cast<float>(aspects.empty() ? 0 : aspects.size() - 1) * kLogoGap * height;
}

void placeRow(IdentitySignLayout& layout, std::span<const float> aspects, float x, float y, float height) {
    for (const float aspect : aspects) {
        layout.logos.push_back(SignBox{x, y, aspect * height, height});
        x += (aspect + kLogoGap) * height;
    }
}

/// The name's lines placed in their box: centred vertically, each line centred horizontally, or from `x` when
/// `leftAligned`.
void placeName(IdentitySignLayout& layout, const FittedName& name, const FontFace& font, SignBox box,
               bool leftAligned) {
    layout.nameLines = name.lines;
    layout.namePixelHeight = name.pixelHeight;
    const float total = name.pixelHeight * static_cast<float>(name.lines.size());
    float top = box.y + (box.height - total) / 2.0F;
    for (const std::u32string& line : name.lines) {
        const float advance = font.layout(line, name.pixelHeight).width;
        const float x = leftAligned ? box.x : box.x + (box.width - advance) / 2.0F;
        layout.nameBoxes.push_back(SignBox{x, top, advance, name.pixelHeight});
        top += name.pixelHeight;
    }
}

float lineAdvance(const FittedName& name, const FontFace& font) {
    float wider = 0.0F;
    for (const std::u32string& line : name.lines) {
        wider = std::max(wider, font.layout(line, name.pixelHeight).width);
    }
    return wider;
}

/// The distance (pixels, a 3-4 chamfer) of every pixel of `inside` to the nearest pixel outside it; 0 outside.
std::vector<float> insideDistance(const std::vector<std::uint8_t>& inside, int width, int height) {
    constexpr float kFar = 1.0e6F;
    std::vector<float> d(inside.size());
    for (std::size_t i = 0; i < inside.size(); ++i) {
        d[i] = inside[i] != 0 ? kFar : 0.0F;
    }
    const auto at = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    };
    const auto value = [&](int x, int y) { return x < 0 || y < 0 || x >= width || y >= height ? 0.0F : d[at(x, y)]; };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float& here = d[at(x, y)];
            here = std::min({here, value(x - 1, y) + 3.0F, value(x, y - 1) + 3.0F, value(x - 1, y - 1) + 4.0F,
                             value(x + 1, y - 1) + 4.0F});
        }
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            float& here = d[at(x, y)];
            here = std::min({here, value(x + 1, y) + 3.0F, value(x, y + 1) + 3.0F, value(x + 1, y + 1) + 4.0F,
                             value(x - 1, y + 1) + 4.0F});
        }
    }
    for (float& distance : d) {
        distance /= 3.0F;
    }
    return d;
}

/// The colour difference (the sum of the channels' in linear light) past which two neighbouring pixels of a logo lie on
/// either side of an edge between its colours.
constexpr float kColourEdge = 0.25F;

/// 1 for every pixel of `picture` but the edges between its colours inside it (`inside`): an opaque pixel whose right or
/// lower opaque neighbour differs by kColourEdge or more, and that neighbour (insideDistance() of it measures the
/// distance to the nearest edge).
std::vector<std::uint8_t> notColourEdge(const ImageData& picture, const std::vector<std::uint8_t>& inside) {
    const int w = picture.width;
    const int h = picture.height;
    std::vector<std::uint8_t> away(inside.size(), 1);
    const auto at = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
    };
    const auto differ = [&](std::size_t a, std::size_t b) {
        float sum = 0.0F;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            sum += std::abs(srgbByteToLinear(picture.pixels[a * 4 + channel]) -
                            srgbByteToLinear(picture.pixels[b * 4 + channel]));
        }
        return sum >= kColourEdge;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t here = at(x, y);
            if (inside[here] == 0) {
                continue;
            }
            for (const std::size_t next : {x + 1 < w ? at(x + 1, y) : here, y + 1 < h ? at(x, y + 1) : here}) {
                if (next != here && inside[next] != 0 && differ(here, next)) {
                    away[here] = 0;
                    away[next] = 0;
                }
            }
        }
    }
    return away;
}

/// A logo resampled into its box (rounded to whole pixels, at least one): its pixels, and where it starts.
struct PlacedLogo {
    ImageData picture;
    int x = 0;
    int y = 0;
};

std::optional<PlacedLogo> placeLogo(const ImageData& logo, const SignBox& box) {
    if (!logo.isValid() || logo.channels != 4) {
        return std::nullopt;
    }
    const int x = static_cast<int>(std::lround(box.x));
    const int y = static_cast<int>(std::lround(box.y));
    const int width = std::max(1, static_cast<int>(std::lround(box.x + box.width)) - x);
    const int height = std::max(1, static_cast<int>(std::lround(box.y + box.height)) - y);
    auto resampled = resampleSrgbAlpha(logo, width, height);
    if (!resampled) {
        return std::nullopt;
    }
    return PlacedLogo{std::move(*resampled), x, y};
}

std::vector<PlacedRun> nameRuns(const IdentitySignLayout& layout, const FontFace& font) {
    std::vector<PlacedRun> runs;
    for (std::size_t line = 0; line < layout.nameLines.size(); ++line) {
        const SignBox& box = layout.nameBoxes[line];
        runs.push_back(PlacedRun{&font, layout.nameLines[line], layout.namePixelHeight, box.x,
                                 box.y + font.ascent(layout.namePixelHeight)});
    }
    return runs;
}

std::size_t pixelAt(int x, int y, int width) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
}

/// The lit plate: each logo over the black in linear light, a dark one as a white silhouette, then the name in white.
void drawLit(ImageData& image, std::span<const ImageData> logos, const IdentitySignLayout& layout,
             const FontFace& font) {
    for (std::size_t i = 0; i < layout.logos.size() && i < logos.size(); ++i) {
        const std::optional<PlacedLogo> placed = placeLogo(logos[i], layout.logos[i]);
        if (!placed) {
            continue;
        }
        const bool silhouette = isDarkLogo(logos[i]);
        for (int y = 0; y < placed->picture.height; ++y) {
            for (int x = 0; x < placed->picture.width; ++x) {
                const int tx = placed->x + x;
                const int ty = placed->y + y;
                if (tx < 0 || ty < 0 || tx >= image.width || ty >= image.height) {
                    continue;
                }
                const std::size_t from = pixelAt(x, y, placed->picture.width);
                const std::size_t to = pixelAt(tx, ty, image.width);
                const float alpha = static_cast<float>(placed->picture.pixels[from + 3]) / 255.0F;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float source = silhouette ? 1.0F : srgbByteToLinear(placed->picture.pixels[from + channel]);
                    const float below = srgbByteToLinear(image.pixels[to + channel]);
                    image.pixels[to + channel] = linearToSrgbByte(source * alpha + below * (1.0F - alpha));
                }
            }
        }
    }
    const std::vector<PlacedRun> runs = nameRuns(layout, font);
    if (!runs.empty()) {
        drawText(image, Rgb8{255, 255, 255}, runs);
    }
}

/// The neon: the name's letters and each logo's outline, a band as wide as the letters' strokes inside its shapes, as
/// the neon's tubes.
ImageData drawNeon(int width, int height, std::span<const ImageData> logos, const IdentitySignLayout& layout,
                   const FontFace& font) {
    ImageData strokes = makeSolid(width, height, glm::u8vec4(0, 0, 0, 255));
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    // The letters, and their strokes' half width.
    float halfStroke = 0.0F;
    const std::vector<PlacedRun> runs = nameRuns(layout, font);
    if (!runs.empty()) {
        drawText(strokes, Rgb8{255, 255, 255}, runs);
        std::vector<std::uint8_t> letters(count);
        for (std::size_t i = 0; i < count; ++i) {
            letters[i] = strokes.pixels[i * 4] >= 128 ? 1 : 0;
        }
        const std::vector<float> depth = insideDistance(letters, width, height);
        halfStroke = depth.empty() ? 0.0F : *std::max_element(depth.begin(), depth.end());
    }
    for (std::size_t i = 0; i < layout.logos.size() && i < logos.size(); ++i) {
        const std::optional<PlacedLogo> placed = placeLogo(logos[i], layout.logos[i]);
        if (!placed) {
            continue;
        }
        const int w = placed->picture.width;
        const int h = placed->picture.height;
        std::vector<std::uint8_t> inside(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (std::size_t p = 0; p < inside.size(); ++p) {
            inside[p] = placed->picture.pixels[p * 4 + 3] >= 128 ? 1 : 0;
        }
        // The tube along the outline: half a letter's stroke (a logo's shapes are larger than letters: a thinner tube
        // keeps them apart), else a twenty-eighth of the logo's height; never under 1.5 pixels.
        const float band = std::max(1.5F, halfStroke > 0.0F ? halfStroke : static_cast<float>(h) / 28.0F);
        const std::vector<float> depth = insideDistance(inside, w, h);
        // And along the edges between its colours (a dot, a bar, a word inside a badge): a tube of the same width,
        // half on each side of the edge.
        const std::vector<float> fromEdge = insideDistance(notColourEdge(placed->picture, inside), w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t p = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                      static_cast<std::size_t>(x);
                const float d = depth[p];
                const int tx = placed->x + x;
                const int ty = placed->y + y;
                const bool tube = d > 0.0F && (d <= band || fromEdge[p] <= 0.5F * band);
                if (!tube || tx < 0 || ty < 0 || tx >= width || ty >= height) {
                    continue;
                }
                const std::size_t to = pixelAt(tx, ty, width);
                strokes.pixels[to] = 255;
                strokes.pixels[to + 1] = 255;
                strokes.pixels[to + 2] = 255;
            }
        }
    }
    return makeNeonTubes(std::move(strokes));
}

} // namespace

bool isDarkLogo(const ImageData& logo) {
    if (!logo.isValid() || logo.channels != 4) {
        return false;
    }
    double weighted = 0.0;
    double alphas = 0.0;
    for (std::size_t at = 0; at + 3 < logo.pixels.size(); at += 4) {
        const double alpha = static_cast<double>(logo.pixels[at + 3]) / 255.0;
        const double luminance = 0.2126 * srgbByteToLinear(logo.pixels[at]) +
                                 0.7152 * srgbByteToLinear(logo.pixels[at + 1]) +
                                 0.0722 * srgbByteToLinear(logo.pixels[at + 2]);
        weighted += alpha * luminance;
        alphas += alpha;
    }
    return alphas > 0.0 && weighted / alphas < static_cast<double>(kDarkLogoLuminance);
}

IdentitySignLayout layoutIdentitySign(int width, int height, std::span<const std::pair<int, int>> logoSizes,
                                      std::string_view name, const FontFace& font) {
    IdentitySignLayout layout;
    if (width <= 0 || height <= 0) {
        return layout;
    }
    const float margin = static_cast<float>(std::max(1, std::min(width, height) / 12));
    const SignBox inner{margin, margin, static_cast<float>(width) - 2.0F * margin,
                        static_cast<float>(height) - 2.0F * margin};
    if (inner.width <= 0.0F || inner.height <= 0.0F) {
        return layout;
    }
    std::vector<float> aspects;
    for (const auto& [w, h] : logoSizes) {
        if (w > 0 && h > 0 && aspects.size() < kMaxSignLogos) {
            aspects.push_back(static_cast<float>(w) / static_cast<float>(h));
        }
    }
    std::u32string text = decodeUtf8(name);
    while (!text.empty() && text.back() == U' ') {
        text.pop_back();
    }
    while (!text.empty() && text.front() == U' ') {
        text.erase(text.begin());
    }
    if (aspects.empty() && text.empty()) {
        return layout;
    }
    if (text.empty()) {
        const float h = rowHeight(aspects, inner.width, inner.height);
        const float w = rowWidth(aspects, h);
        placeRow(layout, aspects, inner.x + (inner.width - w) / 2.0F, inner.y + (inner.height - h) / 2.0F, h);
        return layout;
    }
    if (aspects.empty()) {
        placeName(layout, fitName(text, inner.width, inner.height, font), font, inner, false);
        return layout;
    }
    layout.sideBySide = inner.width / inner.height >= kSideBySideAspect;
    if (layout.sideBySide) {
        const float h = rowHeight(aspects, inner.width * kSideBySideLogoShare, inner.height);
        const float logosWidth = rowWidth(aspects, h);
        const float gap = kSideBySideGap * inner.height;
        const SignBox nameBox{0.0F, inner.y, std::max(0.0F, inner.width - logosWidth - gap), inner.height};
        const FittedName fitted = fitName(text, nameBox.width, nameBox.height, font);
        const float group = logosWidth + gap + lineAdvance(fitted, font);
        const float left = inner.x + std::max(0.0F, (inner.width - group) / 2.0F);
        placeRow(layout, aspects, left, inner.y + (inner.height - h) / 2.0F, h);
        placeName(layout, fitted, font, SignBox{left + logosWidth + gap, nameBox.y, nameBox.width, nameBox.height},
                  true);
        return layout;
    }
    const SignBox logoBox{inner.x, inner.y, inner.width, inner.height * kStackedLogoShare};
    const float h = rowHeight(aspects, logoBox.width, logoBox.height);
    const float w = rowWidth(aspects, h);
    placeRow(layout, aspects, logoBox.x + (logoBox.width - w) / 2.0F, logoBox.y + (logoBox.height - h) / 2.0F, h);
    const SignBox nameBox{inner.x, inner.y + inner.height * kStackedNameTop, inner.width,
                          inner.height * kStackedNameShare};
    placeName(layout, fitName(text, nameBox.width, nameBox.height, font), font, nameBox, false);
    return layout;
}

ImageData makeIdentitySign(IdentitySignStyle style, int width, int height, std::span<const ImageData> logos,
                           std::string_view name, const FontFace& font) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    // The logos that can be drawn, with their sizes, in order.
    std::vector<ImageData> usable;
    std::vector<std::pair<int, int>> sizes;
    for (const ImageData& logo : logos) {
        if (logo.isValid() && logo.channels == 4 && usable.size() < kMaxSignLogos) {
            usable.push_back(logo);
            sizes.emplace_back(logo.width, logo.height);
        }
    }
    const IdentitySignLayout layout = layoutIdentitySign(width, height, sizes, name, font);
    if (style == IdentitySignStyle::Neon) {
        return drawNeon(width, height, usable, layout, font);
    }
    ImageData image = makeSolid(width, height, glm::u8vec4(0, 0, 0, 255));
    drawLit(image, usable, layout, font);
    return image;
}

} // namespace rv::assets
