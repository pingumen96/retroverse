#include "path_util.hpp"

#include <rv/core/text.hpp>

namespace rv::assets::detail {

Expected<std::string> normalizeRelativePath(std::string_view path, bool allowDotSegments, std::string_view context) {
    const auto fail = [&](std::string_view why) {
        return makeError(ErrorCode::InvalidArgument,
                         "invalid relative path \"" + std::string(path) + "\": " + std::string(why),
                         std::string(context));
    };
    if (path.empty()) {
        return fail("the path is empty");
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
