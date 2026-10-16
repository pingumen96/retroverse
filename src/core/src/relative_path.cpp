#include <rv/core/relative_path.hpp>

#include <algorithm>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "path_elements.hpp"

namespace rv::core {
namespace {

/// The first element foldedPathElements() gives a UNC path, before its server and share.
constexpr std::wstring_view kUncMarker = L"\\\\";

/// `text` in quotes for messages: control characters, and every byte above ASCII when the text is not UTF-8, written
/// as \xHH so that messages stay printable UTF-8.
std::string quoted(std::string_view text, bool utf8) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F || (byte >= 0x80 && !utf8)) {
            out += "\\x";
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

bool isAsciiLetter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// `element` upper-cased with the file-system casing, as UTF-16; nothing when it cannot be.
std::optional<std::wstring> foldedElement(std::string_view element) {
    const auto wide = utf8ToWide(element);
    if (!wide) {
        return std::nullopt;
    }
    auto upper = fileSystemUpperCase(std::wstring_view(*wide));
    if (!upper) {
        return std::nullopt;
    }
    return std::move(*upper);
}

} // namespace

Expected<std::string> resolveRelativePath(std::string_view pathText, const std::filesystem::path& base) {
    const bool utf8 = isValidUtf8(pathText);
    const std::string baseText = pathToUtf8Lossy(base);
    const auto refuse = [&](ErrorCode code, std::string_view why) {
        return makeError(code, "path " + quoted(pathText, utf8) + " refused: " + std::string(why), baseText);
    };
    const auto malformed = [&](std::string_view why) { return refuse(ErrorCode::InvalidArgument, why); };
    if (pathText.empty()) {
        return malformed("it is empty");
    }
    if (pathText.find('\0') != std::string_view::npos) {
        return malformed("it holds a NUL character");
    }
    for (const char c : pathText) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return malformed("it holds a control character");
        }
    }
    if (!utf8) {
        return malformed(fmt::format("malformed UTF-8 at byte offset {}", findInvalidUtf8(pathText).value_or(0)));
    }
    if (!base.is_absolute()) {
        return makeError(ErrorCode::InvalidArgument, "the folder a path is resolved against must be absolute",
                         baseText);
    }
    const auto baseElements = detail::foldedPathElements(base);
    if (!baseElements || baseElements->empty()) {
        return makeError(ErrorCode::InvalidArgument, "the folder a path is resolved against cannot be compared",
                         baseText);
    }

    std::string text(pathText);
    std::replace(text.begin(), text.end(), '\\', '/');
    // Where the path starts, folded as the file system compares names: `base` for a relative path; for an absolute
    // one its drive ("I:"), or the UNC marker followed by its server and share, which are its first elements here.
    // ".." never takes back the first `rootSize` elements of `reached`, as Windows never goes above a drive or share.
    std::vector<std::wstring> reached;
    std::size_t rootSize = 0;
    std::vector<std::string_view> elements;
    std::string_view rest = text;
    if (text.starts_with("//")) {
        if (text.size() >= 4 && (text[2] == '?' || text[2] == '.') && text[3] == '/') {
            return malformed("the \\\\?\\ and \\\\.\\ forms are not allowed");
        }
        const std::vector<std::string_view> parts = split(std::string_view(text).substr(2), '/');
        if (parts.size() < 2 || parts[0].empty() || parts[1].empty() || parts[0] == "." || parts[0] == ".." ||
            parts[1] == "." || parts[1] == "..") {
            return malformed("a UNC path names a server and a share");
        }
        reached.emplace_back(kUncMarker);
        rootSize = 3;
        elements = {parts[0], parts[1]};
        rest = std::string_view(text).substr(2 + parts[0].size() + 1 + parts[1].size());
    } else if (text.starts_with('/')) {
        return malformed("a path rooted without a drive letter is not allowed");
    } else if (text.size() >= 2 && isAsciiLetter(text[0]) && text[1] == ':') {
        if (text.size() == 2 || text[2] != '/') {
            return malformed("drive-relative paths are not allowed");
        }
        const auto drive = foldedElement(std::string_view(text).substr(0, 2));
        if (!drive) {
            return malformed("its drive cannot be compared");
        }
        reached.push_back(*drive);
        rootSize = 1;
        rest = std::string_view(text).substr(3);
    } else {
        reached = *baseElements;
        rootSize = reached.front() == kUncMarker ? 3 : 1;
    }

    bool parentElement = false;
    for (const std::string_view element : split(rest, '/')) {
        if (element.empty() || element == ".") {
            continue;
        }
        parentElement = parentElement || element == "..";
        elements.push_back(element);
    }
    for (const std::string_view element : elements) {
        if (element.find(':') != std::string_view::npos) {
            return malformed("':' is not allowed in a name (alternate data streams)");
        }
    }
    for (const std::string_view element : elements) {
        if (element == "..") {
            if (reached.size() > rootSize) {
                reached.pop_back();
            }
            continue;
        }
        auto folded = foldedElement(element);
        if (!folded) {
            return malformed("its names cannot be compared");
        }
        reached.push_back(std::move(*folded));
    }

    const bool inside = reached.size() >= baseElements->size() &&
                        std::equal(baseElements->begin(), baseElements->end(), reached.begin());
    if (!inside) {
        return refuse(ErrorCode::OutOfRange, parentElement
                                                 ? "its \"..\" elements lead outside the folder it belongs to"
                                                 : "it lies outside the folder it belongs to");
    }
    if (parentElement) {
        return malformed("\"..\" elements are not allowed, even when they stay inside the folder");
    }
    if (reached.size() == baseElements->size()) {
        return malformed("it names the folder itself, not a file in it");
    }

    // Without "..", what lies below `base` is the path's last elements, as written.
    std::string relative;
    for (std::size_t i = elements.size() - (reached.size() - baseElements->size()); i < elements.size(); ++i) {
        if (!relative.empty()) {
            relative += '/';
        }
        relative += elements[i];
    }
    return relative;
}

} // namespace rv::core
