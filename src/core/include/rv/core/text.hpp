#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rv::core {

/// UTF-8 text <-> std::filesystem::path without going through the ANSI code page. Both throw std::system_error on
/// text that does not convert (malformed UTF-8, a path holding an unpaired UTF-16 surrogate): for names that come from
/// a file system or an untrusted file, use tryPathFromUtf8, tryPathToUtf8 or pathToUtf8Lossy (rv/core/utf8.hpp).
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8);
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);
/// pathToUtf8() with '/' between the elements (the generic format), for paths shown to people: on Windows a path
/// joined from parts ("build/models" / "cube.gltf") otherwise mixes both separators.
[[nodiscard]] std::string pathToGenericUtf8(const std::filesystem::path& path);

[[nodiscard]] std::string toLowerAscii(std::string_view text);
[[nodiscard]] bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept;
/// Removes leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;
/// Splits on `delimiter`, keeping empty fields.
[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delimiter);

} // namespace rv::core
