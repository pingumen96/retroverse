#include <rv/catalog/lookup_key.hpp>

#include <rv/core/case_fold.hpp>
#include <rv/core/utf8.hpp>

namespace rv::catalog {

LookupKeyVersion currentLookupKeyVersion() {
    return LookupKeyVersion{kLookupKeyRule, core::casingTableVersion()};
}

Expected<LookupKey> makeLookupKey(std::string_view relativePath) {
    const auto refuse = [&](std::string_view why) {
        return makeError(ErrorCode::InvalidArgument, "not a relative path for a lookup key: " + std::string(why),
                         std::string(relativePath));
    };
    if (relativePath.empty()) {
        return refuse("empty");
    }
    if (!core::isValidUtf8(relativePath)) {
        return refuse("malformed UTF-8");
    }
    for (const char c : relativePath) {
        // Below U+0020 only, what NTFS and exFAT refuse in names; DEL (U+007F) is a valid name character.
        if (static_cast<unsigned char>(c) < 0x20) {
            return refuse("a control character (U+0000 to U+001F)");
        }
        if (c == '\\' || c == ':') {
            return refuse("'\\' or ':' (paths are normalized with '/' between the elements)");
        }
    }
    // Every element between the '/' separators is a name: not empty (no leading, trailing or repeated '/'), not "." or
    // ".." (the resolver removes "./" and refuses "..").
    std::size_t start = 0;
    while (true) {
        const std::size_t end = relativePath.find('/', start);
        const std::string_view element =
            relativePath.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (element.empty() || element == "." || element == "..") {
            return refuse("an empty, \".\" or \"..\" element");
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    auto key = core::fileSystemUpperCase(relativePath);
    if (!key) {
        return propagate(std::move(key.error()));
    }
    return LookupKey(std::move(*key), std::string(relativePath));
}

} // namespace rv::catalog
