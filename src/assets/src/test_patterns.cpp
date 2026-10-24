#include <rv/assets/test_patterns.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace rv::assets {
namespace {

using Rgba = glm::u8vec4;

constexpr Rgba fromHex(std::uint32_t hex) noexcept {
    return Rgba(static_cast<std::uint8_t>((hex >> 16) & 0xFFu), static_cast<std::uint8_t>((hex >> 8) & 0xFFu),
                static_cast<std::uint8_t>(hex & 0xFFu), std::uint8_t{255});
}

// Palette of style §3 (sRGB art-direction colors).
constexpr Rgba kWalnut = fromHex(0x493024);
constexpr Rgba kOak = fromHex(0x76513A);
constexpr Rgba kWall = fromHex(0x383735);
constexpr Rgba kGraphite = fromHex(0x252A2E);
constexpr Rgba kFabric = fromHex(0x683D42);
constexpr Rgba kTungsten = fromHex(0xFFC88A);
constexpr Rgba kColdAccent = fromHex(0x4C8BC9);
constexpr Rgba kArcadeAccent = fromHex(0xB34DCE);
constexpr Rgba kSignage = fromHex(0x6EAF93);
constexpr Rgba kUiNeutral = fromHex(0x171B22);
constexpr Rgba kUiText = fromHex(0xF4EFE6);

// Dark, warm backgrounds (the 70% of style §3.1) and bright accents that always contrast with them.
constexpr std::array<Rgba, 5> kCoverBackgrounds = {kWalnut, kOak, kWall, kGraphite, kFabric};
constexpr std::array<Rgba, 4> kCoverAccents = {kTungsten, kColdAccent, kArcadeAccent, kSignage};

/// The colors of a cover-like picture: its backgrounds, its accents, the border and title lettering, the title band.
struct CoverPalette {
    std::array<Rgba, 5> backgrounds;
    std::array<Rgba, 4> accents;
    Rgba border;
    Rgba band;
};

constexpr CoverPalette kCoverPalette{kCoverBackgrounds, kCoverAccents, kUiNeutral, kUiText};
/// The halls' prints (I6 step 2): the same palette muted, a framed print in a museum's warm gallery rather than a
/// shop's box art: the walnut, oak, wall and graphite grounds, bordeaux, the signage green and cold blue at about 60%
/// of their saturation and a softer tungsten, on an aged paper band.
constexpr CoverPalette kPrintPalette{{kWalnut, kOak, kWall, kGraphite, fromHex(0x5A3A2C)},
                                     {fromHex(0xD9B482), kFabric, fromHex(0x7AA291), fromHex(0x658AB0)},
                                     fromHex(0x1E1B19),
                                     fromHex(0xE3D7C1)};

ImageData blankImage(int width, int height) {
    if (width < 1 || height < 1 || width > kMaxImageDimension || height > kMaxImageDimension) {
        return {};
    }
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    return image;
}

void setPixel(ImageData& image, int x, int y, Rgba color) noexcept {
    std::uint8_t* pixel =
        image.pixels.data() +
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4u;
    pixel[0] = color.r;
    pixel[1] = color.g;
    pixel[2] = color.b;
    pixel[3] = color.a;
}

/// Fills [x0, x1) x [y0, y1), clipped to the image.
void fillRect(ImageData& image, int x0, int y0, int x1, int y1, Rgba color) noexcept {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, image.width);
    y1 = std::min(y1, image.height);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            setPixel(image, x, y, color);
        }
    }
}

/// Deterministic 32-bit generator (SplitMix32 finalizer over a Weyl sequence); identical on every platform.
class Rng {
public:
    explicit Rng(std::uint32_t seed) noexcept : m_state(seed) {}

    std::uint32_t next() noexcept {
        std::uint32_t z = (m_state += 0x9E3779B9u);
        z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
        z = (z ^ (z >> 13)) * 0xC2B2AE35u;
        return z ^ (z >> 16);
    }
    /// Uniform-enough integer in [0, bound).
    int below(int bound) noexcept { return static_cast<int>(next() % static_cast<std::uint32_t>(bound)); }

private:
    std::uint32_t m_state;
};

// ---- 5x7 bitmap font -----------------------------------------------------------------------------------------
// Each glyph is 7 rows, top to bottom; bit 4 of a row is the leftmost column.

struct Glyph {
    char character;
    std::array<std::uint8_t, 7> rows;
};

constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},  {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},  {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},  {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},  {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},  {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},  {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},  {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},  {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},  {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},  {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},  {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},  {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},  {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},  {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},  {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},  {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},  {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},  {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},  {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},  {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {';', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}},  {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},  {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}}, {'"', {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}},
    {'&', {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},  {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},  {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},  {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},  {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
};

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphAdvance = kGlyphWidth + 1; // in font pixels
constexpr int kLineGap = 2;                    // in font pixels
constexpr std::size_t kMaxCoverTitleLines = 3;
constexpr std::size_t kMaxSignLines = 2;

const Glyph* findGlyph(char c) noexcept {
    for (const Glyph& glyph : kFont) {
        if (glyph.character == c) {
            return &glyph;
        }
    }
    return nullptr;
}

/// Latin-1 Supplement letters folded to ASCII (U+00C0..U+00FF); nullptr for symbols.
const char* foldLatin1(std::uint32_t codePoint) noexcept {
    static constexpr const char* kFold[64] = {
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I",     "I",  // C0-CF
        "D", "N", "O", "O", "O", "O", "O",  "X", "O", "U", "U", "U", "U", "Y", nullptr, "SS", // D0-DF
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I",     "I",  // E0-EF
        "D", "N", "O", "O", "O", "O", "O",  "/", "O", "U", "U", "U", "U", "Y", nullptr, "Y",  // F0-FF
    };
    if (codePoint < 0xC0 || codePoint > 0xFF) {
        return nullptr;
    }
    return kFold[codePoint - 0xC0];
}

/// Decodes one UTF-8 sequence at `pos` (advancing it); malformed bytes decode as U+FFFD one byte at a time.
std::uint32_t nextCodePoint(std::string_view text, std::size_t& pos) noexcept {
    const auto byte = [&text](std::size_t i) {
        return static_cast<std::uint8_t>(text[i]);
    };
    const std::uint8_t lead = byte(pos);
    std::size_t length = 1;
    std::uint32_t codePoint = lead;
    if (lead < 0x80) {
        ++pos;
        return codePoint;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        codePoint = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        codePoint = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        codePoint = lead & 0x07u;
    } else {
        ++pos;
        return 0xFFFD;
    }
    if (pos + length > text.size()) {
        ++pos;
        return 0xFFFD;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(pos + i) & 0xC0u) != 0x80u) {
            ++pos;
            return 0xFFFD;
        }
        codePoint = (codePoint << 6) | (byte(pos + i) & 0x3Fu);
    }
    pos += length;
    return codePoint;
}

/// Title -> printable glyph string: uppercase, single spaces, unsupported characters as '?'.
std::string normalizeTitle(std::string_view title) {
    std::string text;
    const auto append = [&text](std::string_view piece) {
        for (const char c : piece) {
            if (c == ' ' && (text.empty() || text.back() == ' ')) {
                continue;
            }
            text += c;
        }
    };
    std::size_t pos = 0;
    while (pos < title.size()) {
        const std::uint32_t codePoint = nextCodePoint(title, pos);
        if (codePoint < 0x80) {
            char c = static_cast<char>(codePoint);
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
            if (c == '\t' || c == '\n' || c == '\r' || c == '_') {
                c = ' ';
            }
            append(std::string_view(findGlyph(c) != nullptr ? &c : "?", 1));
        } else if (const char* folded = foldLatin1(codePoint)) {
            append(folded);
        } else if (codePoint == 0x2018 || codePoint == 0x2019) {
            append("'");
        } else if (codePoint == 0x201C || codePoint == 0x201D) {
            append("\"");
        } else if (codePoint == 0x2013 || codePoint == 0x2014) {
            append("-");
        } else if (codePoint == 0x2026) {
            append("...");
        } else if (codePoint == 0x00A0) {
            append(" ");
        } else {
            append("?");
        }
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

/// Greedy word wrap to lines of at most `perLine` glyphs; words longer than a line are split.
std::vector<std::string> wrapWords(const std::string& text, std::size_t perLine) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string word = text.substr(start, end - start);
        start = end + 1;
        while (word.size() > perLine) {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
            lines.push_back(word.substr(0, perLine));
            word.erase(0, perLine);
        }
        if (word.empty()) {
            continue;
        }
        if (current.empty()) {
            current = word;
        } else if (current.size() + 1 + word.size() <= perLine) {
            current += ' ';
            current += word;
        } else {
            lines.push_back(current);
            current = word;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

int textBlockHeight(std::size_t lines, int scale) noexcept {
    const int count = static_cast<int>(lines);
    return count * kGlyphHeight * scale + (count - 1) * kLineGap * scale;
}

void drawText(ImageData& image, const std::vector<std::string>& lines, int scale, int x0, int y0, int x1, int y1,
              Rgba color) {
    const int blockHeight = textBlockHeight(lines.size(), scale);
    int y = y0 + (y1 - y0 - blockHeight) / 2;
    for (const std::string& line : lines) {
        const int lineWidth = static_cast<int>(line.size()) * kGlyphAdvance * scale - scale;
        int x = x0 + (x1 - x0 - lineWidth) / 2;
        for (const char c : line) {
            const Glyph* glyph = findGlyph(c);
            if (glyph != nullptr) {
                for (int row = 0; row < kGlyphHeight; ++row) {
                    for (int column = 0; column < kGlyphWidth; ++column) {
                        if ((glyph->rows[static_cast<std::size_t>(row)] >> (kGlyphWidth - 1 - column)) & 1u) {
                            fillRect(image, x + column * scale, y + row * scale, x + (column + 1) * scale,
                                     y + (row + 1) * scale, color);
                        }
                    }
                }
            }
            x += kGlyphAdvance * scale;
        }
        y += (kGlyphHeight + kLineGap) * scale;
    }
}

/// Glyphs of the longest space-separated word of `text`.
std::size_t longestWord(const std::string& text) noexcept {
    std::size_t longest = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        longest = std::max(longest, end - start);
        start = end + 1;
    }
    return longest;
}

/// Lays the title out in the box: the largest integer scale at which it wraps onto at most `maxLines` lines without
/// splitting a word; words are split only when no scale keeps them whole (a word longer than a line even at scale 1),
/// at the largest scale that fits; text that does not fit even at scale 1 is truncated.
void drawTitle(ImageData& image, std::string_view title, int x0, int y0, int x1, int y1, Rgba color,
               std::size_t maxLines) {
    const std::string text = normalizeTitle(title);
    const int availableWidth = x1 - x0;
    const int availableHeight = y1 - y0;
    if (text.empty() || availableWidth < kGlyphWidth || availableHeight < kGlyphHeight) {
        return;
    }
    const std::size_t wordLength = longestWord(text);
    std::vector<std::string> splitLines; // the largest layout that splits a word, used if no scale keeps words whole
    int splitScale = 0;
    for (int scale = availableHeight / kGlyphHeight; scale >= 1; --scale) {
        const int perLine = (availableWidth + scale) / (kGlyphAdvance * scale);
        if (perLine < 1) {
            continue;
        }
        std::vector<std::string> lines = wrapWords(text, static_cast<std::size_t>(perLine));
        if (lines.size() > maxLines || textBlockHeight(lines.size(), scale) > availableHeight) {
            continue;
        }
        if (wordLength <= static_cast<std::size_t>(perLine)) {
            drawText(image, lines, scale, x0, y0, x1, y1, color);
            return;
        }
        if (splitScale == 0) {
            splitScale = scale;
            splitLines = std::move(lines);
        }
    }
    if (splitScale > 0) {
        drawText(image, splitLines, splitScale, x0, y0, x1, y1, color);
        return;
    }
    // Does not fit even at scale 1: keep the lines that fit and mark the cut.
    const auto perLine = static_cast<std::size_t>((availableWidth + 1) / kGlyphAdvance);
    const auto fittingLines = std::min<std::size_t>(
        maxLines, static_cast<std::size_t>((availableHeight + kLineGap) / (kGlyphHeight + kLineGap)));
    std::vector<std::string> lines = wrapWords(text, perLine);
    if (lines.size() > fittingLines) {
        lines.resize(fittingLines);
        std::string& last = lines.back();
        constexpr std::string_view kEllipsis = "...";
        if (perLine > kEllipsis.size()) {
            if (last.size() + kEllipsis.size() > perLine) {
                last.resize(perLine - kEllipsis.size());
            }
            last += kEllipsis;
        }
    }
    drawText(image, lines, 1, x0, y0, x1, y1, color);
}

/// Abstract geometric backdrop, chosen and colored by the generator.
void drawCoverPattern(ImageData& image, Rng& rng, Rgba background, Rgba accent, Rgba secondary) {
    const int width = image.width;
    const int height = image.height;
    const int unit = std::max(2, std::min(width, height) / 8);
    const int pattern = rng.below(5);
    const int offset = rng.below(unit);
    if (pattern == 4) { // vertical bars with seeded widths
        std::vector<Rgba> columns(static_cast<std::size_t>(width), background);
        int x = 0;
        for (int bar = 0; x < width; ++bar) {
            const int barWidth = std::max(1, unit / 2) + rng.below(unit * 2);
            const Rgba color = bar % 3 == 0 ? accent : bar % 3 == 1 ? background : secondary;
            for (int i = 0; i < barWidth && x < width; ++i, ++x) {
                columns[static_cast<std::size_t>(x)] = color;
            }
        }
        for (int y = 0; y < height; ++y) {
            for (int column = 0; column < width; ++column) {
                setPixel(image, column, y, columns[static_cast<std::size_t>(column)]);
            }
        }
        return;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Rgba color = background;
            switch (pattern) {
            case 0: { // diagonal stripes
                const int band = ((x + y + offset) / std::max(1, unit / 2)) % 4;
                color = band == 0 ? accent : band == 2 ? secondary : background;
                break;
            }
            case 1: { // concentric frames
                const int edge = std::min({x, y, width - 1 - x, height - 1 - y});
                const int ring = (edge / std::max(1, unit / 2)) % 3;
                color = ring == 0 ? accent : ring == 1 ? background : secondary;
                break;
            }
            case 2: { // disc with horizontal slits over a ground of stripes
                const int cx = width / 2;
                const int cy = height * 2 / 5;
                const int radius = std::min(width, height) * 3 / 10;
                const long long dx = x - cx;
                const long long dy = y - cy;
                const bool inDisc = dx * dx + dy * dy <= static_cast<long long>(radius) * radius;
                const bool slit = y > cy && ((y - cy) / std::max(1, unit / 3)) % 2 == 1;
                if (inDisc && !slit) {
                    color = accent;
                } else if (y > cy + radius / 2 && ((y + offset) / std::max(1, unit / 2)) % 2 == 0) {
                    color = secondary;
                }
                break;
            }
            default: { // diamond lattice
                const int cellX = ((x + offset) % unit) - unit / 2;
                const int cellY = ((y + offset) % unit) - unit / 2;
                const int parity = (((x + offset) / unit) + ((y + offset) / unit)) % 2;
                if (std::abs(cellX) + std::abs(cellY) < unit / 2) {
                    color = parity == 0 ? accent : secondary;
                }
                break;
            }
            }
            setPixel(image, x, y, color);
        }
    }
}

ImageData coverWithPalette(int width, int height, std::string_view title, std::uint32_t seed,
                           const CoverPalette& palette) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    Rng rng(seed);
    const Rgba background = palette.backgrounds[static_cast<std::size_t>(rng.below(5))];
    const int first = rng.below(4);
    const int second = (first + 1 + rng.below(3)) % 4; // always different from the first accent
    drawCoverPattern(image, rng, background, palette.accents[static_cast<std::size_t>(first)],
                     palette.accents[static_cast<std::size_t>(second)]);

    const int border = std::max(1, std::min(width, height) / 24);
    fillRect(image, 0, 0, width, border, palette.border);
    fillRect(image, 0, height - border, width, height, palette.border);
    fillRect(image, 0, 0, border, height, palette.border);
    fillRect(image, width - border, 0, width, height, palette.border);

    const int innerWidth = width - 2 * border;
    const int innerHeight = height - 2 * border;
    if (innerWidth < 1 || innerHeight < 1) {
        return image;
    }
    // Title band from 58% to 88% of the inner height on portrait and square covers. Wide covers such as cartridge
    // labels, whose short height would leave a two-line title tiny, get a taller band, down to the same line: about the
    // height two lines of a dozen glyphs take across the band (11/40 of its width with the padding), at most 45%.
    const int baseTop = border + innerHeight * 58 / 100;
    const int bandBottom = std::max(baseTop + 1, border + innerHeight * 88 / 100);
    const int wideHeight = std::min(innerWidth * 11 / 40, innerHeight * 45 / 100);
    const int bandTop = std::min(baseTop, bandBottom - wideHeight);
    fillRect(image, border, bandTop, width - border, bandBottom, palette.band);
    const int paddingX = std::max(1, innerWidth / 24);
    const int paddingY = std::max(1, (bandBottom - bandTop) / 8);
    drawTitle(image, title, border + paddingX, bandTop + paddingY, width - border - paddingX, bandBottom - paddingY,
              palette.border, kMaxCoverTitleLines);
    return image;
}

} // namespace

ImageData makeColorBars(int width, int height) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    constexpr std::uint8_t kLevel = 191; // 75%
    constexpr Rgba kGrey(kLevel, kLevel, kLevel, 255);
    constexpr Rgba kYellow(kLevel, kLevel, 0, 255);
    constexpr Rgba kCyan(0, kLevel, kLevel, 255);
    constexpr Rgba kGreen(0, kLevel, 0, 255);
    constexpr Rgba kMagenta(kLevel, 0, kLevel, 255);
    constexpr Rgba kRed(kLevel, 0, 0, 255);
    constexpr Rgba kBlue(0, 0, kLevel, 255);
    constexpr Rgba kBlack(0, 0, 0, 255);
    constexpr std::array<Rgba, 7> kBars = {kGrey, kYellow, kCyan, kGreen, kMagenta, kRed, kBlue};
    constexpr std::array<Rgba, 7> kReverse = {kBlue, kBlack, kMagenta, kBlack, kCyan, kBlack, kGrey};

    const int barsEnd = height * 2 / 3;
    const int reverseEnd = height * 3 / 4;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto bar = static_cast<std::size_t>(x * 7 / width);
            if (y < barsEnd) {
                setPixel(image, x, y, kBars[bar]);
            } else if (y < reverseEnd) {
                setPixel(image, x, y, kReverse[bar]);
            } else {
                const auto level = static_cast<std::uint8_t>(width > 1 ? x * 255 / (width - 1) : 255);
                setPixel(image, x, y, Rgba(level, level, level, 255));
            }
        }
    }
    return image;
}

ImageData makeCheckerboard(int width, int height, int cells, glm::u8vec4 a, glm::u8vec4 b) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    const int count = std::clamp(cells, 1, std::min(width, height));
    for (int y = 0; y < height; ++y) {
        const int cellY = y * count / height;
        for (int x = 0; x < width; ++x) {
            const int cellX = x * count / width;
            setPixel(image, x, y, (cellX + cellY) % 2 == 0 ? a : b);
        }
    }
    return image;
}

ImageData makeCoverPlaceholder(int width, int height, std::string_view title, std::uint32_t seed) {
    return coverWithPalette(width, height, title, seed, kCoverPalette);
}

ImageData makePrintPlaceholder(int width, int height, std::string_view title, std::uint32_t seed) {
    return coverWithPalette(width, height, title, seed, kPrintPalette);
}

ImageData makeSignLettering(int width, int height, std::string_view text) {
    ImageData image = makeSolid(width, height, Rgba(0, 0, 0, 255));
    if (!image.isValid()) {
        return image;
    }
    const int margin = std::max(1, std::min(width, height) / 12);
    drawTitle(image, text, margin, margin, width - margin, height - margin, Rgba(255, 255, 255, 255), kMaxSignLines);
    return image;
}

ImageData makeSolid(int width, int height, glm::u8vec4 color) {
    ImageData image = blankImage(width, height);
    if (image.isValid()) {
        fillRect(image, 0, 0, width, height, color);
    }
    return image;
}

std::span<const std::string_view> patternNames() noexcept {
    static constexpr std::array<std::string_view, 5> kNames = {kPatternColorBars, kPatternChecker, kPatternCover,
                                                               kPatternSign, kPatternPrint};
    return kNames;
}

Expected<ImageData> makeNamedPattern(std::string_view name, int width, int height, std::string_view title,
                                     std::uint32_t seed) {
    if (width < 1 || height < 1 || width > kMaxImageDimension || height > kMaxImageDimension) {
        return makeError(ErrorCode::InvalidArgument,
                         "pattern size " + std::to_string(width) + "x" + std::to_string(height) + " is outside [1, " +
                             std::to_string(kMaxImageDimension) + "]",
                         std::string(name));
    }
    if (name == kPatternColorBars) {
        return makeColorBars(width, height);
    }
    if (name == kPatternChecker) {
        return makeCheckerboard(width, height, 8, kUiText, kWall);
    }
    if (name == kPatternCover) {
        return makeCoverPlaceholder(width, height, title, seed);
    }
    if (name == kPatternSign) {
        return makeSignLettering(width, height, title);
    }
    if (name == kPatternPrint) {
        return makePrintPlaceholder(width, height, title, seed);
    }
    return makeError(ErrorCode::NotFound, "unknown test pattern (expected color_bars, checker, cover, sign or print)",
                     std::string(name));
}

int patternLongSideFor(float meters) noexcept {
    if (!std::isfinite(meters) || meters <= 0.0f) {
        return kPatternLongSide;
    }
    const double pixels = std::round(static_cast<double>(meters) * static_cast<double>(kPatternPixelsPerMeter));
    return static_cast<int>(std::clamp(pixels, static_cast<double>(kMinPatternLongSide),
                                       static_cast<double>(kPatternLongSide)));
}

glm::ivec2 patternSizeForAspect(float aspect, int longSide) noexcept {
    const int side = std::clamp(longSide, 1, kMaxImageDimension);
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        return glm::ivec2(side);
    }
    // The shorter side in floating point first: aspects beyond the int range must not overflow.
    const auto shorter = [side](double ratio) {
        return static_cast<int>(
            std::clamp(std::lround(static_cast<double>(side) * ratio), 1L, static_cast<long>(side)));
    };
    if (aspect >= 1.0f) {
        return glm::ivec2(side, shorter(1.0 / static_cast<double>(aspect)));
    }
    return glm::ivec2(shorter(static_cast<double>(aspect)), side);
}

std::string patternKey(std::string_view name, std::string_view title, std::uint32_t seed) {
    std::string key = "pattern:";
    key += name;
    key += ':';
    key += title;
    key += ':';
    key += std::to_string(seed);
    return key;
}

} // namespace rv::assets
