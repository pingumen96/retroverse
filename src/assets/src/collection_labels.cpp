#include <rv/assets/collection_labels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <rv/core/random.hpp>

namespace rv::assets {
namespace {

constexpr char32_t kEllipsis = 0x2026;

/// The spine's sizes as shares of its thickness, and its end margins as a share of its length.
constexpr float kSpineOneLineMax = 0.62F;
constexpr float kSpineOneLineMin = 0.45F;
constexpr float kSpineTwoLinesMax = 0.45F;
constexpr float kSpineTwoLinesMin = 0.34F;
constexpr float kSpineEndMargin = 0.05F;
constexpr float kSpineLinePitch = 0.92F; ///< the distance between the two baselines, in pixel heights

/// The front label's geometry at 256 x 188 (scaled with the layer's width).
constexpr float kFrontReference = 256.0F;
constexpr float kFrontFrameInset = 7.0F;
constexpr float kFrontFrameStroke = 3.0F;
constexpr float kFrontTextLeft = 20.0F;
constexpr float kFrontTextRight = 236.0F;
constexpr float kFrontTitleTop = 44.0F;
constexpr float kFrontTitleBottom = 146.0F;
constexpr float kFrontOneLineMax = 34.0F;
constexpr float kFrontOneLineMin = 20.0F;
constexpr float kFrontLinesMax = 26.0F;
constexpr float kFrontLinesMin = 14.0F;
constexpr float kFrontLinePitch = 1.0F;
constexpr int kFrontMaxLines = 3;
constexpr float kFrontFewerLinesShare = 0.85F;
constexpr float kFrontDetailSize = 14.0F;
constexpr float kFrontDetailBaseline = 172.0F;
constexpr float kFrontDetailGap = 10.0F;

bool isSpace(char32_t c) noexcept {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0x00A0 || c == 0x3000 || c == 0x2002 ||
           c == 0x2003 || c == 0x2009;
}

/// Runs of white space become one space; leading and trailing white space is dropped.
std::u32string collapseWhitespace(std::u32string_view text) {
    std::u32string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char32_t c : text) {
        if (isSpace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(U' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

bool hasDrawableCharacter(const FontFace& font, std::u32string_view text) {
    return std::any_of(text.begin(), text.end(), [&](char32_t c) { return !isSpace(c) && font.hasGlyph(c); });
}

float widthAt(const FontFace& font, std::u32string_view text, float size) {
    return font.layout(text, size).width;
}

/// The largest size up to `maximum` at which every line fits in `available` (widths scale with the size).
float fittingSize(const FontFace& font, const std::vector<std::u32string>& lines, float available, float maximum) {
    float widest = 0.0F;
    for (const std::u32string& line : lines) {
        widest = std::max(widest, widthAt(font, line, 1.0F));
    }
    if (widest <= 0.0F) {
        return maximum;
    }
    return std::min(maximum, available / widest);
}

std::u32string trimmed(std::u32string_view text) {
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && isSpace(text[first])) {
        ++first;
    }
    while (last > first && isSpace(text[last - 1])) {
        --last;
    }
    return std::u32string(text.substr(first, last - first));
}

/// `text` cut at its end, with an ellipsis, to fit in `available` at `size`.
std::u32string ellipsize(const FontFace& font, std::u32string_view text, float size, float available) {
    if (widthAt(font, text, size) <= available) {
        return std::u32string(text);
    }
    const std::u32string mark = font.hasGlyph(kEllipsis) ? std::u32string(1, kEllipsis) : std::u32string(U"...");
    std::u32string prefix(text);
    while (!prefix.empty()) {
        prefix.pop_back();
        while (!prefix.empty() && (isSpace(prefix.back()) || prefix.back() == U',' || prefix.back() == U':' ||
                                   prefix.back() == U';' || prefix.back() == U'-' || prefix.back() == U'.')) {
            prefix.pop_back();
        }
        if (widthAt(font, prefix + mark, size) <= available) {
            return prefix + mark;
        }
    }
    return mark;
}

struct SeparatorSplit {
    std::u32string first;
    std::u32string second;
    std::size_t position = 0;
};

/// Every split at " : ", ": " or " - " (the colon kept on the first line, the dash dropped), in text order.
std::vector<SeparatorSplit> separatorSplits(const std::u32string& text) {
    std::vector<SeparatorSplit> splits;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        SeparatorSplit split;
        if (i + 2 < text.size() && text[i] == U' ' && text[i + 1] == U':' && text[i + 2] == U' ') {
            split = {text.substr(0, i) + U":", text.substr(i + 3), i};
        } else if (text[i] == U':' && text[i + 1] == U' ' && (i == 0 || text[i - 1] != U' ')) {
            split = {text.substr(0, i + 1), text.substr(i + 2), i};
        } else if (i + 2 < text.size() && text[i] == U' ' && text[i + 1] == U'-' && text[i + 2] == U' ') {
            split = {text.substr(0, i), text.substr(i + 3), i};
        } else {
            continue;
        }
        split.first = trimmed(split.first);
        split.second = trimmed(split.second);
        if (!split.first.empty() && !split.second.empty()) {
            splits.push_back(std::move(split));
        }
    }
    return splits;
}

std::vector<std::u32string> words(const std::u32string& text) {
    std::vector<std::u32string> out;
    std::u32string current;
    for (const char32_t c : text) {
        if (c == U' ') {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

std::u32string joined(const std::vector<std::u32string>& parts, std::size_t first, std::size_t last) {
    std::u32string out;
    for (std::size_t i = first; i < last; ++i) {
        if (!out.empty()) {
            out.push_back(U' ');
        }
        out += parts[i];
    }
    return out;
}

/// Greedy wrapping of `text` at `size`: as many words per line as fit. A word wider than the line stands alone (and
/// is reported by `overflow`).
std::vector<std::u32string> wrapGreedy(const FontFace& font, const std::u32string& text, float size, float available,
                                       bool& overflow) {
    std::vector<std::u32string> lines;
    std::u32string line;
    for (std::u32string& word : words(text)) {
        std::u32string candidate = line.empty() ? word : line + U" " + word;
        if (line.empty() || widthAt(font, candidate, size) <= available) {
            line = std::move(candidate);
        } else {
            lines.push_back(std::move(line));
            line = std::move(word);
        }
        if (widthAt(font, line, size) > available) {
            overflow = true;
        }
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

/// `text` broken between words into exactly `count` lines whose widest is the narrowest possible (the earliest such
/// breaks on a tie); fewer lines when it has fewer words. Widths are measured at size 1 (they scale with the size).
std::vector<std::u32string> balancedLines(const FontFace& font, const std::u32string& text, int count) {
    const std::vector<std::u32string> parts = words(text);
    const std::size_t n = parts.size();
    if (count <= 1 || n <= 1) {
        return {text};
    }
    if (static_cast<std::size_t>(count) >= n) {
        return parts;
    }
    // The width of the words [i, j) on one line, measured once each.
    std::vector<float> widths(n * (n + 1), -1.0F);
    const auto width = [&](std::size_t i, std::size_t j) {
        float& cached = widths[i * (n + 1) + j];
        if (cached < 0.0F) {
            cached = widthAt(font, joined(parts, i, j), 1.0F);
        }
        return cached;
    };
    std::vector<std::size_t> bestBreaks;
    float bestWidest = 0.0F;
    std::vector<std::size_t> breaks(static_cast<std::size_t>(count - 1));
    // Every choice of count - 1 increasing break positions in [1, n - 1] (count is at most 3).
    const auto visit = [&](auto&& self, std::size_t index, std::size_t from) -> void {
        if (index == breaks.size()) {
            float widest = 0.0F;
            std::size_t begin = 0;
            for (std::size_t b = 0; b <= breaks.size(); ++b) {
                const std::size_t end = b < breaks.size() ? breaks[b] : n;
                widest = std::max(widest, width(begin, end));
                begin = end;
            }
            if (bestBreaks.empty() || widest < bestWidest - 1e-6F) {
                bestWidest = widest;
                bestBreaks = breaks;
            }
            return;
        }
        const std::size_t remaining = breaks.size() - index - 1;
        for (std::size_t at = from; at + remaining < n; ++at) {
            breaks[index] = at;
            self(self, index + 1, at + 1);
        }
    };
    visit(visit, 0, 1);
    std::vector<std::u32string> lines;
    std::size_t begin = 0;
    for (std::size_t b = 0; b <= bestBreaks.size(); ++b) {
        const std::size_t end = b < bestBreaks.size() ? bestBreaks[b] : n;
        lines.push_back(joined(parts, begin, end));
        begin = end;
    }
    return lines;
}

TitleLayout finish(const FontFace& font, std::vector<std::u32string> lines, float size, LineSplit split,
                   bool ellipsized, bool usedFallback) {
    TitleLayout layout;
    layout.pixelHeight = size;
    layout.split = split;
    layout.ellipsized = ellipsized;
    layout.usedFallbackTitle = usedFallback;
    for (const std::u32string& line : lines) {
        layout.replacementGlyphs += font.layout(line, size).replacementGlyphs;
        layout.lines.push_back(encodeUtf8(line));
    }
    return layout;
}

TitleLayout layoutTwoLines(const FontFace& font, const std::u32string& text, float available, float oneMax,
                           float oneMin, float twoMax, float twoMin, bool usedFallback) {
    if (text.empty()) {
        return finish(font, {}, oneMax, LineSplit::OneLine, false, usedFallback);
    }
    const float one = fittingSize(font, {text}, available, oneMax);
    if (one >= oneMin) {
        return finish(font, {text}, one, LineSplit::OneLine, false, usedFallback);
    }
    // A separator first: the one whose lines fit at the larger size.
    const std::vector<SeparatorSplit> separators = separatorSplits(text);
    float best = 0.0F;
    std::vector<std::u32string> bestLines;
    for (const SeparatorSplit& split : separators) {
        std::vector<std::u32string> lines = {split.first, split.second};
        const float size = fittingSize(font, lines, available, twoMax);
        if (size > best) {
            best = size;
            bestLines = std::move(lines);
        }
    }
    if (best >= twoMin) {
        return finish(font, bestLines, best, LineSplit::Separator, false, usedFallback);
    }
    // Then between words.
    const std::vector<std::u32string> parts = words(text);
    best = 0.0F;
    bestLines.clear();
    for (std::size_t i = 1; i < parts.size(); ++i) {
        std::vector<std::u32string> lines = {joined(parts, 0, i), joined(parts, i, parts.size())};
        const float size = fittingSize(font, lines, available, twoMax);
        if (size > best) {
            best = size;
            bestLines = std::move(lines);
        }
    }
    if (best >= twoMin) {
        return finish(font, bestLines, best, LineSplit::Words, false, usedFallback);
    }
    // Still too long at the minimum size: an ellipsis. Unbreakable text stays on one line at the one-line minimum.
    if (parts.size() < 2) {
        return finish(font, {ellipsize(font, text, oneMin, available)}, oneMin, LineSplit::OneLine, true,
                      usedFallback);
    }
    // The separator's first part when it fits, else as many words as fit; the rest on the second line, cut.
    for (const SeparatorSplit& split : separators) {
        if (widthAt(font, split.first, twoMin) <= available) {
            return finish(font, {split.first, ellipsize(font, split.second, twoMin, available)}, twoMin,
                          LineSplit::Separator, true, usedFallback);
        }
    }
    std::size_t taken = 0;
    while (taken + 1 < parts.size() && widthAt(font, joined(parts, 0, taken + 1), twoMin) <= available) {
        ++taken;
    }
    if (taken == 0) {
        return finish(font, {ellipsize(font, text, oneMin, available)}, oneMin, LineSplit::OneLine, true,
                      usedFallback);
    }
    return finish(font, {joined(parts, 0, taken), ellipsize(font, joined(parts, taken, parts.size()), twoMin, available)},
                  twoMin, LineSplit::Words, true, usedFallback);
}

float frontScale(TextureFormat format) noexcept {
    return static_cast<float>(layerSize(TextureClass::Front, format).width) / kFrontReference;
}

LinearRgb mixLinear(Rgb8 a, Rgb8 b, float t) noexcept {
    const LinearRgb la = toLinear(a);
    const LinearRgb lb = toLinear(b);
    return {la.r + (lb.r - la.r) * t, la.g + (lb.g - la.g) * t, la.b + (lb.b - la.b) * t};
}

ImageData filledImage(int width, int height, Rgb8 colour) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i + 0] = colour.r;
        image.pixels[i + 1] = colour.g;
        image.pixels[i + 2] = colour.b;
        image.pixels[i + 3] = 255;
    }
    return image;
}

/// Fills the pixels of [x0, x1) x [y0, y1), the bounds rounded to whole pixels and clamped to the image.
void fillRect(ImageData& image, float x0, float y0, float x1, float y1, Rgb8 colour) {
    const int left = std::clamp(static_cast<int>(std::lround(x0)), 0, image.width);
    const int right = std::clamp(static_cast<int>(std::lround(x1)), 0, image.width);
    const int top = std::clamp(static_cast<int>(std::lround(y0)), 0, image.height);
    const int bottom = std::clamp(static_cast<int>(std::lround(y1)), 0, image.height);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            std::uint8_t* pixel = image.pixels.data() +
                                  (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                                   static_cast<std::size_t>(x)) *
                                      4;
            pixel[0] = colour.r;
            pixel[1] = colour.g;
            pixel[2] = colour.b;
            pixel[3] = 255;
        }
    }
}

template <class Picture>
Expected<GeneratedLayer> encodeGenerated(Expected<Picture> drawn, TextureClass textureClass, TextureFormat format) {
    if (!drawn) {
        return propagate(drawn.error());
    }
    auto texture = makeLayerTexture(drawn->picture, textureClass, format, drawn->colours.background);
    if (!texture) {
        return propagate(texture.error());
    }
    GeneratedLayer layer;
    layer.texture = std::move(*texture);
    layer.layout = std::move(drawn->layout);
    layer.colours = drawn->colours;
    return layer;
}

} // namespace

double relativeLuminance(Rgb8 colour) noexcept {
    const LinearRgb linear = toLinear(colour);
    return 0.2126 * linear.r + 0.7152 * linear.g + 0.0722 * linear.b;
}

double contrastRatio(Rgb8 a, Rgb8 b) noexcept {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

LabelColours labelColours(Rgb8 base) noexcept {
    const bool lightText = contrastRatio(base, kLabelTextLight) >= contrastRatio(base, kLabelTextDark);
    const Rgb8 text = lightText ? kLabelTextLight : kLabelTextDark;
    const Rgb8 towards = lightText ? Rgb8{0, 0, 0} : Rgb8{255, 255, 255};
    Rgb8 background = base;
    for (int step = 1; step <= 50 && contrastRatio(background, text) < kLabelContrastTarget; ++step) {
        background = toSrgb8(mixLinear(base, towards, static_cast<float>(step) * 0.02F));
    }
    return {background, text, contrastRatio(background, text)};
}

Rgb8 paletteColour(std::uint64_t seed, std::span<const Rgb8> palette) noexcept {
    if (palette.empty()) {
        return kDefaultLabelPalette[0];
    }
    core::Random random(seed);
    return palette[static_cast<std::size_t>(random.below(palette.size()))];
}

std::string_view toString(LineSplit split) noexcept {
    switch (split) {
    case LineSplit::OneLine:
        return "one-line";
    case LineSplit::Separator:
        return "separator";
    case LineSplit::Words:
        return "words";
    }
    return "unknown";
}

std::string_view toString(FrontOrigin origin) noexcept {
    switch (origin) {
    case FrontOrigin::Cover:
        return "cover";
    case FrontOrigin::NoCover:
        return "no-cover";
    case FrontOrigin::CoverFailed:
        return "cover-failed";
    }
    return "unknown";
}

std::u32string chooseTitleText(const FontFace& font, std::string_view title, std::string_view fallbackTitle,
                               bool* usedFallback) {
    std::u32string primary = collapseWhitespace(decodeUtf8(title));
    bool fallback = false;
    if (!hasDrawableCharacter(font, primary)) {
        std::u32string secondary = collapseWhitespace(decodeUtf8(fallbackTitle));
        if (hasDrawableCharacter(font, secondary) || primary.empty()) {
            primary = std::move(secondary);
            fallback = true;
        }
    }
    if (usedFallback != nullptr) {
        *usedFallback = fallback;
    }
    return primary;
}

TitleLayout layoutSpineTitle(const FontFace& font, std::string_view title, std::string_view fallbackTitle,
                             TextureFormat format) {
    const LayerSize size = layerSize(TextureClass::Spine, format);
    const auto thickness = static_cast<float>(size.width);
    const auto length = static_cast<float>(size.height);
    const float available = length - 2.0F * std::round(length * kSpineEndMargin);
    bool usedFallback = false;
    const std::u32string text = chooseTitleText(font, title, fallbackTitle, &usedFallback);
    return layoutTwoLines(font, text, available, kSpineOneLineMax * thickness, kSpineOneLineMin * thickness,
                          kSpineTwoLinesMax * thickness, kSpineTwoLinesMin * thickness, usedFallback);
}

TitleLayout layoutFrontTitle(const FontFace& font, std::string_view title, std::string_view fallbackTitle,
                             TextureFormat format) {
    const float k = frontScale(format);
    const float available = (kFrontTextRight - kFrontTextLeft) * k;
    bool usedFallback = false;
    const std::u32string text = chooseTitleText(font, title, fallbackTitle, &usedFallback);
    if (text.empty()) {
        return finish(font, {}, kFrontOneLineMax * k, LineSplit::OneLine, false, usedFallback);
    }
    const float one = fittingSize(font, {text}, available, kFrontOneLineMax * k);
    if (one >= kFrontOneLineMin * k) {
        return finish(font, {text}, one, LineSplit::OneLine, false, usedFallback);
    }
    // A separator forces the first break; then each part is broken between words into the lines that keep the widest
    // one narrowest, for two and three lines in all. The largest size wins, unless fewer lines reach at least
    // kFrontFewerLinesShare of it (a third line must buy a real gain in size).
    const std::vector<SeparatorSplit> separators = separatorSplits(text);
    std::vector<std::u32string> parts;
    if (separators.empty()) {
        parts.push_back(text);
    } else {
        parts = {separators.front().first, separators.front().second};
    }
    const LineSplit split = separators.empty() ? LineSplit::Words : LineSplit::Separator;
    // The best layout for each line count.
    std::array<float, kFrontMaxLines + 1> sizes{};
    std::array<std::vector<std::u32string>, kFrontMaxLines + 1> layouts;
    const auto consider = [&](std::vector<std::u32string> lines, std::size_t total) {
        if (lines.size() != total) {
            return; // a part with fewer words than lines
        }
        const float size = fittingSize(font, lines, available, kFrontLinesMax * k);
        if (size > sizes[total] + 1e-4F) {
            sizes[total] = size;
            layouts[total] = std::move(lines);
        }
    };
    for (int total = 2; total <= kFrontMaxLines; ++total) {
        const auto count = static_cast<std::size_t>(total);
        if (parts.size() == 1) {
            consider(balancedLines(font, parts[0], total), count);
            continue;
        }
        for (int first = 1; first < total; ++first) {
            std::vector<std::u32string> lines = balancedLines(font, parts[0], first);
            std::vector<std::u32string> rest = balancedLines(font, parts[1], total - first);
            lines.insert(lines.end(), rest.begin(), rest.end());
            consider(std::move(lines), count);
        }
    }
    const float largest = *std::max_element(sizes.begin(), sizes.end());
    float best = 0.0F;
    std::vector<std::u32string> bestLines;
    for (std::size_t total = 2; total <= static_cast<std::size_t>(kFrontMaxLines); ++total) {
        if (sizes[total] > 0.0F && sizes[total] >= kFrontFewerLinesShare * largest - 1e-4F) {
            best = sizes[total];
            bestLines = layouts[total];
            break;
        }
    }
    if (best >= kFrontLinesMin * k) {
        return finish(font, std::move(bestLines), best, split, false, usedFallback);
    }
    const auto wrapAll = [&](float size, bool& overflow) {
        std::vector<std::u32string> lines;
        for (const std::u32string& part : parts) {
            for (std::u32string& line : wrapGreedy(font, part, size, available, overflow)) {
                lines.push_back(std::move(line));
            }
        }
        return lines;
    };
    // At the minimum size: three lines, the last one holding the rest, cut; a word wider than a line cut too.
    const float size = kFrontLinesMin * k;
    bool overflow = false;
    std::vector<std::u32string> lines = wrapAll(size, overflow);
    if (lines.size() > static_cast<std::size_t>(kFrontMaxLines)) {
        std::u32string rest;
        for (std::size_t i = kFrontMaxLines - 1; i < lines.size(); ++i) {
            rest += (rest.empty() ? U"" : U" ") + lines[i];
        }
        lines.resize(kFrontMaxLines - 1);
        lines.push_back(std::move(rest));
    }
    for (std::u32string& line : lines) {
        line = ellipsize(font, line, size, available);
    }
    return finish(font, std::move(lines), size, split, true, usedFallback);
}

Rgb8 spineBaseColour(const SpineRequest& request) noexcept {
    return request.coverColour ? *request.coverColour : paletteColour(request.seed);
}

Expected<GeneratedPicture> drawSpine(const LabelFonts& fonts, const SpineRequest& request, TextureFormat format) {
    if (fonts.title == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "a spine needs the title font", "spine");
    }
    const FontFace& font = *fonts.title;
    const LayerSize size = layerSize(TextureClass::Spine, format);
    GeneratedPicture result;
    result.colours = labelColours(spineBaseColour(request));
    result.layout = layoutSpineTitle(font, request.title, request.fallbackTitle, format);

    // Set horizontally on a canvas as long as the spine and as wide as it is thick, then turned clockwise.
    const int length = size.height;
    const int thickness = size.width;
    ImageData canvas = filledImage(length, thickness, result.colours.background);
    const float pixelHeight = result.layout.pixelHeight;
    const float cap = font.capHeight(pixelHeight);
    std::vector<PlacedRun> runs;
    const std::size_t lineCount = result.layout.lines.size();
    const float pitch = kSpineLinePitch * pixelHeight;
    const float block = cap + pitch * static_cast<float>(lineCount > 0 ? lineCount - 1 : 0);
    float baseline = (static_cast<float>(thickness) - block) / 2.0F + cap;
    for (const std::string& line : result.layout.lines) {
        PlacedRun run;
        run.font = &font;
        run.text = decodeUtf8(line);
        run.pixelHeight = pixelHeight;
        run.x = (static_cast<float>(length) - widthAt(font, run.text, pixelHeight)) / 2.0F;
        run.baseline = baseline;
        runs.push_back(std::move(run));
        baseline += pitch;
    }
    drawText(canvas, result.colours.text, runs);

    // Turned 90 degrees clockwise: the canvas's (u, v) lands at (thickness - 1 - v, u).
    result.picture = filledImage(size.width, size.height, result.colours.background);
    for (int u = 0; u < length; ++u) {
        for (int v = 0; v < thickness; ++v) {
            const std::uint8_t* in =
                canvas.pixels.data() +
                (static_cast<std::size_t>(v) * static_cast<std::size_t>(length) + static_cast<std::size_t>(u)) * 4;
            std::uint8_t* out = result.picture.pixels.data() +
                                (static_cast<std::size_t>(u) * static_cast<std::size_t>(thickness) +
                                 static_cast<std::size_t>(thickness - 1 - v)) *
                                    4;
            std::copy(in, in + 4, out);
        }
    }
    return result;
}

Expected<GeneratedLayer> buildSpineLayer(const LabelFonts& fonts, const SpineRequest& request, TextureFormat format) {
    return encodeGenerated(drawSpine(fonts, request, format), TextureClass::Spine, format);
}

Expected<GeneratedPicture> drawFrontLabel(const LabelFonts& fonts, const FrontLabelRequest& request,
                                          TextureFormat format) {
    if (fonts.title == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "a generated label needs the title font", "front label");
    }
    const FontFace& titleFont = *fonts.title;
    const FontFace& detailFont = fonts.detail != nullptr ? *fonts.detail : titleFont;
    const LayerSize size = layerSize(TextureClass::Front, format);
    const float k = frontScale(format);
    GeneratedPicture result;
    result.colours = labelColours(paletteColour(request.seed));
    result.layout = layoutFrontTitle(titleFont, request.title, request.fallbackTitle, format);
    result.picture = filledImage(size.width, size.height, result.colours.background);
    const Rgb8 background = result.colours.background;
    const Rgb8 text = result.colours.text;

    // The frame and the stripes, in tints of the text colour.
    const Rgb8 frame = toSrgb8(mixLinear(background, text, 0.45F));
    const auto w = static_cast<float>(size.width);
    const auto h = static_cast<float>(size.height);
    const float inset = kFrontFrameInset * k;
    const float stroke = kFrontFrameStroke * k;
    fillRect(result.picture, inset, inset, w - inset, inset + stroke, frame);
    fillRect(result.picture, inset, h - inset - stroke, w - inset, h - inset, frame);
    fillRect(result.picture, inset, inset, inset + stroke, h - inset, frame);
    fillRect(result.picture, w - inset - stroke, inset, w - inset, h - inset, frame);
    const Rgb8 stripe = toSrgb8(mixLinear(background, text, 0.65F));
    fillRect(result.picture, 18.0F * k, 20.0F * k, w - 18.0F * k, 24.0F * k, stripe);
    fillRect(result.picture, 18.0F * k, 28.0F * k, w - 18.0F * k, 32.0F * k, stripe);

    std::vector<PlacedRun> runs;
    const float left = kFrontTextLeft * k;
    const float right = kFrontTextRight * k;
    const float pixelHeight = result.layout.pixelHeight;
    const float cap = titleFont.capHeight(pixelHeight);
    const std::size_t lineCount = result.layout.lines.size();
    const float pitch = kFrontLinePitch * pixelHeight;
    const float block = cap + pitch * static_cast<float>(lineCount > 0 ? lineCount - 1 : 0);
    float baseline = (kFrontTitleTop + kFrontTitleBottom) * k / 2.0F - block / 2.0F + cap;
    for (const std::string& line : result.layout.lines) {
        PlacedRun run;
        run.font = &titleFont;
        run.text = decodeUtf8(line);
        run.pixelHeight = pixelHeight;
        run.x = left + (right - left - widthAt(titleFont, run.text, pixelHeight)) / 2.0F;
        run.baseline = baseline;
        runs.push_back(std::move(run));
        baseline += pitch;
    }

    // The system's name at the bottom left, the year at the bottom right.
    const float detailSize = kFrontDetailSize * k;
    const float detailBaseline = kFrontDetailBaseline * k;
    const std::u32string year = collapseWhitespace(decodeUtf8(request.year));
    float yearWidth = 0.0F;
    if (!year.empty()) {
        yearWidth = widthAt(detailFont, year, detailSize);
        runs.push_back({&detailFont, year, detailSize, right - yearWidth, detailBaseline});
    }
    const std::u32string system = collapseWhitespace(decodeUtf8(request.systemName));
    if (!system.empty()) {
        const float room = right - left - (year.empty() ? 0.0F : yearWidth + kFrontDetailGap * k);
        runs.push_back({&detailFont, ellipsize(detailFont, system, detailSize, room), detailSize, left,
                        detailBaseline});
    }
    drawText(result.picture, text, runs);
    return result;
}

Expected<GeneratedLayer> buildFrontLabelLayer(const LabelFonts& fonts, const FrontLabelRequest& request,
                                              TextureFormat format) {
    return encodeGenerated(drawFrontLabel(fonts, request, format), TextureClass::Front, format);
}

Expected<FrontLayer> buildFrontLayer(std::optional<std::span<const std::byte>> coverBytes,
                                     const FrontLabelRequest& label, const LabelFonts& fonts, TextureFormat format,
                                     std::string_view context, const FitOptions& options) {
    FrontLayer result;
    if (coverBytes) {
        auto cover = buildCoverLayer(*coverBytes, format, context, options);
        if (cover) {
            result.texture = std::move(cover->texture);
            result.origin = FrontOrigin::Cover;
            return result;
        }
        result.coverError = cover.error();
        result.origin = FrontOrigin::CoverFailed;
    } else {
        result.origin = FrontOrigin::NoCover;
    }
    auto generated = buildFrontLabelLayer(fonts, label, format);
    if (!generated) {
        return propagate(generated.error());
    }
    result.texture = std::move(generated->texture);
    return result;
}

} // namespace rv::assets
