#include "path_util.hpp"

#include <rv/core/text.hpp>

namespace rv::assets::detail {
namespace {

/// Well-formed UTF-8 (Unicode table 3-7): no overlong forms, UTF-16 surrogates, code points above U+10FFFF, stray
/// continuation bytes or truncated sequences. std::filesystem throws on such text on Windows.
bool isWellFormedUtf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned low = 0x80; // valid range of the second byte
        unsigned high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            low = lead == 0xE0 ? 0xA0 : low;   // overlong
            high = lead == 0xED ? 0x9F : high; // surrogates
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            low = lead == 0xF0 ? 0x90 : low;   // overlong
            high = lead == 0xF4 ? 0x8F : high; // above U+10FFFF
        } else {
            return false; // continuation byte without a lead byte, or C0, C1, F5..FF
        }
        if (text.size() - i < length) {
            return false;
        }
        const unsigned second = static_cast<unsigned char>(text[i + 1]);
        if (second < low || second > high) {
            return false;
        }
        for (std::size_t k = 2; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0u) != 0x80u) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

/// `path` in quotes for error messages. Control characters, and every non-ASCII byte when the path is not UTF-8,
/// are written as \xHH so that messages stay printable UTF-8.
std::string quoted(std::string_view path, bool utf8) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string text = "\"";
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F || (byte >= 0x80 && !utf8)) {
            text += "\\x";
            text += kHex[byte >> 4];
            text += kHex[byte & 0x0F];
        } else {
            text += c;
        }
    }
    text += '"';
    return text;
}

} // namespace

Expected<std::string> normalizeRelativePath(std::string_view path, bool allowDotSegments, std::string_view context) {
    const bool utf8 = isWellFormedUtf8(path);
    const auto fail = [&](std::string_view why) {
        return makeError(ErrorCode::InvalidArgument,
                         "invalid relative path " + quoted(path, utf8) + ": " + std::string(why),
                         std::string(context));
    };
    if (path.empty()) {
        return fail("the path is empty");
    }
    if (!utf8) {
        return fail("the path is not valid UTF-8");
    }
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return fail("control characters are not allowed");
        }
        if (c == '\\') {
            return fail("use forward slashes");
        }
        if (c == ':') {
            return fail("drive letters and URI schemes are not allowed");
        }
    }
    if (path.front() == '/') {
        return fail("absolute paths are not allowed");
    }

    std::string normalized;
    for (const std::string_view segment : core::split(path, '/')) {
        if (segment.empty()) {
            return fail("empty path segment");
        }
        if (segment == "..") {
            return fail("\"..\" segments are not allowed");
        }
        if (segment == ".") {
            if (!allowDotSegments) {
                return fail("\".\" segments are not allowed");
            }
            continue;
        }
        if (!normalized.empty()) {
            normalized += '/';
        }
        normalized += segment;
    }
    if (normalized.empty()) {
        return fail("the path names no file");
    }
    return normalized;
}

} // namespace rv::assets::detail
