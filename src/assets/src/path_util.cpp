#include "path_util.hpp"

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::assets::detail {
namespace {

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
    const bool utf8 = core::isValidUtf8(path);
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
