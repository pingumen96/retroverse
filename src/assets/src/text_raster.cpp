// The only translation unit that compiles the stb_truetype implementation (STBTT_STATIC: internal linkage, so it never
// clashes with the copy Dear ImGui carries). stb_truetype reads the font through a const stbtt_fontinfo and allocates
// per call, so the const functions of FontFace are safe on several threads at once.

#include <rv/assets/text_raster.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <new>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace rv::assets {

struct FontFace::Impl {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    std::string sha256;
    int replacement = 0;
    int unitsAscent = 0;
    int unitsDescent = 0;
    int unitsCapHeight = 0;
};

namespace {

constexpr char32_t kReplacementCharacter = 0xFFFD;

bool isSafeRelativePath(std::string_view relative) {
    if (relative.empty() || relative.front() == '/' || relative.front() == '\\' ||
        relative.find(':') != std::string_view::npos) {
        return false;
    }
    for (const std::string_view element : core::split(relative, '/')) {
        if (element.empty() || element == "." || element == ".." || element.find('\\') != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

} // namespace

std::u32string decodeUtf8(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    const auto byteAt = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    while (i < text.size()) {
        const unsigned char lead = byteAt(i);
        std::size_t length = 0;
        char32_t value = 0;
        char32_t minimum = 0;
        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
            length = 2;
            value = lead & 0x1FU;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            value = lead & 0x0FU;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
            length = 4;
            value = lead & 0x07U;
            minimum = 0x10000;
        } else {
            out.push_back(kReplacementCharacter);
            ++i;
            continue;
        }
        std::size_t consumed = 1;
        bool valid = true;
        for (; consumed < length; ++consumed) {
            if (i + consumed >= text.size() || (byteAt(i + consumed) & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            value = (value << 6) | (byteAt(i + consumed) & 0x3FU);
        }
        if (!valid || value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            out.push_back(kReplacementCharacter);
            i += std::max<std::size_t>(consumed, 1);
            continue;
        }
        out.push_back(value);
        i += length;
    }
    return out;
}

std::string encodeUtf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char32_t value : text) {
        if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            value = kReplacementCharacter;
        }
        core::appendUtf8(out, value);
    }
    return out;
}

Expected<FontFace> FontFace::loadVerified(const std::filesystem::path& file, std::string_view expectedSha256) {
    auto bytes = core::readBinaryFile(file, kMaxFontFileBytes);
    if (!bytes) {
        return propagate(bytes.error());
    }
    const std::string actual = core::sha256Hex(*bytes);
    const std::string context = core::pathToUtf8Lossy(file);
    if (actual != core::toLowerAscii(expectedSha256)) {
        return makeError(ErrorCode::ValidationFailed,
                         "the font's SHA-256 " + actual + " is not its registry entry's " + std::string(expectedSha256) +
                             ": refused",
                         context);
    }
    if (bytes->size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::OutOfRange, "the font is too large", context);
    }
    try {
        auto impl = std::make_shared<Impl>();
        impl->bytes.resize(bytes->size());
        std::transform(bytes->begin(), bytes->end(), impl->bytes.begin(),
                       [](std::byte value) { return static_cast<unsigned char>(value); });
        impl->sha256 = actual;
        const int offset = stbtt_GetFontOffsetForIndex(impl->bytes.data(), 0);
        if (offset < 0 || stbtt_InitFont(&impl->info, impl->bytes.data(), offset) == 0) {
            return makeError(ErrorCode::ParseError, "stb_truetype cannot read the font", context);
        }
        impl->replacement = stbtt_FindGlyphIndex(&impl->info, static_cast<int>(kReplacementCharacter));
        int lineGap = 0;
        stbtt_GetFontVMetrics(&impl->info, &impl->unitsAscent, &impl->unitsDescent, &lineGap);
        if (impl->unitsAscent - impl->unitsDescent <= 0) {
            return makeError(ErrorCode::ParseError, "the font has no vertical metrics", context);
        }
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        const int capital = stbtt_FindGlyphIndex(&impl->info, 'H');
        if (capital != 0 && stbtt_GetGlyphBox(&impl->info, capital, &x0, &y0, &x1, &y1) != 0) {
            impl->unitsCapHeight = y1;
        } else {
            impl->unitsCapHeight = impl->unitsAscent * 7 / 10;
        }
        return FontFace(std::move(impl));
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while loading the font", context);
    }
}

Expected<FontFace> FontFace::loadBundled(const std::filesystem::path& assetRoot, std::string_view relativePath) {
    if (!isSafeRelativePath(relativePath)) {
        return makeError(ErrorCode::InvalidArgument, "not an asset-relative path", std::string(relativePath));
    }
    const std::filesystem::path registry = assetRoot / "registry" / "assets.json";
    auto json = core::readJsonFile(registry);
    if (!json) {
        return propagate(json.error());
    }
    const auto assets = json->find("assets");
    if (assets == json->end() || !assets->is_array()) {
        return makeError(ErrorCode::ParseError, "the registry has no assets array", core::pathToUtf8Lossy(registry));
    }
    for (const nlohmann::json& entry : *assets) {
        if (!entry.is_object()) {
            continue;
        }
        const auto path = entry.find("path");
        if (path == entry.end() || !path->is_string() || path->get<std::string>() != relativePath) {
            continue;
        }
        const auto kind = entry.find("kind");
        const auto sha = entry.find("sha256");
        if (kind == entry.end() || !kind->is_string() || kind->get<std::string>() != "font" || sha == entry.end() ||
            !sha->is_string() || sha->get<std::string>().size() != 64) {
            return makeError(ErrorCode::ParseError, "the registry entry is not a font with a SHA-256",
                             std::string(relativePath));
        }
        auto file = core::tryPathFromUtf8(relativePath);
        if (!file) {
            return propagate(file.error());
        }
        return loadVerified(assetRoot / *file, sha->get<std::string>());
    }
    return makeError(ErrorCode::NotFound, "the font has no entry in the asset registry", std::string(relativePath));
}

const std::string& FontFace::sha256() const noexcept {
    return m_impl->sha256;
}

int FontFace::glyphIndex(char32_t codePoint) const noexcept {
    if (codePoint > 0x10FFFF) {
        return 0;
    }
    return stbtt_FindGlyphIndex(&m_impl->info, static_cast<int>(codePoint));
}

int FontFace::replacementGlyph() const noexcept {
    return m_impl->replacement;
}

float FontFace::ascent(float pixelHeight) const noexcept {
    return static_cast<float>(m_impl->unitsAscent) * stbtt_ScaleForPixelHeight(&m_impl->info, pixelHeight);
}

float FontFace::descent(float pixelHeight) const noexcept {
    return static_cast<float>(m_impl->unitsDescent) * stbtt_ScaleForPixelHeight(&m_impl->info, pixelHeight);
}

float FontFace::capHeight(float pixelHeight) const noexcept {
    return static_cast<float>(m_impl->unitsCapHeight) * stbtt_ScaleForPixelHeight(&m_impl->info, pixelHeight);
}

GlyphRun FontFace::layout(std::u32string_view text, float pixelHeight) const {
    GlyphRun run;
    run.glyphs.reserve(text.size());
    run.positions.reserve(text.size());
    const float scale = stbtt_ScaleForPixelHeight(&m_impl->info, pixelHeight);
    float pen = 0.0F;
    int previous = -1;
    for (const char32_t codePoint : text) {
        int glyph = glyphIndex(codePoint);
        if (glyph == 0) {
            glyph = m_impl->replacement;
            ++run.replacementGlyphs;
        }
        if (previous >= 0) {
            pen += static_cast<float>(stbtt_GetGlyphKernAdvance(&m_impl->info, previous, glyph)) * scale;
        }
        int advance = 0;
        int bearing = 0;
        stbtt_GetGlyphHMetrics(&m_impl->info, glyph, &advance, &bearing);
        run.glyphs.push_back(glyph);
        run.positions.push_back(pen);
        pen += static_cast<float>(advance) * scale;
        previous = glyph;
    }
    run.width = pen;
    return run;
}

void FontFace::draw(const GlyphRun& run, float pixelHeight, float x, float baseline, std::span<std::uint8_t> coverage,
                    int width, int height) const {
    if (width < 1 || height < 1 ||
        coverage.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return;
    }
    const float scale = stbtt_ScaleForPixelHeight(&m_impl->info, pixelHeight);
    const float originY = std::floor(baseline);
    const float shiftY = baseline - originY;
    std::vector<unsigned char> glyphPixels;
    for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
        const int glyph = run.glyphs[i];
        if (stbtt_IsGlyphEmpty(&m_impl->info, glyph) != 0) {
            continue;
        }
        const float penX = x + run.positions[i];
        const float originX = std::floor(penX);
        const float shiftX = penX - originX;
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&m_impl->info, glyph, scale, scale, shiftX, shiftY, &x0, &y0, &x1, &y1);
        const int glyphWidth = x1 - x0;
        const int glyphHeight = y1 - y0;
        if (glyphWidth <= 0 || glyphHeight <= 0) {
            continue;
        }
        glyphPixels.assign(static_cast<std::size_t>(glyphWidth) * static_cast<std::size_t>(glyphHeight), 0);
        stbtt_MakeGlyphBitmapSubpixel(&m_impl->info, glyphPixels.data(), glyphWidth, glyphHeight, glyphWidth, scale,
                                      scale, shiftX, shiftY, glyph);
        const int left = static_cast<int>(originX) + x0;
        const int top = static_cast<int>(originY) + y0;
        for (int gy = 0; gy < glyphHeight; ++gy) {
            const int py = top + gy;
            if (py < 0 || py >= height) {
                continue;
            }
            for (int gx = 0; gx < glyphWidth; ++gx) {
                const int px = left + gx;
                if (px < 0 || px >= width) {
                    continue;
                }
                std::uint8_t& target =
                    coverage[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)];
                // The larger coverage where glyphs overlap (kerned pairs), so that their edges never darken twice.
                target = std::max(target, glyphPixels[static_cast<std::size_t>(gy) * static_cast<std::size_t>(glyphWidth) +
                                                      static_cast<std::size_t>(gx)]);
            }
        }
    }
}

void drawText(ImageData& image, Rgb8 foreground, std::span<const PlacedRun> runs) {
    if (!image.isValid() || image.channels != 4) {
        return;
    }
    constexpr int kScale = 2;
    const int width = image.width * kScale;
    const int height = image.height * kScale;
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    for (const PlacedRun& placed : runs) {
        if (placed.font == nullptr || placed.text.empty() || placed.pixelHeight <= 0.0F) {
            continue;
        }
        const float size = placed.pixelHeight * kScale;
        const GlyphRun run = placed.font->layout(placed.text, size);
        placed.font->draw(run, size, placed.x * kScale, placed.baseline * kScale, coverage, width, height);
    }
    const LinearRgb ink = toLinear(foreground);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t top = static_cast<std::size_t>(y * kScale) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x * kScale);
            const std::size_t bottom = top + static_cast<std::size_t>(width);
            const int sum = coverage[top] + coverage[top + 1] + coverage[bottom] + coverage[bottom + 1];
            if (sum == 0) {
                continue;
            }
            const float alpha = static_cast<float>(sum) / (4.0F * 255.0F);
            std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                                                         static_cast<std::size_t>(x)) *
                                                            4;
            pixel[0] = linearToSrgbByte(srgbByteToLinear(pixel[0]) * (1.0F - alpha) + ink.r * alpha);
            pixel[1] = linearToSrgbByte(srgbByteToLinear(pixel[1]) * (1.0F - alpha) + ink.g * alpha);
            pixel[2] = linearToSrgbByte(srgbByteToLinear(pixel[2]) * (1.0F - alpha) + ink.b * alpha);
        }
    }
}

} // namespace rv::assets
