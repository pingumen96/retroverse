#include <rv/core/text.hpp>

#include <algorithm>

namespace rv::core {

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string converted(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(converted);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string converted = path.u8string();
    return std::string(reinterpret_cast<const char*>(converted.data()), converted.size());
}

std::string toLowerAscii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return result;
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view kWhitespace = " \t\r\n\f\v";
    const std::size_t first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

} // namespace rv::core
