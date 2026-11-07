// The only translation unit that includes lunasvg (I11 step 2, ADR-0019 §3): its documents never leave this file.

#include <rv/assets/svg_raster.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <string>

#include <fmt/format.h>
#include <lunasvg.h>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::assets {
namespace {

bool equalsAt(std::string_view text, std::size_t at, std::string_view word) {
    return at + word.size() <= text.size() && core::equalsIgnoreCaseAscii(text.substr(at, word.size()), word);
}

/// What a logo never needs and a hostile document could use: a DOCTYPE or an entity declaration (entity expansion), a
/// CSS @import, an href that leaves the document. Returns the reason, or nothing when the document has none.
std::optional<std::string> refusedFeature(std::string_view text) {
    for (std::size_t at = 0; at < text.size(); ++at) {
        const char c = text[at];
        if (c == '<' && (equalsAt(text, at, "<!DOCTYPE") || equalsAt(text, at, "<!ENTITY"))) {
            return std::string("a DOCTYPE or an entity declaration");
        }
        if (c == '@' && equalsAt(text, at, "@import")) {
            return std::string("a CSS @import");
        }
        if ((c == 'h' || c == 'H') && equalsAt(text, at, "href")) {
            // The attribute's value: past "href", blanks, '=', blanks and the quote.
            std::size_t i = at + 4;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
                ++i;
            }
            if (i >= text.size() || text[i] != '=') {
                continue;
            }
            ++i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
                ++i;
            }
            if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                ++i;
            }
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
                ++i;
            }
            if (i < text.size() && (text[i] == '#' || equalsAt(text, i, "data:"))) {
                continue;
            }
            return std::string("an href to another file or address");
        }
    }
    return std::nullopt;
}

std::size_t elementCount(std::string_view text) noexcept {
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '<'));
}

} // namespace

std::optional<std::pair<int, int>> fitInto(double width, double height, int boxWidth, int boxHeight) {
    if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) || !std::isfinite(height) || boxWidth < 1 ||
        boxHeight < 1) {
        return std::nullopt;
    }
    const double scale = std::min(static_cast<double>(boxWidth) / width, static_cast<double>(boxHeight) / height);
    const int fittedWidth = std::clamp(static_cast<int>(std::lround(width * scale)), 1, boxWidth);
    const int fittedHeight = std::clamp(static_cast<int>(std::lround(height * scale)), 1, boxHeight);
    return std::make_pair(fittedWidth, fittedHeight);
}

Expected<ImageData> rasterizeSvg(std::span<const std::byte> document, int boxWidth, int boxHeight,
                                 std::string_view context) {
    const std::string source(context);
    if (boxWidth < 1 || boxHeight < 1 || boxWidth > kMaxLogoSide || boxHeight > kMaxLogoSide) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("a logo is rasterized at 1 to {} pixels a side, not {} x {}", kMaxLogoSide,
                                     boxWidth, boxHeight),
                         source);
    }
    if (document.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the SVG document is empty", source);
    }
    if (document.size() > kMaxSvgBytes) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("the SVG document holds {} bytes, more than the {} taken", document.size(),
                                     kMaxSvgBytes),
                         source);
    }
    const std::string_view text(reinterpret_cast<const char*>(document.data()), document.size());
    if (elementCount(text) > kMaxSvgElements) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("the SVG document holds more than {} elements", kMaxSvgElements), source);
    }
    if (const auto refused = refusedFeature(text)) {
        return makeError(ErrorCode::Unsupported, fmt::format("the SVG document holds {}", *refused), source);
    }
    try {
        const std::unique_ptr<lunasvg::Document> svg = lunasvg::Document::loadFromData(text.data(), text.size());
        if (!svg) {
            return makeError(ErrorCode::ParseError, "lunasvg cannot read the SVG document", source);
        }
        const auto size = fitInto(svg->width(), svg->height(), boxWidth, boxHeight);
        if (!size) {
            return makeError(ErrorCode::ParseError, "the SVG document has no size (no width, height or viewBox)",
                             source);
        }
        lunasvg::Bitmap bitmap = svg->renderToBitmap(size->first, size->second, 0x00000000);
        if (bitmap.isNull() || bitmap.width() != size->first || bitmap.height() != size->second) {
            return makeError(ErrorCode::ParseError, "lunasvg could not draw the SVG document", source);
        }
        // lunasvg draws premultiplied ARGB32 (BGRA in memory); convertToRGBA() makes it straight RGBA.
        bitmap.convertToRGBA();
        ImageData image;
        image.width = bitmap.width();
        image.height = bitmap.height();
        image.channels = 4;
        image.pixels.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4);
        const std::size_t row = static_cast<std::size_t>(image.width) * 4;
        for (int y = 0; y < image.height; ++y) {
            const std::uint8_t* from = bitmap.data() + static_cast<std::size_t>(y) * bitmap.stride();
            std::copy(from, from + row, image.pixels.data() + static_cast<std::size_t>(y) * row);
        }
        return image;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while rasterizing the SVG document", source);
    }
}

Expected<ImageData> loadLogo(const std::filesystem::path& file, int boxWidth, int boxHeight) {
    const std::string context = core::pathToUtf8Lossy(file);
    const std::string extension = core::toLowerAscii(core::pathToUtf8Lossy(file.extension()));
    const bool svg = extension == ".svg";
    if (!svg && extension != ".png" && extension != ".jpg" && extension != ".jpeg") {
        return makeError(ErrorCode::Unsupported, "a logo is an .svg, .png or .jpg file", context);
    }
    auto bytes = core::readFileShared(file, svg ? kMaxSvgBytes : kMaxImageFileBytes);
    if (!bytes) {
        return propagate(bytes.error());
    }
    const auto data = std::as_bytes(std::span(bytes->data(), bytes->size()));
    if (svg) {
        return rasterizeSvg(data, boxWidth, boxHeight, context);
    }
    auto decoded = decodeUprightImage(data, 4, context);
    if (!decoded) {
        return propagate(decoded.error());
    }
    const auto size = fitInto(decoded->width, decoded->height, boxWidth, boxHeight);
    if (!size || (size->first >= decoded->width && size->second >= decoded->height)) {
        return decoded; // already within the box: never enlarged
    }
    return resampleSrgbAlpha(*decoded, size->first, size->second);
}

} // namespace rv::assets
