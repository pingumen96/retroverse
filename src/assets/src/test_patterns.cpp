#include <rv/assets/test_patterns.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/vec2.hpp>

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

namespace {

/// A coverage map of `width` x `height` floats, blurred in place by `passes` box passes of `radius` each way (about a
/// Gaussian of sigma ~ radius for three passes).
void boxBlur(std::vector<float>& values, int width, int height, int radius, int passes) {
    std::vector<float> scratch(values.size());
    const auto pass = [&](bool horizontal) {
        const int along = horizontal ? width : height;
        const int across = horizontal ? height : width;
        for (int line = 0; line < across; ++line) {
            const auto at = [&](int i) {
                const int x = horizontal ? i : line;
                const int y = horizontal ? line : i;
                return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            };
            float sum = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                sum += values[at(std::clamp(i, 0, along - 1))];
            }
            for (int i = 0; i < along; ++i) {
                scratch[at(i)] = sum / static_cast<float>(2 * radius + 1);
                sum += values[at(std::min(i + radius + 1, along - 1))] - values[at(std::max(i - radius, 0))];
            }
        }
        values.swap(scratch);
    };
    for (int i = 0; i < passes; ++i) {
        pass(true);
        pass(false);
    }
}

/// The distance (pixels, a 3-4 chamfer) of every pixel of `inside` to the nearest pixel outside it; 0 outside.
std::vector<float> insideDistance(const std::vector<bool>& inside, int width, int height) {
    constexpr float kFar = 1.0e6f;
    std::vector<float> d(inside.size());
    for (std::size_t i = 0; i < inside.size(); ++i) {
        d[i] = inside[i] ? kFar : 0.0f;
    }
    const auto at = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    };
    const auto value = [&](int x, int y) { return x < 0 || y < 0 || x >= width || y >= height ? 0.0f : d[at(x, y)]; };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float& here = d[at(x, y)];
            here = std::min({here, value(x - 1, y) + 3.0f, value(x, y - 1) + 3.0f, value(x - 1, y - 1) + 4.0f,
                             value(x + 1, y - 1) + 4.0f});
        }
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            float& here = d[at(x, y)];
            here = std::min({here, value(x + 1, y) + 3.0f, value(x, y + 1) + 3.0f, value(x + 1, y + 1) + 4.0f,
                             value(x - 1, y + 1) + 4.0f});
        }
    }
    for (float& distance : d) {
        distance /= 3.0f;
    }
    return d;
}

} // namespace

ImageData makeNeonLettering(int width, int height, std::string_view text) {
    ImageData image = makeSignLettering(width, height, text);
    if (!image.isValid()) {
        return image;
    }
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<bool> stroke(count);
    for (std::size_t i = 0; i < count; ++i) {
        stroke[i] = image.pixels[i * 4] >= 128;
    }
    // The stroke's width: twice the deepest point inside the letters (the pixel font's strokes are all alike).
    const std::vector<float> depth = insideDistance(stroke, width, height);
    const float halfWidth = std::max(1.0f, *std::max_element(depth.begin(), depth.end()));
    const int strokeWidth = std::max(1, static_cast<int>(std::lround(2.0f * halfWidth)));
    // Rounded tubes: the strokes blurred over a third of their width and cut again, so that corners and the strokes'
    // ends come round like bent glass: a stroke's pixel stays glass unless it is an outer corner (below 0.40), another
    // pixel becomes glass only well inside an inner corner (0.62), so the pixel font's diagonal joints hold.
    std::vector<float> coverage(count);
    for (std::size_t i = 0; i < count; ++i) {
        coverage[i] = stroke[i] ? 1.0f : 0.0f;
    }
    std::vector<float> rounded = coverage;
    boxBlur(rounded, width, height, std::max(1, strokeWidth / 3), 2);
    std::vector<bool> tube(count);
    for (std::size_t i = 0; i < count; ++i) {
        tube[i] = rounded[i] >= (stroke[i] ? 0.40f : 0.62f);
    }
    // Across a tube, its depth against the strokes' half width (a joint or a filled inner corner, deeper, is all core).
    const std::vector<float> inTube = insideDistance(tube, width, height);
    // The halo: the tubes' coverage blurred over about a stroke's width (three passes), so it reaches about twice the
    // stroke's width around it.
    std::vector<float> halo(count);
    for (std::size_t i = 0; i < count; ++i) {
        halo[i] = tube[i] ? 1.0f : 0.0f;
    }
    boxBlur(halo, width, height, strokeWidth, 3);
    for (std::size_t i = 0; i < count; ++i) {
        float lit = kNeonBacking;
        if (tube[i]) {
            // Across the tube: its glass at kNeonTubeEdge at the rim, rising to the white core over its inner third or
            // so (from the centre line out to 45% of the half width).
            const float t = std::clamp(inTube[i] / halfWidth, 0.0f, 1.0f);
            const float s = std::clamp((t - 0.20f) / 0.35f, 0.0f, 1.0f);
            lit = kNeonTubeEdge + (1.0f - kNeonTubeEdge) * s * s * (3.0f - 2.0f * s);
        } else {
            lit = std::max(lit, std::min(kNeonHalo, 0.9f * halo[i]));
        }
        const auto value = static_cast<std::uint8_t>(std::lround(std::clamp(lit, 0.0f, 1.0f) * 255.0f));
        image.pixels[i * 4] = value;
        image.pixels[i * 4 + 1] = value;
        image.pixels[i * 4 + 2] = value;
    }
    return image;
}

ImageData makeSolid(int width, int height, glm::u8vec4 color) {
    ImageData image = blankImage(width, height);
    if (image.isValid()) {
        fillRect(image, 0, 0, width, height, color);
    }
    return image;
}

// ---- The era posters, the cork board and the terminal (I6 step 3b) ------------------------------------------------

namespace {

/// `a` towards `b` by t in [0, 1], per channel, rounded.
Rgba mix(Rgba a, Rgba b, float t) noexcept {
    const auto channel = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - x) * t));
    };
    return Rgba(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), 255);
}

/// A disc (or a ring of `thickness` pixels when > 0) of radius r centred on (cx, cy), clipped to the image.
void fillDisc(ImageData& image, float cx, float cy, float r, Rgba color, float thickness = 0.0f) noexcept {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r)));
    const int x1 = std::min(image.width, static_cast<int>(std::ceil(cx + r)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r)));
    const int y1 = std::min(image.height, static_cast<int>(std::ceil(cy + r)) + 1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= r && (thickness <= 0.0f || d >= r - thickness)) {
                setPixel(image, x, y, color);
            }
        }
    }
}

/// A filled triangle, clipped to the image.
void fillTriangle(ImageData& image, glm::vec2 a, glm::vec2 b, glm::vec2 c, Rgba color) noexcept {
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(image.width, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(image.height, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))) + 1);
    const auto edge = [](glm::vec2 p, glm::vec2 q, glm::vec2 r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    const float area = edge(a, b, c);
    if (area == 0.0f) {
        return;
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const glm::vec2 p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            const float w0 = edge(b, c, p) / area;
            const float w1 = edge(c, a, p) / area;
            const float w2 = edge(a, b, p) / area;
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                setPixel(image, x, y, color);
            }
        }
    }
}

/// A zigzag stroke from (x0, y) rightwards over `length` pixels, `teeth` teeth of `amplitude`, `thickness` wide.
void drawZigzag(ImageData& image, float x0, float y, float length, int teeth, float amplitude, float thickness,
                Rgba color) noexcept {
    const int steps = std::max(8, static_cast<int>(length * 2.0f));
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float phase = t * static_cast<float>(teeth);
        const float tri = 2.0f * std::abs(phase - std::floor(phase) - 0.5f); // 1 at the teeth, 0 between
        fillDisc(image, x0 + t * length, y + (tri - 0.5f) * 2.0f * amplitude, 0.5f * thickness, color);
    }
}

/// The title of an era poster, in `color` within the band, at most three lines.
void posterTitle(ImageData& image, std::string_view title, int x0, int y0, int x1, int y1, Rgba color) {
    drawTitle(image, title, x0, y0, x1, y1, color, kMaxCoverTitleLines);
}

/// A 90s collector's poster: confetti of the decade (triangles, rings, zigzags, dots and squares) in teal, magenta,
/// yellow and violet on a charcoal or off-white ground, the title in a yellow or teal band below.
ImageData memphisPoster(int width, int height, std::string_view title, std::uint32_t seed) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    Rng rng(seed ^ 0x90A5C011u);
    const bool dark = rng.below(2) == 0;
    const Rgba ground = dark ? fromHex(0x22232A) : fromHex(0xEDE6D6);
    constexpr std::array<Rgba, 5> kConfetti = {fromHex(0x2FA59C), fromHex(0xD9487E), fromHex(0xF2C230),
                                               fromHex(0x6B4FA0), fromHex(0xEF7B45)};
    fillRect(image, 0, 0, width, height, ground);
    const float unit = static_cast<float>(std::min(width, height));
    const int bandTop = height * 70 / 100;
    const int shapes = 18 + rng.below(8);
    for (int i = 0; i < shapes; ++i) {
        const Rgba color = kConfetti[static_cast<std::size_t>(rng.below(static_cast<int>(kConfetti.size())))];
        const float cx = static_cast<float>(rng.below(width));
        const float cy = static_cast<float>(rng.below(std::max(1, bandTop)));
        const float size = unit * (0.05f + 0.01f * static_cast<float>(rng.below(8)));
        switch (rng.below(5)) {
        case 0:
            fillTriangle(image, {cx, cy - size}, {cx - size, cy + size * 0.8f}, {cx + size, cy + size * 0.6f}, color);
            break;
        case 1:
            fillDisc(image, cx, cy, size, color, std::max(2.0f, size * 0.3f));
            break;
        case 2:
            drawZigzag(image, cx - size * 1.5f, cy, size * 3.0f, 4, size * 0.3f, std::max(2.0f, size * 0.18f), color);
            break;
        case 3:
            for (int dy = 0; dy < 3; ++dy) {
                for (int dx = 0; dx < 3; ++dx) {
                    const float step = size * 0.6f;
                    fillDisc(image, cx + static_cast<float>(dx) * step, cy + static_cast<float>(dy) * step,
                             std::max(1.0f, size * 0.12f), color);
                }
            }
            break;
        default:
            fillRect(image, static_cast<int>(cx - size * 0.6f), static_cast<int>(cy - size * 0.6f),
                     static_cast<int>(cx + size * 0.6f), static_cast<int>(cy + size * 0.6f), color);
            break;
        }
    }
    const Rgba band = rng.below(2) == 0 ? fromHex(0xF2C230) : fromHex(0x2FA59C);
    fillRect(image, 0, bandTop, width, height, band);
    const int pad = std::max(1, width / 16);
    posterTitle(image, title, pad, bandTop + pad / 2, width - pad, height - pad / 2, fromHex(0x22232A));
    return image;
}

/// An optical era's large poster: a cinematic night, a deep blue gradient, a great pale disc with a cold rim of light
/// over a horizon of light, the title in pale letters at the top and a block of credits at the foot.
ImageData loungePoster(int width, int height, std::string_view title, std::uint32_t seed) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    Rng rng(seed ^ 0x0F71CA15u);
    constexpr std::array<Rgba, 3> kSkies = {fromHex(0x1E2F55), fromHex(0x3A2450), fromHex(0x123A44)};
    const Rgba sky = kSkies[static_cast<std::size_t>(rng.below(3))];
    const Rgba top = fromHex(0x07090F);
    for (int y = 0; y < height; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(height);
        const Rgba row = t < 0.6f ? mix(top, sky, t / 0.6f) : mix(sky, fromHex(0x05060A), (t - 0.6f) / 0.4f);
        fillRect(image, 0, y, width, y + 1, row);
    }
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float r = w * (0.28f + 0.01f * static_cast<float>(rng.below(8)));
    const float cx = w * (0.35f + 0.05f * static_cast<float>(rng.below(7)));
    const float cy = h * 0.42f;
    fillDisc(image, cx, cy, r * 1.08f, mix(sky, fromHex(0x4C8BC9), 0.45f));
    fillDisc(image, cx, cy, r, fromHex(0xC8CED6));
    fillDisc(image, cx + r * 0.18f, cy - r * 0.12f, r * 0.9f, mix(fromHex(0xC8CED6), sky, 0.35f));
    // A horizon of light across the foot of the disc, and a dark silhouette ridge below it.
    fillRect(image, 0, static_cast<int>(h * 0.62f), width, static_cast<int>(h * 0.62f) + std::max(1, height / 120),
             fromHex(0x9CC4F0));
    fillTriangle(image, {0.0f, h * 0.78f}, {w * 0.45f, h * 0.63f}, {w * 0.9f, h * 0.78f}, fromHex(0x05060A));
    fillRect(image, 0, static_cast<int>(h * 0.78f), width, height, fromHex(0x05060A));
    const int pad = std::max(1, width / 14);
    posterTitle(image, title, pad, height * 4 / 100, width - pad, height * 20 / 100, fromHex(0xE8ECF2));
    // Credits: rows of short grey bars.
    const int rowHeight = std::max(1, height / 90);
    for (int row = 0; row < 4; ++row) {
        const int y = height * 86 / 100 + row * rowHeight * 2;
        int x = pad;
        while (x < width - pad) {
            const int word = std::max(1, width / 30 + rng.below(std::max(1, width / 12)));
            fillRect(image, x, y, std::min(x + word, width - pad), y + rowHeight, fromHex(0x6A7380));
            x += word + std::max(1, width / 40);
        }
    }
    return image;
}

/// A handheld archive's console art: a pocket console of original design drawn large on a pale ground (a body of a
/// seeded colour, its green screen, a cross pad and two buttons), the title in a band below.
ImageData consolePoster(int width, int height, std::string_view title, std::uint32_t seed) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    Rng rng(seed ^ 0x6A11D0E7u);
    constexpr std::array<Rgba, 4> kBodies = {fromHex(0xB8B4A8), fromHex(0x3E8A86), fromHex(0xD8B64A),
                                             fromHex(0xA8414E)};
    fillRect(image, 0, 0, width, height, fromHex(0xE8DFC8));
    // Diagonal stripes behind the console.
    const Rgba stripe = fromHex(0xDCCFB0);
    const int period = std::max(4, width / 8);
    for (int y = 0; y < height * 7 / 10; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((x + y) % period < period / 3) {
                setPixel(image, x, y, stripe);
            }
        }
    }
    const Rgba body = kBodies[static_cast<std::size_t>(rng.below(4))];
    const int bx0 = width * 26 / 100;
    const int bx1 = width * 74 / 100;
    const int by0 = height * 7 / 100;
    const int by1 = height * 66 / 100;
    const int corner = (bx1 - bx0) / 8;
    fillRect(image, bx0 + corner, by0, bx1 - corner, by1, body);
    fillRect(image, bx0, by0 + corner, bx1, by1 - corner, body);
    for (const int x : {bx0 + corner, bx1 - corner}) {
        for (const int y : {by0 + corner, by1 - corner}) {
            fillDisc(image, static_cast<float>(x), static_cast<float>(y), static_cast<float>(corner), body);
        }
    }
    const int sx0 = bx0 + (bx1 - bx0) / 7;
    const int sx1 = bx1 - (bx1 - bx0) / 7;
    const int sy0 = by0 + (by1 - by0) / 10;
    const int sy1 = by0 + (by1 - by0) * 48 / 100;
    fillRect(image, sx0 - 4, sy0 - 4, sx1 + 4, sy1 + 4, fromHex(0x3A3A40));
    fillRect(image, sx0, sy0, sx1, sy1, fromHex(0x8EA35A));
    fillRect(image, sx0 + (sx1 - sx0) / 5, sy0 + (sy1 - sy0) * 3 / 5, sx1 - (sx1 - sx0) / 3,
             sy0 + (sy1 - sy0) * 3 / 5 + std::max(2, height / 80), fromHex(0x4E6A2E));
    const float padY = static_cast<float>(by0) + static_cast<float>(by1 - by0) * 0.72f;
    const float padX = static_cast<float>(bx0) + static_cast<float>(bx1 - bx0) * 0.3f;
    const float arm = static_cast<float>(bx1 - bx0) * 0.1f;
    const float thick = arm * 0.38f;
    fillRect(image, static_cast<int>(padX - arm), static_cast<int>(padY - thick), static_cast<int>(padX + arm),
             static_cast<int>(padY + thick), fromHex(0x2A2A30));
    fillRect(image, static_cast<int>(padX - thick), static_cast<int>(padY - arm), static_cast<int>(padX + thick),
             static_cast<int>(padY + arm), fromHex(0x2A2A30));
    const float buttonX = static_cast<float>(bx0) + static_cast<float>(bx1 - bx0) * 0.7f;
    fillDisc(image, buttonX, padY + arm * 0.3f, arm * 0.45f, fromHex(0x8C2A48));
    fillDisc(image, buttonX + arm * 1.1f, padY - arm * 0.4f, arm * 0.45f, fromHex(0x8C2A48));
    const int bandTop = height * 72 / 100;
    fillRect(image, 0, bandTop, width, height, fromHex(0x2E3B36));
    const int pad = std::max(1, width / 16);
    posterTitle(image, title, pad, bandTop + pad / 2, width - pad, height - pad / 2, fromHex(0xE8DFC8));
    return image;
}

/// An arcade's flyer: a magenta and violet sunburst on black over a chequered floor, the title in yellow with a
/// magenta shadow at the top.
ImageData flyerPoster(int width, int height, std::string_view title, std::uint32_t seed) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    Rng rng(seed ^ 0xA2CADE77u);
    const float cx = static_cast<float>(width) * 0.5f;
    const float cy = static_cast<float>(height) * (0.5f + 0.02f * static_cast<float>(rng.below(5)));
    const int rays = 14 + 2 * rng.below(4);
    const Rgba rayA = fromHex(0xB34DCE);
    const Rgba rayB = fromHex(0x2A0F33);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float angle = std::atan2(static_cast<float>(y) + 0.5f - cy, static_cast<float>(x) + 0.5f - cx);
            const float turn = (angle + 3.14159265f) / 6.2831853f;
            const int sector = static_cast<int>(std::floor(turn * static_cast<float>(rays)));
            setPixel(image, x, y, sector % 2 == 0 ? rayA : rayB);
        }
    }
    fillDisc(image, cx, cy, static_cast<float>(width) * 0.16f, fromHex(0x5AD8F0));
    fillDisc(image, cx, cy, static_cast<float>(width) * 0.12f, fromHex(0x0A0A12));
    // The chequered floor.
    const int floorTop = height * 74 / 100;
    const int cell = std::max(2, width / 10);
    for (int y = floorTop; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool dark = ((x / cell) + ((y - floorTop) / cell)) % 2 == 0;
            setPixel(image, x, y, dark ? fromHex(0x0A0A12) : fromHex(0xE0E0E8));
        }
    }
    const int pad = std::max(1, width / 14);
    const int shadow = std::max(1, width / 90);
    posterTitle(image, title, pad + shadow, height * 4 / 100 + shadow, width - pad + shadow,
                height * 26 / 100 + shadow, fromHex(0xF0509E));
    posterTitle(image, title, pad, height * 4 / 100, width - pad, height * 26 / 100, fromHex(0xF2D040));
    return image;
}

/// The computer studio's cork board: speckled cork with pinned index cards (ruled, one carrying the title), a few
/// sticky notes and a photograph, each with a coloured pin.
ImageData corkBoard(int width, int height, std::string_view title, std::uint32_t seed) {
    ImageData image = blankImage(width, height);
    if (!image.isValid()) {
        return image;
    }
    Rng speckle(0xC0B5u);
    const Rgba cork = fromHex(0xB0835A);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int value = speckle.below(100);
            const Rgba grain = value < 12   ? fromHex(0x7A5634)
                               : value < 24 ? fromHex(0xC89A6A)
                               : value < 30 ? fromHex(0x8E6642)
                                            : cork;
            setPixel(image, x, y, grain);
        }
    }
    Rng rng(seed ^ 0xC0A4B0A2u);
    const auto pin = [&](int x, int y) {
        constexpr std::array<Rgba, 3> kPins = {fromHex(0xC23A3A), fromHex(0x3A6AC2), fromHex(0xE0B030)};
        const float r = std::max(2.0f, static_cast<float>(std::min(width, height)) / 50.0f);
        fillDisc(image, static_cast<float>(x), static_cast<float>(y), r,
                 kPins[static_cast<std::size_t>(rng.below(3))]);
    };
    // The title card, then others around it.
    const int cardW = width * 42 / 100;
    const int cardH = height * 30 / 100;
    const int tx = width * 8 / 100;
    const int ty = height * 10 / 100;
    fillRect(image, tx + 3, ty + 3, tx + cardW + 3, ty + cardH + 3, fromHex(0x5A3F28));
    fillRect(image, tx, ty, tx + cardW, ty + cardH, fromHex(0xF2EAD6));
    fillRect(image, tx, ty + cardH / 5, tx + cardW, ty + cardH / 5 + std::max(1, height / 200), fromHex(0xC85050));
    for (int line = 2; line < 5; ++line) {
        fillRect(image, tx, ty + cardH * line / 5, tx + cardW, ty + cardH * line / 5 + std::max(1, height / 300),
                 fromHex(0x9AB0D0));
    }
    posterTitle(image, title, tx + cardW / 12, ty + cardH / 4, tx + cardW - cardW / 12, ty + cardH - cardH / 8,
                fromHex(0x2A2A40));
    pin(tx + cardW / 2, ty + cardH / 12);
    const int items = 4 + rng.below(3);
    for (int i = 0; i < items; ++i) {
        const int kind = rng.below(3);
        const int w = kind == 1 ? width * 16 / 100 : width * (20 + rng.below(10)) / 100;
        const int h = kind == 1 ? w : height * (16 + rng.below(10)) / 100; // a sticky note is square
        const int x = width * 50 / 100 + rng.below(std::max(1, width / 2 - w - width / 20));
        const int y = rng.below(std::max(1, height - h - height / 20)) + height / 40;
        const Rgba paper = kind == 0 ? fromHex(0xF2EAD6) : kind == 1 ? fromHex(0xF2D84A) : fromHex(0x4A6A8A);
        fillRect(image, x + 2, y + 2, x + w + 2, y + h + 2, fromHex(0x5A3F28));
        fillRect(image, x, y, x + w, y + h, paper);
        if (kind == 2) { // a photograph: a sky and a hill
            fillRect(image, x + w / 10, y + h / 10, x + w - w / 10, y + h * 6 / 10, fromHex(0x8AB0D8));
            fillRect(image, x + w / 10, y + h * 6 / 10, x + w - w / 10, y + h - h / 10, fromHex(0x5A7A4A));
        } else {
            for (int line = 1; line < 4; ++line) {
                const int lineY = y + h * line / 4;
                fillRect(image, x + w / 8, lineY, x + w - w / 4, lineY + std::max(1, height / 150), fromHex(0x6A6A7A));
            }
        }
        pin(x + w / 2, y + h / 10);
    }
    // The lower left: a second ruled card.
    const int lx = width * 12 / 100;
    const int ly = height * 52 / 100;
    fillRect(image, lx + 3, ly + 3, lx + cardW * 8 / 10 + 3, ly + cardH + 3, fromHex(0x5A3F28));
    fillRect(image, lx, ly, lx + cardW * 8 / 10, ly + cardH, fromHex(0xEDE3C8));
    for (int line = 1; line < 5; ++line) {
        const int lineY = ly + cardH * line / 5;
        fillRect(image, lx + cardW / 12, lineY, lx + cardW * 7 / 10, lineY + std::max(1, height / 150),
                 fromHex(0x6A6A7A));
    }
    pin(lx + cardW * 4 / 10, ly + cardH / 12);
    return image;
}

/// A home computer's screen in a study: green text on a black-green ground (a prompt, a short listing that names the
/// title, a cursor), framed by a dark border.
ImageData terminalScreen(int width, int height, std::string_view title) {
    ImageData image = makeSolid(width, height, fromHex(0x0A1A10));
    if (!image.isValid()) {
        return image;
    }
    const Rgba ink = fromHex(0x7CF09A);
    const int border = std::max(1, std::min(width, height) / 16);
    const std::string name = normalizeTitle(title.empty() ? std::string_view("PROGRAM") : title);
    const std::vector<std::string> lines = {"READY.", "10 PRINT \"" + name.substr(0, 14) + "\"", "20 GOTO 10", "RUN"};
    const int scale = std::max(1, (height - 2 * border) / (8 * (kGlyphHeight + kLineGap)));
    int y = border;
    // Each line left-aligned: drawn in a box exactly its width.
    const auto line = [&](const std::string& text) {
        const int right = border + static_cast<int>(text.size()) * kGlyphAdvance * scale;
        drawText(image, {text}, scale, border, y, right, y + kGlyphHeight * scale, ink);
        y += (kGlyphHeight + kLineGap) * scale;
    };
    for (const std::string& text : lines) {
        line(text);
    }
    line(name.substr(0, 14));
    line(name.substr(0, 14));
    fillRect(image, border, y, border + kGlyphWidth * scale, y + kGlyphHeight * scale, ink);
    return image;
}

} // namespace

std::span<const std::string_view> patternNames() noexcept {
    static constexpr std::array<std::string_view, 12> kNames = {
        kPatternColorBars,     kPatternChecker,      kPatternCover,        kPatternSign,
        kPatternPrint,         kPatternNeon,         kPatternPosterMemphis, kPatternPosterLounge,
        kPatternPosterConsole, kPatternPosterFlyer,  kPatternCorkBoard,     kPatternTerminal};
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
    if (name == kPatternNeon) {
        return makeNeonLettering(width, height, title);
    }
    if (name == kPatternPosterMemphis) {
        return memphisPoster(width, height, title, seed);
    }
    if (name == kPatternPosterLounge) {
        return loungePoster(width, height, title, seed);
    }
    if (name == kPatternPosterConsole) {
        return consolePoster(width, height, title, seed);
    }
    if (name == kPatternPosterFlyer) {
        return flyerPoster(width, height, title, seed);
    }
    if (name == kPatternCorkBoard) {
        return corkBoard(width, height, title, seed);
    }
    if (name == kPatternTerminal) {
        return terminalScreen(width, height, title);
    }
    return makeError(ErrorCode::NotFound,
                     "unknown test pattern (expected color_bars, checker, cover, sign, print, neon, poster_memphis, "
                     "poster_lounge, poster_console, poster_flyer, cork_board or terminal)",
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
